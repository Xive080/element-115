// h22_video.cpp (OLA F3, F3b): reproductor del video de carga de cada mapa (<zona>_load.webm de los datos del juego; nunca en el repo ni
// en el APK). Como el original, el video suena DURANTE la carga del mapa:
//   - BO1 llama a R_BeginRemoteScreenUpdate al empezar a cargar: el hilo de render sigue presentando la pantalla de carga con la
//     cinematica mientras el hilo principal carga zonas y arranca el servidor; R_EndRemoteScreenUpdate le devuelve el render. Si la
//     carga termina antes, la cinematica sigue hasta su final o hasta que el jugador la salta.
//   - Aqui el render es de un solo hilo, asi que se hace igual por tramos (H18_*LoadVideo* en h18_maps.cpp):
//       Begin (main2, tras crear el backend): abre el clip, arranca el hilo DECODIFICADOR y el audio, y muestra la primera imagen;
//       Pump (cooperativo): lo llama el hilo principal desde las lecturas de zona (ReadFileEx con g_h0InDbLoad, rt2.cpp) y entre
//         fases; presenta la imagen que toque segun el reloj del audio. Nunca durante un dibujo del motor;
//       Remote(true/false): en tramos en que el hilo principal NO usa el backend (SV_SpawnServer: GSC y entidades, sin render) un
//         hilo PRESENTADOR hace lo mismo, como el hilo de render del original;
//       End (h5_game, carga hecha, antes del primer frame): sigue el video hasta el final o hasta saltarlo, para el audio y libera.
//   - Contrato de la OLA M0 intacto: g_h18PlayVideo(path) (bloqueante) sigue siendo Begin + End. Sin ventana (pruebas offscreen,
//     selftest) no se reproduce salvo H22_FORCE=1; con H21_MENU=1 (menu principal) tampoco. Fuera del juego completo (sin H5_GAME)
//     se usa el modo bloqueante. Siempre devuelve false si no puede: el juego sigue sin video y sin error.
//   - Decodificacion PORTABLE en h22_codec.cpp (demuxer WebM propio + libvpx + libopus compiladas desde third_party).
//   - Imagen: textura dinamica BGRA del backend copiada al backbuffer con g9::StretchRect, centrada con bandas negras.
//   - Sonido: dispositivo miniaudio del juego (h6a_dev_start), libre hasta SND_Init (primer frame, despues de End). Reloj del video =
//     muestras de audio consumidas (sin audio, reloj de pared). Volumen H6A_VOL como el juego.
//   - Saltar: Mac, tecla o clic de la ventana (H10_MacInputInstall); Android, la app llama H22_VideoSkip() con un toque o una tecla
//     (H22_VideoActive() dice si hay video) y g_h22PollEvents bombea sus eventos durante la carga.
// Pruebas: H22_SHOT=<prefijo> guarda capturas a 1, 5 y 10 s; H22_MAXS=<s> corta a esos segundos; H22_THREAD=0 quita el hilo
// presentador (solo tramos cooperativos); H22_GAPLOG=1 anota los huecos de mas de 100 ms sin presentar; H22_BLOCK=1 vuelve al modo bloqueante antes de cargar (el de la OLA F3).
#include "../vk/g9_api.h"
#include "h22_codec.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern bool (*g_h18PlayVideo)(const char *path);
extern bool (*g_h18BeginVideo)(const char *path);
extern void (*g_h18VideoRemote)(bool on);
extern void (*g_h18EndVideo)();
extern void (*g_h0LoadPump)();   // rt2.cpp: lo llama ReadFileEx durante DB_LoadXFile
int g_h22W, g_h22H;   // tamano del backbuffer (main2.cpp lo fija antes de llamar a H18_PlayLoadVideo)
void (*g_h22PollEvents)() = nullptr;   // Android: la app bombea sus eventos (toques) durante la carga

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
using Clk = std::chrono::steady_clock;
double Ms(Clk::time_point a, Clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

std::atomic<bool> s_skip{ false }, s_active{ false }, s_loadDone{ false };   // s_loadDone: el video solo se puede saltar cuando la carga (mapa + shaders) ha terminado
struct Aud { H22Clip *clip = nullptr; int ch = 2; float vol = 1; std::atomic<long long> frames{ 0 }; std::atomic<bool> ended{ false }; };
struct Frame { std::vector<uint8_t> px; double pts = 0; };

struct Video {
    H22Clip *clip = nullptr; H22Info in{};
    g9::Tex *tex = nullptr; g9::Surf *src = nullptr, *bb = nullptr; RECT dr{}, sr{};
    Aud aud; bool audioOn = false;
    // hilo decodificador: hasta kQueue imagenes por delante
    static constexpr int kQueue = 4;
    std::mutex mq; std::deque<Frame> ready; std::vector<std::vector<uint8_t>> pool; std::atomic<bool> decDone{ false }, stop{ false };
    std::thread dec; double decMs = 0; int decoded = 0;
    // presentar (hilo principal en Pump/End o hilo presentador en Remote): s_present serializa
    std::mutex present; std::thread presenter; std::atomic<bool> remote{ false };
    Clk::time_point t0, lastPump, lastWin, loadDoneAt; bool loadDone = false;
    int shown = 0, late = 0, nextShot = 0, pumps = 0, remoteShown = 0; double maxGap = 0, maxS = 0; const char *shot = nullptr; bool skipped = false, blanked = false;
    double Clock() const { return audioOn && !aud.ended ? aud.frames * 1000.0 / in.audioRate : Ms(t0, Clk::now()); }
};
Video *s_v = nullptr;

#ifdef H6SND
void Render(float *out, unsigned int n, void *user)
{
    Aud *a = (Aud *)user; const int ch = a->ch;
    float tmp[2 * 4096]; unsigned done = 0;
    while (done < n) {   // el video puede ser mono o estereo; la salida es estereo
        const unsigned want = n - done < 4096 ? n - done : 4096;
        const int got = a->ended || s_skip ? 0 : h22c_audio(a->clip, tmp, (int)want);
        for (unsigned i = 0; i < want; ++i) {
            const float l = (int)i < got ? tmp[(size_t)i * ch] * a->vol : 0.f, r = (int)i < got ? tmp[(size_t)i * ch + (ch > 1)] * a->vol : 0.f;
            out[(done + i) * 2] = l; out[(done + i) * 2 + 1] = r;
        }
        a->frames += got; done += want;
        if (got < (int)want) { if (!s_skip) a->ended = true; for (; done < n; ++done) out[done * 2] = out[done * 2 + 1] = 0.f; }
    }
}
#endif
#ifdef H0_MACWIN
void MacEv(int kind, float, float, int) { if ((kind == 1 || kind == 4) && s_loadDone) s_skip = true; }   // clic o tecla
#endif

void DecodeMain(Video *v)
{
    const size_t sz = (size_t)v->in.w * v->in.h * 4;
    while (!v->stop && !s_skip) {
        std::vector<uint8_t> buf;
        { std::lock_guard<std::mutex> lk(v->mq);
          if ((int)v->ready.size() >= Video::kQueue) buf.clear(); else if (!v->pool.empty()) { buf.swap(v->pool.back()); v->pool.pop_back(); } else buf.resize(sz); }
        if (buf.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); continue; }
        double pts = 0; const auto d0 = Clk::now();
        if (!h22c_video(v->clip, buf.data(), v->in.w * 4, &pts) || (v->maxS > 0 && pts > v->maxS * 1000)) break;
        v->decMs += Ms(d0, Clk::now()); ++v->decoded;
        std::lock_guard<std::mutex> lk(v->mq); v->ready.push_back(Frame{ std::move(buf), pts });
    }
    v->decDone = true;
}

// presenta la imagen que toca ahora (descarta las que se quedaron atras). Devuelve true cuando el video ha terminado.
bool PresentDue(Video *v)
{
    std::lock_guard<std::mutex> pk(v->present);
    if (s_skip) {   // saltado: pantalla en negro una vez; la carga sigue
        if (!v->blanked) { v->blanked = true; g9::SetRenderTarget(0, v->bb); g9::Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.f, 0); g9::Present(); }
        return true;
    }
    const double now = v->Clock();
    Frame f; bool have = false;
    { std::lock_guard<std::mutex> lk(v->mq);
      while (!v->ready.empty() && v->ready.front().pts <= now) {
          if (have) { ++v->late; v->pool.push_back(std::move(f.px)); }
          f = std::move(v->ready.front()); v->ready.pop_front(); have = true;
      }
      if (!have && v->ready.empty() && v->decDone) return !v->audioOn || v->aud.ended; }
    if (!have) return false;
    UINT pitch = 0, slice = 0; uint8_t *p = (uint8_t *)g9::TexLock(v->tex, 0, 0, &pitch, &slice, D3DLOCK_DISCARD);
    if (p) {
        for (int y = 0; y < v->in.h; ++y) memcpy(p + (size_t)y * pitch, f.px.data() + (size_t)y * v->in.w * 4, (size_t)v->in.w * 4);
        g9::TexUnlock(v->tex, 0, 0);
        g9::SetRenderTarget(0, v->bb);
        g9::Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.f, 0);
        g9::StretchRect(v->src, &v->sr, v->bb, &v->dr, D3DTEXF_LINEAR);
        static const double shotAt[3] = { 1000, 5000, 10000 };
        if (v->shot && v->nextShot < 3 && f.pts >= shotAt[v->nextShot]) {
            char fn[1024]; snprintf(fn, sizeof fn, "%s_%02ds.png", v->shot, (int)(shotAt[v->nextShot] / 1000)); int w, h; g9::ReadbackPNG(fn, &w, &h); ++v->nextShot; }
        g9::Present(); ++v->shown; if (v->remote) ++v->remoteShown;
    }
    { std::lock_guard<std::mutex> lk(v->mq); v->pool.push_back(std::move(f.px)); }
    return false;
}

void Events(Video *v, bool always)
{
    const auto now = Clk::now();
    if (!always && Ms(v->lastWin, now) < 16) return;
    v->lastWin = now;
#ifdef H0_MACWIN
    WinPump();
#endif
    if (g_h22PollEvents) g_h22PollEvents();
}

void Pump()   // hilo principal, durante la carga
{
    Video *v = s_v; if (!v || v->remote) return;
    const auto now = Clk::now(); const double gap = Ms(v->lastPump, now);
    if (gap < 4) return;   // se llama en cada lectura de zona: barato si no toca nada
    if (gap > v->maxGap) v->maxGap = gap;
    static const bool gapLog = getenv("H22_GAPLOG") && atoi(getenv("H22_GAPLOG"));
    if (gapLog && gap > 100) fprintf(stderr, "[H22] hueco de %.0f ms sin presentar (t=%.0f ms de video)\n", gap, Ms(v->t0, now));
    v->lastPump = now; ++v->pumps;
    Events(v, false);
    PresentDue(v);
}

void PresenterMain(Video *v)
{
    while (v->remote) {
        if (PresentDue(v)) { while (v->remote) std::this_thread::sleep_for(std::chrono::milliseconds(5)); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

void Remote(bool on)
{
    Video *v = s_v; if (!v) return;
    if (getenv("H22_THREAD") && !atoi(getenv("H22_THREAD"))) { Pump(); return; }
    if (on && !v->remote) { Pump(); v->remote = true; v->presenter = std::thread(PresenterMain, v); }
    else if (!on && v->remote) { v->remote = false; if (v->presenter.joinable()) v->presenter.join(); v->lastPump = Clk::now(); Pump(); }
}

void Close(Video *v, bool wait)
{
    if (v->remote) { v->remote = false; if (v->presenter.joinable()) v->presenter.join(); }
    v->stop = true; if (v->dec.joinable()) v->dec.join();
#ifdef H6SND
    if (v->audioOn) {
        for (int i = 0; wait && i < 300 && !v->aud.ended && !s_skip && !(v->maxS > 0); ++i) { Events(v, true); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        h6a_dev_stop();
    }
#endif
    (void)wait;
    g9::Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.f, 0); g9::Present();   // pantalla en negro hasta el primer frame
    g9::Release(v->src); g9::Release(v->tex);
    h22c_close(v->clip);
}

bool Begin(const char *path)
{
    const bool force = getenv("H22_FORCE") && atoi(getenv("H22_FORCE"));
    if (s_v) return false;
    if (getenv("H21_MENU") && atoi(getenv("H21_MENU"))) { fprintf(stderr, "[H22] video de carga: primero el menu principal (se vera al relanzar con el mapa elegido)\n"); return false; }
    if (!g9::Active() || (!g9::SurfaceWidth() && !force)) { fprintf(stderr, "[H22] video de carga: sin ventana, se omite (H22_FORCE=1 lo fuerza)\n"); return false; }
    Video *v = new Video; char why[200] = "";
    v->clip = h22c_open(path, &v->in, why, sizeof why);
    if (!v->clip) { fprintf(stderr, "[H22] video de carga: %s (%s)\n", why, h22c_backend()); delete v; return false; }
    const H22Info &in = v->in;
    v->tex = g9::TexCreate(0, in.w, in.h, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, nullptr);
    v->src = v->tex ? g9::SurfFromTex(v->tex, 0, 0, in.w, in.h) : nullptr; v->bb = g9::SurfBackbuffer(false);
    if (!v->src || !v->bb) { if (v->tex) g9::Release(v->tex); h22c_close(v->clip); delete v; fprintf(stderr, "[H22] video de carga: sin textura de video en el backend\n"); return false; }
    const int W = g_h22W > 0 ? g_h22W : g9::SurfaceWidth(), H = g_h22H > 0 ? g_h22H : g9::SurfaceHeight();
    { const double s = (double)W / in.w < (double)H / in.h ? (double)W / in.w : (double)H / in.h; const int dw = (int)(in.w * s), dh = (int)(in.h * s);
      v->dr.left = (W - dw) / 2; v->dr.top = (H - dh) / 2; v->dr.right = v->dr.left + dw; v->dr.bottom = v->dr.top + dh; }
    v->sr = { 0, 0, in.w, in.h };
    v->shot = getenv("H22_SHOT"); v->maxS = getenv("H22_MAXS") ? atof(getenv("H22_MAXS")) : 0;
    fprintf(stderr, "[H22] video de carga %dx%d %.2f fps %.1f s, audio %d Hz %d canales, %s; salida %dx%d, durante la carga\n", in.w, in.h, in.fps, in.seconds, in.audioRate, in.audioChannels, h22c_backend(), W, H);
    v->aud.clip = v->clip; v->aud.ch = in.audioChannels; { const char *vo = getenv("H6A_VOL"); v->aud.vol = vo ? (float)atof(vo) : 1.f; if (!(v->aud.vol >= 0)) v->aud.vol = 0; if (v->aud.vol > 1) v->aud.vol = 1; }
    s_skip = false; s_loadDone = false;
#ifdef H0_MACWIN
    if (getenv("P4_WINDOW")) H10_MacInputInstall(MacEv);
#endif
    v->dec = std::thread(DecodeMain, v);
    // la primera imagen antes de arrancar el reloj: el video empieza entero aunque la carga tarde en llamar a Pump
    for (int i = 0; i < 500; ++i) { { std::lock_guard<std::mutex> lk(v->mq); if (!v->ready.empty() || v->decDone) break; } std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
#ifdef H6SND
    if (in.audioRate) v->audioOn = h6a_dev_start((unsigned)in.audioRate, 2, 480, getenv("H6A_DEVICE"), Render, &v->aud) != 0;
#endif
    v->t0 = v->lastPump = v->lastWin = Clk::now();
    s_v = v; s_active = true;
    PresentDue(v);
    g_h0LoadPump = Pump;
    static bool s_atexit; if (!s_atexit) { s_atexit = true; atexit([] { if (Video *w = s_v) { s_v = nullptr; g_h0LoadPump = nullptr; s_active = false; w->remote = false; if (w->presenter.joinable()) w->presenter.join(); w->stop = true; if (w->dec.joinable()) w->dec.join(); } }); }
    return true;
}

void End()
{
    Video *v = s_v; if (!v) return;
    g_h0LoadPump = nullptr;
    if (v->remote) Remote(false);
    v->lastPump -= std::chrono::milliseconds(5); Pump();   // cuenta tambien el ultimo tramo de la carga
    const double loadMs = Ms(v->t0, Clk::now());
    s_loadDone = true;   // carga hecha: desde ahora el jugador puede saltar el video
    for (;;) {   // carga hecha: el video sigue hasta su final o hasta saltarlo (como el original)
        if (PresentDue(v)) break;
        Events(v, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    v->skipped = s_skip;
    s_v = nullptr;
    Close(v, true);
    s_active = false;
    const double wall = Ms(v->t0, Clk::now()), dec = v->decoded ? v->decMs / v->decoded : 0;
    fprintf(stderr, "[H22] video de carga: %d imagenes mostradas (%d desde el hilo presentador), %d descartadas por llegar tarde, decodificacion %.2f ms por imagen; "
        "carga del mapa %.1f s, video %.1f s de %.1f (%d bombeos, mayor hueco sin presentar en la carga %.0f ms)%s\n",
        v->shown, v->remoteShown, v->late, dec, loadMs / 1000, wall / 1000, v->in.seconds, v->pumps, v->maxGap, v->skipped ? " (SALTADO por el jugador)" : "");
    delete v;
}

bool Play(const char *path)   // contrato de la OLA M0: bloqueante
{
    if (!Begin(path)) return false;
    End();
    return true;
}
bool BeginOrBlock(const char *path)   // H18_PlayLoadVideo: durante la carga solo con el juego completo (h5_game llama a End)
{
    if (!getenv("H5_GAME") || (getenv("H22_BLOCK") && atoi(getenv("H22_BLOCK")))) return Play(path);
    return Begin(path);
}
struct Reg { Reg() { g_h18PlayVideo = Play; g_h18BeginVideo = BeginOrBlock; g_h18VideoRemote = Remote; g_h18EndVideo = End; } } s_reg;   // punteros nullptr constantes: ya inicializados
} // namespace

void H22_VideoSkip() { if (s_loadDone) s_skip = true; }   // hasta que no acaba la carga (mapa y shaders) el video no se salta       // Android: toque o tecla durante el video
bool H22_VideoActive() { return s_active; }   // Android: el toque es para saltar el video, no para el juego
