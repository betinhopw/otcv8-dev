// Ethedor (06/11/2026): MP4 H.264 pelo Media Foundation (ver ethedor_h264.h)
#include "ethedor_h264.h"

#ifdef _WIN32
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

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace {

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
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
            return false;
        m_mf = true;

        IMFAttributes* at = nullptr;
        if (FAILED(MFCreateAttributes(&at, 2)))
            return false;
        at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE); // usa a placa de video se tiver codificador
        at->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        hr = MFCreateSinkWriterFromURL(largo(caminho).c_str(), nullptr, at, &m_w);
        soltar(at);
        if (FAILED(hr) || !m_w)
            return false;

        // saida H.264 (perfil High; se o codificador recusar, tenta o padrao dele)
        bool adicionou = false;
        for (int tentativa = 0; tentativa < 2 && !adicionou; ++tentativa) {
            IMFMediaType* out = nullptr;
            if (FAILED(MFCreateMediaType(&out)))
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
        if (FAILED(MFCreateMediaType(&in)))
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
        if (SUCCEEDED(MFCreateMemoryBuffer(tam, &buf))) {
            BYTE* p = nullptr;
            if (SUCCEEDED(buf->Lock(&p, nullptr, nullptr))) {
                std::memcpy(p, bgra, tam);
                buf->Unlock();
                buf->SetCurrentLength(tam);
                if (SUCCEEDED(MFCreateSample(&sample)) && SUCCEEDED(sample->AddBuffer(buf))) {
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
        if (m_mf) { MFShutdown(); m_mf = false; }
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
