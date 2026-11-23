// H5d/H5f: BUCLE DE JUEGO. La entrada del jugador (h5_input.h: ejes + botones, lo que dara la app con los toques) se convierte en
// el usercmd del jugador de prueba del servidor ORIGINAL como haria el cliente local (CL_CreateCmd -> CL_CmdButtons -> CL_FinishMove):
// movimiento/vista/postura por la capa de H4 (world_h4.cpp PlayerMove, que ya imita cl_input_mp.cpp) y los botones de arma/uso con
// los bits originales (H5P_CmdButtons, g_h5pCmdExtra). Ese usercmd entra por el SV_ClientThink ordinario de SV_UpdateBots (gancho
// g_h0BotCmdHook, sv_main_mp.cpp): Pmove, PM_Weapon, Player_UpdateActivate (compras), dano, puntos y rondas son el codigo y los GSC
// originales. La camara sale del playerState, interpolada entre los dos ultimos frames de servidor (sin prediccion: mismo proceso).
//   H5_PLAY=1          -> bucle de render a 60 Hz: entrada -> H4_Update -> SV_Frame(0, ms) -> H4_Frame cada frame
//   H5_PLAY_SECONDS    -> duracion (90 s de juego)
//   H5_PLAY_SCRIPT     -> 0 quieto | 1 (por defecto) guion de H5d (gira, anda, se aparta y mira al zombi mas cercano)
//                         | 2 PILOTO (H5f): la politica de pelea del arnes del mod (G_SP_HeadlessFightCommand: apunta, dispara, recarga,
//                           cuchillo, compra Quick Revive, retrocede) traducida a H5PlayerInput y metida por la MISMA ruta que la app
//   H5_PLAY_CAM        -> first (ojo del jugador, por defecto) | chase (detras del jugador, con su cuerpo dibujado)
//   H5_PLAY_SHOT       -> segundos entre capturas PNG (5; 0 = ninguna); tambien en cada golpe recibido (max 6) y en cada cambio de ronda
//   H5_HUD=0           -> sin HUD (por defecto hay HUD minimo cuando hay vista)
// Log [H5D]: cada segundo posicion/salud y zombis; [H5F]: ronda, zombis vivos y por salir, puntos, municion, compras, abatido/revivido.
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <server_mp/server_mp.h>
#include <server_mp/sv_main_mp.h>
#include <server_mp/sv_init_mp.h>
#include <server_mp/sv_bot_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <game_sp/g_sp_client.h>
#include <game_sp/g_sp_player_state.h>
#include <bgame/bg_weapons.h>
#include <bgame/bg_weapons_def.h>
#include <bgame/bg_misc.h>
#include <clientscript/cscr_compiler.h>
#include <clientscript/cscr_variable.h>
#include <clientscript/cscr_stringlist.h>
#include <qcommon/cm_trace.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_material.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pmove_h4.h"
#include "world_h4.h"
#include "h5_input.h"

extern bool (*g_h0BotCmdHook)(client_t *bot, usercmd_s *cmd);              // sv_main_mp.cpp
extern void (*g_h0EndLoadScriptsHook)(scriptInstance_t inst);              // cscr_main.cpp
extern bool (*g_h4PlayerSink)(const H4PmCmd &c, int msec, H4PmState *out);  // world_h4.cpp
extern void (*g_h4DrawExtra)(unsigned char techType);
extern int g_h5vActors, g_h5vSurfs; extern double g_h5vSkinMs, g_h5vDrawMs; extern bool g_h5vDrawPlayer;   // h5_game.cpp
extern bool g_h5wViewOverride; extern float g_h5wViewOrg[3], g_h5wViewAng[3]; extern int g_h5wViewTime;      // h5_weapon.cpp
extern bot_info_t botInfos[32];                                                                               // sv_bot_mp.cpp
// Completa el usercmd tras la entrada de movimiento (H5f: H5P_CmdButtons). Firma: (cliente, usercmd ya armado).
void (*g_h5pCmdExtra)(client_t *bot, usercmd_s *cmd);
void (*g_h5pInputProvider)(H5PlayerInput *in, float dt);
void (*g_h5pInputOverride)(H5PlayerInput *in, const float *camOrg, const float *camAng);   // OLA 15C (h5_audio_cl.cpp): entrada guionizada
// App (Android): se llama al final de cada frame de render (medidas de fps, CPU). Con proveedor de entrada el bucle va a tiempo REAL
// (dt medido, recortado a [1/240, 1/15] s) en lugar de 1/60 fijo: si el movil baja de 60 fps el juego no va a camara lenta.
void (*g_h5pPostFrame)();
// OLA 6a (h5_audio_cl.cpp): sonido, una vez por frame de render (eventos tras un frame de servidor, oyente = camara).
void (*g_h5pAudio)(bool serverFrame, const float *camOrg, const float *camAng);
// App: pausa (boton de menu). Con g_h5pPause no corre ni la entrada de movimiento ni SV_Frame: el mundo se sigue dibujando congelado.
bool g_h5pPause;
// App: controles tactiles dibujados sobre el juego (modo horizontal, sin capa DS). Se llama dentro de la lista 2D del HUD.
void (*g_h5pDrawTouch)(int W, int H);
// OLA 11 (h11_net.cpp): red del anfitrion, una vez por frame de render (registro, cola de simulacion de red). Nulo sin H11_HOST.
void (*g_h5pNetPump)();
// actorLocational{Mins,Maxs}{,Big} (cg_world.cpp) los define h5_weapon.cpp (H5e): sin ellos el melee de zombie no toca al jugador.

static H4PmCmd s_in; static bool s_haveIn;
static bool s_active;                  // el jugador del servidor esta jugando (tras ClientBegin/ClientSpawn)
static float s_prevOrg[3], s_curOrg[3], s_prevVh, s_curVh; static int s_curTime = -1;
static short s_evSeq; static int s_forced = -1;
static float s_sentPitch;

static gentity_s *H5P_Player(client_t **out = nullptr)
{
    for (int i = 0; i < com_maxclients->current.integer; ++i)
    {
        client_t *c = &svs.clients[i];
        if (c->header.state == CS_ACTIVE && c->bIsTestClient && c->gentity && c->gentity->client)
        {
            if (out) *out = c;
            return c->gentity;
        }
    }
    return nullptr;
}
static bool H5P_Playing(gentity_s *pl) { return pl && pl->client && pl->health > 0 && pl->client->sess.sessionState == SESS_STATE_PLAYING; }
static unsigned short Ang2Short(float a) { return (unsigned short)(((int)(a * 65536.0f / 360.0f)) & 0xFFFF); }   // ANGLE2SHORT
static signed char Axis(float v) { int i = (int)lrintf(v * 127.0f); return (signed char)(i > 127 ? 127 : (i < -127 ? -127 : i)); }

// ---------------------------------------------------------------------------------------------------------------
// H5f: botones -> usercmd. Bits de CL_CmdButtons (client_mp/cl_input_mp.cpp: kbutton 17 attack -> 0, 21 melee -> 2, 22 activate -> 3,
// 24 reload -> 5, 19 frag -> 14, 20 smoke -> 15; +sprint -> 1; ADS -> 11 como CL_AddCurrentStanceToCmd/IN_Speed_Throw y el arnes del mod).
int H5P_UsercmdBit(unsigned b)
{
    switch (b)
    {
    case H5IN_FIRE: return 0;
    case H5IN_SPRINT: return 1;
    case H5IN_MELEE: return 2;
    case H5IN_USE: return 3;
    case H5IN_RELOAD: return 5;
    case H5IN_ADS: return 11;
    case H5IN_FRAG: return 14;
    case H5IN_SMOKE: return 15;
    default: return -1;
    }
}
static H5PlayerInput s_pin;                  // entrada del frame de render actual
static unsigned s_prevButtons, s_latched;    // flancos de pulsacion retenidos hasta el siguiente usercmd (kbutton_t.wasPressed)
static unsigned s_latchedWeapon;             // peticion directa de arma retenida
static unsigned s_pendingWeapon;             // cgameUserCmdWeapon pedido y aun no levantado
static int s_lastBotWeapon;
static int s_weapSelectTime = -100000;

static void H5P_LatchInput(const H5PlayerInput &in)
{
    s_latched |= in.buttons & ~s_prevButtons;
    s_prevButtons = in.buttons;
    if (in.weaponRequest) s_latchedWeapon = in.weaponRequest;
}

// CycleWeapPrimary (cgame/cg_weapons.cpp) en su rama normal: siguiente ranura de heldWeapons con un arma primaria, con el retardo
// cg_weaponCycleDelay (0 en el original) y sin cambiar en last stand (pm_type 6/7) ni mientras se despliega (WeaponCycleAllowed).
static unsigned H5P_NextPrimary(const playerState_s *ps, unsigned from)
{
    if (ps->pm_type == 6 || ps->pm_type == 7 || ps->weaponstate == WEAPON_DEPLOYING) return 0;
    int slot = from ? BG_GetHeldWeaponSlot(ps, from) : -1;
    if (slot < 0) slot = 0;
    for (int k = 1; k <= 15; ++k)
    {
        const int s = (slot + k) % 15;
        const unsigned w = ps->heldWeapons[s].weapon;
        if (!w || w == from) continue;
        const WeaponDef *d = BG_GetWeaponDef(w);
        if (d && d->inventoryType == WEAPINVENTORY_PRIMARY) return w;
    }
    return 0;
}

static void H5P_CmdButtons(client_t *bot, usercmd_s *cmd)
{
    gentity_s *pl = bot->gentity;
    const playerState_s *ps = &pl->client->ps;
    const unsigned held = s_pin.buttons | s_latched;
    const unsigned list[] = { H5IN_FIRE, H5IN_ADS, H5IN_RELOAD, H5IN_MELEE, H5IN_USE, H5IN_FRAG, H5IN_SMOKE };
    for (unsigned b : list) if (held & b) cmd->button_bits.setBit((unsigned)H5P_UsercmdBit(b));
    { extern int g_h5eUseUntil, g_h5eFireUntil; if (level.time < g_h5eUseUntil) cmd->button_bits.setBit((unsigned)H5P_UsercmdBit(H5IN_USE));   // OLA M1: H5_TIMED
      if (level.time < g_h5eFireUntil) cmd->button_bits.setBit((unsigned)H5P_UsercmdBit(H5IN_FIRE)); }
    { extern int g_h5eBtnUntil[16]; for (unsigned b = 0; b < 16; ++b) if (level.time < g_h5eBtnUntil[b]) cmd->button_bits.setBit(b); }   // OLA E2b: jump/melee/frag
    // arma (cgameUserCmdWeapon): un SwitchToWeapon de script llega a un cliente de prueba como peticion de bot (SV_BotSwitchWeapon ->
    // botInfos[].weapon, lo que usa Bot_UpdateWeapon); la eleccion del jugador (weapnext / peticion directa) va despues y manda.
    const int req = botInfos[pl->s.number].weapon;
    if (req && req != s_lastBotWeapon) s_pendingWeapon = (unsigned)req;
    s_lastBotWeapon = req;
    if (s_latchedWeapon) s_pendingWeapon = s_latchedWeapon;
    { extern unsigned g_h5eWeapReq; if (g_h5eWeapReq) { s_pendingWeapon = g_h5eWeapReq; g_h5eWeapReq = 0; } }   // OLA E3a: guion "weap NOMBRE"
    if (held & H5IN_NEXTWEAPON && level.time - s_weapSelectTime >= 0)
    {
        const unsigned n = H5P_NextPrimary(ps, s_pendingWeapon ? s_pendingWeapon : ps->weapon);
        if (n) { s_pendingWeapon = n; s_weapSelectTime = level.time; }
    }
    if (s_pendingWeapon && (s_pendingWeapon == ps->weapon || !BG_PlayerHasWeapon(ps, s_pendingWeapon))) s_pendingWeapon = 0;
    cmd->weapon = s_pendingWeapon ? s_pendingWeapon : (ps->weapon ? ps->weapon : bot->lastUsercmd.weapon);
    // OLA F1: un arma de modo alterno (microwavegun_zm de microwavegundw_zm) solo se queda si el usercmd nombra su primaria
    // (cl_input_mp: cmd->lastWeaponAltModeSwitch = cgameUserCmdLastWeaponForAlt); sin ella PM_Weapon_FinishWeaponChange la deja en 0
    if (cmd->weapon && BG_GetWeaponDef(cmd->weapon)->inventoryType == WEAPINVENTORY_ALTMODE)
        cmd->lastWeaponAltModeSwitch = ps->weapon && BG_GetWeaponDef(ps->weapon)->inventoryType != WEAPINVENTORY_ALTMODE ? ps->weapon : ps->lastWeaponAltModeSwitch;
    s_latched = 0; s_latchedWeapon = 0;
}

// CL_CreateCmd/CL_FinishMove del cliente local, con la entrada de H4: angulos = ANGLE2SHORT(vista - delta_angles) (PM_UpdateViewAngles
// hace vista = SHORT2ANGLE(cmd) + delta), ejes -127..127, botones con los bits de CL_CmdButtons/CL_AddCurrentStanceToCmd.
static bool H5P_BotCmd(client_t *bot, usercmd_s *cmd)
{
    client_t *me = nullptr; gentity_s *pl = H5P_Player(&me);
    if (!pl || bot != me || !s_haveIn) return false;
    const playerState_s *ps = &pl->client->ps;
    *cmd = bot->lastUsercmd;
    cmd->serverTime = svs.time;
    cmd->button_bits.array[0] = cmd->button_bits.array[1] = 0;
    cmd->forwardmove = Axis(s_in.fwd); cmd->rightmove = Axis(s_in.strafe);
    cmd->angles[0] = Ang2Short(s_in.pitch - ps->delta_angles[0]);
    cmd->angles[1] = Ang2Short(s_in.yaw - ps->delta_angles[1]);
    cmd->angles[2] = Ang2Short(0.0f - ps->delta_angles[2]);
    s_sentPitch = s_in.pitch;
    cmd->weapon = ps->weapon ? ps->weapon : bot->lastUsercmd.weapon;   // cgameUserCmdWeapon sigue al arma del playerState
    if (s_in.sprint) cmd->button_bits.setBit(1u);
    if (s_in.prone) cmd->button_bits.setBit(8u);
    if (s_in.crouch) cmd->button_bits.setBit(9u);
    if (s_in.jump) cmd->button_bits.setBit(10u);
    if (s_in.dive) cmd->button_bits.setBit(0x2Cu);
    if (g_h5pCmdExtra) g_h5pCmdExtra(bot, cmd);
    return true;
}

// Tras cada frame de servidor: guarda el estado para interpolar y recoge los EV_STANCE_FORCE_* (8/9/10) del Pmove del servidor
// (en el juego llegan al cliente por el snapshot -> CG_EntityEvent -> CL_SetStance).
static void H5P_AfterServerFrame()
{
    gentity_s *pl = H5P_Player();
    if (!pl) return;
    const playerState_s *ps = &pl->client->ps;
    if (!s_active && H5P_Playing(pl))
    {
        s_active = true; s_evSeq = ps->predictableEventSequence;
        memcpy(s_prevOrg, ps->origin, 12); s_prevVh = ps->viewHeightCurrent;
        fprintf(stderr, "[H5D] t=%d jugador del servidor activo en (%.1f,%.1f,%.1f) yaw %.1f: la entrada de H4 va a su usercmd\n", level.time,
            ps->origin[0], ps->origin[1], ps->origin[2], ps->viewangles[1]);
    }
    memcpy(s_prevOrg, s_curOrg, 12); s_prevVh = s_curVh;
    memcpy(s_curOrg, ps->origin, 12); s_curVh = ps->viewHeightCurrent;
    if (s_curTime < 0) { memcpy(s_prevOrg, s_curOrg, 12); s_prevVh = s_curVh; }
    s_curTime = svs.time;
    for (short i = s_evSeq; i != ps->predictableEventSequence; i = (short)(i + 1))
    {
        const int e = ps->predictableEvents[i & 3] & 0xFF;
        if (e >= 8 && e <= 10) s_forced = e - 8;
    }
    s_evSeq = ps->predictableEventSequence;
}

static float H5P_Frac()
{
    const float frameMsec = 1000.0f / (float)sv_fps->current.integer;
    float f = (float)sv.timeResidual / frameMsec; if (f < 0) f = 0; if (f > 1) f = 1;
    return f;
}

static bool H5P_Sink(const H4PmCmd &c, int msec, H4PmState *out)
{
    (void)msec;   // el tiempo lo lleva SV_Frame (usercmd por frame de servidor, serverTime = svs.time)
    gentity_s *pl = H5P_Player();
    memset(out, 0, sizeof *out); out->forcedStance = -1; out->viewHeight = 60.0f;
    if (!pl) { out->viewangles[0] = c.pitch; out->viewangles[1] = c.yaw; return false; }
    const playerState_s *ps = &pl->client->ps;
    // tras un TeleportPlayer del guion H5_TIMED (tp/tpl) manda la vista del servidor, como al empezar
    extern bool g_h5eViewResync;
    if (g_h5eViewResync) { g_h5eViewResync = false; s_haveIn = false; }
    const bool first = !s_haveIn && s_active;
    if (s_active) { s_in = c; s_haveIn = true; }
    // posicion: interpolada entre los dos ultimos frames de servidor (fraccion = sv.timeResidual / frameMsec)
    const float f = H5P_Frac();
    for (int i = 0; i < 3; ++i) out->origin[i] = s_prevOrg[i] + (s_curOrg[i] - s_prevOrg[i]) * f;
    out->viewHeight = s_prevVh + (s_curVh - s_prevVh) * f;
    memcpy(out->velocity, ps->velocity, 12);
    // vista: al empezar, la del servidor (ClientSpawn -> SetClientViewAngle); despues la pedida (la del cliente, como cl.viewangles),
    // con el limite de cabeceo del propio Pmove si el servidor la recorto. En last stand / controles congelados manda el servidor.
    if (first || !s_active || ps->pm_type >= 9) { memcpy(out->viewangles, ps->viewangles, 12); }
    else
    {
        out->viewangles[0] = c.pitch; out->viewangles[1] = c.yaw; out->viewangles[2] = 0;
        if (fabsf(AngleDelta(ps->viewangles[0], s_sentPitch)) > 0.5f && fabsf(AngleDelta(c.pitch, s_sentPitch)) < 1.0f) out->viewangles[0] = ps->viewangles[0];
    }
    out->pmFlags = ps->pm_flags;
    out->onGround = ps->groundEntityNum != 1023; out->sprinting = (ps->pm_flags & 0x8000) != 0;
    out->crouched = (ps->pm_flags & 2) != 0; out->prone = (ps->pm_flags & 1) != 0;
    out->forcedStance = s_forced; s_forced = -1;
    return true;
}

// ---- guion de entrada (Mac): solo produce lo que daria el mando; nunca escribe estado del juego
static double s_scriptT;   // segundos desde que el jugador esta activo
static void H5P_Script(H5PlayerInput &in, float dt, const float *camAng, int mode)
{
    memset(&in, 0, sizeof in);
    if (!s_active) return;
    if (!mode) { s_scriptT += dt; return; }   // OLA 15C: quieto tambien cuenta el tiempo (capturas H5_PLAY_SHOT con la camara fija)
    s_scriptT += dt;
    const double t = s_scriptT;
    if (t < 2.0) return;                                          // quieto
    if (t < 4.0) { in.lookYaw = 45.0f * dt; return; }            // gira 90 grados a la izquierda
    if (t < 7.0) { in.moveFwd = 1.0f; return; }                   // anda (el sprint automatico entra a los 1,2 s)
    if (t < 8.5) { in.moveRight = -0.7f; in.moveFwd = 0.3f; return; }   // se aparta en diagonal
    if (t < 9.0) { in.buttons = H5IN_STANCE; return; }            // toque de postura: agacharse
    if (t < 9.5) return;
    if (t < 10.0) { in.buttons = H5IN_STANCE; return; }           // otro toque: de pie
    // mira al zombi vivo mas cercano (gira la vista como lo haria el stick derecho, a 120 grados/s como maximo)
    gentity_s *pl = H5P_Player(); if (!pl) return;
    gentity_s *best = nullptr; float bd = 1e30f;
    for (int e = 0; e < level.num_entities; ++e)
    {
        gentity_s *a = &g_entities[e];
        if (!a->r.inuse || a->s.eType != ET_ACTOR || a->health <= 0) continue;
        float d = 0; for (int j = 0; j < 3; ++j) { float q = a->r.currentOrigin[j] - pl->r.currentOrigin[j]; d += q * q; }
        if (d < bd) { bd = d; best = a; }
    }
    if (!best) return;
    float dir[3], ang[3]; const float eye[3] = { pl->r.currentOrigin[0], pl->r.currentOrigin[1], pl->r.currentOrigin[2] + 60.0f };
    const float tgt[3] = { best->r.currentOrigin[0], best->r.currentOrigin[1], best->r.currentOrigin[2] + 40.0f };
    Vec3Sub(tgt, eye, dir); vectoangles(dir, ang);
    const float maxStep = 120.0f * dt;
    float dy = AngleDelta(ang[1], camAng[1]), dp = AngleDelta(ang[0], camAng[0]);
    in.lookYaw = dy > maxStep ? maxStep : (dy < -maxStep ? -maxStep : dy);
    in.lookPitch = dp > maxStep ? maxStep : (dp < -maxStep ? -maxStep : dp);
}

// ---- PILOTO (H5_PLAY_SCRIPT=2): la politica de pelea del arnes headless del mod (game_sp/g_sp_client.cpp G_SP_HeadlessFightCommand,
// la que llevo el exe x86 a la ronda 12) se ejecuta una vez por frame de servidor sobre un usercmd de usar y tirar, y su resultado se
// TRADUCE a H5PlayerInput (stick, giro de vista hasta el angulo pedido, botones, arma). Asi la partida guionizada pasa por la misma
// ruta de entrada que la app. La politica nunca escribe estado de juego (lo dice su propio comentario: solo campos del usercmd).
static bool s_pilotHave; static usercmd_s s_pilotCmd; static float s_pilotAng[2];
static void H5P_PilotServerFrame()
{
    client_t *me = nullptr; gentity_s *pl = H5P_Player(&me);
    s_pilotHave = false;
    if (!pl || !s_active) return;
    usercmd_s c = me->lastUsercmd;
    // Su traza de bala puede dar en otro jugador (co-op): G_DObjCalcPose -> G_PlayerController lee bgs, nulo fuera de G_RunFrame.
    bgs_t *const prevBgs = bgs; bgs = &level_bgs;
    const bool fight = G_SP_HeadlessFightCommand(me, &c);
    bgs = prevBgs;
    if (!fight) return;
    const playerState_s *ps = &pl->client->ps;
    s_pilotCmd = c; s_pilotHave = true;
    s_pilotAng[0] = AngleNormalize180((float)(unsigned short)c.angles[0] * (360.0f / 65536.0f) + ps->delta_angles[0]);
    s_pilotAng[1] = AngleNormalize360((float)(unsigned short)c.angles[1] * (360.0f / 65536.0f) + ps->delta_angles[1]);
}
static void H5P_Pilot(H5PlayerInput &in, const float *camAng)
{
    memset(&in, 0, sizeof in);
    if (!s_pilotHave) return;
    gentity_s *pl = H5P_Player(); if (!pl) return;
    const playerState_s *ps = &pl->client->ps;
    in.moveFwd = s_pilotCmd.forwardmove / 127.0f; in.moveRight = s_pilotCmd.rightmove / 127.0f;
    in.lookYaw = AngleDelta(s_pilotAng[1], camAng[1]); in.lookPitch = AngleDelta(s_pilotAng[0], camAng[0]);
    const unsigned list[] = { H5IN_FIRE, H5IN_ADS, H5IN_RELOAD, H5IN_MELEE, H5IN_USE, H5IN_FRAG, H5IN_SMOKE };
    for (unsigned b : list) if (s_pilotCmd.button_bits.testBit((unsigned)H5P_UsercmdBit(b))) in.buttons |= b;
    if (s_pilotCmd.weapon && s_pilotCmd.weapon != ps->weapon) in.weaponRequest = s_pilotCmd.weapon;
}

// ---------------------------------------------------------------------------------------------------------------
// [H5F] estado de la partida: ronda (level.round_number), zombis por salir (level.zombie_total), vivos, puntos, municion, salud.
// Los campos de script se leen como G_SP_MeasureFields: identificadores canonicos guardados al final de la carga, solo FindVariable.
static unsigned s_keyRound, s_keyTotal;
static void H5F_ScriptFields(scriptInstance_t inst)
{
    if (inst != SCRIPTINSTANCE_SERVER) return;
    unsigned s = SL_FindString("round_number", inst); s_keyRound = s ? gScrCompilePub[inst].canonicalStrings[s] : 0;
    s = SL_FindString("zombie_total", inst); s_keyTotal = s ? gScrCompilePub[inst].canonicalStrings[s] : 0;
}
void H5F_InstallHooks() { g_h0EndLoadScriptsHook = H5F_ScriptFields; }
static int H5F_LevelInt(unsigned key)
{
    if (!key) return -1;
    const unsigned id = FindVariable(SCRIPTINSTANCE_SERVER, gScrVarPub[SCRIPTINSTANCE_SERVER].levelId, key);
    if (!id || GetValueType(SCRIPTINSTANCE_SERVER, id) != VAR_INTEGER) return -1;
    return GetVariableValueAddress(SCRIPTINSTANCE_SERVER, id)->u.intValue;
}
struct H5FState { int round, total, alive, score, health, clip, stock, weapon, laststand, pmType, sessionState; bool qr; };
static H5FState H5F_Read()
{
    H5FState s; memset(&s, 0, sizeof s);
    s.round = H5F_LevelInt(s_keyRound); s.total = H5F_LevelInt(s_keyTotal);
    for (int e = 0; e < level.num_entities; ++e) { gentity_s *a = &g_entities[e]; if (a->r.inuse && a->s.eType == ET_ACTOR && a->health > 0) ++s.alive; }
    gentity_s *pl = H5P_Player();
    if (pl)
    {
        const playerState_s *ps = &pl->client->ps;
        s.score = pl->client->sess.cs.score.score; s.health = pl->health; s.weapon = ps->weapon;
        s.clip = ps->weapon ? BG_GetAmmoInClip(ps, ps->weapon) : 0; s.stock = ps->weapon ? BG_GetAmmoNotInClip(ps, ps->weapon) : 0;
        s.laststand = pl->client->lastStand; s.pmType = ps->pm_type; s.sessionState = pl->client->sess.sessionState;
        s.qr = G_SP_HasPerk(pl->client, "specialty_quickrevive");
    }
    return s;
}
static H5FState s_f; static bool s_fHave; static int s_fNextLog, s_fRoundShots, s_fKills, s_fDowns, s_fRevives, s_fBuys;
extern "C" void H15_MemPhase(const char *phase);   // OLA 15A: memguard.cpp (H15_MEMPROFILE)
static int s_fRoundStart[16], s_fMaxRound;
static bool s_fShotNow;
static void H5F_AfterServerFrame()
{
    if (!s_active) return;
    const H5FState n = H5F_Read();
    if (!s_fHave) { s_f = n; s_fHave = true; s_fNextLog = level.time; }
    if (n.round != s_f.round && n.round > 0)
    {
        fprintf(stderr, "[H5F] t=%d RONDA %d -> %d | puntos %d | vivos %d por salir %d | arma %s %d/%d salud %d\n", level.time, s_f.round, n.round, n.score,
            n.alive, n.total, n.weapon ? BG_WeaponName(n.weapon) : "-", n.clip, n.stock, n.health);
        if (n.round < 16) s_fRoundStart[n.round] = level.time;
        { static const char *kR[] = { "ronda 0", "ronda 1", "ronda 2", "ronda 3", "ronda 4", "ronda 5", "ronda 6", "ronda 7", "ronda 8+" }; H15_MemPhase(kR[n.round < 8 ? n.round : 8]); }   // OLA 15A
        if (n.round > s_fMaxRound) s_fMaxRound = n.round;
        s_fShotNow = true;
    }
    if (n.alive < s_f.alive && n.score > s_f.score) s_fKills += s_f.alive - n.alive;
    if (n.score < s_f.score && n.laststand && !s_f.laststand)
        // _zombiemode.gsc player_laststand: al caer, el script resta un porcentaje de los puntos (no es una compra)
        fprintf(stderr, "[H5F] t=%d penalizacion de last stand: puntos %d -> %d (-%d)\n", level.time, s_f.score, n.score, s_f.score - n.score);
    else if (n.score < s_f.score)
    {
        ++s_fBuys;
        fprintf(stderr, "[H5F] t=%d COMPRA: puntos %d -> %d (-%d) arma %s QR %d\n", level.time, s_f.score, n.score, s_f.score - n.score,
            n.weapon ? BG_WeaponName(n.weapon) : "-", (int)n.qr);
        s_fShotNow = true;
    }
    if (n.qr != s_f.qr) fprintf(stderr, "[H5F] t=%d perk Quick Revive %s\n", level.time, n.qr ? "COMPRADO" : "perdido");
    if (n.weapon != s_f.weapon) fprintf(stderr, "[H5F] t=%d arma %s -> %s (%d/%d)\n", level.time, s_f.weapon ? BG_WeaponName(s_f.weapon) : "-",
        n.weapon ? BG_WeaponName(n.weapon) : "-", n.clip, n.stock);
    if (n.laststand && !s_f.laststand) { ++s_fDowns; s_fShotNow = true; fprintf(stderr, "[H5F] t=%d ABATIDO (last stand) salud %d pm_type %d QR %d\n", level.time, n.health, n.pmType, (int)n.qr); }
    if (!n.laststand && s_f.laststand) { ++s_fRevives; s_fShotNow = true; fprintf(stderr, "[H5F] t=%d REVIVIDO salud %d pm_type %d\n", level.time, n.health, n.pmType); }
    if (n.sessionState != s_f.sessionState) fprintf(stderr, "[H5F] t=%d sessionState %d -> %d\n", level.time, s_f.sessionState, n.sessionState);
    if (level.time >= s_fNextLog)
    {
        s_fNextLog = level.time + 5000;
        fprintf(stderr, "[H5F] t=%d ronda %d | zombis vivos %d por salir %d | puntos %d | %s %d/%d | salud %d%s%s\n", level.time, n.round, n.alive, n.total,
            n.score, n.weapon ? BG_WeaponName(n.weapon) : "-", n.clip, n.stock, n.health, n.laststand ? " ABATIDO" : "", n.qr ? " QR" : "");
    }
    s_f = n;
}

// ---- HUD minimo (H5f). El HUD original de zombies son menus de UI con ownerdraws (puntos, municion) y hudelems del servidor (tiza de
// ronda): eso es la Ola 9. Mientras, se dibujan los MISMOS valores (playerState / level) con la fuente del juego (fonts/normalFont,
// R_AddCmdDrawText del renderer original) en la lista 2D que abre H4_Frame.
static Font_s *s_hudFont, *s_hudBig; static Material *s_hudWhite;
static void H5F_DrawHud(int W, int H)
{
    if (!s_hudFont) { s_hudFont = R_RegisterFont("fonts/normalFont", 3); s_hudBig = R_RegisterFont("fonts/bigFont", 3); if (!s_hudBig) s_hudBig = s_hudFont; s_hudWhite = Material_RegisterHandle("white", 3); }
    if (s_hudFont && g_h5pDrawTouch) g_h5pDrawTouch(W, H);
    if (!s_hudFont || !s_active) return;
    const H5FState &s = s_f;
    const float sc = (float)H / 720.0f;
    extern bool g_h10MenuOpen;   // OLA 10: con el menu de pausa original abierto no se dibuja el rotulo provisional
    extern bool H23_Editing() __attribute__((weak));   // OLA U2: en Customize HUD no hay rotulo de pausa
    if (g_h5pPause && !g_h10MenuOpen && !(H23_Editing && H23_Editing())) { const float w[4] = { 1, 1, 1, 1 }; R_AddCmdDrawText((char *)"PAUSA", 0x7fffffff, s_hudBig, W * 0.5f - 70 * sc, H * 0.5f - 40 * sc, 1.5f * sc, 1.5f * sc, 0, w, 3); }
    const float white[4] = { 1, 1, 1, 0.95f }, red[4] = { 0.9f, 0.12f, 0.1f, 1 }, gold[4] = { 1.0f, 0.85f, 0.3f, 1 }, shade[4] = { 0, 0, 0, 0.35f };
    char b[128];
    extern bool H7C_DrawHud(int, int, int, int, int, int) __attribute__((weak));   // OLA 7c: HUD con el arte original (h7c_gfx.cpp)
    extern bool H13_DrawHud(int, int) __attribute__((weak));   // OLA 13: HUD ORIGINAL (CG_Draw2D: menus, ownerdraws, hudelems; h13_hud.cpp)
    const bool h13 = H13_DrawHud && H13_DrawHud(W, H);
    const bool h7c = h13 || (H7C_DrawHud && H7C_DrawHud(W, H, s.round, s.score, s.clip, s.stock));
    if (!h7c) {
    // ronda (abajo a la izquierda, rojo como la tiza)
    snprintf(b, sizeof b, "%d", s.round > 0 ? s.round : 0);
    R_AddCmdDrawText(b, 0x7fffffff, s_hudBig, 30 * sc, H - 30 * sc, 2.2f * sc, 2.2f * sc, 0, red, 3);
    // puntos (derecha, como el marcador)
    snprintf(b, sizeof b, "%d", s.score);
    if (s_hudWhite) R_AddCmdDrawStretchPic(W - 210 * sc, H - 230 * sc, 190 * sc, 40 * sc, 0, 0, 1, 1, shade, s_hudWhite);
    R_AddCmdDrawText(b, 0x7fffffff, s_hudFont, W - 200 * sc, H - 200 * sc, 1.6f * sc, 1.6f * sc, 0, gold, 3);
    // municion (abajo a la derecha)
    snprintf(b, sizeof b, "%d / %d", s.clip, s.stock);
    R_AddCmdDrawText(b, 0x7fffffff, s_hudFont, W - 200 * sc, H - 60 * sc, 1.8f * sc, 1.8f * sc, 0, white, 3);
    snprintf(b, sizeof b, "%s", s.weapon ? BG_WeaponName(s.weapon) : "");
    R_AddCmdDrawText(b, 0x7fffffff, s_hudFont, W - 200 * sc, H - 30 * sc, 1.0f * sc, 1.0f * sc, 0, white, 3);
    }
    // estado (depuracion: con el HUD original de 7c solo con H7C_HUD_DEBUG=1)
    static const bool dbg = getenv("H7C_HUD_DEBUG") && atoi(getenv("H7C_HUD_DEBUG"));
    if (!h7c || dbg) {
    snprintf(b, sizeof b, "salud %d%s   zombis %d+%d", s.health, s.qr ? "   QR" : "", s.alive, s.total > 0 ? s.total : 0);
    R_AddCmdDrawText(b, 0x7fffffff, s_hudFont, 30 * sc, 40 * sc, 1.0f * sc, 1.0f * sc, 0, white, 3);
    }
    if (s.sessionState == SESS_STATE_INTERMISSION)   // game over del GSC (solo sin Quick Revive): camara de intermedio
    {
        if (!h13) {   // con el HUD original el fin de partida lo pintan los hudelems del GSC
        R_AddCmdDrawText((char *)"FIN DE LA PARTIDA", 0x7fffffff, s_hudBig, W * 0.5f - 200 * sc, H * 0.40f, 1.4f * sc, 1.4f * sc, 0, red, 3);
        snprintf(b, sizeof b, "ronda %d   puntos %d", s.round, s.score);
        R_AddCmdDrawText(b, 0x7fffffff, s_hudFont, W * 0.5f - 150 * sc, H * 0.40f + 50 * sc, 1.3f * sc, 1.3f * sc, 0, white, 3);
        }
        if (g_h5pInputProvider) R_AddCmdDrawText((char *)"MENU: otra partida", 0x7fffffff, s_hudFont, W * 0.5f - 140 * sc, H * 0.40f + 95 * sc, 1.2f * sc, 1.2f * sc, 0, gold, 3);
    }
    if (s.laststand && !h13) R_AddCmdDrawText((char *)"ABATIDO", 0x7fffffff, s_hudBig, W * 0.5f - 90 * sc, H * 0.35f, 1.5f * sc, 1.5f * sc, 0, red, 3);
    // mira
    if (s_hudWhite && s.pmType < 9 && !h13) { const float c[4] = { 1, 1, 1, 0.7f }; R_AddCmdDrawStretchPic(W * 0.5f - 1, H * 0.5f - 8 * sc, 2, 16 * sc, 0, 0, 1, 1, c, s_hudWhite); R_AddCmdDrawStretchPic(W * 0.5f - 8 * sc, H * 0.5f - 1, 16 * sc, 2, 0, 0, 1, 1, c, s_hudWhite); }
}

// OLA 10: menus de ajustes originales (h10_settings.cpp) encima del HUD, en la misma lista 2D
void (*g_h10Draw2D)(int W, int H);
static bool s_hudOn = true;
// OLA 17: bateria del movil (h13_batt.cpp) encima de todo, tambien con la pausa/ajustes abiertos. Puntero de datos: sin H13 queda a 0.
extern void (*g_h13DrawBattery)(int W, int H);
static void H5F_Draw2D(int W, int H) { if (s_hudOn) H5F_DrawHud(W, H); if (g_h10Draw2D) g_h10Draw2D(W, H); if (g_h13DrawBattery) g_h13DrawBattery(W, H); }
// primitivas 2D para g_h5pDrawTouch (renderer original: material "white" y fonts/normalFont, como el HUD)
// App: la partida termino (game over del GSC: sessionState de intermedio)
bool H5P_GameOver() { return s_fHave && s_f.sessionState == SESS_STATE_INTERMISSION; }
void H5P_DrawRect(float x, float y, float w, float h, const float *rgba) { if (s_hudWhite) R_AddCmdDrawStretchPic(x, y, w, h, 0, 0, 1, 1, rgba, s_hudWhite); }
void H5P_DrawLabel(const char *t, float x, float y, float scale, const float *rgba) { if (s_hudFont) R_AddCmdDrawText((char *)t, 0x7fffffff, s_hudFont, x, y, scale, scale, 0, rgba, 3); }

static double NowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

// camara de tercera persona (solo para ver la persecucion en las capturas): detras y por encima del jugador, recortada contra el mundo
static void H5P_ChaseCam(float *org, const float *ang)
{
    float fwd[3]; const float flat[3] = { 15.0f, ang[1], 0 }; AngleVectors(flat, fwd, nullptr, nullptr);
    const float start[3] = { org[0], org[1], org[2] + 10.0f };
    float end[3] = { start[0] - fwd[0] * 140.0f, start[1] - fwd[1] * 140.0f, start[2] - fwd[2] * 140.0f + 25.0f };
    trace_t tr; const float mn[3] = { -4, -4, -4 }, mx[3] = { 4, 4, 4 }; col_context_t ctx(1);
    CM_BoxTrace(&tr, start, end, mn, mx, 1, &ctx);
    for (int i = 0; i < 3; ++i) org[i] = start[i] + (end[i] - start[i]) * tr.fraction;
}

int H5P_Run(int maxFrames, bool view)
{
    g_h0BotCmdHook = H5P_BotCmd;
    g_h4PlayerSink = H5P_Sink;
    g_h5pCmdExtra = H5P_CmdButtons;
    const float seconds = getenv("H5_PLAY_SECONDS") ? (float)atof(getenv("H5_PLAY_SECONDS")) : 90.0f;
    const float shotEvery = getenv("H5_PLAY_SHOT") ? (float)atof(getenv("H5_PLAY_SHOT")) : 5.0f;
    const bool chase = getenv("H5_PLAY_CAM") && !strcmp(getenv("H5_PLAY_CAM"), "chase");
    const int mode = getenv("H5_PLAY_SCRIPT") ? atoi(getenv("H5_PLAY_SCRIPT")) : 1;
    if (mode == 2 && !Dvar_GetBool("bo1_testclient_fight")) Dvar_SetBoolByName("bo1_testclient_fight", true);   // politica del piloto
    g_h5vDrawPlayer = chase;
    s_hudOn = !getenv("H5_HUD") || atoi(getenv("H5_HUD"));
    { void H10_Install(); H10_Install(); }   // OLA 10: ajustes (H10=0 lo quita)
    { void H23_Install(); H23_Install(); }   // OLA U2: controles tactiles (h23_touch.cpp): comando h23_customize; en el Mac con H23_PILOT es la entrada
    { void H11_Install(); H11_Install(); }   // OLA 11: anfitrion en red (solo con H11_HOST=1)
    if (view && (s_hudOn || g_h10Draw2D)) g_h4Draw2D = H5F_Draw2D;
    g_h5wViewOverride = !chase;
    const bool realTime = (g_h5pInputProvider != nullptr || getenv("H5_REALTIME")) && !getenv("H5_FIXEDDT");   // OLA 15E: H5_FIXEDDT=1 paso fijo de 1/60 s aunque haya proveedor de entrada (partidas repetibles con H12_SEED)
    float dt = 1.0f / 60.0f;
    const int renderFrames = getenv("H5_PLAY_FRAMES") ? atoi(getenv("H5_PLAY_FRAMES")) : (int)(seconds * 60.0f);   // H5_PLAY_FRAMES: pruebas por pasos (1, 30, 600)
    const bool eventShots = getenv("P4_OUT") != nullptr;   // capturas por evento solo con destino (en la app no hay P4_OUT: nada de readbacks)
    double lastNow = NowMs();
    fprintf(stderr, "[H5D] bucle unificado: %.0f s a 60 Hz (%d frames de render), servidor a sv_fps=%d, vista %s, camara %s, entrada %s\n", seconds, renderFrames,
        sv_fps->current.integer, view ? "si (cada frame)" : "no", chase ? "chase" : "first",
        g_h5pInputProvider ? "app" : mode == 2 ? "piloto (politica de pelea del mod -> H5PlayerInput)" : mode == 1 ? "guion H5d" : "quieto");
    (void)maxFrames;
    double msAcc = 0; int lastLog = -1, lastHealth = -1, hits = 0, hitShots = 0; double nextShot = shotEvery;
    double skinSum = 0, drawSum = 0, frameSum = 0, svSum = 0; int nMeasured = 0, maxActors = 0; double skinMax = 0;
    float minDist = 1e30f; int minDistT = 0, firstHitT = -1, deadT = -1;
    for (int f = 0; f < renderFrames; ++f)
    {
        if (f == 1) H15_MemPhase("partida");   // OLA 15A: primer frame presentado (fin de la carga)
        if (realTime) { const double now = NowMs(); dt = (float)((now - lastNow) / 1000.0); lastNow = now; if (dt < 1.0f / 240.0f) dt = 1.0f / 240.0f; if (dt > 1.0f / 15.0f) dt = 1.0f / 15.0f; }
        H4Input in; float camOrg[3], camAng[3]; H4_GetCam(camOrg, camAng);
        if (g_h5pInputProvider) g_h5pInputProvider(&s_pin, dt);
        else if (mode == 2) { if (s_active) s_scriptT += dt; H5P_Pilot(s_pin, camAng); }
        else H5P_Script(s_pin, dt, camAng, mode);
        if (g_h5pInputOverride) g_h5pInputOverride(&s_pin, camOrg, camAng);   // OLA 15C: tramos guionizados encima del piloto (H6A_BUYTEST)
        H5P_LatchInput(s_pin);
        // H5PlayerInput -> capa de H4 (stick, vista, postura, sprint); los botones de arma van al usercmd en H5P_CmdButtons
        memset(&in, 0, sizeof in);
        in.fwd = s_pin.moveFwd; in.strafe = s_pin.moveRight; in.dyaw = s_pin.lookYaw; in.dpitch = s_pin.lookPitch;
        in.up = (s_pin.buttons & H5IN_JUMP) ? 1.0f : (s_pin.buttons & H5IN_STANCE) ? -1.0f : 0.0f;
        in.prone = (s_pin.buttons & H5IN_PRONE) != 0; in.sprint = (s_pin.buttons & H5IN_SPRINT) != 0;
        const int t0 = svs.time; double s0 = NowMs();
        if (g_h5pNetPump) g_h5pNetPump();
        if (!g_h5pPause)
        {
            H4_Update(in, dt);          // PlayerMove (postura/sprint de cl_input) -> H5P_Sink: usercmd pendiente + camara del servidor
            msAcc += dt * 1000.0; int msec = (int)msAcc; msAcc -= msec;
            s0 = NowMs();
            SV_Frame(0, msec);
            { void H8_Frame(); H8_Frame(); }   // OLA 8: puertas/cristales (h8_doors.cpp)
        }
        const double s1 = NowMs();
        if (svs.time != t0)
        {
            H5P_AfterServerFrame();
            { void H5E_Frame(); H5E_Frame(); }   // [H5E] arma/municion (y el montaje H5E_NEAR si se pide)
            H5F_AfterServerFrame();
            if (mode == 2 && !g_h5pInputProvider) H5P_PilotServerFrame();
        }
        // cifras del frame
        gentity_s *pl = H5P_Player();
        if (pl && s_active)
        {
            int alive = 0, near64 = 0; float bd = 1e30f; int bn = -1;
            for (int e = 0; e < level.num_entities; ++e)
            {
                gentity_s *a = &g_entities[e];
                if (!a->r.inuse || a->s.eType != ET_ACTOR || a->health <= 0) continue;
                ++alive;
                const float dx = a->r.currentOrigin[0] - pl->r.currentOrigin[0], dy = a->r.currentOrigin[1] - pl->r.currentOrigin[1];
                const float d = sqrtf(dx * dx + dy * dy);
                if (d < 64.0f) ++near64;
                if (d < bd) { bd = d; bn = e; }
            }
            if (bn >= 0 && bd < minDist) { minDist = bd; minDistT = level.time; }
            if (lastHealth >= 0 && pl->health < lastHealth)
            {
                ++hits; if (firstHitT < 0) firstHitT = level.time;
                fprintf(stderr, "[H5D] t=%d GOLPE %d al jugador: salud %d -> %d; zombi mas cercano ent %d a %.1f u (%d a <64 u)\n", level.time, hits, lastHealth,
                    pl->health, bn, bd, near64);
                if (view && hitShots < 6) { ++hitShots; nextShot = 0; }
            }
            if ((pl->health <= 0 || pl->client->sess.sessionState != SESS_STATE_PLAYING) && deadT < 0) { deadT = level.time; fprintf(stderr, "[H5D] t=%d jugador sin salud (sessionState %d)\n", level.time, (int)pl->client->sess.sessionState); }
            lastHealth = pl->health;
            if (level.time / 1000 != lastLog)
            {
                lastLog = level.time / 1000;
                const playerState_s *ps = &pl->client->ps;
                fprintf(stderr, "[H5D] t=%d jugador (%.1f,%.1f,%.1f) |v|=%.0f yaw %.1f salud %d | zombis vivos %d, mas cercano ent %d a %.1f u (%.1f,%.1f,%.1f) | <64 u: %d\n",
                    level.time, ps->origin[0], ps->origin[1], ps->origin[2], sqrtf(ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1]), ps->viewangles[1], pl->health,
                    alive, bn, bn >= 0 ? bd : -1.0f, bn >= 0 ? g_entities[bn].r.currentOrigin[0] : 0, bn >= 0 ? g_entities[bn].r.currentOrigin[1] : 0,
                    bn >= 0 ? g_entities[bn].r.currentOrigin[2] : 0, near64);
            }
        }
        if (view)
        {
            H4_GetCam(camOrg, camAng);
            if (chase) { H5P_ChaseCam(camOrg, camAng); H4_SetCam(camOrg, camAng); }
            // viewmodel por frame de render: camara interpolada y tiempo de servidor interpolado (cg.time entre snapshots)
            memcpy(g_h5wViewOrg, camOrg, 12); memcpy(g_h5wViewAng, camAng, 12);
            g_h5wViewTime = s_curTime >= 0 ? s_curTime - (int)(1000 / sv_fps->current.integer) + (int)lrintf(H5P_Frac() * (1000.0f / sv_fps->current.integer)) : level.time;
            float shakeAng[3]; memcpy(shakeAng, camAng, 12);   // OLA E2a: Earthquake() del GSC (CG_ShakeCamera) solo en el dibujo, no en la entrada
            extern bool H5FX_ShakeCamera(const float *, float *, int) __attribute__((weak));   // h5_fx.cpp (H5FX=1)
            const bool shaken = H5FX_ShakeCamera && H5FX_ShakeCamera(camOrg, shakeAng, g_h5wViewTime);
            if (shaken) { memcpy(g_h5wViewAng, shakeAng, 12); H4_SetCam(camOrg, shakeAng); }
            g_h5vSkinMs = g_h5vDrawMs = 0;
            const double r0 = NowMs(); H4_Frame(); const double r1 = NowMs();
            if (shaken) H4_SetCam(camOrg, camAng);
            if (g_h5vActors > 0) { skinSum += g_h5vSkinMs; drawSum += g_h5vDrawMs; frameSum += r1 - r0; svSum += s1 - s0; ++nMeasured; if (g_h5vSkinMs > skinMax) skinMax = g_h5vSkinMs; if (g_h5vActors > maxActors) maxActors = g_h5vActors; }
            { extern bool g_h5eShotReq; if (g_h5eShotReq) { g_h5eShotReq = false; s_fShotNow = true; } }   // OLA M1: H5_TIMED shot
            if (s_active && ((shotEvery > 0 && s_scriptT >= nextShot) || (s_fShotNow && eventShots)))
            {
                if (!s_fShotNow) { if (nextShot > 0) nextShot += shotEvery; else nextShot = s_scriptT + 0.5; }   // tras un golpe vuelve al ritmo normal
                char tag[48]; snprintf(tag, sizeof tag, "_h5f_t%06d_r%d", level.time, s_f.round); H4_Shot(tag);
                fprintf(stderr, "[H5D] captura t=%d%s cam (%.1f,%.1f,%.1f) ang (%.1f,%.1f) actores=%d superficies=%d skin %.2f ms\n", level.time, s_fShotNow ? " (evento)" : "",
                    camOrg[0], camOrg[1], camOrg[2], camAng[0], camAng[1], g_h5vActors, g_h5vSurfs, g_h5vSkinMs);
                s_fShotNow = false;
            }
            if (f % 600 == 0 && nMeasured) fprintf(stderr, "[H5D-PERF] t=%d media de %d frames con actores: skinning CPU %.2f ms (max %.2f, hasta %d actores), envio de actores %.2f ms, H4_Frame %.2f ms, SV_Frame %.2f ms\n",
                level.time, nMeasured, skinSum / nMeasured, skinMax, maxActors, drawSum / nMeasured, frameSum / nMeasured, svSum / nMeasured);
        }
        if (!view || !eventShots) s_fShotNow = false;
        if (g_h5pAudio) { float ao[3], aa[3]; H4_GetCam(ao, aa); g_h5pAudio(svs.time != t0, ao, aa); }
        if (g_h5pPostFrame) g_h5pPostFrame();
    }
    fprintf(stderr, "[H5D] FIN t=%d: golpes=%d (primero en t=%d), distancia minima zombi-jugador %.1f u en t=%d, salud final %d%s\n", level.time, hits, firstHitT,
        minDist, minDistT, H5P_Player() ? H5P_Player()->health : -1, deadT >= 0 ? " (sin salud)" : "");
    {
        const H5FState s = H5F_Read();
        fprintf(stderr, "[H5F] FIN t=%d (%.1f min de juego): ronda %d (maxima %d), puntos %d, muertes de zombi con puntos %d, compras %d, abatido %d, revivido %d, salud %d, sessionState %d\n",
            level.time, level.time / 60000.0, s.round, s_fMaxRound, s.score, s_fKills, s_fBuys, s_fDowns, s_fRevives, s.health, s.sessionState);
        for (int r = 1; r < 16; ++r) if (s_fRoundStart[r]) fprintf(stderr, "[H5F]   ronda %d empieza en t=%d\n", r, s_fRoundStart[r]);
    }
    if (nMeasured) fprintf(stderr, "[H5D-PERF] total: %d frames con actores: skinning CPU %.2f ms de media (max %.2f, hasta %d actores), envio %.2f ms, H4_Frame %.2f ms, SV_Frame %.2f ms\n",
        nMeasured, skinSum / nMeasured, skinMax, maxActors, drawSum / nMeasured, frameSum / nMeasured, svSum / nMeasured);
    g_h0BotCmdHook = nullptr; g_h4PlayerSink = nullptr; g_h5pCmdExtra = nullptr; g_h4Draw2D = nullptr; g_h5wViewOverride = false;
    return 0;
}
