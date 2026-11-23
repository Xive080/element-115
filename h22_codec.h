// h22_codec.h (OLA F3): decodificador PORTABLE de los videos de carga <zona>_load.webm (VP9 + Opus, los de vel.gg).
// Sin cabeceras del motor ni de la plataforma: demuxer WebM propio (EBML: Tracks, Cluster, SimpleBlock/BlockGroup, todos los
// encajes) + libvpx (VP9) + libopus. Mismo codigo en el Mac y en Android; se compila aparte (build.sh) con H22_VPX solo si estan
// las dos bibliotecas. Sin ellas h22c_open devuelve nullptr y el juego sigue sin video (nunca es un error).
#pragma once
#include <stdint.h>

struct H22Clip;
struct H22Info { int w, h; double fps, seconds; int audioRate, audioChannels; };

// abre el fichero (lo lee entero: unos 3-8 MB). nullptr = no se puede (fichero, contenedor o sin decodificadores); why lo explica
H22Clip *h22c_open(const char *path, H22Info *info, char *why, int whyLen);
// siguiente imagen de video en BGRA (fila 0 arriba, pitch en bytes); *ptsMs = su tiempo. false = fin del video
bool h22c_video(H22Clip *c, uint8_t *bgra, int pitch, double *ptsMs);
// muestras de audio float entrelazadas a audioRate; devuelve cuantas tramas dejo (0 = fin del audio)
int h22c_audio(H22Clip *c, float *out, int maxFrames);
void h22c_close(H22Clip *c);
const char *h22c_backend();   // "libvpx x.y + libopus x.y" o "sin decodificadores"
