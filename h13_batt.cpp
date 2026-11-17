// OLA 17: bateria del movil en el HUD. Icono relleno segun el nivel + porcentaje + rayo al cargar; en rojo por debajo del 15 %.
// Esquina superior izquierda DENTRO de la zona segura (H13_SafeInsets: insets del sistema leidos en tiempo de ejecucion, nada fijo a un
// movil). Se dibuja con la lista 2D del renderer original (R_AddCmdDrawStretchPic / R_AddCmdDrawQuadPic / R_AddCmdDrawText, material
// "white" y fonts/normalFont, como el HUD), encima del HUD y de los menus de pausa/ajustes (h5_play.cpp: H5F_Draw2D).
//   Dato: la app Android escribe g_h13Batt (nivel | cargando << 8; -1 = sin dato) desde un hilo propio cada 30 s (android_main.cpp).
//   Este fichero no depende de Android: en el Mac nadie escribe g_h13Batt y no se dibuja nada, salvo con la simulacion.
// Variables: H13_BATT=0 lo quita. H13_BATT_SIM=pct,carga (p.ej. 57,0 o 12,1) simula el dato (pruebas en el Mac).
// Ajuste: h13_battery (fila BATTERY INDICATOR del panel GAME, h10_settings.cpp; 1 por defecto, se guarda en players/config.cfg).
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_material.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int g_h13Batt = -1;   // nivel 0-100 | (cargando << 8); -1 = sin dato. Lo escribe otro hilo: lectura atomica.
void H13_SafeInsets(int W, int H, float out[4]);   // h13_hud.cpp

static void Rect(float x, float y, float w, float h, const float *c, Material *m) { if (w > 0 && h > 0) R_AddCmdDrawStretchPic(x, y, w, h, 0, 0, 1, 1, c, m); }
// rayo: dos cuadrilateros convexos (abanico 0-1-2 / 0-2-3 en RB_DrawQuadPicCmd) en una caja bw x bh;
static void Bolt(float x, float y, float bw, float bh, const float *c, Material *m)
{
    // mismo sentido de giro que los quads del original (grenade_vertices de cg_draw_indicators: arriba-izq, arriba-der, abajo-der,
    // abajo-izq con y hacia abajo); al reves el backend los descarta
    static const float k[2][4][2] = { { { 0.65f, 0 }, { 0.55f, 0.42f }, { 0.45f, 0.58f }, { 0.10f, 0.58f } },
                                      { { 0.35f, 1 }, { 0.45f, 0.58f }, { 0.55f, 0.42f }, { 0.90f, 0.42f } } };
    for (int q = 0; q < 2; ++q) {
        float v[4][2]; for (int i = 0; i < 4; ++i) { v[i][0] = x + k[q][i][0] * bw; v[i][1] = y + k[q][i][1] * bh; }
        R_AddCmdDrawQuadPic(v, c, m);
    }
}

static void DrawBattery(int W, int H)
{
    static const bool on = !(getenv("H13_BATT") && !atoi(getenv("H13_BATT")));
    if (!on) return;
    static int sim = -2;
    if (sim == -2) { sim = -1; if (const char *e = getenv("H13_BATT_SIM")) { int p = -1, c = 0; if (sscanf(e, "%d,%d", &p, &c) >= 1 && p >= 0) sim = (p > 100 ? 100 : p) | (c ? 256 : 0); } }
    const int v = sim >= 0 ? sim : __atomic_load_n(&g_h13Batt, __ATOMIC_RELAXED);
    if (v < 0) return;
    static const dvar_s *opt; if (!opt) opt = Dvar_FindVar("h13_battery");   // lo registra la OLA 10 (h10_settings.cpp); sin ella, visible
    if (opt && !opt->current.integer) return;
    static Font_s *font; static Material *white;
    if (!white) { font = R_RegisterFont("fonts/normalFont", 3); white = Material_RegisterHandle((char *)"white", 3); if (!white) return; }
    const int pct = v & 255; const bool chg = (v >> 8) & 1;
    static int last = -1; if (v != last) { last = v; fprintf(stderr, "[H13B] bateria %d%%%s%s\n", pct, chg ? " cargando" : "", sim >= 0 ? " (H13_BATT_SIM)" : ""); }

    float s[4]; H13_SafeInsets(W, H, s);
    // tamano por el lado corto (igual en horizontal y vertical): icono de ~2,8 % de alto, borde y separaciones en pixeles enteros
    const float sh = (float)(W < H ? W : H), ih = floorf(sh * 0.028f + 0.5f) < 12 ? 12 : floorf(sh * 0.028f + 0.5f);
    const float t = fmaxf(1, floorf(ih * 0.09f + 0.5f)), iw = floorf(ih * 1.9f + 0.5f), nubW = fmaxf(t, floorf(ih * 0.12f + 0.5f)), nubH = floorf(ih * 0.42f + 0.5f);
    const float sd = fmaxf(1, floorf(ih * 0.07f + 0.5f));   // desplazamiento de la sombra
    const float x0 = floorf(s[0] + 0.5f), y0 = floorf(s[1] + 0.5f);
    const bool low = pct < 15;
    const float white4[4] = { 1, 1, 1, 0.92f }, red4[4] = { 0.92f, 0.16f, 0.12f, 1 }, shadow[4] = { 0, 0, 0, 0.55f };
    const float *col = low ? red4 : white4;
    const float fillW = pct > 0 ? fmaxf(1, floorf((iw - 4 * t) * pct / 100.0f + 0.5f)) : 0;
    for (int pass = 0; pass < 2; ++pass) {   // 0 sombra, 1 color
        const float o = pass ? 0 : sd, x = x0 + o, y = y0 + o; const float *c = pass ? col : shadow;
        Rect(x, y, iw, t, c, white); Rect(x, y + ih - t, iw, t, c, white);                 // borde arriba / abajo
        Rect(x, y + t, t, ih - 2 * t, c, white); Rect(x + iw - t, y + t, t, ih - 2 * t, c, white);   // izquierda / derecha
        Rect(x + iw, y + floorf((ih - nubH) * 0.5f), nubW, nubH, c, white);                // polo
        Rect(x + 2 * t, y + 2 * t, fillW, ih - 4 * t, c, white);                            // relleno
    }
    float tx = x0 + iw + nubW + floorf(ih * 0.35f + 0.5f);
    if (chg) {
        const float bw = floorf(ih * 0.62f + 0.5f);
        Bolt(tx + sd, y0 + sd, bw, ih, shadow, white); Bolt(tx, y0, bw, ih, col, white);
        tx += bw + floorf(ih * 0.15f + 0.5f);
    }
    if (font) {   // porcentaje: alto de mayuscula ~ alto del icono; linea base en el borde inferior del icono (estilo 3 = con sombra, como el HUD)
        char b[8]; snprintf(b, sizeof b, "%d%%", pct);
        const float sc = ih * 1.3f / (float)(font->pixelHeight > 0 ? font->pixelHeight : 16);
        R_AddCmdDrawText(b, 0x7fffffff, font, tx, y0 + ih, sc, sc, 0, col, 3);
    }
}
// puntero de datos (no referencia debil a funcion: genstubs le pondria un stub que aborta); sin H13 en el build queda a 0
void (*g_h13DrawBattery)(int W, int H) = DrawBattery;
