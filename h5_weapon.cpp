// H5e: ARMA del jugador del servidor (H5G=1). Todo lo que decide el disparo es el codigo ORIGINAL que ya corre en el servidor:
// usercmd -> SV_ClientThink -> ClientThink_real -> Pmove -> PM_Weapon (bgame/bg_weapons.cpp: cadencia, municion, recarga, ADS)
// -> evento EV_FIRE_WEAPON -> FireWeapon (game/g_weapon.cpp) -> Bullet_Fire (game/bullet.cpp: BulletTrace con SV_LocationalTrace
// contra mundo y DObj) -> Bullet_Process -> G_Damage -> actor pain/death y los callbacks de script (_zombiemode_spawner).
// Este fichero solo:
//   - registra lo que pasa: [H5E] municion/estado del arma por cambio, cada bala que impacta (gancho #ifdef H5G en Bullet_Process),
//     salud de los actores alcanzados y muertes;
//   - H5_SET="dvar valor;dvar valor": dvars antes del primer frame (p.ej. bo1_testclient_fight 1, la pelea guionizada que ya trae
//     el arnes headless del mod: apunta al zombie visible mas cercano y pulsa los bits originales del usercmd);
//   - dibuja el viewmodel (H5e paso 1, ver H5W_DrawViewModel).
// Bits del usercmd (button_bits, CL_CmdButtons de client_mp/cl_input_mp.cpp; los mismos que usa g_sp_client.cpp):
//   0 attack (fuego), 2 melee, 3 activate (usar), 5 reload, 8 prone, 9 crouch, 11 ADS (speed_throw/toggle ads), 14 granada letal.
//   El cambio de arma no es un bit: es usercmd.weapon (indice de arma), como Bot_UpdateWeapon / CG_NextWeapon_f.
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <qcommon/common.h>
#include <server_mp/server_mp.h>
#include <game_mp/g_main_mp.h>
#include <game/actor.h>
#include <game_mp/actor_mp.h>
#include <bgame/bg_weapons.h>
#include <bgame/bg_weapons_def.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// aligned_alloc es API 28 en Android (la app apunta a 26): posix_memalign con la misma semantica (se libera con free)
static inline void *H5_AlignedAlloc(size_t a, size_t n) { void *p = nullptr; return posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n ? n : a) ? nullptr : p; }

#include <string>
#include <vector>
#include <array>
#include <math.h>
#include <server/sv_world.h>
#include <game_mp/g_combat_mp.h>
#include <game_mp/g_utils_mp.h>
#include <game_mp/g_misc_mp.h>
#include <clientscript/scr_const.h>
#include <clientscript/cscr_stringlist.h>
#include <game_mp/g_spawn_mp.h>
#include <game/g_items.h>   // OLA F1: Add_Ammo (municion)
#include <client_mp/g_client_mp.h>   // OLA F1: SetClientViewAngle, G_GetPlayerViewOrigin (dispara)
#include <universal/com_math.h>

static int g_h5eShots, g_h5eHits, g_h5eActorHits, g_h5eDamage, g_h5eKills, g_h5eReloads, g_h5eLogged;
static int g_h5eLastClip = -1, g_h5eLastStock = -1, g_h5eLastState = -1, g_h5eLastWeapon = -1;

// Dvars de TUs de UI/cliente que el Pmove del servidor lee (PM_Weapon_WeaponTimeAdjust al recargar). Mismo nombre, valor, rango
// y banderas que el original (ui_mp/ui_gametype_variants_mp.cpp UI_InitGametypeVariants).
#include <ui_mp/ui_gametype_variants_mp.h>
static void H5E_RegisterMissingDvars()
{
    if (!ui_gv_reloadSpeedModifier)
        ui_gv_reloadSpeedModifier = _Dvar_RegisterFloat("ui_gv_reloadSpeedModifier", 0.0f, 0.0f, 4.0f, 0, "Adjusts the reload speed");
}

void H5E_ApplyDvars()
{
    H5E_RegisterMissingDvars();
    const char *s = getenv("H5_SET");
    if (!s) return;
    std::string all = s; size_t p0 = 0;
    while (p0 < all.size())
    {
        size_t p1 = all.find(';', p0); if (p1 == std::string::npos) p1 = all.size();
        std::string kv = all.substr(p0, p1 - p0); p0 = p1 + 1;
        size_t sp = kv.find_first_of(" =");
        if (sp == std::string::npos) continue;
        std::string k = kv.substr(0, sp), v = kv.substr(sp + 1);
        if (!Dvar_FindVar(k.c_str())) { fprintf(stderr, "[H5E] H5_SET: la dvar %s no existe\n", k.c_str()); continue; }
        Dvar_SetFromStringByName(k.c_str(), (char *)v.c_str());
        fprintf(stderr, "[H5E] H5_SET %s = %s\n", k.c_str(), v.c_str());
    }
}

// Gancho #ifdef H5G en Bullet_Process (game/bullet.cpp), justo despues de G_Damage (o sin el si la entidad no recibe dano).
void H5E_NoteBulletHit(const float *start, const float *hitPos, gentity_s *attacker, gentity_s *hitEnt, int damage, int hitLoc,
    int healthBefore, int healthAfter, int hitType)
{
    ++g_h5eHits;
    const bool actor = hitEnt && hitEnt->actor;
    if (actor) { ++g_h5eActorHits; g_h5eDamage += healthBefore - (healthAfter > 0 ? healthAfter : 0); if (healthBefore > 0 && healthAfter <= 0) ++g_h5eKills; }
    if (g_h5eLogged < 400 || actor)
    {
        ++g_h5eLogged;
        const float dx = hitPos[0] - start[0], dy = hitPos[1] - start[1], dz = hitPos[2] - start[2];
        fprintf(stderr, "[H5E] t=%d bala de %d: impacto %s ent %d tipo %d a %.0f u (%.0f,%.0f,%.0f) dano %d hitloc %d salud %d -> %d%s\n", level.time,
            attacker ? attacker->s.number : -1, actor ? "ACTOR" : hitEnt ? "entidad" : "mundo", hitEnt ? hitEnt->s.number : -1, hitType,
            sqrtf(dx * dx + dy * dy + dz * dz), hitPos[0], hitPos[1], hitPos[2], damage, hitLoc, healthBefore, healthAfter,
            actor && healthBefore > 0 && healthAfter <= 0 ? " MUERTO" : "");
    }
}

// Montaje de la prueba guionizada (H5E_NEAR=<distancia>, desactivado por defecto): hasta H5d los zombies no persiguen al jugador
// (no le llega entrada) y el jugador de prueba no ve ninguno. Cada 2 s, si ningun actor vivo es visible desde el ojo (la misma traza
// G_LocationalTrace con bulletPriorityMap que usa la pelea del arnes), se coloca al jugador a esa distancia del actor vivo mas
// cercano con TeleportPlayer (lo que hace el cheat setviewpos, g_cmds_mp.cpp), en un hueco libre comprobado con G_TraceCapsule.
// No toca municion, dano ni script: la pelea del arnes (bo1_testclient_fight) apunta y dispara despues con el usercmd normal.
static int g_h5eNextNear;
static bool H5E_ActorVisible(const float *eye, gentity_s *ent, int passEnt)
{
    float point[3];
    if (!G_DObjGetWorldTagPos(ent, scr_const.j_head, point)) { point[0] = ent->r.currentOrigin[0]; point[1] = ent->r.currentOrigin[1]; point[2] = ent->r.currentOrigin[2] + 40.0f; }
    trace_t tr; G_LocationalTrace(&tr, eye, point, passEnt, 0x280E833, bulletPriorityMap, nullptr);
    return tr.fraction >= 1.0f || (tr.hitType == TRACE_HITTYPE_ENTITY && tr.hitId == ent->s.number);
}
static void H5E_PlaceNearActor()
{
    static const float dist = getenv("H5E_NEAR") ? (float)atof(getenv("H5E_NEAR")) : 0.0f;
    if (dist <= 0.0f || level.time < g_h5eNextNear || !level.actors) return;
    g_h5eNextNear = level.time + 2000;
    gentity_s *pl = &g_entities[0];
    if (!pl->r.inuse || !pl->client || pl->health <= 0) return;
    playerState_s *ps = &pl->client->ps;
    const float eye[3] = { ps->origin[0], ps->origin[1], ps->origin[2] + ps->viewHeightCurrent };
    gentity_s *best = nullptr; float bd = 1e30f;
    for (int i = 0; i < MAX_ACTORS; ++i)
    {
        actor_s *a = &level.actors[i];
        if (!a->inuse || !a->ent || a->ent->health <= 0 || !a->Physics.bIsAlive) continue;
        if (H5E_ActorVisible(eye, a->ent, pl->s.number)) return;   // ya hay uno a la vista: la pelea se encarga
        const float d = Vec3DistanceSq(a->ent->r.currentOrigin, ps->origin);
        if (d < bd) { bd = d; best = a->ent; }
    }
    if (!best) return;
    const float mins[3] = { -15.0f, -15.0f, 0.0f }, maxs[3] = { 15.0f, 15.0f, 70.0f };
    for (int k = 0; k < 16; ++k)
    {
        const float yaw = (float)k * (6.2831853f / 16.0f);
        const float from[3] = { best->r.currentOrigin[0], best->r.currentOrigin[1], best->r.currentOrigin[2] + 4.0f };
        const float to[3] = { from[0] + cosf(yaw) * dist, from[1] + sinf(yaw) * dist, from[2] };
        trace_t tr; col_context_t ctx; G_TraceCapsule(&tr, from, mins, maxs, to, best->s.number, 0x2810011, &ctx);
        if (tr.fraction < 1.0f || tr.startsolid || tr.allsolid) continue;
        float org[3] = { to[0], to[1], best->r.currentOrigin[2] }, dir[3], ang[3];
        const float neye[3] = { org[0], org[1], org[2] + ps->viewHeightCurrent };
        if (!H5E_ActorVisible(neye, best, pl->s.number)) continue;
        for (int j = 0; j < 3; ++j) dir[j] = best->r.currentOrigin[j] - org[j];
        dir[2] += 40.0f - ps->viewHeightCurrent;
        vectoangles(dir, ang);
        TeleportPlayer(pl, org, ang);
        fprintf(stderr, "[H5E] t=%d montaje: jugador a %.0f u del actor %d (salud %d) en (%.0f,%.0f,%.0f)\n", level.time, dist, best->s.number,
            best->health, org[0], org[1], org[2]);
        return;
    }
}

// OLA M1: guion de prueba por tiempo de partida (diagnostico; sin la variable no hace nada), para probar en cualquier mapa lo que el
// piloto de Five no alcanza (puertas, corriente, caja, perks, Pack-a-Punch, teletransporte): H5_TIMED="T:accion;T:accion" con T en
// segundos de partida y accion =
//   dvar NOMBRE VALOR        Dvar_SetFromStringByName (con H5_DEV=1 vale el devgui ORIGINAL: zombie_devgui power_on/open_sesame/money...)
//   tp X Y Z YAW [PITCH]     TeleportPlayer (lo que hace el cheat setviewpos)
//   use MS                   mantiene USAR (bit 3 del usercmd) MS milisegundos: la compra/accion la hace el GSC original
//   fire MS                  mantiene DISPARO MS milisegundos
//   shot                     captura (solo con P4_OUT)
//   usetn TARGETNAME         (OLA S1) el trigger con ese targetname mas cercano al jugador recibe notify("trigger", jugador),
//                            como USAR sobre el trigger (para probar la caja/maquinas sin depender de la posicion exacta)
//   tpsee TX TY TZ [R]       (OLA E1a) al punto con suelo mas cercano desde el que el ojo ve T a <= R (64), mirandolo
//   tpsee TARGETNAME [R]     (OLA E1a) igual, con T = centro de los limites de la entidad mas cercana con ese targetname
//   jump|melee|frag [MS]     (OLA E2b) mantiene salto (bit 10), cuchillo (bit 2) o granada letal (bit 14) MS ms (200/200/300)
//   tact [MS]                (OLA E3a) mantiene el boton de la granada tactica (bit 15 del usercmd) MS (300) ms
//   weap NOMBRE              (OLA E3a) cambia al arma NOMBRE si el jugador la tiene (como elegirla con weapnext)
//   dispara X Y Z|yo R MS    (OLA F1) apunta y dispara al actor vivo mas cercano en planta (H5E_DisparaFrame)
//   acude X Y Z [X Y Z...]   (OLA F1) acude con USAR a un bucle de sonido que aparezca junto a uno de los puntos (H5E_AcudeFrame)
//   municion ARMA N          (OLA F1) Add_Ammo al jugador (granadas, cargador)
//   tpdmg TN IDX [R [RMIN]]  (OLA F1) al suelo desde cuya mano la explosion alcanza la IDX-esima entidad (CanDamage de G_RadiusDamage)
bool g_h5eViewResync = false;
unsigned g_h5eWeapReq;   // OLA E3a: lo consume h5_play.cpp (H5P_CmdButtons)
int g_h5eUseUntil, g_h5eFireUntil; bool g_h5eShotReq;
int g_h5eBtnUntil[16];   // OLA E2b: jump/melee/frag MS del guion -> bit del usercmd (10 salto, 2 cuchillo, 14 granada)
namespace {
struct TimedAct { int t; std::string a; bool done; };
std::vector<TimedAct> s_timed; bool s_timedInit;
// OLA F1: suelo bajo (X,Y) entre Z+160 y Z-240 para la capsula del jugador, probando el punto y anillos de 24/48 u (false = no hay)
bool H5E_FloorNear(const float *p, int self, float *out)
{
    const float mins[3] = { -15.0f, -15.0f, 0.0f }, maxs[3] = { 15.0f, 15.0f, 70.0f };
    for (float r = 0.0f; r <= 48.5f; r += 24.0f) for (int k = 0; k < (r > 0 ? 8 : 1); ++k) {
        const float a = k * 0.78539816f, o[2] = { p[0] + r * cosf(a), p[1] + r * sinf(a) };
        for (float dz = 160.0f; dz >= 0.0f; dz -= 40.0f) {
            const float from[3] = { o[0], o[1], p[2] + dz }, to[3] = { o[0], o[1], p[2] - 240.0f };
            trace_t tr; col_context_t ctx; G_TraceCapsule(&tr, from, mins, maxs, to, self, 0x2810011, &ctx);
            if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f) continue;
            out[0] = o[0]; out[1] = o[1]; out[2] = from[2] + (to[2] - from[2]) * tr.fraction + 1.0f; return true;
        }
    }
    return false;
}
// OLA F1: acude X Y Z [X Y Z...]: desde su tiempo y hasta el final, cada 0,25 s, si una entidad con bucle de sonido (s.loopSoundId) aparece
// a < 40 u de uno de los puntos (el script_origin "looper" del telefono rojo de Ascension, que suena 10 s en un sitio al azar a los 120-300 s),
// el jugador va andando... aqui teletransportado al suelo junto al punto, mirandolo, y mantiene USAR 2 s (lo que hace un jugador que lo oye).
std::vector<std::array<float, 3>> s_acude; int s_acudeNext, s_acudeLastEnt = -1, s_acudeLastT;
void H5E_AcudeFrame(gentity_s *pl)
{
    if (s_acude.empty() || !pl->r.inuse || !pl->client || level.time < s_acudeNext) return;
    s_acudeNext = level.time + 250;
    for (int i = 0; i < level.num_entities; ++i) { gentity_s *e = &g_entities[i];
        if (!e->r.inuse || !e->s.loopSoundId || e == pl) continue;
        for (const auto &p : s_acude) {
            const float dx = e->r.currentOrigin[0] - p[0], dy = e->r.currentOrigin[1] - p[1], dz = e->r.currentOrigin[2] - p[2];
            if (dx * dx + dy * dy > 1600.0f || fabsf(dz) > 60.0f) continue;
            if (i == s_acudeLastEnt && level.time - s_acudeLastT < 12000) return;   // ya acudido a este bucle
            float fl[3]; const bool ok = H5E_FloorNear(p.data(), pl->s.number, fl); if (!ok) memcpy(fl, p.data(), 12);
            float dir[3] = { p[0] - fl[0], p[1] - fl[1], p[2] - (fl[2] + 60.0f) }, ang[3]; if (fabsf(dir[0]) + fabsf(dir[1]) < 1.0f) dir[0] = 1.0f;
            vectoangles(dir, ang); ang[2] = 0; if (ang[0] > 180.0f) ang[0] -= 360.0f;
            TeleportPlayer(pl, fl, ang); g_h5eViewResync = true; g_h5eUseUntil = level.time + 2000;
            s_acudeLastEnt = i; s_acudeLastT = level.time;
            fprintf(stderr, "[H5E] t=%d guion: acude: bucle de sonido en la entidad %d (%.0f,%.0f,%.0f): jugador en (%.0f,%.0f,%.0f)%s y USAR 2 s\n", level.time, i,
                e->r.currentOrigin[0], e->r.currentOrigin[1], e->r.currentOrigin[2], fl[0], fl[1], fl[2], ok ? "" : " (SIN suelo)");
            return;
        }
    }
}
// OLA F1: dispara X Y Z R MS | dispara yo R MS: durante MS ms, cada 0,25 s, apunta (SetClientViewAngle) a la cabeza del actor vivo mas
// cercano en planta a (X,Y) (o al jugador) dentro de R y pulsa DISPARO 100 ms (pulsaciones sueltas: sirve tambien con armas semiautomaticas)
struct Dispara { float p[3]; float R; bool yo; int until; } s_disp; int s_dispNext;
void H5E_DisparaFrame(gentity_s *pl)
{
    if (!pl->r.inuse || !pl->client || level.time > s_disp.until || level.time < s_dispNext) return;
    s_dispNext = level.time + 250;
    const float *c = s_disp.yo ? pl->r.currentOrigin : s_disp.p; gentity_s *best = nullptr; float bd = s_disp.R * s_disp.R;
    for (int i = 0; i < level.num_entities; ++i) { gentity_s *e = &g_entities[i];
        if (!e->r.inuse || !e->actor || e->health <= 0 || e->health >= 100000) continue;   // fuera jefes invulnerables (el Director de Coast: 1000000)
        const float dx = e->r.currentOrigin[0] - c[0], dy = e->r.currentOrigin[1] - c[1], d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = e; } }
    if (!best) return;
    float tg[3], eye[3], dir[3], ang[3];
    if (!G_DObjGetWorldTagPos(best, scr_const.j_head, tg)) { memcpy(tg, best->r.currentOrigin, 12); tg[2] += 40.0f; }
    G_GetPlayerViewOrigin(&pl->client->ps, eye);
    for (int k = 0; k < 3; ++k) dir[k] = tg[k] - eye[k];
    vectoangles(dir, ang); ang[2] = 0; if (ang[0] > 180.0f) ang[0] -= 360.0f;
    SetClientViewAngle(pl, ang); g_h5eViewResync = true; g_h5eFireUntil = level.time + 100;
    static int lastEnt = -1; if (best->s.number != lastEnt) { lastEnt = best->s.number;
        fprintf(stderr, "[H5E] t=%d guion: dispara al actor %d (salud %d) en (%.0f,%.0f,%.0f)\n", level.time, best->s.number, best->health, tg[0], tg[1], tg[2]); }
}
void H5E_Timed()
{
    H5E_AcudeFrame(&g_entities[0]);
    H5E_DisparaFrame(&g_entities[0]);
    if (!s_timedInit) {
        s_timedInit = true;
        if (const char *e = getenv("H5_TIMED")) { std::string all = e; size_t p0 = 0;
            while (p0 < all.size()) { size_t p1 = all.find(';', p0); if (p1 == std::string::npos) p1 = all.size(); std::string it = all.substr(p0, p1 - p0); p0 = p1 + 1;
                const size_t c = it.find(':'); if (c == std::string::npos) continue; s_timed.push_back({ (int)(atof(it.substr(0, c).c_str()) * 1000.0), it.substr(c + 1), false }); } }
    }
    for (TimedAct &ta : s_timed)
    {
        if (ta.done || level.time < ta.t) continue;
        ta.done = true;
        char cmd[32] = ""; sscanf(ta.a.c_str(), "%31s", cmd);
        gentity_s *pl = &g_entities[0];
        if (!strcmp(cmd, "dvar")) { char k[96] = "", v[256] = ""; if (sscanf(ta.a.c_str(), "%*s %95s %255[^\n]", k, v) >= 1) { if (!Dvar_FindVar(k)) { fprintf(stderr, "[H5E] guion: la dvar %s no existe: se crea (set)\n", k); Dvar_SetCommand(k, v); } else Dvar_SetFromStringByName(k, v); } }   // OLA E2a: set crea (cheat_sq)
        else if (!strcmp(cmd, "tp") && pl->r.inuse && pl->client) { float o[3], a[3] = { 0, 0, 0 }; if (sscanf(ta.a.c_str(), "%*s %f %f %f %f %f", &o[0], &o[1], &o[2], &a[1], &a[0]) >= 4) { TeleportPlayer(pl, o, a); g_h5eViewResync = true; } }
        else if (!strcmp(cmd, "tpl") && pl->r.inuse && pl->client) {   // tpl X Y Z TX TY TZ: al suelo bajo (X,Y,Z) mirando al punto T
            float o[3], tg[3]; if (sscanf(ta.a.c_str(), "%*s %f %f %f %f %f %f", &o[0], &o[1], &o[2], &tg[0], &tg[1], &tg[2]) == 6) {
                const float mins[3] = { -15.0f, -15.0f, 0.0f }, maxs[3] = { 15.0f, 15.0f, 70.0f };
                const float from[3] = { o[0], o[1], o[2] + 32.0f }, to[3] = { o[0], o[1], o[2] - 400.0f };
                trace_t tr; col_context_t ctx; G_TraceCapsule(&tr, from, mins, maxs, to, pl->s.number, 0x2810011, &ctx);
                if (!tr.startsolid) o[2] = from[2] + (to[2] - from[2]) * tr.fraction + 1.0f;
                float dir[3] = { tg[0] - o[0], tg[1] - o[1], tg[2] - (o[2] + 60.0f) }, ang[3]; vectoangles(dir, ang); ang[2] = 0; if (ang[0] > 180.0f) ang[0] -= 360.0f;   /* E1a: cabeceo en (-180,180]: la camara recorta 0..360 a 89 */
                TeleportPlayer(pl, o, ang); g_h5eViewResync = true;
                fprintf(stderr, "[H5E] guion: jugador en (%.0f,%.0f,%.0f)%s mirando (%.0f,%.0f,%.0f)\n", o[0], o[1], o[2], tr.startsolid ? " (SIN suelo: dentro de solido)" : "", tg[0], tg[1], tg[2]);
            } }
        else if (!strcmp(cmd, "tpsee") && pl->r.inuse && pl->client) {   // OLA E1a: tpsee TX TY TZ R: el punto con suelo mas cercano a T
            // (anillos de 16 a R, 16 direcciones) cuyo ojo (suelo + 60) ve T sin obstaculos y queda a <= R; mirando a T. Para triggers de
            // uso (alcance 72 desde el centro, player_use_mp) o blancos de disparo sin conocer la geometria.
            // tpsee TARGETNAME [R]: T = centro de los limites (absmin/absmax) de la entidad con ese targetname mas cercana, que es el punto
            // al que mira y mide Player_GetUseList (el "origin" del .map no siempre coincide)
            float tg[3], R = 64.0f; char tn[96] = ""; bool okArgs = sscanf(ta.a.c_str(), "%*s %f %f %f %f", &tg[0], &tg[1], &tg[2], &R) >= 3;
            if (!okArgs && sscanf(ta.a.c_str(), "%*s %95s %f", tn, &R) >= 1) { float bd2 = 1e30f;
                for (int i = 0; i < level.num_entities; ++i) { gentity_s *t = &g_entities[i];
                    if (!t->r.inuse || ((!t->targetname || strcmp(SL_ConvertToString(t->targetname, SCRIPTINSTANCE_SERVER), tn))
                        && (!t->model || strcmp(SL_ConvertToString(G_ModelName(t->model), SCRIPTINSTANCE_SERVER), tn)))) continue;   // OLA F1: o por modelo (script_model sin targetname)
                    float c[3], d2 = 0; for (int k = 0; k < 3; ++k) { c[k] = 0.5f * (t->r.absmin[k] + t->r.absmax[k]); d2 += (c[k] - pl->r.currentOrigin[k]) * (c[k] - pl->r.currentOrigin[k]); }
                    if (d2 < bd2) { bd2 = d2; memcpy(tg, c, 12); okArgs = true; } }
                if (!okArgs) fprintf(stderr, "[H5E] guion: tpsee %s: no hay entidad con ese targetname\n", tn); }
            if (okArgs) {
                const float mins[3] = { -15.0f, -15.0f, 0.0f }, maxs[3] = { 15.0f, 15.0f, 70.0f }, z0[3] = { 0, 0, 0 };
                float best[3] = { 0, 0, 0 }, bd = 1e30f, bmiss = 1e30f; bool ok = false;
                // OLA F1: H5E_SEEMASK=0x... cambia la mascara de la vista: 0x11 = SV_SightTracePoint de Player_GetUseList (solo solido),
                // 0x280E833 = la de las balas (Bullet_Fire); la de por defecto incluye el playerclip, que no tapa ni el uso ni los disparos
                const char *smE = getenv("H5E_SEEMASK"); const int seeMask = smE ? (int)strtol(smE, nullptr, 0) : 0x2810011;
                for (int pass = 0; pass < 3 && !ok; ++pass)   // 1a: vista limpia (la que exige UseTriggerRequireLookAt); 2a: tolerante; 3a: el menor obstaculo
                for (float r = 16.0f; r <= R + 0.5f && (!ok || pass == 2); r += 8.0f) for (int k = 0; k < 16; ++k) {
                    const float a = k * 0.39269908f, o[3] = { tg[0] + r * cosf(a), tg[1] + r * sinf(a), tg[2] };
                    for (float dz = 160.0f; dz >= -160.0f; dz -= 16.0f) {   // el suelo puede estar por encima o por debajo del blanco
                        const float from[3] = { o[0], o[1], o[2] + dz }, to[3] = { o[0], o[1], o[2] + dz - 400.0f };
                        trace_t tr; col_context_t ctx; G_TraceCapsule(&tr, from, mins, maxs, to, pl->s.number, 0x2810011, &ctx);
                        if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f) continue;
                        const float fl = from[2] + (to[2] - from[2]) * tr.fraction + 1.0f, eye[3] = { o[0], o[1], fl + 60.0f };
                        const float d = sqrtf((eye[0] - tg[0]) * (eye[0] - tg[0]) + (eye[1] - tg[1]) * (eye[1] - tg[1]) + (eye[2] - tg[2]) * (eye[2] - tg[2]));
                        if (d > R || (pass < 2 && d >= bd)) continue;
                        trace_t lt; col_context_t c2; G_TraceCapsule(&lt, eye, z0, z0, tg, pl->s.number, seeMask, &c2);
                        const float miss = (1.0f - lt.fraction) * d;   // el blanco puede estar dentro de un modelo con colision (radio, mesa)
                        if (pass < 2 && lt.fraction < 0.97f && (pass == 0 || miss > 20.0f)) continue;
                        if (pass == 2) { if (miss >= bmiss) continue; bmiss = miss; }
                        bd = d; best[0] = o[0]; best[1] = o[1]; best[2] = fl; ok = true;
                    }
                }
                if (ok) { float dir[3] = { tg[0] - best[0], tg[1] - best[1], tg[2] - (best[2] + 60.0f) }, ang[3]; vectoangles(dir, ang); ang[2] = 0; if (ang[0] > 180.0f) ang[0] -= 360.0f;
                    TeleportPlayer(pl, best, ang); g_h5eViewResync = true; }
                fprintf(stderr, "[H5E] guion: tpsee (%.0f,%.0f,%.0f) R=%.0f -> %s (%.0f,%.0f,%.0f) a %.0f%s\n", tg[0], tg[1], tg[2], R, ok ? "jugador en" : "SIN punto",
                    best[0], best[1], best[2], ok ? bd : 0.0f, bmiss < 1e29f ? " (SIN vista limpia: el menor obstaculo)" : "");
            } }
        else if (!strcmp(cmd, "use")) { int ms = 1500; sscanf(ta.a.c_str(), "%*s %d", &ms); g_h5eUseUntil = level.time + ms; }
        else if (!strcmp(cmd, "fire")) { int ms = 500; sscanf(ta.a.c_str(), "%*s %d", &ms); g_h5eFireUntil = level.time + ms; }
        else if (!strcmp(cmd, "ads")) { int ms = 1000; sscanf(ta.a.c_str(), "%*s %d", &ms); g_h5eBtnUntil[11] = level.time + ms; }   // OLA F1: apuntar (+speed_throw, bit 11)
        else if (!strcmp(cmd, "jump") || !strcmp(cmd, "melee") || !strcmp(cmd, "frag")) {   // OLA E2b
            int ms = !strcmp(cmd, "frag") ? 300 : 200; sscanf(ta.a.c_str(), "%*s %d", &ms);
            g_h5eBtnUntil[cmd[0] == 'j' ? 10 : cmd[0] == 'm' ? 2 : 14] = level.time + ms; }
        else if (!strcmp(cmd, "weap")) { char wn[64] = ""; sscanf(ta.a.c_str(), "%*s %63s", wn); g_h5eWeapReq = (unsigned)BG_GetWeaponIndexForName(wn);   // OLA E3a: cambiar al arma
            { bool BG_PlayerHasWeapon(const playerState_s *, unsigned int);   // OLA F1: arma alternativa de la actual (y si esta en heldWeapons)
              const unsigned cw = pl->client ? pl->client->ps.weapon : 0, alt = cw ? BG_GetWeaponVariantDef(cw)->altWeaponIndex : 0;
              fprintf(stderr, "[H5E] guion: weap %s -> %u (actual %s estado %d, alternativa %s %s)\n", wn, g_h5eWeapReq, cw ? BG_WeaponName(cw) : "-",
                  pl->client ? (int)pl->client->ps.weaponstate : -1, alt ? BG_WeaponName(alt) : "-", alt && BG_PlayerHasWeapon(&pl->client->ps, alt) ? "en mano" : "NO llevada"); } }
        else if (!strcmp(cmd, "municion") && pl->client) {   // OLA F1: municion ARMA N -> Add_Ammo (g_items.cpp), p. ej. granadas para la granada santa
            char wn[64] = ""; int n = 1; sscanf(ta.a.c_str(), "%*s %63s %d", wn, &n);
            const unsigned wi = (unsigned)BG_GetWeaponIndexForName(wn);
            const int got = wi ? Add_Ammo(pl, wi, 0, n, 1) : 0;
            fprintf(stderr, "[H5E] guion: municion %s +%d -> %s (%d)\n", wn, n, wi ? "ok" : "arma desconocida", got); }
        else if (!strcmp(cmd, "tact")) { int ms = 300; sscanf(ta.a.c_str(), "%*s %d", &ms); g_h5eBtnUntil[15] = level.time + ms; }   // OLA E3a: granada tactica (bit 15: agujero negro, munecas)
        else if (!strcmp(cmd, "shot")) g_h5eShotReq = true;
        else if (!strcmp(cmd, "entpos")) {   // OLA F1: entpos TARGETNAME -> origen de cada entidad con ese targetname (MoveZ/MoveTo del GSC)
            char tn[96] = ""; sscanf(ta.a.c_str(), "%*s %95s", tn); int n = 0;
            for (int i = 0; i < level.num_entities; ++i) { gentity_s *t = &g_entities[i];
                if (!t->r.inuse || !t->targetname || strcmp(SL_ConvertToString(t->targetname, SCRIPTINSTANCE_SERVER), tn)) continue;
                fprintf(stderr, "[H5E] guion: entpos %s #%d -> (%.1f,%.1f,%.1f)\n", tn, i, t->r.currentOrigin[0], t->r.currentOrigin[1], t->r.currentOrigin[2]); ++n; }
            if (!n) fprintf(stderr, "[H5E] guion: entpos %s -> NO hay entidad con ese targetname\n", tn); }
        else if (!strcmp(cmd, "dispara")) {   // OLA F1: ver H5E_DisparaFrame
            int ms = 5000; s_disp.yo = !strncmp(ta.a.c_str() + 8, "yo", 2);
            if (s_disp.yo) sscanf(ta.a.c_str(), "%*s %*s %f %d", &s_disp.R, &ms); else sscanf(ta.a.c_str(), "%*s %f %f %f %f %d", &s_disp.p[0], &s_disp.p[1], &s_disp.p[2], &s_disp.R, &ms);
            s_disp.until = level.time + ms; }
        else if (!strcmp(cmd, "acude")) {   // OLA F1: ver H5E_AcudeFrame
            std::array<float, 3> p; int n = 0; const char *s = ta.a.c_str() + 5;
            while (sscanf(s, "%f %f %f%n", &p[0], &p[1], &p[2], &n) == 3) { s_acude.push_back(p); s += n; }
            fprintf(stderr, "[H5E] guion: acude a %zu puntos\n", s_acude.size()); }
        else if (!strcmp(cmd, "tpdmg") && pl->r.inuse && pl->client) {   // OLA F1: tpdmg TARGETNAME IDX [R]: la IDX-esima entidad con ese
            // targetname (orden de GetEntArray = numero de entidad); el suelo a <= R (160) cuyo punto de la mano (suelo + 48) tiene CanDamage > 0
            // contra ella (la prueba de G_RadiusDamage, mascara 0x802013): ahi una granada cocinada en la mano le hace dano de explosion
            char tn[96] = ""; int idx = 0; float R = 160.0f, Rmin = 0.0f; sscanf(ta.a.c_str(), "%*s %95s %d %f %f", tn, &idx, &R, &Rmin); gentity_s *t = nullptr;
            for (int i = 0, k = 0; i < level.num_entities; ++i) { gentity_s *e = &g_entities[i];
                if (!e->r.inuse || !e->targetname || strcmp(SL_ConvertToString(e->targetname, SCRIPTINSTANCE_SERVER), tn)) continue;
                if (k++ == idx) { t = e; break; } }
            if (!t) fprintf(stderr, "[H5E] guion: tpdmg %s %d: no existe\n", tn, idx);
            else {
                float c[3]; for (int k = 0; k < 3; ++k) c[k] = 0.5f * (t->r.absmin[k] + t->r.absmax[k]);
                int nAir = 0, nAll = 0;   // rejilla 3D de 16 u en la caja ampliada 64 u: cuantos puntos (en aire) la ven
                for (float x = t->r.absmin[0] - 64; x <= t->r.absmax[0] + 64; x += 16) for (float y = t->r.absmin[1] - 64; y <= t->r.absmax[1] + 64; y += 16)
                    for (float z = t->r.absmin[2] - 64; z <= t->r.absmax[2] + 64; z += 16) { const float q[3] = { x, y, z }; ++nAll;
                        if (CanDamage(t, nullptr, q, -1.0f, nullptr, 0x802013) > 0.0) ++nAir; }
                const float mins[3] = { -15.0f, -15.0f, 0.0f }, maxs[3] = { 15.0f, 15.0f, 70.0f };
                float best[3] = { 0, 0, 0 }, bd = 1e30f; int nFloor = 0;
                for (float r = Rmin; r <= R + 0.5f; r += 16.0f) for (int k = 0; k < 16; ++k) {
                    const float a = k * 0.39269908f, o[2] = { c[0] + r * cosf(a), c[1] + r * sinf(a) };
                    for (float dz = 160.0f; dz >= -160.0f; dz -= 32.0f) {
                        const float from[3] = { o[0], o[1], c[2] + dz }, to[3] = { o[0], o[1], c[2] + dz - 400.0f };
                        trace_t tr; col_context_t ctx; G_TraceCapsule(&tr, from, mins, maxs, to, pl->s.number, 0x2810011, &ctx);
                        if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f) continue;
                        const float fl = from[2] + (to[2] - from[2]) * tr.fraction + 1.0f, hand[3] = { o[0], o[1], fl + 48.0f };
                        if (CanDamage(t, nullptr, hand, -1.0f, nullptr, 0x802013) <= 0.0) continue;
                        ++nFloor; const float d = (hand[0] - c[0]) * (hand[0] - c[0]) + (hand[1] - c[1]) * (hand[1] - c[1]) + (hand[2] - c[2]) * (hand[2] - c[2]);
                        if (d < bd) { bd = d; best[0] = o[0]; best[1] = o[1]; best[2] = fl; }
                    } }
                fprintf(stderr, "[H5E] guion: tpdmg %s %d: entidad %d caja (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f), rejilla %d/%d puntos la ven, suelos validos %d%s\n", tn, idx,
                    t->s.number, t->r.absmin[0], t->r.absmin[1], t->r.absmin[2], t->r.absmax[0], t->r.absmax[1], t->r.absmax[2], nAir, nAll, nFloor, nFloor ? "" : ": NO se mueve");
                if (nFloor) { float dir[3] = { c[0] - best[0], c[1] - best[1], c[2] - (best[2] + 60.0f) }, ang[3]; vectoangles(dir, ang); ang[2] = 0; if (ang[0] > 180.0f) ang[0] -= 360.0f;
                    TeleportPlayer(pl, best, ang); g_h5eViewResync = true;
                    fprintf(stderr, "[H5E] guion: tpdmg: jugador en (%.0f,%.0f,%.0f), mano a %.0f u del centro\n", best[0], best[1], best[2], sqrtf(bd)); }
            } }
        else if (!strcmp(cmd, "usetn") && pl->r.inuse) {   // OLA S1: usetn TARGETNAME -> el trigger con ese targetname mas cercano recibe "trigger" del jugador
            char tn[96] = ""; sscanf(ta.a.c_str(), "%*s %95s", tn); gentity_s *best = nullptr; float bd = 1e30f;
            for (int i = 0; i < level.num_entities; ++i) { gentity_s *t = &g_entities[i];
                if (!t->r.inuse || !t->targetname || strcmp(SL_ConvertToString(t->targetname, SCRIPTINSTANCE_SERVER), tn)) continue;
                float d = 0; for (int k = 0; k < 3; ++k) d += (t->r.currentOrigin[k] - pl->r.currentOrigin[k]) * (t->r.currentOrigin[k] - pl->r.currentOrigin[k]);
                if (d < bd) { bd = d; best = t; } }
            if (best) { Scr_AddEntity(pl, SCRIPTINSTANCE_SERVER); Scr_Notify(best, scr_const.trigger, 1); }
            fprintf(stderr, "[H5E] guion: usetn %s -> %s\n", tn, best ? "notify trigger" : "NO hay entidad con ese targetname"); }
        fprintf(stderr, "[H5E] t=%d guion: %s\n", level.time, ta.a.c_str());
    }
}
}

// Cada frame de servidor: estado del arma del jugador 0 por cambio (municion del cargador y reserva, weaponstate, arma).
void H5E_Frame()
{
    H5E_Timed();
    H5E_PlaceNearActor();
    gentity_s *pl = &g_entities[0];
    if (!pl->r.inuse || !pl->client) return;
    const playerState_s *ps = &pl->client->ps;
    const unsigned w = ps->weapon;
    const int clip = w ? BG_GetAmmoInClip(ps, w) : -1, stock = w ? BG_GetAmmoNotInClip(ps, w) : -1, st = ps->weaponstate;
    if (w != (unsigned)g_h5eLastWeapon || clip != g_h5eLastClip || stock != g_h5eLastStock || st != g_h5eLastState)
    {
        if (g_h5eLastWeapon == (int)w && clip < g_h5eLastClip && g_h5eLastClip >= 0) g_h5eShots += g_h5eLastClip - clip;
        auto rel = [](int s) { return s >= WEAPON_RELOADING_RIGHT && s <= WEAPON_RELOAD_QUICK_EMPTY; };
        if (rel(st) && !rel(g_h5eLastState)) ++g_h5eReloads;
        fprintf(stderr, "[H5E] t=%d arma %s (%u) cargador %d reserva %d estado %d weaponTime %d ADS %.2f\n", level.time,
            w ? BG_WeaponName(w) : "-", w, clip, stock, st, ps->weaponTime, ps->fWeaponPosFrac);
        g_h5eLastWeapon = w; g_h5eLastClip = clip; g_h5eLastStock = stock; g_h5eLastState = st;
    }
}

void H5E_Report()
{
    fprintf(stderr, "[H5E] resumen: disparos %d (por cargador), impactos %d (en actores %d), dano a actores %d, muertes %d, recargas %d\n",
        g_h5eShots, g_h5eHits, g_h5eActorHits, g_h5eDamage, g_h5eKills, g_h5eReloads);
}

// ---------------------------------------------------------------------------------------------------------------
// Datos con valor inicial de TUs de cliente que no se compilan. genstubs los convertiria en BSS a cero (AABB de actor vacia:
// SV_TracePointToEntity descartaba todos los actores y las balas atravesaban a los zombies; el melee de zombie de H5d depende
// de lo mismo). Copiados tal cual de cgame/cg_world.cpp:21-24. Unica definicion en el harness (h5_play.cpp remite aqui).
float actorLocationalMinsBig[3] = { -54.0, -54.0, -32.0 };
float actorLocationalMaxsBig[3] = { 54.0, 54.0, 72.0 };
float actorLocationalMins[3]    = { -36.0, -36.0, -10.0 };
float actorLocationalMaxs[3]    = { 36.0, 36.0, 72.0 };
// cgame_mp/cg_ents_mp.cpp:57, tal cual: G_PlayerController (G_DObjCalcPose del jugador cuando una traza localizada le alcanza)
// desreferencia estos punteros; como stub de datos eran nulos (SIGSEGV).
const unsigned short *controller_names[6] = { &scr_const.back_low, &scr_const.back_mid, &scr_const.back_up, &scr_const.neck, &scr_const.head, &scr_const.pelvis };

// ---------------------------------------------------------------------------------------------------------------
// H5e paso 1: VIEWMODEL (brazos + arma en primera persona) con el codigo ORIGINAL del cliente (cgame/cg_weapons.cpp, que se
// compila entero en H5G): CG_UpdateViewWeaponAnim (CG_RegisterWeapon, ChangeViewmodelDobj -> Com_ClientDObjCreate con las
// manos + gunXModel en tag_weapon, CG_CreateWeaponViewModelXAnim, WeaponRunXModelAnims con ps->weapAnim, DObjUpdateClientInfo)
// y la cadena de posicion de CG_AddViewWeapon (CalculateWeaponPostion_PositionToADS, CG_BuildWeaponState,
// BG_CalculateWeaponMovement, CG_CalculateWeaponMovement_ClientSpecific, CG_SaveWeaponState) y de CG_AddPlayerWeapon
// (placement -> viewModelAxis -> CG_UpdateViewModelPose). El cliente (cg_s) no corre: se rellena su predictedPlayerState con el
// playerState del servidor (sin prediccion: es el mismo proceso) y la vista con el ojo del jugador, como CG_CalcViewValues.
// R_AddDObjToScene no esta portado: el esqueleto se calcula como CL_DObjCreateSkelForBones + DObjCalcSkel (CG_DObjCalcPose sin
// controladores) y se deforma/dibuja con el mismo puente que los actores (R_SkinXSurfaceSkinned + H4_DrawSkinnedSurf) tras limpiar
// la profundidad, para que el arma quede delante del mundo como el depth hack del viewmodel.
// Stubs que quedan (olas posteriores): fogonazo (WeaponFlash/EffectsCore), sonidos de notetrack, laser, opciones de camuflaje
// (CG_SetupWeaponOptionsRender), luz de modelo real del viewmodel (lightgrid).
#include <cgame_mp/cg_local_mp.h>
#include <cgame_mp/cg_main_mp.h>
#include <cgame/cg_local.h>
#include <cgame/cg_weapons.h>
#include <bgame/bg_weapons_view.h>
#include <xanim/dobj.h>
#include <xanim/dobj_utils.h>
#include <xanim/dobj_skel.h>
#include <xanim/xmodel.h>
#include <xanim/xmodel_utils.h>
#include <gfx_d3d/r_model_skin.h>
#include <gfx_d3d/r_state.h>
#include <gfx_d3d/rb_backend.h>
#include <EffectsCore/fx_marks.h>
#include <universal/com_math_anglevectors.h>
#include "world_h4.h"

// cgame/cg_main.cpp:84 (TU no compilada), tal cual: CG_SetupWeaponDef (cg_weapons.cpp, ahora real) la usa desde G_InitGame.
int __cdecl CG_GetWeaponIndexForName(char *name) { return BG_GetWeaponIndexForName(name); }

#include <cgame_mp/cg_pose_mp.h>
#include <cgame/cg_drawtools.h>
// MatrixTransformVectorQuatTrans (DObjTraceline) y BG_Player_DoControllers (G_PlayerController), extraidas tal cual de
// cgame_mp/cg_pose_mp.cpp por build.sh (el resto de esa TU es pose de cliente que no hace falta).
#include <xanim/dobj_utils.h>
#include "h5_weapon_pose.inc"

// Hunk_AllocXAnimClient, CG_GetPredictedPlayerState...: extraidas tal cual de cgame_mp/cg_main_mp.cpp por build.sh.
#include <universal/com_memory.h>
#include "h5_weapon_cgmain.inc"

static bool g_h5wInit;
// H5f (h5_play.cpp): vista y tiempo del frame de render (camara interpolada, tiempo de servidor interpolado en ms)
bool g_h5wViewOverride; float g_h5wViewOrg[3], g_h5wViewAng[3]; int g_h5wViewTime;
static int g_h5wLastTime = -1, g_h5wSkelStamp = 1;
static char *g_h5wSkelBuf; static int g_h5wSkelPos;
static void (*g_h5wPrevDraw)(unsigned char);
static DObjSkelMat g_h5wSkel[256] __attribute__((aligned(16)));
static GfxPackedVertex *g_h5wVerts; static unsigned g_h5wVertCap;
int g_h5wSurfs, g_h5wLogged;

// CG_RegisterDvars (cgame_mp/cg_main_mp.cpp): solo las dvars que lee la cadena del viewmodel, con nombre, valor, rango y banderas
// del original. Las demas siguen sin registrar (un acceso a una nula da SIGSEGV con su traza).
static void H5W_RegisterDvars()
{
    if (!cg_fov) cg_fov = _Dvar_RegisterFloat("cg_fov", 65.0, 1.0, 160.0, 0x80u, "The field of view angle in degrees");
    if (!cg_fov_default) cg_fov_default = _Dvar_RegisterFloat("cg_fov_default", 65.0, 65.0, 80.0, 1u, "User default field of view angle in degrees");
    if (!cg_fovCompMax) cg_fovCompMax = _Dvar_RegisterFloat("cg_fovCompMax", 85.0, 1.0, 160.0, 0, "The maximum field of view to compensate for gun placement");
    if (!cg_gun_fovcomp_x) cg_gun_fovcomp_x = _Dvar_RegisterFloat("cg_gun_fovcomp_x", -2.0, -3.4028235e38, 3.4028235e38, 0, "x position FOV offset compensation of the viewmodel");
    if (!cg_gun_fovcomp_y) cg_gun_fovcomp_y = _Dvar_RegisterFloat("cg_gun_fovcomp_y", 0.0, -3.4028235e38, 3.4028235e38, 0, "y position FOV offset compensation of the viewmodel");
    if (!cg_gun_fovcomp_z) cg_gun_fovcomp_z = _Dvar_RegisterFloat("cg_gun_fovcomp_z", 0.0, -3.4028235e38, 3.4028235e38, 0, "z position FOV offset compensation of the viewmodel");
    if (!cg_gun_x) cg_gun_x = _Dvar_RegisterFloat("cg_gun_x", 0.0, -3.4028235e38, 3.4028235e38, 0x80u, "x position of the viewmodel");
    if (!cg_gun_y) cg_gun_y = _Dvar_RegisterFloat("cg_gun_y", 0.0, -3.4028235e38, 3.4028235e38, 0x80u, "y position of the viewmodel");
    if (!cg_gun_z) cg_gun_z = _Dvar_RegisterFloat("cg_gun_z", 0.0, -3.4028235e38, 3.4028235e38, 0x80u, "z position of the viewmodel");
}

static void H5W_Init()
{
    if (g_h5wInit) return;
    g_h5wInit = true;
    // CG_Init (cg_main_mp.cpp): cgArray, cg_viewModelArray y cg_weaponsArray se reservan en el hunk de cliente. En LP64 los tamanos
    // son sizeof (weaponInfo_s 64 bytes, no 36; 2048 armas como los 0x12000 bytes del original).
    cgArray = (cg_s *)calloc(1, sizeof(cg_s));
    cg_viewModelArray = (ViewModelInfo *)calloc(1, sizeof(ViewModelInfo));
    cg_weaponsArray[0] = (weaponInfo_s *)calloc(2048, sizeof(weaponInfo_s));
    g_h5wSkelBuf = (char *)H5_AlignedAlloc(16, 262144);
    H5W_RegisterDvars();
    cgArray->resetWeaponStateFlag = 1;
    fprintf(stderr, "[H5W] viewmodel: cg_s %zu bytes, ViewModelInfo %zu, weaponInfo_s %zu\n", sizeof(cg_s), sizeof(ViewModelInfo), sizeof(weaponInfo_s));
}

// CL_DObjCreateSkelForBones (client_mp/cl_pose_mp.cpp) con un bufer propio en lugar del de clients[]: mismo contrato.
static DObjAnimMat *H5W_CreateSkel(DObj *obj, int *partBits, bool *upToDate)
{
    if (DObjSkelExists(obj, g_h5wSkelStamp)) { *upToDate = DObjSkelAreBonesUpToDate(obj, partBits); return I_dmaGetDObjSkel(obj); }
    const int len = (DObjGetAllocSkelSize(obj) + 15) & ~15;
    if (g_h5wSkelPos + len > 262144) return nullptr;
    char *buf = g_h5wSkelBuf + g_h5wSkelPos; g_h5wSkelPos += len;
    DObjCreateSkel(obj, buf, g_h5wSkelStamp);
    *upToDate = false;
    return (DObjAnimMat *)buf;
}

// Un frame de cliente del viewmodel (lo que hacen CG_DrawActiveFrame -> CG_UpdateViewWeaponAnim y CG_AddViewWeapon).
static DObj *H5W_Update()
{
    gentity_s *pl = &g_entities[0];
    if (!pl->r.inuse || !pl->client) return nullptr;
    H5W_Init();
    cg_s *cg = CG_GetLocalClientGlobals(0);
    const playerState_s *sps = &pl->client->ps;
    const bool first = g_h5wLastTime < 0;
    // H5f: con el bucle de juego (h5_play.cpp) el cliente corre por frame de RENDER: cg->time es el tiempo de servidor interpolado
    // (como cg.time entre dos snapshots) y la vista la camara interpolada; asi el XAnim del viewmodel avanza en todos los frames.
    const int now = g_h5wViewOverride ? g_h5wViewTime : level.time;
    cg->frametime = first ? 50 : now - g_h5wLastTime;
    if (cg->frametime < 0) cg->frametime = 0;
    if (!g_h5wViewOverride && cg->frametime <= 0) cg->frametime = 1;
    g_h5wLastTime = now;
    cg->time = now;
    memcpy(&cg->predictedPlayerState, sps, sizeof(playerState_s));
    playerState_s *ps = &cg->predictedPlayerState;
    if (g_h5wViewOverride)
    {
        for (int j = 0; j < 3; ++j) ps->origin[j] = g_h5wViewOrg[j] - (j == 2 ? ps->viewHeightCurrent : 0.0f);
        ps->viewangles[0] = g_h5wViewAng[0]; ps->viewangles[1] = g_h5wViewAng[1]; ps->viewangles[2] = 0.0f;
    }
    // CG_CalcViewValues: vista en el ojo del jugador con sus angulos; viewModelAxis = ejes de la vista + origen (cg_camera.cpp:734)
    cg->refdef.vieworg[0] = ps->origin[0]; cg->refdef.vieworg[1] = ps->origin[1]; cg->refdef.vieworg[2] = ps->origin[2] + ps->viewHeightCurrent;
    cg->refdefViewAngles[0] = ps->viewangles[0]; cg->refdefViewAngles[1] = ps->viewangles[1]; cg->refdefViewAngles[2] = ps->viewangles[2];
    AnglesToAxis(cg->refdefViewAngles, cg->refdef.viewaxis);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) cg->viewModelAxis[i][j] = cg->refdef.viewaxis[i][j];
    for (int j = 0; j < 3; ++j) cg->viewModelAxis[3][j] = cg->refdef.vieworg[j];
    cg->xyspeed = sqrtf(ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1]);
    if (ps->pm_type >= 9 || BG_GetViewmodelWeaponIndex(ps) <= 0) return nullptr;
    { extern void H7C_UpdateHands(const playerState_s *) __attribute__((weak)); if (H7C_UpdateHands) H7C_UpdateHands(ps); }   // OLA 7c: CG_UpdateHandViewmodels (brazos del personaje); OLA E3b: antes de la animacion, como en el snapshot
    CG_UpdateViewWeaponAnim(0, first ? 1 : 0);
    cg->prevViewmodelWeapon = BG_GetViewmodelWeaponIndex(ps);   // CG_DrawActiveFrame lo guarda tras la animacion
    ViewModelInfo *vmi = CG_GetLocalClientViewModelInfo(0);
    DObj *obj = vmi->viewModelDObj;
    if (!obj) return nullptr;
    // CG_AddViewWeapon
    weaponState_t ws; GfxScaledPlacement placement; float angles[3];
    CalculateWeaponPostion_PositionToADS(cg, ps);
    CG_BuildWeaponState(cg, &ws);
    BG_CalculateWeaponMovement(&ws, placement.base.origin, angles);
    CG_CalculateWeaponMovement_ClientSpecific(cg, &placement, angles);
    CG_SaveWeaponState(&ws, cg);
    // CG_AddPlayerWeapon (rama bViewModel)
    UnitQuatToAxis(placement.base.quat, cg->viewModelAxis);
    for (int j = 0; j < 3; ++j) cg->viewModelAxis[3][j] = placement.base.origin[j];
    CG_UpdateViewModelPose(obj, 0);
    return obj;
}

// Esqueleto (CG_DObjCalcPose sin controladores) y dibujo de cada superficie con la pose del viewmodel.
static void H5W_DrawViewModel(unsigned char techType)
{
    if (g_h5wPrevDraw) g_h5wPrevDraw(techType);
    { void H4_DrawTranslucentOnce(unsigned char); H4_DrawTranslucentOnce(techType); }   // OLA 15B: cristal translucido antes del depth hack
    g_h5wSurfs = 0;
    DObj *obj = H5W_Update();
    if (!obj) return;
    cg_s *cg = CG_GetLocalClientGlobals(0);
    ++g_h5wSkelStamp; g_h5wSkelPos = 0;
    int partBits[5] = { -1, -1, -1, -1, -1 };
    bool upToDate = false;
    DObjAnimMat *mats = H5W_CreateSkel(obj, partBits, &upToDate);
    if (!mats) return;
    if (!upToDate) { DObjCompleteHierarchyBits(obj, partBits); DObjCalcSkel(obj, partBits); }
    mats = I_dmaGetDObjSkel(obj);
    // pose del viewmodel -> mundo (viewModelAxis, igual que el eje + origen de la entidad en el puente de actores)
    DObjSkelMat poseMat; memset(&poseMat, 0, sizeof poseMat);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) poseMat.axis[i][j] = cg->viewModelAxis[i][j];
    for (int j = 0; j < 3; ++j) poseMat.origin[j] = cg->viewModelAxis[3][j]; poseMat.origin[3] = 1.0f;
    // depth hack del viewmodel: el arma va delante de todo lo ya dibujado
    { float clr[4] = { 0, 0, 0, 0 }; R_ClearScreen(gfxCmdBufContext.state->prim.device, 2, clr, 1.0f, 0, 0); }
    { void H5L_Bind(int); H5L_Bind(-1); }   // OLA 6b: luz del viewmodel (lightgrid en el ojo)
    const int numModels = DObjGetNumModels(obj);
    int boneIndex = 0;
    for (int m = 0; m < numModels; ++m)
    {
        XModel *model = DObjGetModel(obj, m);
        const int nb = XModelNumBones(model);
        if (boneIndex + nb > 256) break;
        const DObjAnimMat *base = XModelGetBasePose(model);
        for (int j = 0; j < nb; ++j)
        {
            DObjSkelMat inv, bone, tmp;
            ConvertQuatToInverseSkelMat(&base[j], &inv);
            ConvertQuatToSkelMat(&mats[boneIndex + j], &bone);
            R_MultiplySkelMat(&inv, &bone, &tmp);
            R_MultiplySkelMat(&tmp, &poseMat, &g_h5wSkel[boneIndex + j]);
            DObjSkelMat &o = g_h5wSkel[boneIndex + j];
            o.axis[0][3] = o.axis[1][3] = o.axis[2][3] = 0.0f; o.origin[3] = 1.0f;
        }
        XSurface *surfs; const int ns = XModelGetSurfaces(model, &surfs, 0);
        PTR32(Material) *skins = XModelGetSkins(model, 0);
        for (int si = 0; si < ns; ++si)
        {
            XSurface *xs = &surfs[si];
            Material *mtl = skins ? skins[si].get() : nullptr;
            if (!mtl || !xs->vertCount) continue;
            { extern bool H7C_SurfHidden(const DObj *, const XSurface *, int) __attribute__((weak)); if (H7C_SurfHidden && H7C_SurfHidden(obj, xs, boneIndex)) continue; }   // OLA 7c: hideTags (R_SkinXModel)
            if (xs->vertCount > g_h5wVertCap) { free(g_h5wVerts); g_h5wVertCap = xs->vertCount + 1024; g_h5wVerts = (GfxPackedVertex *)H5_AlignedAlloc(16, ((g_h5wVertCap * sizeof(GfxPackedVertex)) + 15) & ~15u); }
            R_SkinXSurfaceSkinned(xs, &g_h5wSkel[boneIndex], g_h5wVerts);
            if (g_h5wLogged < 8) { ++g_h5wLogged; fprintf(stderr, "[H5W] viewmodel modelo %d '%s' superficie %d verts=%u material '%s' v0=(%.1f,%.1f,%.1f)\n", m, model->name.get(), si, xs->vertCount, mtl->info.name.get(), g_h5wVerts[0].xyz[0], g_h5wVerts[0].xyz[1], g_h5wVerts[0].xyz[2]); }
            H4_DrawSkinnedSurf(mtl, xs, g_h5wVerts, techType);
            ++g_h5wSurfs;
        }
        boneIndex += nb;
    }
}

// h5_game.cpp, tras instalar el puente de actores: el viewmodel se dibuja despues (H5_VIEWMODEL=1; por defecto con
// H5_VIEW_CAM=player o H5_PLAY=1 salvo H5_PLAY_CAM=chase).
void H5W_InstallViewModel()
{
    const char *cam = getenv("H5_VIEW_CAM"), *pcam = getenv("H5_PLAY_CAM");   // H5_PLAY_CAM=first: bucle unificado de H5d
    const bool on = getenv("H5_VIEWMODEL") ? atoi(getenv("H5_VIEWMODEL")) != 0
                                           : ((cam && !strcmp(cam, "player")) || (getenv("H5_PLAY") && !(pcam && !strcmp(pcam, "chase"))));
    if (!on) return;
    g_h5wPrevDraw = g_h4DrawExtra;
    g_h4DrawExtra = H5W_DrawViewModel;
    fprintf(stderr, "[H5W] viewmodel activado (puente de skinning)\n");
}
