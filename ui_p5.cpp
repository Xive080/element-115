// P5: dibuja un MENU real del juego (zona) con la UI del motor: Menu_PaintAll -> UI_DrawHandlePic/UI_DrawText -> R_AddCmd*.
// Se llama desde main2.cpp con el renderer 2D ya listo y las zonas cargadas.
#include <database/db_registry.h>
#include <database/db_assetnames.h>
#include <universal/dvar.h>
#include <universal/ptr32.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_font.h>
#include <client/screen_placement.h>
#include <client_mp/cl_main_mp.h>
#include <qcommon/common.h>
#include <live/live_win.h>
#include <ui/ui_shared.h>
#include <cgame/cg_hudelem.h>
#include <ui/ui_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern uiInfo_s uiInfoArray[1];
void ScrPlace_SetupFloatViewport(ScreenPlacement *, float, float, float, float);
void UI_AssetCache();
#include <universal/com_expressions.h>
void __cdecl CL_GetScreenDimensions(int *width, int *height, float *aspect);
unsigned int Sys_Milliseconds();

static void EnumMenu(XAssetHeader h, void *u)
{
    menuDef_t *m = h.menu; auto *v = (std::vector<menuDef_t *> *)u; v->push_back(m);
}
static void EnumList(XAssetHeader h, void *u)
{
    MenuList *l = h.menuList; fprintf(stderr, "[P5] menulist %s: %d menus\n", l->name.get(), l->menuCount);
    if (u) ((std::vector<MenuList *> *)u)->push_back(l);
}

void P5_ListMenus()
{
    std::vector<menuDef_t *> v;
    DB_EnumXAssets_FastFile(ASSET_TYPE_MENULIST, EnumList, 0, false);
    DB_EnumXAssets_FastFile(ASSET_TYPE_MENU, EnumMenu, &v, false);
    printf("[P5] %d menus\n", (int)v.size());
    for (auto *m : v) {
        printf("[P5] menu %-40s items=%3d rect=(%g,%g,%g,%g) fullscreen=%d visibleExp=%d bg=%s\n", m->window.name.get(), m->itemCount,
               m->window.rect.x, m->window.rect.y, m->window.rect.w, m->window.rect.h, m->fullScreen, m->visibleExp.numRpn,
               m->window.background ? (const char *)m->window.background.get()->info.name.get() : "-");
    }
}

float g_p5Safe[4] = { 0, 0, 0, 0 };   // izquierda, arriba, derecha, abajo en pixeles (la app lo rellena antes de P5_SetupUi)

// Prepara el contexto de UI minimo y abre el menu. Devuelve el UiContext.
UiContext *P5_SetupUi(const char *menuName, int W, int H)
{
    cls.vidConfig.displayWidth = cls.vidConfig.sceneWidth = W; cls.vidConfig.displayHeight = cls.vidConfig.sceneHeight = H;
    cls.vidConfig.aspectRatioWindow = (float)W / H; cls.vidConfig.aspectRatioScenePixel = 1.0f;
    ScrPlace_SetupFloatViewport(&scrPlaceView[0], 0, 0, (float)W, (float)H);
    memcpy(&scrPlaceFull, &scrPlaceView[0], sizeof(ScreenPlacement)); memcpy(&scrPlaceFullUnsafe, &scrPlaceView[0], sizeof(ScreenPlacement));
    if (g_p5Safe[0] + g_p5Safe[1] + g_p5Safe[2] + g_p5Safe[3] > 0)   // safe areas (recorte/barras): scrPlaceFull = zona segura, FullUnsafe = pantalla entera
    {   // OLA 10: el inset como viewportX/Y no desplaza (CalcSafeAreaOffsets lo resta, realViewportBase queda a 0); se mueven los bordes
        // viewable/tweakable (anclas LEFT/RIGHT/TOP/BOTTOM) como el safeArea del original, con viewport y escala de pantalla entera
        ScreenPlacement *p = &scrPlaceFull; const float mn[2] = { g_p5Safe[0], g_p5Safe[1] }, mx[2] = { W - g_p5Safe[2], H - g_p5Safe[3] };
        const float hs = p->scaleVirtualToFull[0] / p->scaleVirtualToReal[0];
        for (int i = 0; i < 2; ++i) { const float k = i == 0 ? hs * 640.0f / W : 480.0f / H;
            p->realViewableMin[i] = p->realTweakableMin[i] = mn[i]; p->realViewableMax[i] = p->realTweakableMax[i] = mx[i];
            p->virtualViewableMin[i] = p->virtualTweakableMin[i] = mn[i] * k; p->virtualViewableMax[i] = p->virtualTweakableMax[i] = mx[i] * k; }
    }
    // dvars de common.cpp/cl_main_mp.cpp/live_win.cpp (no compilados) que la UI consulta por puntero; valores de zombies en menu
    zombiemode = _Dvar_RegisterBool("zombiemode", 1, 0, ""); zombietron = _Dvar_RegisterBool("zombietron", 0, 0, "");
    com_sv_running = _Dvar_RegisterBool("sv_running", 0, 0, ""); com_wideScreen = _Dvar_RegisterBool("wideScreen", 1, 0, "");
    com_maxclients = _Dvar_RegisterInt("com_maxclients", 4, 1, 32, 0, ""); cl_voice = _Dvar_RegisterBool("cl_voice", 0, 0, "");
    cl_bypassMouseInput = _Dvar_RegisterBool("cl_bypassMouseInput", 0, 0, "");
    xblive_wagermatch = _Dvar_RegisterBool("xblive_wagermatch", 0, 0, ""); xblive_basictraining = _Dvar_RegisterBool("xblive_basictraining", 0, 0, "");
    { extern const dvar_s *loc_forceEnglish, *loc_translate; loc_forceEnglish = _Dvar_RegisterBool("loc_forceEnglish", 0, 0, ""); loc_translate = _Dvar_RegisterBool("loc_translate", 1, 0, ""); }
    Expression_Init();
    UI_RegisterDvars();
    UI_AssetCache();
    uiInfo_s *ui = &uiInfoArray[0]; UiContext *dc = &ui->uiDC;
    dc->contextIndex = 0; ui->allowScriptMenuResponse = 1;
    Menu_Setup(dc);
    CL_GetScreenDimensions(&dc->screenWidth, &dc->screenHeight, &dc->screenAspect);
    dc->bias = (480 * dc->screenWidth <= 640 * dc->screenHeight) ? 0.0f : ((float)dc->screenWidth - (float)dc->screenHeight * 1.3333334f) * 0.5f;
    menuDef_t *asset = DB_FindXAssetHeader(ASSET_TYPE_MENU, (char *)menuName, 1, -1).menu;
    std::vector<MenuList *> lists; DB_EnumXAssets_FastFile(ASSET_TYPE_MENULIST, EnumList, &lists, false);
    for (MenuList *l : lists) {          // la lista (ui_mp/code.txt, ui/ingame.txt, hud...) que contiene el menu pedido
        bool has = false; for (int i = 0; i < l->menuCount; ++i) if (l->menus[i].get() == asset) has = true;
        if (!has) continue;
        fprintf(stderr, "[P5] lista elegida: %s (%d menus)\n", l->name.get(), l->menuCount);
        UI_AddMenuList(0, dc, l, 1); break;
    }
    fprintf(stderr, "[P5] menus en el contexto: %d\n", dc->menuCount);
    Menus_CloseAll(0, dc);
    menuDef_t *m = Menus_FindByName(dc, menuName);
    if (!m) {
        m = DB_FindXAssetHeader(ASSET_TYPE_MENU, (char *)menuName, 1, -1).menu;
        fprintf(stderr, "[P5] menu '%s' no esta en el contexto; asset=%p\n", menuName, (void *)m);
        if (!m) return 0;
        dc->Menus[dc->menuCount++] = m;
    }
    dc->realTime = (int)Sys_Milliseconds(); dc->frameTime = 16;
    Menus_OpenByName(0, dc, menuName);
    fprintf(stderr, "[P5] abierto '%s'; openMenuCount=%d\n", menuName, dc->openMenuCount);
    return dc;
}

void P5_PaintMenu(UiContext *dc)
{
    Menu_PaintAll(0, dc);
}

// ---- runtime minimo que la UI espera del resto del motor (cada funcion con su origen)
// cgame/cg_hudelem.cpp (no se compila entero): pila de ScreenPlacement, copia literal sin asserts
ScopedScrPlaceViewStack::ScopedScrPlaceViewStack(ScreenPlacementStack *stack, const ScreenPlacement *newPlacement)
{
    mStack = stack;
    if (mStack) {
        int size = stack->size;
        memcpy(&stack->stack[size], &scrPlaceView[stack->contextIndex], sizeof(stack->stack[size]));
        stack->size = size + 1;
        memcpy(&scrPlaceView[stack->contextIndex], newPlacement, sizeof(ScreenPlacement));
    }
}
ScopedScrPlaceViewStack::~ScopedScrPlaceViewStack()
{
    if (mStack) memcpy(&scrPlaceView[mStack->contextIndex], &mStack->stack[--mStack->size], sizeof(ScreenPlacement));
}
const rectDef_s *__cdecl Window_GetRect(const windowDef_t *w) { return &w->rect; }   // cg_main_mp.cpp (no se compila entero)

// Volcado de lo que define el menu en la zona (para comparar con lo pintado)
void P5_DumpMenu(const char *name)
{
    menuDef_t *m = DB_FindXAssetHeader(ASSET_TYPE_MENU, (char *)name, 1, -1).menu; if (!m) return;
    printf("[P5] MENU %s rect=(%g,%g,%g,%g) align=(%d,%d) fore=(%g,%g,%g,%g) back=(%g,%g,%g,%g) style=%d bg=%s fullscreen=%d items=%d\n", name,
           m->window.rect.x, m->window.rect.y, m->window.rect.w, m->window.rect.h, m->window.rect.horzAlign, m->window.rect.vertAlign,
           m->window.foreColor[0], m->window.foreColor[1], m->window.foreColor[2], m->window.foreColor[3],
           m->window.backColor[0], m->window.backColor[1], m->window.backColor[2], m->window.backColor[3], m->window.style,
           m->window.background ? m->window.background.get()->info.name.get() : "-", m->fullScreen, m->itemCount);
    for (int i = 0; i < m->itemCount; ++i) {
        itemDef_s *it = m->items[i].get(); windowDef_t &w = it->window;
        const char *txt = ""; float tsc = 0, tax = 0, tay = 0; int fe = 0, al = 0, ts = 0;
        if (it->type == 0 || it->type == 1 || it->type == 4) { /* ITEM_TYPE_TEXT/BUTTON... */ }
        printf("[P5]   item %2d type=%d name=%s rect=(%g,%g,%g,%g) align=(%d,%d) style=%d fore=(%g,%g,%g,%g) back=(%g,%g,%g,%g) bg=%s visExp=%d dvar=%s\n", i, it->type,
               w.name ? w.name.get() : "-", w.rect.x, w.rect.y, w.rect.w, w.rect.h, w.rect.horzAlign, w.rect.vertAlign, w.style,
               w.foreColor[0], w.foreColor[1], w.foreColor[2], w.foreColor[3], w.backColor[0], w.backColor[1], w.backColor[2], w.backColor[3],
               w.background ? w.background.get()->info.name.get() : "-", it->visibleExp.numRpn, it->dvar ? it->dvar.get() : "-");
        if (it->type == 0 || it->type == 1 || it->type == 6 || it->type == 9) { textDef_s *t = it->typeData.textDef.get(); if (t)
            printf("[P5]        text='%s' fontEnum=%d textscale=%g align=%d alignx=%g aligny=%g style=%d textRect=(%g,%g,%g,%g)\n", t->text ? t->text.get() : "-", t->fontEnum, t->textscale, t->alignment, t->textalignx, t->textaligny, t->textStyle, t->textRect[0].x, t->textRect[0].y, t->textRect[0].w, t->textRect[0].h); }
    }
}

// ---- P6: entrada/ciclo para la app
void P5_Touch(UiContext *dc, float x, float y) { dc->cursor.x = x; dc->cursor.y = y; Display_MouseMove(0, dc); }
void P5_Tick(UiContext *dc, int ms, int dt) { dc->realTime = ms; dc->frameTime = dt; }
const char *P5_NextMenu(UiContext *dc, const char *const *names, int n)
{
    static int idx = 0; idx = (idx + 1) % n;
    Menus_CloseAll(0, dc); Menus_OpenByName(0, dc, names[idx]);
    return names[idx];
}
