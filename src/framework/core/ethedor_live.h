/*
 * Ethedor (20/11/2026): transmissao AO VIVO (YouTube / Twitch) direto do client do PC.
 * - O client le a tela (o mesmo glReadPixels do gravador) e entrega aqui; uma thread reduz para o tamanho da live
 *   (barras pretas se a janela tiver outro formato), converte para NV12 e manda pelo stdin de um ffmpeg.exe proprio
 *   (pasta "ffmpeg" ao lado do exe, compilado so com partes LGPL: tools/live/compilar_ffmpeg.sh no projeto).
 * - Som: o som do PC inteiro (WASAPI loopback do dispositivo padrao, como o OBS faz), por um named pipe. Sem som
 *   tocando o Windows nao entrega nada: a thread completa com silencio pelo relogio.
 * - O ffmpeg codifica (H.264 pelo Media Foundation do Windows, usa a placa de video quando tem; audio AAC) e manda
 *   por RTMP. Morre junto com o client (Job Object).
 * - A chave da live nunca aparece no log: as linhas do ffmpeg passam com a chave trocada por ***.
 * Fica num .cpp separado (como o ethedor_h264.cpp): o windows.h e o WASAPI nao entram no resto do client.
 * Fora do Windows tudo devolve "nao disponivel".
 */
#ifndef ETHEDOR_LIVE_H
#define ETHEDOR_LIVE_H

#include <cstdint>
#include <string>
#include <vector>

// servico: "youtube" ou "twitch". chave: so letras, numeros, _ e -. qualidade: 1 = 480p, 2 = 720p, 3 = 1080p (30 fps).
// comSom: manda o som do PC. Devolve "" se comecou (ainda conectando) ou o texto do erro.
std::string ethedorLiveStart(const std::string& servico, const std::string& chave, int qualidade, bool comSom);
// termina a live (em segundo plano: o ffmpeg fecha a transmissao direito). esperar = so volta quando acabou.
void ethedorLiveStop(bool esperar = false);
// "estado\tsegundos\tkbps\tatrasoMs\tmensagem" (atrasoMs = quanto o video esta atras do relogio) - estado: parado, conectando, aovivo, parando, erro
std::string ethedorLiveStatus();
// ultimas linhas que o ffmpeg escreveu (sem a chave), separadas por \n
std::string ethedorLiveLog();
// o client deve ler a tela agora? (live ligada e ja passou o tempo de um quadro)
bool ethedorLiveQuerQuadro();
// quadro da tela, RGBA de BAIXO para CIMA (como o glReadPixels devolve). A live fica com o vetor.
void ethedorLivePush(std::vector<uint8_t>&& rgba, int w, int h);

#endif
