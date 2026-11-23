// h6_loading.cpp -- pantalla "Loading shaders" (OLA 6b). Se llama justo despues de g9::InitEx, antes de cargar zonas.
// Recorre pipelines.keys (claves con pk= de v9_shaders.spvc) y crea cada VkPipeline con progreso REAL en pantalla. En el primer arranque
// (sin VkPipelineCache valida) el driver compila todo aqui; en los siguientes las creaciones salen de la cache y la pantalla ni se ve.
// Dibujo sin shaders: rectangulos con g9::Clear (vkCmdClearAttachments) y una fuente de 5x7 en bits. H6_NOPREWARM=1 lo desactiva.
// OLA U2b: con el menu principal (H21_MENU=1) NO se precompila: el menu dibuja pocos pipelines y los crea al vuelo; la barra ya no sale
// al abrir la app. Al entrar en el mapa main2 arranca primero el video de carga (h22) y el precompilado corre DEBAJO de el, sin barra:
// entre pipeline y pipeline se bombea el video (g_h0LoadPump, como las lecturas de zona). La barra solo queda si no hay video (falta el
// .webm, sin ventana, H18_NOVIDEO). Tras precompilar (y al acabar la carga, al pausar, relanzar o salir de la app) se guarda la cache si
// hay pipelines nuevos (H6_SavePipelines): la app sale con _exit al relanzar y PipelineShutdown no llegaba a escribirla (medido en el Fold: cache del
// 17-11, 432 pipelines en 4,8 s "con cache caliente" en cada arranque).
#include "../vk/g9_api.h"
#include <chrono>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace g9 { int PrewarmCount(); bool PrewarmCold(); int PrewarmStep(int i); }
namespace v9 { bool PipelineSave(); }   // vk_state_ext.h (no se incluye: arrastra volk); escribe VkPipelineCache + pipelines.keys

namespace {
// 5x7: cada glifo son 7 filas de 5 bits (bit 4 = columna izquierda)
struct Glyph { char c; unsigned char r[7]; };
const Glyph kFont[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}}, {'G', {14, 17, 16, 23, 17, 17, 15}},
    {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},     {'L', {16, 16, 16, 16, 16, 16, 31}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'R', {30, 17, 17, 30, 20, 18, 17}}, {'S', {15, 16, 16, 14, 1, 1, 30}},   {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {30, 1, 1, 14, 1, 1, 30}},     {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'5', {31, 16, 30, 1, 1, 17, 14}},   {'6', {6, 8, 16, 30, 17, 17, 14}},   {'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 2, 12}},   {'/', {1, 1, 2, 4, 8, 16, 16}},      {'.', {0, 0, 0, 0, 0, 12, 12}},
};
void Rect(int x, int y, int w, int h, D3DCOLOR c) { D3DRECT r = {x, y, x + w, y + h}; g9::Clear(1, &r, D3DCLEAR_TARGET, c, 1.f, 0); }
void Text(const char *s, int x, int y, int px, D3DCOLOR c) {
    for (; *s; s++, x += 6 * px) {
        const Glyph *g = nullptr; for (const Glyph &k : kFont) if (k.c == *s) g = &k; if (!g) continue;
        for (int row = 0; row < 7; row++) for (int col = 0; col < 5; col++) if (g->r[row] & (16 >> col)) Rect(x + col * px, y + row * px, px, px, c);
    }
}
void Draw(int W, int H, int done, int total) {
    g9::Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000, 1.f, 0);
    int px = H / 120 > 2 ? H / 120 : 2;
    const char *title = "LOADING SHADERS"; int tw = (int)strlen(title) * 6 * px;
    Text(title, (W - tw) / 2, H / 2 - 14 * px, px, 0xFFD8D8D8);
    int bw = W / 2, bh = 2 * px, bx = (W - bw) / 2, by = H / 2;
    Rect(bx, by, bw, bh, 0xFF303030);
    if (total > 0) Rect(bx, by, (int)((long long)bw * done / total), bh, 0xFFB01010);
    char n[32]; snprintf(n, sizeof n, "%d/%d", done, total); int nw = (int)strlen(n) * 6 * (px / 2 > 1 ? px / 2 : 1);
    Text(n, (W - nw) / 2, by + 4 * px, px / 2 > 1 ? px / 2 : 1, 0xFF909090);
    g9::Present();
}
}  // namespace

extern void (*g_h0LoadPump)();   // rt2.cpp: video de carga en marcha (h22_video.cpp); nullptr si no hay
bool H22_VideoActive() __attribute__((weak));

// Guarda VkPipelineCache + pipelines.keys si hay pipelines nuevos desde el ultimo guardado (g9::Stats::n_pipeline, los vivos del backend).
// No hace nada si no hay ninguno nuevo. Solo desde el hilo del render (main2 / la app entre frames), nunca con el presentador de h22 activo.
void H6_SavePipelines(const char *why) {
    static uint32_t saved = 0;
    if (!g9::Active()) return;
    const uint32_t n = g9::GetStats().n_pipeline;
    if (n <= saved) return;
    auto t0 = std::chrono::steady_clock::now(); const bool ok = v9::PipelineSave();
    fprintf(stderr, "[H6] cache de pipelines guardada (%s): %u pipelines nuevos, %s en %.0f ms\n", why ? why : "-", n - saved, ok ? "ok" : "FALLO",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    if (ok) saved = n;
}

void H6_PrewarmPipelines(int W, int H) {
    if (getenv("H6_NOPREWARM")) return;
    if (getenv("H21_MENU") && atoi(getenv("H21_MENU")) && !getenv("H6_SHOW")) { fprintf(stderr, "[H6] prewarm: menu principal, sin precompilar (se hace bajo el video de carga del mapa)\n"); return; }
    const bool video = &H22_VideoActive && H22_VideoActive() && g_h0LoadPump;   // el video tapa la carga: sin barra encima
    int total = g9::PrewarmCount(); if (total <= 0) { fprintf(stderr, "[H6] prewarm: sin pipelines.keys (se compilaran al vuelo)\n"); return; }
    bool cold = g9::PrewarmCold(); bool force = getenv("H6_SHOW") != nullptr; const char *shot = getenv("H6_SHOT");   // H6_SHOW: pantalla siempre; H6_SHOT=png: captura a mitad
    auto t0 = std::chrono::steady_clock::now(), last = t0; bool shown = false; int ok = 0, skip = 0, fail = 0;
    for (int i = 0; i < total; i++) {
        int r = g9::PrewarmStep(i); if (r > 0) ok++; else if (r == 0) skip++; else fail++;
        auto now = std::chrono::steady_clock::now();
        // la pantalla solo aparece si el precompilado tarda (primer arranque o driver nuevo); con cache caliente no hay parpadeo
        if (video) { g_h0LoadPump(); continue; }   // presenta la imagen del video que toque (no hace nada si no han pasado 4 ms)
        if (std::chrono::duration<double, std::milli>(now - (shown ? last : t0)).count() > (shown ? 33.0 : force ? 0.0 : 150.0)) {
            Draw(W, H, i + 1, total); shown = true; last = now;
            if (shot && i >= total / 2) { int w, h; g9::ReadbackPNG(shot, &w, &h); shot = nullptr; }
        }
    }
    if (shown) Draw(W, H, total, total);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "[H6] prewarm: %d/%d pipelines en %.0f ms (sin pk/programa=%d fallos=%d, cache %s, pantalla %s)\n", ok, total, ms, skip, fail, cold ? "fria" : "caliente",
            video ? "no (bajo el video de carga)" : shown ? "si" : "no");
    if (!video) g9::Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000, 1.f, 0);
    H6_SavePipelines("tras precompilar");
}
