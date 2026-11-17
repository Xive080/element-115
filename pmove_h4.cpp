// H4: pegamento minimo para mover al jugador con el codigo ORIGINAL de BO1 (bgame/bg_pmove.cpp, bg_slidemove.cpp, bg_jump.cpp,
// bg_mantle.cpp, bg_dtp.cpp, bg_misc.cpp). Aqui solo hay:
//  - lo que en el juego pone el servidor antes de Pmove (ClientThink_real en game_mp/g_active_mp.cpp): ps.speed=g_speed,
//    ps.gravity=bg_gravity, tracemask, handler=1 (servidor), moveSpeedScaleMultiplier=1;
//  - la traza del servidor sin entidades: G_TraceCapsule = SV_TraceCapsule sin el recorte contra entidades (CM_BoxTrace + hitType);
//  - arma: el WeaponDef REAL de la zona (m1911_zm, el arma inicial de zombies) para moveSpeedScale/sprintDurationScale;
//  - no-ops de lo que no cambia el movimiento: animaciones (BG_Anim*), armas (PM_Weapon, ADS, dispersion), cristal, eventos.
// Lo no implementado sigue siendo un stub que aborta con su nombre (genstubs.py).
#ifdef H5G
#define H5W __attribute__((weak)) // H5G: el servidor ORIGINAL se enlaza entero y su TU real gana a este pegamento
#else
#define H5W
#endif

#include "bgame/bg_pmove.h"
#include "bgame/bg_public.h"
#include "bgame/bg_local.h"
#include "bgame/bg_misc.h"
#include "bgame/bg_animation.h"
#include "bgame/bg_weapons.h"
#include "bgame/bg_weapons_def.h"
#include "bgame/bg_jump.h"
#include "bgame/bg_mantle.h"
#include "bgame/bg_dtp.h"
#include <cgame_mp/cg_predict_mp.h>
#include <qcommon/cm_mapkit.h>
#include <server/sv_world.h>
#include <glass/glass_server.h>
#include <database/db_registry.h>
#include <universal/dvar.h>
#include <universal/ptr32.h>
#include <qcommon/cm_trace.h>
#include <qcommon/com_bsp.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "pmove_h4.h"

// ---- datos globales que el motor define en ficheros no compilados aqui
H5W thread_local bgs_t *bgs;                         // bg_animation.cpp (solo lo leen las ramas de animacion/footsteps)
pmove_t cg_pmove[1];                             // cg_predict_mp.cpp (lo nombra destroy_client_gjkcc_info)
const dvar_s *zombiemode;                        // qcommon/common.cpp
const dvar_s *com_maxclients;                    // qcommon/common.cpp
const dvar_s *zombietron;                        // qcommon/common.cpp
const dvar_s *phys_player_collision_mode;        // physics/phys_main.cpp
const dvar_s *render_player_collision;           // physics/phys_main.cpp
const dvar_s *phys_debugExpensivePushout;        // physics/phys_main.cpp
const dvar_s *enable_new_prone_check;            // physics/phys_main.cpp
H5W const dvar_s *bg_moonGravity;                    // game_sp/actor_sp_ext.cpp
static const dvar_s *g_speed;                    // game_mp/g_main_mp.cpp (alli es global; aqui solo lo lee este fichero)

pmove_t::pmove_t() {}
// H5f: el constructor de copia de cg_predict_mp.cpp (ese TU no compila en LP64, casts de puntero en 418-420), copiado literal.
// Lo usa reviveplayer (g_scr_sp_players.cpp, revivir solo con Quick Revive).
pmove_t::pmove_t(const pmove_t *__that)
{
    colgeom_visitor_inlined_t<200> *p_proximity_data; // [esp+10h] [ebp-4h]

    this->ps = __that->ps;
    memcpy(&this->cmd, &__that->cmd, sizeof(this->cmd));
    memcpy(&this->oldcmd, &__that->oldcmd, 0x3Cu);
    memcpy(this->touchents, __that->touchents, 0x84u);
    memcpy(this->touchGlasses, __that->touchGlasses, 0xA8u);
    memcpy(this->pitchHistory, __that->pitchHistory, 0x81u);
    this->mantleEndPos[0] = __that->mantleEndPos[0];
    this->mantleEndPos[1] = __that->mantleEndPos[1];
    this->mantleEndPos[2] = __that->mantleEndPos[2];
    this->mantleDuration = __that->mantleDuration;
    this->viewChangeTime = __that->viewChangeTime;
    this->viewChange = __that->viewChange;
    this->vehicleAngles[0] = __that->vehicleAngles[0];
    this->vehicleAngles[1] = __that->vehicleAngles[1];
    this->vehicleAngles[2] = __that->vehicleAngles[2];
    this->vehAnimState = __that->vehAnimState;
    this->handler = __that->handler;
    this->localClientNum = __that->localClientNum;
    this->m_gjkcc_input = __that->m_gjkcc_input;
    p_proximity_data = &this->proximity_data;
    //colgeom_visitor_t::colgeom_visitor_t(&this->proximity_data, &__that->proximity_data);
    //p_proximity_data->__vftable = (colgeom_visitor_inlined_t<200>_vtbl *)&colgeom_visitor_inlined_t<200>::`vftable';
    p_proximity_data->nprims = __that->proximity_data.nprims;
    p_proximity_data->overflow = __that->proximity_data.overflow;
    memcpy(p_proximity_data->prims, __that->proximity_data.prims, sizeof(p_proximity_data->prims));
    this->spDisabledStances = __that->spDisabledStances;
    //return this;
}

// ---- mapkit (rt2.cpp tiene el resto: un mapa de zona no tiene cajas de mapkit)
H5W int CM_Mapkit_BoxCount() { return 0; }
H5W const cbrush_t *CM_Mapkit_GetBrush(int) { return nullptr; }

// ---- manejador del servidor (pmoveHandlers[1], bg_public.h)
H5W void G_TraceCapsule(trace_t *results, const float *start, const float *mins, const float *maxs, const float *end, int passEntityNum, int contentmask, col_context_t *context)
{
    // SV_TraceCapsule (server/sv_world.cpp) sin SV_ClipMoveToEntities: en H4 no hay entidades (zombis, puertas...) que recortar
    (void)passEntityNum;
    context->mask = contentmask;
    CM_BoxTrace(results, start, end, mins, maxs, contentmask, context);
    if (results->fraction == 1.0f) { results->hitType = TRACE_HITTYPE_NONE; results->hitId = 0; }
    else { results->hitType = TRACE_HITTYPE_ENTITY; results->hitId = 1022; }   // ENTITYNUM_WORLD
}
H5W void G_PlayerEvent(int, int) {}
H5W bool G_GetEntityOriginAngles(int, int, float *, float *) { return false; }
H5W unsigned short G_GetVehicleTypeString(int, int) { return 0; }
H5W void G_EntityLinkFromPMove(unsigned int, int, int) {}
H5W int G_GetPlayerWeapon(const playerState_s *ps, int) { return ps->weapon; }
void G_SetVehDriverInputs(int, int, usercmd_s *) {}
H5W bool G_IsEntWalkable(int, int) { return true; }   // el mundo (1022) siempre; no hay otras entidades
H5W void GlassSv_ClipMoveTrace(const moveclip_t *, trace_t *) {}   // sin cristales rompibles en H4

// ---- agua: R_WaterSimulationGetHeight (gfx_d3d/r_water_sim.cpp, no compilado) sin simulacion activa = R_WaterSimulationGetHeightGlobal,
// que busca en las celdas de agua del ComWorld. Si la zona no tiene celdas (zombie_pentagon) devuelve -131072 como el original.
static const ComWorld *g_comWorld;
static void EnumComWorld(XAssetHeader h, void *) { if (!g_comWorld) g_comWorld = h.comWorld.get(); }
double R_WaterSimulationGetHeight(const float *)
{
    static bool once; if (!once) { once = true; DB_EnumXAssets_FastFile(ASSET_TYPE_COMWORLD, EnumComWorld, 0, false);
        fprintf(stderr, "[PM] ComWorld %s: celdas de agua=%u%s\n", g_comWorld ? "si" : "no", g_comWorld ? g_comWorld->numWaterCells : 0u, g_comWorld && g_comWorld->numWaterCells ? " (AVISO: agua no portada, se ignora)" : ""); }
    return -131072.0;
}

// el Pmove de H4 hace de servidor (handler 1, como ClientThink_real); PM_Footsteps/BG_CheckThread preguntan en que hilo estan
bool Sys_IsServerThread() { return true; }

// ---- Sys_SnapVector (win32/win_shared.cpp): fld/fistp = redondeo al entero mas cercano (modo por defecto de la FPU)
void Sys_SnapVector(float *v) { v[0] = (float)lrintf(v[0]); v[1] = (float)lrintf(v[1]); v[2] = (float)lrintf(v[2]); }

// ---- armas: indice 0 = "none" (todo a cero, como bg_weaponVariantDefs[0]); indice 1 = WeaponDef real de la zona
static WeaponDef *g_noneDef; static const WeaponDef *g_wDef; static const WeaponVariantDef *g_wVar;
static const char *g_wantWeap;
static void EnumWeap(XAssetHeader h, void *)
{
    const WeaponVariantDef *v = h.weapon.get();
    if (!v || g_wVar) return;
    const char *n = v->szInternalName.get();
    if (n && !strcmp(n, g_wantWeap)) g_wVar = v;
}
H5W const WeaponDef *BG_GetWeaponDef(unsigned int weaponIndex) { return (weaponIndex == 1 && g_wDef) ? g_wDef : g_noneDef; }
H5W const WeaponVariantDef *BG_GetWeaponVariantDef(unsigned int weaponIndex) { return weaponIndex == 1 ? g_wVar : nullptr; }
H5W bool BG_UsingSniperScope(const playerState_s *) { return false; }
H5W void PM_ExitAimDownSight(playerState_s *) {}
H5W void PM_ResetWeaponState(playerState_s *) {}
H5W bool BG_WeaponBlocksProne(unsigned int) { return false; }
H5W bool BG_IsBayonetWeapon(unsigned int) { return false; }
H5W bool BG_IsUseAsMeleeWeapon(unsigned int) { return false; }
H5W int PM_WeaponAmmoAvailable(playerState_s *) { return 1; }
H5W bool BG_PlayerWeaponOverheating(const playerState_s *, unsigned int) { return false; }
H5W bool PM_IsWeaponMinFireTimeElapsed(playerState_s *, bool) { return false; }
H5W int PM_InteruptWeaponWithProneMove(playerState_s *) { return 0; }
H5W void PM_AdjustAimSpreadScale(pmove_t *, pml_t *) {}
H5W void PM_UpdateAimDownSightFlag(pmove_t *, pml_t *) {}
H5W void PM_UpdateAimDownSightLerp(pmove_t *, pml_t *) {}
H5W void PM_UpdateSpinLerp(pmove_t *, pml_t *) {}
H5W void PM_Weapon(pmove_t *, pml_t *) {}
H5W void PM_Weapon_Turret(pmove_t *, pml_t *) {}

// ---- animacion del cuerpo (no hay modelo de jugador en H4): sin animacion = -1 / nada
H5W int BG_PlayAnim(playerState_s *, int, animBodyPart_t, int, int, int, int) { return -1; }
H5W int BG_AnimScriptAnimation(pmove_t *, aistateEnum_t, scriptAnimMoveTypes_t, int) { return -1; }
H5W animScriptCommand_t *BG_AnimScriptEventGetCommand(playerState_s *, scriptAnimEventTypes_t) { return nullptr; }
H5W int BG_AnimScriptEvent(pmove_t *, scriptAnimEventTypes_t, int, int) { return -1; }
H5W void BG_AnimUpdatePlayerStateConditions(pmove_t *) {}
// registros de dvars que BG_RegisterDvars llama y que no tocan al jugador a pie (vehiculos, perros, viento, lanzallamas)
H5W void VehAnim_RegisterDvars() {}
H5W void Dog_RegisterDvars() {}
H5W void BG_Wind_RegisterDvars() {}
H5W void BG_Flame_RegisterDvars() {}

// ---- estado persistente del jugador
static playerState_s *g_ps; static pmove_t *g_pm; static usercmd_s g_oldcmd; static int g_serverTime; static bool g_ready;

static void InitOnce()
{
    if (g_ready) return;
    // qcommon/common.cpp (Com_Init) y Com_LoadLevelFastFiles: zombiemode=1 en un mapa de zombis -> defaults de SP en BG_RegisterDvars
    zombiemode = _Dvar_RegisterBool("zombiemode", 0, 0x40u, "Current game is an zombie game");
    Dvar_SetBool((dvar_s *)zombiemode, true);
    zombietron = _Dvar_RegisterBool("zombietron", 0, 0x40u, "Current game is an zombietron top down game");
    com_maxclients = _Dvar_RegisterInt("com_maxclients", 18, 1, 32, 0x44u, "Maximum amount of clients on the server");
    g_speed = _Dvar_RegisterInt("g_speed", 190, 0x80000000, 0x7FFFFFFF, 0, "Player speed");
    // physics/phys_main.cpp: el original usa la colision GJK (modo 1). Esa pila (phys_gjk*, caches AVL) no esta portada: modo 0
    // = la ruta CM_BoxTrace del propio motor (PM_playerTrace -> pmoveHandlers.trace), seleccionable por este mismo dvar.
    phys_player_collision_mode = _Dvar_RegisterInt("phys_player_collision_mode", 1, 0, 1, 0, "phys player collision mode");
    Dvar_SetInt((dvar_s *)phys_player_collision_mode, 0);
    render_player_collision = _Dvar_RegisterInt("render_player_collision", 0, 0, 1, 0, "render player collision");
    phys_debugExpensivePushout = _Dvar_RegisterBool("phys_debugExpensivePushout", 0, 0, "Draw debug info when the engine resort to exensive Player/AI pushouts");
    enable_new_prone_check = _Dvar_RegisterInt("enable_new_prone_check", 1, 0, 1, 0x80u, "enable new prone check");   // SP: 1 (BG_UpdateZombieDvarDefaults)
    bg_moonGravity = _Dvar_RegisterFloat("bg_moonGravity", 136.0f, 0.0f, 3.4028235e38f, 0x80u, "Gravity on the moon");
    BG_RegisterDvars();
    bgs = (bgs_t *)Ptr32_ArenaAlloc(sizeof(bgs_t), 64);
    g_noneDef = (WeaponDef *)Ptr32_ArenaAlloc(sizeof(WeaponDef), 64);
    g_wantWeap = getenv("H4_WEAPON") ? getenv("H4_WEAPON") : "m1911_zm";
    DB_EnumXAssets_FastFile(ASSET_TYPE_WEAPON, EnumWeap, 0, false);
    g_wDef = g_wVar ? g_wVar->weapDef.get() : nullptr;
    if (g_wDef) fprintf(stderr, "[PM] arma '%s': moveSpeedScale=%.3f adsMoveSpeedScale=%.3f sprintDurationScale=%.3f\n", g_wantWeap, g_wDef->moveSpeedScale, g_wDef->adsMoveSpeedScale, g_wDef->sprintDurationScale);
    else fprintf(stderr, "[PM] arma '%s' NO esta en las zonas cargadas: sin arma (indice 0, escala 1, sin sprint)\n", g_wantWeap);
    g_ps = (playerState_s *)Ptr32_ArenaAlloc(sizeof(playerState_s), 64);
    g_pm = new (Ptr32_ArenaAlloc(sizeof(pmove_t), 64)) pmove_t();
    g_ready = true;
}

bool H4Pm_Spawn(const float *feet, float yaw)
{
    InitOnce();
    playerState_s *ps = g_ps;
    memset(ps, 0, sizeof *ps);
    // lo que dejan ClientSpawn/ClientEndFrame (game_mp): de pie, en el suelo por decidir, ojos a 60
    ps->pm_type = 0; ps->clientNum = 0; ps->groundEntityNum = 1023;
    ps->origin[0] = feet[0]; ps->origin[1] = feet[1]; ps->origin[2] = feet[2];
    ps->viewHeightTarget = 60; ps->viewHeightCurrent = 60.0f;
    ps->moveSpeedScaleMultiplier = 1.0f;
    ps->weapon = g_wDef ? 1 : 0;
    ps->viewangles[1] = yaw;
    ps->commandTime = g_serverTime;
    memset(&g_oldcmd, 0, sizeof g_oldcmd); g_oldcmd.serverTime = g_serverTime;
    return true;
}

static int Ang2Short(float a) { return ((int)(a * 65536.0f / 360.0f)) & 0xFFFF; }   // ANGLE2SHORT
static signed char Axis(float v) { int i = (int)lrintf(v * 127.0f); return (signed char)(i > 127 ? 127 : (i < -127 ? -127 : i)); }

static int g_forced = -1;
void H4Pm_Move(const H4PmCmd &c, int msec)
{
    g_forced = -1;
    if (!g_ready || msec <= 0) return;
    playerState_s *ps = g_ps; pmove_t *pm = g_pm;
    g_serverTime += msec;
    // ClientThink_real (game_mp/g_active_mp.cpp): gravedad y velocidad del jugador antes de cada Pmove
    ps->gravity = (int)lrintf(bg_gravity->current.value + 9.3e-10f);
    ps->speed = g_speed->current.integer;
    memset((void *)pm, 0, offsetof(pmove_t, proximity_data));
    pm->localClientNum = -1;
    pm->ps = ps;
    usercmd_s &cmd = pm->cmd;
    cmd.serverTime = g_serverTime;
    cmd.forwardmove = Axis(c.fwd); cmd.rightmove = Axis(c.strafe);
    // CL_FinishMove: cmd.angles = ANGLE2SHORT(viewangles - delta_angles); PM_UpdateViewAngles hace angulo = SHORT2ANGLE(cmd) + delta
    cmd.angles[0] = Ang2Short(c.pitch - ps->delta_angles[0]);
    cmd.angles[1] = Ang2Short(c.yaw - ps->delta_angles[1]);
    cmd.angles[2] = 0;
    cmd.weapon = ps->weapon;
    if (c.sprint) cmd.button_bits.setBit(1u);
    if (c.prone) cmd.button_bits.setBit(8u);
    if (c.crouch) cmd.button_bits.setBit(9u);
    if (c.jump) cmd.button_bits.setBit(10u);
    if (c.dive) cmd.button_bits.setBit(0x2Cu);
    memcpy(&pm->oldcmd, &g_oldcmd, sizeof pm->oldcmd);
    pm->tracemask = ps->pm_type < 9 ? 0x2818011 : 0x810011;
    pm->handler = 1;
    pm->spDisabledStances = 0;
    // Pmove() = setup_gjkcc_input + gjkcc_prolog (cache GJK, solo modo 1) + Pmove_1 + gjkcc_epilog. En modo 0 la cache no se usa.
    gjkcc_input_t gin; setup_gjkcc_input(pm, &gin); pm->m_gjkcc_input = &gin;
    int seq0 = ps->predictableEventSequence;
    Pmove_1(pm);
    // EV_STANCE_FORCE_STAND/CROUCH/PRONE (8/9/10): en el juego CG_EntityEvent -> CL_SetStance; aqui se devuelven a la capa de entrada
    for (int i = seq0; i != ps->predictableEventSequence; i = (short)(i + 1)) { int e = ps->predictableEvents[i & 3] & 0xFF; if (e >= 8 && e <= 10) g_forced = e - 8; }
    pm->m_gjkcc_input = nullptr;
    memcpy(&g_oldcmd, &pm->cmd, sizeof g_oldcmd);   // client->sess.oldcmd = *ucmd
}

void H4Pm_Get(H4PmState *s)
{
    s->forcedStance = g_forced;
    const playerState_s *ps = g_ps;
    memcpy(s->origin, ps->origin, 12); memcpy(s->velocity, ps->velocity, 12); memcpy(s->viewangles, ps->viewangles, 12);
    s->viewHeight = ps->viewHeightCurrent; s->pmFlags = ps->pm_flags;
    s->onGround = ps->groundEntityNum != 1023; s->sprinting = (ps->pm_flags & 0x8000) != 0;
    s->crouched = (ps->pm_flags & 2) != 0; s->prone = (ps->pm_flags & 1) != 0;
}

void H4Pm_Teleport(const float *o) { if (!g_ready) return; memcpy(g_ps->origin, o, 12); memset(g_ps->velocity, 0, 12); }

void H4Pm_PrintDvars()
{
    InitOnce();
    static const char *names[] = { "g_speed", "bg_gravity", "jump_height", "jump_stepSize", "jump_slowdownEnable", "friction", "stopspeed",
        "player_sprintSpeedScale", "player_sprintStrafeSpeedScale", "player_sprintForwardMinimum", "player_sprintMinTime", "player_sprintUnlimited",
        "player_backSpeedScale", "player_strafeSpeedScale", "player_view_pitch_up", "player_view_pitch_down", "bg_fallDamageMinHeight",
        "bg_fallDamageMaxHeight", "phys_player_collision_mode", "zombiemode", "mantle_enable" };
    for (const char *n : names) {
        const dvar_s *d = Dvar_FindVar(n);
        if (!d) { fprintf(stderr, "[PMDVAR] %-30s (no registrado)\n", n); continue; }
        switch (d->type) {
        case DVAR_TYPE_FLOAT: fprintf(stderr, "[PMDVAR] %-30s %g\n", n, d->current.value); break;
        case DVAR_TYPE_BOOL: fprintf(stderr, "[PMDVAR] %-30s %d\n", n, (int)d->current.enabled); break;
        default: fprintf(stderr, "[PMDVAR] %-30s %d\n", n, d->current.integer); break;
        }
    }
}
