// h22_video.cpp (OLA F3): reproductor del video de carga de cada mapa (<zona>_load.webm de los datos del juego; nunca en el repo ni en
// el APK). Rellena g_h18PlayVideo (h18_maps.cpp, contrato de la OLA M0): bloquea hasta el final del video o hasta saltarlo (tecla,
// clic o toque) y devuelve false si no puede (sin fichero, sin decodificadores, error): el juego sigue sin video y sin error.
//   - decodificacion PORTABLE en h22_codec.cpp (demuxer WebM propio + libvpx + libopus), igual en Mac y Android;
//   - imagen: textura dinamica BGRA del backend (g9::TexCreate/TexLock) copiada al backbuffer con g9::StretchRect, centrada con
//     bandas negras (sin shaders: vale antes de cargar ninguna zona);
//   - sonido: el mismo dispositivo miniaudio del juego (h6a_dev_start), parado antes de que arranque el sonido del motor; reloj del
//     video = muestras de audio consumidas (sin audio, reloj de pared). Volumen H6A_VOL como el juego;
//   - saltar: Mac, tecla o clic de la ventana (H10_MacInputInstall); Android, la app llama H22_VideoSkip() con un toque.
// Sin ventana (pruebas offscreen, selftest) no se reproduce salvo H22_FORCE=1. H22_SHOT=<prefijo> guarda capturas a 1, 5 y 10 s;
// H22_MAXS=<s> corta a esos segundos (pruebas). H18_NOVIDEO=1 lo omite (h18_maps.cpp).
#include "../vk/g9_api.h"
#include "h22_codec.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern bool (*g_h18PlayVideo)(const char *path);
int g_h22W, g_h22H;   // tamano del backbuffer (main2.cpp lo fija antes de llamar a H18_PlayLoadVideo)

#ifdef H6SND
extern "C" {
typedef void (*h6a_render_fn)(float *out, unsigned int frames, void *user);
int h6a_dev_start(unsigned int rate, unsigned int channels, unsigned int periodFrames, const char *backend, h6a_render_fn fn, void *user);
void h6a_dev_stop(void);
}
#endif
#ifdef H0_MACWIN
extern "C" { void WinPump(void); void H10_MacInputInstall(void (*ev)(int, float, float, int)); }
#endif

namespace {
std::atomic<bool> s_skip{ false };
struct Aud { H22Clip *clip; int ch; float vol; std::atomic<long long> frames{ 0 }; std::atomic<bool> ended{ false }; };
#ifdef H6SND
void Render(float *out, unsigned int n, void *user)
{
    Aud *a = (Aud *)user; const int ch = a->ch;
    std::vector<float> tmp((size_t)n * ch); const int got = a->ended ? 0 : h22c_audio(a->clip, tmp.data(), (int)n);
    for (unsigned i = 0; i < n; ++i) {   // el video puede ser mono o estereo; la salida es estereo
        const float l = (int)i < got ? tmp[(size_t)i * ch] * a->vol : 0.f, r = (int)i < got ? tmp[(size_t)i * ch + (ch > 1)] * a->vol : 0.f;
        out[i * 2] = l; out[i * 2 + 1] = r;
    }
    if (got < (int)n) a->ended = true;
    a->frames += got;
}
#endif
#ifdef H0_MACWIN
void MacEv(int kind, float, float, int) { if (kind == 1 || kind == 4) s_skip = true; }   // clic o tecla
#endif

bool Play(const char *path)
{
    const bool force = getenv("H22_FORCE") && atoi(getenv("H22_FORCE"));
    if (getenv("H21_MENU") && atoi(getenv("H21_MENU"))) { fprintf(stderr, "[H22] video de carga: primero el menu principal (se vera al relanzar con el mapa elegido)\n"); return false; }
    if (!g9::Active() || (!g9::SurfaceWidth() && !force)) { fprintf(stderr, "[H22] video de carga: sin ventana, se omite (H22_FORCE=1 lo fuerza)\n"); return false; }
    H22Info in{}; char why[200] = "";
    H22Clip *c = h22c_open(path, &in, why, sizeof why);
    if (!c) { fprintf(stderr, "[H22] video de carga: %s (%s)\n", why, h22c_backend()); return false; }
    g9::Tex *tex = g9::TexCreate(0, in.w, in.h, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, nullptr);
    g9::Surf *src = tex ? g9::SurfFromTex(tex, 0, 0, in.w, in.h) : nullptr, *bb = g9::SurfBackbuffer(false);
    if (!src || !bb) { if (tex) g9::Release(tex); h22c_close(c); fprintf(stderr, "[H22] video de carga: sin textura de video en el backend\n"); return false; }
    const int W = g_h22W > 0 ? g_h22W : g9::SurfaceWidth(), H = g_h22H > 0 ? g_h22H : g9::SurfaceHeight();
    RECT dr; { const double s = (double)W / in.w < (double)H / in.h ? (double)W / in.w : (double)H / in.h; const int dw = (int)(in.w * s), dh = (int)(in.h * s);
        dr.left = (W - dw) / 2; dr.top = (H - dh) / 2; dr.right = dr.left + dw; dr.bottom = dr.top + dh; }
    const RECT sr = { 0, 0, in.w, in.h };
    fprintf(stderr, "[H22] video de carga %dx%d %.2f fps %.1f s, audio %d Hz %d canales, %s; salida %dx%d\n", in.w, in.h, in.fps, in.seconds, in.audioRate, in.audioChannels, h22c_backend(), W, H);

    Aud aud; aud.clip = c; aud.ch = in.audioChannels; { const char *v = getenv("H6A_VOL"); aud.vol = v ? (float)atof(v) : 1.f; if (!(aud.vol >= 0)) aud.vol = 0; if (aud.vol > 1) aud.vol = 1; }
    bool audioOn = false;
#ifdef H6SND
    if (in.audioRate) audioOn = h6a_dev_start((unsigned)in.audioRate, 2, 480, getenv("H6A_DEVICE"), Render, &aud) != 0;
#endif
    s_skip = false;
#ifdef H0_MACWIN
    if (getenv("P4_WINDOW")) H10_MacInputInstall(MacEv);
#endif
    const char *shot = getenv("H22_SHOT"); const double maxS = getenv("H22_MAXS") ? atof(getenv("H22_MAXS")) : 0;
    const double shotAt[3] = { 1000, 5000, 10000 }; int nextShot = 0;
    auto t0 = std::chrono::steady_clock::now();
    auto Clock = [&]() -> double { return audioOn && !aud.ended ? aud.frames * 1000.0 / in.audioRate : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    std::vector<uint8_t> frame((size_t)in.w * in.h * 4);
    int shown = 0, late = 0; double pts = 0, decMs = 0; bool skipped = false;
    for (;;) {
        const auto d0 = std::chrono::steady_clock::now();
        if (!h22c_video(c, frame.data(), in.w * 4, &pts)) break;
        decMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - d0).count();
        if (maxS > 0 && pts > maxS * 1000) break;
        double now = Clock();
        if (pts + 100 < now) { ++late; continue; }   // mas de 100 ms tarde: se salta la imagen para no perder la sincronia
        while (now < pts && !s_skip) {
#ifdef H0_MACWIN
            WinPump();
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); now = Clock();
        }
        if (s_skip) { skipped = true; break; }
        UINT pitch = 0, slice = 0; uint8_t *p = (uint8_t *)g9::TexLock(tex, 0, 0, &pitch, &slice, D3DLOCK_DISCARD);
        if (!p) break;
        for (int y = 0; y < in.h; ++y) memcpy(p + (size_t)y * pitch, frame.data() + (size_t)y * in.w * 4, (size_t)in.w * 4);
        g9::TexUnlock(tex, 0, 0);
        g9::SetRenderTarget(0, bb);
        g9::Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.f, 0);
        g9::StretchRect(src, &sr, bb, &dr, D3DTEXF_LINEAR);
        if (shot && nextShot < 3 && pts >= shotAt[nextShot]) { char f[1024]; snprintf(f, sizeof f, "%s_%02ds.png", shot, (int)(shotAt[nextShot] / 1000)); int w, h; g9::ReadbackPNG(f, &w, &h); ++nextShot; }
        g9::Present(); ++shown;
#ifdef H0_MACWIN
        WinPump();
#endif
    }
    // fin: esperar a que suene lo que queda del audio (si no se salto), parar el dispositivo y dejar la pantalla en negro
#ifdef H6SND
    if (audioOn) {
        for (int i = 0; i < 300 && !aud.ended && !s_skip && !skipped && !(maxS > 0); ++i) {
#ifdef H0_MACWIN
            WinPump();
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        h6a_dev_stop();
    }
#endif
    g9::Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.f, 0); g9::Present();
    g9::Release(src); g9::Release(tex);
    const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "[H22] video de carga: %d imagenes mostradas, %d tarde (saltadas), decodificacion %.2f ms por imagen, %.1f s de %.1f%s\n",
        shown, late, shown + late ? decMs / (shown + late) : 0, wall / 1000, in.seconds, skipped || s_skip ? " (SALTADO por el jugador)" : "");
    h22c_close(c);
    return shown > 0;
}
struct Reg { Reg() { g_h18PlayVideo = Play; } } s_reg;   // g_h18PlayVideo es nullptr constante: se inicializa antes que esto
} // namespace

void H22_VideoSkip() { s_skip = true; }   // Android: toque en pantalla durante el video
