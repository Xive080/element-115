// h22_codec.cpp (OLA F3): ver h22_codec.h. C++17 portable (Mac y Android); solo libvpx y libopus como dependencias (con H22_VPX).
#include "h22_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#ifdef H22_VPX
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#include <opus/opus.h>
#endif

namespace {
// ---------------------------------------------------------------- demuxer WebM (subconjunto de Matroska que escribe ffmpeg y cualquier muxer)
enum : uint32_t {
    ID_SEGMENT = 0x18538067, ID_INFO = 0x1549A966, ID_TCSCALE = 0x2AD7B1, ID_DURATION = 0x4489, ID_TRACKS = 0x1654AE6B, ID_TRACK = 0xAE,
    ID_TRACKNUM = 0xD7, ID_TRACKTYPE = 0x83, ID_CODEC = 0x86, ID_CODECPRIV = 0x63A2, ID_VIDEO = 0xE0, ID_AUDIO = 0xE1, ID_PIXW = 0xB0,
    ID_PIXH = 0xBA, ID_RATE = 0xB5, ID_CHANNELS = 0x9F, ID_DEFDUR = 0x23E383, ID_CLUSTER = 0x1F43B675, ID_CLTIME = 0xE7,
    ID_SIMPLEBLOCK = 0xA3, ID_BLOCKGROUP = 0xA0, ID_BLOCK = 0xA1,
};
struct Pkt { size_t off, size; double ms; };
struct Track { int num = 0, type = 0, w = 0, h = 0, ch = 0; double rate = 0, defDur = 0; std::string codec; std::vector<uint8_t> priv; };

struct Demux {
    const uint8_t *d = nullptr; size_t n = 0;
    double scale = 1e6, durTicks = 0, clusterTc = 0;
    std::vector<Track> tracks; int vnum = -1, anum = -1;
    std::vector<Pkt> video, audio;
    bool bad = false;

    bool Id(size_t &p, uint32_t &id) const
    {
        if (p >= n) return false;
        const uint8_t b = d[p]; int len = b & 0x80 ? 1 : b & 0x40 ? 2 : b & 0x20 ? 3 : b & 0x10 ? 4 : 0; if (!len || p + len > n) return false;
        id = 0; for (int i = 0; i < len; ++i) id = id << 8 | d[p + i]; p += len; return true;
    }
    // tamano (vint); *unknown = todos los bits a 1
    bool Size(size_t &p, uint64_t &v, bool *unknown) const
    {
        if (p >= n) return false;
        const uint8_t b = d[p]; int len = 1; while (len <= 8 && !(b & (0x80 >> (len - 1)))) ++len; if (len > 8 || p + len > n) return false;
        v = b & (0xFF >> len); bool ones = v == (uint64_t)(0xFF >> len);
        for (int i = 1; i < len; ++i) { v = v << 8 | d[p + i]; ones = ones && d[p + i] == 0xFF; }
        if (unknown) *unknown = ones; p += len; return true;
    }
    uint64_t UInt(size_t p, size_t len) const { uint64_t v = 0; for (size_t i = 0; i < len && i < 8; ++i) v = v << 8 | d[p + i]; return v; }
    double Float(size_t p, size_t len) const
    {
        if (len == 4) { uint32_t u = (uint32_t)UInt(p, 4); float f; memcpy(&f, &u, 4); return f; }
        if (len == 8) { uint64_t u = UInt(p, 8); double f; memcpy(&f, &u, 8); return f; }
        return 0;
    }
    // bloque (SimpleBlock o Block): numero de pista, tiempo relativo int16, banderas y encaje
    void Block(size_t p, size_t end)
    {
        uint64_t tn; if (!Size(p, tn, nullptr) || p + 3 > end) return;
        const int16_t rel = (int16_t)(d[p] << 8 | d[p + 1]); const uint8_t flags = d[p + 2]; p += 3;
        std::vector<Pkt> *dst = (int)tn == vnum ? &video : (int)tn == anum ? &audio : nullptr; if (!dst) return;
        const double ms = (clusterTc + rel) * scale / 1e6;
        const int lacing = (flags >> 1) & 3;
        if (!lacing) { dst->push_back({ p, end - p, ms }); return; }
        if (p >= end) return;
        const int count = d[p++] + 1; std::vector<size_t> sz(count, 0); size_t sum = 0;
        if (lacing == 1) {   // Xiph
            for (int i = 0; i < count - 1; ++i) { size_t s = 0; while (p < end) { const uint8_t b = d[p++]; s += b; if (b != 255) break; } sz[i] = s; sum += s; }
        } else if (lacing == 3) {   // EBML: el primero tamano entero, los demas diferencias con signo
            uint64_t first; if (!Size(p, first, nullptr)) return; sz[0] = (size_t)first; sum = sz[0]; int64_t prev = (int64_t)first;
            for (int i = 1; i < count - 1; ++i) {
                const size_t q = p; uint64_t raw; if (!Size(p, raw, nullptr)) return; const int len = (int)(p - q);
                const int64_t bias = ((int64_t)1 << (7 * len - 1)) - 1; prev += (int64_t)raw - bias; if (prev < 0) return; sz[i] = (size_t)prev; sum += sz[i];
            }
        } else {   // fijo
            const size_t each = (end - p) / count; for (int i = 0; i < count - 1; ++i) sz[i] = each; sum = each * (count - 1);
        }
        if (p + sum > end) return; sz[count - 1] = end - p - sum;
        for (int i = 0; i < count; ++i) { dst->push_back({ p, sz[i], ms }); p += sz[i]; }
    }
    void Parse(size_t p, size_t end, Track *tr)
    {
        while (p < end && !bad) {
            uint32_t id; uint64_t sz; bool unk = false;
            if (!Id(p, id) || !Size(p, sz, &unk)) { bad = p < end && video.empty(); return; }
            size_t e = unk ? end : p + (size_t)sz; if (e > end) e = end;   // fichero cortado: lo que haya
            switch (id) {
            case ID_SEGMENT: case ID_INFO: case ID_TRACKS: case ID_BLOCKGROUP: Parse(p, e, tr); break;
            case ID_CLUSTER: Parse(p, e, tr); if (unk) return; break;   // tamano desconocido: sus hijos llegan hasta el final del padre
            case ID_TRACK: { Track t; Parse(p, e, &t); tracks.push_back(t);
                if (t.type == 1 && vnum < 0 && t.codec == "V_VP9") vnum = t.num; if (t.type == 2 && anum < 0 && t.codec == "A_OPUS") anum = t.num; break; }
            case ID_VIDEO: case ID_AUDIO: Parse(p, e, tr); break;
            case ID_TCSCALE: scale = (double)UInt(p, e - p); break;
            case ID_DURATION: durTicks = Float(p, e - p); break;
            case ID_CLTIME: clusterTc = (double)UInt(p, e - p); break;
            case ID_SIMPLEBLOCK: case ID_BLOCK: Block(p, e); break;
            default:
                if (tr) {
                    if (id == ID_TRACKNUM) tr->num = (int)UInt(p, e - p); else if (id == ID_TRACKTYPE) tr->type = (int)UInt(p, e - p);
                    else if (id == ID_CODEC) tr->codec.assign((const char *)d + p, e - p); else if (id == ID_CODECPRIV) tr->priv.assign(d + p, d + e);
                    else if (id == ID_PIXW) tr->w = (int)UInt(p, e - p); else if (id == ID_PIXH) tr->h = (int)UInt(p, e - p);
                    else if (id == ID_RATE) tr->rate = Float(p, e - p); else if (id == ID_CHANNELS) tr->ch = (int)UInt(p, e - p);
                    else if (id == ID_DEFDUR) tr->defDur = (double)UInt(p, e - p);
                }
                break;
            }
            p = e;
        }
    }
    const Track *Find(int num) const { for (const Track &t : tracks) if (t.num == num) return &t; return nullptr; }
};

inline uint8_t Clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
// I420 -> BGRA, BT.601 de rango limitado (el de libvpx-vp9 por defecto, color_range=tv)
void YuvToBgra(const uint8_t *Y, int ys, const uint8_t *U, int us, const uint8_t *V, int vs, int w, int h, uint8_t *out, int pitch)
{
    for (int y = 0; y < h; ++y) {
        const uint8_t *yr = Y + y * ys, *ur = U + (y >> 1) * us, *vr = V + (y >> 1) * vs; uint8_t *o = out + (size_t)y * pitch;
        for (int x = 0; x < w; ++x) {
            const int c = 298 * (yr[x] - 16), dU = ur[x >> 1] - 128, dV = vr[x >> 1] - 128;
            o[0] = Clamp8((c + 516 * dU + 128) >> 8); o[1] = Clamp8((c - 100 * dU - 208 * dV + 128) >> 8); o[2] = Clamp8((c + 409 * dV + 128) >> 8); o[3] = 255; o += 4;
        }
    }
}
} // namespace

struct H22Clip {
    std::vector<uint8_t> file; Demux dm; H22Info info{};
    size_t vi = 0, ai = 0;
#ifdef H22_VPX
    vpx_codec_ctx_t vpx{}; bool vpxOk = false;
    OpusDecoder *opus = nullptr; int preSkip = 0;
    std::vector<float> pcm; size_t pcmPos = 0;
#endif
};

const char *h22c_backend()
{
#ifdef H22_VPX
    static char b[160]; if (!b[0]) snprintf(b, sizeof b, "%s + %s", vpx_codec_version_str(), opus_get_version_string()); return b;
#else
    return "sin decodificadores (compilado sin libvpx/libopus)";
#endif
}

H22Clip *h22c_open(const char *path, H22Info *info, char *why, int whyLen)
{
    auto Fail = [&](const char *m, H22Clip *c) -> H22Clip * { if (why && whyLen > 0) snprintf(why, whyLen, "%s", m); delete c; return nullptr; };
    H22Clip *c = new H22Clip;
    FILE *f = fopen(path, "rb"); if (!f) return Fail("no se puede abrir el fichero", c);
    fseek(f, 0, SEEK_END); const long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (64L << 20)) { fclose(f); return Fail("tamano de fichero no valido (tope 64 MB)", c); }
    c->file.resize((size_t)len); const size_t got = fread(c->file.data(), 1, (size_t)len, f); fclose(f);
    if (got != (size_t)len) return Fail("lectura incompleta", c);
    if (len < 4 || c->file[0] != 0x1A || c->file[1] != 0x45 || c->file[2] != 0xDF || c->file[3] != 0xA3) return Fail("no es un fichero EBML/WebM", c);
    c->dm.d = c->file.data(); c->dm.n = c->file.size(); c->dm.Parse(0, c->dm.n, nullptr);
    const Track *vt = c->dm.Find(c->dm.vnum), *at = c->dm.Find(c->dm.anum);
    if (!vt || c->dm.video.empty()) return Fail("sin pista de video VP9", c);
    H22Info &in = c->info; in.w = vt->w; in.h = vt->h;
    in.seconds = c->dm.durTicks > 0 ? c->dm.durTicks * c->dm.scale / 1e9 : c->dm.video.back().ms / 1000.0;
    in.fps = vt->defDur > 0 ? 1e9 / vt->defDur : (c->dm.video.size() > 1 && in.seconds > 0 ? c->dm.video.size() / in.seconds : 30.0);
    in.audioRate = at && !c->dm.audio.empty() ? 48000 : 0; in.audioChannels = at ? (at->ch > 0 ? at->ch : 2) : 0;   // Opus decodifica siempre a 48 kHz
    if (in.w <= 0 || in.h <= 0 || in.w > 4096 || in.h > 4096) return Fail("tamano de imagen no valido", c);
#ifdef H22_VPX
    vpx_codec_dec_cfg_t cfg{}; cfg.threads = 2; cfg.w = in.w; cfg.h = in.h;
    if (vpx_codec_dec_init(&c->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) return Fail("libvpx no inicia VP9", c);
    c->vpxOk = true;
    if (in.audioRate) {
        int err = 0; c->opus = opus_decoder_create(48000, in.audioChannels > 2 ? 2 : in.audioChannels, &err);
        if (!c->opus || err != OPUS_OK) { c->opus = nullptr; in.audioRate = 0; in.audioChannels = 0; }
        else { in.audioChannels = in.audioChannels > 2 ? 2 : in.audioChannels; if (at->priv.size() >= 12) c->preSkip = at->priv[10] | at->priv[11] << 8; }   // OpusHead: pre-skip
    }
    if (info) *info = in;
    return c;
#else
    if (info) *info = in;
    return Fail(h22c_backend(), c);
#endif
}

bool h22c_video(H22Clip *c, uint8_t *bgra, int pitch, double *ptsMs)
{
#ifdef H22_VPX
    while (c->vi < c->dm.video.size()) {
        const Pkt &k = c->dm.video[c->vi++];
        if (vpx_codec_decode(&c->vpx, c->file.data() + k.off, (unsigned)k.size, nullptr, 0) != VPX_CODEC_OK) continue;   // paquete roto: el siguiente
        vpx_codec_iter_t it = nullptr; vpx_image_t *img = nullptr, *last = nullptr;
        while ((img = vpx_codec_get_frame(&c->vpx, &it))) last = img;
        if (!last || last->fmt != VPX_IMG_FMT_I420) continue;   // fotograma oculto (superframe) o formato que no es 4:2:0 de 8 bits
        const int w = (int)last->d_w < c->info.w ? (int)last->d_w : c->info.w, h = (int)last->d_h < c->info.h ? (int)last->d_h : c->info.h;
        YuvToBgra(last->planes[0], last->stride[0], last->planes[1], last->stride[1], last->planes[2], last->stride[2], w, h, bgra, pitch);
        if (ptsMs) *ptsMs = k.ms;
        return true;
    }
#endif
    (void)c; (void)bgra; (void)pitch; (void)ptsMs;
    return false;
}

int h22c_audio(H22Clip *c, float *out, int maxFrames)
{
#ifdef H22_VPX
    if (!c->opus) return 0;
    const int ch = c->info.audioChannels; int done = 0;
    while (done < maxFrames) {
        if (c->pcmPos >= c->pcm.size()) {
            if (c->ai >= c->dm.audio.size()) break;
            const Pkt &k = c->dm.audio[c->ai++];
            c->pcm.resize(5760 * ch); c->pcmPos = 0;
            const int n = opus_decode_float(c->opus, c->file.data() + k.off, (opus_int32)k.size, c->pcm.data(), 5760, 0);
            if (n <= 0) { c->pcm.clear(); continue; }
            c->pcm.resize((size_t)n * ch);
            if (c->preSkip > 0) { const int s = c->preSkip < n ? c->preSkip : n; c->pcmPos = (size_t)s * ch; c->preSkip -= s; }
            continue;
        }
        const int avail = (int)((c->pcm.size() - c->pcmPos) / ch), take = avail < maxFrames - done ? avail : maxFrames - done;
        memcpy(out + (size_t)done * ch, c->pcm.data() + c->pcmPos, sizeof(float) * take * ch); c->pcmPos += (size_t)take * ch; done += take;
    }
    return done;
#else
    (void)c; (void)out; (void)maxFrames; return 0;
#endif
}

void h22c_close(H22Clip *c)
{
    if (!c) return;
#ifdef H22_VPX
    if (c->vpxOk) vpx_codec_destroy(&c->vpx);
    if (c->opus) opus_decoder_destroy(c->opus);
#endif
    delete c;
}
