// OLA 7g (fidelidad grafica): mips altos de las texturas con streaming.
// En BO1 PC las imagenes IMG_CATEGORY_LOAD_FROM_FILE con la bandera de streaming en la cabecera .iwi se cargan al principio SIN sus
// niveles mas detallados (Image_LoadToBuffer con loadHighmip=0 -> skippedMipLevels = Image_GetPcStreamedMips, 1-2 niveles). El sistema
// de streaming (r_stream.cpp: R_StreamUpdate -> R_StreamRequestImageRead -> hilo de streaming R_StreamUpdate_ReadTextures ->
// R_StreamUpdate_ProcessFileCallbacks) carga despues el fichero entero (loadHighmip=1) segun la importancia de lo que se ve. H4 no corre
// r_stream (front-end/dpvs sin portar), asi que el mundo, las armas y los zombis se veian a la mitad o a la cuarta parte de resolucion.
// Aqui se hace lo mismo que el camino de "render thread" de R_StreamUpdate_ProcessFileCallbacks, pero sincrono y para TODAS las imagenes
// con streaming de la zona al arrancar el mapa (el mapa entero cabe: se mide en el log): Image_LoadToBuffer(loadHighmip=1) ->
// Image_Release -> Image_LoadFromData; si falla, se deja la imagen como estaba.
//   H7_HIGHMIP=0   -> desactiva (texturas como antes)
//   H7_HIGHMIP_MAXDIM=N -> no sube imagenes cuyo tamano completo supere N (0 = sin limite)
#include <database/db_registry.h>
#include <database/db_assetnames.h>
#include <universal/ptr32.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_image_load_obj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <string>

char __cdecl Image_LoadToBuffer(GfxImage *image, bool loadHighmip, unsigned __int8 **imageBuffer, int *bufferSize);
void Z_VirtualFree(void *ptr, int track);

static std::vector<GfxImage *> s_imgs;
static void EnumImg(XAssetHeader h, void *) { if (h.image) s_imgs.push_back(h.image); }

void H7_LoadHighMips()
{
    const char *e = getenv("H7_HIGHMIP"); if (e && !atoi(e)) { fprintf(stderr, "[H7] mips altos: desactivado (H7_HIGHMIP=0)\n"); return; }
    const int maxDim = getenv("H7_HIGHMIP_MAXDIM") ? atoi(getenv("H7_HIGHMIP_MAXDIM")) : 0;
    s_imgs.clear(); DB_EnumXAssets_FastFile(ASSET_TYPE_IMAGE, EnumImg, 0, false);
    timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    int cand = 0, done = 0, failed = 0, skippedDim = 0; long long bytesRead = 0; unsigned before = 0, after = 0;
    for (GfxImage *img : s_imgs) {
        if (img->category != 3 || !img->streaming || !img->skippedMipLevels || !img->texture.basemap) continue;
        ++cand;
        const int fw = img->width << img->skippedMipLevels, fh = img->height << img->skippedMipLevels;
        if (maxDim && (fw > maxDim || fh > maxDim)) { ++skippedDim; continue; }
        unsigned char *buf = nullptr; int size = 0;
        GfxImage copy = *img;
        if (!Image_LoadToBuffer(&copy, true, &buf, &size) || !buf) { ++failed; continue; }
        const unsigned w0 = img->width, h0 = img->height; const GfxImage keep = *img;
        IDirect3DBaseTexture9 *oldTex = img->texture.basemap; if (oldTex) oldTex->AddRef();
        *img = copy;
        img->texture = keep.texture;            // Image_Release libera la textura vieja (la copia apunta a la misma)
        Image_Release(img);
        Image_LoadFromData(img, (GfxImageFileHeader *)buf, buf + sizeof(GfxImageFileHeader));
        Z_VirtualFree(buf, 20);
        if (!img->texture.basemap) { Image_Release(img); *img = keep; ++failed; continue; }   // como el SP: vuelve la imagen vieja
        if (oldTex) oldTex->Release();
        ++done; bytesRead += size; before += w0 * h0; after += img->width * img->height;
        if (getenv("H7_HIGHMIP_LOG")) fprintf(stderr, "[H7]   %s %ux%u -> %ux%u\n", img->name.get(), w0, h0, img->width, img->height);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    fprintf(stderr, "[H7] mips altos: %zu imagenes, %d con streaming pendiente, %d subidas (%d fallos, %d por encima de %d), %.1f MB leidos de .iwi, texeles nivel 0 %.1f M -> %.1f M, %.0f ms\n",
        s_imgs.size(), cand, done, failed, skippedDim, maxDim, bytesRead / 1048576.0, before / 1e6, after / 1e6,
        (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6);
}

// Diagnostico (H7_LMINFO=1): formato y tamano de lightmaps y sondas de reflexion del mundo
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_reflection_probe.h>
void H7_LightmapInfo(GfxWorld *w)
{
    if (!getenv("H7_LMINFO") || !w) return;
    auto info = [](const char *what, GfxImage *im) {
        if (!im) return; D3DSURFACE_DESC d; memset(&d, 0, sizeof d); int lv = 0;
        if (im->texture.map && im->mapType == 3) { im->texture.map->GetLevelDesc(0, &d); lv = im->texture.map->GetLevelCount(); }
        fprintf(stderr, "[H7] %s '%s' %ux%u mapType=%d cat=%d sem=%d fmt=%d niveles=%d\n", what, im->name.get(), im->width, im->height, im->mapType, im->category, im->semantic, (int)d.Format, lv);
    };
    for (int i = 0; i < w->draw.lightmapCount && i < 3; ++i) { info("lm.primary", w->draw.lightmaps[i].primary.get()); info("lm.secondary", w->draw.lightmaps[i].secondary.get()); info("lm.secondaryB", w->draw.lightmaps[i].secondaryB.get()); }
    fprintf(stderr, "[H7] lightmaps=%d sondas=%u\n", w->draw.lightmapCount, w->draw.reflectionProbeCount);
    for (unsigned i = 0; i < w->draw.reflectionProbeCount && i < 2; ++i) info("probe", w->draw.reflectionProbes[i].reflectionImage.get());
}

// Diagnostico: H7_DUMP_RAW="a,b" vuelca rawfiles SIN comprimir (p.ej. vision/*.vision) a H7_DUMP_DIR (fuera del repo)
void H7_DumpRaw()
{
    const char *dl = getenv("H7_DUMP_RAW"); if (!dl) return;
    std::string all = dl; size_t p0 = 0;
    while (p0 < all.size()) {
        size_t p1 = all.find(',', p0); if (p1 == std::string::npos) p1 = all.size();
        std::string nm = all.substr(p0, p1 - p0); p0 = p1 + 1;
        RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)nm.c_str(), 1, -1).rawfile;
        if (!rf) { fprintf(stderr, "[H7] raw %s: no esta\n", nm.c_str()); continue; }
        std::string fn = std::string(getenv("H7_DUMP_DIR") ? getenv("H7_DUMP_DIR") : ".") + "/" + nm.substr(nm.rfind('/') + 1);
        if (FILE *f = fopen(fn.c_str(), "wb")) { fwrite(rf->buffer.get(), 1, rf->len, f); fclose(f); fprintf(stderr, "[H7] raw %s -> %s (%d bytes)\n", nm.c_str(), fn.c_str(), rf->len); }
    }
}

// Vision file del mapa (vision/<mapa>.vision): en BO1 lo aplica CG_VisionSetStartLerp / CG_VisionSetApplyToRefdef al entrar en el
// mapa, poniendo los dvars r_film*, r_bloom* y r_primaryLight*. H4 no tiene cgame visual, asi que aqui se leen las lineas
// `dvar "valor"` y se aplican solo las familias que consume el render portado (H7_VISION=0 lo desactiva).
#include <universal/dvar.h>
static std::string s_visionTxt;
static std::string s_visName;   // OLA 7e: vision set activo (H7_VisionSetNaked)
void H7_ApplyVision(GfxWorld *w)
{
    const char *e = getenv("H7_VISION"); if ((e && !atoi(e)) || !w) return;
    std::string base = w->name.get(); size_t s = base.rfind('/'); if (s != std::string::npos) base = base.substr(s + 1);
    size_t d = base.find('.'); if (d != std::string::npos) base = base.substr(0, d);
    std::string nm = "vision/" + base + ".vision";
    RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)nm.c_str(), 1, -1).rawfile;
    if (!rf || !rf->buffer.get()) { fprintf(stderr, "[H7] vision %s: no esta\n", nm.c_str()); return; }
    std::string txt(rf->buffer.get(), rf->len); size_t p = 0; int n = 0; s_visionTxt = txt; s_visName = base;
    while (p < txt.size()) {
        size_t q = txt.find('\n', p); if (q == std::string::npos) q = txt.size();
        std::string ln = txt.substr(p, q - p); p = q + 1;
        size_t a = ln.find_first_not_of(" \t\r"); if (a == std::string::npos) continue;
        size_t b = ln.find_first_of(" \t", a); if (b == std::string::npos) continue;
        std::string key = ln.substr(a, b - a);
        size_t v0 = ln.find('"', b), v1 = v0 == std::string::npos ? v0 : ln.find('"', v0 + 1); if (v1 == std::string::npos) continue;
        std::string val = ln.substr(v0 + 1, v1 - v0 - 1);
        if (key.rfind("r_primaryLight", 0) && key.rfind("r_film", 0) && key.rfind("r_bloom", 0)) continue;
        if (!Dvar_FindVar(key.c_str())) continue;
        Dvar_SetFromStringByName(key.c_str(), (char *)val.c_str()); ++n;
    }
    fprintf(stderr, "[H7] vision %s: %d dvars aplicados\n", nm.c_str(), n);
}

// Diagnostico (H7_FINDCONST=N): que tecnicas leen la constante de codigo N
static void EnumTs(XAssetHeader h, void *ctx)
{
    const int want = *(int *)ctx; MaterialTechniqueSet *ts = h.techniqueSet; if (!ts) return;
    for (int t = 0; t < 130; ++t) { MaterialTechnique *te = ts->techniques[t].get(); if (!te) continue;
        for (int p = 0; p < te->passCount; ++p) { const MaterialPass &ps = te->passArray[p]; const int n = ps.perPrimArgCount + ps.perObjArgCount + ps.stableArgCount;
            for (int a = 0; a < n; ++a) { const MaterialShaderArgument &ar = ps.args.get()[a];
                if (ar.type != 0 && ar.type != 2 && ar.type != 7 && ar.u.codeConst.index == want) fprintf(stderr, "[H7] const %d: %s tech %d '%s' type=%d\n", want, ts->name.get(), t, te->name.get(), ar.type); } } }
}
void H7_FindConst()
{
    const char *e = getenv("H7_FINDCONST"); if (!e) return; int want = atoi(e);
    DB_EnumXAssets_FastFile(ASSET_TYPE_TECHNIQUE_SET, EnumTs, &want, false);
}

// ---- OLA 7g: postproceso ORIGINAL de BO1 (rb_postfx.cpp RB_BloomLDR): bloom LDR + curva en S de pelicula (bloom_apply_ldr) +
// correccion de color del vision set (postfx_color: matrices de temperatura/tono/saturacion x tintes x contraste por rangos
// oscuro/medio/claro). En el juego corre SIEMPRE tras la escena 3D (RB_ProcessPostEffects -> RB_BloomLDR) y antes del HUD 2D.
// H4 arrancaba con R_AllocateMinimalResources (sin render targets de postproceso): aqui se crean los que usa RB_BloomLDR con las
// mismas llamadas que R_InitRenderTargets_PC y se rellena film/bloom del GfxViewInfo desde vision/<mapa>.vision como hace
// CG_VisionSetApplyToRefdef (nombres = campo con prefijo r_).
//   H7_POSTFX=0 desactiva
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_rendertarget.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/rb_backend.h>
#include <gfx_d3d/rb_tess.h>
#include <gfx_d3d/rb_shade.h>
#include <gfx_d3d/r_state.h>
#include <gfx_d3d/rb_draw3d.h>
#include <stddef.h>
void __cdecl R_InitFullscreenRenderTargetImage(int, FullscreenType, int, _D3DFORMAT, RenderTargetUsage, GfxRenderTarget *);
void __cdecl RB_BloomLDR(const GfxViewInfo *viewInfo);
static GfxViewInfo *s_pvi; static int s_pfxState;   // 0 sin probar, 1 listo, -1 desactivado
struct VisField { const char *name; int off; int n; bool isInt; bool bloom; };
#define FF(f, n) { "r_" #f, (int)offsetof(GfxFilm, f), n, false, false }
#define BF(f, n) { "r_" #f, (int)offsetof(GfxBloom, f), n, false, true }
static const VisField kVis[] = {
    FF(filmMidStart, 1), FF(filmMidEnd, 1), FF(filmDarkFeather, 1), FF(filmLightFeather, 1), FF(filmBleach, 3), FF(filmColorTemp, 3),
    FF(filmHue, 3), FF(filmSaturation, 3), FF(filmDarkTint, 3), FF(filmMidTint, 3), FF(filmLightTint, 3), FF(filmContrast, 3),
    FF(sCurveShoulderStrength, 1), FF(sCurveLinearStrength, 1), FF(sCurveLinearAngle, 1), FF(sCurveToeStrength, 1), FF(sCurveToeNumerator, 1), FF(sCurveToeDenominator, 1),
    BF(bloomTintWeights, 4), BF(bloomColorScale, 4), BF(bloomTintScale, 4), BF(bloomCurveBreakpoint, 4), BF(bloomCurveLoBlack, 4), BF(bloomCurveLoGamma, 4),
    BF(bloomCurveLoWhite, 4), BF(bloomCurveLoRemapBlack, 4), BF(bloomCurveLoRemapWhite, 4), BF(bloomCurveHiBlack, 4), BF(bloomCurveHiGamma, 4),
    BF(bloomCurveHiWhite, 4), BF(bloomCurveHiRemapBlack, 4), BF(bloomCurveHiRemapWhite, 4), BF(bloomExpansionControl, 4), BF(bloomExpansionWeights, 4),
    { "r_bloomExpansionSource", (int)offsetof(GfxBloom, bloomExpansionSource), 1, true, true }, BF(bloomBlurRadius, 1), BF(bloomPersistence, 1),
    BF(bloomStreakXLevels0, 4), BF(bloomStreakXLevels1, 4), BF(bloomStreakXInnerTint, 3), BF(bloomStreakXOuterTint, 3), BF(bloomStreakXTintControl, 4), BF(bloomStreakXTint, 3),
    BF(bloomStreakYLevels0, 4), BF(bloomStreakYLevels1, 4), BF(bloomStreakYInnerTint, 3), BF(bloomStreakYOuterTint, 3), BF(bloomStreakYTintControl, 4), BF(bloomStreakYTint, 3),
};
static void ParseVisionInto(GfxViewInfo *vi)
{
    size_t p = 0; int n = 0;
    while (p < s_visionTxt.size()) {
        size_t q = s_visionTxt.find('\n', p); if (q == std::string::npos) q = s_visionTxt.size();
        std::string ln = s_visionTxt.substr(p, q - p); p = q + 1;
        size_t a = ln.find_first_not_of(" \t\r"); if (a == std::string::npos) continue;
        size_t b = ln.find_first_of(" \t", a); if (b == std::string::npos) continue;
        std::string key = ln.substr(a, b - a);
        size_t v0 = ln.find('"', b), v1 = v0 == std::string::npos ? v0 : ln.find('"', v0 + 1); if (v1 == std::string::npos) continue;
        std::string val = ln.substr(v0 + 1, v1 - v0 - 1);
        if (key == "r_filmEnable") { vi->film.enabled = atof(val.c_str()) != 0; continue; }
        for (const VisField &f : kVis) if (key == f.name) {
            char *base = f.bloom ? (char *)&vi->bloom : (char *)&vi->film;
            if (f.isInt) *(int *)(base + f.off) = atoi(val.c_str());
            else { float v[4] = {0, 0, 0, 0}; sscanf(val.c_str(), "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]); memcpy(base + f.off, v, 4 * f.n); }
            ++n; break; }
    }
    fprintf(stderr, "[H7] postfx: %d campos de film/bloom del vision set\n", n);
}
// Los materiales de postfx de code_post_gfx llegan con shaders sin crear (prog.vs/ps nulos con loadDef valido): se crean aqui con
// las mismas funciones del cargador (Load_CreateMaterialVertexShader / PixelShader) que usa el DB al cargar la zona.
static int EnsureShaders(Material *m)
{
    int made = 0; if (!m || !m->techniqueSet) return 0; MaterialTechniqueSet *ts = m->techniqueSet.get();
    for (int t = 0; t < 130; ++t) { MaterialTechnique *te = ts->techniques[t].get(); if (!te) continue;
        for (int p = 0; p < te->passCount; ++p) { MaterialPass &ps = te->passArray[p];
            if (MaterialVertexShader *vs = ps.vertexShader.get()) if (!vs->prog.vs && vs->prog.loadDef.program) { Load_CreateMaterialVertexShader(&vs->prog.loadDef, vs); ++made; }
            if (MaterialPixelShader *px = ps.pixelShader.get()) if (!px->prog.ps && px->prog.loadDef.program) { Load_CreateMaterialPixelShader(&px->prog.loadDef, px); ++made; } } }
    return made;
}
static bool PostFxInit()
{
    const char *e = getenv("H7_POSTFX"); if (e && !atoi(e)) return false;
    if (s_visionTxt.empty()) { fprintf(stderr, "[H7] postfx: sin vision set, desactivado\n"); return false; }
    if (!rgp.bloomDownsampleLDR || !rgp.resampleFinal || !rgp.bloomDownsampleConvolution || !rgp.bloomRemap || !rgp.bloomBlurX || !rgp.bloomBlurY
        || !rgp.bloomSmoothA || !rgp.bloomApplyLDR || !rgp.postFxColorMaterial) { fprintf(stderr, "[H7] postfx: faltan materiales de bloom/postfx, desactivado\n"); return false; }
    { int made = 0; Material *ms[] = { rgp.bloomDownsampleLDR, rgp.resampleFinal, rgp.bloomDownsampleConvolution, rgp.bloomRemap, rgp.bloomBlurX, rgp.bloomBlurY, rgp.bloomSmoothA, rgp.bloomApplyLDR, rgp.postFxColorMaterial, rgp.bloomStreakX, rgp.bloomStreakY, rgp.bloomRemapStreak };
      for (Material *m : ms) made += EnsureShaders(m); if (made) fprintf(stderr, "[H7] postfx: %d shaders creados (estaban sin crear)\n", made); }
    // con R_AllocateMinimalResources el FRAME_BUFFER queda con depthStencil nulo (R_InitFrameBufferRenderTarget_Win32) y se usa el
    // depth automatico del dispositivo; al volver de los render targets de bloom R_HW_SetRenderTarget no lo religaria. Se le asigna
    // el depth del dispositivo (lo que en el juego completo es la superficie creada para el frame buffer).
    if (!gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER].surface.depthStencil) {
        IDirect3DSurface9 *ds = nullptr; dx.device->GetDepthStencilSurface(&ds);
        if (!ds) { fprintf(stderr, "[H7] postfx: sin depth del dispositivo, desactivado\n"); return false; }
        gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER].surface.depthStencil = ds;
        if (!gfxRenderTargets[R_RENDERTARGET_SCENE].surface.depthStencil) { ds->AddRef(); gfxRenderTargets[R_RENDERTARGET_SCENE].surface.depthStencil = ds; }
    }
    D3DSURFACE_DESC d; memset(&d, 0, sizeof d);
    if (gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER].surface.color) gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER].surface.color->GetDesc(&d);
    const _D3DFORMAT bb = d.Format != D3DFMT_UNKNOWN ? d.Format : D3DFMT_A8R8G8B8;
    // mismas llamadas que R_InitFrameBufferRenderTarget / R_InitRenderTargets_PC (rama !g_allocateMinimalResources)
    if (!gfxRenderTargets[R_RENDERTARGET_RESOLVED_SCENE].image) R_InitFullscreenRenderTargetImage(9, FULLSCREEN_SCENE, 0, bb, RENDERTARGET_USAGE_RENDER, &gfxRenderTargets[R_RENDERTARGET_RESOLVED_SCENE]);
    const _D3DFORMAT bf = getenv("H7_BLOOM8") ? D3DFMT_A8R8G8B8 : D3DFMT_A16B16G16R16;
    struct { int prog, pic; GfxRenderTargetId id; } rts[] = { {23, 1, R_RENDERTARGET_BLOOM_MIP1}, {24, 2, R_RENDERTARGET_BLOOM_MIP2}, {25, 3, R_RENDERTARGET_BLOOM_MIP3},
                                                              {26, 3, R_RENDERTARGET_BLOOM_MIP3_PING}, {27, 3, R_RENDERTARGET_BLOOM_MIP3_PONG} };
    for (auto &r : rts) if (!gfxRenderTargets[r.id].image) R_InitFullscreenRenderTargetImage(r.prog, FULLSCREEN_SCENE, r.pic, bf, RENDERTARGET_USAGE_RENDER, &gfxRenderTargets[r.id]);
    if (!gfxRenderTargets[R_RENDERTARGET_BLOOM_STREAK].image) R_InitFullscreenRenderTargetImage(28, FULLSCREEN_SCENE, 3, D3DFMT_A8R8G8B8, RENDERTARGET_USAGE_RENDER, &gfxRenderTargets[R_RENDERTARGET_BLOOM_STREAK]);
    for (auto &r : rts) if (!gfxRenderTargets[r.id].image || !gfxRenderTargets[r.id].surface.color) { fprintf(stderr, "[H7] postfx: render target %d sin crear, desactivado\n", (int)r.id); return false; }
    if (!gfxRenderTargets[R_RENDERTARGET_RESOLVED_SCENE].image) { fprintf(stderr, "[H7] postfx: sin RESOLVED_SCENE, desactivado\n"); return false; }
    s_pvi = (GfxViewInfo *)calloc(1, sizeof(GfxViewInfo));
    s_pvi->isRenderingFullScreen = true;
    ParseVisionInto(s_pvi);
    fprintf(stderr, "[H7] postfx: listo (escena %ux%u fmt %d, bloom fmt %d, mip1 %ux%u mip3 %ux%u)\n", gfxRenderTargets[R_RENDERTARGET_RESOLVED_SCENE].width, gfxRenderTargets[R_RENDERTARGET_RESOLVED_SCENE].height,
        (int)bb, (int)bf, gfxRenderTargets[R_RENDERTARGET_BLOOM_MIP1].width, gfxRenderTargets[R_RENDERTARGET_BLOOM_MIP1].height, gfxRenderTargets[R_RENDERTARGET_BLOOM_MIP3].width, gfxRenderTargets[R_RENDERTARGET_BLOOM_MIP3].height);
    return true;
}
// ---- OLA 7e: VisionSetNaked(name, transition) del script de cliente (zombie_pentagon.csc: vis1-4 por planta, offices_poweroff tras la
// corriente). Como CG_VisionSetStartLerp / CG_VisionSetLerp: se toma lo que se ve ahora como origen, el vision file nuevo como destino y se
// interpola cada campo de film/bloom con el tiempo del cliente; los dvars r_film*/r_bloom*/r_primaryLight* se ponen con los del destino.
// Lo llama h7e_light.cpp al recibir el levelNotify. H7E_VISION=0 lo desactiva (se queda el vision file del mapa, como antes).
static GfxFilm s_visFrom, s_visTo; static GfxBloom s_visFromB, s_visToB; static int s_visT0 = -1, s_visDur, s_visNow;
void H7_VisionTime(int nowMs) { s_visNow = nowMs; }
static void H7_VisionTick()
{
    if (s_visT0 < 0 || !s_pvi) return;
    const float f = s_visDur <= 0 ? 1.0f : (float)(s_visNow - s_visT0) / (float)s_visDur;
    const float t = f < 0 ? 0 : f > 1 ? 1 : f;
    for (const VisField &fd : kVis) {
        char *pb = (fd.bloom ? (char *)&s_pvi->bloom : (char *)&s_pvi->film) + fd.off;
        const char *pa = (fd.bloom ? (const char *)&s_visFromB : (const char *)&s_visFrom) + fd.off, *pz = (fd.bloom ? (const char *)&s_visToB : (const char *)&s_visTo) + fd.off;
        if (fd.isInt) *(int *)pb = t < 1 ? *(const int *)pa : *(const int *)pz;
        else for (int k = 0; k < fd.n; ++k) ((float *)pb)[k] = ((const float *)pa)[k] + (((const float *)pz)[k] - ((const float *)pa)[k]) * t; }
    s_pvi->film.enabled = s_visTo.enabled;
    if (t >= 1) s_visT0 = -1;
}
bool H7_VisionSetNaked(const char *name, float seconds)
{
    if (!name || !*name) return false;
    if (s_visName == name) return true;
    std::string nm = std::string("vision/") + name + ".vision";
    RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)nm.c_str(), 1, -1).rawfile;
    if (!rf || !rf->buffer.get()) { fprintf(stderr, "[H7E] vision %s: no esta\n", nm.c_str()); return false; }
    s_visName = name;
    s_visionTxt.assign(rf->buffer.get(), rf->len);
    int n = 0; size_t p = 0;   // dvars, como H7_ApplyVision
    while (p < s_visionTxt.size()) {
        size_t q = s_visionTxt.find('\n', p); if (q == std::string::npos) q = s_visionTxt.size();
        std::string ln = s_visionTxt.substr(p, q - p); p = q + 1;
        size_t a = ln.find_first_not_of(" \t\r"); if (a == std::string::npos) continue;
        size_t b = ln.find_first_of(" \t", a); if (b == std::string::npos) continue;
        std::string key = ln.substr(a, b - a);
        size_t v0 = ln.find('"', b), v1 = v0 == std::string::npos ? v0 : ln.find('"', v0 + 1); if (v1 == std::string::npos) continue;
        if (key.rfind("r_primaryLight", 0) && key.rfind("r_film", 0) && key.rfind("r_bloom", 0)) continue;
        if (!Dvar_FindVar(key.c_str())) continue;
        std::string val = ln.substr(v0 + 1, v1 - v0 - 1); Dvar_SetFromStringByName(key.c_str(), (char *)val.c_str()); ++n;
    }
    if (s_pvi) {
        s_visFrom = s_pvi->film; s_visFromB = s_pvi->bloom;
        GfxViewInfo tmp; memset(&tmp, 0, sizeof tmp); tmp.film = s_pvi->film; tmp.bloom = s_pvi->bloom; ParseVisionInto(&tmp);
        s_visTo = tmp.film; s_visToB = tmp.bloom; s_visT0 = s_visNow; s_visDur = (int)(seconds * 1000.0f);
    }
    fprintf(stderr, "[H7E] VisionSetNaked %s (%.1f s): %d dvars\n", name, seconds, n);
    return true;
}
// Llamado al final de la escena 3D (world_h4 RenderFrame, tras actores/arma/efectos) con el render target FRAME_BUFFER (= SCENE) activo
bool g_h10PostFxOff;   // OLA 10: "BLOOM / POST FX" del menu de ajustes (en caliente)
void H7_PostFX()
{
    if (g_h10PostFxOff) return;
    if (s_pfxState == 0) s_pfxState = PostFxInit() ? 1 : -1;
    if (s_pfxState < 0) return;
    if (tess.indexCount) RB_EndTessSurface();
    if (gfxCmdBufContext.state->renderTargetId != R_RENDERTARGET_SCENE && gfxCmdBufContext.state->renderTargetId != R_RENDERTARGET_FRAME_BUFFER) return;
    const GfxRenderTargetId keep = (GfxRenderTargetId)gfxCmdBufContext.state->renderTargetId;
    gfxCmdBufContext.state->renderTargetId = R_RENDERTARGET_SCENE;   // RB_GetResolvedScene/RB_BloomLDR esperan SCENE (compartido con FRAME_BUFFER)
    R_Resolve(gfxCmdBufContext, gfxRenderTargets[R_RENDERTARGET_RESOLVED_SCENE].image);
    R_SetCodeImageTexture(&gfxCmdBufSourceState, 0xAu, gfxRenderTargets[R_RENDERTARGET_RESOLVED_SCENE].image);
    H7_VisionTick();
    RB_BloomLDR(s_pvi);
    if (tess.indexCount) RB_EndTessSurface();
    R_SetRenderTargetSize(&gfxCmdBufSourceState, keep);
    R_SetRenderTarget(gfxCmdBufContext, keep);
}
