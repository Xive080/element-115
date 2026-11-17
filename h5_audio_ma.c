// H6a (sonido): salida de audio portable con miniaudio 0.11.21 (third_party/miniaudio, MIT-0 / dominio publico).
// Se compila APARTE, en C y sin el prelude de compat (Windows.h del harness choca con los tipos de miniaudio).
// Mac: CoreAudio. Android: AAudio (OpenSL ES de reserva), ambos por dlopen. "null": hilo propio a tiempo real sin dispositivo.
// API minima para h5_audio.cpp: un dispositivo de reproduccion f32 entrelazado que pide frames a una funcion del mezclador.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#include "miniaudio.h"
#include <string.h>

typedef void (*h6a_render_fn)(float *out, unsigned int frames, void *user);
static ma_device s_dev;
static ma_context s_ctx;
static int s_open;
static h6a_render_fn s_fn;
static void *s_user;
static char s_backend[64];

static void h6a_cb(ma_device *d, void *out, const void *in, ma_uint32 frames)
{
    (void)d; (void)in;
    s_fn((float *)out, frames, s_user);
}

// backend: NULL/"" = el del sistema; "null" = sin dispositivo (hilo a tiempo real). Devuelve 1 si arranca.
int h6a_dev_start(unsigned int rate, unsigned int channels, unsigned int periodFrames, const char *backend, h6a_render_fn fn, void *user)
{
    ma_backend nullBackend[1] = { ma_backend_null };
    ma_device_config cfg;
    if (s_open) return 1;
    s_fn = fn; s_user = user;
    if (ma_context_init(backend && !strcmp(backend, "null") ? nullBackend : NULL, backend && !strcmp(backend, "null") ? 1 : 0, NULL, &s_ctx) != MA_SUCCESS)
        return 0;
    cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = channels;
    cfg.sampleRate = rate;
    cfg.periodSizeInFrames = periodFrames;
    cfg.periods = 3;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.noPreSilencedOutputBuffer = MA_TRUE;
    cfg.dataCallback = h6a_cb;
#ifdef __ANDROID__
    cfg.aaudio.usage = ma_aaudio_usage_game;
    cfg.aaudio.contentType = ma_aaudio_content_type_sonification;
    cfg.opensl.streamType = ma_opensl_stream_type_media;
#endif
    if (ma_device_init(&s_ctx, &cfg, &s_dev) != MA_SUCCESS) { ma_context_uninit(&s_ctx); return 0; }
    strncpy(s_backend, ma_get_backend_name(s_ctx.backend), sizeof s_backend - 1);
    if (ma_device_start(&s_dev) != MA_SUCCESS) { ma_device_uninit(&s_dev); ma_context_uninit(&s_ctx); return 0; }
    s_open = 1;
    return 1;
}

const char *h6a_dev_backend(void) { return s_backend; }
unsigned int h6a_dev_rate(void) { return s_open ? s_dev.sampleRate : 0; }
unsigned int h6a_dev_period(void) { return s_open ? s_dev.playback.internalPeriodSizeInFrames : 0; }

void h6a_dev_stop(void)
{
    if (!s_open) return;
    ma_device_uninit(&s_dev);
    ma_context_uninit(&s_ctx);
    s_open = 0;
}
