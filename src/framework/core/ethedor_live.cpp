// Ethedor (20/11/2026): transmissao ao vivo (YouTube / Twitch). Ver ethedor_live.h.
#include "ethedor_live.h"

#ifdef _WIN32
// o projeto compila mirando o Windows XP; o WASAPI aparece nos cabecalhos a partir do Vista: sobe a versao SO neste
// arquivo (mesmo jeito do ethedor_h264.cpp)
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
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#pragma comment(lib, "ole32.lib")

namespace {

int64_t agoraUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Preset { int w, h, fps, kbps; };
Preset presetDe(int q)
{
    if (q <= 1) return { 854, 480, 30, 1500 };
    if (q >= 3) return { 1920, 1080, 30, 5000 };
    return { 1280, 720, 30, 3000 };
}

const int TAXA_AUDIO = 48000; // o som vai sempre em 48 kHz estereo (o C++ converte se o PC usar outra taxa)

std::wstring utf8ParaW(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring pastaDoExe()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    size_t barra = p.find_last_of(L"\\/");
    return barra == std::wstring::npos ? std::wstring() : p.substr(0, barra + 1);
}

bool chaveValida(const std::string& c)
{
    if (c.size() < 4 || c.size() > 128) return false;
    for (char ch : c)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
            return false;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Captura do som do PC (WASAPI loopback). Le o que o dispositivo padrao esta tocando e devolve estereo 16 bits.
class SomDoPc
{
public:
    ~SomDoPc() { fechar(); }

    bool abrir(std::string& erro)
    {
        fechar();
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&m_en);
        if (FAILED(hr)) { erro = "nao abriu o som do Windows"; return false; }
        hr = m_en->GetDefaultAudioEndpoint(eRender, eConsole, &m_dev);
        if (FAILED(hr)) { erro = "nenhuma saida de som no PC"; fechar(); return false; }
        hr = m_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_ac);
        if (FAILED(hr)) { erro = "nao abriu a saida de som"; fechar(); return false; }
        hr = m_ac->GetMixFormat(&m_wf);
        if (FAILED(hr) || !m_wf) { erro = "formato do som desconhecido"; fechar(); return false; }
        hr = m_ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, m_wf, nullptr);
        if (FAILED(hr)) { erro = "o Windows nao deixou capturar o som"; fechar(); return false; }
        hr = m_ac->GetService(__uuidof(IAudioCaptureClient), (void**)&m_cc);
        if (FAILED(hr)) { erro = "o Windows nao deixou capturar o som"; fechar(); return false; }
        m_canais = m_wf->nChannels;
        m_bits = m_wf->wBitsPerSample;
        m_bloco = m_wf->nBlockAlign;
        m_taxa = (int)m_wf->nSamplesPerSec;
        m_float = m_wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (m_wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && m_wf->cbSize >= 22) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_wf);
            m_float = ext->SubFormat.Data1 == 3; // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-...}
            if (ext->SubFormat.Data1 != 1 && ext->SubFormat.Data1 != 3) { erro = "formato do som nao suportado"; fechar(); return false; }
        }
        if (m_canais < 1 || m_taxa < 8000 || (m_float && m_bits != 32) || (!m_float && m_bits != 16 && m_bits != 24 && m_bits != 32)) {
            erro = "formato do som nao suportado";
            fechar();
            return false;
        }
        if (FAILED(m_ac->Start())) { erro = "nao comecou a capturar o som"; fechar(); return false; }
        m_pos = 0; m_ultL = m_ultR = 0;
        return true;
    }

    void fechar()
    {
        if (m_ac) m_ac->Stop();
        if (m_cc) { m_cc->Release(); m_cc = nullptr; }
        if (m_wf) { CoTaskMemFree(m_wf); m_wf = nullptr; }
        if (m_ac) { m_ac->Release(); m_ac = nullptr; }
        if (m_dev) { m_dev->Release(); m_dev = nullptr; }
        if (m_en) { m_en->Release(); m_en = nullptr; }
    }

    bool aberto() const { return m_cc != nullptr; }

    // le tudo que chegou e poe no fim de saida (estereo 16 bits, 48 kHz). false = o dispositivo sumiu (trocou o fone...)
    bool ler(std::vector<int16_t>& saida)
    {
        if (!m_cc) return false;
        for (;;) {
            UINT32 pacote = 0;
            HRESULT hr = m_cc->GetNextPacketSize(&pacote);
            if (FAILED(hr)) return false;
            if (pacote == 0) return true;
            BYTE* dados = nullptr;
            UINT32 quadros = 0;
            DWORD flags = 0;
            hr = m_cc->GetBuffer(&dados, &quadros, &flags, nullptr, nullptr);
            if (FAILED(hr)) return false;
            m_tmp.resize((size_t)quadros * 2);
            bool mudo = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !dados;
            for (UINT32 i = 0; i < quadros; ++i) {
                int16_t l = 0, r = 0;
                if (!mudo) {
                    const BYTE* q = dados + (size_t)i * m_bloco;
                    l = amostra(q);
                    r = m_canais > 1 ? amostra(q + m_bits / 8) : l;
                }
                m_tmp[i * 2] = l;
                m_tmp[i * 2 + 1] = r;
            }
            m_cc->ReleaseBuffer(quadros);
            converterTaxa(m_tmp, quadros, saida);
        }
    }

private:
    int16_t amostra(const BYTE* q) const
    {
        if (m_float) {
            float f;
            std::memcpy(&f, q, 4);
            if (!(f == f)) f = 0; // NaN
            int v = (int)std::lround(f * 32767.0f);
            return (int16_t)std::max(-32768, std::min(32767, v));
        }
        if (m_bits == 16) { int16_t v; std::memcpy(&v, q, 2); return v; }
        if (m_bits == 24) return (int16_t)(q[1] | (q[2] << 8));
        int32_t v; std::memcpy(&v, q, 4);
        return (int16_t)(v >> 16);
    }

    // taxa do PC -> 48 kHz (linear; a maioria dos PCs ja usa 48 kHz e ai so copia)
    void converterTaxa(const std::vector<int16_t>& in, UINT32 n, std::vector<int16_t>& out)
    {
        if (n == 0) return;
        if (m_taxa == TAXA_AUDIO) {
            out.insert(out.end(), in.begin(), in.begin() + (size_t)n * 2);
            return;
        }
        const double passo = (double)m_taxa / TAXA_AUDIO;
        // indice -1 = ultima amostra do pacote anterior
        auto pega = [&](int64_t i, int c) -> double {
            if (i < 0) return c == 0 ? m_ultL : m_ultR;
            return in[(size_t)i * 2 + c];
        };
        while (m_pos < (double)n - 1.0) {
            int64_t i = (int64_t)std::floor(m_pos);
            double f = m_pos - (double)i;
            for (int c = 0; c < 2; ++c)
                out.push_back((int16_t)std::lround(pega(i, c) * (1.0 - f) + pega(i + 1, c) * f));
            m_pos += passo;
        }
        m_pos -= (double)n;
        m_ultL = in[(size_t)(n - 1) * 2];
        m_ultR = in[(size_t)(n - 1) * 2 + 1];
    }

    IMMDeviceEnumerator* m_en = nullptr;
    IMMDevice* m_dev = nullptr;
    IAudioClient* m_ac = nullptr;
    IAudioCaptureClient* m_cc = nullptr;
    WAVEFORMATEX* m_wf = nullptr;
    int m_canais = 2, m_bits = 32, m_bloco = 8, m_taxa = TAXA_AUDIO;
    bool m_float = true;
    double m_pos = 0;
    int16_t m_ultL = 0, m_ultR = 0;
    std::vector<int16_t> m_tmp;
};

// ---------------------------------------------------------------------------------------------------------------
class Sessao
{
public:
    Preset p{};
    bool comSom = true;
    std::vector<std::string> chaves; // escondidas no log (uma por servico)
    std::string servico;

    std::atomic_bool parar{ false }, ffmpegSaiu{ false }, videoAcabou{ false };
    std::atomic<int64_t> ultimoQuadroUs{ 0 };

    ~Sessao() { fecharHandles(); }

    // urls: 1 destino = flv direto; 2 destinos (YouTube + Twitch) = um codificador so e o "tee" manda para os dois
    // (onfail=ignore: se um recusar, a live continua no outro)
    std::string comecar(const std::wstring& ffmpeg, const std::vector<std::string>& urls)
    {
        static std::atomic_int contador{ 0 };
        m_nomePipe = L"\\\\.\\pipe\\ethedor_live_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++contador);
        m_pipeSom = CreateNamedPipeW(m_nomePipe.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1 << 20, 0, 0, nullptr);
        if (m_pipeSom == INVALID_HANDLE_VALUE) { m_pipeSom = nullptr; return "nao criou o canal do som"; }

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE inR = nullptr, outW = nullptr, errW = nullptr;
        if (!CreatePipe(&inR, &m_in, &sa, 4 << 20) || !CreatePipe(&m_out, &outW, &sa, 0) || !CreatePipe(&m_err, &errW, &sa, 0)) {
            if (inR) CloseHandle(inR);
            if (outW) CloseHandle(outW);
            if (errW) CloseHandle(errW);
            return "nao criou os canais do ffmpeg";
        }
        // as pontas do client nao vao para o ffmpeg
        SetHandleInformation(m_in, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(m_out, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(m_err, HANDLE_FLAG_INHERIT, 0);

        const int k = p.kbps;
        wchar_t args[2048];
        swprintf(args, 2048,
                 L"\"%ls\" -hide_banner -loglevel warning -nostats -nostdin -progress pipe:1"
                 L" -thread_queue_size 64 -f rawvideo -pix_fmt nv12 -s %dx%d -framerate %d -i pipe:0"
                 L" -thread_queue_size 4096 -f s16le -ar %d -ac 2 -i \"%ls\""
                 L" -map 0:v:0 -map 1:a:0"
                 L" -c:v h264_mf -rate_control cbr -b:v %dk -maxrate %dk -bufsize %dk -g %d -bf 0"
                 L" -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv"
                 L" -c:a aac -b:a 128k -ar 48000 -ac 2"
                 L" %ls",
                 ffmpeg.c_str(), p.w, p.h, p.fps, TAXA_AUDIO, m_nomePipe.c_str(), k, k, k * 2, p.fps * 2,
                 utf8ParaW(saida(urls)).c_str());

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inR;
        si.hStdOutput = outW;
        si.hStdError = errW;
        PROCESS_INFORMATION pi{};
        std::wstring pasta = ffmpeg.substr(0, ffmpeg.find_last_of(L"\\/") + 1);
        BOOL ok = CreateProcessW(ffmpeg.c_str(), args, nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                 nullptr, pasta.c_str(), &si, &pi);
        CloseHandle(inR);
        CloseHandle(outW);
        CloseHandle(errW);
        if (!ok) return "nao abriu o ffmpeg (" + std::to_string(GetLastError()) + ")";
        m_proc = pi.hProcess;
        // o ffmpeg morre junto com o client (fechou, travou, foi encerrado pelo gerenciador de tarefas)
        m_job = CreateJobObjectW(nullptr, nullptr);
        if (m_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
            AssignProcessToJobObject(m_job, m_proc);
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        m_inicioUs = agoraUs();
        estado("conectando", "");
        m_tSaida = std::thread([this] { lerSaida(); });
        m_tErro = std::thread([this] { lerErros(); });
        m_tSom = std::thread([this] { loopSom(); });
        m_tVideo = std::thread([this] { loopVideo(); });
        return "";
    }

    // fecha tudo (pode demorar alguns segundos: o ffmpeg termina a transmissao). Chamar UMA vez.
    void terminar()
    {
        parar = true;
        // o video para no proximo quadro; se estiver preso escrevendo (ffmpeg travado), o ffmpeg e encerrado
        for (int i = 0; i < 300 && !videoAcabou; ++i) Sleep(10);
        if (!videoAcabou && m_proc) TerminateProcess(m_proc, 1);
        if (m_tVideo.joinable()) m_tVideo.join();
        if (m_in) { CloseHandle(m_in); m_in = nullptr; } // fim do video: o ffmpeg fecha o arquivo/transmissao
        if (m_tSom.joinable()) m_tSom.join();
        if (m_proc && WaitForSingleObject(m_proc, 6000) == WAIT_TIMEOUT)
            TerminateProcess(m_proc, 1);
        if (m_tSaida.joinable()) m_tSaida.join();
        if (m_tErro.joinable()) m_tErro.join();
        fecharHandles();
    }

    void push(std::vector<uint8_t>&& rgba, int w, int h)
    {
        std::lock_guard<std::mutex> l(m_mQuadro);
        m_quadro = std::move(rgba);
        m_qw = w;
        m_qh = h;
        ++m_qseq;
    }

    std::string status()
    {
        std::lock_guard<std::mutex> l(m_mSt);
        std::string est = m_estado, msg = m_msg;
        if (ffmpegSaiu && !parar && est != "erro") { est = "erro"; msg = "A live parou."; }
        if (parar && est != "erro") est = "parando";
        int seg = (est == "aovivo" && m_aoVivoUs > 0) ? (int)((agoraUs() - m_aoVivoUs) / 1000000) : 0;
        if (est == "aovivo" && msg.empty() && !m_aviso.empty()) msg = m_aviso;
        if (est == "aovivo" && m_atrasado && msg.empty()) msg = "Internet lenta: a live esta atrasando.";
        char buf[64];
        snprintf(buf, sizeof(buf), "%d\t%d\t%d\t", seg, (int)m_kbps, est == "aovivo" ? (int)m_atrasoMs : 0);
        return est + "\t" + buf + msg;
    }

    std::string log()
    {
        std::lock_guard<std::mutex> l(m_mSt);
        std::string s;
        for (auto& x : m_linhas) { s += x; s += "\n"; }
        return s;
    }

private:
    void estado(const std::string& e, const std::string& msg)
    {
        std::lock_guard<std::mutex> l(m_mSt);
        m_estado = e;
        m_msg = msg;
    }

    std::string semChave(std::string s) const
    {
        for (auto& chave : chaves) {
            if (chave.empty()) continue;
            size_t i;
            while ((i = s.find(chave)) != std::string::npos) s.replace(i, chave.size(), "***");
        }
        return s;
    }

    static std::string saida(const std::vector<std::string>& urls)
    {
        if (urls.size() == 1) return "-f flv -flvflags no_duration_filesize \"" + urls[0] + "\"";
        std::string t = "-flags +global_header -f tee \"";
        for (size_t i = 0; i < urls.size(); ++i) {
            if (i) t += "|";
            t += "[f=flv:onfail=ignore:flvflags=no_duration_filesize]" + urls[i];
        }
        return t + "\"";
    }

    // com 2 destinos, o tee avisa quando um deles cai ("Slave ... failed" / "continuing with N/2 slaves")
    void conferirDestino(const std::string& linha)
    {
        std::string b = linha;
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        if (chaves.size() < 2 || (b.find("slave") == std::string::npos && b.find("continuing with") == std::string::npos)) return;
        std::string qual = b.find("youtube") != std::string::npos ? "YouTube" : (b.find("twitch") != std::string::npos ? "Twitch" : "");
        std::lock_guard<std::mutex> l(m_mSt);
        if (!qual.empty()) m_caiu = qual;
        m_aviso = (m_caiu.empty() ? std::string("Um dos destinos") : "O " + m_caiu) +
                  " nao aceitou a live (confira a chave). A live continua no outro.";
    }

    // --- video ---------------------------------------------------------------------------------------------
    void prepararMapa(int sw, int sh)
    {
        const int W = p.w, H = p.h;
        double esc = std::min((double)W / sw, (double)H / sh);
        m_dw = std::max(2, (int)(sw * esc) & ~1);
        m_dh = std::max(2, (int)(sh * esc) & ~1);
        m_dw = std::min(m_dw, W);
        m_dh = std::min(m_dh, H);
        m_ox = ((W - m_dw) / 2) & ~1;
        m_oy = ((H - m_dh) / 2) & ~1;
        m_xs.resize((size_t)m_dw + 1);
        m_ys.resize((size_t)m_dh + 1);
        for (int x = 0; x <= m_dw; ++x) m_xs[x] = (int)((int64_t)x * sw / m_dw);
        for (int y = 0; y <= m_dh; ++y) m_ys[y] = (int)((int64_t)y * sh / m_dh);
        m_rgb.assign((size_t)m_dw * m_dh * 3, 0);
        // tudo preto (barras)
        std::memset(m_nv12.data(), 16, (size_t)W * H);
        std::memset(m_nv12.data() + (size_t)W * H, 128, (size_t)W * H / 2);
        m_sw = sw;
        m_sh = sh;
    }

    void converter(const std::vector<uint8_t>& src, int sw, int sh)
    {
        if (sw != m_sw || sh != m_sh) prepararMapa(sw, sh);
        const int W = p.w, H = p.h, dw = m_dw, dh = m_dh;
        // reduz (media dos pixels de cada bloco); a tela vem de baixo para cima
        for (int y = 0; y < dh; ++y) {
            int y0 = m_ys[y], y1 = std::max(m_ys[y + 1], y0 + 1);
            uint8_t* d = m_rgb.data() + (size_t)y * dw * 3;
            for (int x = 0; x < dw; ++x, d += 3) {
                int x0 = m_xs[x], x1 = std::max(m_xs[x + 1], x0 + 1);
                uint32_t r = 0, g = 0, b = 0, n = 0;
                for (int sy = y0; sy < y1; ++sy) {
                    const uint8_t* linha = src.data() + (size_t)(sh - 1 - sy) * sw * 4;
                    for (int sx = x0; sx < x1; ++sx) { const uint8_t* q = linha + (size_t)sx * 4; r += q[0]; g += q[1]; b += q[2]; ++n; }
                }
                d[0] = (uint8_t)(r / n); d[1] = (uint8_t)(g / n); d[2] = (uint8_t)(b / n);
            }
        }
        // RGB -> NV12 (BT.709, faixa limitada)
        uint8_t* Y = m_nv12.data();
        uint8_t* UV = m_nv12.data() + (size_t)W * H;
        for (int y = 0; y < dh; ++y) {
            const uint8_t* s = m_rgb.data() + (size_t)y * dw * 3;
            uint8_t* d = Y + (size_t)(m_oy + y) * W + m_ox;
            for (int x = 0; x < dw; ++x, s += 3)
                d[x] = (uint8_t)(((47 * s[0] + 157 * s[1] + 16 * s[2] + 128) >> 8) + 16);
        }
        for (int y = 0; y < dh; y += 2) {
            const uint8_t* a = m_rgb.data() + (size_t)y * dw * 3;
            const uint8_t* b = a + (size_t)dw * 3;
            uint8_t* d = UV + (size_t)((m_oy + y) / 2) * W + m_ox;
            for (int x = 0; x < dw; x += 2, a += 6, b += 6, d += 2) {
                int r = (a[0] + a[3] + b[0] + b[3] + 2) >> 2;
                int g = (a[1] + a[4] + b[1] + b[4] + 2) >> 2;
                int bl = (a[2] + a[5] + b[2] + b[5] + 2) >> 2;
                int u = ((-26 * r - 87 * g + 112 * bl + 128) >> 8) + 128;
                int v = ((112 * r - 102 * g - 10 * bl + 128) >> 8) + 128;
                d[0] = (uint8_t)std::max(0, std::min(255, u));
                d[1] = (uint8_t)std::max(0, std::min(255, v));
            }
        }
    }

    bool escreverTudo(HANDLE h, const uint8_t* dados, size_t n)
    {
        while (n > 0) {
            DWORD foi = 0;
            if (!WriteFile(h, dados, (DWORD)std::min<size_t>(n, 1 << 20), &foi, nullptr) || foi == 0) return false;
            dados += foi;
            n -= foi;
        }
        return true;
    }

    void loopVideo()
    {
        m_nv12.resize((size_t)p.w * p.h * 3 / 2);
        std::memset(m_nv12.data(), 16, (size_t)p.w * p.h);
        std::memset(m_nv12.data() + (size_t)p.w * p.h, 128, (size_t)p.w * p.h / 2);
        uint64_t seqUsado = 0;
        std::vector<uint8_t> quadro;
        int qw = 0, qh = 0;
        const int64_t t0 = agoraUs();
        int64_t n = 0;
        while (!parar) {
            int64_t alvo = t0 + n * 1000000 / p.fps, agora = agoraUs();
            if (agora < alvo) { Sleep((DWORD)std::max<int64_t>(1, (alvo - agora) / 1000)); continue; }
            // passou 3 s do horario: internet lenta (o ffmpeg nao consegue mandar tudo)
            m_atrasoMs = (int)std::max<int64_t>(0, (agora - alvo) / 1000);
            m_atrasado = agora - alvo > 3000000;
            {
                std::lock_guard<std::mutex> l(m_mQuadro);
                if (m_qseq != seqUsado && !m_quadro.empty()) {
                    quadro.swap(m_quadro);
                    m_quadro.clear();
                    qw = m_qw;
                    qh = m_qh;
                    seqUsado = m_qseq;
                } else {
                    qw = 0;
                }
            }
            if (qw > 1 && qh > 1 && quadro.size() >= (size_t)qw * qh * 4) converter(quadro, qw, qh);
            if (!escreverTudo(m_in, m_nv12.data(), m_nv12.size())) break;
            ++n;
        }
        videoAcabou = true;
    }

    // --- som -----------------------------------------------------------------------------------------------
    bool escreverSom(const int16_t* dados, size_t amostras, OVERLAPPED& ov)
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(dados);
        size_t n = amostras * 2;
        while (n > 0) {
            DWORD foi = 0;
            ResetEvent(ov.hEvent);
            if (!WriteFile(m_pipeSom, b, (DWORD)std::min<size_t>(n, 1 << 16), &foi, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                for (;;) {
                    if (WaitForSingleObject(ov.hEvent, 100) == WAIT_OBJECT_0) break;
                    if (parar || ffmpegSaiu) { CancelIo(m_pipeSom); return false; }
                }
                if (!GetOverlappedResult(m_pipeSom, &ov, &foi, FALSE)) return false;
            }
            if (foi == 0) return false;
            b += foi;
            n -= foi;
        }
        return true;
    }

    void loopSom()
    {
        bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // espera o ffmpeg abrir o canal do som
        bool ligado = false;
        if (!ConnectNamedPipe(m_pipeSom, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) ligado = true;
            else if (e == ERROR_IO_PENDING) {
                while (!parar && !ffmpegSaiu) {
                    if (WaitForSingleObject(ov.hEvent, 100) == WAIT_OBJECT_0) { ligado = true; break; }
                }
                if (!ligado) CancelIo(m_pipeSom);
            }
        } else {
            ligado = true;
        }
        SomDoPc som;
        int64_t tentarDe = 0;
        if (ligado && comSom) {
            std::string erro;
            if (!som.abrir(erro)) { anotar("Sem o som do PC: " + erro); tentarDe = agoraUs() + 2000000; }
        }
        std::vector<int16_t> buf, silencio;
        const int64_t t0 = agoraUs();
        int64_t escritas = 0; // amostras estereo ja mandadas
        while (ligado && !parar && !ffmpegSaiu) {
            buf.clear();
            if (comSom) {
                if (som.aberto()) {
                    if (!som.ler(buf)) { som.fechar(); tentarDe = agoraUs() + 1000000; } // trocou o fone / a saida de som
                } else if (tentarDe && agoraUs() >= tentarDe) {
                    std::string erro;
                    tentarDe = som.abrir(erro) ? 0 : agoraUs() + 2000000;
                }
            }
            int64_t devido = (agoraUs() - t0) * TAXA_AUDIO / 1000000;
            size_t qtd = buf.size() / 2;
            if (qtd > 0) {
                // adiantado demais (o Windows mandou um monte de uma vez): joga fora o excesso para nao atrasar a live
                int64_t sobra = escritas + (int64_t)qtd - (devido + TAXA_AUDIO * 3 / 10);
                if (sobra > 0) qtd = sobra >= (int64_t)qtd ? 0 : qtd - (size_t)sobra;
                if (qtd > 0) {
                    if (!escreverSom(buf.data(), qtd * 2, ov)) break;
                    escritas += (int64_t)qtd;
                }
            }
            // sem som tocando o Windows nao manda nada: completa com silencio (deixa 20 ms de folga para o som de verdade)
            if (escritas < devido - TAXA_AUDIO / 20) {
                int64_t falta = std::min<int64_t>(devido - TAXA_AUDIO / 50 - escritas, TAXA_AUDIO);
                silencio.assign((size_t)falta * 2, 0);
                if (!escreverSom(silencio.data(), silencio.size(), ov)) break;
                escritas += falta;
            }
            Sleep(10);
        }
        som.fechar();
        // fechar o lado do client = fim do som para o ffmpeg (ele ainda le o que ficou no canal). Sem Flush: com o
        // ffmpeg travado o Flush nunca voltaria.
        if (m_pipeSom) {
            CloseHandle(m_pipeSom);
            m_pipeSom = nullptr;
        }
        CloseHandle(ov.hEvent);
        if (com) CoUninitialize();
    }

    // --- saidas do ffmpeg ----------------------------------------------------------------------------------
    void anotar(const std::string& linha)
    {
        std::string s = semChave(linha);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        if (s.empty()) return;
        conferirDestino(s);
        std::lock_guard<std::mutex> l(m_mSt);
        m_linhas.push_back(s);
        while (m_linhas.size() > 40) m_linhas.pop_front();
    }

    void lerLinhas(HANDLE h, bool progresso)
    {
        std::string pendente;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
            pendente.append(buf, n);
            size_t i;
            while ((i = pendente.find('\n')) != std::string::npos) {
                std::string linha = pendente.substr(0, i);
                pendente.erase(0, i + 1);
                if (progresso) linhaProgresso(linha); else anotar(linha);
            }
        }
        if (!pendente.empty() && !progresso) anotar(pendente);
    }

    void linhaProgresso(const std::string& linha)
    {
        size_t eq = linha.find('=');
        if (eq == std::string::npos) return;
        std::string k = linha.substr(0, eq), v = linha.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (k == "out_time_us" || k == "out_time_ms") {
            long long t = atoll(v.c_str());
            if (t > 0) {
                std::lock_guard<std::mutex> l(m_mSt);
                if (m_estado == "conectando") { m_estado = "aovivo"; m_msg.clear(); m_aoVivoUs = agoraUs(); }
            }
        } else if (k == "bitrate") {
            double kb = atof(v.c_str()); // "2500.1kbits/s"
            if (kb > 0) m_kbps = kb;
        }
    }

    void lerSaida()
    {
        lerLinhas(m_out, true);
        // o stdout fechou: o ffmpeg terminou (por pedido ou por erro)
        if (m_proc) WaitForSingleObject(m_proc, 3000);
        ffmpegSaiu = true;
        if (!parar) estado("erro", motivoDoErro());
    }

    void lerErros() { lerLinhas(m_err, false); }

    std::string motivoDoErro()
    {
        std::string ultima;
        {
            std::lock_guard<std::mutex> l(m_mSt);
            for (auto& s : m_linhas) {
                std::string b = s;
                std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                if (b.find("refused") != std::string::npos || b.find("timed out") != std::string::npos ||
                    b.find("cannot open connection") != std::string::npos || b.find("failed to resolve") != std::string::npos ||
                    b.find("error number -138") != std::string::npos || b.find("error number -10060") != std::string::npos)
                    return "Nao conectou no servidor da live. Confira a internet.";
                if (b.find("badname") != std::string::npos || b.find("publish") != std::string::npos ||
                    b.find("server error") != std::string::npos || b.find("unauthorized") != std::string::npos ||
                    b.find("broken pipe") != std::string::npos || b.find("end of file") != std::string::npos)
                    ultima = "O servidor recusou a live. Confira a chave da transmissao.";
                if (b.find("h264_mf") != std::string::npos || b.find("encoder") != std::string::npos)
                    return "O Windows nao conseguiu codificar o video (H.264).";
                // RTMP que nao abre (Twitch/YouTube recusam chave errada fechando a conexao: "I/O error")
                if (b.find("error opening output") != std::string::npos && ultima.empty())
                    ultima = "O servidor nao aceitou a live. Confira a chave da transmissao e a internet.";
            }
        }
        return ultima.empty() ? "A live parou. Tente de novo." : ultima;
    }

    void fecharHandles()
    {
        for (HANDLE* h : { &m_in, &m_out, &m_err, &m_pipeSom, &m_proc, &m_job })
            if (*h) { CloseHandle(*h); *h = nullptr; }
    }

    HANDLE m_in = nullptr, m_out = nullptr, m_err = nullptr, m_pipeSom = nullptr, m_proc = nullptr, m_job = nullptr;
    std::wstring m_nomePipe;
    std::thread m_tVideo, m_tSom, m_tSaida, m_tErro;
    int64_t m_inicioUs = 0, m_aoVivoUs = 0;

    std::mutex m_mQuadro;
    std::vector<uint8_t> m_quadro;
    int m_qw = 0, m_qh = 0;
    uint64_t m_qseq = 0;

    std::vector<uint8_t> m_nv12, m_rgb;
    std::vector<int> m_xs, m_ys;
    int m_sw = 0, m_sh = 0, m_dw = 0, m_dh = 0, m_ox = 0, m_oy = 0;

    std::mutex m_mSt;
    std::string m_estado = "parado", m_msg, m_aviso, m_caiu;
    std::deque<std::string> m_linhas;
    std::atomic<double> m_kbps{ 0 };
    std::atomic_int m_atrasoMs{ 0 };
    std::atomic_bool m_atrasado{ false };
};

// ---------------------------------------------------------------------------------------------------------------
std::mutex g_m;
std::shared_ptr<Sessao> g_sessao;      // a live atual (ligada, com erro ou terminando)
std::shared_ptr<Sessao> g_terminando;  // sendo fechada em segundo plano
std::thread g_tTerminar;
std::string g_ultimoLog;

void juntarTerminando()
{
    // chamar com g_m travado
    if (g_tTerminar.joinable()) g_tTerminar.join();
    g_terminando.reset();
}

} // namespace

std::string ethedorLiveStart(const std::string& servico, const std::string& chave, int qualidade, bool comSom)
{
    // "ambos": chave = "chaveDoYouTube|chaveDaTwitch" (a | nunca aparece numa chave valida)
    std::vector<std::string> urls, chaves;
    auto destino = [&](const std::string& srv, const std::string& c) -> std::string {
        if (!chaveValida(c)) return "Chave da transmissao invalida (so letras, numeros, - e _).";
        if (srv == "youtube") urls.push_back("rtmp://a.rtmp.youtube.com/live2/" + c);
        else if (srv == "twitch") urls.push_back("rtmp://live.twitch.tv/app/" + c);
#ifdef ETHEDOR_LIVE_TESTE
        else if (srv == "teste") urls.push_back(std::string(getenv("ETHEDOR_LIVE_URL") ? getenv("ETHEDOR_LIVE_URL") : "") + "/" + c);
        else if (srv == "teste2") urls.push_back(std::string(getenv("ETHEDOR_LIVE_URL2") ? getenv("ETHEDOR_LIVE_URL2") : "") + "/" + c);
#endif
        else return "Escolha YouTube, Twitch ou os dois.";
        chaves.push_back(c);
        return "";
    };
    std::string erro;
    if (servico == "ambos" || servico == "testes") {
        size_t bar = chave.find('|');
        if (bar == std::string::npos) return "Cole as duas chaves (YouTube e Twitch).";
        std::string a = servico == "ambos" ? "youtube" : "teste", b = servico == "ambos" ? "twitch" : "teste2";
        erro = destino(a, chave.substr(0, bar));
        if (erro.empty()) erro = destino(b, chave.substr(bar + 1));
    } else {
        erro = destino(servico, chave);
    }
    if (!erro.empty()) return erro;

    std::wstring ffmpeg = pastaDoExe() + L"ffmpeg\\ffmpeg.exe";
    if (GetFileAttributesW(ffmpeg.c_str()) == INVALID_FILE_ATTRIBUTES)
        return "Falta o programa da live (pasta ffmpeg do jogo). Abra o jogo pelo launcher para atualizar.";

    std::lock_guard<std::mutex> l(g_m);
    if (g_sessao) {
        if (!g_sessao->ffmpegSaiu) return "A live ja esta ligada.";
        g_sessao->terminar(); // a anterior deu erro: o ffmpeg ja saiu, fecha rapido
        g_ultimoLog = g_sessao->log();
        g_sessao.reset();
    }
    if (g_terminando) {
        if (g_terminando->videoAcabou && g_terminando->ffmpegSaiu) juntarTerminando();
        else return "Espere a live anterior terminar de fechar.";
    }
    auto s = std::make_shared<Sessao>();
    s->p = presetDe(qualidade);
    s->comSom = comSom;
    s->chaves = chaves;
    s->servico = servico;
    erro = s->comecar(ffmpeg, urls);
    if (!erro.empty()) {
        s->terminar();
        return erro;
    }
    g_sessao = s;
    return "";
}

void ethedorLiveStop(bool esperar)
{
    std::lock_guard<std::mutex> l(g_m);
    if (g_terminando) juntarTerminando();
    if (!g_sessao) return;
    g_terminando = g_sessao;
    g_sessao.reset();
    g_terminando->parar = true;
    auto s = g_terminando;
    g_tTerminar = std::thread([s] { s->terminar(); });
    if (esperar) juntarTerminando();
}

std::string ethedorLiveStatus()
{
    std::lock_guard<std::mutex> l(g_m);
    if (g_sessao) return g_sessao->status();
    if (g_terminando && !(g_terminando->videoAcabou && g_terminando->ffmpegSaiu)) return "parando\t0\t0\t0\t";
    return "parado\t0\t0\t0\t";
}

std::string ethedorLiveLog()
{
    std::lock_guard<std::mutex> l(g_m);
    if (g_sessao) return g_sessao->log();
    if (g_terminando) return g_terminando->log();
    return g_ultimoLog;
}

bool ethedorLiveQuerQuadro()
{
    std::shared_ptr<Sessao> s;
    {
        std::lock_guard<std::mutex> l(g_m);
        s = g_sessao;
    }
    if (!s || s->parar || s->ffmpegSaiu) return false;
    int64_t agora = agoraUs(), ultimo = s->ultimoQuadroUs;
    if (ultimo != 0 && agora - ultimo < 1000000 / s->p.fps - 3000) return false;
    s->ultimoQuadroUs = agora;
    return true;
}

void ethedorLivePush(std::vector<uint8_t>&& rgba, int w, int h)
{
    std::shared_ptr<Sessao> s;
    {
        std::lock_guard<std::mutex> l(g_m);
        s = g_sessao;
    }
    if (s && !s->parar) s->push(std::move(rgba), w, h);
}

#else

std::string ethedorLiveStart(const std::string&, const std::string&, int, bool) { return "A live so funciona no Windows."; }
void ethedorLiveStop(bool) {}
std::string ethedorLiveStatus() { return "parado\t0\t0\t0\t"; }
std::string ethedorLiveLog() { return ""; }
bool ethedorLiveQuerQuadro() { return false; }
void ethedorLivePush(std::vector<uint8_t>&&, int, int) {}

#endif
