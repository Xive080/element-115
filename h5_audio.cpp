// H6a (sonido): driver del sistema de sonido ORIGINAL (snd*.cpp de zdec) sobre una salida portable (miniaudio, h5_audio_ma.c).
// Sustituye a snd_driver_xaudio2.cpp / snd_driver_xaudio2_dsp.cpp / snd_stream.cpp con la misma interfaz SD_* y el mismo grafo
// de XAudio2, mezclado en software:
//   voz fuente (PCM16 / MS-ADPCM, SRC lineal a 48 kHz con el pitch de SND_GetPitch, tope 2.0 como CreateSourceVoice)
//     -> SND_DspFxSourceMono por canal (LPF de oclusion + futz, parametros de SDXA2_UpdateVoiceSends)
//     -> matriz seca (Snd_SpeakerMapGetVolume * dryLevel) al masterBus si (flags & 0x3000) >> 12 == 2, si no al novoiceBus
//     -> matriz humeda (wetLevel) al radverbBus de 4 canales
//   radverbBus: SND_RvFrame (salida 3 -> L, 1 -> R como la matriz de SND_InitMasterVoice) -> novoiceBus
//   novoiceBus: SND_DspFxMasterNoVoiceSingleChannel (compresor) -> masterBus
//   masterBus: SND_DspFxMasterSingleChannel (EQ + limitador) -> salida
// Quantum de 480 frames a 48 kHz (el de los XAPO del original). El mezclador corre en el hilo del dispositivo con su propio
// mutex; snd.cpp sigue en el hilo principal (SND_Frame sincrono, ver h5_audio_cl.cpp).
// Streams (voces 0..9): el fichero sound/*.wav de los .iwd (cabecera snd_asset de header_size bytes + datos) se lee entero
// en un hilo de carga propio (pread, sin el FS del harness) a una cache LRU (H6A_CACHE_MB, 20 por defecto desde la OLA 15C).
// Depuracion: H6A=0 sin sonido; H6A_DEVICE=null sin dispositivo (hilo a tiempo real); H6A_VOL=0..1 volumen de salida
// (Android: propiedad debug.bo1.vol); H6A_WAV=fichero graba la salida a WAV 16 bits; [H6A] en el log: reproducciones,
// RMS/pico por tramo de 1 s, voces activas y coste del mezclado.
#include <sound/snd_driver_xaudio2.h>
#include <sound/snd_dvar.h>
#include <sound/snd_utils.h>
#include <sound/snd_radverb.h>
#include <sound/snd_bank.h>
#include <sound/snd_globals.h>
#include <sound/snd_db.h>
#include <sound/snd_public_async.h>
#include <qcommon/common.h>
#include <mutex>
#include <algorithm>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <vector>
#include <string>
#include <chrono>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

extern "C" {
typedef void (*h6a_render_fn)(float *out, unsigned int frames, void *user);
int h6a_dev_start(unsigned int rate, unsigned int channels, unsigned int periodFrames, const char *backend, h6a_render_fn fn, void *user);
const char *h6a_dev_backend(void);
unsigned int h6a_dev_rate(void);
unsigned int h6a_dev_period(void);
void h6a_dev_stop(void);
}
bool FS_H6A_Locate(const char *filename, int *fd, unsigned long long *off, unsigned int *size);   // iwd.cpp

namespace {
const unsigned QF = 480;          // frames por quantum (SDXA2_MAX_FRAME_COUNT)
const float RATE = 48000.0f;

// ---------------------------------------------------------------- streams (cache de ficheros enteros)
struct StreamFile {
    std::string name;
    int fd = -1; unsigned long long off = 0; unsigned size = 0;
    std::atomic<int> state{0};    // 0 cargando, 1 listo, 2 error
    unsigned char *buf = nullptr;
    snd_asset hdr;                // campos numericos de la cabecera del fichero (seek_table/data no se usan)
    const unsigned char *data = nullptr; unsigned dataSize = 0;
    int refs = 0; unsigned long long lastUse = 0;
    double loadMs = 0;
};
std::vector<StreamFile *> s_files;          // solo hilo principal (y el cargador lee/escribe la entrada que carga)
std::deque<StreamFile *> s_loadQ; std::mutex s_qmx; std::condition_variable s_qcv;
std::thread s_loader; bool s_loaderRun = false;
unsigned long long s_useTick = 0, s_cacheBytes = 0, s_cacheCap = 20ull << 20;   // OLA 15C: 20 MB (antes 64; el stream mas grande visto es de 4,6 MB y los que suenan no se expulsan)

// ---------------------------------------------------------------- voces del mezclador
struct Src { const unsigned char *data; unsigned dataSize, frames, rate, ch; int format; unsigned align = 0, spb = 512; };   // align/spb: bloque MS-ADPCM (0 = el del juego, 262 * canales)
struct MixVoice {
    bool created, playing, done, looping;
    int bus;                       // 2 = masterBus, otro = novoiceBus
    Src src;
    double pos; float ratio;
    long long samplesPlayed;
    int ic;                        // canales de entrada de las matrices (pan.input_channel_count)
    float dry[2][2], dryPrev[2][2], wet[4][2], wetPrev[4][2];
    bool sendsValid, rampInit;
    snd_dsp_source_params params;
    snd_dsp_source_state state[2];
    int cacheBlk[2]; short pcm[2][2][512];
    StreamFile *file;
};
MixVoice s_v[SND_MAX_VOICES];
std::mutex s_mx;                   // protege s_v, los parametros de bus y el estado DSP frente al hilo del dispositivo

struct alignas(128) Buses {
    snd_rv_state rvState;
    alignas(16) float rvIn[4 * QF];  // (alignas: SND_Dsp* exige 16 bytes) radverb: entrada y salida en el mismo bloque (SND_RvFrame usa diferencias de 32 bits)
    alignas(16) float rvOut[4 * QF];
    alignas(16) float master[2][QF];
    alignas(16) float novoice[2][QF];
    alignas(16) float vb[2][QF];
    alignas(16) float tempa[960];
    alignas(16) float tempb[960];
    alignas(16) float out[2 * QF];
};
Buses *s_b;
snd_rv_params s_rvParams;
snd_dsp_master_params s_masterParams;
snd_dsp_master_no_voice_params s_nvParams;
snd_dsp_master_state s_masterState[2];
snd_dsp_master_no_voice_state s_nvState[2];
bool s_open;
unsigned s_outPos = QF;            // frames ya entregados del quantum actual
float s_vol = 1.0f;
// estadisticas y WAV (hilo del dispositivo)
FILE *s_wav; unsigned long long s_wavFrames;
double s_segSq; float s_segPeak; unsigned s_segFrames; unsigned long long s_totalFrames;
double s_mixUsSum, s_mixUsMax; unsigned s_mixQ; int s_segVoices;
int s_logLeft = 400;
std::chrono::steady_clock::time_point s_t0;

const short kAdapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
const short kCoef[7][2] = { { 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 }, { 240, 0 }, { 460, -208 }, { 392, -232 } };

// Bloque MS-ADPCM (nBlockAlign = 262 * canales, 512 frames por bloque como iSND_CreateVoice; los xWMA transcodificados de
// wma_adpcm.pak van en bloques de 256 * canales con 500 frames).
void DecodeBlock(MixVoice &v, int blk, int slot)
{
    const unsigned ch = v.src.ch, align = v.src.align, spb = v.src.spb;
    v.cacheBlk[slot] = blk;
    const unsigned long long boff = (unsigned long long)blk * align;
    if (boff + 7 * ch > v.src.dataSize) { memset(v.pcm[slot], 0, sizeof v.pcm[slot]); return; }
    const unsigned char *p = v.src.data + boff;
    const unsigned avail = (unsigned)std::min<unsigned long long>(align, v.src.dataSize - boff);
    int c1[2], c2[2], delta[2], s1[2], s2[2];
    for (unsigned c = 0; c < ch; ++c) { int pi = p[c]; if (pi > 6) pi = 0; c1[c] = kCoef[pi][0]; c2[c] = kCoef[pi][1]; }
    p += ch;
    for (unsigned c = 0; c < ch; ++c) delta[c] = (short)(p[2 * c] | (p[2 * c + 1] << 8));
    p += 2 * ch;
    for (unsigned c = 0; c < ch; ++c) s1[c] = (short)(p[2 * c] | (p[2 * c + 1] << 8));
    p += 2 * ch;
    for (unsigned c = 0; c < ch; ++c) s2[c] = (short)(p[2 * c] | (p[2 * c + 1] << 8));
    p += 2 * ch;
    for (unsigned c = 0; c < ch; ++c) { v.pcm[slot][c][0] = (short)s2[c]; v.pcm[slot][c][1] = (short)s1[c]; }
    const unsigned nbytes = avail - 7 * ch;
    unsigned n = 2;   // frame siguiente
    unsigned c = 0;
    for (unsigned b = 0; b < nbytes && n < spb; ++b)
    {
        for (int h = 0; h < 2 && n < spb; ++h)
        {
            const int nib = h == 0 ? (p[b] >> 4) : (p[b] & 15);
            const int sn = nib >= 8 ? nib - 16 : nib;
            int pred = ((s1[c] * c1[c]) + (s2[c] * c2[c])) >> 8;
            pred += sn * delta[c];
            if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
            s2[c] = s1[c]; s1[c] = pred;
            delta[c] = (kAdapt[nib] * delta[c]) >> 8; if (delta[c] < 16) delta[c] = 16;
            v.pcm[slot][c][n] = (short)pred;
            if (++c >= ch) { c = 0; ++n; }
        }
    }
    for (; n < spb; ++n) for (unsigned k = 0; k < ch; ++k) v.pcm[slot][k][n] = 0;
}

inline float Fetch(MixVoice &v, unsigned idx, unsigned c)
{
    if (v.src.format == SND_ASSET_FORMAT_PCMS16)
    {
        const unsigned long long o = ((unsigned long long)idx * v.src.ch + c) * 2;
        if (o + 2 > v.src.dataSize) return 0.0f;
        return (float)(short)(v.src.data[o] | (v.src.data[o + 1] << 8)) * (1.0f / 32768.0f);
    }
    const unsigned spb = v.src.spb;
    const int blk = spb == 512 ? (int)(idx >> 9) : (int)(idx / spb), slot = blk & 1;
    if (v.cacheBlk[slot] != blk) DecodeBlock(v, blk, slot);
    return (float)v.pcm[slot][c][spb == 512 ? idx & 511 : idx - (unsigned)blk * spb] * (1.0f / 32768.0f);
}

// Una voz -> s_b->vb (planar, ic canales). Devuelve false si no produjo nada.
bool RenderVoice(MixVoice &v)
{
    const unsigned frames = v.src.frames, ch = v.src.ch;
    if (!frames) { v.done = true; return false; }
    float r = v.ratio; if (!(r > 0.0009765625f)) r = 0.0009765625f; if (r > 2.0f) r = 2.0f;
    const double step = (double)v.src.rate / RATE * r;
    unsigned t = 0;
    for (; t < QF; ++t)
    {
        if (v.pos >= frames)
        {
            if (!v.looping) { v.done = true; break; }
            v.pos = fmod(v.pos, (double)frames);
        }
        const unsigned i0 = (unsigned)v.pos; const float f = (float)(v.pos - i0);
        unsigned i1 = i0 + 1; if (i1 >= frames) i1 = v.looping ? 0 : i0;
        for (unsigned c = 0; c < ch; ++c)
        {
            const float a = Fetch(v, i0, c), b = Fetch(v, i1, c);
            s_b->vb[c][t] = a + (b - a) * f;
        }
        v.pos += step;
    }
    for (unsigned k = t; k < QF; ++k) for (unsigned c = 0; c < ch; ++c) s_b->vb[c][k] = 0.0f;
    v.samplesPlayed += (long long)(t * step + 0.5);   // frames de la fuente consumidos (SamplesPlayed de XAudio2)
    if (v.samplesPlayed <= 0 && t) v.samplesPlayed = 1;
    return t > 0;
}

void MixQuantum()
{
    Buses &b = *s_b;
    memset(b.master, 0, sizeof b.master); memset(b.novoice, 0, sizeof b.novoice); memset(b.rvIn, 0, sizeof b.rvIn);
    int active = 0;
    for (int i = 0; i < SND_MAX_VOICES; ++i)
    {
        MixVoice &v = s_v[i];
        if (!v.created || !v.playing || v.done || !v.sendsValid) continue;
        if (!RenderVoice(v)) continue;
        ++active;
        const unsigned ic = v.src.ch;
        // SDXA2SourceEffect::Process: SND_DspFxSourceMono por canal con los parametros de la voz (pasados como en el original)
        for (unsigned c = 0; c < ic; ++c)
            SND_DspFxSourceMono((const snd_dsp_futz_param *)&v.params, &v.state[c], QF, b.vb[c], b.tempa, b.tempb);
        if (!v.rampInit) { memcpy(v.dryPrev, v.dry, sizeof v.dry); memcpy(v.wetPrev, v.wet, sizeof v.wet); v.rampInit = true; }
        float (*dbus)[QF] = v.bus == 2 ? b.master : b.novoice;
        const float inv = 1.0f / QF;
        for (unsigned o = 0; o < 2; ++o)
            for (unsigned c = 0; c < ic; ++c)
            {
                const float g0 = v.dryPrev[o][c], g1 = v.dry[o][c];
                if (g0 == 0.0f && g1 == 0.0f) continue;
                const float dg = (g1 - g0) * inv;
                for (unsigned t = 0; t < QF; ++t) dbus[o][t] += b.vb[c][t] * (g0 + dg * t);
            }
        for (unsigned k = 0; k < 4; ++k)
            for (unsigned c = 0; c < ic; ++c)
            {
                const float g0 = v.wetPrev[k][c], g1 = v.wet[k][c];
                if (g0 == 0.0f && g1 == 0.0f) continue;
                const float dg = (g1 - g0) * inv;
                float *dst = &b.rvIn[k * QF];
                for (unsigned t = 0; t < QF; ++t) dst[t] += b.vb[c][t] * (g0 + dg * t);
            }
        memcpy(v.dryPrev, v.dry, sizeof v.dry); memcpy(v.wetPrev, v.wet, sizeof v.wet);
    }
    // SDXA2RadverbEffect::Process (entrada LF=3, RF=1, LS=0, RS=2) y su matriz de salida (3 -> L, 1 -> R) al novoiceBus
    if (s_rvParams.earlySize > 0.0f)
    {
        SND_RvFrame(&s_rvParams, &b.rvState, (const float *)(uintptr_t)QF, &b.rvIn[3 * QF], &b.rvIn[QF], b.rvIn, &b.rvIn[2 * QF],
                    &b.rvOut[3 * QF], &b.rvOut[QF], b.rvOut, &b.rvOut[2 * QF]);
        for (unsigned t = 0; t < QF; ++t) { b.novoice[0][t] += b.rvOut[3 * QF + t]; b.novoice[1][t] += b.rvOut[QF + t]; }
    }
    for (unsigned c = 0; c < 2; ++c)
    {
        SND_DspFxMasterNoVoiceSingleChannel(QF, RATE, b.novoice[c], &s_nvParams, &s_nvState[c], &g_snd.meters[c]);
        for (unsigned t = 0; t < QF; ++t) b.master[c][t] += b.novoice[c][t];
        SND_DspFxMasterSingleChannel(QF, RATE, b.master[c], &s_masterParams, &s_masterState[c], &g_snd.meters[c]);
    }
    for (unsigned t = 0; t < QF; ++t)
        for (unsigned c = 0; c < 2; ++c)
        {
            float x = b.master[c][t] * s_vol;
            if (!(x == x)) x = 0.0f;
            if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
            b.out[2 * t + c] = x;
        }
    if (active > s_segVoices) s_segVoices = active;
}

void WavHeader(FILE *f, unsigned long long frames)
{
    const unsigned data = (unsigned)std::min<unsigned long long>(frames * 4, 0xFFFFFFF0ull);
    unsigned char h[44]; memcpy(h, "RIFF", 4); const unsigned riff = 36 + data; memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
    const unsigned fmtLen = 16, rate = 48000, bps = 48000 * 4; const unsigned short fmt = 1, chn = 2, align = 4, bits = 16;
    memcpy(h + 16, &fmtLen, 4); memcpy(h + 20, &fmt, 2); memcpy(h + 22, &chn, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &bps, 4);
    memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fseek(f, 0, SEEK_END);
}

void Stats(const float *out, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
    {
        const float l = out[2 * i], r = out[2 * i + 1];
        s_segSq += (double)l * l + (double)r * r;
        const float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r); if (a > s_segPeak) s_segPeak = a;
    }
    if (s_wav)
    {
        short tmp[2 * QF];
        for (unsigned i = 0; i < 2 * n; ++i) tmp[i] = (short)lrintf(out[i] * 32767.0f);
        fwrite(tmp, 4, n, s_wav); s_wavFrames += n;
    }
    s_segFrames += n; s_totalFrames += n;
    if (s_segFrames >= 48000)
    {
        const double rms = sqrt(s_segSq / (2.0 * s_segFrames));
        fprintf(stderr, "[H6A] tramo t=%.1fs rms=%.4f (%.1f dBFS) pico=%.3f voces=%d mezcla=%.0fus media %.0fus max (quantum 10ms)\n",
                s_totalFrames / 48000.0, rms, rms > 0 ? 20.0 * log10(rms) : -200.0, s_segPeak, s_segVoices,
                s_mixQ ? s_mixUsSum / s_mixQ : 0.0, s_mixUsMax);
        if (s_wav) { WavHeader(s_wav, s_wavFrames); fflush(s_wav); }
        s_segSq = 0; s_segPeak = 0; s_segFrames = 0; s_segVoices = 0; s_mixUsSum = 0; s_mixUsMax = 0; s_mixQ = 0;
    }
}

void Render(float *out, unsigned frames, void *)
{
    while (frames)
    {
        if (s_outPos >= QF)
        {
            const auto t0 = std::chrono::steady_clock::now();
            { std::lock_guard<std::mutex> lk(s_mx); MixQuantum(); }
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            s_mixUsSum += us; if (us > s_mixUsMax) s_mixUsMax = us; ++s_mixQ;
            Stats(s_b->out, QF);
            s_outPos = 0;
        }
        const unsigned n = std::min(frames, QF - s_outPos);
        memcpy(out, &s_b->out[2 * s_outPos], n * 2 * sizeof(float));
        out += 2 * n; frames -= n; s_outPos += n;
    }
}

// ---------------------------------------------------------------- carga de streams
void LoaderMain()
{
    for (;;)
    {
        StreamFile *f;
        {
            std::unique_lock<std::mutex> lk(s_qmx);
            s_qcv.wait(lk, [] { return !s_loadQ.empty() || !s_loaderRun; });
            if (!s_loaderRun) return;
            f = s_loadQ.front(); s_loadQ.pop_front();
        }
        const auto t0 = std::chrono::steady_clock::now();
        int st = 2;
        f->buf = (unsigned char *)malloc(f->size ? f->size : 1);
        if (f->buf)
        {
            unsigned got = 0;
            while (got < f->size) { ssize_t r = pread(f->fd, f->buf + got, f->size - got, (off_t)(f->off + got)); if (r <= 0) break; got += (unsigned)r; }
            if (got == f->size && f->size >= sizeof(snd_asset))
            {
                memcpy(&f->hdr, f->buf, sizeof(snd_asset));
                const unsigned hs = f->hdr.header_size;
                if (hs < f->size && f->hdr.frame_rate && f->hdr.channel_count >= 1 && f->hdr.channel_count <= 2)
                {
                    f->data = f->buf + hs;
                    f->dataSize = std::min<unsigned>(f->hdr.data_size, f->size - hs);
                    st = 1;
                }
            }
        }
        f->loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        f->state.store(st, std::memory_order_release);
    }
}

void CacheTrim()
{
    while (s_cacheBytes > s_cacheCap)
    {
        StreamFile *best = nullptr; size_t bi = 0;
        for (size_t i = 0; i < s_files.size(); ++i)
        {
            StreamFile *f = s_files[i];
            if (f->refs || f->state.load(std::memory_order_acquire) == 0) continue;
            if (!best || f->lastUse < best->lastUse) { best = f; bi = i; }
        }
        if (!best) return;
        s_cacheBytes -= best->size;
        free(best->buf);
        s_files.erase(s_files.begin() + bi);
        delete best;
    }
}

StreamFile *StreamOpen(const char *filename)
{
    std::string k = filename; for (auto &c : k) { c = (char)tolower((unsigned char)c); if (c == '\\') c = '/'; }
    for (StreamFile *f : s_files) if (f->name == k) { ++f->refs; f->lastUse = ++s_useTick; return f; }
    int fd; unsigned long long off; unsigned size;
    if (!FS_H6A_Locate(k.c_str(), &fd, &off, &size) && !FS_H6A_Locate(("sound/" + k).c_str(), &fd, &off, &size)) return nullptr;
    StreamFile *f = new StreamFile; f->name = k; f->fd = fd; f->off = off; f->size = size; f->refs = 1; f->lastUse = ++s_useTick;
    s_files.push_back(f); s_cacheBytes += size;
    { std::lock_guard<std::mutex> lk(s_qmx); s_loadQ.push_back(f); }
    s_qcv.notify_one();
    CacheTrim();
    return f;
}

void LogPlay(int idx, const snd_alias_t *alias, const char *kind, const Src &s)
{
    if (s_logLeft <= 0) return;
    --s_logLeft;
    fprintf(stderr, "[H6A] play t=%.2fs voz=%d '%s' %s fmt=%d %uHz %uch %.2fs bus=%s loop=%d\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count(), idx, alias->name.get(), kind, s.format,
            s.rate, s.ch, s.rate ? (double)s.frames / s.rate : 0.0, ((alias->flags & 0x3000) >> 12) == 2 ? "master" : "novoice",
            (alias->flags & 1) ? 1 : 0);
}

// iSND_CreateVoice con un descriptor de datos ya resuelto (RAM o fichero de stream)
const char *s_createName = "?";
const char *s_createFile = "";     // nombre del LoadedSound (clave de wma_adpcm.pak)

// ---------------------------------------------------------------- wma_adpcm.pak (h6a_wma_pak.py): xWMA -> MS-ADPCM
struct PakEnt { unsigned hash, frames, rate; unsigned short ch, align; unsigned long long off; unsigned size, pad; };
static_assert(sizeof(PakEnt) == 32, "PakEnt");
const unsigned char *s_pak; size_t s_pakSize; const PakEnt *s_pakEnt; unsigned s_pakCount; unsigned s_wmaMiss, s_wmaHit;

unsigned Fnv1a(const char *n)
{
    unsigned h = 0x811C9DC5u;
    for (; *n; ++n) { unsigned char c = (unsigned char)tolower((unsigned char)*n); if (c == '\\') c = '/'; h ^= c; h *= 0x01000193u; }
    return h;
}

void PakOpen()
{
    if (s_pak) return;
    const char *gd = getenv("P3_GAMEDATA");
    const std::string path = std::string(gd ? gd : "../../gamedata") + "/h6a/wma_adpcm.pak";
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "[H6A] FALTA %s: los sonidos xWMA (casi todos los cargados en RAM: disparos, voces, zombis) NO suenan. "
                          "Generalo con 'h6a_pak.sh build' (harness2) y en el Fold copialo con 'h6a_pak.sh fold' a files/game/h6a/\n", path.c_str()); return; }
    struct stat st; fstat(fd, &st);
    void *m = st.st_size > 12 ? mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (m == MAP_FAILED) return;
    const unsigned char *p = (const unsigned char *)m;
    unsigned ver, n; memcpy(&ver, p + 4, 4); memcpy(&n, p + 8, 4);
    if (memcmp(p, "H6AW", 4) || ver != 1 || 12 + 32ull * n > (unsigned long long)st.st_size) { munmap(m, (size_t)st.st_size); fprintf(stderr, "[H6A] %s no valido\n", path.c_str()); return; }
    // OLA 14: integridad barata en el arranque (la completa, con decodificacion, es 'h6a_pak.sh verify'): orden para la busqueda
    // binaria y cada entrada dentro del fichero con canales y bloque coherentes. Un paquete roto no se usa: mejor sin xWMA que basura.
    const PakEnt *pe = (const PakEnt *)(p + 12); unsigned bad = 0;
    for (unsigned i = 0; i < n; ++i)
        if ((i && pe[i].hash <= pe[i - 1].hash) || pe[i].off < 12 + 32ull * n || pe[i].off + pe[i].size > (unsigned long long)st.st_size
            || (pe[i].ch != 1 && pe[i].ch != 2) || pe[i].align != 256u * pe[i].ch || !pe[i].rate) ++bad;
    if (bad) { munmap(m, (size_t)st.st_size); fprintf(stderr, "[H6A] %s CORRUPTO (%u de %u entradas mal): no se usa; regeneralo con 'h6a_pak.sh build'\n", path.c_str(), bad, n); return; }
    s_pak = p; s_pakSize = (size_t)st.st_size; s_pakEnt = pe; s_pakCount = n;
    madvise(m, (size_t)st.st_size, MADV_RANDOM);   // OLA 15C: acceso por entradas sueltas, sin lectura anticipada que infle el RSS
    fprintf(stderr, "[H6A] wma_adpcm.pak: %u sonidos, %.1f MB (mmap, cabecera comprobada)\n", n, st.st_size / 1048576.0);
}

bool PakLookup(const char *name, Src &s)
{
    if (!s_pakCount || !name) return false;
    const unsigned h = Fnv1a(name);
    unsigned lo = 0, hi = s_pakCount;
    while (lo < hi) { const unsigned mid = (lo + hi) / 2; if (s_pakEnt[mid].hash < h) lo = mid + 1; else hi = mid; }
    if (lo >= s_pakCount || s_pakEnt[lo].hash != h) return false;
    const PakEnt &e = s_pakEnt[lo];
    if (e.off + e.size > s_pakSize || e.ch != s.ch) return false;
    s.data = s_pak + e.off; s.dataSize = e.size; s.frames = e.frames; s.rate = e.rate;
    s.format = SND_ASSET_FORMAT_MSADPCM; s.align = e.align; s.spb = (e.align - 7u * e.ch) * 2u / e.ch + 2u;
    ++s_wmaHit;
    return true;
}   // alias de la voz que se crea (g_snd.voice[].alias aun no esta puesto en SND_StartAliasRam)
int CreateVoice(int voiceIndex, const Src &src, int isLooping, int bus, StreamFile *file)
{
    if (src.format != SND_ASSET_FORMAT_PCMS16 && src.format != SND_ASSET_FORMAT_MSADPCM)
    {
        static int warned[16];
        if (src.format >= 0 && src.format < 16 && !warned[src.format]++)
            fprintf(stderr, "[H6A] formato %d sin decodificador (alias %s): no suena\n", src.format, s_createName);
        Com_PrintError(9, "invalid asset for alias %s\n", s_createName);
        return 0;
    }
    if (src.ch < 1 || src.ch > 2 || !src.rate) return 0;
    std::lock_guard<std::mutex> lk(s_mx);
    MixVoice &v = s_v[voiceIndex];
    memset(&v, 0, sizeof v);
    v.created = true; v.looping = isLooping != 0; v.bus = bus; v.src = src; v.ratio = 1.0f;
    if (!v.src.align) { v.src.align = 262 * src.ch; v.src.spb = 512; }
    v.cacheBlk[0] = v.cacheBlk[1] = -1; v.file = file;
    return 1;
}
} // namespace

// ================================================================ interfaz del driver (snd_driver_xaudio2.h)
bool __cdecl SD_Xaudio2CanInit() { return true; }

bool SND_IsVoiceFree(int voiceIndex)
{
    return !g_snd.voiceAliasHash[voiceIndex];
}

void __cdecl SD_TruncateAudioDeviceNames(Font_s *font, float scale, int size) { (void)font; (void)scale; (void)size; }

void __cdecl Snd_StreamInit()
{
    if (s_loaderRun) return;
    s_loaderRun = true;
    s_loader = std::thread(LoaderMain);
}

void __cdecl Snd_StreamFini()
{
    if (!s_loaderRun) return;
    { std::lock_guard<std::mutex> lk(s_qmx); s_loaderRun = false; }
    s_qcv.notify_all();
    s_loader.join();
}

static float H6_ReadVolume()
{
    const char *e = getenv("H6A_VOL");
#ifdef __ANDROID__
    static char prop[PROP_VALUE_MAX];
    if (!e && __system_property_get("debug.bo1.vol", prop) > 0) e = prop;
#endif
    float v = e ? (float)atof(e) : 1.0f;
    if (!(v >= 0.0f)) v = 0.0f; if (v > 1.0f) v = 1.0f;
    return v;
}

char __cdecl SD_Init()
{
    const char *on = getenv("H6A");
    if (on && !atoi(on)) { fprintf(stderr, "[H6A] sonido desactivado (H6A=0)\n"); return 0; }
    if (s_open) return 1;
    s_t0 = std::chrono::steady_clock::now();
    if (const char *l = getenv("H6A_LOG")) s_logLeft = atoi(l);   // lineas '[H6A] play' (400 por defecto)
    if (!s_b) { void *m = nullptr; if (posix_memalign(&m, 128, sizeof(Buses))) return 0; s_b = (Buses *)m; }   // (aligned_alloc no esta en la API minima del NDK)
    memset(s_b, 0, sizeof(Buses));
    memset(s_v, 0, sizeof s_v);
    memset(s_masterState, 0, sizeof s_masterState); memset(s_nvState, 0, sizeof s_nvState);
    memset(&s_masterParams, 0, sizeof s_masterParams); memset(&s_nvParams, 0, sizeof s_nvParams);
    SND_RvParamsDefault(&s_rvParams);
    s_rvParams.frameRate = RATE;
    s_vol = H6_ReadVolume();
    if (const char *c = getenv("H6A_CACHE_MB")) s_cacheCap = (unsigned long long)atoi(c) << 20;
    if (const char *w = getenv("H6A_WAV")) { s_wav = fopen(w, "wb"); if (s_wav) WavHeader(s_wav, 0); }
    const char *dev = getenv("H6A_DEVICE");
    int ok = h6a_dev_start(48000, 2, QF, dev, Render, nullptr);
    if (!ok && !(dev && !strcmp(dev, "null")))
    {
        fprintf(stderr, "[H6A] sin dispositivo de audio: sigo con el backend null (mezcla sin salida)\n");
        ok = h6a_dev_start(48000, 2, QF, "null", Render, nullptr);
    }
    if (!ok) { fprintf(stderr, "[H6A] no arranca la salida de audio\n"); return 0; }
    s_open = true;
    // el harness sale con exit() sin SND_Shutdown: parar el dispositivo, cerrar el WAV y el hilo de carga antes de los
    // destructores estaticos (un std::thread vivo ahi es std::terminate)
    static bool s_atexit; if (!s_atexit) { s_atexit = true; atexit([] { h6a_dev_stop(); s_open = false; if (s_wav) { WavHeader(s_wav, s_wavFrames); fclose(s_wav); s_wav = nullptr; } Snd_StreamFini(); }); }
    fprintf(stderr, "[H6A] salida %s %u Hz periodo %u frames, volumen %.2f, mezclador %zu KB, cache streams %llu MB%s%s\n",
            h6a_dev_backend(), h6a_dev_rate(), h6a_dev_period(), s_vol, (sizeof(Buses) + sizeof s_v) / 1024, s_cacheCap >> 20,
            s_wav ? ", WAV: " : "", s_wav ? getenv("H6A_WAV") : "");
    PakOpen();
    SND_InitMasterVoice();
    return 1;
}

// SND_InitMasterVoice (snd_driver_xaudio2.cpp): solo la parte de configuracion de altavoces (la salida es siempre estereo)
void SND_InitMasterVoice()
{
    Snd_SetConfigStringsBySpeakerCount(2);
    for (unsigned index = 0; index < SND_GetSpeakerConfigCount(); ++index)
    {
        if (Snd_GetSpeakerConfig(index)->speakerCount == 2)
        {
            Dvar_SetInt((dvar_s *)snd_speakerConfiguration, index);
            return;
        }
    }
}

void SND_ShutdownMasterVoice()
{
    for (int i = 0; i < SND_MAX_VOICES; ++i) SND_StopVoice(i);
}

void SD_Shutdown()
{
    SND_ShutdownMasterVoice();
    h6a_dev_stop();
    s_open = false;
    if (s_wav) { WavHeader(s_wav, s_wavFrames); fclose(s_wav); s_wav = nullptr; }
}

// SD_PreUpdate (snd_driver_xaudio2.cpp) tal cual, a los parametros de bus del mezclador
// OLA 15C: las paginas del pak (mmap de fichero, solo lectura) ya mezcladas cuentan en el RSS aunque el kernel las pueda tirar.
// Cada H6A_PAKTRIM_S segundos (10 por defecto, 0 = nunca) se sueltan con MADV_DONTNEED: las voces que siguen sonando vuelven a
// leerlas de la cache de paginas (fallo menor, mismos bytes). Con MAP_PRIVATE y sin escrituras no se pierde nada.
static void PakTrim()
{
    if (!s_pak) return;
    static const int every = getenv("H6A_PAKTRIM_S") ? atoi(getenv("H6A_PAKTRIM_S")) : 10;
    static auto last = std::chrono::steady_clock::now();
    if (every <= 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - last).count() < every) return;
    last = now;
    madvise((void *)s_pak, s_pakSize, MADV_DONTNEED);
}

void __cdecl SD_PreUpdate()
{
    PakTrim();
    snd_rv_params rv;
    snd_dsp_master_params mp;
    snd_dsp_master_no_voice_params nv;
    float v2, v3, v4;
    SND_RvParamsDefault(&rv);
    const snd_radverb *radverb = SND_GetRadverb(SND_GetCurrentReverb());
    rv.frameRate = 48000.0f;
    if (radverb)
    {
        rv.smoothing = radverb->smoothing;
        rv.earlyTime = radverb->earlyTime;
        rv.lateTime = radverb->lateTime;
        rv.earlyGain = radverb->earlyGain;
        rv.lateGain = radverb->lateGain;
        rv.lateGainProx[3] = g_snd.proximityLateModifier[0];
        rv.lateGainProx[1] = g_snd.proximityLateModifier[1];
        rv.lateGainProx[2] = g_snd.proximityLateModifier[2];
        rv.lateGainProx[0] = g_snd.proximityLateModifier[3];
        rv.returnGain = radverb->returnGain;
        rv.earlyLpf = radverb->earlyLpf;
        rv.lateLpf = radverb->lateLpf;
        rv.inputLpf = radverb->inputLpf;
        rv.dampLpf = radverb->dampLpf;
        for (unsigned i = 0; i < 4; ++i) rv.wallReflect[i] = radverb->wallReflect;
        rv.dryGain = radverb->dryGain;
        rv.earlySize = radverb->earlySize;
        rv.lateSize = radverb->lateSize;
        rv.diffusion = radverb->diffusion;
        rv.delayMatrix = snd_radverb_matrix->current.color[0];
        if (SND_ActiveListenerCount() == 1)
        {
            v2 = atan2(g_snd.listeners[0].orient.axis[0][0], g_snd.listeners[0].orient.axis[0][1]);
            if ((float)((float)(v2 + 3.1415927) - 6.2831855) < 0.0) v4 = v2 + 3.1415927; else v4 = 6.2831855f;
            if ((float)(0.0 - (float)(v2 + 3.1415927)) < 0.0) v3 = v4; else v3 = 0.0f;
            rv.angle = v3;
        }
        else
            rv.angle = 0.0f;
    }
    memset(&mp, 0, sizeof mp);
    const snd_master *master = SND_GetMasterCurrent();
    mp.lowE = master->lowE; mp.lowG = master->lowG; mp.lowF = master->lowF; mp.lowQ = master->lowQ;
    mp.peak1E = master->peak1E; mp.peak1G = master->peak1G; mp.peak1F = master->peak1F; mp.peak1Q = master->peak1Q;
    mp.peak2E = master->peak2E; mp.peak2G = master->peak2G; mp.peak2F = master->peak2F; mp.peak2Q = master->peak2Q;
    mp.hiE = master->hiE; mp.hiG = master->hiG; mp.hiF = master->hiF; mp.hiQ = master->hiQ;
    mp.eqG = master->eqG;
    mp.limitE = master->limitE; mp.limitPG = master->limitPG; mp.limitMG = master->limitMG; mp.limitT = master->limitT;
    mp.limitR = master->limitR; mp.limitTA = master->limitTA; mp.limitTR = master->limitTR;
    nv = *(const snd_dsp_master_no_voice_params *)&master->compE;
    std::lock_guard<std::mutex> lk(s_mx);
    s_rvParams = rv; s_masterParams = mp; s_nvParams = nv;
}

void __cdecl SD_StopVoice(int voiceIndex)
{
    std::lock_guard<std::mutex> lk(s_mx);
    MixVoice &v = s_v[voiceIndex];
    if (v.file) { --v.file->refs; v.file->lastUse = ++s_useTick; }
    memset(&v, 0, sizeof v);
}

int __cdecl iSND_CreateVoice(int voiceIndex, const snd_asset *snd, int isLooping, int bus)
{
    Src s; s.data = (const unsigned char *)snd->data.get(); s.dataSize = snd->data_size; s.frames = snd->frame_count;
    s.rate = snd->frame_rate; s.ch = snd->channel_count; s.format = snd->format;
    if (s.format == SND_ASSET_FORMAT_WMA && !PakLookup(s_createFile, s)) ++s_wmaMiss;
    return CreateVoice(voiceIndex, s, isLooping, bus, nullptr);
}

void __cdecl iSND_ChannelError(int voiceIndex)
{
    const snd_alias_t *alias = g_snd.voice[voiceIndex].alias;
    SND_LengthNotify(voiceIndex, 0);
    SND_StopVoice(voiceIndex);
    alias->soundFile->exists = 0;
}

int __cdecl SND_StartAliasRam(SndStartAliasInfo *startAliasInfo, int voiceIndex)
{
    const bool isLooping = (startAliasInfo->alias->flags & 1) != 0;
    snd_asset *snd = &startAliasInfo->alias->soundFile->u.loadSnd->sound;
    s_createName = startAliasInfo->alias->name.get();
    s_createFile = startAliasInfo->alias->soundFile->u.loadSnd->name.get();
    if (!iSND_CreateVoice(voiceIndex, snd, isLooping, (startAliasInfo->alias->flags & 0x3000) >> 12))
        return -1;
    const unsigned rate = snd->frame_rate;
    unsigned total_msec;
    if (isLooping) total_msec = 0;
    else total_msec = startAliasInfo->startDelay + (__int64)((double)snd->frame_count * 1000.0 / (double)rate);
    SND_SetSoundFileVoiceInfo(voiceIndex, snd->channel_count, rate, total_msec, 0, SFLS_LOADED);
    SND_SetVoiceStartInfo(voiceIndex, startAliasInfo);
    LogPlay(voiceIndex, startAliasInfo->alias, "ram", s_v[voiceIndex].src);
    return startAliasInfo->playbackId;
}

static StreamFile *s_streamOf[10];

int __cdecl SND_StartAliasStream(SndStartAliasInfo *startAliasInfo, unsigned int voiceIndex)
{
    char filename2[260];
    const snd_alias_t *alias = startAliasInfo->alias;
    SND_AliasGetFileName(alias, filename2, 256);
    if (alias->soundFile->exists)
    {
        s_streamOf[voiceIndex] = StreamOpen(filename2);
        if (!s_streamOf[voiceIndex])
        {
            // OLA 15C: sin el fichero (p. ej. la VO de personajes/anunciador que no esta en los gamedata) la voz falla en silencio
            // por iSND_ChannelError; el aviso sale una vez por fichero y despues solo un recuento cada 64 fallos
            static std::vector<std::string> s_missing; static unsigned s_missN;
            ++s_missN;
            if (std::find(s_missing.begin(), s_missing.end(), filename2) == s_missing.end())
            {
                s_missing.push_back(filename2);
                Com_PrintError(9, "### Could not open streaming sound %s (alias %s; se avisa una vez)\n", filename2, alias->name.get());
            }
            else if (!(s_missN & 63)) fprintf(stderr, "[H6A] streams sin fichero: %u intentos, %zu ficheros distintos\n", s_missN, s_missing.size());
        }
        SND_SetVoiceStartInfo(voiceIndex, startAliasInfo);
        SND_SetSoundFileVoiceInfo(voiceIndex, 0, 0, 0, 0, SFLS_LOADING);
        SD_UpdateVoice(voiceIndex);
        if (g_snd.voice[voiceIndex].alias)
            return startAliasInfo->playbackId;
        return -1;
    }
    { static std::vector<std::string> s_noload; if (std::find(s_noload.begin(), s_noload.end(), filename2) == s_noload.end()) { s_noload.push_back(filename2);
      Com_PrintError(1, "Tried to play streamed sound '%s' from alias '%s', but it was not found at load time.\n", filename2, alias->name.get()); } }
    return SND_SetPlaybackIdNotPlayed(voiceIndex);
}

// SD_UpdateStreamVoice: con el fichero entero en memoria solo queda crear la voz cuando termina la carga (SND_STREAM_OK) o
// marcar el error (SND_STREAM_ERROR -> iSND_ChannelError); el final del fichero lo detecta SD_UpdateVoice (sin buffers).
void __cdecl SD_UpdateStreamVoice(int voiceIndex)
{
    if (s_v[voiceIndex].created) return;
    StreamFile *f = s_streamOf[voiceIndex];
    if (!f) { iSND_ChannelError(voiceIndex); return; }   // Snd_StreamOpen fallido -> SND_STREAM_ERROR
    const int st = f->state.load(std::memory_order_acquire);
    if (st == 0) return;
    if (st == 2) { fprintf(stderr, "[H6A] stream '%s' ilegible\n", f->name.c_str()); s_streamOf[voiceIndex] = nullptr; --f->refs; iSND_ChannelError(voiceIndex); return; }
    snd_voice_t *voice = &g_snd.voice[voiceIndex];
    const int isLooping = (voice->alias->flags & 1) != 0;
    Src s; s.data = f->data; s.dataSize = f->dataSize; s.frames = f->hdr.frame_count; s.rate = f->hdr.frame_rate;
    s.ch = f->hdr.channel_count; s.format = f->hdr.format;
    s_createName = voice->alias->name.get();
    s_streamOf[voiceIndex] = nullptr;   // la referencia pasa a la voz del mezclador (la suelta SD_StopVoice)
    if (!CreateVoice(voiceIndex, s, isLooping, (voice->alias->flags & 0x3000) >> 12, f))
    {
        --f->refs;
        iSND_ChannelError(voiceIndex);
        return;
    }
    const unsigned lengthMS = (unsigned)(1000LL * f->hdr.frame_count / f->hdr.frame_rate);
    SND_SetSoundFileVoiceInfo(voiceIndex, f->hdr.channel_count, f->hdr.frame_rate, lengthMS, 0, SFLS_LOADED);
    SND_UpdateVoice(&g_snd.voice[voiceIndex], 0.0);
    char kind[64]; snprintf(kind, sizeof kind, "stream (%.1f MB, carga %.0f ms)", f->size / 1048576.0, f->loadMs);
    LogPlay(voiceIndex, voice->alias, kind, s);
}

void __cdecl SD_PauseVoice(int voiceIndex)
{
    g_snd.voice[voiceIndex].paused = 1;
    if (g_snd.voice[voiceIndex].soundFileInfo.loadingState != SFLS_LOADING)
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_v[voiceIndex].playing = false;
    }
}

void __cdecl SD_UnpauseVoice(int voiceIndex)
{
    if (g_snd.voice[voiceIndex].soundFileInfo.loadingState != SFLS_LOADING)
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_v[voiceIndex].created) s_v[voiceIndex].playing = true;
    }
}

__int64 SD_BO1SamplesPlayed(int voiceIndex)
{
    if (g_snd.voice[voiceIndex].soundFileInfo.loadingState == SFLS_LOADING || !s_v[voiceIndex].created)
        return -1;
    std::lock_guard<std::mutex> lk(s_mx);
    return s_v[voiceIndex].samplesPlayed;
}

unsigned int __cdecl SDXA2_ChannelFixup(unsigned int outputChannels, unsigned int channel)
{
    if (outputChannels != 8) return channel;
    switch (channel) { case 4u: return 6; case 5u: return 7; case 6u: return 4; case 7u: return 5; default: return channel; }
}

// SDXA2_UpdateVoiceSends (snd_driver_xaudio2.cpp): parametros del efecto de la voz y matrices seca/humeda (salida estereo)
void __cdecl SDXA2_UpdateVoiceSends(int voiceIndex)
{
    snd_voice_t *voice = &g_snd.voice[voiceIndex];
    snd_dsp_source_params params;
    params.lpfAttenuation = SND_GetLpfLevel(voice);
    params.lpfRatio = 0.25f;
    params.frameRate = 48000.0f;
    params.futz.blend = voice->futzBlend;
    params.futz.bpfF = snd_futz_bpf_f->current.value;
    params.futz.bpfQ = snd_futz_bpf_q->current.value;
    params.futz.lsG = snd_futz_ls_g->current.value;
    params.futz.lsF = snd_futz_ls_f->current.value;
    params.futz.lsQ = snd_futz_ls_q->current.value;
    params.futz.distortion = snd_futz_distortion->current.value;
    params.futz.pregain = snd_futz_preg->current.value;
    params.futz.postgain = snd_futz_postg->current.value;
    params.futz.squelch.tg = snd_futz_tg->current.value;
    params.futz.squelch.th = snd_futz_th->current.value;
    params.futz.preclip = snd_futz_clip_pre->current.value;
    params.futz.postclip = snd_futz_clip_post->current.value;
    unsigned inputChannels = voice->pan.input_channel_count;
    const unsigned outputChannels = voice->pan.output_channel_count;
    if (inputChannels > 2) inputChannels = 2;
    float wet[4][2] = {}, dry[2][2] = {};
    for (unsigned m = 0; m < inputChannels; ++m)
    {
        if (outputChannels == 1)
            for (int k = 0; k < 4; ++k) wet[k][m] = Snd_SpeakerMapGetVolume(&voice->pan, m, 0) * 0.5 * voice->wetLevel;
        else
        {
            wet[0][m] = Snd_SpeakerMapGetVolume(&voice->pan, m, 0) * 0.70710677 * voice->wetLevel;
            wet[1][m] = Snd_SpeakerMapGetVolume(&voice->pan, m, 1) * 0.70710677 * voice->wetLevel;
            wet[2][m] = Snd_SpeakerMapGetVolume(&voice->pan, m, 1) * 0.70710677 * voice->wetLevel;
            wet[3][m] = Snd_SpeakerMapGetVolume(&voice->pan, m, 0) * 0.70710677 * voice->wetLevel;
        }
    }
    for (unsigned channel = 0; channel < outputChannels && channel < 2; ++channel)
        for (unsigned j = 0; j < inputChannels; ++j)
            dry[channel][j] = Snd_SpeakerMapGetVolume(&voice->pan, j, SDXA2_ChannelFixup(outputChannels, channel)) * voice->dryLevel;
    if (outputChannels == 1)
        for (unsigned j = 0; j < inputChannels; ++j) dry[1][j] = dry[0][j];   // salida mono (no se usa): duplicada en L/R
    std::lock_guard<std::mutex> lk(s_mx);
    MixVoice &v = s_v[voiceIndex];
    v.params = params;
    memcpy(v.wet, wet, sizeof wet); memcpy(v.dry, dry, sizeof dry);
    v.ic = (int)inputChannels;
    v.sendsValid = true;
}

void __cdecl SD_UpdateVoice(unsigned int voiceIndex)
{
    bcassert(voiceIndex, SND_MAX_VOICES);
    iassert(!SND_IsVoiceFree(voiceIndex));
    if (!SND_IsStream(voiceIndex) || (SD_UpdateStreamVoice(voiceIndex), g_snd.voiceAliasHash[voiceIndex]))
    {
        if (g_snd.voice[voiceIndex].soundFileInfo.loadingState != SFLS_LOADING)
        {
            snd_voice_t *voice = &g_snd.voice[voiceIndex];
            SDXA2_UpdateVoiceSends(voiceIndex);
            bool queued;
            {
                std::lock_guard<std::mutex> lk(s_mx);
                MixVoice &v = s_v[voiceIndex];
                queued = v.created && !v.done;
                if (!voice->startDelay && !v.samplesPlayed && queued && !voice->paused) v.playing = true;
                if (queued)
                {
                    const double p = SND_GetPitch(voice);
                    v.ratio = (float)(p > 2.0 ? 2.0 : p);
                }
            }
            if (!queued) SND_StopVoice(voiceIndex);
        }
    }
}

int __cdecl SD_StartAlias(SndStartAliasInfo *startAliasInfo, unsigned int voice)
{
    bcassert(voice, SND_MAX_VOICES);
    if (SND_IsStream(voice)) return SND_StartAliasStream(startAliasInfo, voice);
    return SND_StartAliasRam(startAliasInfo, voice);
}

// Para h5_audio_cl.cpp: resumen de la cache de streams
void H6A_CacheInfo(unsigned *files, unsigned long long *bytes)
{
    *files = (unsigned)s_files.size(); *bytes = s_cacheBytes;
}
void H6A_WmaInfo(unsigned *hit, unsigned *miss) { *hit = s_wmaHit; *miss = s_wmaMiss; }
