/*
 * Ethedor (06/11/2026): saida MP4 (H.264) do gravador de video, pelo Media Foundation do Windows.
 * Fica num .cpp separado de proposito: o windows.h e o Media Foundation nao entram no resto do client.
 * Fora do Windows (ou Windows sem o codificador, ex.: edicao "N" sem o Media Feature Pack) devolve nullptr e o
 * gravador volta para o .avi MJPEG.
 */
#ifndef ETHEDOR_H264_H
#define ETHEDOR_H264_H

#include <cstdint>
#include <memory>
#include <string>

class EthedorH264Writer
{
public:
    virtual ~EthedorH264Writer() {}
    // bgra = quadro de CIMA para BAIXO, largura*4 bytes por linha. Tempos em unidades de 100 ns.
    virtual bool write(const uint8_t* bgra, int64_t tempo100ns, int64_t duracao100ns) = 0;
    // fecha o arquivo (indice do MP4). Chamar na MESMA thread que criou.
    virtual bool finish() = 0;
};

// w e h pares. bitrate em bits por segundo.
std::unique_ptr<EthedorH264Writer> ethedorCriarH264(const std::string& caminhoUtf8, int w, int h, int fps, int bitrate);

#endif
