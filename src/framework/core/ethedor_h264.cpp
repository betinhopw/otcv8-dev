// Ethedor (06/11/2026): MP4 H.264 pelo Media Foundation (ver ethedor_h264.h)
#include "ethedor_h264.h"

#ifdef _WIN32
// o projeto compila mirando o Windows XP (_WIN32_WINNT=0x0501), e o Sink Writer so aparece nos cabecalhos a partir do
// Windows 7: sobe a versao SO neste arquivo (o resto do client nao muda). No XP/Vista o MFStartup falha e o
// gravador usa o .avi.
// As DLLs (mfplat/mfreadwrite) sao abertas SO na hora de gravar (LoadLibrary): no Windows "N" sem o Media Feature
// Pack elas nao existem, e ligadas direto no exe o client nem abriria.
#undef _WIN32_WINNT
#undef WINVER
#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <cstring>

#pragma comment(lib, "mfuuid.lib") // so os GUIDs (dados), nao importa DLL nenhuma
#pragma comment(lib, "ole32.lib")

namespace {

// funcoes das DLLs do Media Foundation, carregadas na hora
struct MfFuncoes
{
    HMODULE plat = nullptr, rw = nullptr;
    HRESULT(WINAPI* Startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* Shutdown)() = nullptr;
    HRESULT(WINAPI* CreateAttributes)(IMFAttributes**, UINT32) = nullptr;
    HRESULT(WINAPI* CreateMediaType)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* CreateMemoryBuffer)(DWORD, IMFMediaBuffer**) = nullptr;
    HRESULT(WINAPI* CreateSample)(IMFSample**) = nullptr;
    HRESULT(WINAPI* CreateSinkWriterFromURL)(LPCWSTR, IMFByteStream*, IMFAttributes*, IMFSinkWriter**) = nullptr;
    bool ok = false;
};

const MfFuncoes& mf()
{
    static MfFuncoes f;
    static bool tentou = false;
    if (tentou) return f;
    tentou = true;
    f.plat = LoadLibraryW(L"mfplat.dll");
    f.rw = LoadLibraryW(L"mfreadwrite.dll");
    if (!f.plat || !f.rw) return f;
    f.Startup = reinterpret_cast<decltype(f.Startup)>(GetProcAddress(f.plat, "MFStartup"));
    f.Shutdown = reinterpret_cast<decltype(f.Shutdown)>(GetProcAddress(f.plat, "MFShutdown"));
    f.CreateAttributes = reinterpret_cast<decltype(f.CreateAttributes)>(GetProcAddress(f.plat, "MFCreateAttributes"));
    f.CreateMediaType = reinterpret_cast<decltype(f.CreateMediaType)>(GetProcAddress(f.plat, "MFCreateMediaType"));
    f.CreateMemoryBuffer = reinterpret_cast<decltype(f.CreateMemoryBuffer)>(GetProcAddress(f.plat, "MFCreateMemoryBuffer"));
    f.CreateSample = reinterpret_cast<decltype(f.CreateSample)>(GetProcAddress(f.plat, "MFCreateSample"));
    f.CreateSinkWriterFromURL = reinterpret_cast<decltype(f.CreateSinkWriterFromURL)>(GetProcAddress(f.rw, "MFCreateSinkWriterFromURL"));
    f.ok = f.Startup && f.Shutdown && f.CreateAttributes && f.CreateMediaType && f.CreateMemoryBuffer && f.CreateSample &&
        f.CreateSinkWriterFromURL;
    return f;
}

template <class T> void soltar(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

std::wstring largo(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

class MfWriter : public EthedorH264Writer
{
public:
    ~MfWriter() override { finish(); }

    bool init(const std::string& caminho, int w, int h, int fps, int bitrate)
    {
        m_W = w; m_H = h;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_com = SUCCEEDED(hr); // RPC_E_CHANGED_MODE: a thread ja tem COM de outro tipo (nao desligar no fim)
        if (!mf().ok)
            return false; // Windows sem Media Foundation: o gravador usa o .avi
        if (FAILED(mf().Startup(MF_VERSION, MFSTARTUP_LITE)))
            return false;
        m_mf = true;

        IMFAttributes* at = nullptr;
        if (FAILED(mf().CreateAttributes(&at, 2)))
            return false;
        at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE); // usa a placa de video se tiver codificador
        at->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        hr = mf().CreateSinkWriterFromURL(largo(caminho).c_str(), nullptr, at, &m_w);
        soltar(at);
        if (FAILED(hr) || !m_w)
            return false;

        // saida H.264 (perfil High; se o codificador recusar, tenta o padrao dele)
        bool adicionou = false;
        for (int tentativa = 0; tentativa < 2 && !adicionou; ++tentativa) {
            IMFMediaType* out = nullptr;
            if (FAILED(mf().CreateMediaType(&out)))
                return false;
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
            out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(out, MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
            MFSetAttributeRatio(out, MF_MT_FRAME_RATE, (UINT32)fps, 1);
            MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (tentativa == 0)
                out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
            adicionou = SUCCEEDED(m_w->AddStream(out, &m_stream));
            soltar(out);
        }
        if (!adicionou)
            return false;

        // entrada: BGRA de cima para baixo (stride positivo); o Sink Writer converte para o formato do codificador
        IMFMediaType* in = nullptr;
        if (FAILED(mf().CreateMediaType(&in)))
            return false;
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(w * 4));
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
        MFSetAttributeRatio(in, MF_MT_FRAME_RATE, (UINT32)fps, 1);
        MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = m_w->SetInputMediaType(m_stream, in, nullptr);
        soltar(in);
        if (FAILED(hr))
            return false;
        if (FAILED(m_w->BeginWriting()))
            return false;
        m_ok = true;
        return true;
    }

    bool write(const uint8_t* bgra, int64_t tempo, int64_t duracao) override
    {
        if (!m_ok || !m_w) return false;
        const DWORD tam = (DWORD)m_W * m_H * 4;
        IMFMediaBuffer* buf = nullptr;
        IMFSample* sample = nullptr;
        bool ok = false;
        if (SUCCEEDED(mf().CreateMemoryBuffer(tam, &buf))) {
            BYTE* p = nullptr;
            if (SUCCEEDED(buf->Lock(&p, nullptr, nullptr))) {
                std::memcpy(p, bgra, tam);
                buf->Unlock();
                buf->SetCurrentLength(tam);
                if (SUCCEEDED(mf().CreateSample(&sample)) && SUCCEEDED(sample->AddBuffer(buf))) {
                    sample->SetSampleTime(tempo);
                    sample->SetSampleDuration(duracao);
                    ok = SUCCEEDED(m_w->WriteSample(m_stream, sample));
                    if (ok) ++m_amostras;
                }
            }
        }
        soltar(sample);
        soltar(buf);
        return ok;
    }

    bool finish() override
    {
        bool ok = false;
        if (m_w) {
            if (m_ok && m_amostras > 0)
                ok = SUCCEEDED(m_w->Finalize());
            soltar(m_w);
        }
        m_ok = false;
        if (m_mf) { mf().Shutdown(); m_mf = false; }
        if (m_com) { CoUninitialize(); m_com = false; }
        return ok;
    }

private:
    IMFSinkWriter* m_w = nullptr;
    DWORD m_stream = 0;
    int m_W = 0, m_H = 0;
    uint64_t m_amostras = 0;
    bool m_com = false, m_mf = false, m_ok = false;
};

} // namespace

std::unique_ptr<EthedorH264Writer> ethedorCriarH264(const std::string& caminhoUtf8, int w, int h, int fps, int bitrate)
{
    std::unique_ptr<MfWriter> p(new MfWriter());
    if (!p->init(caminhoUtf8, w, h, fps, bitrate))
        return nullptr; // o destrutor desliga o que chegou a abrir
    return std::unique_ptr<EthedorH264Writer>(p.release());
}

#else

std::unique_ptr<EthedorH264Writer> ethedorCriarH264(const std::string&, int, int, int, int)
{
    return nullptr;
}

#endif
