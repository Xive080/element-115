// OLA 10: MENU DE AJUSTES ORIGINAL dentro del juego (ui/ingame_options.txt -> options_graphics, con la UI del motor: Menu_PaintAll,
// Menu_HandleKey, Display_MouseMove) + perfil de calidad + guardado con el formato original (players/config.cfg, "seta nombre valor").
// Mismo codigo en Mac (raton/teclado), Fold (tactil) y Windows: solo cambia quien llama a H10_Pointer/H10_Key.
// Variables: H10=0 lo desactiva entero; H10_OPEN=<menu> lo abre al arrancar; H10_PILOT="t accion;..." entrada guionizada (pruebas);
// H10_DUMP=<menu> vuelca items/acciones; H10_CFG=<ruta> fichero de config (por defecto <datos>/players/config.cfg).
#include <database/db_registry.h>
#include <database/db_assetnames.h>
#include <universal/dvar.h>
#include <universal/ptr32.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_font.h>
#include <client/screen_placement.h>
#include <client_mp/cl_main_mp.h>
#include <qcommon/common.h>
#include <qcommon/cmd.h>
#include <ui/ui_shared.h>
#include <ui/ui_main.h>
#include <ui/keycodes.h>
#include <live/live_win.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <math.h>
#include <ctype.h>
#include <ui/ui_localvars.h>
#include <ui/ui_utils.h>
#include <ui/ui_feeders.h>
#include <qcommon/dvar_cmds.h>

extern uiInfo_s uiInfoArray[1];
void ScrPlace_SetupFloatViewport(ScreenPlacement *, float, float, float, float);
void UI_AssetCache();
#include <universal/com_expressions.h>
void __cdecl CL_GetScreenDimensions(int *width, int *height, float *aspect);
unsigned int Sys_Milliseconds();
extern float g_p5Safe[4];
extern bool g_h5pPause;
void H4_Shot(const char *tag);

static bool s_on = true, s_ready = false, s_open = false;
static UiContext *s_dc;
static int s_W, s_H;
static double NowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

// ---------------------------------------------------------------- zona segura (recorte, esquinas redondeadas, barra de gestos), en tiempo de ejecucion
// La app rellena g_h10SafeFrac (fracciones de la ZONA DE JUEGO: izq, arriba, der, abajo) con WindowInsets/DisplayCutout/RoundedCorner y la
// vuelve a leer al girar o plegar. En el Mac: H10_SAFE=l,t,r,b (pixeles del motor, o fracciones si todos <= 1). H10_SAFE_MIN = margen minimo
// (fraccion del lado corto; 0.02 en Android aunque no haya datos, 0 en el Mac). Los menus (scrPlaceView[0] mientras corre la UI) se colocan
// dentro de ese rectangulo: las alineaciones originales (centro, izquierda, pantalla completa...) se resuelven contra el y no contra la pantalla.
float g_h10SafeFrac[4];   // la app (Android) la rellena; 0 = sin datos
float g_h10Dpi;           // la app: pixeles por pulgada (para el tamano minimo de los objetivos tactiles); 0 = desconocido
static float s_safe[4]; static ScreenPlacement s_place; static bool s_placeOk;
static void SafeNow(float o[4])
{
    bool frac = false; for (int i = 0; i < 4; ++i) frac |= g_h10SafeFrac[i] > 0;
    for (int i = 0; i < 4; ++i) o[i] = frac ? g_h10SafeFrac[i] * (i & 1 ? s_H : s_W) : g_p5Safe[i];   // sin datos de la ola 10: los de JniSetup
    if (const char *e = getenv("H10_SAFE")) { float v[4] = {}; if (sscanf(e, "%f,%f,%f,%f", v, v + 1, v + 2, v + 3) == 4) { const bool fr = v[0] <= 1 && v[1] <= 1 && v[2] <= 1 && v[3] <= 1;
        for (int i = 0; i < 4; ++i) o[i] = fr ? v[i] * (i & 1 ? s_H : s_W) : v[i]; } }
#ifdef __ANDROID__
    float mn = 0.02f;
#else
    float mn = 0.0f;
#endif
    if (const char *e = getenv("H10_SAFE_MIN")) mn = (float)atof(e);
    const float m = mn * (s_W < s_H ? s_W : s_H);
    for (int i = 0; i < 4; ++i) { const float lim = (i & 1 ? s_H : s_W) / 3.0f; if (o[i] < m) o[i] = m; if (o[i] > lim) o[i] = lim; }   // nunca mas de un tercio por lado
}
static void PlaceUpdate()
{
    float o[4]; SafeNow(o);
    if (s_placeOk && !memcmp(o, s_safe, sizeof o)) return;
    memcpy(s_safe, o, sizeof o); s_placeOk = true;
    ScrPlace_SetupFloatViewport(&s_place, 0, 0, s_W - o[0] - o[2], s_H - o[1] - o[3]);
    for (int k = 0; k < 2; ++k) { const float d = o[k];   // k 0: x (izquierda), 1: y (arriba)
        s_place.realViewportBase[k] += d; s_place.realViewportMid[k] += d; s_place.realViewableMin[k] += d; s_place.realViewableMax[k] += d;
        s_place.realTweakableMin[k] += d; s_place.realTweakableMax[k] += d; s_place.subScreen[k] += d;
        // alineacion 'pantalla completa' (fondos que oscurecen el juego): toda la pantalla, tambien bajo el recorte y la barra de gestos.
        // El centro (alineacion 2 = tamano/2 + base) sigue en el centro de la zona segura con base 0 y tamano = 2*margen + zona segura.
        const float safeSz = (k ? s_H - o[1] - o[3] : s_W - o[0] - o[2]);
        s_place.realViewportBase[k] = 0; s_place.realViewportSize[k] = 2 * d + safeSz; s_place.scaleVirtualToFull[k] = k ? s_H / 480.0f : s_W / 640.0f; }
    fprintf(stderr, "[H10] zona segura: izq %.0f arriba %.0f der %.0f abajo %.0f px en %dx%d (menus en %.0fx%.0f)\n", o[0], o[1], o[2], o[3], s_W, s_H, s_W - o[0] - o[2], s_H - o[1] - o[3]);
}
// la UI original lee scrPlaceView[0]: mientras corre el menu se usa la zona segura y despues se deja como estaba (HUD y resto intactos)
// (tambien scrPlaceFull: algunos dibujos de la UI la usan; H10_PLACEFULL=0 deja scrPlaceFull como la tenga el HUD)
static bool PlaceFullOn() { static const bool on = !getenv("H10_PLACEFULL") || atoi(getenv("H10_PLACEFULL")); return on; }
struct PlaceScope { ScreenPlacement saved, savedFull; PlaceScope() { saved = scrPlaceView[0]; savedFull = scrPlaceFull; if (s_placeOk) { scrPlaceView[0] = s_place; if (PlaceFullOn()) scrPlaceFull = s_place; } }
    ~PlaceScope() { scrPlaceView[0] = saved; scrPlaceFull = savedFull; } };

// Publicado para el HUD (h13_hud.cpp, H13_SafeInsets) como PUNTERO DE DATOS (una referencia debil a funcion acaba en stub): insets l,t,r,b
// en pixeles de una pantalla W x H con la misma fuente que los menus (g_h10SafeFrac de la app; si no hay, g_p5Safe; H10_SAFE en el Mac).
// Sin margen minimo (el HUD pone el suyo, H13_SAFE_MIN). H10_SAFEPUB=0 -> devuelve false y el HUD usa su camino propio.
static bool H10_GetSafeInsets(int W, int H, float out[4])
{
    static const bool on = !getenv("H10_SAFEPUB") || atoi(getenv("H10_SAFEPUB")); if (!on || W <= 0 || H <= 0) return false;
    bool frac = false; for (int i = 0; i < 4; ++i) frac |= g_h10SafeFrac[i] > 0;
    for (int i = 0; i < 4; ++i) out[i] = frac ? g_h10SafeFrac[i] * (i & 1 ? H : W) : g_p5Safe[i];
    if (const char *e = getenv("H10_SAFE")) { float v[4] = {}; if (sscanf(e, "%f,%f,%f,%f", v, v + 1, v + 2, v + 3) == 4) { const bool fr = v[0] <= 1 && v[1] <= 1 && v[2] <= 1 && v[3] <= 1;
        for (int i = 0; i < 4; ++i) out[i] = fr ? v[i] * (i & 1 ? H : W) : v[i] * (s_W > 0 && s_H > 0 ? (i & 1 ? (float)H / s_H : (float)W / s_W) : 1.0f); } }
    for (int i = 0; i < 4; ++i) { const float lim = (i & 1 ? H : W) / 3.0f; if (out[i] < 0) out[i] = 0; if (out[i] > lim) out[i] = lim; }
    return true;
}
bool (*g_h10GetSafeInsets)(int W, int H, float out[4]) = H10_GetSafeInsets;

// ---------------------------------------------------------------- copias literales de common.cpp (no compilado): nombre del config del jugador
extern const dvar_s *bo1_zombies;
const char *__cdecl Com_PlayerConfigName() { return bo1_zombies && bo1_zombies->current.enabled ? "config.cfg" : "config_mp.cfg"; }

// ---------------------------------------------------------------- volcado (diagnostico)
static void DumpScripts(const char *who, GenericEventHandler *h)
{
    for (; h; h = h->next.get())
        for (GenericEventScript *s = h->eventScript.get(); s; s = s->next.get())
            fprintf(stderr, "[H10]     %s on '%s' type=%d action='%s'\n", who, h->name ? h->name.get() : "-", s->type, s->action ? s->action.get() : "");
}
extern const char *g_expOperatorNames[]; 
static std::string ExpStr(const ExpressionStatement &e)
{
    std::string o; const expressionRpn *r = e.rpn.get();
    for (int i = 0; r && i < e.numRpn; ++i) {
        char b[96];
        if (r[i].type == 0) { const Operand &c = r[i].data.constant; if (c.dataType == VAL_INT) snprintf(b, sizeof b, "%d ", c.internals.intVal); else if (c.dataType == VAL_FLOAT) snprintf(b, sizeof b, "%g ", c.internals.floatVal); else snprintf(b, sizeof b, "'%s' ", c.internals.string ? c.internals.string.get() : ""); }
        else { int k = r[i].data.cmdIdx; if (k < 24) snprintf(b, sizeof b, "%s ", g_expOperatorNames[k]); else snprintf(b, sizeof b, "f%d ", k - 24); }
        o += b;
    }
    return o;
}
textDef_s *__cdecl Item_GetTextDef(itemDef_s *item);   // ui_utils.cpp (solo tipos con textDef)
static void H10_DumpItems(const char *name, int a, int b)
{
    menuDef_t *m = DB_FindXAssetHeader(ASSET_TYPE_MENU, (char *)name, 1, -1).menu; if (!m) return;
    for (int i = a; i <= b && i < m->itemCount; ++i) {
        itemDef_s *it = m->items[i].get(); textDef_s *t = Item_GetTextDef(it);
        fprintf(stderr, "[H10] I%d type=%d name=%s rect=(%g,%g,%g,%g) style=%d border=%d flags=%x/%x fore=(%g,%g,%g,%g) back=(%g,%g,%g,%g) bg=%s dvar=%s vis=[%s] foreA=[%s]\n", i, it->type, it->window.name ? it->window.name.get() : "-",
            it->window.rect.x, it->window.rect.y, it->window.rect.w, it->window.rect.h, it->window.style, it->window.border, it->window.staticFlags, it->window.dynamicFlags[0],
            it->window.foreColor[0], it->window.foreColor[1], it->window.foreColor[2], it->window.foreColor[3], it->window.backColor[0], it->window.backColor[1], it->window.backColor[2], it->window.backColor[3],
            it->window.background ? it->window.background.get()->info.name.get() : "-", it->dvar ? it->dvar.get() : "-", ExpStr(it->visibleExp).c_str(), ExpStr(it->forecolorAExp).c_str());
        if (rectData_s *rd = it->rectExpData.get()) fprintf(stderr, "[H10]    rectExp x=[%s] y=[%s] w=[%s] h=[%s]\n", ExpStr(rd->rectXExp).c_str(), ExpStr(rd->rectYExp).c_str(), ExpStr(rd->rectWExp).c_str(), ExpStr(rd->rectHExp).c_str());
        if (t) {
            fprintf(stderr, "[H10]    text='%s' textExp=%p align=%d x=%g y=%g scale=%g font=%d style=%d flags=%x\n", t->text ? t->text.get() : "", (void *)t->textExpData.get(), t->alignment, t->textalignx, t->textaligny, t->textscale, t->fontEnum, t->textStyle, t->itemFlags);
            if (t->textExpData) fprintf(stderr, "[H10]    textExp=[%s]\n", ExpStr(*(ExpressionStatement *)t->textExpData.get()).c_str());
        }
    }
}
static void H10_DumpRaw(const char *name)
{
    RawFile *r = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)name, 1, -1).rawfile.get(); if (!r) { fprintf(stderr, "[H10] rawfile %s: no esta\n", name); return; }
    const char *b = r->buffer.get(); int n = r->len > 6000 ? 6000 : r->len; int z = 0; while (z < n && !b[z]) ++z;
    fprintf(stderr, "[H10] rawfile %s (%d bytes, %d ceros al principio, primeros %02x %02x %02x %02x):\n%.*s\n[H10] fin rawfile\n", name, r->len, z, (unsigned char)b[0], (unsigned char)b[1], (unsigned char)b[2], (unsigned char)b[3], n - z, b + z);
}
static void H10_Dump(const char *name)
{
    menuDef_t *m = DB_FindXAssetHeader(ASSET_TYPE_MENU, (char *)name, 1, -1).menu; if (!m) { fprintf(stderr, "[H10] dump: no hay menu %s\n", name); return; }
    fprintf(stderr, "[H10] MENU %s items=%d\n", name, m->itemCount);
    DumpScripts("menu", m->onEvent.get());
    for (ItemKeyHandler *k = m->onKey.get(); k; k = k->next.get()) for (GenericEventScript *s = k->keyScript.get(); s; s = s->next.get()) fprintf(stderr, "[H10]     menu key %d action='%s'\n", k->key, s->action ? s->action.get() : "");
    for (int i = 0; i < m->itemCount; ++i) {
        itemDef_s *it = m->items[i].get(); windowDef_t &w = it->window;
        const char *txt = "";
        textDef_s *t = (it->type != 3 && it->type != 7) ? it->typeData.textDef.get() : nullptr;   // 3 imagen 7 ownerdraw (sin textDef)
        if (t && t->text) txt = t->text.get();
        fprintf(stderr, "[H10]   item %3d type=%2d name=%s rect=(%g,%g,%g,%g) al=(%d,%d) vis=%d dvar=%s dvarTest=%s enable=%s flags=%x text='%s'\n", i, it->type, w.name ? w.name.get() : "-",
            w.rect.x, w.rect.y, w.rect.w, w.rect.h, w.rect.horzAlign, w.rect.vertAlign, it->visibleExp.numRpn, it->dvar ? it->dvar.get() : "-", it->dvarTest ? it->dvarTest.get() : "-",
            it->enableDvar ? it->enableDvar.get() : "-", it->dvarFlags, txt);
        DumpScripts("item", it->onEvent.get());
        if (t && t->textTypeData.focusItemDef && (it->type == 1 || it->type == 4 || it->type == 8 || it->type == 10 || it->type == 11 || it->type == 12 || it->type == 6)) {
            focusItemDef_s *f = t->textTypeData.focusItemDef.get();
            if (f->mouseEnter) fprintf(stderr, "[H10]     mouseEnter='%s'\n", f->mouseEnter.get());
            for (ItemKeyHandler *k = f->onKey.get(); k; k = k->next.get()) for (GenericEventScript *s = k->keyScript.get(); s; s = s->next.get()) fprintf(stderr, "[H10]     key %d action='%s'\n", k->key, s->action ? s->action.get() : "");
            if (it->type == 11) { enumDvarDef_s *ed = f->focusTypeData.enumDvar.get(); if (ed && ed->enumDvarName) fprintf(stderr, "[H10]     enum %s\n", ed->enumDvarName.get()); }
            if (it->type == 10) { multiDef_s *md = f->focusTypeData.multi.get(); if (md) { std::string l; for (int k = 0; k < md->count; ++k) { char b[160]; snprintf(b, sizeof b, " [%s=%s/%g]", md->dvarList[k] ? md->dvarList[k].get() : "", md->strDef && md->dvarStr[k] ? md->dvarStr[k].get() : "", md->dvarValue[k]); l += b; } fprintf(stderr, "[H10]     multi strDef=%d:%s\n", md->strDef, l.c_str()); } }
            if (it->type == 8) { editFieldDef_s *e = f->focusTypeData.editField.get(); if (e) fprintf(stderr, "[H10]     slider min=%g max=%g def=%g\n", e->minVal, e->maxVal, e->defVal); }
        }
    }
}

// ---------------------------------------------------------------- contexto de UI (como P5_SetupUi, sin pisar dvars que el juego ya tiene)
static const dvar_s *RegB(const char *n, bool v) { const dvar_s *d = Dvar_FindVar(n); return d ? d : _Dvar_RegisterBool(n, v, 0, ""); }
static const dvar_s *RegI(const char *n, int v, int lo, int hi) { const dvar_s *d = Dvar_FindVar(n); return d ? d : _Dvar_RegisterInt(n, v, lo, hi, 0, ""); }
static bool H10_Setup(int W, int H)
{
    cls.vidConfig.displayWidth = cls.vidConfig.sceneWidth = W; cls.vidConfig.displayHeight = cls.vidConfig.sceneHeight = H;
    cls.vidConfig.aspectRatioWindow = (float)W / H; cls.vidConfig.aspectRatioScenePixel = 1.0f;
    ScrPlace_SetupFloatViewport(&scrPlaceView[0], 0, 0, (float)W, (float)H);
    memcpy(&scrPlaceFull, &scrPlaceView[0], sizeof(ScreenPlacement)); memcpy(&scrPlaceFullUnsafe, &scrPlaceView[0], sizeof(ScreenPlacement));
    // la zona segura NO se pasa como viewportX/Y (CalcSafeAreaOffsets lo resta y realViewportBase queda a 0: solo encoge la escala);
    // los menus usan s_place (PlaceUpdate, desplazada de verdad) via PlaceScope y el HUD la suya (h13_hud.cpp, g_h10GetSafeInsets)
    zombiemode = RegB("zombiemode", 1); zombietron = RegB("zombietron", 0); com_sv_running = RegB("sv_running", 1); com_wideScreen = RegB("wideScreen", 1);
    com_maxclients = RegI("com_maxclients", 4, 1, 32); cl_voice = RegB("cl_voice", 0); cl_bypassMouseInput = RegB("cl_bypassMouseInput", 0);
    xblive_wagermatch = RegB("xblive_wagermatch", 0); xblive_basictraining = RegB("xblive_basictraining", 0);
    { extern const dvar_s *loc_forceEnglish, *loc_translate; loc_forceEnglish = RegB("loc_forceEnglish", 0); loc_translate = RegB("loc_translate", 1); }
    Expression_Init();
    UI_RegisterDvars();
    UI_AssetCache();
    uiInfo_s *ui = &uiInfoArray[0]; UiContext *dc = &ui->uiDC;
    dc->contextIndex = 0; ui->allowScriptMenuResponse = 1;
    Menu_Setup(dc);
    CL_GetScreenDimensions(&dc->screenWidth, &dc->screenHeight, &dc->screenAspect);
    dc->bias = (480 * dc->screenWidth <= 640 * dc->screenHeight) ? 0.0f : ((float)dc->screenWidth - (float)dc->screenHeight * 1.3333334f) * 0.5f;
    // listas: las opciones de partida (ingame_options) y la que trae los popups de vid_restart / fondo de pausa (como UI_LoadMenus del juego)
    std::vector<MenuList *> lists; DB_EnumXAssets_FastFile(ASSET_TYPE_MENULIST, [](XAssetHeader h, void *u) { ((std::vector<MenuList *> *)u)->push_back(h.menuList); }, &lists, false);
    static const char *const kNeed[] = { "options_graphics", "vid_restart_popmenu", "pausedbkgnd" };
    for (const char *need : kNeed) {
        menuDef_t *a = DB_FindXAssetHeader(ASSET_TYPE_MENU, (char *)need, 1, -1).menu; if (!a || Menus_FindByName(dc, need)) continue;
        for (MenuList *l : lists) { bool has = false; for (int i = 0; i < l->menuCount; ++i) if (l->menus[i].get() == a) has = true;
            if (has) { UI_AddMenuList(0, dc, l, 1); fprintf(stderr, "[H10] lista %s (%d menus) por %s\n", l->name.get(), l->menuCount, need); break; } }
    }
    if (!Menus_FindByName(dc, "options_graphics")) { fprintf(stderr, "[H10] falta options_graphics\n"); return false; }
    Menus_CloseAll(0, dc);
    fprintf(stderr, "[H10] UI lista: %d menus en %dx%d\n", dc->menuCount, W, H);
    if (getenv("H10_LIST")) for (int i = 0; i < dc->menuCount; ++i) fprintf(stderr, "[H10]   menu %d '%s' (%d items)\n", i, dc->Menus[i]->window.name.get(), dc->Menus[i]->itemCount);
    if (getenv("H10_LIST")) DB_EnumXAssets_FastFile(ASSET_TYPE_MENU, [](XAssetHeader h, void *) { menuDef_t *m = h.menu.get(); if (m && m->window.name) fprintf(stderr, "[H10]   asset menu '%s'\n", m->window.name.get()); }, nullptr, false);
    s_dc = dc; s_W = W; s_H = H;
    return true;
}

bool H10_Open(const char *menu)
{
    if (!s_dc) return false;
    PlaceScope ps; s_dc->realTime = (int)Sys_Milliseconds(); s_dc->frameTime = 16;
    Menus_CloseAll(0, s_dc);
    int ok = Menus_OpenByName(0, s_dc, menu);
    fprintf(stderr, "[H10] abre '%s': %d (abiertos %d)\n", menu, ok, s_dc->openMenuCount);
    s_open = s_dc->openMenuCount > 0; g_h5pPause = s_open;
    return s_open;
}
void H10_Close() { if (s_dc) { PlaceScope ps; Menus_CloseAll(0, s_dc); } s_open = false; g_h5pPause = false; }
bool H10_IsOpen() { return s_open; }

// menu que no esta en las listas cargadas: busca el asset y anade la lista que lo trae (como UI_LoadMenus del juego)
static bool EnsureMenu(const char *name)
{
    if (!s_dc) return false;
    if (Menus_FindByName(s_dc, name)) return true;
    struct Q { const char *n; menuDef_t *m; } q = { name, nullptr };
    DB_EnumXAssets_FastFile(ASSET_TYPE_MENU, [](XAssetHeader h, void *u) { Q *q = (Q *)u; menuDef_t *m = h.menu.get(); if (!q->m && m && m->window.name && !I_stricmp(m->window.name.get(), q->n)) q->m = m; }, &q, false);
    if (!q.m) { fprintf(stderr, "[H10] menu '%s' no esta en las zonas cargadas\n", name); return false; }
    std::vector<MenuList *> lists; DB_EnumXAssets_FastFile(ASSET_TYPE_MENULIST, [](XAssetHeader h, void *u) { ((std::vector<MenuList *> *)u)->push_back(h.menuList); }, &lists, false);
    for (MenuList *l : lists) for (int i = 0; i < l->menuCount; ++i) if (l->menus[i].get() == q.m) {
        UI_AddMenuList(0, s_dc, l, 1); fprintf(stderr, "[H10] lista %s (%d menus) por %s\n", l->name.get(), l->menuCount, name);
        return Menus_FindByName(s_dc, name) != nullptr;
    }
    return false;
}
// UI_SetActiveMenu (ui_main_mp.cpp, no compilado en el harness): misma semantica que el original para lo que hay en partida de zombies.
// NONE cierra todo y quita la pausa; INGAME abre el menu de pausa (pausedmenu, con pausa, como el SP); SCOREBOARD/ENDOFGAME cierran
// todo y abren "scoreboard"/"endofgame" (sin pausa: la partida sigue o ya acabo); MAIN/PREGAME/... no existen dentro de la partida -> 0.
// H10_ACTIVEMENU=0 lo deja como antes (no hace nada, devuelve 0).
static const char *PauseMenu();
int __cdecl UI_SetActiveMenu(int, uiMenuCommand_t menu)
{
    static const bool on = !getenv("H10_ACTIVEMENU") || atoi(getenv("H10_ACTIVEMENU"));
    if (!on || !s_dc) return 0;
    if (getenv("H10_TRACE")) fprintf(stderr, "[H10] UI_SetActiveMenu %d\n", (int)menu);
    switch (menu) {
    case UIMENU_NONE: H10_Close(); return 1;
    case UIMENU_INGAME: return H10_Open(PauseMenu()) ? 1 : 0;
    case UIMENU_SCOREBOARD: case UIMENU_ENDOFGAME: {
        // como el original: Menus_CloseAll + Menus_OpenByName (su resultado se ignora) y devuelve 1. En las zonas de zombies (225 menus)
        // NO hay "scoreboard" ni "endofgame" (medido con H10_LIST): queda todo cerrado y el marcador/fin de partida es cosa del cgame/GSC.
        const char *n = menu == UIMENU_SCOREBOARD ? "scoreboard" : "endofgame";
        if (EnsureMenu(n)) { H10_Open(n); g_h5pPause = false; }   // el marcador no congela la partida
        else H10_Close();
        return 1; }
    default: return 0;
    }
}

// ---------------------------------------------------------------- entrada: puntero (raton o dedo, en pixeles del motor) y teclas
// El cursor de la UI esta en el espacio virtual 640x480 (alineacion 4, como Rect_ContainsPoint): se invierte ScrPlace_ApplyX/Y.
static void ToVirtual(float px, float py, float *vx, float *vy)
{
    const ScreenPlacement *sp = s_placeOk ? &s_place : &scrPlaceView[0];
    const float x0 = ScrPlace_ApplyX(sp, 0, 4), x1 = ScrPlace_ApplyX(sp, 640, 4), y0 = ScrPlace_ApplyY(sp, 0, 4), y1 = ScrPlace_ApplyY(sp, 480, 4);
    *vx = (px - x0) * 640.0f / (x1 - x0); *vy = (py - y0) * 480.0f / (y1 - y0);
}
void H10_Pointer(float px, float py)
{
    if (!s_open) return;
    PlaceScope ps; float vx, vy; ToVirtual(px, py, &vx, &vy);
    s_dc->prevCursor = s_dc->cursor; s_dc->cursor.x = vx; s_dc->cursor.y = vy; s_dc->isCursorVisible = 1;
    Display_MouseMove(0, s_dc);
}
// ui_main_mp.cpp (uiScript del proyecto) no esta en el port: se registran y se ignoran
static void ApplyDefaults();
// common.cpp no esta en el port: los ajustes recomendados (por CPU/GPU en el original) son el perfil de serie de la plataforma
// (se aplica en el siguiente Poll, despues del "exec default_options.cfg" que el mismo boton encola antes)
static bool s_defPending;
void __cdecl Com_SetRecommended(int, int) { s_defPending = true; }
// cl_console.cpp no esta en el port: sin consola restringida, el cfg leido de disco se ejecuta tal cual (Cmd_ExecFromDisk)
void __cdecl Con_Restricted_ExecuteBuf(int localClientNum, int controllerIndex, char *buffer) { Cbuf_ExecuteBuffer(localClientNum, controllerIndex, buffer); }
void __cdecl UI_Project_RunMenuScript(int, int, const char *name, const char **, const char *actualScript)
{
    if (name && !I_stricmp(name, "setRecommended")) { s_defPending = true; return; }   // "Default System Settings" -> perfil de serie de la plataforma
    if (getenv("H10_TRACE")) fprintf(stderr, "[H10] uiScript sin soporte: %s (%s)\n", name ? name : "", actualScript ? actualScript : "");
}
// splitscreen.cpp no esta en el port: en H5G hay un cliente local en partida (CA_ACTIVE); los scripts de blur lo consultan
char __cdecl CL_AnyLocalClientStateActive() { return 1; }
// win_gamerprofile.cpp no esta en el port: hay un perfil local (el del config.cfg) siempre "conectado"
bool __cdecl GamerProfile_IsProfileLoggedIn(int) { return true; }
// g_sp_savegame.cpp no esta en el port: no hay partida guardada de SP (el menu de pausa oculta "guardar y salir", como sin guardado)
struct SPSaveGame; SPSaveGame *SP_GetSaveBuffer(int) { return nullptr; }
// cl_keys.cpp no esta en el port: sin bindings (Menu_HandleKey los consulta) y estado de teclas propio del menu
static bool s_keyDown[256];
const char *__cdecl Key_GetBinding(int, unsigned int, int) { return nullptr; }
// CL_GetKeyBinding (cl_keys.cpp): teclas de TECLADO asignadas a una orden, para la rama de teclado del HUD (H13_GPAD=0) y la UI.
// Tabla de binds como la del original: primero los "bind/bind2 TECLA orden" del config.cfg del jugador; si no trae ninguno, los .cfg
// del juego que esten como rawfile en las zonas cargadas (default*.cfg) (hoy default_mp_controls.cfg, 47 binds). H10_KEYS=0 -> sin teclas (como antes).
// Nombres como Key_KeynumToString(k, translate=1): letra/cifra en mayuscula, el resto "KEY_<NOMBRE>" (el HUD lo localiza). Sin tecla:
// 0 y "KEY_UNBOUND", como el original. bindNum 0 = primera tecla (y segunda si hay), 1 = solo la segunda.
static std::string CfgPath();
struct Bind { std::string key, cmd; int slot; };
static std::vector<Bind> s_binds; static bool s_bindsRead;
static void BindsParse(const char *txt, size_t n, const char *from)
{
    size_t before = s_binds.size(); std::string all(txt, n), line;
    for (size_t p = 0; p <= all.size(); ++p) {
        if (p < all.size() && all[p] != '\n' && all[p] != '\r' && all[p] != ';') { line += all[p]; continue; }
        char verb[16], key[64], cmd[128] = "";
        if (sscanf(line.c_str(), " %15s %63s \"%127[^\"]\"", verb, key, cmd) >= 3 || sscanf(line.c_str(), " %15s %63s %127s", verb, key, cmd) == 3) {
            const int slot = !strcmp(verb, "bind") ? 0 : !strcmp(verb, "bind2") ? 1 : -1;
            if (slot >= 0 && strncmp(key, "BUTTON_", 7) && strncmp(key, "DPAD_", 5) && strncmp(key, "APAD_", 5)) s_binds.push_back({ key, cmd, slot });   // solo teclado/raton
        }
        line.clear();
    }
    if (s_binds.size() > before) fprintf(stderr, "[H10] teclas: %d binds de %s\n", (int)(s_binds.size() - before), from);
}
static void BindsRead()
{
    if (s_bindsRead) return; s_bindsRead = true;
    if (getenv("H10_KEYS") && !atoi(getenv("H10_KEYS"))) return;
    if (FILE *f = fopen(CfgPath().c_str(), "rb")) { std::string t; char b[4096]; size_t r; while ((r = fread(b, 1, sizeof b, f)) > 0) t.append(b, r); fclose(f); BindsParse(t.data(), t.size(), "config.cfg"); }
    if (s_binds.empty()) DB_EnumXAssets_FastFile(ASSET_TYPE_RAWFILE, [](XAssetHeader h, void *) {
        RawFile *r = h.rawfile.get(); const char *n = r && r->name ? r->name.get() : ""; size_t l = strlen(n);
        if (l > 4 && !strcmp(n + l - 4, ".cfg") && !strncmp(n, "default", 7) && r->buffer && r->len > 0) BindsParse(r->buffer.get(), (size_t)r->len, n); }, nullptr, false);
    if (s_binds.empty()) fprintf(stderr, "[H10] teclas: sin binds (ni config.cfg ni default*.cfg en las zonas): todo KEY_UNBOUND\n");
}
static void KeyDisplayName(const std::string &k, char *out)
{
    if (k.size() == 1) { out[0] = (char)toupper((unsigned char)k[0]); out[1] = 0; return; }
    std::string u = k; for (char &c : u) c = (char)toupper((unsigned char)c);
    snprintf(out, 128, "KEY_%s", u.c_str());
}
int __cdecl CL_GetKeyBinding(int, const char *command, char (*keyNames)[128], int bindNum)
{
    keyNames[0][0] = keyNames[1][0] = 0; BindsRead();
    int n = 0;
    if (command) for (const Bind &b : s_binds) {
        if (I_stricmp(b.cmd.c_str(), command)) continue;
        if (bindNum == 1 && b.slot != 1) continue;
        bool dup = false; for (int i = 0; i < n; ++i) { char t[128]; KeyDisplayName(b.key, t); dup |= !strcmp(t, keyNames[i]); } if (dup) continue;
        KeyDisplayName(b.key, keyNames[n]); if (++n == 2) break;
    }
    // el original las da en orden de keynum: letras/cifras (ASCII) antes que raton/F1.. (>127); aproximado sin la tabla de keynums
    if (n == 2 && keyNames[1][1] == 0 && keyNames[0][1] != 0) { char t[128]; strcpy(t, keyNames[0]); strcpy(keyNames[0], keyNames[1]); strcpy(keyNames[1], t); }
    if (!n) strcpy(keyNames[0], "KEY_UNBOUND");
    return n;
}
int __cdecl Key_IsDown(int, int keynum) { return keynum >= 0 && keynum < 256 && s_keyDown[keynum]; }
void H10_Key(int key, bool down)
{
    if (key >= 0 && key < 256) s_keyDown[key] = down;
    if (!s_open) return;
    PlaceScope ps; menuDef_t *m = Menu_GetFocused(s_dc);
    if (!m) { H10_Close(); return; }
    if (getenv("H10_TRACE") && down) { itemDef_s *f = nullptr; for (int i = 0; i < m->itemCount && !f; ++i) if (Window_HasFocus(s_dc->contextIndex, &m->items[i]->window)) f = m->items[i].get(); fprintf(stderr, "[H10] tecla %d en %s: foco %s (%s)\n", key, m->window.name.get(), f && f->window.name ? f->window.name.get() : "-", f && f->dvar ? Dvar_GetVariantString(f->dvar) : ""); }
    Menu_HandleKey(0, s_dc, m, key, down);
    if (!Menu_Count(s_dc) || !s_dc->openMenuCount) { fprintf(stderr, "[H10] menus cerrados\n"); s_open = false; g_h5pPause = false; }
}

// ---------------------------------------------------------------- piloto de entrada (pruebas en el Mac): "t accion;..."
// acciones: open <menu> | move x y | tap x y (raton; x,y en fraccion 0..1) | down/drag/up x y (dedo) | key <n> | set <dvar> <valor> | save | shot <tag> | close
struct PilotEv { double t; std::string a; };
static std::string s_shot;
static std::vector<PilotEv> s_pilot; static size_t s_pilotI; static double s_t0;
static void PilotParse(const char *p)
{
    while (p && *p) {
        const char *e = strchr(p, ';'); std::string s(p, e ? e - p : strlen(p));
        double t; int n = 0; if (sscanf(s.c_str(), "%lf %n", &t, &n) == 1) s_pilot.push_back({ t, s.substr(n) });
        p = e ? e + 1 : nullptr;
    }
}
void H10_Touch(int phase, float fx, float fy);
static void PilotStep()
{
    // tiempo del piloto = frames de render / 60 (determinista: el harness sin app no va a tiempo real)
    static int frame = 0; const double t = ++frame / 60.0;
    while (s_pilotI < s_pilot.size() && s_pilot[s_pilotI].t <= t) {
        const std::string &a = s_pilot[s_pilotI++].a; char w[64] = ""; float x = 0, y = 0; int k = 0;
        fprintf(stderr, "[H10] piloto t=%.2f: %s\n", t, a.c_str());
        if (sscanf(a.c_str(), "open %63s", w) == 1) H10_Open(w);
        else if (sscanf(a.c_str(), "move %f %f", &x, &y) == 2) H10_Pointer(x * s_W, y * s_H);
        else if (sscanf(a.c_str(), "tap %f %f", &x, &y) == 2) { H10_Pointer(x * s_W + 2, y * s_H); H10_Pointer(x * s_W, y * s_H); H10_Key(K_MOUSE1, true); H10_Key(K_MOUSE1, false); }
        else if (sscanf(a.c_str(), "key %d", &k) == 1) { H10_Key(k, true); H10_Key(k, false); }
        else if (sscanf(a.c_str(), "pad %d", &k) == 1) { void H10_Pad(int); H10_Pad(k); }   // mando: 0 arriba 1 abajo 2 izq 3 der 4 A 5 B 6 START
        else if (sscanf(a.c_str(), "shot %63s", w) == 1) s_shot = std::string("_h10_") + w;   // tras presentar este frame (H10_PostFrame)
        else if (sscanf(a.c_str(), "down %f %f", &x, &y) == 2) H10_Touch(0, x, y);   // dedo (fracciones de la zona de juego)
        else if (sscanf(a.c_str(), "drag %f %f", &x, &y) == 2) H10_Touch(1, x, y);
        else if (sscanf(a.c_str(), "up %f %f", &x, &y) == 2) H10_Touch(2, x, y);
        else if (sscanf(a.c_str(), "set %63s %n", w, &k) == 1) Dvar_SetFromStringByName(w, (char *)a.c_str() + k);
        else if (sscanf(a.c_str(), "active %d", &k) == 1) { int r = UI_SetActiveMenu(0, (uiMenuCommand_t)k); fprintf(stderr, "[H10] UI_SetActiveMenu(%d) = %d, arriba %s\n", k, r, s_dc && s_dc->openMenuCount ? s_dc->menuStack[s_dc->openMenuCount - 1].menu->window.name.get() : "-"); }
        else if (sscanf(a.c_str(), "keys %63[^\n]", w) == 1) { char kn[2][128]; int r = CL_GetKeyBinding(0, w, kn, 0); fprintf(stderr, "[H10] CL_GetKeyBinding(%s) = %d: '%s' '%s'\n", w, r, kn[0], r > 1 ? kn[1] : ""); }
        else if (a == "safe") { float o[4] = {}; bool r = g_h10GetSafeInsets && g_h10GetSafeInsets(s_W, s_H, o); fprintf(stderr, "[H10] g_h10GetSafeInsets(%d,%d) = %d: l%.0f t%.0f r%.0f b%.0f\n", s_W, s_H, r, o[0], o[1], o[2], o[3]); }
        else if (a == "save") { void H10_SaveNow(); H10_SaveNow(); }
        else if (a == "close") H10_Close();
    }
}

// ---------------------------------------------------------------- tactil: el dedo se traduce a los eventos de raton del UI original
// down: si cae en un slider se pulsa (salta el pomo) y se vuelve a pulsar para arrastrarlo (captura de Scroll_Slider_ThumbFunc);
// si no, el clic se hace al levantar (asi un arrastre vertical desplaza listas con la rueda y no activa nada). fx,fy en fraccion 0..1
// de la zona de juego (formato DS: mitad de arriba; horizontal: pantalla entera).
// ---------------------------------------------------------------- objetivos interactivos (tactil con tolerancia y mando)
// Items que se pueden usar: botones con 'action', listas, sliders, si/no, multi y enum visibles. Con un popup encima solo cuenta el popup.
static bool HasAction(itemDef_s *it) { for (GenericEventHandler *h = it->onEvent.get(); h; h = h->next.get()) if (h->name && !I_stricmp(h->name.get(), "action") && h->eventScript) return true; return false; }
static bool Interactive(itemDef_s *it)
{
    if (it->window.rect.w < 2 || it->window.rect.h < 2 || !Item_IsVisible(0, s_dc->contextIndex, it)) return false;
    switch (it->type) { case 4: return true; case 8: case 9: case 10: case 11: return it->dvar != nullptr; }
    return HasAction(it);   // botones: tipo 0 (texto) o 1 (boton) con 'action'

}
static bool IsPopup(menuDef_t *m) { const char *n = m && m->window.name ? m->window.name.get() : ""; return strstr(n, "popmenu") || strstr(n, "popup"); }
static void NavItems(std::vector<itemDef_s *> &v)
{
    v.clear(); menuDef_t *f = Menu_GetFocused(s_dc); const bool popup = IsPopup(f);
    for (int k = 0; k < s_dc->openMenuCount; ++k) { menuDef_t *m = s_dc->menuStack[k].menu; if (!m || (popup && m != f) || !Menu_IsVisible(0, s_dc, m)) continue;
        for (int i = 0; i < m->itemCount; ++i) if (Interactive(m->items[i].get())) v.push_back(m->items[i].get()); }
}
static void ScreenRect(itemDef_s *it, float r[4])   // pixeles del motor (con la colocacion activa: llamar dentro de PlaceScope)
{
    r[0] = it->window.rect.x; r[1] = it->window.rect.y; r[2] = it->window.rect.w; r[3] = it->window.rect.h;
    ScrPlace_ApplyRect(&scrPlaceView[0], &r[0], &r[1], &r[2], &r[3], it->window.rect.horzAlign, it->window.rect.vertAlign);
}
// radio de tolerancia del dedo: ~3 mm (la mitad de los 44 pt minimos de un objetivo tactil); sin dpi, 3.5 % del lado corto
static float TouchSlop() { return g_h10Dpi > 0 ? 0.12f * g_h10Dpi : 0.035f * (s_W < s_H ? s_W : s_H); }
static itemDef_s *s_padIt;
static void PointAt(itemDef_s *it)
{
    float r[4]; ScreenRect(it, r); const float px = r[0] + r[2] * 0.5f, py = r[1] + r[3] * 0.5f, d = (s_W / 640.0f) * 2;
    H10_Pointer(px + d, py); H10_Pointer(px, py);   // en dos pasos: Display_MouseMove ignora la posicion repetida tras abrir un menu
}
static void H10_Toggle();
static bool TouchScroll(float dy, float x = NAN);   // disposicion tactil (mas abajo)
static bool PadScroll(itemDef_s *cur, itemDef_s *best, int b);
// Mando (Android: D-pad/sticks/A/B/START; tambien flechas del teclado en el Mac con H10_PAD=1): navegacion espacial sobre los mismos
// items que el raton (el foco es el "hover" del original, con su resaltado). b: 0 arriba 1 abajo 2 izq 3 der 4 A 5 B 6 START
void H10_Pad(int b)
{
    if (b == 6) { H10_Toggle(); return; }
    if (!s_open || !s_dc) return;
    PlaceScope ps;
    if (b == 5) { H10_Key(K_ESCAPE, true); if (s_open) H10_Key(K_ESCAPE, false); return; }
    std::vector<itemDef_s *> v; NavItems(v);
    if (v.empty()) { if (getenv("H10_TRACE")) { fprintf(stderr, "[H10] mando %d: sin objetivos (%d menus abiertos)\n", b, s_dc->openMenuCount);
        for (int k = 0; k < s_dc->openMenuCount; ++k) { menuDef_t *m = s_dc->menuStack[k].menu; int n = 0, a = 0; for (int i = 0; m && i < m->itemCount; ++i) { n += Item_IsVisible(0, s_dc->contextIndex, m->items[i].get()) ? 1 : 0; a += HasAction(m->items[i].get()); }
            fprintf(stderr, "[H10]   %s visible=%d items visibles %d con accion %d\n", m && m->window.name ? m->window.name.get() : "-", m ? (int)Menu_IsVisible(0, s_dc, m) : -1, n, a); } } return; }
    bool cur = false; for (itemDef_s *i : v) if (i == s_padIt) cur = true; if (!cur) s_padIt = nullptr;
    if (b == 4) { if (!s_padIt) return; PointAt(s_padIt); H10_Key(K_MOUSE1, true); if (s_open) H10_Key(K_MOUSE1, false); return; }
    if (s_padIt && (b == 2 || b == 3)) {   // izq/der sobre un valor: lo cambia el propio item (Item_*_HandleKey, como con las flechas)
        const int key = b == 2 ? K_LEFTARROW : K_RIGHTARROW; int done = 0; PointAt(s_padIt);
        switch (s_padIt->type) { case 8: done = Item_Slider_HandleKey(s_dc, s_padIt, key); break; case 9: done = Item_YesNo_HandleKey(s_dc, s_padIt, key); break;
            case 10: done = Item_Multi_HandleKey(s_dc, s_padIt, key); break; case 11: done = Item_DvarEnum_HandleKey(s_dc, s_padIt, key); break; }
        if (done) return;
    }
    if (s_padIt && s_padIt->type == 4 && (b == 0 || b == 1)) { const int k = b ? K_DOWNARROW : K_UPARROW; H10_Key(k, true); H10_Key(k, false); return; }
    itemDef_s *best = nullptr; float bs = 1e30f, c[4];
    if (!s_padIt) {   // primera pulsacion: el primer item del menu enfocado (arriba a la izquierda)
        menuDef_t *f = Menu_GetFocused(s_dc);
        for (itemDef_s *i : v) { ScreenRect(i, c); const float sc = c[1] * 4 + c[0] + (i->parent == f ? 0 : 1e6f); if (sc < bs) { bs = sc; best = i; } }
    } else {
        float o[4]; ScreenRect(s_padIt, o); const float ox = o[0] + o[2] * 0.5f, oy = o[1] + o[3] * 0.5f;
        for (itemDef_s *i : v) { if (i == s_padIt) continue; ScreenRect(i, c); const float dx = c[0] + c[2] * 0.5f - ox, dy = c[1] + c[3] * 0.5f - oy; float sc;
            switch (b) { case 0: if (dy > -1) continue; sc = -dy + 2.5f * fabsf(dx); break; case 1: if (dy < 1) continue; sc = dy + 2.5f * fabsf(dx); break;
                case 2: if (dx > -1) continue; sc = -dx + 1.5f * fabsf(dy); break; default: if (dx < 1) continue; sc = dx + 1.5f * fabsf(dy); break; }
            if (sc < bs) { bs = sc; best = i; } }
    }
    {   // el panel tactil del item actual tiene mas filas fuera de la ventana en esa direccion y lo mejor esta fuera del panel (p.ej. Back):
        // primero se desplaza el panel y se vuelve a buscar
        static int depth; if ((b == 0 || b == 1) && depth < 4 && PadScroll(s_padIt, best, b)) { ++depth; H10_Pad(b); --depth; return; }
    }
    if (!best) {   // nada mas en esa direccion: si el panel tactil tiene mas filas fuera de la ventana, se desplaza y se vuelve a buscar
        static int depth; if ((b == 0 || b == 1) && depth == 0 && TouchScroll(b ? 40.0f : -40.0f)) { ++depth; H10_Pad(b); --depth; }
        return;
    }
    s_padIt = best; PointAt(best);
    if (getenv("H10_TRACE")) { float q[4]; ScreenRect(best, q); fprintf(stderr, "[H10] mando %d -> item tipo %d de %s en (%.0f,%.0f %.0fx%.0f)\n", b, best->type, best->parent && best->parent->window.name ? best->parent->window.name.get() : "-", q[0], q[1], q[2], q[3]); }
}
static struct { bool on, slider, scrolled; float y0, ystep, ox, oy; } s_tc;
static itemDef_s *ItemAt(float vx, float vy, int type)
{
    menuDef_t *m = Menu_GetFocused(s_dc); if (!m) return nullptr;
    for (int i = m->itemCount - 1; i >= 0; --i) { itemDef_s *it = m->items[i].get();
        if ((type < 0 || it->type == type) && Item_IsVisible(0, s_dc->contextIndex, it) && Rect_ContainsPoint(s_dc->contextIndex, &it->window.rect, vx, vy)) return it; }
    return nullptr;
}
static bool MenuHasList() { menuDef_t *m = Menu_GetFocused(s_dc); if (!m) return false; for (int i = 0; i < m->itemCount; ++i) if (m->items[i]->type == 4 && Item_IsVisible(0, s_dc->contextIndex, m->items[i].get())) return true; return false; }
void H10_Touch(int phase, float fx, float fy)
{
    if (!s_open) return;
    PlaceScope ps; s_padIt = nullptr;
    float px = fx * s_W, py = fy * s_H;
    if (phase == 0) {
        // tolerancia del dedo: si no cae dentro de ningun objetivo pero hay uno a menos de TouchSlop(), el toque se lleva a ese objetivo
        s_tc.ox = s_tc.oy = 0; std::vector<itemDef_s *> v; NavItems(v); itemDef_s *near = nullptr; float bd = TouchSlop(), c[4];
        for (itemDef_s *it : v) { ScreenRect(it, c); const float dx = px < c[0] ? c[0] - px : px > c[0] + c[2] ? px - c[0] - c[2] : 0, dy = py < c[1] ? c[1] - py : py > c[1] + c[3] ? py - c[1] - c[3] : 0;
            const float d = hypotf(dx, dy); if (d == 0) { near = nullptr; break; } if (d < bd) { bd = d; near = it; } }
        if (near) { ScreenRect(near, c); const float nx = fminf(fmaxf(px, c[0] + 2), c[0] + c[2] - 2), ny = fminf(fmaxf(py, c[1] + 2), c[1] + c[3] - 2);
            if (getenv("H10_TRACE")) fprintf(stderr, "[H10] dedo fuera de objetivo: llevado %.0f px al item tipo %d\n", bd, near->type);
            s_tc.ox = nx - px; s_tc.oy = ny - py; }
    }
    px += s_tc.ox; py += s_tc.oy; float vx, vy; ToVirtual(px, py, &vx, &vy);
    if (phase == 0) {
        // el dedo salta (no hay movimiento previo como con el raton): el primer Display_MouseMove tras abrir el menu no da foco
        // (Menus_Open deja mouseLocation en INT_MIN y Display_MouseMove ignora posiciones repetidas), asi que se acerca en dos pasos
        { const float d = (s_W / 640.0f) * 2; H10_Pointer(px + d, py); }
        H10_Pointer(px, py); s_tc = { true, ItemAt(s_dc->cursor.x, s_dc->cursor.y, 8) != nullptr, false, vy, vy, s_tc.ox, s_tc.oy };
        if (getenv("H10_TRACE")) { itemDef_s *it = ItemAt(s_dc->cursor.x, s_dc->cursor.y, -1); { menuDef_t *m = Menu_GetFocused(s_dc); for (int i = 0; m && i < m->itemCount; ++i) { itemDef_s *q = m->items[i].get(); if (Item_IsVisible(0, s_dc->contextIndex, q) && Rect_ContainsPoint(s_dc->contextIndex, &q->window.rect, s_dc->cursor.x, s_dc->cursor.y)) fprintf(stderr, "[H10]   bajo el dedo: I%d tipo %d %s dyn=%x\n", i, q->type, q->window.name ? q->window.name.get() : "-", q->window.dynamicFlags[0]); } }
            fprintf(stderr, "[H10] dedo (%.0f,%.0f) virtual (%.1f,%.1f): item %s tipo %d%s\n", px, py, s_dc->cursor.x, s_dc->cursor.y, it && it->window.name ? it->window.name.get() : "-", it ? it->type : -1, s_tc.slider ? " (slider)" : ""); }
        if (s_tc.slider) {
            // clic = el pomo salta al dedo (Scroll_Slider_SetThumbPos); despues se captura el slider como Item_StartCapture
            // aunque el dedo no caiga en los 10 px del pomo (con el raton hay que acertarle; con el dedo se arrastra desde donde se toca)
            H10_Key(K_MOUSE1, true); H10_Key(K_MOUSE1, false);
            extern itemDef_s *itemCapture; extern void *captureData; extern void(__cdecl *captureFunc)(int, UiContext *, void *); extern scrollInfo_s scrollInfo;
            if (itemDef_s *it = ItemAt(s_dc->cursor.x, s_dc->cursor.y, 8)) {
                scrollInfo.scrollKey = K_MOUSE1; scrollInfo.item = it; scrollInfo.xStart = s_dc->cursor.x; scrollInfo.yStart = s_dc->cursor.y;
                captureData = &scrollInfo; captureFunc = (void(__cdecl *)(int, UiContext *, void *))Scroll_Slider_ThumbFunc; itemCapture = it;
            }
        }
    } else if (phase == 1 && s_tc.on) {
        H10_Pointer(px, py);
        if (!s_tc.slider) {
            if (fabsf(vy - s_tc.y0) > 12) s_tc.scrolled = true;
            if (s_tc.scrolled && !MenuHasList()) { TouchScroll(s_tc.ystep - vy, px); s_tc.ystep = vy; }   // panel tactil mas alto que la pantalla
            else if (s_tc.scrolled && MenuHasList()) while (fabsf(vy - s_tc.ystep) >= 16) { const int k = vy < s_tc.ystep ? K_MWHEELDOWN : K_MWHEELUP; H10_Key(k, true); H10_Key(k, false); s_tc.ystep += vy < s_tc.ystep ? -16 : 16; }
        }
    } else if (phase == 2 && s_tc.on) {
        H10_Pointer(px, py);
        if (s_tc.slider) { extern itemDef_s *itemCapture; extern void *captureData; extern void(__cdecl *captureFunc)(int, UiContext *, void *);
            if (captureFunc) captureFunc(0, s_dc, captureData);   // ultima posicion del dedo
            itemCapture = nullptr; captureFunc = nullptr; captureData = nullptr; }
        else if (!s_tc.scrolled) { H10_Key(K_MOUSE1, true); if (s_open) H10_Key(K_MOUSE1, false); }
        s_tc.on = false;
    }
}

// ---------------------------------------------------------------- perfiles de calidad
// Bajo/Medio/Alto agrupan lo que el port soporta de verdad; tocar un valor suelto pasa el perfil a Personalizado (3).
struct Prof { int scale, picmip, maxTex, aniso; const char *mip; int postfx, fps; float fov; int shadows; };   // fps: com_maxfps (0 = sin limite; con vsync manda la pantalla); fov: cg_fov_default (65-80 del original)
#ifdef __ANDROID__
// Fold (presupuesto RSS 1500 MB, >=55 fps): Alto = la configuracion medida hoy (picmip 0, V9_MAXTEX 512, resolucion nativa, postfx)
// fps: Bajo 30 (bateria/temperatura), Medio y Alto 60 (el presupuesto pide >=55; 90/120 quedan a mano para la pantalla de 120 Hz)
// shadows (OLA 9): focos con mapa de sombra (sm_enable + sm_maxLights del original). En el Fold Alto va con 2 hasta medirlo en el
// dispositivo (en el M1 4 focos cuestan ~0,7 ms/frame); 4 = el original, a mano (fila SOMBRAS / sm_maxLights en config.cfg).
static const Prof kProf[3] = { { 67, 2, 256, 1, "Force Bilinear", 0, 30, 65, 0 }, { 85, 1, 512, 4, "Force Trilinear", 1, 60, 65, 0 }, { 100, 0, 512, 4, "Force Trilinear", 1, 60, 65, 2 } };
#else
// fps: Bajo y Medio 60, Alto sin limite (lo marca el vsync de la pantalla)
// shadows (OLA 9): Bajo sin sombras, Medio 2 focos, Alto 4 (= sm_maxLights del original)
static const Prof kProf[3] = { { 67, 2, 256, 1, "Force Bilinear", 0, 60, 65, 0 }, { 85, 1, 512, 4, "Force Trilinear", 1, 60, 65, 2 }, { 100, 0, 1024, 16, "Force Trilinear", 1, 0, 65, 4 } };
#endif
static const int kDefQuality = 2;
static int DefQuality() { const char *e = getenv("H10_QUALITY"); int q = e ? atoi(e) : kDefQuality; return q < 0 || q > 2 ? kDefQuality : q; }
static int DI(const char *n) { return atoi(Dvar_GetVariantString(n)); }
static void DSet(const char *n, const char *v) { Dvar_SetFromStringByName(n, (char *)v); }
static void DSetI(const char *n, int v) { char b[32]; snprintf(b, sizeof b, "%d", v); DSet(n, b); }
static int PendingPicmip() { const dvar_s *d = Dvar_FindVar("ui_r_picmip"); if (!d) return DI("r_picmip_manual") ? DI("r_picmip") : -1; return atoi(Dvar_GetVariantString("ui_r_picmip")); }
static void ApplyProf(int q)
{
    const Prof &p = kProf[q];
    DSetI("h10_renderScale", p.scale); DSetI("h10_maxTex", p.maxTex); DSetI("h10_postfx", p.postfx);
    DSetI("r_texFilterAnisoMin", p.aniso); DSet("r_texFilterMipMode", p.mip); DSetI("com_maxfps", p.fps); { char b[16]; snprintf(b, sizeof b, "%g", p.fov); DSet("cg_fov_default", b); }
    DSetI("ui_r_picmip", p.picmip); DSetI("ui_r_picmip_bump", p.picmip); DSetI("ui_r_picmip_spec", p.picmip); DSetI("ui_r_picmip_manual", 1);
    DSetI("sm_enable", p.shadows > 0); if (p.shadows > 0) DSetI("sm_maxLights", p.shadows);   // OLA 9 (h9_shadow.cpp)
    fprintf(stderr, "[H10] perfil %d: escala %d%% picmip %d maxtex %d aniso %d %s postfx %d fps %d fov %g sombras %d\n", q, p.scale, p.picmip, p.maxTex, p.aniso, p.mip, p.postfx, p.fps, p.fov, p.shadows);
}
static void ApplyDefaults() { const int q = DefQuality(); DSetI("h10_quality", q); ApplyProf(q); fprintf(stderr, "[H10] ajustes por defecto: perfil %d de la plataforma\n", q); }
static bool MatchProf(int q)
{
    const Prof &p = kProf[q];
    return DI("h10_renderScale") == p.scale && DI("h10_maxTex") == p.maxTex && DI("h10_postfx") == p.postfx && DI("r_texFilterAnisoMin") == p.aniso
        && !strcmp(Dvar_GetVariantString("r_texFilterMipMode"), p.mip) && PendingPicmip() == p.picmip && DI("com_maxfps") == p.fps
        && fabsf((float)atof(Dvar_GetVariantString("cg_fov_default")) - p.fov) < 0.5f
        && (DI("sm_enable") ? DI("sm_maxLights") : 0) == p.shadows;
}

// ---------------------------------------------------------------- config (formato original: players/config.cfg con "seta nombre "valor"")
static std::string CfgPath()
{
    if (const char *c = getenv("H10_CFG")) return c;
    const char *gd = getenv("P3_GAMEDATA"); return std::string(gd ? gd : "../../gamedata") + "/players/" + "config.cfg";   // Com_PlayerConfigName (zombies)
}
static std::vector<std::pair<std::string, std::string>> s_cfg; static bool s_cfgFound, s_cfgRead;
static void CfgRead()
{
    if (s_cfgRead) return; s_cfgRead = true;
    FILE *f = fopen(CfgPath().c_str(), "r"); if (!f) { fprintf(stderr, "[H10] config: no hay %s (primera ejecucion: perfil de serie %d)\n", CfgPath().c_str(), DefQuality()); return; }
    s_cfgFound = true; char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "seta ", 5)) continue;
        char *n = line + 5, *e = strchr(n, ' '); if (!e) continue; *e = 0; char *v = strchr(e + 1, '"'); if (!v) continue; ++v; char *z = strchr(v, '"'); if (z) *z = 0;
        s_cfg.push_back({ n, v });
    }
    fclose(f); fprintf(stderr, "[H10] config: %zu valores de %s\n", s_cfg.size(), CfgPath().c_str());
}
static const char *CfgGet(const char *n) { for (auto &kv : s_cfg) if (!I_stricmp(kv.first.c_str(), n)) return kv.second.c_str(); return nullptr; }
// solo se cargan los ajustes de este menu (el fichero lleva todos los dvars archivados, como el original)
static const char *const kCfgKeys[] = { "h10_quality", "h10_renderScale", "h10_maxTex", "h10_postfx", "h10_fog", "h10_reflections", "r_picmip", "r_picmip_bump", "r_picmip_spec",
    "r_picmip_manual", "r_texFilterAnisoMin", "r_texFilterMipMode", "com_maxfps", "cg_fov_default", "sm_enable", "sm_maxLights" };
static void CfgApply(bool onlyExisting)
{
    for (const char *k : kCfgKeys) if (const char *v = CfgGet(k)) if (!onlyExisting || Dvar_FindVar(k)) Dvar_SetCommand(k, (char *)v);
}
// OLA 15C: volumenes del menu SOUND original (snd_menu_*, snd_dvar.cpp; los aplica SND_SetVoiceAttenuation al cuadrado por grupo).
// Se registran en SND_Init, despues de CfgApply: h5_audio_cl.cpp llama a esto justo tras SND_Init.
void H10_CfgApplySound()
{
    CfgRead();
    static const char *const kSnd[] = { "snd_menu_master", "snd_menu_music", "snd_menu_sfx", "snd_menu_voice", "snd_menu_cinematic" };
    int n = 0; for (const char *k : kSnd) if (const char *v = CfgGet(k)) if (Dvar_FindVar(k)) { Dvar_SetCommand(k, (char *)v); ++n; }
    if (n) fprintf(stderr, "[H10] config: %d volumenes de sonido aplicados (snd_menu_master %s, music %s, sfx %s, voice %s)\n", n,
        CfgGet("snd_menu_master") ? CfgGet("snd_menu_master") : "1", CfgGet("snd_menu_music") ? CfgGet("snd_menu_music") : "1",
        CfgGet("snd_menu_sfx") ? CfgGet("snd_menu_sfx") : "1", CfgGet("snd_menu_voice") ? CfgGet("snd_menu_voice") : "1");
}
static FILE *s_fsf[2];
void FS_Printf(int h, char *fmt, ...) { if (h != 1 || !s_fsf[1]) return; va_list a; va_start(a, fmt); vfprintf(s_fsf[1], fmt, a); va_end(a); }
static void CfgSave()
{
    const std::string p = CfgPath(), t = p + ".tmp"; { std::string d = p.substr(0, p.rfind('/')); mkdir(d.c_str(), 0755); }
    FILE *f = fopen(t.c_str(), "w"); if (!f) { fprintf(stderr, "[H10] config: no se puede escribir %s\n", t.c_str()); return; }
    fprintf(f, "// generated by Call of Duty, do not modify\n");   // Com_WriteConfigToFile (sin bindings: el port no tiene Key_WriteBindings)
    s_fsf[1] = f; Dvar_WriteVariables(1); s_fsf[1] = nullptr; fclose(f); rename(t.c_str(), p.c_str());
    fprintf(stderr, "[H10] config guardada en %s (calidad %d)\n", p.c_str(), DI("h10_quality"));
}

// antes del backend (main2): resolucion interna y tope de texturas se aplican al arrancar (como el vid_restart del original)
static int s_bootScale = 100, s_bootMaxTex = 512, s_bootPicmip = -1;
void H10_PreBackend(int *W, int *H)
{
    const char *e = getenv("H10"); if (e && !atoi(e)) return;
    CfgRead();
    const Prof &p = kProf[DefQuality()]; int scale = p.scale, maxTex = p.maxTex;
    if (const char *v = CfgGet("h10_renderScale")) scale = atoi(v);
    if (const char *v = CfgGet("h10_maxTex")) maxTex = atoi(v);
    if (scale < 25 || scale > 100) scale = 100;
    if (maxTex < 64 || maxTex > 4096) maxTex = 512;
    s_bootScale = scale; s_bootMaxTex = maxTex;
    if (scale != 100) {
        *W = (*W * scale / 100) & ~1; *H = (*H * scale / 100) & ~1; char b[16];
        snprintf(b, sizeof b, "%d", *W); setenv("P4_W", b, 1); snprintf(b, sizeof b, "%d", *H); setenv("P4_H", b, 1);
    }
    char b[16]; snprintf(b, sizeof b, "%d", maxTex); setenv("V9_MAXTEX", b, 1);
    fprintf(stderr, "[H10] arranque: resolucion interna %d%% -> %dx%d, V9_MAXTEX %d\n", scale, *W, *H, maxTex);
}
// tras R_RegisterDvars (antes de cargar imagenes): picmip / filtro del config
void H10_PreInit()
{
    const char *e = getenv("H10"); if (e && !atoi(e)) return;
    CfgRead();
    if (s_cfgFound) CfgApply(true);
    else { const Prof &p = kProf[DefQuality()]; DSetI("r_picmip_manual", 1); DSetI("r_picmip", p.picmip); DSetI("r_picmip_bump", p.picmip); DSetI("r_picmip_spec", p.picmip); }
    s_bootPicmip = DI("r_picmip_manual") ? DI("r_picmip") : -1;
}

// ---------------------------------------------------------------- comandos de los scripts del menu
extern bool g_h10PostFxOff; extern float g_h10FovX;   // h7_gfx.cpp / world_h4.cpp
float H7C_FovX(int W, int H);
void (*g_h10RestartApp)();          // la app Android lo apunta a RestartApp (el motor no es reentrante)
static cmd_function_s s_cmdVid, s_cmdPic, s_cmdProf, s_cmdAnim, s_cmdRestart, s_cmdQuit, s_cmdFade, s_cmdSilence;
static void Cmd_H10VidRestart()
{
    CfgSave();
    fprintf(stderr, "[H10] vid_restart: config guardada; resolucion/texturas se aplican al reiniciar%s\n", g_h10RestartApp ? " (reinicio la app)" : " (relanza el juego)");
    if (g_h10RestartApp) g_h10RestartApp();
    s_bootScale = DI("h10_renderScale"); s_bootMaxTex = DI("h10_maxTex"); s_bootPicmip = PendingPicmip();   // ya guardado: no se vuelve a pedir Aplicar
}
void (*g_h10QuitApp)();             // la app Android: cerrar la actividad
// pausedmenu: "Restart level" (fast_restart) y "Quit" (disconnect). El motor no es reentrante: en Android se relanza la app (partida nueva)
// o se cierra; en el Mac (arnes) se cierra el proceso si hay ventana y si no solo se registra.
static void Cmd_H10FastRestart() { fprintf(stderr, "[H10] fast_restart: partida nueva%s\n", g_h10RestartApp ? " (reinicio la app)" : " (en el arnes: sin efecto)"); CfgSave(); if (g_h10RestartApp) g_h10RestartApp(); }
static void Cmd_H10Disconnect()
{
    fprintf(stderr, "[H10] disconnect: salir del juego\n"); CfgSave();
    if (g_h10QuitApp) g_h10QuitApp();
    else if (getenv("P4_WINDOW")) { fflush(stderr); exit(0); }
}
static void Cmd_H10ApplyPicmip() { fprintf(stderr, "[H10] r_applyPicmip: picmip %d al reiniciar\n", DI("r_picmip")); }
static void Cmd_H10Profile() { CfgSave(); }
static void Cmd_Nop() {}
void H10_SaveNow() { CfgSave(); }
static void AddCmd(const char *n, void (*f)(), cmd_function_s *c, bool replace) { if (Cmd_FindCommand(n)) { if (!replace) return; Cmd_RemoveCommand(n); } Cmd_AddCommandInternal(n, f, c); }

// ---------------------------------------------------------------- filas del menu original options_graphics
// cada fila N son 5 items: barra, barra resaltada ('ui_highlight' == N), etiqueta, etiqueta resaltada y valor
static menuDef_t *s_og; static ExpressionStatement s_never, s_hl; static bool s_neverOk;
static int RowBase(int n) { static const int b[] = { 63, 68, 73, 83, 88, 93, 98, 103, 108, 113, 118, 123, 128, 133, 138 }; return b[n - 22]; }
static itemDef_s *It(int i) { return s_og->items[i].get(); }
static ExpressionStatement HlExp(int n)
{
    ExpressionStatement e = s_hl; expressionRpn *r = (expressionRpn *)Ptr32_ArenaAlloc(sizeof(expressionRpn) * e.numRpn, 4); memcpy(r, s_hl.rpn.get(), sizeof(expressionRpn) * e.numRpn);
    for (int i = 0; i < e.numRpn; ++i) if (r[i].type == 0 && r[i].data.constant.dataType == VAL_INT && r[i].data.constant.internals.intVal == 28) r[i].data.constant.internals.intVal = n;
    e.rpn = r; return e;
}
static void Hide(int a, int b) { for (int i = a; i <= b; ++i) It(i)->visibleExp = s_never; }
static void EnableRow(int n, int base)
{
    for (int k : { 0, 2, 4 }) It(base + k)->visibleExp.filename = nullptr;   // sin expresion = visible
    It(base + 1)->visibleExp = HlExp(n); It(base + 3)->visibleExp = HlExp(n);
}
static void Label(int base, const char *txt)
{
    for (int k : { 2, 3 }) { textDef_s *t = It(base + k)->typeData.textDef.get(); if (!t || !t->textExpData) continue;
        ExpressionStatement *e = (ExpressionStatement *)t->textExpData.get(); expressionRpn *r = (expressionRpn *)Ptr32_ArenaAlloc(sizeof(expressionRpn) * e->numRpn, 4); memcpy(r, e->rpn.get(), sizeof(expressionRpn) * e->numRpn);
        for (int i = 0; i < e->numRpn; ++i) if (r[i].type == 0 && r[i].data.constant.dataType == VAL_STRING) r[i].data.constant.internals.string = (char *)Ptr32_ArenaStrdup(txt);
        e->rpn = r; }
}
static const char *A(const char *s) { return Ptr32_ArenaStrdup(s); }
static void Multi(int base, const char *dvar, std::initializer_list<std::pair<const char *, float>> opts)
{
    itemDef_s *it = It(base + 4); multiDef_s *m = (multiDef_s *)Ptr32_ArenaAlloc(sizeof(multiDef_s), 4); int k = 0;
    for (auto &o : opts) { m->dvarList[k] = o.first[0] == '@' || o.first[0] == '\x01' ? o.first : A(o.first); m->dvarStr[k] = A(""); m->dvarValue[k] = o.second; ++k; }
    m->count = k; m->strDef = 0; it->type = 10; it->dataType = 10; it->dvar = (char *)A(dvar);
    it->typeData.textDef->textTypeData.focusItemDef->focusTypeData.multi = m;
}
static void MoveRow(int base, float y, float h)
{
    for (int k = 0; k < 5; ++k) { itemDef_s *it = It(base + k); it->window.rectClient.y += y - it->window.rect.y; it->window.rect.y = y; it->window.rect.h = h; it->window.rectClient.h = h; }
}
// options_new_pc (raiz de ajustes): sin VOICE CHAT (el port no tiene voz: Voice_GetVoiceLevel no existe) y la fila GAME sube a su sitio;
// '@PATCH_GAME_CAPS' no esta en las tablas de texto del port: se escribe "GAME".
Font_s *__cdecl Item_GetFont(int contextIndex, const itemDef_s *item); int __cdecl UI_TextHeight(Font_s *font, float scale);
static void SetTextConst(itemDef_s *it, const char *from, const char *to)
{
    textDef_s *t = Item_GetTextDef(it); if (!t || !t->textExpData) return;
    ExpressionStatement *e = (ExpressionStatement *)t->textExpData.get(); expressionRpn *r = e->rpn.get(); bool hit = false;
    for (int i = 0; i < e->numRpn; ++i) if (r[i].type == 0 && r[i].data.constant.dataType == VAL_STRING && r[i].data.constant.internals.string && !strcmp(r[i].data.constant.internals.string.get(), from)) hit = true;
    if (!hit) return;
    expressionRpn *n = (expressionRpn *)Ptr32_ArenaAlloc(sizeof(expressionRpn) * e->numRpn, 4); memcpy(n, r, sizeof(expressionRpn) * e->numRpn);
    for (int i = 0; i < e->numRpn; ++i) if (n[i].type == 0 && n[i].data.constant.dataType == VAL_STRING && n[i].data.constant.internals.string && !strcmp(n[i].data.constant.internals.string.get(), from)) n[i].data.constant.internals.string = (char *)Ptr32_ArenaStrdup(to);
    e->rpn = n;
}
static bool HasTextConst(itemDef_s *it, const char *str)
{
    textDef_s *t = Item_GetTextDef(it); if (!t || !t->textExpData) return false;
    ExpressionStatement *e = (ExpressionStatement *)t->textExpData.get(); expressionRpn *r = e->rpn.get();
    for (int i = 0; r && i < e->numRpn; ++i) if (r[i].type == 0 && r[i].data.constant.dataType == VAL_STRING && r[i].data.constant.internals.string && !strcmp(r[i].data.constant.internals.string.get(), str)) return true;
    return false;
}
// la columna izquierda esta repetida en options_new_pc y options_graphics_defaults (fondo del popup de valores por defecto): fila = 4 items
// (barra, barra resaltada, boton, etiqueta resaltada); se buscan por el texto de la etiqueta
static void PatchRootMenu()
{
    int fixed = 0;
    for (int k = 0; k < s_dc->menuCount; ++k) {
        menuDef_t *m = s_dc->Menus[k]; const char *name = m->window.name ? m->window.name.get() : "";
        if (strncmp(name, "options_", 8) || !strcmp(name, "options_controls_new_pc")) continue;   // todos los paneles repiten la columna
        int voice = -1, game = -1;
        for (int i = 3; i < m->itemCount; ++i) { if (HasTextConst(m->items[i].get(), "@MENU_VOICECHAT_CAPS")) voice = i; if (HasTextConst(m->items[i].get(), "@PATCH_GAME_CAPS")) game = i; }   // la ultima de la fila (etiqueta resaltada)
        if (voice < 3) continue;
        const float y = m->items[voice]->window.rect.y;
        for (int i = voice - 3; i <= voice; ++i) m->items[i]->visibleExp = s_never;
        if (game >= 0) for (int i = game - 3; i <= game; ++i) { windowDef_t &w = m->items[i]->window; w.rectClient.y += y - w.rect.y; w.rect.y = y; }
        for (int i = 0; i < m->itemCount; ++i) SetTextConst(m->items[i].get(), "@PATCH_GAME_CAPS", "GAME");
        if (getenv("H10_TRACE")) fprintf(stderr, "[H10] %s: sin VOICE CHAT (items %d-%d), GAME %s\n", name, voice - 3, voice, game >= 0 ? "en su sitio" : "no esta");
        ++fixed;
    }
    fprintf(stderr, "[H10] columna de ajustes sin VOICE CHAT y con GAME en %d paneles\n", fixed);
}
// pestana CONTROLS (asignacion de teclas y raton): el port se juega con el tactil y el mando, no hay cl_keys. Se quita la pestana
// (boton, marco y etiqueta de la pestana 2) y los atajos de flecha izq/der que llevan a ella desde cada panel de ajustes.
static bool ScriptHas(GenericEventScript *sc, const char *what) { for (; sc; sc = sc->next.get()) if (sc->action && strstr(sc->action.get(), what)) return true; return false; }
static bool HandlerHas(GenericEventHandler *h, const char *what) { for (; h; h = h->next.get()) if (ScriptHas(h->eventScript.get(), what)) return true; return false; }
static void PatchControlsTab()
{
    int keys = 0, tabs = 0, frames = 0;
    for (int k = 0; k < s_dc->menuCount; ++k) {
        menuDef_t *m = s_dc->Menus[k]; const char *n = m->window.name ? m->window.name.get() : "";
        if (strncmp(n, "options_", 7) || !strcmp(n, "options_controls_new_pc")) continue;
        PTR32(ItemKeyHandler) *pp = &m->onKey;   // atajos de menu (flechas) que abren los controles
        while (*pp) { ItemKeyHandler *h = pp->get(); if (ScriptHas(h->keyScript.get(), "options_controls_new_pc")) { *pp = h->next; ++keys; } else pp = &h->next; }
        for (int i = 0; i < m->itemCount; ++i) if (HandlerHas(m->items[i]->onEvent.get(), "options_controls_new_pc")) { m->items[i]->visibleExp = s_never; ++tabs; }
        // marco y etiqueta de la pestana 2 (items 13-16 'menu_mp_tab_frame_inner' y 19-20 de texto, mismo patron en todos los paneles)
        bool tpl = m->itemCount > 20 && m->items[19]->type == 3 && m->items[20]->type == 3;
        for (int i = 13; tpl && i <= 16; ++i) { windowDef_t &w = m->items[i]->window; tpl = w.background && w.background.get()->info.name && !strcmp(w.background.get()->info.name.get(), "menu_mp_tab_frame_inner"); }
        if (tpl) { for (int i = 13; i <= 16; ++i) m->items[i]->visibleExp = s_never; m->items[19]->visibleExp = s_never; m->items[20]->visibleExp = s_never; ++frames; }
    }
    fprintf(stderr, "[H10] pestana CONTROLS fuera: %d botones, %d atajos de teclado, %d marcos\n", tabs, keys, frames);
}
// ---------------------------------------------------------------- disposicion tactil generica (H10_TOUCH=1; por defecto en Android)
// Las filas de los menus (17 unidades de 480 = ~3 mm en un movil) se agrandan en vertical por un factor k para que cada objetivo mida
// >= 0.30 pulgadas (48 dp / 44 pt) con el dpi real (g_h10Dpi; H10_DPI en el Mac; sin dato, 7.5 % del alto de la zona segura). Lo que no
// cabe se desplaza con el dedo (arrastre vertical), la rueda o el mando. Se guarda la geometria original y se recalcula si cambia la zona
// segura (giro/plegado). Paneles: derecha de options_* (x > -110), columna izquierda de la raiz, lista de pausedmenu y botones de popups.
struct TItem { itemDef_s *it; float y, h, ta, ts; ExpressionStatement vis; float cy; };   // cy: rectClient.y (relativo al menu: el motor rehace rect con el)
struct TMenu { menuDef_t *m; std::vector<TItem> v; float top, bottom, limit, scroll, maxScroll, k, kMax; bool popup; };
static std::vector<TMenu> s_tm; static float s_tk = -1;
static float TouchK(float kMax)
{
    const float safeH = s_H - s_safe[1] - s_safe[3]; float dpi = g_h10Dpi; if (const char *e = getenv("H10_DPI")) dpi = (float)atof(e);
    const float rowPx = dpi > 0 ? 0.30f * dpi : 0.075f * safeH;
    float k = rowPx * 480.0f / (safeH > 1 ? safeH : 1) / 17.0f; return k < 1 ? 1 : k > kMax ? kMax : k;
}
static void TouchApply(TMenu &t)
{
    float maxB = t.top; for (TItem &x : t.v) maxB = fmaxf(maxB, x.y + x.h);
    const float span = (maxB - t.top) * t.k; t.maxScroll = t.popup ? 0 : fmaxf(0, span - (t.limit - t.top)); t.scroll = fminf(fmaxf(t.scroll, 0), t.maxScroll);
    // popup: los botones crecen hacia abajo hasta el borde del marco y, si no caben, suben sobre el hueco que queda bajo el texto
    const float lift = t.popup ? fmaxf(0, t.top + span - t.limit) : 0;
    for (TItem &x : t.v) {
        windowDef_t &w = x.it->window; const float ny = t.top + (x.y - t.top) * t.k - t.scroll - lift, nh = x.h < 40 ? x.h * t.k : x.h;
        w.rect.y = ny; w.rectClient.y = x.cy + (ny - x.y); w.rect.h = w.rectClient.h = nh;
        if (textDef_s *td = Item_GetTextDef(x.it)) {
            // texto algo mayor (sin salirse del ancho de la fila) y con la misma posicion relativa al centro de la fila: la linea base cae en
            // rect.y + P(modo, textaligny, alto, altoTexto) (Item_GetTextPlacementY); se despeja textaligny para la fila nueva
            const float f = fmaxf(1, fminf(0.8f * t.k, 1.15f)); Font_s *fn = Item_GetFont(0, x.it);
            const float th0 = fn ? (float)UI_TextHeight(fn, x.ts) : 0.85f * x.h, th1 = fn ? (float)UI_TextHeight(fn, x.ts * f) : th0 * f;
            auto P = [&](float ta, float h, float th) { switch (td->textAlignMode & 0xC) { case 0: return ta; case 4: return ta + th; case 8: return ta + (h + th) * 0.5f; default: return ta + h; } };
            const float b0 = P(x.ta, x.h, th0), b1 = nh * 0.5f + (b0 - x.h * 0.5f) * f;
            td->textaligny = x.ta + (b1 - P(x.ta, nh, th1)); td->textscale = x.ts * f; }
        const bool in = ny >= t.top - lift - 0.5f && ny + nh <= t.limit + 0.5f;
        x.it->visibleExp = in ? x.vis : s_never;
    }
}
static void TouchCollect(menuDef_t *m, float xmin, float xmax, float limit, bool popup)
{
    if (!m) return; TMenu t{}; t.m = m; t.limit = limit; t.popup = popup; t.top = 1e9f; t.kMax = popup ? 1.7f : 3.2f;
    float firstBtn = 1e9f; if (popup) for (int i = 0; i < m->itemCount; ++i) { itemDef_s *it = m->items[i].get(); if (HasAction(it) && it->window.rect.h > 2 && it->window.rect.h < 40) firstBtn = fminf(firstBtn, it->window.rect.y); }
    for (int i = 0; i < m->itemCount; ++i) {
        itemDef_s *it = m->items[i].get(); const rectDef_s &rc = it->window.rect;
        const rectData_s *rd = it->rectExpData.get();
        // expresion de x sola (pausedmenu: 50 + zombietron*66): vale; de y/h la rehace el motor; de ancho: pestanas, no filas
        if ((rd && (rd->rectYExp.filename || rd->rectHExp.filename || rd->rectWExp.filename)) || rc.h < 4 || rc.h >= 40 || rc.x < xmin || rc.x > xmax || (popup && rc.y < firstBtn - 0.5f)) continue;
        float ta = 0, ts = 0; if (textDef_s *td = Item_GetTextDef(it)) { ta = td->textaligny; ts = td->textscale; }
        t.v.push_back({ it, rc.y, rc.h, ta, ts, it->visibleExp, it->window.rectClient.y }); t.top = fminf(t.top, rc.y);
    }
    if (t.v.empty()) return;
    if (popup) { float maxB = t.top; for (TItem &x : t.v) maxB = fmaxf(maxB, x.y + x.h); t.limit = fmaxf(limit, maxB); }
    s_tm.push_back(t);
}
static void TouchLayoutInit()
{
    for (int k = 0; k < s_dc->menuCount; ++k) {
        menuDef_t *m = s_dc->Menus[k]; const char *n = m->window.name ? m->window.name.get() : "";
        if (!strcmp(n, "options_new_pc") || !strcmp(n, "options_graphics_defaults")) TouchCollect(m, -300, -110, 185, false);
        else if (!strncmp(n, "options_", 8) && strcmp(n, "options_controls_new_pc")) { TouchCollect(m, -300, -110, 185, false); TouchCollect(m, -110, 1e9f, 185, false); }
        else if (!strcmp(n, "pausedmenu")) TouchCollect(m, 40, 400, 470, false);
        else if (IsPopup(m)) {   // botones del popup: crecen hacia abajo dentro del marco (borde inferior del menu)
            const rectDef_s &mr = m->window.rect; TouchCollect(m, -1e9f, 1e9f, mr.h > 0 ? mr.y + mr.h - 6 : 1e9f, true); }
    }
    fprintf(stderr, "[H10] disposicion tactil: %zu menus\n", s_tm.size());
    if (getenv("H10_TRACE")) for (TMenu &t : s_tm) fprintf(stderr, "[H10]   tactil %s: %zu items, arriba %.0f, tope %.0f%s\n", t.m->window.name.get(), t.v.size(), t.top, t.limit, t.popup ? " (popup)" : "");
}
static const float kPopupLift = 24;   // unidades que los botones de un popup pueden subir sobre el hueco de debajo del texto
static void TouchLayoutUpdate()   // k segun la zona segura actual (cada frame: barato si no cambia)
{
    const float k = TouchK(3.2f); if (k == s_tk) return; s_tk = k;
    for (TMenu &t : s_tm) { t.k = fminf(k, t.kMax); if (t.popup) { /* el grupo de botones no puede salirse del marco */ float maxB = t.top; for (TItem &x : t.v) maxB = fmaxf(maxB, x.y + x.h);
            if (maxB > t.top) t.k = fmaxf(1, fminf(t.k, (t.limit - t.top + kPopupLift) / (maxB - t.top))); } TouchApply(t); }
    float px = 0.30f * (g_h10Dpi > 0 ? g_h10Dpi : 0); (void)px;
    fprintf(stderr, "[H10] tactil: filas x%.2f (%.0f px de alto por fila en %0.f px de zona segura)\n", k, 17.0f * k * (s_H - s_safe[1] - s_safe[3]) / 480.0f, s_H - s_safe[1] - s_safe[3]);
}
// desplazamiento: el menu tactil abierto mas arriba que tenga contenido fuera de la ventana
// (con x en pixeles, el panel bajo el dedo; sin x, el primero que lo necesite: rueda y mando)
static bool UnderX(TMenu &t, float px)
{
    float a = 1e9f, b = -1e9f, r[4]; for (TItem &x : t.v) { ScreenRect(x.it, r); a = fminf(a, r[0]); b = fmaxf(b, r[0] + r[2]); }
    return px >= a - 0.02f * s_W && px <= b + 0.02f * s_W;
}
static TMenu *ScrollMenu(float px = NAN)
{
    for (int k = s_dc->openMenuCount - 1; k >= 0; --k) for (TMenu &t : s_tm)
        if (t.m == s_dc->menuStack[k].menu && t.maxScroll > 0 && (std::isnan(px) || UnderX(t, px))) return &t;
    return nullptr;
}
static bool PadScroll(itemDef_s *cur, itemDef_s *best, int b)
{
    if (!cur) return false;
    for (TMenu &t : s_tm) {
        if (t.maxScroll <= 0) continue; bool hasCur = false, hasBest = false;
        for (TItem &x : t.v) { if (x.it == cur) hasCur = true; if (x.it == best) hasBest = true; }
        if (!hasCur || hasBest) continue;
        if (getenv("H10_TRACE")) fprintf(stderr, "[H10] mando: desplaza %s (%.0f de %.0f)\n", t.m->window.name.get(), t.scroll, t.maxScroll);
        if (b == 1 ? t.scroll >= t.maxScroll - 0.5f : t.scroll <= 0.5f) return false;
        t.scroll += (b == 1 ? 1 : -1) * 17.0f * t.k; TouchApply(t); return true;   // una fila
    }
    return false;
}
static bool TouchScroll(float dy, float x) { TMenu *t = ScrollMenu(x); if (!t) return false; const float o = t->scroll; t->scroll += dy; TouchApply(*t); return t->scroll != o; }
static void PatchGraphicsMenu(bool touch)
{
    s_og = Menus_FindByName(s_dc, "options_graphics"); if (!s_og || s_og->itemCount < 181) { fprintf(stderr, "[H10] options_graphics inesperado (%d items): sin cambios\n", s_og ? s_og->itemCount : 0); return; }
    s_never = It(138)->visibleExp; s_neverOk = true;   // 'ui_multiplayer' (falso en zombies) antes de habilitar el FOV
    s_hl = It(99)->visibleExp;       // resaltado de la fila 28, siempre activa
    multiDef_s *yesno = It(72)->typeData.textDef->textTypeData.focusItemDef->focusTypeData.multi.get();
    multiDef_s *texq = It(112)->typeData.textDef->textTypeData.focusItemDef->focusTypeData.multi.get();
    const char *no = yesno->dvarList[0].get(), *yes = yesno->dvarList[1].get(), *lo = texq->dvarList[1].get(), *nor = texq->dvarList[2].get(), *hi = texq->dvarList[3].get(), *ex = texq->dvarList[4].get();
    // filas nuevas en el sitio de las de modo de video (en partida el original las deja en gris: aqui se usan para lo que el port cambia)
    EnableRow(22, 63); Label(63, "QUALITY PRESET"); Multi(63, "h10_quality", { { lo, 0 }, { nor, 1 }, { hi, 2 }, { "CUSTOM", 3 } });
    EnableRow(23, 68); Label(68, "RENDER RESOLUTION"); Multi(68, "h10_renderScale", { { "50%", 50 }, { "67%", 67 }, { "75%", 75 }, { "85%", 85 }, { "100%", 100 } });
    EnableRow(24, 73); Hide(78, 82); Label(73, "BLOOM / POST FX"); Multi(73, "h10_postfx", { { no, 0 }, { yes, 1 } });
    EnableRow(25, 83); Label(83, "FRAME RATE LIMIT"); Multi(83, "com_maxfps", { { "30", 30 }, { "60", 60 }, { "90", 90 }, { "120", 120 }, { "UNLIMITED", 0 } });
    EnableRow(26, 88); Label(88, "TEXTURE SIZE LIMIT"); Multi(88, "h10_maxTex", { { lo, 256 }, { nor, 512 }, { hi, 1024 }, { ex, 2048 } });
    // 27 VSYNC: original, en gris (el swapchain va siempre en FIFO); 28/29 filtro y anisotropico: originales (en caliente)
    EnableRow(30, 108);   // calidad de texturas original (ui_r_picmip -> Aplicar -> r_picmip)
    for (int b : { 143, 146, 149, 152, 155, 161 }) Hide(b, b + 2);
    Hide(172, 176); Hide(178, 178);   // copias en gris de los valores de esas filas   // sombreados de "no se puede en partida" de las filas habilitadas
    Hide(113, 117); Hide(123, 127); Hide(128, 132); Hide(133, 137);   // shader warming, impactos, cadaveres, brillo: sin soporte en el port
    EnableRow(36, 138);   // FOV (cg_fov_default 65-80, en caliente)
    // 32 SOMBRAS: la controla ui_showShadowOptions (0 = gris); desde la OLA 9 activa (sm_enable, h9_shadow.cpp)
    const int rows[] = { 22, 23, 24, 25, 26, 27, 28, 29, 30, 32, 36 };
    for (int i = 0; i < 11; ++i) {
        const int n = rows[i]; float y = It(RowBase(n))->window.rect.y, h = 17;
        if (n == 32) y = 76; else if (n == 36) y = 95;
        MoveRow(RowBase(n), y, h);
        auto setY = [&](int b, float dy) { windowDef_t &w = It(b)->window; w.rect.y = w.rectClient.y = y + dy; w.rect.h = w.rectClient.h = h; };
        if (n == 27) { for (int b = 158; b <= 160; ++b) setY(b, 0); setY(177, 1); }   // en gris: barra, sombreado y valor gris (x 5124 + textalignx)
        if (n == 32) { for (int b = 164; b <= 169; ++b) setY(b, 0); setY(179, 0); setY(180, 0); }
    }
    fprintf(stderr, "[H10] options_graphics: filas del port (disposicion original%s)\n", touch ? "; el tactil las agranda despues" : "");
}

// ---------------------------------------------------------------- estado por frame
static int s_prevQ = -1; static float s_prevFov = -1; static int s_prevPfx = -1;
static void Poll()
{
    if (s_defPending) { s_defPending = false; ApplyDefaults(); }
    int q = DI("h10_quality");
    // PERSONALIZADO no se elige a mano: pasar por el con el multi (ALTO -> sig. o BAJO -> ant.) salta al perfil del otro extremo
    if (q == 3 && s_prevQ >= 0 && s_prevQ < 3 && MatchProf(s_prevQ)) { q = s_prevQ == 2 ? 0 : 2; DSetI("h10_quality", q); }
    if (q != s_prevQ && s_prevQ != -1 && q >= 0 && q < 3) ApplyProf(q);
    s_prevQ = q;
    if (q >= 0 && q < 3 && !MatchProf(q)) { DSetI("h10_quality", 3); s_prevQ = 3; fprintf(stderr, "[H10] valor suelto cambiado: perfil PERSONALIZADO\n"); }
    const int pfx = DI("h10_postfx"); if (pfx != s_prevPfx) { g_h10PostFxOff = !pfx; s_prevPfx = pfx; }
    const float fov = (float)atof(Dvar_GetVariantString("cg_fov_default"));
    if (fov != s_prevFov && fov >= 1) { s_prevFov = fov; DSet("cg_fov", Dvar_GetVariantString("cg_fov_default")); g_h10FovX = H7C_FovX(s_W, s_H); }
    if (s_open) {
        const bool pend = DI("h10_renderScale") != s_bootScale || DI("h10_maxTex") != s_bootMaxTex || PendingPicmip() != s_bootPicmip;
        if (UILocalVar *v = (UILocalVar *)UILocalVar_FindOrCreate(&s_dc->localVars, "ui_showApply")) UILocalVar_SetBool(v, pend);
        if (const dvar_s *d = Dvar_FindVar("ui_allow_graphic_change")) if (d->current.enabled != pend) Dvar_SetBool((dvar_s *)d, pend);   // boton Aplicar solo con cambios que piden reinicio
    }
}
static const dvar_s *RegIA(const char *n, int v, int lo, int hi) { const dvar_s *d = Dvar_FindVar(n); if (!d) d = _Dvar_RegisterInt(n, v, lo, hi, 1, "OLA 10"); return d; }
static void SettingsInit()
{
    const Prof &p = kProf[DefQuality()];
    RegIA("h10_quality", DefQuality(), 0, 3); RegIA("h10_renderScale", p.scale, 25, 100); RegIA("h10_maxTex", p.maxTex, 64, 4096); RegIA("h10_postfx", p.postfx, 0, 1);
    RegIA("h10_fog", 1, 0, 1); RegIA("h10_reflections", 1, 0, 1);   // ganchos para la ola 9 (niebla / reflejos), sin fila todavia
    RegIA("com_maxfps", 0, 0, 1000);
    if (!Dvar_FindVar("cg_fov_default")) _Dvar_RegisterFloat("cg_fov_default", 65, 65, 80, 1, "");
    if (!Dvar_FindVar("cg_fov")) _Dvar_RegisterFloat("cg_fov", 65, 1, 160, 0, "");
    for (const char *n : kCfgKeys) if (const dvar_s *d = Dvar_FindVar(n)) if (!(d->flags & 1)) Dvar_AddFlags(d, 1);   // archivados: Dvar_WriteVariables los guarda
    if (s_cfgFound) CfgApply(false);
    else { DSetI("h10_quality", DefQuality()); DSetI("h10_renderScale", s_bootScale); DSetI("h10_maxTex", s_bootMaxTex); DSetI("r_texFilterAnisoMin", p.aniso); DSet("r_texFilterMipMode", p.mip); DSetI("h10_postfx", p.postfx);
           DSetI("sm_enable", p.shadows > 0); if (p.shadows > 0) DSetI("sm_maxLights", p.shadows); }
    { const int f = DI("com_maxfps"); if (!s_cfgFound || (f != 0 && f != 30 && f != 60 && f != 90 && f != 120)) DSetI("com_maxfps", p.fps); }   // valores de la fila (el 85 del original -> el del perfil)
    { const dvar_s *d = Dvar_FindVar("ui_allow_graphic_change"); if (!d) d = _Dvar_RegisterBool("ui_allow_graphic_change", 1, 0, ""); Dvar_SetBool((dvar_s *)d, true); }
    { const dvar_s *d = Dvar_FindVar("ui_showShadowOptions"); if (!d) d = _Dvar_RegisterBool("ui_showShadowOptions", 0, 0, ""); Dvar_SetBool((dvar_s *)d, !(getenv("H10_SHADOWS") && !atoi(getenv("H10_SHADOWS")))); }   // OLA 9: hay sombras de focos
    if (!Cmd_FindCommand("setfromdvar")) Dvar_AddCommands();
    AddCmd("vid_restart", Cmd_H10VidRestart, &s_cmdVid, true); AddCmd("r_applyPicmip", Cmd_H10ApplyPicmip, &s_cmdPic, true);
    AddCmd("updategamerprofile", Cmd_H10Profile, &s_cmdProf, true); AddCmd("ui_animate", Cmd_Nop, &s_cmdAnim, false);
    AddCmd("fast_restart", Cmd_H10FastRestart, &s_cmdRestart, true); AddCmd("disconnect", Cmd_H10Disconnect, &s_cmdQuit, true);
    AddCmd("fade", Cmd_Nop, &s_cmdFade, false); AddCmd("silence", Cmd_Nop, &s_cmdSilence, false);
    if (!s_cfgFound) CfgSave();   // primera ejecucion: el perfil de la plataforma queda escrito
    fprintf(stderr, "[H10] ajustes: calidad %d, escala %d%%, maxtex %d, picmip %d, aniso %s, mip %s, postfx %d, fps %d, fov %s\n", DI("h10_quality"), DI("h10_renderScale"), DI("h10_maxTex"),
        s_bootPicmip, Dvar_GetVariantString("r_texFilterAnisoMin"), Dvar_GetVariantString("r_texFilterMipMode"), DI("h10_postfx"), DI("com_maxfps"), Dvar_GetVariantString("cg_fov_default"));
}

// ---------------------------------------------------------------- dibujo (lista 2D del HUD, encima de todo)
static bool TouchLayout() { const char *e = getenv("H10_TOUCH");
#ifdef __ANDROID__
    return !e || atoi(e);
#else
    return e && atoi(e);
#endif
}
void H10_Draw(int W, int H)
{
    if (!s_on) return;
    if (!s_ready) {
        s_ready = true; s_t0 = NowMs();
        if (!H10_Setup(W, H)) { s_on = false; return; }
        if (const char *d = getenv("H10_DUMP")) { std::string l = d; for (size_t p = 0; p < l.size();) { size_t q = l.find(',', p); if (q == std::string::npos) q = l.size(); H10_Dump(l.substr(p, q - p).c_str()); p = q + 1; } }
        if (const char *d = getenv("H10_RAW")) { std::string l = d; for (size_t p = 0; p < l.size();) { size_t q = l.find(',', p); if (q == std::string::npos) q = l.size(); H10_DumpRaw(l.substr(p, q - p).c_str()); p = q + 1; } }
        if (const char *d = getenv("H10_ITEMS")) { char mn[64]; int a = 0, b = 0; if (sscanf(d, "%63[^:]:%d:%d", mn, &a, &b) == 3) H10_DumpItems(mn, a, b); }
        SettingsInit();
        if (!getenv("H10_ORIG")) { PatchGraphicsMenu(TouchLayout()); if (s_neverOk) { PatchRootMenu(); if (!getenv("H10_KEEPCONTROLS")) PatchControlsTab(); if (TouchLayout()) TouchLayoutInit(); } }
        if (const char *d = getenv("H10_ITEMS2")) { char mn[64]; int a = 0, b = 0; if (sscanf(d, "%63[^:]:%d:%d", mn, &a, &b) == 3) H10_DumpItems(mn, a, b); }   // despues de cambiar las filas
        if (const char *o = getenv("H10_OPEN")) H10_Open(o);
        if (const char *p = getenv("H10_PILOT")) PilotParse(p);
    }
    PlaceUpdate();   // la app puede cambiar la zona segura al girar o plegar
    if (!s_tm.empty()) TouchLayoutUpdate();
    if (!s_pilot.empty()) PilotStep();
    if (Cmd_FindCommand("wait")) Cbuf_Execute(0, 0);
    Poll();
    if (!s_open) return;
    if (getenv("H10_TRACE")) { static menuDef_t *top; menuDef_t *m = s_dc->openMenuCount > 0 ? s_dc->menuStack[s_dc->openMenuCount - 1].menu : nullptr;
        if (m != top) { top = m; fprintf(stderr, "[H10] menu arriba: %s\n", m && m->window.name ? m->window.name.get() : "-"); } }
    static double last = NowMs(); const double now = NowMs();
    s_dc->realTime = (int)Sys_Milliseconds(); s_dc->frameTime = (int)(now - last); last = now;
    PlaceScope ps; Menu_PaintAll(0, s_dc);
}

// ---------------------------------------------------------------- variables para la app (la app no enlaza simbolos de H5G salvo debiles)
bool g_h10MenuOpen;
void (*g_h10TouchFn)(int, float, float) = H10_Touch;
void (*g_h10KeyFn)(int, bool) = H10_Key;
// boton MENU / START / Esc con el juego en marcha: abre el menu de pausa original (pausedmenu: continuar, opciones, reiniciar, salir);
// con menus abiertos los cierra todos y sigue la partida. H10_PAUSE=<menu> cambia el menu de pausa.
static const char *PauseMenu() { const char *e = getenv("H10_PAUSE"); return e && *e ? e : "pausedmenu"; }
static void H10_Toggle() { if (s_open) H10_Close(); else H10_Open(PauseMenu()); }
void (*g_h10ToggleFn)() = H10_Toggle;
void (*g_h10PadFn)(int) = H10_Pad;

extern void (*g_h5pPostFrame)();
static void (*s_prevPost)();
static void H10_PostFrame()
{
    g_h10MenuOpen = s_open;
    if (!s_shot.empty()) { H4_Shot(s_shot.c_str()); s_shot.clear(); }
    if (s_prevPost) s_prevPost();
    // limite de fps (com_maxfps; 0 = sin limite) por plazo absoluto: duerme hasta el siguiente plazo y apura el ultimo ms con espera activa,
    // asi 30/60 salen exactos aunque usleep se pase. Si el frame llega tarde no se acumula deuda (el plazo se reinicia). Con vsync (FIFO)
    // el present ya espera a la pantalla: el limite solo actua por debajo del refresco. H10_NOLIMIT=1 lo quita.
    static const bool noLimit = getenv("H10_NOLIMIT") && atoi(getenv("H10_NOLIMIT"));
    static double next = 0; int mf = s_ready && !noLimit ? DI("com_maxfps") : 0;
    // en pausa (menu abierto, mundo congelado) se baja a H10_PAUSEFPS (30 en Android: bateria; 0 = igual que en juego)
#ifdef __ANDROID__
    static const int pauseFps = getenv("H10_PAUSEFPS") ? atoi(getenv("H10_PAUSEFPS")) : 30;
#else
    static const int pauseFps = getenv("H10_PAUSEFPS") ? atoi(getenv("H10_PAUSEFPS")) : 0;
#endif
    if (s_open && !noLimit && pauseFps > 0 && (mf == 0 || mf > pauseFps)) mf = pauseFps;
    if (mf > 0) {
        const double per = 1000.0 / mf; double now = NowMs();
        if (next == 0 || now - next > per) next = now;   // primer frame o atraso de mas de un periodo
        next += per;
        if (next - now > 1.5) usleep((useconds_t)((next - now - 1.0) * 1000));
        while (NowMs() < next) {}
    } else next = 0;
    // medida (H10_FPSLOG=1): fps reales cada segundo, con el limite activo y el frame mas largo
    static const bool fpsLog = getenv("H10_FPSLOG") && atoi(getenv("H10_FPSLOG"));
    if (fpsLog) {
        static double t0 = 0, prev = 0, worst = 0; static int n = 0; const double now = NowMs();
        if (prev) { worst = now - prev > worst ? now - prev : worst; ++n; } prev = now; if (!t0) t0 = now;
        if (now - t0 >= 1000 && n) { fprintf(stderr, "[H10] fps %.1f (limite %d, %d frames, frame max %.1f ms)\n", n * 1000.0 / (now - t0), mf, n, worst); t0 = now; n = 0; worst = 0; }
    }
}
#ifdef H0_MACWIN
extern "C" void H10_MacInputInstall(void (*ev)(int, float, float, int));
// raton/teclado de la ventana del Mac: kind 0 mover 1 boton abajo 2 boton arriba 3 rueda (key = +-1) 4 tecla abajo 5 tecla arriba
static void MacEv(int kind, float fx, float fy, int key)
{
    if (kind == 4 && key == K_ESCAPE && !s_open) { H10_Open(PauseMenu()); return; }
    if ((kind == 4 || kind == 5) && key == K_TAB && !s_open) { extern void H13_ShowScores(bool); H13_ShowScores(kind == 4); return; }   // OLA 15E: marcador mientras se mantiene Tab
    if (!s_open) return;
    static const bool pad = getenv("H10_PAD") && atoi(getenv("H10_PAD"));   // flechas/Enter/Retroceso como un mando (prueba de la navegacion de mando)
    if (pad && kind == 4) { const int b = key == K_UPARROW ? 0 : key == K_DOWNARROW ? 1 : key == K_LEFTARROW ? 2 : key == K_RIGHTARROW ? 3 : key == K_ENTER ? 4 : key == 0x7F ? 5 : -1; if (b >= 0) { H10_Pad(b); return; } }
    if (kind <= 2) H10_Pointer(fx * s_W, fy * s_H);
    if (kind == 1 || kind == 2) H10_Key(K_MOUSE1, kind == 1);
    if (kind == 3) { if (TouchScroll(key > 0 ? -30.0f : 30.0f)) return; int k = key > 0 ? K_MWHEELUP : K_MWHEELDOWN; H10_Key(k, true); H10_Key(k, false); }
    if (kind == 4 || kind == 5) H10_Key(key, kind == 4);
}
#endif
void H10_Install()
{
    const char *e = getenv("H10"); if (e && !atoi(e)) { s_on = false; fprintf(stderr, "[H10] ajustes: desactivado (H10=0)\n"); return; }
    extern void (*g_h10Draw2D)(int, int); g_h10Draw2D = H10_Draw;
    if (g_h5pPostFrame != H10_PostFrame) { s_prevPost = g_h5pPostFrame; g_h5pPostFrame = H10_PostFrame; }
#ifdef H0_MACWIN
    if (getenv("P4_WINDOW")) H10_MacInputInstall(MacEv);
#endif
}
