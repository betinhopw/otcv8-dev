/*
 * Ethedor (05/11/2026): gravador de video do client (botao "Gravar" da barra de cima).
 * Grava um .avi MJPEG (abre em qualquer player: VLC, Windows Media Player, navegador, Discord).
 * - captura: GraphicalApplication::run() chama captureFrame() logo antes do swapBuffers (glReadPixels da tela pronta);
 * - codificacao: uma thread so (fila com limite; se o PC nao der conta, descarta quadro em vez de travar o jogo);
 * - a velocidade do video sai do tempo real: no fim, microssegundos por quadro = duracao / quadros gravados.
 * Header-only de proposito (nao precisa mexer no vcxproj). Incluir SO no graphicalapplication.cpp
 * (ele define o STB_IMAGE_WRITE_IMPLEMENTATION).
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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "stb_image_write.h"

class EthedorVideoRecorder
{
public:
    struct Frame { int w = 0, h = 0; std::vector<uint8_t> rgba; };

    // path em UTF-8. maxWidth = largura maxima do video (reduz para o arquivo nao ficar enorme)
    bool start(const std::string& path, int fps, int maxWidth = 1280, int quality = 70)
    {
        if (m_running)
            return false;
        m_path = path;
        m_fps = fps < 1 ? 1 : (fps > 30 ? 30 : fps);
        m_maxWidth = maxWidth;
        m_quality = quality;
        m_file.open(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
        if (!m_file.is_open())
            return false;
        m_outW = m_outH = 0;
        m_index.clear();
        m_frames = 0;
        m_maxChunk = 0;
        m_stop = false;
        m_running = true;
        m_startUs = nowUs();
        m_thread = std::thread([this] { worker(); });
        return true;
    }

    // devolve o caminho do arquivo; o fim do arquivo (indice e tamanhos) e escrito pela thread
    std::string stop()
    {
        if (!m_running)
            return "";
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cond.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        m_running = false;
        return m_path;
    }

    bool isRunning() const { return m_running; }
    int fps() const { return m_fps; }

    // quadro capturado (de baixo para cima, como o glReadPixels devolve). false = fila cheia, quadro descartado
    bool push(Frame&& f)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop || m_queue.size() >= 6)
            return false;
        m_queue.push_back(std::move(f));
        m_cond.notify_one();
        return true;
    }

    ~EthedorVideoRecorder() { stop(); }

private:
    static int64_t nowUs()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
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

    void finish()
    {
        if (m_frames == 0 || !m_file.is_open()) {
            if (m_file.is_open()) m_file.close();
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(m_path), ec); // nada gravado: nao deixa arquivo quebrado
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

    void encode(Frame& f)
    {
        if (m_outW == 0) {
            // tamanho do video = o da 1a tela, reduzido ate maxWidth, numeros pares (alguns players exigem)
            int w = f.w, h = f.h;
            if (w > m_maxWidth) { h = (int)((int64_t)h * m_maxWidth / w); w = m_maxWidth; }
            m_outW = w & ~1; m_outH = h & ~1;
            if (m_outW < 2 || m_outH < 2) return;
            writeHeader();
        }
        // reamostra (vizinho mais proximo), desvira (glReadPixels vem de baixo para cima) e tira o alfa
        m_rgb.resize((size_t)m_outW * m_outH * 3);
        for (int y = 0; y < m_outH; ++y) {
            int sy = f.h - 1 - (int)((int64_t)y * f.h / m_outH);
            const uint8_t* linha = f.rgba.data() + (size_t)sy * f.w * 4;
            uint8_t* dst = m_rgb.data() + (size_t)y * m_outW * 3;
            for (int x = 0; x < m_outW; ++x) {
                const uint8_t* p = linha + (size_t)((int64_t)x * f.w / m_outW) * 4;
                dst[x * 3] = p[0]; dst[x * 3 + 1] = p[1]; dst[x * 3 + 2] = p[2];
            }
        }
        m_jpg.clear();
        if (!stbi_write_jpg_to_func(&EthedorVideoRecorder::jpgWrite, &m_jpg, m_outW, m_outH, 3, m_rgb.data(), m_quality))
            return;
        uint32_t tam = (uint32_t)m_jpg.size();
        uint32_t offset = (uint32_t)(m_file.tellp() - m_posMovi);
        fcc("00dc"); w32(tam);
        m_file.write(reinterpret_cast<const char*>(m_jpg.data()), tam);
        if (tam & 1) m_file.put(0);
        m_index.emplace_back(offset, tam);
        if (tam > m_maxChunk) m_maxChunk = tam;
        ++m_frames;
    }

    void worker()
    {
        for (;;) {
            Frame f;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cond.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                if (m_queue.empty()) break; // parou e nao sobrou quadro
                f = std::move(m_queue.front());
                m_queue.pop_front();
            }
            encode(f);
        }
        finish();
    }

    std::string m_path;
    int m_fps = 12, m_maxWidth = 1280, m_quality = 70, m_outW = 0, m_outH = 0;
    std::ofstream m_file;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::deque<Frame> m_queue;
    bool m_stop = false;
    std::atomic_bool m_running{ false };
    int64_t m_startUs = 0;
    uint64_t m_frames = 0;
    uint32_t m_maxChunk = 0;
    std::vector<std::pair<uint32_t, uint32_t>> m_index;
    std::vector<uint8_t> m_rgb, m_jpg;
    std::streamoff m_posRiffSize = 0, m_posUsPerFrame = 0, m_posTotalFrames = 0, m_posSuggested1 = 0, m_posScale = 0,
        m_posRate = 0, m_posLength = 0, m_posSuggested2 = 0, m_posMoviSize = 0, m_posMovi = 0;
};

#endif
