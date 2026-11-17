// OLA 15E: partida guionizada para provocar las pistas y avisos del HUD de zombies que no salian en las partidas de prueba (compra de
// arma de pared, puerta, caja, interruptor de la luz, maquina de ventaja, potenciadores, ultima resistencia). Solo prueba: no cambia el
// juego. El jugador se coloca dentro de cada trigger_use del mapa mirando a su objetivo y mantiene USAR por la misma entrada que la app
// (g_h5pInputProvider); las compras, puntos, ventajas y potenciadores son los GSC ORIGINALES. Lo unico que se llama desde aqui son
// funciones del GSC original (specific_powerup_drop de _zombiemode_powerups), con los handles pedidos durante la carga
// (g_h0ScrLoadHook de g_scr_main_mp.cpp), y se da dinero escribiendo el campo de cliente "score" (el mismo que lee el GSC).
// Se enlaza con EXTRA (build.sh); h12_barrier.cpp llama a H15E_Frame (debil) cada frame.
//   H15E_HUD=1        -> guion completo (registro [H15E] y capturas _h15e_<paso> si hay P4_OUT)
//   H15E_STEPS=WDBEPUL -> pasos y orden: W pared, D puerta, B caja, E luz, P ventaja (Juggernog), U potenciadores, L ultima resistencia
//   H15E_MONEY=30000  -> puntos al empezar
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <server_mp/server_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <game_mp/g_misc_mp.h>
#include <game_mp/g_scr_main_mp.h>
#include <clientscript/cscr_stringlist.h>
#include <clientscript/cscr_vm.h>
#include <clientscript/cscr_main.h>
#include <qcommon/cm_trace.h>
#include <universal/com_math.h>
#include "h5_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>

void H4_Shot(const char *tag) __attribute__((weak));
void H13_ShowScores(bool on) __attribute__((weak));
void __cdecl CM_BoxTrace(trace_t *results, const float *start, const float *end, const float *mins, const float *maxs, int brushmask, col_context_t *context);
unsigned int __cdecl Scr_ExecEntThread(gentity_s *ent, int handle, unsigned int paramcount);
extern void (*g_h0ScrLoadHook)(scriptInstance_t inst);
int __cdecl GScr_LoadScriptAndLabel(scriptInstance_t inst, const char *filename, const char *label, int bEnforceExists);

static const char *S(unsigned short s) { return s ? SL_ConvertToString(s, SCRIPTINSTANCE_SERVER) : ""; }
static int g_hPowerupDrop, g_hGivePerk;
static void H15E_LoadHook(scriptInstance_t inst)
{
    if (inst != SCRIPTINSTANCE_SERVER || !getenv("H15E_HUD")) return;
    g_hPowerupDrop = GScr_LoadScriptAndLabel(inst, "maps/_zombiemode_powerups", "specific_powerup_drop", 0);
    g_hGivePerk = GScr_LoadScriptAndLabel(inst, "maps/_zombiemode_perks", "give_perk", 0);
    fprintf(stderr, "[H15E] handles GSC: specific_powerup_drop=%d give_perk=%d\n", g_hPowerupDrop, g_hGivePerk);
}
static struct H15EInit { H15EInit() { g_h0ScrLoadHook = H15E_LoadHook; } } s_h15eInit;

struct Step { char id; const char *tn; const char *what; };
static std::string g_steps; static size_t g_si; static int g_phase, g_t0, g_tPhase; static bool g_use, g_aimOn; static float g_aim[3];
static void (*g_prevInput)(H5PlayerInput *, float);

static void Input(H5PlayerInput *in, float)
{
    memset(in, 0, sizeof *in);
    if (g_use) in->buttons |= H5IN_USE;
    if (g_aimOn && g_entities[0].client) {
        const float *va = g_entities[0].client->ps.viewangles;
        float dy = g_aim[1] - va[1]; while (dy > 180) dy -= 360; while (dy < -180) dy += 360;
        float dp = g_aim[0] - va[0]; while (dp > 180) dp -= 360; while (dp < -180) dp += 360;
        in->lookYaw = dy; in->lookPitch = dp;
    }
}
static void Shot(const char *id)
{
    gentity_s *pl = &g_entities[0]; const playerState_s *ps = pl->client ? &pl->client->ps : nullptr;
    fprintf(stderr, "[H15E] captura %s t=%d puntos %d salud %d cursorHint %d cadena %d arma %d\n", id, level.time, pl->client ? pl->client->sess.cs.score.score : -1,
        pl->health, ps ? ps->cursorHint : -1, ps ? ps->cursorHintString : -1, ps ? (int)ps->weapon : -1);
    if (H4_Shot) { char t[48]; snprintf(t, sizeof t, "_h15e_%s", id); H4_Shot(t); }
}
static int FindTrig(const char *tn, const char *noteworthy = nullptr)
{
    const gentity_s *pl = &g_entities[0]; int best = -1; float bd = 1e30f;
    for (int i = 0; i < level.num_entities; ++i) { const gentity_s *e = &g_entities[i]; if (!e->r.inuse || !e->targetname || strcmp(S(e->targetname), tn)) continue;
        if (noteworthy && strcmp(S(e->script_noteworthy), noteworthy)) continue;
        float c[3]; for (int k = 0; k < 3; ++k) c[k] = 0.5f * (e->r.absmin[k] + e->r.absmax[k]);
        const float d = (c[0] - pl->r.currentOrigin[0]) * (c[0] - pl->r.currentOrigin[0]) + (c[1] - pl->r.currentOrigin[1]) * (c[1] - pl->r.currentOrigin[1]) + 4 * (c[2] - pl->r.currentOrigin[2]) * (c[2] - pl->r.currentOrigin[2]);
        if (d < bd) { bd = d; best = i; } }
    return best;
}
// coloca al jugador dentro del trigger (en el suelo bajo su centro) mirando al objetivo (su .target si existe, si no el centro)
static bool PlaceAt(int ti)
{
    gentity_s *pl = &g_entities[0], *t = &g_entities[ti]; if (!pl->client) return false;
    float c[3]; for (int k = 0; k < 3; ++k) c[k] = 0.5f * (t->r.absmin[k] + t->r.absmax[k]);
    float look[3] = { c[0], c[1], c[2] };
    if (t->target) for (int i = 0; i < level.num_entities; ++i) { const gentity_s *e = &g_entities[i]; if (e->r.inuse && e->targetname == t->target && i != ti) { memcpy(look, e->r.currentOrigin, 12); break; } }
    // el jugador se pone a ~36 u del objetivo, en la direccion (de 16 probadas) que no atraviesa paredes y cae dentro del trigger
    // (ampliado 12 u) con el hueco libre para la capsula; si ninguna vale, en el centro del trigger
    const float z[3] = { 0, 0, 0 }; float o[3] = { c[0], c[1], c[2] }; float bestScore = -1e30f;
    for (int i = 0; i < 16; ++i) { const float an = i * 0.39269908f, dx = cosf(an), dy = sinf(an);
        for (float dist : { 36.0f, 28.0f, 48.0f }) {
            const float p[3] = { look[0] + dx * dist, look[1] + dy * dist, c[2] };
            if (p[0] < t->r.absmin[0] - 12 || p[0] > t->r.absmax[0] + 12 || p[1] < t->r.absmin[1] - 12 || p[1] > t->r.absmax[1] + 12) continue;
            trace_t tw; memset(&tw, 0, sizeof tw); const float a0[3] = { look[0] + dx * 4, look[1] + dy * 4, c[2] }; const float hb0[3] = { -15, -15, 0 }, hb1[3] = { 15, 15, 0 };
            col_context_t cw(1); CM_BoxTrace(&tw, a0, p, z, z, 1, &cw); if (tw.fraction < 1.0f || tw.startsolid) continue;
            trace_t tb; memset(&tb, 0, sizeof tb); col_context_t cb(1); CM_BoxTrace(&tb, p, p, hb0, hb1, 1, &cb); if (tb.startsolid || tb.allsolid) continue;
            const float sc = -fabsf(dist - 36.0f) - 0.01f * ((p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1]));
            if (sc > bestScore) { bestScore = sc; o[0] = p[0]; o[1] = p[1]; } } }
    trace_t tr; memset(&tr, 0, sizeof tr); float a[3] = { o[0], o[1], c[2] + 8 }, b[3] = { o[0], o[1], c[2] - 400 };
    col_context_t ctx(1); CM_BoxTrace(&tr, a, b, z, z, 1, &ctx);
    o[2] = a[2] + (b[2] - a[2]) * tr.fraction + 1.0f; if (o[2] < t->r.absmin[2] - 60) o[2] = t->r.absmin[2];
    const float eye[3] = { o[0], o[1], o[2] + 60 }, dd[3] = { look[0] - eye[0], look[1] - eye[1], look[2] - eye[2] };
    float ang[3] = { -atan2f(dd[2], sqrtf(dd[0] * dd[0] + dd[1] * dd[1])) * 57.29578f, atan2f(dd[1], dd[0]) * 57.29578f, 0 };
    TeleportPlayer(pl, o, ang); memcpy(g_aim, ang, 12); g_aimOn = true;
    fprintf(stderr, "[H15E] t=%d jugador en el trigger %d '%s' (%s) en (%.0f %.0f %.0f) mirando (%.0f %.0f %.0f)\n", level.time, ti, S(t->targetname), S(t->script_noteworthy), o[0], o[1], o[2], look[0], look[1], look[2]);
    return true;
}
static void Powerup(const char *name)
{
    if (!g_hPowerupDrop) { fprintf(stderr, "[H15E] sin handle de specific_powerup_drop\n"); return; }
    gentity_s *pl = &g_entities[0]; float spot[3] = { pl->r.currentOrigin[0], pl->r.currentOrigin[1], pl->r.currentOrigin[2] + 10 };
    Scr_AddVector(spot, SCRIPTINSTANCE_SERVER); Scr_AddString((char *)name, SCRIPTINSTANCE_SERVER);
    const unsigned th = Scr_ExecThread(SCRIPTINSTANCE_SERVER, g_hPowerupDrop, 2); Scr_FreeThread(th, SCRIPTINSTANCE_SERVER);
    fprintf(stderr, "[H15E] t=%d specific_powerup_drop(\"%s\") a los pies del jugador\n", level.time, name);
}

void H15E_Frame()
{
    static int on = -1; if (on < 0) { on = getenv("H15E_HUD") ? 1 : 0; g_steps = getenv("H15E_STEPS") ? getenv("H15E_STEPS") : "WDBEPUL"; }
    if (!on) return;
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return;
    const int t = level.time;
    if (!g_t0) { if (t < 6000) return; g_t0 = t; g_prevInput = g_h5pInputProvider; g_h5pInputProvider = Input;
        pl->client->sess.cs.score.score = getenv("H15E_MONEY") ? atoi(getenv("H15E_MONEY")) : 30000;
        fprintf(stderr, "[H15E] guion %s; puntos %d\n", g_steps.c_str(), pl->client->sess.cs.score.score); Shot("0_inicio"); g_tPhase = t; }
    if (g_si >= g_steps.size()) return;
    const char st = g_steps[g_si]; const int dt = t - g_tPhase;
    auto next = [&]() { g_use = false; g_aimOn = false; ++g_si; g_phase = 0; g_tPhase = t; };
    auto place = [&](const char *tn, const char *nw) { const int ti = FindTrig(tn, nw); if (ti < 0) { fprintf(stderr, "[H15E] no hay trigger '%s'\n", tn); next(); return false; } PlaceAt(ti); g_phase = 1; g_tPhase = t; return true; };
    char id[32];
    switch (st) {
    case 'W': case 'D': case 'B': case 'E': case 'P': {
        const char *tn = st == 'W' ? "weapon_upgrade" : st == 'D' ? "zombie_door" : st == 'B' ? "treasure_chest_use" : st == 'E' ? "use_elec_switch" : "zombie_vending";
        const char *nw = st == 'P' ? "specialty_armorvest" : nullptr;
        if (g_phase == 0) { place(tn, nw); break; }
        if (g_phase == 1 && dt >= 900) { snprintf(id, sizeof id, "%c1_pista", st); Shot(id); g_use = true; g_phase = 2; g_tPhase = t; break; }
        if (g_phase == 2 && dt >= 700) { g_use = false; g_phase = 3; g_tPhase = t; break; }
        // caja: el arma da vueltas ~4 s; despues la pista cambia a "cambiar arma" y se recoge con USAR
        if (g_phase == 3 && dt >= (st == 'B' ? 4500 : st == 'P' ? 3500 : 1500)) { snprintf(id, sizeof id, "%c2_despues", st); Shot(id);
            if (st == 'B') { g_use = true; g_phase = 4; g_tPhase = t; } else next(); break; }
        if (g_phase == 4 && dt >= 600) { g_use = false; g_phase = 5; g_tPhase = t; break; }
        if (g_phase == 5 && dt >= 1500) { Shot("B3_recogida"); next(); }
        break; }
    case 'U': {
        static const char *pu[3] = { "full_ammo", "double_points", "insta_kill" };
        const int k = g_phase / 3, sub = g_phase % 3; if (k >= 3) { next(); break; }
        if (sub == 0) { Powerup(pu[k]); g_phase++; g_tPhase = t; break; }
        if (sub == 1 && dt >= 600) { snprintf(id, sizeof id, "U%d_%s", k, pu[k]); Shot(id); g_phase++; g_tPhase = t; break; }
        if (sub == 2 && dt >= 2500) { snprintf(id, sizeof id, "U%db_%s", k, pu[k]); Shot(id); g_phase++; g_tPhase = t; }
        break; }
    case 'L': {
        // ultima resistencia: con Quick Revive en solitario el original entra en last stand con barra de auto-revivir. Se compra en su
        // maquina (paso P con specialty_quickrevive) o se baja la salud a 1 y se espera al golpe de un zombi
        if (g_phase == 0) { const int ti = FindTrig("zombie_vending", "specialty_quickrevive"); if (ti >= 0) { PlaceAt(ti); g_phase = 1; g_tPhase = t; } else { g_phase = 3; g_tPhase = t; } break; }
        if (g_phase == 1 && dt >= 900) { Shot("L0_pista_qr"); g_use = true; g_phase = 2; g_tPhase = t; break; }
        if (g_phase == 2 && dt >= 700) { g_use = false; g_phase = 3; g_tPhase = t; break; }
        if (g_phase == 3 && dt >= 3500) { g_aimOn = false; g_phase = 4; g_tPhase = t; Shot("L1_con_qr"); break; }
        if (g_phase == 4) { if (pl->health > 1 && pl->client->ps.pm_type < 2) pl->health = 1;
            const bool down = (pl->client->ps.pm_flags & 0x1) != 0 || pl->client->ps.pm_type >= 2 || pl->s.lerp.eFlags & 0x400000;
            static int seen = 0; if (down && !seen) { seen = t; fprintf(stderr, "[H15E] t=%d jugador abatido (pm_type %d pm_flags %x)\n", t, pl->client->ps.pm_type, pl->client->ps.pm_flags); }
            if (seen && t - seen >= 1500 && g_phase == 4) { Shot("L2_abatido"); g_phase = 5; g_tPhase = t; }
            if (dt > 90000) { fprintf(stderr, "[H15E] ningun zombi abatio al jugador en 90 s\n"); next(); } break; }
        if (g_phase == 5 && dt >= 3000) { Shot("L3_barra"); g_phase = 6; g_tPhase = t; break; }
        if (g_phase == 6 && dt >= 6000) { Shot("L4_revivido"); next(); }
        break; }
    case 'S': {   // marcador: lo mismo que mantener Tab / boton MARCADOR / Back del mando (H13_ShowScores)
        if (!H13_ShowScores) { next(); break; }
        if (g_phase == 0) { H13_ShowScores(true); g_phase = 1; g_tPhase = t; break; }
        if (g_phase == 1 && dt >= 800) { Shot("S1_marcador"); H13_ShowScores(false); g_phase = 2; g_tPhase = t; break; }
        if (g_phase == 2 && dt >= 600) { Shot("S2_cerrado"); next(); }
        break; }
    case 'G': {   // fin de partida: sin Quick Revive, en solitario caer = end_game (GAME OVER, rondas, intermision con el marcador)
        static int seen = 0, k = 0;
        if (g_phase == 0) { g_phase = 1; g_tPhase = t; fprintf(stderr, "[H15E] t=%d fin de partida: salud a 1 y se espera al golpe\n", t); break; }
        if (pl->health > 1 && pl->client->ps.pm_type < 2 && !seen) pl->health = 1;
        const bool down = (pl->client->ps.pm_flags & 0x1) != 0 || pl->client->ps.pm_type >= 2;
        if (down && !seen) { seen = t; fprintf(stderr, "[H15E] t=%d abatido sin Quick Revive (pm_type %d)\n", t, pl->client->ps.pm_type); }
        static const int at[] = { 1500, 4000, 7000, 10000, 14000, 20000, 28000, 36000 };
        if (seen && k < 8 && t - seen >= at[k]) { snprintf(id, sizeof id, "G%d_fin_%02ds", k, at[k] / 1000); Shot(id);
            fprintf(stderr, "[H15E] t=%d %s pm_type %d\n", t, id, pl->client->ps.pm_type); ++k; }
        if (k >= 8 || (!seen && dt > 120000)) next();
        break; }
    default: next();
    }
}
