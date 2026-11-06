/*
 * Ethedor (05/11/2026): gravador de video do client (botao "Gravar" da barra de cima).
 * - captura: GraphicalApplication::run() chama captureRecordingFrame() logo antes do swapBuffers (glReadPixels);
 * - 06/11: grava em MP4 H.264 (Media Foundation do Windows, ethedor_h264.cpp): resolucao da tela (ate 1920 px de
 *   largura; acima disso reduz pela MEDIA dos pixels), ~30-45 MB por minuto em 1080p. Sem o codificador H.264
 *   (Windows "N" sem o Media Feature Pack, Linux) volta para o .avi MJPEG (JPEG 85, ate 1280 px, bem maior).
 * - varias threads preparam os quadros em paralelo (desvirar, reduzir, JPEG) e uma escreve o arquivo NA ORDEM;
 *   fila com limite: se o PC nao der conta, descarta quadro em vez de travar o jogo;
 * - o tempo de cada quadro e o tempo real da captura (MP4) / a media no fim (AVI);
 * - o .avi para sozinho perto de 1,8 GB (limite do AVI 1.0, numeros de 32 bits).
 * Incluir SO no graphicalapplication.cpp (ele define o STB_IMAGE_WRITE_IMPLEMENTATION).
 */
#ifndef ETHEDOR_VIDEORECORDER_H
#define ETHEDOR_VIDEORECORDER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "stb_image_write.h"
#include "ethedor_h264.h"

class EthedorVideoRecorder
{
public:
    struct Frame { int w = 0, h = 0; uint64_t seq = 0; int64_t us = 0; std::vector<uint8_t> rgba; };

    // path em UTF-8 (a extensao vira .mp4 ou .avi conforme o formato que der certo). telaW/telaH = tamanho atual da
    // tela (define o tamanho do video). maxWidth 0 = 1920. quality 1-100 (0 = 75): JPEG do .avi e bitrate do .mp4.
    bool start(const std::string& path, int fps, int telaW, int telaH, int maxWidth = 0, int quality = 0)
    {
        if (m_running || telaW < 2 || telaH < 2)
            return false;
        m_fps = fps < 1 ? 1 : (fps > 30 ? 30 : fps);
        m_maxWidth = maxWidth <= 0 ? 1920 : (maxWidth < 320 ? 320 : maxWidth);
        m_quality = quality <= 0 ? 75 : (quality > 100 ? 100 : quality);
        int w = telaW, h = telaH;
        if (w > m_maxWidth) { h = (int)((int64_t)h * m_maxWidth / w); w = m_maxWidth; }
        m_outW = w & ~1; m_outH = h & ~1;
        if (m_outW < 2 || m_outH < 2)
            return false;
        m_base = semExtensao(path);
        m_full = false;
        m_index.clear();
        m_frames = 0;
        m_maxChunk = 0;
        m_stop = false;
        m_nextSeq = m_writeSeq = 0;
        m_encoding = 0;
        m_queue.clear();
        m_done.clear();
        m_startUs = nowUs();
        // a thread de escrita abre o arquivo (o Media Foundation fica todo nela) e avisa se deu certo
        std::promise<bool> pronto;
        std::future<bool> resultado = pronto.get_future();
        m_thread = std::thread([this, &pronto] { writerLoop(pronto); });
        if (!resultado.get()) {
            m_thread.join();
            return false;
        }
        m_running = true;
        unsigned hw = std::thread::hardware_concurrency();
        int n = hw > 2 ? (int)hw - 1 : 1; // deixa 1 nucleo para o jogo
        m_numEncoders = n > 4 ? 4 : n;
        for (int i = 0; i < m_numEncoders; ++i)
            m_encoders.emplace_back([this] { encoderLoop(); });
        return true;
    }

    // devolve o caminho do arquivo; o fim do arquivo (indice e tamanhos) e escrito pela thread de escrita
    std::string stop()
    {
        if (!m_running)
            return "";
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cond.notify_all();
        m_doneCond.notify_all();
        for (auto& t : m_encoders)
            if (t.joinable()) t.join();
        m_encoders.clear();
        m_doneCond.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        m_running = false;
        return m_path;
    }

    bool isRunning() const { return m_running; }
    // o arquivo chegou ao limite de tamanho: parou de gravar quadros (quem chamou deve dar stop())
    bool isFull() const { return m_full; }
    int fps() const { return m_fps; }

    // quadro capturado (de baixo para cima, como o glReadPixels devolve). false = fila cheia, quadro descartado
    bool push(Frame&& f)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop || m_full || f.w < 2 || f.h < 2)
            return false;
        // espera no maximo 2 quadros por thread (fila + prontos esperando a vez de ir pro arquivo)
        if (m_queue.size() + (size_t)m_encoding + m_done.size() >= (size_t)m_numEncoders * 2 + 2)
            return false;
        f.seq = m_nextSeq++;
        f.us = nowUs() - m_startUs;
        m_queue.push_back(std::move(f));
        m_cond.notify_one();
        return true;
    }

    ~EthedorVideoRecorder() { stop(); }

private:
    struct Pronto { int64_t us = 0; std::vector<uint8_t> dados; };

    static int64_t nowUs()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static std::string semExtensao(const std::string& p)
    {
        size_t barra = p.find_last_of("/\\"), ponto = p.find_last_of('.');
        if (ponto != std::string::npos && (barra == std::string::npos || ponto > barra))
            return p.substr(0, ponto);
        return p;
    }
    static void apagar(const std::string& p)
    {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(p), ec);
    }
    void w32(uint32_t v) { m_file.write(reinterpret_cast<const char*>(&v), 4); }
    void w16(uint16_t v) { m_file.write(reinterpret_cast<const char*>(&v), 2); }
    void fcc(const char* s) { m_file.write(s, 4); }
    void patch32(std::streamoff pos, uint32_t v)
    {
        std::streamoff atual = m_file.tellp();
        m_file.seekp(pos);
        w32(v);
        m_file.seekp(atual);
    }

    void writeHeader()
    {
        fcc("RIFF"); m_posRiffSize = m_file.tellp(); w32(0); fcc("AVI ");
        fcc("LIST"); w32(4 + 8 + 56 + 8 + 4 + 8 + 56 + 8 + 40); fcc("hdrl");
        fcc("avih"); w32(56);
        m_posUsPerFrame = m_file.tellp(); w32(1000000 / m_fps);
        w32(0); w32(0); w32(0x10 /*AVIF_HASINDEX*/);
        m_posTotalFrames = m_file.tellp(); w32(0);
        w32(0); w32(1);
        m_posSuggested1 = m_file.tellp(); w32(0);
        w32(m_outW); w32(m_outH); w32(0); w32(0); w32(0); w32(0);
        fcc("LIST"); w32(4 + 8 + 56 + 8 + 40); fcc("strl");
        fcc("strh"); w32(56);
        fcc("vids"); fcc("MJPG"); w32(0); w16(0); w16(0); w32(0);
        m_posScale = m_file.tellp(); w32(1);
        m_posRate = m_file.tellp(); w32(m_fps);
        w32(0);
        m_posLength = m_file.tellp(); w32(0);
        m_posSuggested2 = m_file.tellp(); w32(0);
        w32(0xFFFFFFFF); w32(0);
        w16(0); w16(0); w16((uint16_t)m_outW); w16((uint16_t)m_outH);
        fcc("strf"); w32(40);
        w32(40); w32(m_outW); w32(m_outH); w16(1); w16(24); fcc("MJPG"); w32(m_outW * m_outH * 3);
        w32(0); w32(0); w32(0); w32(0);
        fcc("LIST"); m_posMoviSize = m_file.tellp(); w32(0); m_posMovi = m_file.tellp(); fcc("movi");
    }

    void finishAvi()
    {
        if (m_frames == 0 || !m_file.is_open()) {
            if (m_file.is_open()) m_file.close();
            apagar(m_path); // nada gravado: nao deixa arquivo quebrado
            return;
        }
        std::streamoff fimMovi = m_file.tellp();
        fcc("idx1"); w32((uint32_t)(m_index.size() * 16));
        for (auto& e : m_index) { fcc("00dc"); w32(0x10 /*AVIIF_KEYFRAME*/); w32(e.first); w32(e.second); }
        std::streamoff fim = m_file.tellp();
        int64_t dur = nowUs() - m_startUs;
        uint32_t usPorQuadro = (uint32_t)(dur > 0 ? dur / (int64_t)m_frames : 1000000 / m_fps);
        if (usPorQuadro < 1000) usPorQuadro = 1000;
        patch32(m_posRiffSize, (uint32_t)(fim - 8));
        patch32(m_posMoviSize, (uint32_t)(fimMovi - m_posMovi));
        patch32(m_posUsPerFrame, usPorQuadro);
        patch32(m_posTotalFrames, (uint32_t)m_frames);
        patch32(m_posSuggested1, m_maxChunk);
        patch32(m_posScale, usPorQuadro);
        patch32(m_posRate, 1000000);
        patch32(m_posLength, (uint32_t)m_frames);
        patch32(m_posSuggested2, m_maxChunk);
        m_file.close();
    }

    static void jpgWrite(void* ctx, void* data, int size)
    {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
    }

    // tela (RGBA de baixo para cima) -> quadro do video (roda nas threads de preparo; nao mexe no arquivo)
    // MP4: BGRA de cima para baixo. AVI: JPEG.
    void prepare(Frame& f, std::vector<uint8_t>& rgb, std::vector<uint8_t>& saida)
    {
        saida.clear();
        const int ow = m_outW, oh = m_outH, canais = m_h264 ? 4 : 3;
        std::vector<uint8_t>& dst0 = m_h264 ? saida : rgb;
        dst0.resize((size_t)ow * oh * canais);
        // MP4 quer BGRA (troca R e B); AVI quer RGB
        const int iR = m_h264 ? 2 : 0, iB = m_h264 ? 0 : 2;
        if (ow == f.w && oh <= f.h) {
            for (int y = 0; y < oh; ++y) {
                const uint8_t* src = f.rgba.data() + (size_t)(f.h - 1 - y) * f.w * 4;
                uint8_t* dst = dst0.data() + (size_t)y * ow * canais;
                for (int x = 0; x < ow; ++x, dst += canais, src += 4) {
                    dst[iR] = src[0]; dst[1] = src[1]; dst[iB] = src[2];
                    if (canais == 4) dst[3] = 255;
                }
            }
        } else {
            // reduz (ou amplia, se a janela mudou de tamanho) pela MEDIA dos pixels de cada bloco
            for (int y = 0; y < oh; ++y) {
                int y0 = (int)((int64_t)y * f.h / oh), y1 = (int)((int64_t)(y + 1) * f.h / oh);
                if (y1 <= y0) y1 = y0 + 1;
                uint8_t* dst = dst0.data() + (size_t)y * ow * canais;
                for (int x = 0; x < ow; ++x, dst += canais) {
                    int x0 = (int)((int64_t)x * f.w / ow), x1 = (int)((int64_t)(x + 1) * f.w / ow);
                    if (x1 <= x0) x1 = x0 + 1;
                    uint32_t r = 0, g = 0, bl = 0, n = 0;
                    for (int sy = y0; sy < y1; ++sy) {
                        const uint8_t* linha = f.rgba.data() + (size_t)(f.h - 1 - sy) * f.w * 4;
                        for (int sx = x0; sx < x1; ++sx) { const uint8_t* q = linha + (size_t)sx * 4; r += q[0]; g += q[1]; bl += q[2]; ++n; }
                    }
                    dst[iR] = (uint8_t)(r / n); dst[1] = (uint8_t)(g / n); dst[iB] = (uint8_t)(bl / n);
                    if (canais == 4) dst[3] = 255;
                }
            }
        }
        if (!m_h264) {
            int q = m_quality + 10 > 95 ? 95 : m_quality + 10; // JPEG 85 com o padrao 75
            if (!stbi_write_jpg_to_func(&EthedorVideoRecorder::jpgWrite, &saida, ow, oh, 3, rgb.data(), q))
                saida.clear();
        }
    }

    void encoderLoop()
    {
        std::vector<uint8_t> rgb;
        for (;;) {
            Frame f;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cond.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                if (m_queue.empty()) break; // parou e nao sobrou quadro
                f = std::move(m_queue.front());
                m_queue.pop_front();
                ++m_encoding;
            }
            Pronto p;
            p.us = f.us;
            if (!m_full) prepare(f, rgb, p.dados);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_done[f.seq] = std::move(p); // vazio = quadro perdido (a ordem continua)
                --m_encoding;
            }
            m_doneCond.notify_all();
        }
    }

    void writeAvi(const std::vector<uint8_t>& jpg)
    {
        if (m_frames == 0) writeHeader();
        uint32_t tam = (uint32_t)jpg.size();
        if ((uint64_t)m_file.tellp() + tam + 16 * (m_index.size() + 1) > LIMITE_AVI) { m_full = true; return; }
        uint32_t offset = (uint32_t)(m_file.tellp() - m_posMovi);
        fcc("00dc"); w32(tam);
        m_file.write(reinterpret_cast<const char*>(jpg.data()), tam);
        if (tam & 1) m_file.put(0);
        m_index.emplace_back(offset, tam);
        if (tam > m_maxChunk) m_maxChunk = tam;
        ++m_frames;
    }

    void writeMp4(const Pronto& p)
    {
        int64_t t = p.us * 10; // 100 ns
        if (t <= m_lastT) t = m_lastT + 1;
        m_lastT = t;
        if (m_h264w->write(p.dados.data(), t, 10000000LL / m_fps))
            ++m_frames;
    }

    // abre o arquivo: MP4 se o Windows tiver o codificador H.264, senao AVI
    bool abrir()
    {
        m_h264 = false;
        m_path = m_base + ".mp4";
        {
            std::ofstream teste(std::filesystem::u8path(m_path), std::ios::binary | std::ios::trunc);
            if (!teste.is_open())
                return false; // a pasta nao existe ou nao deixa gravar
        }
        int64_t bits = (int64_t)m_outW * m_outH * m_fps * m_quality * 14 / 10000; // 0,105 bit por pixel com 75
        if (bits < 1000000) bits = 1000000;
        if (bits > 40000000) bits = 40000000;
        m_h264w = ethedorCriarH264(m_path, m_outW, m_outH, m_fps, (int)bits);
        if (m_h264w) {
            m_h264 = true;
            m_lastT = -1;
            return true;
        }
        apagar(m_path);
        // reserva .avi (MJPEG, sem compressao entre quadros): ate 1280 px para o arquivo nao ficar gigante
        if (m_outW > 1280) {
            m_outH = (int)((int64_t)m_outH * 1280 / m_outW) & ~1;
            m_outW = 1280;
        }
        m_path = m_base + ".avi";
        m_file.open(std::filesystem::u8path(m_path), std::ios::binary | std::ios::trunc);
        return m_file.is_open();
    }

    void writerLoop(std::promise<bool>& pronto)
    {
        bool ok = abrir();
        pronto.set_value(ok); // depois disto a promise nao existe mais (o start ja voltou)
        if (!ok)
            return;
        for (;;) {
            Pronto p;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_doneCond.wait(lock, [this] {
                    return m_done.count(m_writeSeq) ||
                        (m_stop && m_queue.empty() && m_encoding == 0 && m_done.empty());
                });
                auto it = m_done.find(m_writeSeq);
                if (it == m_done.end()) break; // parou e acabou tudo
                p = std::move(it->second);
                m_done.erase(it);
                ++m_writeSeq;
            }
            if (p.dados.empty() || m_full) continue;
            if (m_h264) writeMp4(p); else writeAvi(p.dados);
        }
        if (m_h264) {
            bool fechou = m_h264w->finish();
            m_h264w.reset();
            if (!fechou || m_frames == 0) apagar(m_path);
        } else {
            finishAvi();
        }
    }

    static constexpr uint64_t LIMITE_AVI = 1800ull * 1024 * 1024;
    std::string m_path, m_base;
    int m_fps = 12, m_maxWidth = 1920, m_quality = 75, m_outW = 0, m_outH = 0, m_numEncoders = 1;
    bool m_h264 = false;
    std::unique_ptr<EthedorH264Writer> m_h264w;
    int64_t m_lastT = -1;
    std::atomic_bool m_full{ false };
    std::ofstream m_file;
    std::thread m_thread;               // escreve o arquivo
    std::vector<std::thread> m_encoders; // preparam os quadros
    std::mutex m_mutex;
    std::condition_variable m_cond, m_doneCond;
    std::deque<Frame> m_queue;
    std::map<uint64_t, Pronto> m_done;
    uint64_t m_nextSeq = 0, m_writeSeq = 0;
    int m_encoding = 0;
    bool m_stop = false;
    std::atomic_bool m_running{ false };
    int64_t m_startUs = 0;
    uint64_t m_frames = 0;
    uint32_t m_maxChunk = 0;
    std::vector<std::pair<uint32_t, uint32_t>> m_index;
    std::streamoff m_posRiffSize = 0, m_posUsPerFrame = 0, m_posTotalFrames = 0, m_posSuggested1 = 0, m_posScale = 0,
        m_posRate = 0, m_posLength = 0, m_posSuggested2 = 0, m_posMoviSize = 0, m_posMovi = 0;
};

#endif
