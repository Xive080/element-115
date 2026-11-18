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
#include <math.h>
#include <server/sv_world.h>
#include <game_mp/g_combat_mp.h>
#include <game_mp/g_utils_mp.h>
#include <game_mp/g_misc_mp.h>
#include <clientscript/scr_const.h>
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
bool g_h5eViewResync = false;
int g_h5eUseUntil, g_h5eFireUntil; bool g_h5eShotReq;
namespace {
struct TimedAct { int t; std::string a; bool done; };
std::vector<TimedAct> s_timed; bool s_timedInit;
void H5E_Timed()
{
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
        if (!strcmp(cmd, "dvar")) { char k[96] = "", v[256] = ""; if (sscanf(ta.a.c_str(), "%*s %95s %255[^\n]", k, v) >= 1) { if (!Dvar_FindVar(k)) fprintf(stderr, "[H5E] guion: la dvar %s no existe\n", k); else Dvar_SetFromStringByName(k, v); } }
        else if (!strcmp(cmd, "tp") && pl->r.inuse && pl->client) { float o[3], a[3] = { 0, 0, 0 }; if (sscanf(ta.a.c_str(), "%*s %f %f %f %f %f", &o[0], &o[1], &o[2], &a[1], &a[0]) >= 4) { TeleportPlayer(pl, o, a); g_h5eViewResync = true; } }
        else if (!strcmp(cmd, "tpl") && pl->r.inuse && pl->client) {   // tpl X Y Z TX TY TZ: al suelo bajo (X,Y,Z) mirando al punto T
            float o[3], tg[3]; if (sscanf(ta.a.c_str(), "%*s %f %f %f %f %f %f", &o[0], &o[1], &o[2], &tg[0], &tg[1], &tg[2]) == 6) {
                const float mins[3] = { -15.0f, -15.0f, 0.0f }, maxs[3] = { 15.0f, 15.0f, 70.0f };
                const float from[3] = { o[0], o[1], o[2] + 32.0f }, to[3] = { o[0], o[1], o[2] - 400.0f };
                trace_t tr; col_context_t ctx; G_TraceCapsule(&tr, from, mins, maxs, to, pl->s.number, 0x2810011, &ctx);
                if (!tr.startsolid) o[2] = from[2] + (to[2] - from[2]) * tr.fraction + 1.0f;
                float dir[3] = { tg[0] - o[0], tg[1] - o[1], tg[2] - (o[2] + 60.0f) }, ang[3]; vectoangles(dir, ang); ang[2] = 0;
                TeleportPlayer(pl, o, ang); g_h5eViewResync = true;
                fprintf(stderr, "[H5E] guion: jugador en (%.0f,%.0f,%.0f)%s mirando (%.0f,%.0f,%.0f)\n", o[0], o[1], o[2], tr.startsolid ? " (SIN suelo: dentro de solido)" : "", tg[0], tg[1], tg[2]);
            } }
        else if (!strcmp(cmd, "use")) { int ms = 1500; sscanf(ta.a.c_str(), "%*s %d", &ms); g_h5eUseUntil = level.time + ms; }
        else if (!strcmp(cmd, "fire")) { int ms = 500; sscanf(ta.a.c_str(), "%*s %d", &ms); g_h5eFireUntil = level.time + ms; }
        else if (!strcmp(cmd, "shot")) g_h5eShotReq = true;
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
    CG_UpdateViewWeaponAnim(0, first ? 1 : 0);
    { extern void H7C_UpdateHands(const playerState_s *) __attribute__((weak)); if (H7C_UpdateHands) H7C_UpdateHands(ps); }   // OLA 7c: CG_UpdateHandViewmodels (brazos del personaje)
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
