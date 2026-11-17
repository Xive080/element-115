// OLA 13: HUD ORIGINAL de zombies. El cliente (cgame) no corre en el harness: aqui se le da lo que el original recibe por la red y
// se llama a SU dibujo 2D tal cual (CG_Draw2D, cg_draw_mp.cpp): hudelems del GSC (CG_Draw2dHudElems), los menus ui/hud.txt +
// ui/hud_zombie.txt con sus ownerdraws (Menu_PaintAll(cgDC) -> CG_OwnerDraw), punto de mira, indicadores de dano, etc.
//   Estado que se le da al cliente (lo que haria CL_ParseGamestate / CG_SetNextSnap con un snapshot del servidor local):
//     - cls.gameState <- sv.configstrings (cadenas de pista de compra, modelos, iconos...) cuando cambia alguna
//     - snapshot: ps del jugador, clientState_s de cada cliente conectado (G_GetClientState, como SV_BuildClientSnapshot)
//     - bgs.clientinfo: los campos que copia CG_SetNextSnap + CG_CopyScoreFromClientState (puntos) original
//   TUs originales compiladas enteras (h13/h13w_*.cpp): cg_main_mp, cg_draw_mp, cg_newDraw_mp, cg_hudelem, cg_ammocounter,
//   offhandweapons, cg_draw_indicators, cg_draw_reticles, cg_drawtools, cg_sp_hud, cg_snapshot_mp.
// Variables: H13_HUD=0 -> HUD de la Ola 7c. H13_DUMP=1 -> vuelca los menus del HUD (items, ownerdraws, expresiones).
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <universal/com_expressions.h>
#include <universal/com_expressions_eval.h>
#include <ui/ui_feeders.h>
#include <database/db_registry.h>
#include <server_mp/server_mp.h>
#include <server_mp/sv_init_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <client/screen_placement.h>
#include <client/cl_main.h>
#include <client_mp/cl_main_mp.h>
#include <client_mp/cl_cgame_mp.h>
#include <client_mp/cl_scrn_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <cgame_mp/cg_main_mp.h>
#include <cgame_mp/cg_draw_mp.h>
#include <cgame_mp/cg_snapshot_mp.h>
#include <cgame_mp/cg_newDraw_mp.h>
#include <cgame/cg_ammocounter.h>
#include <cgame_mp/cg_scoreboard_mp.h>
#include <cgame/cg_hudelem.h>
#include <clientscript/cscr_stringlist.h>
#include <ui/ui_shared.h>
#include <ui/ui_main.h>
#include <gfx_d3d/r_rendercmds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <string>
#include <mutex>

void ScrPlace_SetupFloatViewport(ScreenPlacement *, float, float, float, float);
void UI_AssetCache();
extern float g_p5Safe[4];
extern clientUIActive_t clientUIActives[1];

#include <ui/ui_shared_obj.h>
#include <ui/ui_utils.h>
#include <database/db_assetnames.h>
bool G_ExitAfterToolComplete();
#include "h13_uiload.inc"   // UI_LoadMenus + UI_LoadMenus_FastFile ORIGINALES (ui_shared_obj.cpp, extraidas por build.sh)
#include <client/client.h>
int __cdecl CL_LocalClient_GetActiveCount();
#include <cgame/cg_draw_indicators.h>
#include <cgame_mp/cg_servercmds_mp.h>
#include <bgame/bg_misc.h>
#include <bgame/bg_perks.h>
#include <universal/com_math_anglevectors.h>
#include <cgame/cg_main.h>
#include "h13_dmgfb.inc"   // CG_DamageFeedback ORIGINAL (cg_playerstate.cpp)
#include <qcommon/msg.h>
#include "h13_notify.inc"   // CG_MenuShowNotify ORIGINAL (cg_servercmds_mp.cpp): enciende los fundidos del HUD (municion/dpad tras disparar o cambiar de arma)

static bool EnvOn(const char *n, bool def) { const char *e = getenv(n); return e ? atoi(e) != 0 : def; }
static double NowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

// ---------------------------------------------------------------- zona segura (UNICO punto de entrada del margen del HUD)
// Rectangulo seguro en tiempo de ejecucion, para cualquier movil/aspecto/orientacion (nada fijo al Pixel Fold):
//   1) la funcion de la Ola 10 si la publica (puntero g_h10GetSafeInsets; dato sin definir = 0 en el harness, como g_h10Draw2D)
//   2) g_p5Safe: insets reales del sistema que rellena la app (recorte/notch, esquinas, barra de gestos; cambian al girar)
//   3) H10_SAFE=l,t,r,b (pixeles) para simularlo en el Mac
// y encima un margen minimo por lado: H13_SAFE_MIN (fraccion del lado corto, por defecto 0.015). Devuelve l,t,r,b en pixeles.
extern bool (*g_h10GetSafeInsets)(int W, int H, float out[4]);
static void H13_SafeInsets(int W, int H, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!(g_h10GetSafeInsets && g_h10GetSafeInsets(W, H, out))) {
        for (int i = 0; i < 4; ++i) out[i] = g_p5Safe[i];
        if (const char *e = getenv("H10_SAFE")) sscanf(e, "%f,%f,%f,%f", &out[0], &out[1], &out[2], &out[3]);
    }
    static const float minFrac = getenv("H13_SAFE_MIN") ? (float)atof(getenv("H13_SAFE_MIN")) : 0.015f;
    const float m = minFrac * (W < H ? W : H);
    for (int i = 0; i < 4; ++i) if (out[i] < m) out[i] = m;
}
// Como el original en consola (safeArea_horizontal/vertical -> ScrPlace_CalcSafeAreaOffsets): el viewport y la escala siguen siendo
// la pantalla entera y solo se mueven los bordes "viewable"/"tweakable", que es donde anclan las alineaciones LEFT/RIGHT/TOP/BOTTOM
// de itemDefs y hudelems. El original usa un inset simetrico (ratio); aqui es por lado, en pixeles, con la misma formula.
// OJO: pasar el inset como viewportX/Y a ScrPlace_SetupFloatViewport NO sirve: CalcSafeAreaOffsets resta viewportX (relativo) y
// realViewportBase queda a 0, asi que solo encoge la escala y el desplazamiento se pierde.
static void SafePlace(ScreenPlacement *p, int W, int H, const float s[4])
{
    ScrPlace_SetupFloatViewport(p, 0, 0, (float)W, (float)H);
    const float hs = p->scaleVirtualToFull[0] / p->scaleVirtualToReal[0];   // horzAspectScale = W / ancho 4:3
    const float mn[2] = { s[0], s[1] }, mx[2] = { W - s[2], H - s[3] };
    for (int i = 0; i < 2; ++i) {
        const float k = i == 0 ? hs * 640.0f / W : 480.0f / H;
        p->realViewableMin[i] = p->realTweakableMin[i] = mn[i]; p->realViewableMax[i] = p->realTweakableMax[i] = mx[i];
        p->virtualViewableMin[i] = p->virtualTweakableMin[i] = mn[i] * k; p->virtualViewableMax[i] = p->virtualTweakableMax[i] = mx[i] * k;
    }
}
// scrPlaceView (casi todos los itemDefs y hudelems) y scrPlaceFull = zona segura; scrPlaceFullUnsafe = pantalla entera.
static void H13_ApplySafe(int W, int H)
{
    float s[4]; H13_SafeInsets(W, H, s);
    static float last[4] = { -1, -1, -1, -1 }; static int lw, lh;
    if (lw != W || lh != H || memcmp(last, s, sizeof s)) {   // cambia al girar (el recorte pasa de lado) o si cambian los insets
        memcpy(last, s, sizeof s); lw = W; lh = H;
        fprintf(stderr, "[H13] zona segura %dx%d: l%.0f t%.0f r%.0f b%.0f\n", W, H, s[0], s[1], s[2], s[3]);
    }
    // cada frame: otras partes (UI de la Ola 10, ui_p5) reponen scrPlace* a su manera; el HUD dibuja con la suya
    ScrPlace_SetupFloatViewport(&scrPlaceFullUnsafe, 0, 0, (float)W, (float)H);
    SafePlace(&scrPlaceView[0], W, H, s);
    memcpy(&scrPlaceFull, &scrPlaceView[0], sizeof(ScreenPlacement));
}

// cg_consolecmds_mp.cpp (TU de comandos de consola, no compilada): misma semantica que el original. Antes era un stub a 0 (safe0.txt):
// con 0 UI_OwnerDraw nunca llamaba a CG_OwnerDraw (bit 32 = cgame inicializado).
bool __cdecl CL_LocalClient_IsCUIFlagSet(int localClientNum, int flag) { return (flag & clientUIActives[localClientNum].flags) != 0; }
// r_ui3d.cpp (render a textura de los menus "ui3d": reloj de pulsera del brazo, iconos de objetivo en mundo; no portado):
// el original devuelve &g_ui3d_windows[i].scrPlace y en MP la ventana 0 es copia de scrPlaceFull (R_UI3D_CheckRenderTarget).
// Sin destino UI3D en el harness, cada ventana es la pantalla entera; nunca nulo (antes era stub a 0 y Menu_Paint caia).
static ScreenPlacement s_ui3dPlace[6];
ScreenPlacement *__cdecl R_UI3D_ScrPlaceFromTextureWindow(unsigned int windowIndex)
{
    if (windowIndex >= 6) windowIndex = 0;
    ScrPlace_SetupUI3DForFullscreen(&s_ui3dPlace[windowIndex], &scrPlaceFull);
    return &s_ui3dPlace[windowIndex];
}

// ---------------------------------------------------------------- teclas en el HUD (H13_GPAD, por defecto 1)
// Los textos del HUD ponen la tecla con UI_GetKeyBindingLocalizedString (ui_shared.cpp): "[{+activate}]" en las pistas, keybinding()
// en el dpad. En tactil no hay teclado ni teclas de actionslot: se usa la rama de mando del original (gpad_enabled=1), que es la de
// las capturas de consola de referencia (dpad con iconos, sin "[5]"/"[6]") y que en GetPlatformCommand ya traduce +activate/+reload a
// +usereload. CL_GetGamePadBinding (cl_keys.cpp) da el nombre del boton fisico; aqui da el del boton tactil que hace esa orden (en
// ingles, como el HUD). Lo que no tiene boton tactil devuelve 0 y el original pone KEY_UNBOUND. H13_GPAD=0 -> rama de teclado
// (CL_GetKeyBinding, que es de la OLA 10).
#include <win32/win_gamerprofile.h>
static bool GpadOn() { static const bool on = EnvOn("H13_GPAD", true); return on; }
static const char *TouchButtonFor(const char *cmd)
{
    static const struct { const char *cmd, *name; } k[] = {
        { "+usereload", "USE" }, { "+activate", "USE" }, { "+attack", "FIRE" }, { "+speed_throw", "AIM" }, { "+toggleads_throw", "AIM" },
        { "+reload", "RELOAD" }, { "+melee", "KNIFE" }, { "+melee_zoom", "KNIFE" }, { "+gostand", "JUMP" }, { "togglecrouch", "CROUCH" },
        { "+stance", "CROUCH" }, { "weapnext", "WEAPON" }, { "+weapnext_inventory", "WEAPON" }, { "togglemenu", "MENU" },
    };
    for (auto &e : k) if (!strcmp(cmd, e.cmd)) return e.name;
    return nullptr;
}
int __cdecl CL_GetGamePadBinding(int, const char *command, char (*keyNames)[128], int bindNum)
{
    keyNames[0][0] = keyNames[1][0] = 0;
    const char *n = command && !bindNum ? TouchButtonFor(command) : nullptr;   // un solo boton por orden: bindNum 1 (segunda tecla) no hay
    if (!n) return 0;
    I_strncpyz(keyNames[0], n, 128);
    return 1;
}
// win_gamerprofile.cpp: GetPlatformCommand lee gpadButtonsConfig del perfil; sin perfil cargado el original tiene el array a cero
static GamerSettingState s_gamerSettings[1];
GamerSettingState *__cdecl GamerProfile_GetProfileSettings(int controllerIndex) { return &s_gamerSettings[controllerIndex ? 0 : controllerIndex]; }
extern const dvar_s *gpad_enabled;
static void H13_SetupGpad()
{
    if (!gpad_enabled) gpad_enabled = _Dvar_RegisterBool("gpad_enabled", 0, 1u, "Game pad enabled");   // win_gamepad.cpp (GPad_RegisterDvars)
}
// Solo mientras pinta el HUD: el menu y los ajustes (OLA 10) siguen viendo el valor que tengan ellos.
struct GpadScope {
    bool old; GpadScope() { dvar_s *d = const_cast<dvar_s *>(gpad_enabled); old = d->current.enabled; d->current.enabled = GpadOn(); }
    ~GpadScope() { const_cast<dvar_s *>(gpad_enabled)->current.enabled = old; }
};

// ---------------------------------------------------------------- volcado de los menus del HUD
static void DumpExp(const char *tag, const ExpressionStatement &e)
{
    if (!e.numRpn || !e.rpn) return;
    extern const char *g_expOperatorNames[];
    std::string s;
    for (int i = 0; i < e.numRpn; ++i) {
        const expressionRpn &r = e.rpn.get()[i]; char b[256];
        if (r.type == 0) {
            const Operand &o = r.data.constant;
            if (o.dataType == VAL_INT) snprintf(b, sizeof b, " %d", o.internals.intVal);
            else if (o.dataType == VAL_FLOAT) snprintf(b, sizeof b, " %g", o.internals.floatVal);
            else snprintf(b, sizeof b, " \"%s\"", o.internals.string ? o.internals.string.get() : "");
        } else if (r.type == 1) snprintf(b, sizeof b, " %s()", g_expOperatorNames[r.data.cmdIdx]);
        else if (r.type == 3) break; else snprintf(b, sizeof b, " <t%d>", r.type);
        s += b;
    }
    fprintf(stderr, "[H13]       %s:%s\n", tag, s.c_str());
}
static void DumpMenu(menuDef_t *m)
{
    windowDef_t &mw = m->window;
    fprintf(stderr, "[H13] MENU %s rect=(%g,%g,%g,%g) al=(%d,%d) ui3d=%d items=%d ownerDrawFlags=%x fore=(%g,%g,%g,%g)\n", mw.name.get(), mw.rect.x, mw.rect.y, mw.rect.w, mw.rect.h,
        mw.rect.horzAlign, mw.rect.vertAlign, m->ui3dWindowId, m->itemCount, mw.ownerDrawFlags, mw.foreColor[0], mw.foreColor[1], mw.foreColor[2], mw.foreColor[3]);
    DumpExp("menu visible", m->visibleExp);
    for (int i = 0; i < m->itemCount; ++i) {
        itemDef_s *it = m->items[i].get(); windowDef_t &w = it->window;
        textDef_s *t = Item_GetTextDef(it);
        fprintf(stderr, "[H13]   item %2d type=%2d/%2d name=%s rect=(%g,%g,%g,%g) al=(%d,%d) style=%d od=%d odf=%x fore=(%.2f,%.2f,%.2f,%.2f) bg=%s dvar=%s text='%s' scale=%g font=%d align=%d tal=(%g,%g)\n",
            i, it->type, it->dataType, w.name ? w.name.get() : "-", w.rect.x, w.rect.y, w.rect.w, w.rect.h, w.rect.horzAlign, w.rect.vertAlign, w.style, w.ownerDraw, w.ownerDrawFlags,
            w.foreColor[0], w.foreColor[1], w.foreColor[2], w.foreColor[3], w.background ? w.background.get()->info.name.get() : "-", it->dvar ? it->dvar.get() : "-",
            t && t->text ? t->text.get() : "", t ? t->textscale : 0.0f, t ? t->fontEnum : 0, t ? t->alignment : 0, t ? t->textalignx : 0.0f, t ? t->textaligny : 0.0f);
        DumpExp("visible", it->visibleExp);
        if (t && t->textExpData) DumpExp("textExp", t->textExpData.get()->textExp);
        if (it->dataType == 6 && it->typeData.ownerDrawDef) DumpExp("dataExp", it->typeData.ownerDrawDef.get()->dataExp);
        if (it->dataType == 2 && it->typeData.imageDef) DumpExp("materialExp", it->typeData.imageDef.get()->materialExp);
    }
}

// ---------------------------------------------------------------- estado de cliente (lo que llega por la red en el original)
static snapshot_s *s_snap;
static unsigned short s_csIds[MAX_CONFIGSTRINGS]; static bool s_csInit;
// CL_ParseGamestate / CL_ConfigstringModified: cls.gameState con todas las cadenas (offset 0 = cadena vacia)
static void SyncConfigStrings()
{
    bool changed = !s_csInit;
    for (int i = 0; i < MAX_CONFIGSTRINGS && !changed; ++i) if (s_csIds[i] != sv.configstrings[i]) changed = true;
    if (!changed) return;
    s_csInit = true;
    gameState_t &gs = cls.gameState;
    memset(gs.stringOffsets, 0, sizeof gs.stringOffsets); gs.stringData[0] = 0; gs.dataCount = 1;
    int n = 0;
    for (int i = 0; i < MAX_CONFIGSTRINGS; ++i) {
        s_csIds[i] = sv.configstrings[i];
        const char *s = sv.configstrings[i] ? SL_ConvertToString(sv.configstrings[i], SCRIPTINSTANCE_SERVER) : "";
        if (!s || !*s) continue;
        const int len = (int)strlen(s);
        if (gs.dataCount + len + 1 > (int)sizeof gs.stringData) { fprintf(stderr, "[H13] gameState lleno en cs %d\n", i); break; }
        gs.stringOffsets[i] = gs.dataCount; memcpy(gs.stringData + gs.dataCount, s, len + 1); gs.dataCount += len + 1; ++n;
    }
    static int logs; if (logs++ < 8) fprintf(stderr, "[H13] configstrings -> cls.gameState: %d cadenas, %d bytes\n", n, gs.dataCount);
}

// SV_BuildClientSnapshot (lo que ve el jugador) + CG_SetNextSnap (clientinfo y puntos)
// Ordenes del jugador en el anillo del cliente (CL_CreateNewCommands, cl_input_mp.cpp): CG_CheckForPlayerInput las lee con
// CL_GetCurrentCmdNumber/CL_GetUserCmd para encender el HUD al disparar, recargar, lanzar o moverse. En el harness el usercmd lo
// genera la entrada de H4/la app y llega directo al servidor: se copia el que el servidor acaba de procesar (sess.cmd).
static void SyncCmds(const gentity_s *pl)
{
    if (!clients) return;
    static int lastTime = -1;
    const usercmd_s &c = pl->client->sess.cmd;
    if (c.serverTime == lastTime) return;
    lastTime = c.serverTime;
    clientActive_t *cl = CL_GetLocalClientGlobals(0);
    memcpy(&cl->cmds[++cl->cmdNumber & 0x7F], &c, sizeof(usercmd_s));
}
// Lo que el cliente original hace al pasar de un snapshot al siguiente y que pinta HUD (CG_TransitionPlayerState + eventos):
//   - dano nuevo (damageEvent cambia)      -> CG_DamageFeedback: flecha de direccion del dano, destello rojo, vida que parpadea
//   - arma nueva (EV_ITEM_PICKUP/cambio)    -> como CG_SelectWeaponIndex: weaponSelect + CG_MenuShowNotify(1) (municion/dpad visibles)
//   - granada/tactica nueva                 -> como CG_SetEquippedOffHand: CG_MenuShowNotify(5)
// Las demas consecuencias de esos eventos (sonidos, fx, viewmodel) son de otras olas (h5_fx, h5_weapon) y no se tocan aqui.
static void Transitions(const playerState_s *ps)
{
    cg_s *cg = CG_GetLocalClientGlobals(0);
    static int lastWeapon = -1, lastOffhand = -1, lastDamage = -1;
    if (lastDamage >= 0 && ps->damageEvent != lastDamage && ps->damageCount) CG_DamageFeedback(0, ps->damageYaw, ps->damagePitch, ps->damageCount);
    lastDamage = ps->damageEvent;
    if ((int)ps->weapon != lastWeapon) { lastWeapon = ps->weapon; cg->weaponSelect = ps->weapon; cg->weaponSelectTime = cg->time; CG_MenuShowNotify(0, 1); }
    if ((int)ps->offHandIndex != lastOffhand) { lastOffhand = ps->offHandIndex; cg->equippedOffHand = ps->offHandIndex; CG_MenuShowNotify(0, 5); }
}
static void SyncSnapshot(const gentity_s *pl)
{
    cg_s *cg = CG_GetLocalClientGlobals(0);
    snapshot_s *sn = s_snap;
    memcpy(&sn->ps, &pl->client->ps, sizeof(playerState_s));
    // Lo que el cliente RECIBE de los hudelems no es el float del servidor: fontScale va por la red como campo -86
    // (MSG_FIELD_FONTSCALE, qcommon/sv_msg_write.cpp: 6 bits de round((fs-1)*10) que dan la vuelta; se lee bits*0.1+1). Asi el
    // "Round" del GSC (fontscale 16) se ve a 3.2 y la tiza/numero de ronda (32) a 6.4, como en las grabaciones. H13_NETFS=0 lo quita.
    static const bool netfs = EnvOn("H13_NETFS", true);
    if (netfs) for (int k = 0; k < 31; ++k) for (hudelem_s *e : { &sn->ps.hud.current[k], &sn->ps.hud.archival[k] })
        if (e->type) e->fontScale = (float)(MSG_RoundFloatToInt((e->fontScale - 1.0f) * 10.0f) & 63) * 0.1f + 1.0f;
    sn->serverTime = level.time; sn->ping = 0; sn->snapFlags = 0;
    sn->numEntities = 0; sn->numClients = 0;
    for (int i = 0; i < com_maxclients->current.integer && i < 32; ++i)
        if (svs.clients[i].header.state >= CS_CONNECTED) memcpy(&sn->clients[sn->numClients++], G_GetClientState(i), sizeof(clientState_s));
    cg->snap = sn; cg->nextSnap = sn;
    cg->clientNum = pl->client->ps.clientNum;
    for (int n = 0; n < 32; ++n) cg->scoreOrder[n] = 32;
    for (int n = 0; n < sn->numClients; ++n) {
        clientState_s *cs = &sn->clients[n];
        clientInfo_t *ci = &cg->bgs.clientinfo[cs->clientIndex];
        ci->oldteam = ci->infoValid ? ci->team : cs->team;
        ci->infoValid = 1; ci->nextValid = 1; ci->clientNum = cs->clientIndex; ci->team = cs->team; ci->ffaTeam = cs->ffaTeam;
        ci->lastStandStartTime = cs->lastStandStartTime; ci->needsRevive = cs->needsRevive; ci->rank = cs->rank; ci->prestige = cs->prestige;
        memcpy(ci->perks, cs->perks, sizeof ci->perks);
        I_strncpyz(ci->name, cs->name, sizeof ci->name);
        CG_CopyScoreFromClientState(0, ci, cs);
        cg->scoreOrder[n] = cs->clientIndex;
    }
    CG_UpdateScoreData(0);
}

// ---------------------------------------------------------------- arranque (CG_Init: la parte del HUD)
static bool s_ready, s_ok;
extern int s_indexToFunctionMap[1024];   // com_expressions_eval.cpp: Expression_Init lo rellena (aserta si se llama dos veces)
static bool ExprReady() { for (int i = 0; i < 1024; ++i) if (s_indexToFunctionMap[i]) return true; return false; }
extern void (*g_h10Draw2D)(int W, int H);   // h5_play.cpp: la OLA 10 prepara la UI en su primer frame (Expression_Init incluido)
static bool Setup(int W, int H)
{
    cls.vidConfig.displayWidth = cls.vidConfig.sceneWidth = W; cls.vidConfig.displayHeight = cls.vidConfig.sceneHeight = H;
    cls.vidConfig.aspectRatioWindow = (float)W / H; cls.vidConfig.aspectRatioScenePixel = 1.0f;
    if (cls.vidConfig.aspectRatioDisplayPixel <= 0) cls.vidConfig.aspectRatioDisplayPixel = 1.0f;   // pixeles cuadrados (R_SetVidConfig); a 0 los textos de hudelem median inf y se iban a x=0
    H13_ApplySafe(W, H);
    if (!ExprReady()) { Expression_Init(); UI_RegisterDvars(); UI_AssetCache(); }   // si la OLA 10 (h10_settings.cpp) ya lo hizo, no se repite
    CG_RegisterDvars();
    H13_SetupGpad();
    // Colores del marcador de puntos: en el original los pone coop_arcademode.cfg (via default.cfg en Com_ExecStartupConfigs,
    // qcommon/common.cpp, mismas lineas). El harness no ejecuta esos cfg: sin esto el texto de los puntos sale NEGRO
    // (cg_ScoresColor_Gamertag_0 se registra 0,0,0) y la mancha mas oscura. H13_SCORECFG=0 lo quita.
    if (EnvOn("H13_SCORECFG", true)) {
        static const char *const kv[][2] = { { "cg_ScoresColor_Zombie", "0.424 0.004 0" }, { "cg_ScoresColor_TransparencyZombie", "0.8" },
            { "cg_ScoresColor_Gamertag_0", "1.0 1.0 1.0" }, { "cg_ScoresColor_Gamertag_1", "0.486 0.812 0.933" },
            { "cg_ScoresColor_Gamertag_2", "0.965 0.792 0.314" }, { "cg_ScoresColor_Gamertag_3", "0.514 0.925 0.533" },
            { "cg_ScoresColor_Player_0", "0.024 0.169 0.204" }, { "cg_ScoresColor_Player_1", "0.447 0.38 0.2" },
            { "cg_ScoresColor_Player_2", "0.024 0.173 0.114" }, { "cg_ScoresColor_Player_3", "0.196 0.059 0.047" },
            { "cg_ScoresColor_Transparency", "0.35" } };
        for (auto &e : kv) if (Dvar_FindVar(e[0])) Dvar_SetFromStringByName(e[0], (char *)e[1]);
    }
    // dvars de TUs no compiladas que CG_Draw2D lee por puntero (mismos parametros que el original)
    { extern const dvar_s *ui_showEndOfGame; if (!ui_showEndOfGame) ui_showEndOfGame = _Dvar_RegisterBool("ui_showEndOfGame", 0, 0, "Currently showing the end of game menu."); }   // ui_main_mp.cpp (UI_Project_RegisterDvars)
    if (!com_sv_running) com_sv_running = _Dvar_RegisterBool("sv_running", 0, 0x40u, "Server is running");   // common.cpp
    { extern const dvar_s *team_indicator; if (!team_indicator) team_indicator = _Dvar_RegisterInt("team_indicator", 0, 0, 3, 1u, "The team indicator modes : 0 - Full Name ; 1 - Abbreviated ; 2 - Icon Only"); }   // cl_main_mp.cpp (CL_InitOnceForAllClients)
    { extern const dvar_s *cl_hudDrawsBehindUI; if (!cl_hudDrawsBehindUI) cl_hudDrawsBehindUI = _Dvar_RegisterBool("cl_hudDrawsBehindUI", 1, 0, "Should the HUD draw when the UI is up?"); }   // cl_main_mp.cpp (CL_InitOnceForAllClients)
    if (!cgArray) cgArray = (cg_s *)calloc(1, sizeof(cg_s));   // CG_AllocateClientMemory (h5_weapon.cpp lo reserva igual si llega antes)
    if (!cgsArray) cgsArray = (cgs_t *)calloc(1, sizeof(cgs_t));
    s_snap = (snapshot_s *)calloc(1, sizeof(snapshot_s));
    cgs_t *cgs = CG_GetLocalClientStaticGlobals(0);
    cgs->viewX = 0; cgs->viewY = 0; cgs->viewWidth = W; cgs->viewHeight = H; cgs->viewAspect = (float)W / H; cgs->localServer = 1;
    cgs->maxclients = com_maxclients->current.integer; I_strncpyz(cgs->gametype, "zom", sizeof cgs->gametype);
    snprintf(cgs->mapname, sizeof cgs->mapname, "maps/%s.d3dbsp", sv_mapname->current.string);
    // Subconjunto de CG_Init (cg_main_mp.cpp) que el HUD necesita; el resto (DObj, FX, scripts de cliente) no esta portado
    {
        cg_s *cgameGlob = CG_GetLocalClientGlobals(0);
        cgameGlob->localClientNum = 0; cgameGlob->clientNum = 0; cgameGlob->drawHud = 1; cgameGlob->lastHealthLerpDelay = 1;
        cgameGlob->groundTiltEntNum = -1; cgameGlob->lastPlayerStateOverride = -1; cgameGlob->extraCamEntity = 1023;
#include "h13_media.inc"
        CG_RegisterScoreboardGraphics();
        CG_AntiBurnInHUD_RegisterDvars();
        CG_InitViewDimensions(0);
    }
    // cgDC (CG_Init): contexto de los menus del HUD
    UiContext *dc = &cgDC[0];
    memset(dc, 0, sizeof(UiContext)); dc->contextIndex = 0;
    Menu_Setup(dc);
    CL_GetScreenDimensions(&dc->screenWidth, &dc->screenHeight, &dc->screenAspect);
    dc->bias = (480 * dc->screenWidth <= 640 * dc->screenHeight) ? 0.0f : ((float)dc->screenWidth - (float)dc->screenHeight * 1.3333334f) * 0.5f;
    CG_LoadHudMenu(0);
    fprintf(stderr, "[H13] HUD original: %d menus en cgDC (%dx%d, bias %.1f)\n", dc->menuCount, W, H, dc->bias);
    if (EnvOn("H13_DUMP", false)) for (int i = 0; i < dc->menuCount; ++i) DumpMenu(dc->Menus[i]);
    clientUIActives[0].flags |= 32;   // CG_Init terminado (CL_InitCGame)
    return dc->menuCount > 0;
}

// OLA 15E: ordenes 'v' del servidor al cliente local (SetClientDvar/SetClientDvars del GSC). cg_servercmds_mp.cpp no esta en el
// arnes, asi que nadie las ejecutaba: end_game pone ammoCounterHide/miniscoreboardhide (los puntos y la municion se iban en el
// original y aqui no) y missionfailed hud_missionFailed. Mismo reparto que CG_DeployServerCommand case 'v' -> CG_SetClientDvarFromServer:
// pares nombre/valor, cg_objectiveText, hud_drawHud y g_scriptMainMenu aparte (no los usa zombies: se ignoran). El gancho corre en el
// hilo del servidor: se encola y se aplica en el frame del cliente. H13_CLDVARS=0 lo quita.
extern void (*g_h0ServerCmdHook)(const client_t *client, const char *cmd);   // server_mp/sv_main_mp.cpp
static void (*s_h13PrevCmd)(const client_t *, const char *);
static std::mutex s_h13CmdMx; static std::vector<std::string> s_h13Cmds;
static void H13_ServerCmd(const client_t *c, const char *cmd)
{
    if (s_h13PrevCmd) s_h13PrevCmd(c, cmd);
    if (!c || !cmd || cmd[0] != 'v' || cmd[1] != ' ' || c != &svs.clients[0]) return;
    std::lock_guard<std::mutex> l(s_h13CmdMx); s_h13Cmds.push_back(cmd + 2);
}
static void H13_RunClientDvars()
{
    static const bool on = EnvOn("H13_CLDVARS", true); if (!on) return;
    if (g_h0ServerCmdHook != H13_ServerCmd) { s_h13PrevCmd = g_h0ServerCmdHook; g_h0ServerCmdHook = H13_ServerCmd; }
    std::vector<std::string> q; { std::lock_guard<std::mutex> l(s_h13CmdMx); q.swap(s_h13Cmds); }
    for (const std::string &line : q) {
        std::vector<std::string> tok; const char *p = line.c_str();
        while (*p) { while (*p == ' ') ++p; if (!*p) break; std::string t;
            if (*p == '"') { ++p; while (*p && *p != '"') t += *p++; if (*p) ++p; } else while (*p && *p != ' ') t += *p++;
            tok.push_back(t); }
        for (size_t i = 0; i + 1 < tok.size(); i += 2) {
            const char *n = tok[i].c_str();
            if (!I_stricmp(n, "cg_objectiveText") || !I_stricmp(n, "hud_drawHud") || !I_stricmp(n, "g_scriptMainMenu")) continue;
            Dvar_SetFromStringByName(n, (char *)tok[i + 1].c_str());
            if (getenv("H13_CLDVARS_LOG")) fprintf(stderr, "[H13] v %s = '%s'\n", n, tok[i + 1].c_str());
        }
    }
}

// OLA 15E: estado del marcador pedido por la entrada (tecla Tab, boton tactil, Back del mando).
static bool s_h13WantScores;
void H13_ShowScores(bool on) { s_h13WantScores = on; }

// Una pasada 2D del cliente: H5F_DrawHud la llama dentro de la lista 2D. Devuelve false si el HUD original no esta activo.
bool H13_DrawHud(int W, int H)
{
    static const bool on = EnvOn("H13_HUD", true);
    if (!on) return false;
    gentity_s *pl = &g_entities[0];
    if (!pl->r.inuse || !pl->client) return false;
    static int waits;
    if (!s_ready && !ExprReady() && g_h10Draw2D && waits++ < 3) return false;   // un frame para que la OLA 10 haga su arranque de UI primero
    if (!s_ready) { s_ready = true; s_ok = Setup(W, H); }
    if (!s_ok) return false;
    const double t0 = NowMs();
    cg_s *cg = CG_GetLocalClientGlobals(0);
    SyncConfigStrings();
    H13_RunClientDvars();
    SyncSnapshot(pl);
    SyncCmds(pl);
    Transitions(&pl->client->ps);
    if (cg->time <= 0) cg->time = level.time;   // h5_weapon.cpp lo pone por frame de render (tiempo interpolado)
    // OLA 15E: el arnes no avanza cg->physicsTime (CG_ProcessSnapshots lo copia de snap->physicsTime = tiempo del servidor). Sin el,
    // CG_FadeColor(physicsTime, scoreFadeTime...) devuelve 0 (startMsec 0) y el marcador de fin de partida no salia. H13_PHYSTIME=0 lo quita.
    { static const bool pt = EnvOn("H13_PHYSTIME", true); if (pt) cg->physicsTime = level.time; }
    static double last = NowMs(); const double now = NowMs();
    UiContext *dc = &cgDC[0];
    dc->realTime = (int)now; dc->frameTime = (int)(now - last); last = now;
    H13_ApplySafe(W, H);
    CL_UpdateUIVisibilityBits(0);   // SCR_UpdateFrame (cl_scrn_mp.cpp): bits showBits/hideBits de los menus (sin esto weaponinfo/dpad no se ven)
    {   // OLA 15E: marcador (CG_SP_DrawScoreboard lo dibuja con cg->showScores, como +scores/-scores de CG_ScoresDown/Up).
        // Tab (Mac), boton MARCADOR (tactil) y Back (mando) llaman a H13_ShowScores; H13_SCORES=t0,t1 lo abre por guion (level.time ms).
        static int sc0 = -1, sc1 = -1; static bool scInit;
        if (!scInit) { scInit = true; if (const char *e = getenv("H13_SCORES")) sscanf(e, "%d,%d", &sc0, &sc1); }
        const bool want = s_h13WantScores || (sc0 >= 0 && level.time >= sc0 && level.time < sc1);
        if (want != (cg->showScores != 0)) {
            if (!want) cg->scoreFadeTime = cg->physicsTime;   // como CG_ScoresUp_f: fundido de 100 ms (CG_FadeColor)
            cg->showScores = want;
        }
    }
    { GpadScope g; CG_Draw2D(0); }
    if (getenv("H13_SBDIAG")) { static int k; if (++k % 120 == 0) { extern const dvar_s *cl_paused;
        fprintf(stderr, "[H13] SB pm_type=%d showScores=%d numScores=%d fade=%d phys=%d paused=%d activeMenu=%d team=%d valid=%d\n", cg->nextSnap->ps.pm_type, cg->showScores, cg->numScores,
            cg->scoreFadeTime, cg->physicsTime, cl_paused ? cl_paused->current.integer : -1, (int)UI_GetActiveMenu(0), cg->bgs.clientinfo[cg->clientNum].team, cg->bgs.clientinfo[cg->clientNum].infoValid); } }
    static const bool vis = EnvOn("H13_VIS", false); static int visN;
    if (vis && ++visN % 300 == 0) {   // diagnostico: por que se ve o no cada menu del HUD
        fprintf(stderr, "[H13] VIS bits=%llx arDP=%g\n", (unsigned long long)sharedUiInfo.visibilityBits[0], (double)CL_GetScreenAspectRatioDisplayPixel());
        { cg_s *c = CG_GetLocalClientGlobals(0); extern const dvar_s *hud_enable, *hud_fade_ammodisplay;
          fprintf(stderr, "[H13] VIS hud_enable=%d shouldDraw=%d time=%d ammoFade=%d fade=%.2f dpadA=%.2f ammoA=%.2f weapon=%d clients=%p cmd#=%d btn=%x/%x\n", hud_enable ? hud_enable->current.enabled : -1,
            (int)CG_ShouldDrawHud(0), c->time, c->ammoFadeTime, CG_FadeHudMenu(0, hud_fade_ammodisplay, c->ammoFadeTime, (int)(hud_fade_ammodisplay->current.value * 1000)),
            CG_GetHudAlphaDPad(0), CG_GetHudAlphaAmmoCounter(0), c->predictedPlayerState.weapon, (void *)clients, clients ? CL_GetLocalClientGlobals(0)->cmdNumber : -1,
            g_entities[0].client->sess.cmd.button_bits.array[0], g_entities[0].client->sess.cmd.button_bits.array[1]); }
        { hudelem_s *list[64]; int n = 0; playerState_s *ps = &CG_GetLocalClientGlobals(0)->nextSnap->ps;
          for (int k = 0; k < 31 && ps->hud.current[k].type; ++k) list[n++] = &ps->hud.current[k];
          for (int k = 0; k < 31 && ps->hud.archival[k].type && n < 64; ++k) list[n++] = &ps->hud.archival[k];
          for (int k = 0; k < n; ++k) { const hudelem_s *e = list[k]; static cg_hudelem_t cghe; static char str[1024]; memset(&cghe, 0, sizeof cghe); cghe.timeNow = CG_GetLocalClientGlobals(0)->time;
            GetHudElemInfo(0, e, &cghe, str);
            fprintf(stderr, "[H13] HE %2d type=%d font=%d fs=%.2f x=%.1f y=%.1f org=%x scr=%x w=%d h=%d a=%d mat=%d -> x=%.1f y=%.1f w=%.1f h=%.1f fs=%.3f fh=%.1f txt='%s' lbl='%s'\n",
              k, e->type, e->font, e->fontScale, e->x, e->y, e->alignOrg, e->alignScreen, e->width, e->height, e->color.a, e->materialIndex, cghe.x, cghe.y, cghe.width, cghe.height, cghe.fontScale, cghe.fontHeight, str, cghe.hudElemLabel); } }
        for (int i = 0; i < dc->menuCount; ++i) { menuDef_t *m = dc->Menus[i]; itemDef_s dummy; dummy.parent = m;
            fprintf(stderr, "[H13] VIS %-26s vis=%d winVis=%d show=%llx hide=%llx exp=%d\n", m->window.name.get(), Menu_IsVisible(0, dc, m),
                Window_IsVisible(dc->contextIndex, &m->window), (unsigned long long)m->showBits, (unsigned long long)m->hideBits,
                m->visibleExp.filename ? (int)IsExpressionTrue(0, &dummy, &m->visibleExp) : -1); }
    }
    static double acc; static int n; acc += NowMs() - t0;
    if (++n == 600) { fprintf(stderr, "[H13] 600 frames: HUD %.3f ms/frame\n", acc / n); acc = 0; n = 0; }
    return true;
}

// ui_main_mp.cpp (TU de UI de MP, no compilada): mismo cuerpo que el original. Antes era stub a 0 y el punto de mira no salia nunca.
bool __cdecl UI_ShouldDrawCrosshair() { extern const dvar_s *ui_drawCrosshair; return ui_drawCrosshair && ui_drawCrosshair->current.enabled; }
// cg_main.cpp (no compilada entera): CG_flrand/CG_random los usan el destello de dano y el borde de poca vida. Misma formula; rand()
// se recorta a 15 bits como el RAND_MAX=32767 de MSVC (con el rand() de 31 bits de Mac/Android el resultado se saldria del rango).
double __cdecl CG_flrand(float min, float max) { return (float)((max - min) * (float)(rand() & 0x7FFF) / 32767.0f) + min; }
float __cdecl CG_random() { return (float)(rand() & 0x7FFF) / 32767.0f; }
