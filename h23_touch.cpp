// OLA U2 (ampliacion): CONTROLES TACTILES EN PARTIDA (docs/DISENO_TACTIL.md), estilo de la APK oficial de BO1 Zombies para movil.
// Solo produce H5PlayerInput (los mismos botones/ejes que el mando y el teclado: h5_play.cpp los pasa al usercmd original); no toca
// fisica, dano ni el HUD original (h13). Lo usa la app Android (android_main.cpp, simbolos debiles) en horizontal sin capa DS, y el
// Mac con H23_PILOT (toques simulados y capturas) o H23=1.
//   Controles: stick flotante (mitad izquierda; a tope adelante = sprint de la capa de H4), mirar arrastrando por la zona libre de la
//   derecha (tambien con el dedo del disparo), DISPARO (mantener = fuego continuo), MIRA como INTERRUPTOR (toque on / toque off),
//   CUCHILLO, SALTO, POSTURA (toque agacha, mantener tumba), RECUADRO DE ARMA (tocar = recargar, deslizar = cambiar de arma; muestra
//   icono y municion), GRANADAS letal y tactica (icono del arma y cuantas quedan; mantener = cocinar, soltar = lanzar), USAR (solo
//   aparece con pista de uso del servidor: ps.cursorHint), PAUSA y PUNTOS arriba a la izquierda (mantener los puntos = marcador).
//   Customize HUD (Options > Controls; comando h23_customize): arrastrar cada icono, tamano con - / + o pellizco, restablecer y Auto-Aim.
//   Dos distribuciones: PLEGADO (pantalla exterior del Fold, lado corto < 1500 px) y DESPLEGADO; se guardan en
//   <P3_GAMEDATA>/players/bo1_touch.cfg (ruta en ejecucion; H23_CFG la cambia). Nada en git.
//   Auto-Aim (por defecto apagado): al ACTIVAR la mira, si hay un zombi visible a menos de 8 grados y 1200 u, la vista gira hacia su
//   pecho en 0,12 s (como el giro de un dedo: no toca balas, dano ni fisica). Es la ayuda de "snap" al apuntar de las consolas.
// Variables: H23=0 la quita (la app vuelve a sus botones de antes), H23_PROFILE=plegado|desplegado, H23_PILOT (ver PilotStep),
// H23_HIDEHUD=0 deja los puntos/municion del HUD original aunque esten tambien en los controles.
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <server_mp/server_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <bgame/bg_weapons.h>
#include <bgame/bg_weapons_def.h>
#include <bgame/bg_weapons_ammo.h>
#include <bgame/bg_mantle.h>
#include <bgame/bg_misc.h>
#include <clientscript/cscr_stringlist.h>
#include <qcommon/cm_trace.h>
#include <qcommon/cmd.h>
#include <universal/com_math.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_material.h>
#include <client_mp/cl_main_mp.h>
#include <ui/ui_shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <string>
#include <vector>
#include "h5_input.h"
#include "world_h4.h"

void H4_Shot(const char *tag) __attribute__((weak));
void H13_ShowScores(bool on) __attribute__((weak));
extern void (*g_h5pInputProvider)(H5PlayerInput *in, float dt);
extern void (*g_h5pDrawTouch)(int W, int H);
extern bool g_h5pPause;
extern bool g_h10MenuOpen;
extern void (*g_h10ToggleFn)();
extern bool (*g_h10GetSafeInsets)(int W, int H, float out[4]);
extern float g_h10Dpi;
void H10_Close();
bool H10_Open(const char *menu);

void H23_AimAssist();
unsigned H5P_ActionSlotWeapon(const playerState_s *ps, int slot);   // h5_play.cpp (OLA U2c)
namespace {
enum Ctl { C_STICK, C_FIRE, C_ADS, C_MELEE, C_JUMP, C_STANCE, C_WEAPON, C_FRAG, C_TACT, C_USE, C_PAUSE, C_SLOT1, C_SLOT2, C_SLOT3, C_SLOT4, C_COUNT };
const char *const kName[C_COUNT] = { "stick", "fire", "ads", "melee", "jump", "stance", "weapon", "frag", "tactical", "use", "pause", "slot1", "slot2", "slot3", "slot4" };
const char *const kLabel[C_COUNT] = { "MOVE", "FIRE", "AIM", "KNIFE", "JUMP", "STANCE", "WEAPON", "LETHAL", "TACTICAL", "USE", "PAUSE", "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT" };
struct Place { float fx, fy, s; };   // centro en fraccion de la zona segura, tamano (radio; recuadro: medio alto) en fraccion del lado corto
// PLEGADO: como las capturas de la APK movil (pausa+puntos arriba-izq, postura a la izquierda, arma+municion arriba-dcha, granada con
// numero a la derecha, mira circular abajo-dcha, cuchillo abajo-centro). DESPLEGADO: lo mismo en las esquinas y el centro libre para mirar.
const Place kDefFold[C_COUNT] = {
    { 0.14f, 0.70f, 0.130f }, { 0.915f, 0.72f, 0.105f }, { 0.770f, 0.86f, 0.068f }, { 0.50f, 0.90f, 0.070f }, { 0.965f, 0.95f, 0.050f },
    { 0.045f, 0.33f, 0.070f }, { 0.880f, 0.085f, 0.060f }, { 0.955f, 0.38f, 0.058f }, { 0.865f, 0.38f, 0.058f }, { 0.690f, 0.58f, 0.075f },
    { 0.030f, 0.075f, 0.045f },
    // OLA U2c: cruceta (actionslot 1-4) en cruz a la izquierda, entre la postura y el stick (el pulgar izquierdo, como en el mando);
    // solo salen los slots con accion (casco, minas...)
    { 0.160f, 0.260f, 0.042f }, { 0.160f, 0.460f, 0.042f }, { 0.110f, 0.360f, 0.042f }, { 0.210f, 0.360f, 0.042f } };
const Place kDefOpen[C_COUNT] = {
    { 0.13f, 0.76f, 0.105f }, { 0.920f, 0.80f, 0.085f }, { 0.800f, 0.89f, 0.055f }, { 0.785f, 0.73f, 0.050f }, { 0.965f, 0.62f, 0.045f },
    { 0.040f, 0.56f, 0.052f }, { 0.885f, 0.055f, 0.048f }, { 0.960f, 0.18f, 0.046f }, { 0.885f, 0.18f, 0.046f }, { 0.700f, 0.62f, 0.060f },
    { 0.025f, 0.045f, 0.036f },
    { 0.120f, 0.275f, 0.036f }, { 0.120f, 0.445f, 0.036f }, { 0.070f, 0.360f, 0.036f }, { 0.170f, 0.360f, 0.036f } };
Place s_lay[2][C_COUNT];
bool s_autoAim, s_loaded;
int s_winW = 1, s_winH = 1;
int Prof() {
    if (const char *e = getenv("H23_PROFILE")) return !strcmp(e, "plegado") || !strcmp(e, "0") ? 0 : 1;
    return (s_winW < s_winH ? s_winW : s_winH) < 1500 ? 0 : 1;
}
const char *ProfName(int p) { return p == 0 ? "plegado" : "desplegado"; }
std::string CfgPath()
{
    if (const char *e = getenv("H23_CFG")) return e;
    const char *b = getenv("P3_GAMEDATA"); return std::string(b ? b : ".") + "/players/bo1_touch.cfg";
}
void Reset(int p) { memcpy(s_lay[p], p == 0 ? kDefFold : kDefOpen, sizeof s_lay[p]); }
void Load()
{
    s_loaded = true; Reset(0); Reset(1); s_autoAim = false;
    FILE *f = fopen(CfgPath().c_str(), "r"); if (!f) { fprintf(stderr, "[H23] sin %s: distribucion por defecto\n", CfgPath().c_str()); return; }
    char line[256]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        char a[32], b[32]; float x, y, s; int v;
        if (sscanf(line, "autoaim %d", &v) == 1) s_autoAim = v != 0;
        else if (sscanf(line, "%31s %31s %f %f %f", a, b, &x, &y, &s) == 5) {
            const int p = !strcmp(a, "plegado") ? 0 : !strcmp(a, "desplegado") ? 1 : -1; if (p < 0) continue;
            for (int c = 0; c < C_COUNT; ++c) if (!strcmp(b, kName[c]) && x >= 0 && x <= 1 && y >= 0 && y <= 1 && s >= 0.02f && s <= 0.3f) { s_lay[p][c] = { x, y, s }; ++n; }
        }
    }
    fclose(f); fprintf(stderr, "[H23] %s: %d posiciones, auto-aim %s\n", CfgPath().c_str(), n, s_autoAim ? "si" : "no");
}
void Save()
{
    const std::string p = CfgPath(), tmp = p + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w"); if (!f) { fprintf(stderr, "[H23] no se puede escribir %s\n", tmp.c_str()); return; }
    fprintf(f, "# controles tactiles de BO1 (Customize HUD): perfil control x y tamano (fracciones de la zona segura y del lado corto)\nautoaim %d\n", s_autoAim ? 1 : 0);
    for (int pr = 0; pr < 2; ++pr) for (int c = 0; c < C_COUNT; ++c) fprintf(f, "%s %s %.4f %.4f %.4f\n", ProfName(pr), kName[c], s_lay[pr][c].fx, s_lay[pr][c].fy, s_lay[pr][c].s);
    fclose(f); rename(tmp.c_str(), p.c_str()); fprintf(stderr, "[H23] guardado %s\n", p.c_str());
}

// ---- geometria (pixeles de la ventana)
float s_safe[4];
void UpdateSafe()
{
    float o[4] = {}; if (!(g_h10GetSafeInsets && g_h10GetSafeInsets(s_winW, s_winH, o))) memset(o, 0, sizeof o);
    const float m = 0.015f * (s_winW < s_winH ? s_winW : s_winH);   // margen minimo como el HUD (H13_SAFE_MIN)
    for (int i = 0; i < 4; ++i) s_safe[i] = o[i] > m ? o[i] : m;
}
float Short() { return (float)(s_winW < s_winH ? s_winW : s_winH); }
void Center(int c, float *x, float *y, float *r)
{
    const Place &p = s_lay[Prof()][c];
    *x = s_safe[0] + p.fx * (s_winW - s_safe[0] - s_safe[2]); *y = s_safe[1] + p.fy * (s_winH - s_safe[1] - s_safe[3]); *r = p.s * Short();
}
float BoxW(float r) { return r * 2.9f; }   // recuadro de arma: medio ancho
bool Hit(int c, float x, float y, float slop)
{
    float cx, cy, r; Center(c, &cx, &cy, &r);
    if (c == C_WEAPON) return fabsf(x - cx) < BoxW(r) + slop && fabsf(y - cy) < r * 1.25f + slop;
    if (c == C_PAUSE) { if (hypotf(x - cx, y - cy) < r + slop) return true; return x > cx && x < cx + r * 7.5f && fabsf(y - cy) < r + slop * 0.5f; }   // la pausa y sus puntos
    return hypotf(x - cx, y - cy) < r + slop;
}
bool OnScore(float x, float y) { float cx, cy, r; Center(C_PAUSE, &cx, &cy, &r); return x > cx + r * 1.1f; }

// ---- estado del jugador para el HUD tactil
struct PState { bool ok = false; int score = 0, weapon = 0, clip = 0, stock = 0, frag = 0, fragN = 0, tact = 0, tactN = 0, stance = 0; bool hint = false, dead = false;
    int slotType[4] = {}, slotW[4] = {}, slotN[4] = {}; bool slotEmpty[4] = {}; };   // cruceta: tipo, arma, municion, equipo agotado
PState Read()
{
    PState s; gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return s;
    const playerState_s *ps = &pl->client->ps; s.ok = true;
    s.score = pl->client->sess.cs.score.score; s.weapon = ps->weapon; s.hint = ps->cursorHint != 0; s.dead = pl->health <= 0;
    if (s.weapon) { s.clip = BG_GetAmmoInClip(ps, s.weapon); s.stock = BG_GetAmmoNotInClip(ps, s.weapon); }
    s.stance = (ps->pm_flags & 1) ? 2 : (ps->pm_flags & 2) ? 1 : 0;   // PMF_PRONE 1, PMF_DUCKED 2
    for (int i = 0; i < 15; ++i) {
        const int w = ps->heldWeapons[i].weapon; if (!w) continue;
        const WeaponDef *d = BG_GetWeaponDef(w); if (!d) continue;
        const int n = BG_GetAmmoInClip(ps, w) + BG_GetAmmoNotInClip(ps, w);   // las granadas van en el cargador
        if (d->offhandClass == OFFHAND_CLASS_FRAG_GRENADE && !s.frag) { s.frag = w; s.fragN = n; }
        else if ((d->offhandClass == OFFHAND_CLASS_SMOKE_GRENADE || d->offhandClass == OFFHAND_CLASS_FLASH_GRENADE) && !s.tact) { s.tact = w; s.tactN = n; }
    }
    for (int k = 0; k < 4; ++k) {   // como CG_ActionSlotIsUsable / CG_DrawPlayerActionSlotDpad (cgame/cg_ammocounter.cpp)
        s.slotType[k] = ps->actionSlotType[k]; s.slotW[k] = (int)H5P_ActionSlotWeapon(ps, k);
        if (const int w = s.slotW[k]) { const WeaponDef *d = BG_GetWeaponDef(w); s.slotN[k] = BG_WeaponAmmo(ps, w);
            s.slotEmpty[k] = d && d->offhandSlot == OFFHAND_SLOT_EQUIPMENT && !BG_GetAmmoInClip(ps, w) && !BG_GetAmmoNotInClip(ps, w) && !d->unlimitedAmmo; }
    }
    return s;
}
PState s_ps;
bool Visible(int c, bool edit)
{
    if (edit) return true;
    if (c == C_USE) return s_ps.hint;
    if (c == C_FRAG) return s_ps.frag != 0;
    if (c == C_TACT) return s_ps.tact != 0;
    if (c >= C_SLOT1 && c <= C_SLOT4) { const int k = c - C_SLOT1; return s_ps.slotW[k] != 0 || s_ps.slotType[k] == ACTIONSLOTTYPE_NIGHTVISION; }   // solo con accion
    return true;
}

// ---- dedos
struct Ptr { bool on = false; int role = -1; float x0, y0, x, y, lx, ly; };   // role: control C_*, -2 mirar, -3 barra de edicion
Ptr s_p[10];
bool s_ads, s_edit, s_pauseEdge, s_reloadPulse, s_swapPulse;
float s_lookDX, s_lookDY, s_stickX, s_stickY;   // stick: centro flotante
int s_sel = C_FIRE, s_barHit = -1;
float s_pinch0, s_pinchS0;
float Slop() { const float dpi = g_h10Dpi > 0 ? g_h10Dpi : (getenv("H10_DPI") ? (float)atof(getenv("H10_DPI")) : 0); return dpi > 0 ? 0.08f * dpi : 0.02f * Short(); }

// barra de Customize HUD: - TAMANO + | AUTO-AIM | RESTABLECER | LISTO
enum { BAR_MINUS, BAR_PLUS, BAR_AIM, BAR_RESET, BAR_DONE, BAR_N };
const char *BarText(int i) { switch (i) { case BAR_MINUS: return "SIZE -"; case BAR_PLUS: return "SIZE +"; case BAR_AIM: return s_autoAim ? "AUTO-AIM: ON" : "AUTO-AIM: OFF"; case BAR_RESET: return "RESET"; default: return "DONE"; } }
void BarRect(int i, float r[4])
{
    const float h = 0.085f * Short(), gap = 0.008f * s_winW, x0 = 0.17f * s_winW, total = 0.58f * s_winW - 2 * gap;   // centro de arriba: entre los puntos y el recuadro de arma
    static const float wt[BAR_N] = { 1, 1, 1.7f, 1.3f, 1.1f }; float sum = 0; for (float w : wt) sum += w;
    float x = x0 + gap; for (int k = 0; k < i; ++k) x += total * wt[k] / sum;
    r[0] = x + gap * 0.3f; r[1] = s_safe[1] + 0.19f * (s_winH - s_safe[1] - s_safe[3]); r[2] = total * wt[i] / sum - gap * 0.6f; r[3] = h;
}
int BarAt(float x, float y) { for (int i = 0; i < BAR_N; ++i) { float r[4]; BarRect(i, r); if (x >= r[0] && x <= r[0] + r[2] && y >= r[1] && y <= r[1] + r[3]) return i; } return -1; }
void Resize(int c, float k) { Place &p = s_lay[Prof()][c]; p.s = fminf(0.3f, fmaxf(0.025f, p.s * k)); }
void MoveTo(int c, float x, float y)
{
    Place &p = s_lay[Prof()][c]; const float w = s_winW - s_safe[0] - s_safe[2], h = s_winH - s_safe[1] - s_safe[3];
    p.fx = fminf(1, fmaxf(0, (x - s_safe[0]) / w)); p.fy = fminf(1, fmaxf(0, (y - s_safe[1]) / h));
}
void EditEnd()
{
    s_edit = false; Save(); for (auto &p : s_p) p.on = false;
    fprintf(stderr, "[H23] Customize HUD: fin (%s guardado)\n", ProfName(Prof()));
    if (g_h10ToggleFn) g_h10ToggleFn(); else g_h5pPause = false;   // vuelve a la pausa original (donde estaba Options)
}
void EditBar(int b)
{
    switch (b) {
    case BAR_MINUS: Resize(s_sel, 1 / 1.12f); break; case BAR_PLUS: Resize(s_sel, 1.12f); break;
    case BAR_AIM: s_autoAim = !s_autoAim; break; case BAR_RESET: Reset(Prof()); break; case BAR_DONE: EditEnd(); return;
    }
    fprintf(stderr, "[H23] Customize HUD: %s (sel %s, tamano %.3f, auto-aim %d)\n", BarText(b), kName[s_sel], s_lay[Prof()][s_sel].s, s_autoAim);
}
int ActivePinchPartner(int id) { for (int i = 0; i < 10; ++i) if (i != id && s_p[i].on && s_p[i].role >= 0) return i; return -1; }

void TouchEdit(int id, int phase, float x, float y)
{
    Ptr &t = s_p[id];
    if (phase == 0) {
        t = Ptr(); t.on = true; t.x0 = t.x = t.lx = x; t.y0 = t.y = t.ly = y;
        const int b = BarAt(x, y); if (b >= 0) { t.role = -3; s_barHit = b; return; }
        const int other = ActivePinchPartner(id);
        if (other >= 0) { t.role = -4; s_pinch0 = hypotf(x - s_p[other].x, y - s_p[other].y); s_pinchS0 = s_lay[Prof()][s_sel].s; return; }   // segundo dedo: pellizco del seleccionado
        for (int c = C_COUNT - 1; c >= 0; --c) if (Hit(c, x, y, Slop() * 0.5f)) { t.role = c; s_sel = c; return; }
        t.role = -1; return;
    }
    if (!t.on) return;
    if (phase == 1) {
        if (t.role >= 0 && ActivePinchPartner(id) < 0) { float cx, cy, r; Center(t.role, &cx, &cy, &r); MoveTo(t.role, cx + x - t.lx, cy + y - t.ly); }
        if (t.role == -4) { const int o = ActivePinchPartner(id); if (o >= 0 && s_pinch0 > 1) { const float d = hypotf(x - s_p[o].x, y - s_p[o].y); s_lay[Prof()][s_sel].s = fminf(0.3f, fmaxf(0.025f, s_pinchS0 * d / s_pinch0)); } }
        t.x = t.lx = x; t.y = t.ly = y; return;
    }
    if (t.role == -3 && BarAt(x, y) == s_barHit) EditBar(s_barHit);
    t.on = false;
}

void TouchGame(int id, int phase, float x, float y)
{
    Ptr &t = s_p[id];
    if (phase == 0) {
        t = Ptr(); t.on = true; t.x0 = t.x = t.lx = x; t.y0 = t.y = t.ly = y;
        const float sl = Slop();
        static const int order[] = { C_PAUSE, C_WEAPON, C_SLOT1, C_SLOT2, C_SLOT3, C_SLOT4, C_USE, C_FRAG, C_TACT, C_ADS, C_MELEE, C_JUMP, C_STANCE, C_FIRE };
        for (int c : order) if (Visible(c, false) && Hit(c, x, y, sl)) {
            t.role = c;
            if (c == C_ADS) { s_ads = !s_ads; fprintf(stderr, "[H23] mira %s\n", s_ads ? "ACTIVADA" : "quitada"); if (s_ads) H23_AimAssist(); }
            if (c == C_PAUSE && !OnScore(x, y)) s_pauseEdge = true;
            return;
        }
        bool stickBusy = false; for (int i = 0; i < 10; ++i) if (i != id && s_p[i].on && s_p[i].role == C_STICK) stickBusy = true;
        if (x < s_winW * 0.45f && !stickBusy) {   // stick flotante: el centro es donde cae el dedo (sin salirse de la pantalla)
            float cx, cy, r; Center(C_STICK, &cx, &cy, &r);
            t.role = C_STICK; s_stickX = fminf(fmaxf(x, s_safe[0] + r), s_winW * 0.45f); s_stickY = fminf(fmaxf(y, s_safe[1] + r), s_winH - s_safe[3] - r);
            return;
        }
        t.role = -2; return;   // mirar
    }
    if (!t.on) return;
    if (phase == 1) {
        if (t.role == -2 || t.role == C_FIRE) { s_lookDX += x - t.lx; s_lookDY += y - t.ly; }   // el dedo del disparo tambien apunta (como la APK)
        t.x = t.lx = x; t.y = t.ly = y; return;
    }
    if (t.role == C_WEAPON) {   // soltar el recuadro: deslizar = cambiar de arma, tocar = recargar
        float cx, cy, r; Center(C_WEAPON, &cx, &cy, &r);
        if (fabsf(x - t.x0) > r * 0.8f) { s_swapPulse = true; if (s_ads) { s_ads = false; } fprintf(stderr, "[H23] recuadro: deslizar -> cambiar de arma\n"); }
        else { s_reloadPulse = true; fprintf(stderr, "[H23] recuadro: toque -> recargar\n"); }
    }
    t.on = false;
}

// ---- dibujo (lista 2D del renderer original; pixeles del motor)
Material *s_white, *s_matPause, *s_matKnife, *s_matStance[3], *s_matGren, *s_matUp;
Font_s *s_font, *s_big;
float s_dx = 1, s_dy = 1;   // ventana -> motor
Material *Mat(const char *n) { Material *m = Material_RegisterHandle((char *)n, 3); return m && !Material_IsDefault(m) ? m : nullptr; }
void Media()
{
    if (s_white) return;
    s_white = Material_RegisterHandle((char *)"white", 3); s_matPause = Mat("demo_pause"); s_matKnife = Mat("killiconmelee");
    s_matStance[0] = Mat("stance_stand"); s_matStance[1] = Mat("stance_crouch"); s_matStance[2] = Mat("stance_prone"); s_matGren = Mat("hud_grenadeicon"); s_matUp = Mat("theater_up_arrow");
    s_font = R_RegisterFont("fonts/normalFont", 3); s_big = R_RegisterFont("fonts/bigFont", 3); if (!s_big) s_big = s_font;
    fprintf(stderr, "[H23] iconos: pausa %d cuchillo %d postura %d/%d/%d granada %d salto %d\n", !!s_matPause, !!s_matKnife, !!s_matStance[0], !!s_matStance[1], !!s_matStance[2], !!s_matGren, !!s_matUp);
}
void Rect(float x, float y, float w, float h, const float *c) { R_AddCmdDrawStretchPic(x * s_dx, y * s_dy, w * s_dx, h * s_dy, 0, 0, 1, 1, c, s_white); }
void Tri(float ax, float ay, float bx, float by, float cx, float cy, const float *col)
{
    const float v[4][2] = { { ax * s_dx, ay * s_dy }, { bx * s_dx, by * s_dy }, { cx * s_dx, cy * s_dy }, { cx * s_dx, cy * s_dy } };
    R_AddCmdDrawQuadPic(v, col, s_white);
}
void Disc(float cx, float cy, float r, const float *col)
{
    const int n = 28; for (int i = 0; i < n; ++i) { const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        Tri(cx, cy, cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), col); }
}
void Ring(float cx, float cy, float r, float th, const float *col)
{
    const int n = 32; const float ri = r - th;
    for (int i = 0; i < n; ++i) { const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        const float v[4][2] = { { (cx + ri * cosf(a0)) * s_dx, (cy + ri * sinf(a0)) * s_dy }, { (cx + r * cosf(a0)) * s_dx, (cy + r * sinf(a0)) * s_dy },
                                { (cx + r * cosf(a1)) * s_dx, (cy + r * sinf(a1)) * s_dy }, { (cx + ri * cosf(a1)) * s_dx, (cy + ri * sinf(a1)) * s_dy } };
        R_AddCmdDrawQuadPic(v, col, s_white); }
}
void Icon(Material *m, float cx, float cy, float w, float h, const float *col) { if (m) R_AddCmdDrawStretchPic((cx - w / 2) * s_dx, (cy - h / 2) * s_dy, w * s_dx, h * s_dy, 0, 0, 1, 1, col, m); }
float TextW(Font_s *f, const char *t, float px) { const int th = R_TextHeight(f); return th > 0 ? R_TextWidth(t, 0, f) * px / th : 0; }
// texto de px de alto (ventana) con la base en y; align 0 izq, 1 centro, 2 dcha
void Text(Font_s *f, const char *t, float x, float y, float px, int align, const float *col)
{
    if (!f) return; const int th = R_TextHeight(f); if (th <= 0) return; const float sc = px / th, w = R_TextWidth(t, 0, f) * sc;
    const float x0 = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
    R_AddCmdDrawText((char *)t, 0x7fffffff, f, x0 * s_dx, y * s_dy, sc * s_dx, sc * s_dy, 0, col, 3);
}
const float kWhite[4] = { 1, 1, 1, 0.92f }, kBack[4] = { 0.05f, 0.05f, 0.06f, 0.38f }, kBackOn[4] = { 0.75f, 0.12f, 0.08f, 0.55f }, kEdge[4] = { 1, 1, 1, 0.45f };
void Button(int c, bool on, bool edit)
{
    float cx, cy, r; Center(c, &cx, &cy, &r);
    const float th = fmaxf(2, r * 0.07f);
    if (c == C_WEAPON) {
        const float bw = BoxW(r);
        Rect(cx - bw, cy - r, 2 * bw, 2 * r, on ? kBackOn : kBack);
        Rect(cx - bw, cy - r, 2 * bw, th, kEdge); Rect(cx - bw, cy + r - th, 2 * bw, th, kEdge);
        const WeaponDef *d = s_ps.weapon ? BG_GetWeaponDef(s_ps.weapon) : nullptr; Material *m = d ? d->hudIcon.get() : nullptr;
        if (m) { const float ratio = d->hudIconRatio == 2 ? 4.0f : d->hudIconRatio == 1 ? 2.0f : 1.0f; const float h = r * 1.5f, w = fminf(h * ratio, bw * 1.15f); Icon(m, cx - bw + w / 2 + r * 0.25f, cy, w, w / ratio, kWhite); }
        char b[32]; snprintf(b, sizeof b, "%d", s_ps.clip); Text(s_big, b, cx + bw - r * 0.2f, cy + r * 0.05f, r * 0.95f, 2, kWhite);
        snprintf(b, sizeof b, "%d", s_ps.stock); const float g[4] = { 0.85f, 0.85f, 0.85f, 0.9f }; Text(s_font, b, cx + bw - r * 0.2f, cy + r * 0.82f, r * 0.62f, 2, g);
        if (edit) Text(s_font, kLabel[c], cx, cy - r * 1.15f, r * 0.45f, 1, kWhite);
        return;
    }
    if (c == C_STICK) {
        const bool act = on; const float bx = act ? s_stickX : cx, by = act ? s_stickY : cy;
        const float a[4] = { 1, 1, 1, act ? 0.16f : 0.07f }; Disc(bx, by, r, a); Ring(bx, by, r, th, kEdge);
        float kx = bx, ky = by; for (auto &p : s_p) if (p.on && p.role == C_STICK) { float dx = p.x - bx, dy = p.y - by, m = hypotf(dx, dy); if (m > r) { dx *= r / m; dy *= r / m; } kx = bx + dx; ky = by + dy; }
        const float k[4] = { 1, 1, 1, act ? 0.55f : 0.28f }; Disc(kx, ky, r * 0.42f, k);
        if (edit) Text(s_font, kLabel[c], cx, cy + r + r * 0.35f, r * 0.3f, 1, kWhite);
        return;
    }
    if (c == C_PAUSE) {
        Disc(cx, cy, r, kBack); Ring(cx, cy, r, th, kEdge);
        if (s_matPause) Icon(s_matPause, cx, cy, r * 1.1f, r * 1.1f, kWhite); else { Rect(cx - r * 0.32f, cy - r * 0.42f, r * 0.22f, r * 0.84f, kWhite); Rect(cx + r * 0.1f, cy - r * 0.42f, r * 0.22f, r * 0.84f, kWhite); }
        char b[24]; snprintf(b, sizeof b, "%d", s_ps.score); Text(s_big, b, cx + r * 1.35f, cy + r * 0.55f, r * 1.45f, 0, kWhite);   // puntos (como la APK)
        if (edit) Text(s_font, kLabel[c], cx, cy + r * 1.9f, r * 0.55f, 1, kWhite);
        return;
    }
    const bool lit = on || (c == C_ADS && s_ads);
    Disc(cx, cy, r, lit ? kBackOn : kBack); Ring(cx, cy, r, th, kEdge);
    const float ic = r * 1.25f;
    switch (c) {
    case C_FIRE: { const float w[4] = { 1, 1, 1, 0.85f }; Ring(cx, cy, r * 0.62f, fmaxf(2, r * 0.06f), w); Rect(cx - r * 0.8f, cy - th / 2, r * 1.6f, th, w); Rect(cx - th / 2, cy - r * 0.8f, th, r * 1.6f, w); } break;
    case C_ADS: { const float w[4] = { 1, 1, 1, 0.85f }; Ring(cx, cy, r * 0.5f, fmaxf(2, r * 0.07f), w); Disc(cx, cy, r * 0.09f, w); } break;
    case C_MELEE: if (s_matKnife) Icon(s_matKnife, cx, cy, ic * 1.4f, ic * 0.7f, kWhite); else Text(s_font, "KNIFE", cx, cy + r * 0.2f, r * 0.5f, 1, kWhite); break;
    case C_JUMP: if (s_matUp) Icon(s_matUp, cx, cy, ic * 0.8f, ic * 0.8f, kWhite); else Tri(cx, cy - r * 0.5f, cx + r * 0.45f, cy + r * 0.3f, cx - r * 0.45f, cy + r * 0.3f, kWhite); break;
    case C_STANCE: { Material *m = s_matStance[s_ps.stance > 2 ? 0 : s_ps.stance]; if (m) Icon(m, cx, cy, ic * 1.3f, ic * 1.3f, kWhite); else Text(s_font, "STANCE", cx, cy + r * 0.2f, r * 0.45f, 1, kWhite); } break;
    case C_FRAG: case C_TACT: {
        const int w = c == C_FRAG ? s_ps.frag : s_ps.tact, n = c == C_FRAG ? s_ps.fragN : s_ps.tactN;
        const WeaponDef *d = w ? BG_GetWeaponDef(w) : nullptr; Material *m = d && d->hudIcon ? d->hudIcon.get() : (c == C_FRAG ? s_matGren : nullptr);
        if (m) Icon(m, cx, cy, ic, ic, kWhite); else Text(s_font, c == C_FRAG ? "FRAG" : "TACT", cx, cy + r * 0.2f, r * 0.45f, 1, kWhite);
        char b[8]; snprintf(b, sizeof b, "%d", edit && !w ? 0 : n); Text(s_big, b, cx - r * 0.95f, cy - r * 0.55f, r * 0.8f, 1, kWhite);
    } break;
    case C_USE: { const float g[4] = { 1, 0.85f, 0.3f, 0.95f }; Text(s_big, "USE", cx, cy + r * 0.25f, r * 0.75f, 1, g); } break;
    case C_SLOT1: case C_SLOT2: case C_SLOT3: case C_SLOT4: {
        // icono de la cruceta del original (WeaponVariantDef.dpadIcon: casco, hacker, claymore...) y municion si iStartAmmo > 1
        const int k = c - C_SLOT1, w = s_ps.slotW[k];
        const WeaponVariantDef *v = w ? BG_GetWeaponVariantDef(w) : nullptr; const WeaponDef *d = w ? BG_GetWeaponDef(w) : nullptr;
        Material *m = v && v->dpadIcon ? v->dpadIcon.get() : (d && d->hudIcon ? d->hudIcon.get() : nullptr);
        const float a[4] = { 1, 1, 1, s_ps.slotEmpty[k] ? 0.45f : 0.92f };
        if (m) { const float ratio = v && v->dpadIconRatio == WEAPON_ICON_RATIO_4TO1 ? 4.0f : v && v->dpadIconRatio == WEAPON_ICON_RATIO_2TO1 ? 2.0f : 1.0f;
            const float iw = ratio > 1 ? ic * 1.3f : ic, ih = iw / ratio; Icon(m, cx, cy, iw, ih, a); }
        else { static const char *const dir[4] = { "UP", "DOWN", "LEFT", "RIGHT" };   // en el editor, sin accion: la direccion dentro
            Text(s_font, s_ps.slotType[k] == ACTIONSLOTTYPE_NIGHTVISION ? "NV" : edit ? dir[k] : "?", cx, cy + r * 0.2f, r * (edit && k >= 2 ? 0.42f : 0.5f), 1, a); }
        // flecha de la direccion de la cruceta, fuera del circulo
        const float dx = k == 2 ? -1.f : k == 3 ? 1.f : 0.f, dy = k == 0 ? -1.f : k == 1 ? 1.f : 0.f, t = r * 1.18f, q = r * 0.2f;
        const float tx = cx + dx * t, ty = cy + dy * t, bx = cx + dx * (t - q * 1.2f), by = cy + dy * (t - q * 1.2f);
        Tri(tx, ty, bx - dy * q, by + dx * q, bx + dy * q, by - dx * q, kEdge);
        if (d && d->iStartAmmo > 1 && w) { char b[8]; snprintf(b, sizeof b, "%d", s_ps.slotN[k]); Text(s_font, b, cx + r * 0.95f, cy + r * 0.95f, r * 0.6f, 1, kWhite); }
    } break;
    }
    if (edit && !(c >= C_SLOT1 && c <= C_SLOT4)) Text(s_font, kLabel[c], cx, cy + r + r * 0.5f, fmaxf(r * 0.4f, 0.02f * Short()), 1, kWhite);   // la cruceta lleva su flecha
}
bool Held(int c) { for (auto &p : s_p) if (p.on && p.role == c) return true; return false; }

// ---- auto-aim: snap leve al activar la mira (no toca balas ni fisica: solo gira la vista como un dedo)
float s_assistYaw, s_assistPitch, s_assistT;
}   // namespace

void H23_AimAssist()
{
    if (!s_autoAim) return;
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return;
    float org[3], ang[3]; H4_GetCam(org, ang);
    float best = 8.0f, by = 0, bp = 0; int bn = -1;
    for (int e = 1; e < level.num_entities; ++e) {
        gentity_s *a = &g_entities[e]; if (!a->r.inuse || a->s.eType != ET_ACTOR || a->health <= 0) continue;
        const float tgt[3] = { a->r.currentOrigin[0], a->r.currentOrigin[1], a->r.currentOrigin[2] + 48.0f };   // pecho
        const float d[3] = { tgt[0] - org[0], tgt[1] - org[1], tgt[2] - org[2] }; const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist > 1200.0f || dist < 16.0f) continue;
        const float yaw = atan2f(d[1], d[0]) * 57.29578f, pitch = -atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) * 57.29578f;
        const float dy = AngleNormalize180(yaw - ang[1]), dp = AngleNormalize180(pitch - ang[0]); const float off = sqrtf(dy * dy + dp * dp);
        if (off >= best) continue;
        trace_t tr; const float z[3] = { 0, 0, 0 }; col_context_t ctx(1); CM_BoxTrace(&tr, org, tgt, z, z, 1, &ctx);
        if (tr.fraction < 0.98f) continue;   // tapado por el mundo
        best = off; by = dy; bp = dp; bn = e;
    }
    if (bn < 0) { fprintf(stderr, "[H23] auto-aim: sin objetivo en el cono\n"); return; }
    s_assistYaw = by; s_assistPitch = bp; s_assistT = 0.12f;
    fprintf(stderr, "[H23] auto-aim: zombi ent %d a %.1f grados (giro %.1f, %.1f)\n", bn, best, by, bp);
}

// ---------------------------------------------------------------- API (la app la enlaza con simbolos debiles)
void H23_SetWindow(int w, int h) { if (w > 0 && h > 0) { s_winW = w; s_winH = h; } }
bool H23_Editing() { return s_edit; }
void H23_EndEdit() { if (s_edit) EditEnd(); }   // Atras del sistema / B del mando en Customize HUD = LISTO
bool H23_HitPause(float x, float y) { UpdateSafe(); return Hit(C_PAUSE, x, y, Slop()) && !OnScore(x, y); }
// phase 0 abajo, 1 mover, 2 arriba, 3 cancelar (todos los dedos). Pixeles de la ventana.
void H23_Touch(int id, int phase, float x, float y)
{
    if (!s_loaded) Load();
    if (phase == 3) { for (auto &p : s_p) p.on = false; return; }
    if (id < 0 || id >= 10) return;
    UpdateSafe();
    if (s_edit) TouchEdit(id, phase, x, y); else TouchGame(id, phase, x, y);
}
enum { H23F_PAUSE = 1, H23F_SCORES = 2 };
// Los puntos y la municion ya estan en los controles (arriba, como la APK): el HUD original los esconde con SUS dvars (los que pone
// end_game: ammoCounterHide; los puntos no tienen dvar). La ronda (hudelem del GSC) y lo demas siguen. H23_HIDEHUD=0 los deja.
static void HideOriginal()
{
    static const bool on = !getenv("H23_HIDEHUD") || atoi(getenv("H23_HIDEHUD")); if (!on) return;
    { const dvar_s *d = Dvar_FindVar("ammoCounterHide"); if (!d || strcmp(Dvar_GetVariantString("ammoCounterHide"), "1")) Dvar_SetFromStringByName("ammoCounterHide", (char *)"1"); }
    // puntos: el menu competitivemodescores (ownerdraw 288 abajo-dcha) no mira ningun dvar propio: su item se saca de la pantalla
    static bool scoresDone; if (scoresDone || cgDC[0].menuCount <= 0) return;
    scoresDone = true;
    if (menuDef_t *m = Menus_FindByName(&cgDC[0], "competitivemodescores")) { for (int i = 0; i < m->itemCount; ++i) { m->items[i]->window.rect.x += 100000; m->items[i]->window.rectClient.x += 100000; }
        fprintf(stderr, "[H23] puntos del HUD original fuera (estan junto a la pausa)\n"); }
}
unsigned H23_Fill(H5PlayerInput *in, float dt)
{
    if (!s_loaded) Load();
    HideOriginal();
    UpdateSafe(); s_ps = Read();
    unsigned f = 0;
    if (s_edit) { s_lookDX = s_lookDY = 0; s_pauseEdge = s_reloadPulse = s_swapPulse = false; return 0; }
    const float lookScale = s_ads ? 0.55f : 1.0f;
    for (auto &p : s_p) if (p.on && p.role == C_STICK) {
        float cx, cy, r; Center(C_STICK, &cx, &cy, &r);
        float dx = (p.x - s_stickX) / r, dy = (p.y - s_stickY) / r; const float m = sqrtf(dx * dx + dy * dy); if (m > 1) { dx /= m; dy /= m; }
        in->moveRight = dx; in->moveFwd = -dy;   // a tope adelante = sprint automatico (capa de H4)
    }
    // sensibilidad: grados por pulgada de arrastre (50 como la capa anterior a 420 dpi)
    const float dpi = g_h10Dpi > 0 ? g_h10Dpi : (getenv("H10_DPI") ? (float)atof(getenv("H10_DPI")) : 0), k = dpi > 0 ? 50.0f / dpi : 0.12f;
    in->lookYaw += -s_lookDX * k * lookScale; in->lookPitch += s_lookDY * k * lookScale; s_lookDX = s_lookDY = 0;
    if (s_assistT > 0) { const float a = fminf(1.0f, dt / s_assistT); in->lookYaw += s_assistYaw * a; in->lookPitch += s_assistPitch * a; s_assistYaw -= s_assistYaw * a; s_assistPitch -= s_assistPitch * a; s_assistT -= dt; }
    if (Held(C_FIRE)) in->buttons |= H5IN_FIRE;
    if (s_ads) in->buttons |= H5IN_ADS;
    if (Held(C_MELEE)) in->buttons |= H5IN_MELEE;
    if (Held(C_JUMP)) in->buttons |= H5IN_JUMP;
    if (Held(C_STANCE)) in->buttons |= H5IN_STANCE;
    if (Held(C_USE) && s_ps.hint) in->buttons |= H5IN_USE;
    if (Held(C_FRAG)) in->buttons |= H5IN_FRAG;
    if (Held(C_TACT)) in->buttons |= H5IN_SMOKE;
    for (int k = 0; k < 4; ++k) if (Held(C_SLOT1 + k) && Visible(C_SLOT1 + k, false)) in->buttons |= H5IN_SLOT1 << k;   // OLA U2c: cruceta
    if (s_reloadPulse) { in->buttons |= H5IN_RELOAD; s_reloadPulse = false; }
    if (s_swapPulse) { in->buttons |= H5IN_NEXTWEAPON; s_swapPulse = false; }
    for (auto &p : s_p) if (p.on && p.role == C_PAUSE && OnScore(p.x0, p.y0)) f |= H23F_SCORES;
    if (s_pauseEdge) { s_pauseEdge = false; f |= H23F_PAUSE; }
    if (s_ps.dead && s_ads) s_ads = false;
    return f;
}
// W x H: pixeles del motor (la ventana entera en horizontal)
void H23_Draw(int W, int H)
{
    if (!s_loaded) Load();
    Media(); if (!s_white) return;
    s_dx = (float)W / s_winW; s_dy = (float)H / s_winH; UpdateSafe();
    if (g_h10MenuOpen && !s_edit) return;   // con la pausa original abierta no se dibujan los controles
    if (s_edit) {
        const float dim[4] = { 0, 0, 0, 0.35f }; Rect(0, 0, (float)s_winW, (float)s_winH, dim);
        const float y = s_safe[1] + 0.08f * (s_winH - s_safe[1] - s_safe[3]);
        char t[96]; snprintf(t, sizeof t, "CUSTOMIZE HUD  -  %s", Prof() == 0 ? "FOLDED" : "UNFOLDED");
        Text(s_big, t, s_winW * 0.46f, y, 0.05f * Short(), 1, kWhite);
        const float g[4] = { 0.85f, 0.85f, 0.85f, 0.9f };
        Text(s_font, "Drag an icon to move it. Select it and use SIZE - / + (or pinch) to resize.", s_winW * 0.46f, y + 0.05f * Short(), 0.028f * Short(), 1, g);
        for (int i = 0; i < BAR_N; ++i) { float r[4]; BarRect(i, r); const bool hot = (i == BAR_AIM && s_autoAim);
            const float bg[4] = { hot ? 0.55f : 0.12f, hot ? 0.10f : 0.12f, hot ? 0.07f : 0.13f, 0.85f }; Rect(r[0], r[1], r[2], r[3], bg);
            Rect(r[0], r[1], r[2], 2, kEdge); Rect(r[0], r[1] + r[3] - 2, r[2], 2, kEdge);
            Text(s_font, BarText(i), r[0] + r[2] / 2, r[1] + r[3] * 0.68f, r[3] * 0.5f, 1, kWhite); }
    }
    for (int c = 0; c < C_COUNT; ++c) if (Visible(c, s_edit)) Button(c, Held(c), s_edit);
    if (s_edit) { float cx, cy, r; Center(s_sel, &cx, &cy, &r); const float y[4] = { 1, 0.8f, 0.1f, 0.95f };
        if (s_sel == C_WEAPON) { const float bw = BoxW(r); Rect(cx - bw - 4, cy - r - 4, 2 * bw + 8, 3, y); Rect(cx - bw - 4, cy + r + 1, 2 * bw + 8, 3, y); Rect(cx - bw - 4, cy - r - 4, 3, 2 * r + 8, y); Rect(cx + bw + 1, cy - r - 4, 3, 2 * r + 8, y); }
        else Ring(cx, cy, r + 6, 4, y); }
}

// ---------------------------------------------------------------- Customize HUD: comando de la pestana CONTROLS (h10_settings.cpp)
static cmd_function_s s_cmdCust;
static void Cmd_H23Customize()
{
    if (!s_loaded) Load();
    H10_Close(); s_edit = true; g_h5pPause = true; s_ads = false; for (auto &p : s_p) p.on = false;
    fprintf(stderr, "[H23] Customize HUD: inicio (%s, %dx%d)\n", ProfName(Prof()), s_winW, s_winH);
}
void H23_RegisterCmd() { if (!Cmd_FindCommand("h23_customize")) Cmd_AddCommandInternal("h23_customize", Cmd_H23Customize, &s_cmdCust); }
bool g_h23Present = true;   // h10_settings.cpp: con esta TU, la pestana CONTROLS abre Customize HUD en vez de quitarse

// ---------------------------------------------------------------- Mac: piloto de toques (H23_PILOT="t accion;...", t en s de frames/60)
// acciones: down <id> <fx> <fy> | move <id> <fx> <fy> | up <id> | tap <control> | hold <control> <id> | swipe <control> | shot <tag> |
//           bar <0..4> (barra de Customize HUD: - + auto-aim restablecer listo) | cmd <orden de consola> | edit | log
namespace {
struct PEv { double t; std::string a; };
std::vector<PEv> s_pil; size_t s_pilI; int s_pilFrame; std::vector<std::pair<int, int>> s_pendUp;   // (frame, id)
int CtlByName(const char *n) { for (int c = 0; c < C_COUNT; ++c) if (!strcmp(n, kName[c])) return c; return -1; }
void PilotStep()
{
    const double t = ++s_pilFrame / 60.0;
    for (size_t i = 0; i < s_pendUp.size();) if (s_pendUp[i].first <= s_pilFrame) { const int id = s_pendUp[i].second; H23_Touch(id, 2, s_p[id].x, s_p[id].y); s_pendUp.erase(s_pendUp.begin() + i); } else ++i;
    while (s_pilI < s_pil.size() && s_pil[s_pilI].t <= t) {
        const std::string &a = s_pil[s_pilI++].a; char w[96] = ""; int id = 0; float x = 0, y = 0;
        fprintf(stderr, "[H23] piloto t=%.2f: %s\n", t, a.c_str());
        if (sscanf(a.c_str(), "down %d %f %f", &id, &x, &y) == 3) H23_Touch(id, 0, x * s_winW, y * s_winH);
        else if (sscanf(a.c_str(), "move %d %f %f", &id, &x, &y) == 3) H23_Touch(id, 1, x * s_winW, y * s_winH);
        else if (sscanf(a.c_str(), "up %d", &id) == 1) H23_Touch(id, 2, s_p[id].x, s_p[id].y);
        else if (sscanf(a.c_str(), "tap %63s", w) == 1 && CtlByName(w) >= 0) { UpdateSafe(); float cx, cy, r; Center(CtlByName(w), &cx, &cy, &r); H23_Touch(9, 0, cx, cy); s_pendUp.push_back({ s_pilFrame + 3, 9 }); }
        else if (sscanf(a.c_str(), "hold %63s %d", w, &id) == 2 && CtlByName(w) >= 0) { UpdateSafe(); float cx, cy, r; Center(CtlByName(w), &cx, &cy, &r); H23_Touch(id, 0, cx, cy); }
        else if (sscanf(a.c_str(), "swipe %63s", w) == 1 && CtlByName(w) >= 0) { UpdateSafe(); float cx, cy, r; Center(CtlByName(w), &cx, &cy, &r); H23_Touch(8, 0, cx, cy); H23_Touch(8, 1, cx - r * 2.5f, cy); s_pendUp.push_back({ s_pilFrame + 2, 8 }); }
        else if (sscanf(a.c_str(), "bar %d", &id) == 1 && id >= 0 && id < BAR_N) { UpdateSafe(); float r[4]; BarRect(id, r); H23_Touch(7, 0, r[0] + r[2] / 2, r[1] + r[3] / 2); s_pendUp.push_back({ s_pilFrame + 2, 7 }); }
        else if (sscanf(a.c_str(), "shot %63s", w) == 1) { if (H4_Shot) H4_Shot((std::string("_h23_") + w).c_str()); }
        else if (!strncmp(a.c_str(), "cmd ", 4)) { Cbuf_AddText(0, (a.substr(4) + "\n").c_str()); }
        else if (a == "edit") Cmd_H23Customize();
        else if (a == "log") { PState s = Read(); fprintf(stderr, "[H23] estado: puntos %d arma %d %d/%d letal %d x%d tactica %d x%d pista %d mira %d editar %d perfil %s\n", s.score, s.weapon, s.clip, s.stock, s.frag, s.fragN, s.tact, s.tactN, s.hint, s_ads, s_edit, ProfName(Prof()));
            fprintf(stderr, "[H23] estado: arma %s tactica %s | cruceta", s.weapon ? BG_WeaponName(s.weapon) : "-", s.tact ? BG_WeaponName(s.tact) : "-");   // OLA U2c
            for (int k = 0; k < 4; ++k) fprintf(stderr, " %d:%s(t%d,n%d)%s", k + 1, s.slotW[k] ? BG_WeaponName(s.slotW[k]) : "-", s.slotType[k], s.slotN[k], Visible(C_SLOT1 + k, false) ? "" : " oculto");
            { gentity_s *pl = &g_entities[0]; const int he = pl->client && s.hint ? pl->client->ps.cursorHintEntIndex : 1023; gentity_s *h = he > 0 && he < 1023 ? &g_entities[he] : nullptr;
              fprintf(stderr, " | pista ent %d %s %s", he, h && h->classname ? SL_ConvertToString(h->classname, SCRIPTINSTANCE_SERVER) : "-", h && h->targetname ? SL_ConvertToString(h->targetname, SCRIPTINSTANCE_SERVER) : "-"); }
            fprintf(stderr, "\n"); }
    }
}
void MacProvider(H5PlayerInput *in, float dt)
{
    memset(in, 0, sizeof *in);
    if (!s_pil.empty()) PilotStep();
    const unsigned f = H23_Fill(in, dt);
    if ((f & H23F_PAUSE) && g_h10ToggleFn) g_h10ToggleFn();
    static bool sc; if (((f & H23F_SCORES) != 0) != sc) { sc = !sc; if (H13_ShowScores) H13_ShowScores(sc); }
    if (g_h5pPause) in->buttons = 0;
}
}   // namespace
// h5_play.cpp (H5P_Run) lo llama antes de elegir la entrada: en el Mac, con H23_PILOT o H23=1 y sin proveedor de la app, los controles
// tactiles son la entrada (ventana = motor P4_W x P4_H). En la app solo registra el comando.
void H23_Install()
{
    const char *e = getenv("H23"); if (e && !atoi(e)) { g_h23Present = false; fprintf(stderr, "[H23] controles tactiles: desactivado (H23=0)\n"); return; }
    H23_RegisterCmd();
    if (g_h5pInputProvider) return;   // la app pone los suyos y nos llama
    const char *p = getenv("H23_PILOT"); if (!p && !(e && atoi(e))) return;
    const int w = getenv("P4_W") ? atoi(getenv("P4_W")) : 1280, h = getenv("P4_H") ? atoi(getenv("P4_H")) : 720; H23_SetWindow(w, h);
    for (const char *q = p; q && *q;) { const char *x = strchr(q, ';'); std::string s(q, x ? x - q : strlen(q)); double t; int n = 0; if (sscanf(s.c_str(), "%lf %n", &t, &n) == 1) s_pil.push_back({ t, s.substr(n) }); q = x ? x + 1 : nullptr; }
    g_h5pInputProvider = MacProvider; g_h5pDrawTouch = H23_Draw;
    fprintf(stderr, "[H23] controles tactiles en el Mac: ventana %dx%d, perfil %s, piloto de %zu pasos\n", w, h, ProfName(Prof()), s_pil.size());
}
