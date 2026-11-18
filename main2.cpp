// P3: R_Init2D propia sobre el device TRACE: carga .ff reales, inicializa el renderer 2D y dibuja un marco (rect rojo + "Hola").
#include <database/db_registry.h>
#include <database/db_file_load.h>
#include <database/db_assetnames.h>
#include <universal/ptr32.h>
#include <universal/dvar.h>
#include <clientscript/cscr_stringlist.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_rendertarget.h>
#include <gfx_d3d/rb_state.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_ui3d.h>
#include <gfx_d3d/rb_backend.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <libgen.h>
#include <unistd.h>
#include "trace.h"
#include "world_h4.h"
struct UiContext; void P5_ListMenus(); void P5_DumpMenu(const char *); UiContext *P5_SetupUi(const char *, int, int); void P5_PaintMenu(UiContext *);
#if defined(D3D9_GLES)
#include "../gles/d3d9_gles.h"
#define M2_G9 1
#elif defined(D3D9_VK)
#include "../vk/g9_api.h"
#define M2_G9 1
#ifdef H0_MACWIN
// ventana Cocoa (vk/tests/t_window_win.mm, compilada APARTE sin compat/Windows.h: Cocoa choca con su BOOL/IUnknown); P4_WINDOW=1
extern "C" { void *WinOpen(int w, int h, int withWindow); void WinPump(void); void WinClose(void); }
#endif
#endif

HANDLE h0_open(const char *path);
extern XAssetList g_varXAssetList;
extern unsigned int g_zoneIndex; extern int g_zoneCount; extern XZoneName g_zoneNames[33];
extern bool isDvarSystemActive;
const dvar_s *_Dvar_RegisterBool(const char *n, bool v, unsigned short, const char *);
void DB_Init();
bool Assert_MyHandler(const char *, int, int, const char *, ...);
void R_RegisterDvars();
void R_InitGlobalStructs(); void R_InitGamma(); void R_InitScene();
static const int kZonePrio[] = { 0x800000, 0x20000, 0x2000000, 0x10000000, 0x1000000, 0x4000, 0x40000, 0x80000 };

// dvars que el motor real define fuera de lo compilado
extern const dvar_s *loc_language, *loc_warnings, *loc_warningsAsErrors;
const dvar_s *useFastFile; const dvar_s *com_developer; const dvar_s *bo1_zombies;
const dvar_s *sys_smp_allowed;
#ifdef H5G
__attribute__((weak))
#endif
const dvar_s *sv_cheats;
 const dvar_s *dedicated; const dvar_s *cl_paused; const dvar_s *cg_draw2D;

const char *H18_DefaultMap(); void H18_ResolveBoot(int argc, char **argv); void H18_PlayLoadVideo();   // OLA M0: h18_maps.cpp (mapa de arranque y su carpeta de datos)
static int g_loaded = 0;
extern "C" void H15_MemPhase(const char *phase);   // OLA 15A: memguard.cpp (H15_MEMPROFILE)
static void LoadFF(const char *path)
{
    HANDLE h = h0_open(path); if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "no abre %s\n", path); exit(1); }
    static unsigned char *cbuf = (unsigned char *)Ptr32_ArenaAlloc(0x80000, 4096);
    static XBlock blocks[7]; memset(blocks, 0, sizeof(blocks));
    int a = ++g_loaded; char *nm = strdup(path); std::string zn = basename(nm); zn.resize(zn.size() - 3);
    g_zoneIndex = a; I_strncpyz(g_zoneNames[a].name, zn.c_str(), 64); g_zoneNames[a].flags = kZonePrio[(a - 1) % 8]; ++g_zoneCount;
    if (getenv("H5_VM") || getenv("H5_GAME")) {
        // H5: banderas REALES de zona (DB_LoadXAssets/Com_LoadLevelFastFiles): comun 256 (prioridad 52) y su _patch 1024 (60),
        // nivel 0x4000 (55) y su _patch 0x40000 (57): los _patch pisan a la zona base. Con el reparto ciclico de kZonePrio
        // common_zombie (57) ganaba a common_zombie_patch (3) y salia el _zombiemode.gsc viejo (llama a _zombiemode_ffotd::init).
        const char *hm = getenv("H5_MAP") ? getenv("H5_MAP") : H18_DefaultMap();   // H18_ResolveBoot ya lo fijo
        if (zn == "common_zombie") g_zoneNames[a].flags = 256;
        else if (zn == "common_zombie_patch") g_zoneNames[a].flags = 1024;
        else if (zn == hm) g_zoneNames[a].flags = 0x4000;
        else if (zn == std::string(hm) + "_patch") g_zoneNames[a].flags = 0x40000;
    }
    fprintf(stderr, "== carga %s\n", path);
    extern bool g_h0InDbLoad; g_h0InDbLoad = true;
    DB_LoadXFile(path, h, strdup(zn.c_str()), blocks, 0, cbuf, 0, 0);
    g_h0InDbLoad = false;
    if (getenv("H15_MEMPROFILE")) {   // OLA 15A: bloques de la zona (temp, runtime, large_runtime, physical_runtime, virtual, large, physical) en KB
        fprintf(stderr, "[H15MEM] zona %s bloques KB:", zn.c_str()); for (int b = 0; b < 7; ++b) fprintf(stderr, " %u", blocks[b].size >> 10); fprintf(stderr, "\n"); }
    if (getenv("H5_VM") || getenv("H5_GAME")) {
        // H5: lo que hace DB_PostLoadXZone con los duplicados (g_copyInfo): enlazarlos con override, asi un _patch posterior
        // y de mas prioridad sustituye al asset de la zona base (sin esto los GSC parcheados se perdian)
        extern XAssetEntry *g_copyInfo[]; extern unsigned int g_copyInfoCount;
        // Solo RAWFILE (los GSC): el resto de tipos sigue como antes en el harness (el override de otros tipos esta sin validar).
        unsigned n = 0;
        for (unsigned i = 0; i < g_copyInfoCount; ++i) if (g_copyInfo[i]->asset.type == ASSET_TYPE_RAWFILE) { DB_LinkXAssetEntry(g_copyInfo[i], 1); ++n; }
        if (n) fprintf(stderr, "[H5] %s: %u rawfiles duplicados enlazados con override (de %u duplicados)\n", zn.c_str(), n, g_copyInfoCount);
        g_copyInfoCount = 0;
    }
    printf("%s: %d assets\n", zn.c_str(), g_varXAssetList.assetCount);
    CloseHandle(h);
}

// ---- nombres de objetos D3D para la traza: se resuelven recorriendo los assets (imagen->texture.basemap, shader->prog)
static std::map<const void *, std::string> g_names[4];
static void EnumImg(XAssetHeader h, void *) { GfxImage *i = h.image; if (i->texture.basemap) g_names[0][i->texture.basemap.get()] = i->name.get(); }
static void EnumTS(XAssetHeader h, void *)
{
    MaterialTechniqueSet *ts = h.techniqueSet;
    for (int i = 0; i < 130; ++i) {
        MaterialTechnique *t = ts->techniques[i].get(); if (!t || (intptr_t)t == -1) continue;
        for (int p = 0; p < t->passCount; ++p) {
            MaterialPass *ps = &t->passArray[p];
            MaterialVertexShader *vs = ps->vertexShader.get(); if (vs && vs->prog.vs) g_names[1][vs->prog.vs.get()] = vs->name.get();
            MaterialPixelShader *px = ps->pixelShader.get(); if (px && px->prog.ps) g_names[2][px->prog.ps.get()] = px->name.get();
        }
    }
}
static const char *NameOf(int kind, const void *p)
{
    auto it = g_names[kind].find(p);
    if (it == g_names[kind].end()) {
        g_names[0].clear(); g_names[1].clear(); g_names[2].clear();
        DB_EnumXAssets_FastFile(ASSET_TYPE_IMAGE, EnumImg, 0, false);
        DB_EnumXAssets_FastFile(ASSET_TYPE_TECHNIQUE_SET, EnumTS, 0, false);
        it = g_names[kind].find(p); if (it == g_names[kind].end()) return "?";
    }
    return it->second.c_str();
}

#ifdef BO1_APP
// P6: variante app Android: la ventana/tamano los pone android_main.cpp; el bucle de frames corre aqui con ganchos de la app.
bool BO1App_PreFrame(void *dc); bool BO1App_PreFrame3D(H4Input *in, float *dt); void BO1App_PostFrame(); const char *BO1App_Menu();
int bo1_engine_main(void *nativeWindow, int AW, int AH)
#else
int main(int argc, char **argv)
#endif
{
#ifndef BO1_APP
    H18_ResolveBoot(argc, argv);   // OLA M0: mapa y P3_GAMEDATA antes de leer datos o config
    int W0 = getenv("P4_W") ? atoi(getenv("P4_W")) : 1280, H0 = getenv("P4_H") ? atoi(getenv("P4_H")) : 720;
#else
    H18_ResolveBoot(0, nullptr);
    int W0 = AW, H0 = AH;
#endif
    H18_PlayLoadVideo();   // OLA M0: video de carga del mapa si existe (stub: sin reproductor todavia; nunca es un error)
#ifdef H5G
    { void H10_PreBackend(int *, int *); H10_PreBackend(&W0, &H0); }   // OLA 10: resolucion interna y tope de texturas del config (h10_settings.cpp)
#endif
    const char *gd = getenv("P3_GAMEDATA"); std::string G = std::string(gd ? gd : "../../gamedata") + "/zone/";
#ifndef BO1_APP
    void *nativeWindow = nullptr;
#endif
#ifdef M2_G9
    // Selector de backend: variable de EJECUCION P3_BACKEND=trace desactiva el backend enlazado (solo traza); sin ella se usa el enlazado.
    // Variable de COMPILACION: BACKEND=vk|trace (harness2/build.sh; mac: vk o trace, ndk: gles por defecto, vk o trace).
#ifdef D3D9_VK
    struct G9Guard { ~G9Guard() { if (g9::Active()) { TraceDevice_ReleaseAll(); g9::Shutdown(); } } } g9guard;   // Shutdown tambien al salir por return de error (guarda pipelines.keys / cache)
#endif
    {   // el backend antes que la arena de 32 bits: el driver necesita su espacio de direcciones
        const char *be = getenv("P3_BACKEND"); bool useGL = !(be && !strcmp(be, "trace"));
#ifdef D3D9_VK
        if (useGL) {
            g9::InitDesc d; d.w = W0; d.h = H0;
#ifndef BO1_APP
            // El motor sube casi todas las texturas de la escena en los primeros frames (creacion perezosa): ~230 MB de golpe superan el tope de RITMO de subida
            // de vk_budget (200 MB/s, pensado para el Fold) y abortan. En el harness del Mac se desactiva salvo que el usuario fije V9_UPLOAD_MB_S.
            setenv("V9_UPLOAD_MB_S", "0", 0);
#endif
#ifdef BO1_APP
            d.wsi = g9::WSI_ANDROID; d.window = nativeWindow;
#elif defined(H0_MACWIN)
            if (getenv("P4_WINDOW")) { d.window = WinOpen(W0, H0, 1); d.wsi = d.window ? g9::WSI_METAL_LAYER : g9::WSI_NONE; }
#endif
            if (!g9::InitEx(d)) { fprintf(stderr, "[P4] g9::InitEx fallo\n"); return 1; }
            fprintf(stderr, "[P4] backend Vulkan: %s\n", g9::Info());
            // El contenido inicial del backbuffer es indefinido en Vulkan (en GLES salia negro por defecto): se limpia una vez para que las capturas sean deterministas.
            g9::Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000, 1.0f, 0);
            atexit([] { if (g9::Active()) { TraceDevice_ReleaseAll(); g9::Shutdown(); } });   // exit(1) de LoadFF etc. (los return los cubre g9guard)
            { extern void H6_PrewarmPipelines(int, int); H6_PrewarmPipelines(W0, H0); }   // OLA 6b: "Loading shaders" (h6_loading.cpp)
        }
#else
        if (useGL && !g9::Init(W0, H0, nativeWindow)) { fprintf(stderr, "[P4] g9::Init fallo\n"); return 1; }
#endif
    }
#endif
    Ptr32_InitArena();
    SL_Init(SCRIPTINSTANCE_SERVER); SL_Init(SCRIPTINSTANCE_CLIENT);   // tabla de cadenas de script real (los loaders de zonas la usan)
    isDvarSystemActive = 1;
    useFastFile = _Dvar_RegisterBool("useFastFile", true, 0, "");
    com_developer = _Dvar_RegisterInt("developer", 0, 0, 2, 0, ""); loc_language = _Dvar_RegisterInt("loc_language", 0, 0, 20, 0, ""); loc_warnings = _Dvar_RegisterBool("loc_warnings", false, 0, "");
    loc_warningsAsErrors = _Dvar_RegisterBool("loc_warningsAsErrors", false, 0, ""); 
#ifdef H5G
    // H5G: Com_InitDvars ORIGINAL vuelve a registrarlos; mismo valor de reset que el original (assert de Dvar_Reregister) y luego el valor del harness.
    bo1_zombies = _Dvar_RegisterBool("bo1_zombies", false, 0x10u, ""); Dvar_SetBool((dvar_s *)bo1_zombies, true);
#else
    bo1_zombies = _Dvar_RegisterBool("bo1_zombies", true, 0, "");
#endif

    sv_cheats = _Dvar_RegisterBool("sv_cheats", true, 0, ""); 
#ifndef H5G
    dedicated = _Dvar_RegisterInt("dedicated", 0, 0, 2, 0, "");   // H5G: lo registra Com_InitDvars original (enum)
#endif
     cl_paused = _Dvar_RegisterInt("cl_paused", 0, 0, 2, 0, "");
    cg_draw2D = _Dvar_RegisterBool("cg_draw2D", true, 0, "");
    
#ifdef H5G
    sys_smp_allowed = _Dvar_RegisterBool("sys_smp_allowed", true, 0x10u, ""); Dvar_SetBool((dvar_s *)sys_smp_allowed, false);   // reset como el original (CpuCount > 1)
#else
    sys_smp_allowed = _Dvar_RegisterBool("sys_smp_allowed", false, 0, "");
#endif

    // OLA 7g: sys_sysMB (memoria fisica) como Sys_RegisterInfoDvars/Sys_ArchiveInfo (win_main.cpp). Sin registrar, Dvar_GetInt daba 0 y
    // R_SetPicmip (r_image.cpp, "texture detail automatic") forzaba minPicmip = 2 (sysMB <= 384): todas las texturas del mapa a 1/4 de lado.
    { long long mb = (long long)sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE) >> 20; if (getenv("H7_SYSMB")) mb = atoll(getenv("H7_SYSMB"));
      Dvar_SetInt((dvar_s *)_Dvar_RegisterInt("sys_sysMB", 0, 0x80000000, 0x7FFFFFFF, 0x11u, "Physical memory in the system"), (int)mb);
      fprintf(stderr, "[H7] sys_sysMB = %lld\n", mb); }
    R_RegisterDvars();
#ifdef H5G
    { void H10_PreInit(); H10_PreInit(); }   // OLA 10: picmip/filtro del config antes de cargar imagenes
#endif
    DB_Init();
    g_traceName = NameOf;

    // 1. device TRACE
    unsigned W = W0, H = H0;
    dx.d3d9 = Direct3DCreate9(32); dx.device = TraceDevice_Create(W, H);
    dx.adapterIndex = 0;
    GfxConfiguration cfg; memset(&cfg, 0, sizeof cfg);
    cfg.maxClientViews = 1; cfg.entCount = 1536; cfg.entnumNone = 1023; cfg.entnumOrdinaryEnd = 1022; cfg.threadContextCount = 15; cfg.critSectCount = 75;
    R_InitGlobalStructs();
    R_ConfigureRenderer(&cfg);          // SetGfxConfig + R_InitRenderCommands
    vidConfig.displayWidth = vidConfig.sceneWidth = W; vidConfig.displayHeight = vidConfig.sceneHeight = H; vidConfig.aspectRatioWindow = (float)W / (float)H; vidConfig.aspectRatioScenePixel = 1.0f;
    dx.depthStencilFormat = D3DFMT_D24S8;
    dx.windows[0].width = W; dx.windows[0].height = H; dx.windowCount = 1;
    fprintf(stderr, "[P3] device creado\n");

    // 2. zonas de gfx como DB_LoadGraphicsAssetsForPC (zombies): pre + post_mp + code_post_gfx (SP). Los materiales/imagenes/fuentes
    //    internos estan en code_post_gfx*, no en code_pre_gfx_mp (solo rawfiles de shader text): el orden pre->R_InitSystems->post no es viable.
#ifdef H5G
    if (getenv("H11_CONNECT") && getenv("H11C_NOLOAD")) { extern int H11C_Run(); return H11C_Run(); }   // OLA 11: cliente de red sin render ni zonas (pruebas de protocolo)
#endif
    H15_MemPhase("carga_zonas");
    LoadFF((G + "Common/code_pre_gfx_mp.ff").c_str());
    LoadFF((G + "english/en_code_pre_gfx_mp.ff").c_str());
    LoadFF((G + "Common/code_post_gfx_mp.ff").c_str());
    LoadFF((G + "english/en_code_post_gfx_mp.ff").c_str());
    LoadFF((G + "Common/code_post_gfx.ff").c_str());
    LoadFF((G + "english/en_code_post_gfx.ff").c_str());
    // 3. recursos minimos + render targets
    R_AllocateMinimalResources();
    if (!R_CreateForInitOrReset()) { fprintf(stderr, "R_CreateForInitOrReset fallo\n"); return 1; }
    RB_SetInitialState(); R_InitGamma(); R_InitScene();
    fprintf(stderr, "[P3] estado inicial listo\n");
    // 4. sistemas: imagenes, materiales internos, fuentes
    R_InitSystems();
    fprintf(stderr, "[P3] R_InitSystems ok\n");
    // 5. resto de zonas como H0 (common_zombie; los en_ si existen)
    LoadFF((G + "Common/common_zombie.ff").c_str());
    LoadFF((G + "english/en_common_zombie.ff").c_str());
#ifdef H5G
    { extern int (*g_h21Frontend)(int, int); if (getenv("H5_GAME") && g_h21Frontend) { const int r = g_h21Frontend(W, H); if (r >= 0) return r; } }   // OLA U1: menu principal (H21_MENU=1, h21_menu.cpp)
#endif

    if (getenv("H5_VM") || getenv("H5_GAME")) {   // H5a0: VM GSC original aislada (h5_vm.cpp); H5_MAP = zona del mapa con los _zombiemode*.gsc
        const char *hm = getenv("H5_MAP"); if (!hm) hm = H18_DefaultMap();   // OLA M0: lo fija H18_ResolveBoot (menu de mapas / H5_MAP / defecto)
        // orden del juego: common_zombie(_patch) y luego el mapa y su parche (los _ffotd y las correcciones viven en los _patch)
        // H5_ZONES=a,b,c sustituye la lista (zonas de Common/ en orden de carga)
        std::string zl = getenv("H5_ZONES") ? getenv("H5_ZONES") : std::string("common_zombie_patch,") + hm + "," + hm + "_patch";
        for (size_t p = 0; p < zl.size();) { size_t q = zl.find(',', p); if (q == std::string::npos) q = zl.size(); if (q > p) LoadFF((G + "Common/" + zl.substr(p, q - p) + ".ff").c_str()); p = q + 1; }
#ifdef H5G
        if (getenv("H5_GAME")) {   // H5a: servidor original (h5_game.cpp); las cadenas en_ del mapa tambien
            LoadFF((G + "english/en_" + hm + ".ff").c_str());
            if (getenv("H11_CONNECT")) { extern int H11C_Run(); return H11C_Run(); }   // OLA 11: cliente remoto
            extern int H5G_Run(); return H5G_Run();
        }
#endif
        extern int H5_Run(); return H5_Run();
    }
    if (const char *pm = getenv("P4_MAP")) {
        LoadFF((G + "Common/" + pm + ".ff").c_str());
        LoadFF((G + "english/en_" + pm + ".ff").c_str());
        extern void H4_Probe(); H4_Probe();
#ifdef BO1_APP
        // P7/H4: mapa 3D con camara libre; el bucle de frames lo gobierna la app (entrada tactil)
        if (!H4_Setup(W, H)) { fprintf(stderr, "[H4] sin GfxWorld\n"); return 1; }
        fprintf(stderr, "[H4] bucle de frames: mapa %s %ux%u\n", pm, W, H);
        H4Input in; float dt;
        while (BO1App_PreFrame3D(&in, &dt)) { H4_Update(in, dt); H4_Frame(); BO1App_PostFrame(); }
        return 0;
#else
        extern int H4_Run(int, int); return H4_Run(W, H);
#endif
    }
    // 6. un marco 2D
    if (getenv("P5_DUMP")) { P5_DumpMenu(getenv("P5_DUMP")); return 0; }
    if (getenv("P5_LIST")) { P5_ListMenus(); if (!getenv("P5_MENU")) return 0; }
#ifdef BO1_APP
    {
        const char *mn = BO1App_Menu(); UiContext *dc = P5_SetupUi(mn, W, H);
        if (!dc) { fprintf(stderr, "[P5] menu no encontrado\n"); return 1; }
        fprintf(stderr, "[P6] bucle de frames: menu %s %ux%u\n", mn, W, H);
        while (BO1App_PreFrame(dc)) {
            R_BeginFrame(); R_BeginSharedCmdList(); R_AddCmdProjectionSet2D();
            P5_PaintMenu(dc);
            R_EndFrame(); R_IssueRenderCommands(3u);
            BO1App_PostFrame();
        }
        return 0;
    }
#endif
    Material *white = Material_RegisterHandle("white", 3);
    Font_s *font = R_RegisterFont("fonts/normalFont", 3);
    fprintf(stderr, "[P3] white=%p font=%p\n", (void *)white, (void *)font);
    d3dtrace_log("==== MARCO ====");
    R_BeginFrame();
    R_BeginSharedCmdList();
    R_AddCmdProjectionSet2D();
    if (const char *mn = getenv("P5_MENU")) {
        UiContext *dc = P5_SetupUi(mn, W, H);
        if (!dc) { fprintf(stderr, "[P5] menu no encontrado\n"); return 1; }
        P5_PaintMenu(dc);
    } else {
    float red[4] = { 1, 0, 0, 1 };
    R_AddCmdDrawStretchPic(100, 100, 300, 200, 0, 0, 1, 1, red, white);
    float w[4] = { 1, 1, 1, 1 };
    char hola[] = "Hola";
    R_AddCmdDrawText(hola, 0x7fffffff, font, 120, 400, 1.0f, 1.0f, 0, w, 0);
    }
    R_EndFrame();
    { unsigned char *c=(unsigned char*)frontEndDataOut->cmds; fprintf(stderr,"[P3] cmds=%p used=%d bytes:",(void*)c,(int)frontEndDataOut->viewInfoCount); for(int i=0;i<48;i++) fprintf(stderr," %02x",c[i]); fprintf(stderr,"\n"); }
    R_IssueRenderCommands(3u);          // como CL_SCR: camino sincrono (r_smp_backend=0): RB_BeginFrame -> ejecutar cmds -> RB_EndFrame
    d3dtrace_log("==== FIN MARCO ====");
#ifdef M2_G9
    if (g9::Active()) { const char *o = getenv("P4_OUT"); int w, h; bool ok = g9::ReadbackPNG(o ? o : "p4_frame.png", &w, &h); printf("[P4] PNG %s %dx%d ok=%d (%s)\n", o ? o : "p4_frame.png", w, h, ok, g9::Info()); }
#endif
    printf("\n== resumen traza ==\npresents=%d beginScene=%d setViewport=%d setVS=%d setPS=%d setDecl=%d setTexture=%d draws=%d tris=%d\n", g_ts.presents, g_ts.beginScenes, g_ts.setViewports, g_ts.setVS, g_ts.setPS, g_ts.setDecl, g_ts.setTextures, g_ts.draws, g_ts.drawTris);
    printf("creados: tex=%d vb=%d ib=%d vs=%d ps=%d decl=%d\n", g_ts.textures, g_ts.vbs, g_ts.ibs, g_ts.vshaders, g_ts.pshaders, g_ts.decls);
    for (auto &kv : g_ts.defaults) printf("  default %s x%d\n", kv.first.c_str(), kv.second);
    return 0;
}

// r_texturemem.cpp usa DirectDraw (no compilable): memoria de texturas = lo que dice el device TRACE (512 MB)
unsigned int R_AvailableTextureMemory() { return dx.device->GetAvailableTextureMem() >> 20; }
const Material *Material_FromHandle(Material *h) { return h; }   // cgame/cg_drawtools.cpp: identidad en el exe de 32 bits
// r_ui3d.cpp (menus 3D) no se compila: pila UI3D vacia (nunca se esta dentro de un UI3D)
static GfxUI3DStack g_ui3dStack;
// RB_CheckTessOverflow no esta en la decompilacion (inline del exe): reconstruida desde sus XREF (vertexCount/indexCount)
void RB_TessOverflow();
void RB_CheckTessOverflow(int vc, int ic) { if (tess.vertexCount + vc > 5450 || tess.indexCount + ic > 1048576) RB_TessOverflow(); }
GfxUI3DStack *R_GetUI3DStack() { return &g_ui3dStack; }
int R_UI3DStack_Top(GfxUI3DStack *) { return -1; }
