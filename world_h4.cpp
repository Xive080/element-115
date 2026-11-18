// H4: mapa 3D estatico (zombie_pentagon) con camara libre. Usa las piezas del motor (R_SetupPass, R_DrawBspDrawSurfsLit, ...)
// con un bucle propio en lugar del front-end (dpvs/R_AddWorldSurfaces...), que no esta portado.
#include <database/db_registry.h>
#include <database/db_assetnames.h>
#include <universal/ptr32.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/rb_state.h>
#include <gfx_d3d/rb_backend.h>
#include <gfx_d3d/r_state.h>
#include <gfx_d3d/r_state_utils.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/rb_draw3d.h>
#include <gfx_d3d/rb_tess.h>
#include <gfx_d3d/rb_stats.h>
#include <gfx_d3d/rb_shade.h>
#include <gfx_d3d/r_shade.h>
#include <gfx_d3d/r_draw_bsp.h>
#include <gfx_d3d/r_vertexstream2.h>
#include <gfx_d3d/r_draw_material.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <gfx_d3d/r_reflection_probe.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_image_load_common.h>
#include <gfx_d3d/r_dvars.h>
#include <d3d9.h>
#include <stdlib.h>
#include <math.h>
#include <qcommon/cm_load_obj.h>
#include "world_h4.h"
#include "trace.h"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <map>
#include <algorithm>
#include <string>

static GfxWorld *g_found;
static void EnumWorld(XAssetHeader h, void *) { if (!g_found) g_found = h.gfxWorld; }

GfxWorld *H4_FindWorld()
{
    DB_EnumXAssets_FastFile(ASSET_TYPE_GFXWORLD, EnumWorld, 0, false);
    return g_found;
}

static MapEnts *g_ents;
static void EnumEnts(XAssetHeader h, void *) { if (!g_ents) g_ents = h.mapEnts; }
#include <qcommon/cm_trace.h>
#include <qcommon/cm_load.h>
static void EnumClip(XAssetHeader h, void *) { clipMap_t *c = h.clipMap; fprintf(stderr, "[H4] clipmap '%s' planes=%d brushSides=%u nodes=%u leafs=%u leafBrushNodes=%u verts=%u tris=%d borders=%d partitions=%d brushes=%u models=%u (cm=%p)\n", c->name.get(), c->planeCount, c->numBrushSides, c->numNodes, c->numLeafs, c->leafbrushNodesCount, c->vertCount, c->triCount, c->borderCount, c->partitionCount, c->numBrushes, c->numSubModels, (void *)c); }
void H4_Probe()
{
    DB_EnumXAssets_FastFile(ASSET_TYPE_CLIPMAP_PVS, EnumClip, 0, false);
    DB_EnumXAssets_FastFile(ASSET_TYPE_MAP_ENTS, EnumEnts, 0, false);
    if (g_ents) {
        fprintf(stderr, "[H4] mapents '%s' %d chars\n", g_ents->name.get(), g_ents->numEntityChars);
        if (const char *e = getenv("H4_ENTS")) { FILE *f = fopen(e, "wb"); if (f) { fwrite(g_ents->entityString.get(), 1, g_ents->numEntityChars, f); fclose(f); } }
    }
    GfxWorld *w = H4_FindWorld();
    if (!w) { fprintf(stderr, "[H4] sin GfxWorld\n"); return; }
    fprintf(stderr, "[H4] world '%s' base '%s' surfaces=%d static=%u verts=%u idx=%d lightmaps=%d probes=%u smodels=%u\n",
        w->name.get(), w->baseName.get(), w->surfaceCount, w->dpvs.staticSurfaceCount, w->draw.vertexCount, w->draw.indexCount,
        w->draw.lightmapCount, w->draw.reflectionProbeCount, w->dpvs.smodelCount);
    fprintf(stderr, "[H4] worldVb=%p vld=%p vertices=%p indices=%p mins=%.0f,%.0f,%.0f maxs=%.0f,%.0f,%.0f\n", (void *)w->draw.vd.worldVb.get(), (void *)w->draw.vld.layerVb.get(),
        (void *)w->draw.vd.vertices.get(), (void *)w->draw.indices.get(), w->mins[0], w->mins[1], w->mins[2], w->maxs[0], w->maxs[1], w->maxs[2]);
    std::map<std::string, int> mats; int nolit = 0;
    for (int i = 0; i < w->surfaceCount; ++i) {
        GfxSurface *s = &w->dpvs.surfaces[i];
        Material *m = s->material.get(); if (!m) continue;
        mats[m->info.name.get()]++;
        if (!Material_GetTechnique(m, 10)) nolit++;
    }
    fprintf(stderr, "[H4] materiales distintos=%zu sin tecnica LIT=%d\n", mats.size(), nolit);
    int k = 0; for (auto &kv : mats) { if (k++ < 15) fprintf(stderr, "   %s x%d\n", kv.first.c_str(), kv.second); }
}

#if defined(D3D9_GLES)
#include "../gles/d3d9_gles.h"
#define H4_G9 1
#elif defined(D3D9_VK)
#include "../vk/g9_api.h"
#define H4_G9 1
#endif

#include "bsp_static.cpp"   // T10: IB estatico del mundo + culling por frustum

static void SetupViewProj(GfxViewParms *vp)
{
    MatrixMultiply44(vp->viewMatrix.m, vp->projectionMatrix.m, vp->viewProjectionMatrix.m);
    MatrixInverse44(vp->viewProjectionMatrix.m, vp->inverseViewProjectionMatrix.m);
}

struct Cam { float org[3]; float ang[3]; };
static GfxViewParms g_vp;
static void BuildView(const Cam &c, int W, int H, float fovX)
{
    memset(&g_vp, 0, sizeof g_vp);
    memcpy(g_vp.origin, c.org, 12); g_vp.origin[3] = 1.0f;
    AnglesToAxis(c.ang, g_vp.axis);
    MatrixForViewer(g_vp.origin, g_vp.axis, g_vp.viewMatrix.m);
    g_vp.zNear = 4.0f;
    float tx = tanf(fovX * 0.5f * (float)M_PI / 180.0f), ty = tx * (float)H / (float)W;
    InfinitePerspectiveMatrix(tx, ty, g_vp.zNear, g_vp.projectionMatrix.m);
    g_vp.depthHackNearClip = 0.0f;
    SetupViewProj(&g_vp);
}

// Un lote: surfaces de una misma material -> lista de indices (formato primDrawSurfsBuf: [n][n x u16 empaquetados] ... 0)
struct Batch { Material *m; std::vector<unsigned short> idx; unsigned char prim; };   // prim: luz primaria de las superficies (GfxSurface::primaryLightIndex)
static std::vector<Batch> g_batches;
static std::vector<std::vector<unsigned>> g_prim;   // buffers primDrawSurfPos por lote
static int g_techLit[2048];

// formato primDrawSurfsBuf: [n][n x u16 empaquetados de dos en dos][0]
static void PackPrim(std::vector<unsigned> &p, const std::vector<unsigned short> &v)
{
    p.clear(); p.push_back((unsigned)v.size());
    size_t n = (v.size() + 1) / 2; size_t base = p.size(); p.resize(base + n + 1, 0);
    if (!v.empty()) memcpy(&p[base], v.data(), v.size() * 2);
    p[base + n] = 0;
}

static int g_skyskip;
static void BuildBatches(GfxWorld *w)
{
    // OLA 7g: lote = (material, luz primaria) como las drawsurfs del original (R_AddBspDrawSurfs: el campo primaryLightIndex de la
    // drawsurf elige la tecnica LIT/LIT_SPOT/LIT_OMNI en R_GetTechType y la luz en R_SetShadowableLight) y en el ORDEN de las drawsurfs
    // (primarySortKey del material: opacos, luego decals y mezclas). Antes: un lote por material, en orden de puntero, todo con LIT.
    // H7_WORLDPRIM=0 vuelve a un lote por material sin luz primaria.
    static const bool byPrim = !(getenv("H7_WORLDPRIM") && !atoi(getenv("H7_WORLDPRIM")));
    std::map<std::pair<Material *, int>, std::vector<unsigned short>> by;
    for (int i = 0; i < w->surfaceCount; ++i) {
        Material *m = w->dpvs.surfaces[i].material.get(); if (!m) continue;
        { extern bool H7C_SkipWorldSurf(GfxWorld *, int) __attribute__((weak)); if (H7C_SkipWorldSurf && H7C_SkipWorldSurf(w, i)) continue; }   // OLA 7c: brushmodels con su entidad
        if (getenv("H15F_SKYLOG") && strstr(m->info.name.get(), "sky")) { const GfxSurface &q = w->dpvs.surfaces[i];   // OLA 15F: donde esta el "cielo"
            fprintf(stderr, "[H15F] cielo surf %d %s tris=%d (%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n", i, m->info.name.get(), (int)q.tris.triCount,
                    q.bounds[0][0], q.bounds[0][1], q.bounds[0][2], q.bounds[1][0], q.bounds[1][1], q.bounds[1][2]); }
        if (!getenv("H4_SKY") && strstr(m->info.name.get(), "sky")) { ++g_skyskip; continue; }
        by[{ m, byPrim ? (int)w->dpvs.surfaces[i].primaryLightIndex : 0 }].push_back((unsigned short)i);
    }
    for (auto &kv : by) {
        auto &v = kv.second;
        std::sort(v.begin(), v.end(), [&](unsigned short a, unsigned short b) { return w->dpvs.surfaces[a].tris.baseIndex < w->dpvs.surfaces[b].tris.baseIndex; });
        g_batches.push_back({ kv.first.first, v, (unsigned char)kv.first.second });
    }
    if (!(getenv("H7_SORT") && !atoi(getenv("H7_SORT"))))
        std::stable_sort(g_batches.begin(), g_batches.end(), [](const Batch &a, const Batch &b) {
            const unsigned ka = a.m->info.drawSurf.fields.primarySortKey, kb = b.m->info.drawSurf.fields.primarySortKey;
            return ka != kb ? ka < kb : a.prim < b.prim; });
    g_prim.resize(g_batches.size());
    for (size_t b = 0; b < g_batches.size(); ++b) PackPrim(g_prim[b], g_batches[b].idx);
    fprintf(stderr, "[H4] lotes=%zu (surfaces de cielo omitidas: %d)\n", g_batches.size(), g_skyskip);
}

static int g_drawn, g_skipped, g_passes;
static int g_surfVis, g_surfTot, g_batVis, g_cullOn = 1; static GfxWorld *g_cw;
// H4_CULLCHECK=1: contraste independiente con la matriz viewProjection real (clip = [x y z 1] * M, convenio D3D de fila)
static int g_cullCheck, g_ccFalseCull, g_ccClipVisible, g_ccClipHidden, g_ccMineVisClipHidden;
static bool ClipOutside(const GfxSurface *sf)
{
    const float (*M)[4] = g_vp.viewProjectionMatrix.m; int out[5] = { 0, 0, 0, 0, 0 };
    for (int c = 0; c < 8; ++c) {
        float p[4] = { sf->bounds[c & 1][0], sf->bounds[(c >> 1) & 1][1], sf->bounds[(c >> 2) & 1][2], 1 }, q[4];
        for (int j = 0; j < 4; ++j) q[j] = p[0] * M[0][j] + p[1] * M[1][j] + p[2] * M[2][j] + p[3] * M[3][j];
        out[0] += q[0] < -q[3]; out[1] += q[0] > q[3]; out[2] += q[1] < -q[3]; out[3] += q[1] > q[3]; out[4] += q[3] <= 0;
    }
    for (int k = 0; k < 5; ++k) if (out[k] == 8) return true;
    return false;
}
static void CullCheck(const GfxSurface *sf, bool mineVisible)
{
    bool clipHidden = ClipOutside(sf);
    clipHidden ? ++g_ccClipHidden : ++g_ccClipVisible;
    if (!mineVisible && !clipHidden) ++g_ccFalseCull;            // culling mio descarta algo que la matriz real vería: BUG
    if (mineVisible && clipHidden) ++g_ccMineVisClipHidden;      // conservador (no es error)
}
// T10: surfaces cuyo AABB cae dentro del frustum -> g_prim[b] por frame (lotes sin surfaces visibles no se dibujan)
static void CullBatches(const Frustum *fr)
{
    static std::vector<unsigned short> vis;
    g_surfVis = g_surfTot = g_batVis = 0; g_ccFalseCull = g_ccClipVisible = g_ccClipHidden = g_ccMineVisClipHidden = 0; g_cullCheck = getenv("H4_CULLCHECK") != 0;
    for (size_t b = 0; b < g_batches.size(); ++b) {
        const auto &v = g_batches[b].idx; vis.clear();
        for (unsigned short si : v) { const GfxSurface *sf = &g_cw->dpvs.surfaces[si]; bool in = !fr || BspStatic_BoxVisible(*fr, sf->bounds[0], sf->bounds[1]); if (in) vis.push_back(si); if (fr && g_cullCheck) CullCheck(sf, in); }
        g_surfTot += (int)v.size(); g_surfVis += (int)vis.size(); g_batVis += !vis.empty();
        PackPrim(g_prim[b], vis);
    }
}
unsigned char (*g_h4WorldPrimaryTech)(unsigned char primaryLightIndex);   // h5_light.cpp: tecnica y constantes de la luz primaria
// OLA 9 (h9_shadow.cpp): vista del frame y luces primarias de los lotes visibles en el ultimo culling (como scene.shadowableLightIsUsed)
const GfxViewParms *H4_ViewParms() { return &g_vp; }
void H4_LightsUsed(unsigned char *used, unsigned n) { memset(used, 0, n); for (size_t b = 0; b < g_batches.size(); ++b) if (g_batches[b].prim < n && !g_prim[b].empty() && g_prim[b][0]) used[g_batches[b].prim] = 1; }
static void DrawBatch(size_t b, unsigned char techType)
{
    GfxCmdBufContext ctx = gfxCmdBufContext;
    Material *m = g_batches[b].m;
    const bool litFamily = techType == 10;
    if (litFamily && g_batches[b].prim && g_h4WorldPrimaryTech) {   // R_GetTechType + R_SetShadowableLight (sin mapa de sombras)
        const unsigned char t = g_h4WorldPrimaryTech(g_batches[b].prim);
        if (t != 10 && Material_GetTechnique(m, t)) techType = t;
    }
    const MaterialTechnique *tech = Material_GetTechnique(m, techType);
    if (!tech) { ++g_skipped; return; }
    ctx.state->material = m; ctx.state->technique = tech; ctx.state->techType = techType;
    ctx.state->origMaterial = m; ctx.state->origTechType = techType;
    static std::map<std::string, int> seen;
    if (getenv("H4_ARGS") && seen.insert({ tech->name.get(), 1 }).second && seen.size() < 40) {
        const MaterialPass *ps = &tech->passArray[0]; int n = ps->perPrimArgCount + ps->perObjArgCount + ps->stableArgCount;
        fprintf(stderr, "[H4args] %s (%s) tech=%s prim=%d obj=%d stable=%d:", m->info.name.get(), Material_GetTechniqueSet(m)->name.get(), tech->name.get(), ps->perPrimArgCount, ps->perObjArgCount, ps->stableArgCount);
        for (int i = 0; i < n; ++i) { const MaterialShaderArgument *a = &ps->args[i]; fprintf(stderr, " [t%d d%d i%d]", a->type, a->dest, (a->type == 3 || a->type == 5) ? a->u.codeConst.index : (int)a->u.codeSampler); }
        fprintf(stderr, "\n");
    }
    for (unsigned pass = 0; pass < tech->passCount; ++pass) {
        R_UpdateMaterialTime(ctx.source, 0.0f, 0.0f, 0.0f, 0.0f);
        R_SetupPass(ctx, pass);
        // cuerpo de R_TessTrianglesList
        R_SetupPassCriticalPixelShaderArgs(ctx);
        R_SetObjectIdentityPlacement(ctx.source);
        R_ChangeDepthHackNearClip(ctx.source, 0);
        R_SetVertexDeclTypeWorld(ctx.state);
        R_TrackPrims(&ctx.state->prim, GFX_PRIM_STATS_WORLD);
        if (litFamily) R_SetTerrainScorchTextures(ctx.source, m);   // OLA M3: como R_TessTrianglesList (Nacht: materiales por capas, code texture 29+)
        R_SetupPassPerObjectArgs(ctx);
        R_SetupPassPerPrimArgs(ctx);
        if (litFamily) { GfxCmdBufContext none = { 0, 0 }; R_DrawBspDrawSurfsLit(g_prim[b].data(), ctx, none); }
        else R_DrawBspDrawSurfs(g_prim[b].data(), ctx.state);
        ctx.state->prim.primStats = 0;
        ++g_passes;
    }
    ++g_drawn;
}

// OLA 7c: lista de superficies BSP con colocacion de entidad (brushmodels: como R_TessBModel -> R_ChangeObjectPlacementRemote)
void H4_DrawSurfsPlaced(Material *m, const unsigned short *surfs, int n, unsigned char prim, const GfxScaledPlacement *pl, unsigned char techType)
{
    static std::vector<unsigned> pk; static std::vector<unsigned short> v; v.assign(surfs, surfs + n); PackPrim(pk, v);
    GfxCmdBufContext ctx = gfxCmdBufContext;
    const bool litFamily = techType == 10;
    if (litFamily && prim && g_h4WorldPrimaryTech) { const unsigned char t = g_h4WorldPrimaryTech(prim); if (t != 10 && Material_GetTechnique(m, t)) techType = t; }
    const MaterialTechnique *tech = Material_GetTechnique(m, techType); if (!tech) { ++g_skipped; return; }
    ctx.state->material = m; ctx.state->technique = tech; ctx.state->techType = techType; ctx.state->origMaterial = m; ctx.state->origTechType = techType;
    for (unsigned pass = 0; pass < tech->passCount; ++pass) {
        R_UpdateMaterialTime(ctx.source, 0.0f, 0.0f, 0.0f, 0.0f);
        R_SetupPass(ctx, pass);
        R_SetupPassCriticalPixelShaderArgs(ctx);
        if (ctx.source->objectPlacement != pl) { ctx.source->objectPlacement = pl; R_ChangeObjectPlacementRemote(ctx.source, pl); }
        R_ChangeDepthHackNearClip(ctx.source, 0);
        R_SetVertexDeclTypeWorld(ctx.state);
        R_TrackPrims(&ctx.state->prim, GFX_PRIM_STATS_BMODEL);
        if (litFamily) R_SetTerrainScorchTextures(ctx.source, m);   // OLA M3: idem (R_TessBModel)
        R_SetupPassPerObjectArgs(ctx);
        R_SetupPassPerPrimArgs(ctx);
        if (litFamily) { GfxCmdBufContext none = { 0, 0 }; R_DrawBspDrawSurfsLit(pk.data(), ctx, none); }
        else R_DrawBspDrawSurfs(pk.data(), ctx.state);
        ctx.state->prim.primStats = 0;
        ++g_passes;
    }
    R_SetObjectIdentityPlacement(ctx.source);
    ++g_drawn;
}

// H5b: superficie XModel ya deformada en CPU (vertices en mundo, colocacion identidad). Mismo orden de estado que
// R_TessXModelSkinnedDrawSurfList (rb_tess.cpp) -> R_DrawXModelSkinnedUncached, sin la luz de modelo (lightgrid sin portar).
void (*g_h4DrawExtra)(unsigned char techType);
// OLA 15B: pase translucido del mundo (cristal de las ventanas, h12_barrier.cpp). Va tras los actores y ANTES del viewmodel: el viewmodel
// borra la profundidad (depth hack) y lo que se dibujara despues saldria encima de todo (los rombos oscuros del suelo eran cristales de
// ventanas de otras salas vistos a traves del suelo). H5W_DrawViewModel lo llama antes de borrar; si no hay viewmodel, RenderFrame.
void (*g_h4DrawTranslucent)(unsigned char techType);
static bool g_h4TransDone;
void H4_DrawTranslucentOnce(unsigned char techType) { if (g_h4DrawTranslucent && !g_h4TransDone) { g_h4TransDone = true; g_h4DrawTranslucent(techType); } }
static GfxImage *g_h4ModelLight;   // luz de modelo neutra (H4_NeutralModelLighting) o atlas del lightgrid (h5_light.cpp)
// OLA 6b (h5_light.cpp): atlas de luz de modelo del lightgrid ORIGINAL. g_h4ModelLightImage lo crea; antes de cada modelo el llamador
// pone sus coordenadas (CONST_SRC_CODE_BASE_LIGHTING_COORDS) y la tecnica lit segun su luz primaria (10 ninguna, 11 sol).
GfxImage *(*g_h4ModelLightImage)();
float g_h4ModelLightCoords[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
unsigned char g_h4ModelLitTech = 10;
void (*g_h4PreFrame)();   // antes de abrir el pase del frame (subidas de texturas sin partir el render pass)
int g_h4SkinnedDrawn, g_h4SkinnedSkipped, g_h4SkinnedTech[256];
void (*g_h4ModelProbe)(GfxCmdBufContext, const XSurface *, const GfxPackedVertex *);   // OLA 9: sonda de reflejo del objeto (h9_reflect.cpp)
// OLA M1: materiales de modelo que FALTAN en los datos (el DB los sustituye por el material por defecto, de techset '2d': dibujado
// en 3D sale como triangulos grises enormes, el shader 2D toma la posicion como coordenadas de pantalla). En Kino faltan los del
// uniforme de los zombis (mc/mtl_c_ger_honorguard_zombie_*: ninguna zona de los datos los trae, tampoco la common_zombie nueva).
// Se usan los materiales ORIGINALES del mismo uniforme que si estan (los de los trozos de desmembramiento char_ger_honorgd_body1_g_*,
// mismo personaje): tabla de sustitutos por nombre. Sin sustituto, la superficie no se dibuja (nunca el '2d' en 3D).
static Material *H4_FixModelMaterial(Material *m)
{
    static std::map<Material *, Material *> s_fix;
    auto it = s_fix.find(m); if (it != s_fix.end()) return it->second;
    Material *r = m;
    const MaterialTechniqueSet *ts = Material_GetTechniqueSet(m);
    if (ts && ts->name.get() && !strcmp(ts->name.get(), "2d") && !Material_GetTechnique(m, 10)) {
        static const struct { const char *from, *to; } kSubst[] = {
            { "mc/mtl_c_ger_honorguard_zombie_body1", "mc/mtl_char_ger_honorguard_body1_1" },
            { "mc/mtl_c_ger_honorguard_zombie_linen", "mc/mtl_char_ger_honorguard_body1_1" },
            { "mc/mtl_c_ger_honorguard_zombie_lthr", "mc/mtl_char_ger_honorguard_lthr1_1" },
            { "mc/mtl_c_ger_honorguard_zombie_metal", "mc/mtl_char_ger_honorguard_lthr1_1" },
        };
        r = nullptr; const char *to = nullptr;
        for (const auto &e : kSubst) if (!strcmp(m->info.name.get(), e.from)) { to = e.to; break; }
        if (to) { Material *c = DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, (char *)to, 1, -1).material; const MaterialTechniqueSet *cs = c ? Material_GetTechniqueSet(c) : nullptr; if (cs && strcmp(cs->name.get(), "2d")) r = c; }
        fprintf(stderr, "[H4] material de modelo que falta en los datos: '%s' -> %s\n", m->info.name.get(), r ? r->info.name.get() : "(no se dibuja)");
    }
    s_fix[m] = r; return r;
}
void H4_DrawSkinnedSurf(Material *m, XSurface *xsurf, GfxPackedVertex *verts, unsigned char techType)
{
    if (!(m = H4_FixModelMaterial(m))) return;
    GfxCmdBufContext ctx = gfxCmdBufContext;
    // La tecnica lit de los personajes muestrea TEXTURE_SRC_CODE_MODEL_LIGHTING (volumen de luz de modelo que rellena
    // r_model_lighting.cpp desde el lightgrid: sin portar). Mientras falte, se usa la primera tecnica del material que no lo
    // necesite: FAKELIGHT_NORMAL (121, la de r_fullbright/depuracion), FAKELIGHT_VIEW (122), UNLIT (4).
    const unsigned char cand[] = { techType == 10 ? g_h4ModelLitTech : techType, techType, 121, 122, 4 };
    const MaterialTechnique *tech = nullptr;
    for (unsigned char t : cand) {
        const MaterialTechnique *c = Material_GetTechnique(m, t); if (!c) continue;
        bool usesModelLighting = false;
        for (unsigned p = 0; p < c->passCount && !usesModelLighting; ++p) { const MaterialPass *ps = &c->passArray[p]; int n = ps->perPrimArgCount + ps->perObjArgCount + ps->stableArgCount;
            for (int i = 0; i < n; ++i) if (ps->args[i].type == 4 && ps->args[i].u.codeSampler == 3) { usesModelLighting = true; break; } }
        { static const char *td = getenv("H4_TECHDIAG"); static int tdn; if (td && strstr(m->info.name.get(), td) && tdn < 40) { ++tdn; fprintf(stderr, "[H4-TECH] %s pide %d cand %d '%s' passes %d modelLighting %d atlas %p\n", m->info.name.get(), (int)cand[0], (int)t, c->name.get(), (int)c->passCount, (int)usesModelLighting, (void *)g_h4ModelLight); } }   // OLA M1
        if (usesModelLighting && !g_h4ModelLight) continue;
        tech = c; techType = t; break;
    }
    { static const char *td = getenv("H4_TECHDIAG"); static std::map<std::string, int> seenT;
      if (td && strstr(m->info.name.get(), td) && seenT.insert({ m->info.name.get(), 1 }).second) { const MaterialTechniqueSet *ts = Material_GetTechniqueSet(m);
        fprintf(stderr, "[H4-TECH] %s techset '%s' flags %04x:", m->info.name.get(), ts ? ts->name.get() : "-", ts ? ts->techsetFlags : 0);
        for (int i = 0; ts && i < 130; ++i) if (ts->techniques[i].get()) fprintf(stderr, " %d", i); fprintf(stderr, "\n"); } }   // OLA M1
    if (!tech) { ++g_h4SkinnedSkipped; return; }
    ++g_h4SkinnedTech[techType];
    { static std::map<std::string, int> seenS;
      if (getenv("H4_ARGS") && seenS.insert({ std::string(tech->name.get()) + "|" + m->info.name.get(), 1 }).second) {   // OLA M1: por tecnica Y material
        const MaterialPass *ps = &tech->passArray[0]; int n = ps->perPrimArgCount + ps->perObjArgCount + ps->stableArgCount;
        fprintf(stderr, "[H4args-skin] %s tech=%s(%d) prim=%d obj=%d stable=%d:", m->info.name.get(), tech->name.get(), techType, ps->perPrimArgCount, ps->perObjArgCount, ps->stableArgCount);
        for (int i = 0; i < n; ++i) { const MaterialShaderArgument *a = &ps->args[i]; fprintf(stderr, " [t%d d%d i%d]", a->type, a->dest, (a->type == 3 || a->type == 5) ? a->u.codeConst.index : (int)a->u.codeSampler); }
        fprintf(stderr, "\n"); } }
    ctx.state->material = m; ctx.state->technique = tech; ctx.state->techType = techType;
    ctx.state->origMaterial = m; ctx.state->origTechType = techType;
    for (unsigned pass = 0; pass < tech->passCount; ++pass) {
        R_UpdateMaterialTime(ctx.source, 0.0f, 0.0f, 0.0f, 0.0f);
        R_SetupPass(ctx, pass);
        R_SetupPassCriticalPixelShaderArgs(ctx);
        R_SetObjectIdentityPlacement(ctx.source);
        R_ChangeDepthHackNearClip(ctx.source, 0);
        R_SetVertexDeclTypeModelSkinned(xsurf, ctx.state);
        R_TrackPrims(&ctx.state->prim, GFX_PRIM_STATS_XMODELSKINNED);
        if (g_h4ModelProbe) g_h4ModelProbe(ctx, xsurf, verts);   // OLA 9: R_SetReflectionProbe del objeto (h9_reflect.cpp)
        if (g_h4ModelLight) {   // R_SetModelLightingCoordsForSource: centro del volumen neutro (por objeto)
            float *c = ctx.source->input.consts[CONST_SRC_CODE_BASE_LIGHTING_COORDS]; memcpy(c, g_h4ModelLightCoords, 16); ++ctx.source->constVersions[CONST_SRC_CODE_BASE_LIGHTING_COORDS]; }
        R_SetupPassPerObjectArgs(ctx);
        R_SetupPassPerPrimArgs(ctx);
        R_DrawXModelSkinnedUncached(ctx, xsurf, verts);
        ctx.state->prim.primStats = 0;
        ++g_passes;
    }
    ++g_h4SkinnedDrawn;
}

static GfxCmdBufInput g_in;
// H5b: luz de modelo NEUTRA. Los personajes lit leen TEXTURE_SRC_CODE_MODEL_LIGHTING (atlas de volumen 256xHx4 A8R8G8B8 que
// rellena r_model_lighting.cpp desde el lightgrid del mapa por entidad: sin portar) en CONST_SRC_CODE_BASE_LIGHTING_COORDS.
// Hasta portar el lightgrid se crea un volumen 4x4x4 con un valor uniforme (H4_MODELLIGHT=0xAARRGGBB, por defecto 0x80808080)
// con el mismo formato e imagen de programa (Image_AllocProg(12): la de R_InitModelLightingImage).
static GfxImage *H4_NeutralModelLighting()
{
    if (g_h4ModelLight || !g_h4DrawExtra || getenv("H4_MODELLIGHT_OFF")) return g_h4ModelLight;
    if (g_h4ModelLightImage) { if ((g_h4ModelLight = g_h4ModelLightImage())) fprintf(stderr, "[H4] luz de modelo: atlas del lightgrid (h5_light)\n"); return g_h4ModelLight; }
    unsigned v = getenv("H4_MODELLIGHT") ? (unsigned)strtoul(getenv("H4_MODELLIGHT"), 0, 0) : 0x80808080u;
    GfxImage *img = Image_AllocProg(12, 4u, 1u);
    Image_SetupAndLoad(img, 4, 4, 4, 10, D3DFMT_A8R8G8B8);
    IDirect3DVolumeTexture9 *vt = img->texture.volmap.get();
    D3DLOCKED_BOX lb;
    if (vt && vt->LockBox(0, &lb, nullptr, 0) == 0) {
        for (int z = 0; z < 4; ++z) for (int y = 0; y < 4; ++y) { unsigned *row = (unsigned *)((char *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch); for (int x = 0; x < 4; ++x) row[x] = v; }
        vt->UnlockBox(0);
    }
    fprintf(stderr, "[H4] luz de modelo neutra 4x4x4 A8R8G8B8 = 0x%08X (lightgrid sin portar)\n", v);
    return g_h4ModelLight = img;
}
static void InitInput(GfxWorld *w)
{
    memcpy(&g_in, &gfxCmdBufInput, sizeof g_in);
    if (!g_in.data) g_in.data = backEndData;
    if (!g_in.data) { static GfxBackEndData *dummy = (GfxBackEndData *)calloc(1, sizeof(GfxBackEndData)); g_in.data = dummy; static bool said; if (!said) { said = true; fprintf(stderr, "[H4] backEndData nulo: usando uno vacio (se avisa una vez)\n"); } }
    for (unsigned c = 0x2F; c < 197; ++c) if (s_codeConstUpdateFreq[c] == MTL_UPDATE_RARELY) R_SetInputCodeConstant(&g_in, c, 0, 0, 0, 0);
    R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_FOG, 0, 0, 0, 0);
    R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_FOG2, 0, 0, 0, 0);
    R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_FOG_COLOR, 0.5f, 0.5f, 0.5f, 1);
    R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_MATERIAL_COLOR, 1, 1, 1, 1);
    R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_COLOR_TINT_BASE, 1, 1, 1, 1);
    R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_SKY_COLOR_MULTIPLIER, 1, 1, 1, 1);
    float expo = getenv("H4_EXPO") ? (float)atof(getenv("H4_EXPO")) : 0.0625f;
    R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_HDRCONTROL_0, expo, 0, 0, 0);
    R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_HDRCONTROL_1, 1, 0, 0, 0);
    R_SetInputCodeConstant(&g_in, 0xC2, 1, 1, 1, 1); R_SetInputCodeConstant(&g_in, 0xC3, 1, 1, 1, 1);
    R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_ZNEAR, 0.984375f * g_vp.zNear, 0, 0, 0);
    if (GfxImage *ml = H4_NeutralModelLighting()) {
        g_in.codeImages[TEXTURE_SRC_CODE_MODEL_LIGHTING] = ml;
        { extern float H5L_InvImageHeight() __attribute__((weak)); const float invH = H5L_InvImageHeight ? H5L_InvImageHeight() : 0.25f;   // OLA 7e: alto del atlas variable
          R_SetInputCodeConstant(&g_in, 0x35, 0.005859375f, 1.5f * invH, 0.375f, 0.0f); }   // R_SetModelLightingLookupScale (invImageHeight = 1/alto)
        // R_SetHeroLighting (r_scene.cpp) con valores neutros: escala de luz primaria de personaje 1/1 y matriz de color identidad
        // (saturacion 1, temperatura neutra, r_heroLightScale 1)
        // OLA 7g: R_SetCharacterPrimaryLightScaleInfo (r_scene.cpp): con r_heroLighting y r_primaryLightUseTweaks (vision file) la luz
        // primaria de personajes/armas se escala por r_primaryLightTweakDiffuse/SpecularStrength (pentagon: 1.72 / 1.58)
        float hd = 1, hs = 1;
        if (r_heroLighting && r_heroLighting->current.enabled && r_primaryLightUseTweaks && r_primaryLightUseTweaks->current.enabled && !(getenv("H7_HEROTWEAK") && !atoi(getenv("H7_HEROTWEAK"))))
            { hd = r_primaryLightTweakDiffuseStrength->current.value; hs = r_primaryLightTweakSpecularStrength->current.value; }
        R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_LIGHT_HERO_SCALE, hd, hs, 1, 1);
        { static bool once; if (!once) { once = true; fprintf(stderr, "[H7] escala luz primaria de personajes %.3f / %.3f\n", hd, hs); } }
        R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_HERO_LIGHTING_R, 1, 0, 0, 0);
        R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_HERO_LIGHTING_G, 0, 1, 0, 0);
        R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_HERO_LIGHTING_B, 0, 0, 1, 0);
    }
    if (w->sunLight) {
        const GfxLight *sun = w->sunLight.get();
        R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_SUN_POSITION, sun->dir[0], sun->dir[1], sun->dir[2], 0);
        R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_SUN_DIFFUSE, sun->diffuseColor[0], sun->diffuseColor[1], sun->diffuseColor[2], 1);
        R_SetInputCodeConstant(&g_in, CONST_SRC_CODE_SUN_SPECULAR, sun->specularColor[0], sun->specularColor[1], sun->specularColor[2], 1);
    }
    { extern void H7C_Fog(GfxCmdBufInput *, const float *) __attribute__((weak)); if (H7C_Fog) H7C_Fog(&g_in, g_vp.origin); }   // OLA 7c: niebla del original (R_SetFrameFog)
}

static float g_fovX7d = 90.0f;
// OLA 7d: solo el mundo (lotes del BSP, cull por frustum) con una tecnica dada, en el render target activo; para el prepase floatz
// OLA M3: lotes cuya tecnica LIT pide la escena resuelta tras el sol (MTL_TECHFLAG_NEEDS_RESOLVED_POST_SUN = 1; agua de Nacht). El original
// (RB_DrawScene, rb_draw3d.cpp) los deja fuera de LIT_PHASE_MAIN y del prepase de profundidad, hace R_ResolveDistortion (escena ->
// RESOLVED_POST_SUN, compartido con RESOLVED_SCENE) y los dibuja en LIT_PHASE_POST_RESOLVE. Five y Kino no tienen ninguno.
static bool NeedsPostSun(size_t b) { const MaterialTechnique *t = Material_GetTechnique(g_batches[b].m, 10); return t && (t->flags & 1); }
static void DrawWorldOnly(unsigned char techType, int W, int H)
{
    GfxSceneDef sd; memset(&sd, 0, sizeof sd);
    R_BeginView(gfxCmdBufContext.source, &sd, &g_vp);
    GfxViewport vpt = { 0, 0, W, H };
    R_SetViewportStruct(gfxCmdBufContext.source, &vpt); R_SetViewport(gfxCmdBufContext.state, &vpt);
    Frustum fr; const float tx = tanf(g_fovX7d * 0.5f * (float)M_PI / 180.0f), ty = tx * (float)H / (float)W;
    BspStatic_MakeFrustum(&fr, g_vp.origin, g_vp.axis, tx, ty); CullBatches(g_cullOn ? &fr : 0);
    for (size_t b = 0; b < g_batches.size(); ++b) if (g_prim[b][0] && !NeedsPostSun(b)) DrawBatch(b, techType);
}
static void RenderFrame(GfxWorld *w, const Cam &cam, int W, int H, float fovX, unsigned char techType, float fovOverrideUnused = 0)
{
    BuildView(cam, W, H, fovX); g_fovX7d = fovX;
    if (g_h4PreFrame) g_h4PreFrame();
    InitInput(w);
    R_InitCmdBufSourceState(&gfxCmdBufSourceState, &g_in, 1);
    R_InitLocalCmdBufState(gfxCmdBufContext.state);
    { extern void H9_PreFrame(int, int) __attribute__((weak)); if (H9_PreFrame) { g_cw = w; H9_PreFrame(W, H); } }   // OLA 9: mapas de sombra de focos y pantallas (h9_shadow.cpp)
    { extern void H7D_FloatZPrepass(void (*)(unsigned char, int, int), int, int) __attribute__((weak)); if (H7D_FloatZPrepass) { g_cw = w; H7D_FloatZPrepass(DrawWorldOnly, W, H); } }   // OLA 7d: prepase floatz (h5_fx.cpp)
    {   // OLA M3: floatz para los materiales del MUNDO que la leen (agua de Nacht, sw_waterflow_fresnel_w_sun): el original la deja en
        // codeImages[FLOATZ] tras el prepase (RB_InitCodeImages / R_DepthPrepass); sin ella R_SetupPass* da Com_Error "Code texture 19".
        // El RT del prepase (h5_fx.cpp); sin prepase (H7D_FLOATZ=0) blanco, y esos lotes se ven sin el fundido. Five y Kino no tienen materiales asi.
        GfxCmdBufSourceState *s = gfxCmdBufContext.source;
        if (!s->input.codeImages[TEXTURE_SRC_CODE_FLOATZ]) {
            const GfxImage *fz = gfxRenderTargets[R_RENDERTARGET_FLOAT_Z].image ? (const GfxImage *)gfxRenderTargets[R_RENDERTARGET_FLOAT_Z].image : rgp.whiteImage;
            if (fz) { s->input.codeImages[TEXTURE_SRC_CODE_FLOATZ] = fz; if (!s->input.codeImageSamplerStates[TEXTURE_SRC_CODE_FLOATZ]) s->input.codeImageSamplerStates[TEXTURE_SRC_CODE_FLOATZ] = 98; }
        }
    }
    R_SetRenderTargetSize(gfxCmdBufContext.source, R_RENDERTARGET_FRAME_BUFFER);
    R_SetRenderTarget(gfxCmdBufContext, R_RENDERTARGET_FRAME_BUFFER);
    GfxSceneDef sd; memset(&sd, 0, sizeof sd);
    R_BeginView(gfxCmdBufContext.source, &sd, &g_vp);
    GfxViewport vpt = { 0, 0, W, H };
    R_SetViewportStruct(gfxCmdBufContext.source, &vpt);
    float clr[4] = { 0.1f, 0.12f, 0.2f, 1 };
    R_ClearScreen(gfxCmdBufContext.state->prim.device, 3, clr, 1.0f, 0, 0);
    R_SetViewport(gfxCmdBufContext.state, &vpt);
    g_drawn = g_skipped = g_passes = 0;
    g_cw = w;
    { Frustum fr; float tx = tanf(fovX * 0.5f * (float)M_PI / 180.0f), ty = tx * (float)H / (float)W;
      BspStatic_MakeFrustum(&fr, g_vp.origin, g_vp.axis, tx, ty); CullBatches(g_cullOn ? &fr : 0); }
    { static const char *skm = getenv("F15_SKIPMAT");   // F15/OLA 15B (diagnostico): no dibujar los lotes cuyo material contenga alguna de estas subcadenas (separadas por comas)
      if (skm) for (size_t b = 0; b < g_batches.size(); ++b) { char buf[256]; snprintf(buf, sizeof buf, "%s", skm); for (char *t = strtok(buf, ","); t; t = strtok(0, ",")) if (strstr(g_batches[b].m->info.name.get(), t)) { g_prim[b][0] = 0; break; } }
      static int sk0 = -1, sk1 = -1; if (sk0 < 0) { sk0 = sk1 = 0; if (const char *e = getenv("H15_SKIPB")) sscanf(e, "%d,%d", &sk0, &sk1); }   // OLA 15B (diagnostico): lotes [a,b) fuera
      static const char *keep = getenv("H15_KEEP");   // ...salvo los de material con esta subcadena
      for (int b = sk0; b < sk1 && b < (int)g_batches.size(); ++b) if (!keep || !strstr(g_batches[b].m->info.name.get(), keep)) g_prim[b][0] = 0; }
    { static long mb = getenv("H4_MAXBATCH") ? atol(getenv("H4_MAXBATCH")) : 1L << 30, sb = getenv("H4_SKIPBATCH") ? atol(getenv("H4_SKIPBATCH")) : 0;   // diagnostico de memoria
      bool post = false;
      for (size_t b = (size_t)sb; b < g_batches.size() && (long)b < sb + mb; ++b) if (g_prim[b][0]) { if (NeedsPostSun(b)) post = true; else DrawBatch(b, techType); }
      GfxImage *rps = gfxRenderTargets[R_RENDERTARGET_RESOLVED_POST_SUN].image;   // OLA M3: LIT_PHASE_POST_RESOLVE (lo crea h7_gfx PostFxInit)
      if (post && rps && techType == 10) {
          if (tess.indexCount) RB_EndTessSurface();
          const GfxRenderTargetId keep = (GfxRenderTargetId)gfxCmdBufContext.state->renderTargetId;
          gfxCmdBufContext.state->renderTargetId = R_RENDERTARGET_SCENE;   // como H7_PostFX: R_Resolve espera SCENE (compartido con FRAME_BUFFER)
          R_Resolve(gfxCmdBufContext, rps);
          gfxCmdBufContext.state->renderTargetId = keep;
          R_SetCodeImageTexture(gfxCmdBufContext.source, TEXTURE_SRC_CODE_RESOLVED_POST_SUN, rps);
          for (size_t b = (size_t)sb; b < g_batches.size() && (long)b < sb + mb; ++b) if (g_prim[b][0] && NeedsPostSun(b)) DrawBatch(b, techType);
      } else if (post) { static bool said; if (!said) { said = true; std::string ms; int n = 0;
          for (size_t b = 0; b < g_batches.size(); ++b) if (NeedsPostSun(b)) { if (++n <= 8) { ms += ' '; ms += g_batches[b].m->info.name.get(); } }
          fprintf(stderr, "[H4] %d lotes con escena resuelta tras el sol (%s ): sin RESOLVED_POST_SUN todavia, se omiten este frame\n", n, ms.c_str()); } } }
    { extern void H7C_DrawWorldExtra(unsigned char) __attribute__((weak)); if (H7C_DrawWorldExtra) H7C_DrawWorldExtra(techType); }   // OLA 7c: brushmodels/smodels
    g_h4SkinnedDrawn = g_h4SkinnedSkipped = 0;
    static const int noExtra = getenv("H15_NOEXTRA") ? atoi(getenv("H15_NOEXTRA")) : 0;   // OLA 15B (diagnostico): 1 = sin cadena g_h4DrawExtra
    g_h4TransDone = false;
    if (g_h4DrawExtra && !noExtra) g_h4DrawExtra(techType);   // H5b: actores del servidor (h5_game.cpp)
    H4_DrawTranslucentOnce(techType);   // OLA 15B: si no lo dibujo ya el viewmodel
    { extern void H7_PostFX() __attribute__((weak)); if (H7_PostFX) H7_PostFX(); }   // OLA 7g: RB_BloomLDR original (bloom + curva + color del vision set)
}


// ---- API para el bucle de frames (harness y app)
static Cam g_cam; static GfxWorld *g_world; static int g_W, g_H; static unsigned char g_tech = 10; static float g_fov = 90.0f;
float g_h10FovX;   // OLA 10: FOV del menu de ajustes (H7C_FovX de cg_fov_default), en caliente
// ---- jugador: Pmove ORIGINAL del motor (pmove_h4.cpp -> bgame/bg_pmove.cpp PmoveSingle/PM_WalkMove/PM_StepSlideMove/Jump_*),
// contra el clipmap REAL de la zona (CM_BoxTrace). Aqui solo se arma el usercmd desde el stick y se coloca la camara en
// ps.origin + viewHeightCurrent. H4_FLY=1 devuelve la camara libre antigua.
extern void Sys_SetValue(int, void *);
#include <universal/dvar.h>
#include "pmove_h4.h"
const dvar_s *doublesided_raycasts;   // dvar de colision (en el motor: common.cpp, no compilado aqui); waterbrush_entity lo registra BG_RegisterDvars (bg_misc.cpp)
static TraceThreadInfo g_ti; static cbrush_t g_dbrush; static cmodel_t g_dmodel; static PhysGeomList *g_dgeoms;
// capa de entrada encima del usercmd (no toca el codigo original): sprint automatico = boton de sprint (bit 1) tras ~1.2 s de stick a tope
// postura = maquina de mando de consola de client_mp/cl_input_mp.cpp (IN_Stance_Down/Up + CL_StanceButtonUpdate + IN_UpDown + CL_ToggleStance
// + CL_AddCurrentStanceToCmd): toque = agacharse/levantarse; mantener cl_stanceHoldTime (300 ms) = tumbarse (o levantarse si ya estaba
// tumbado); mantener cl_dtpHoldTime (200 ms) corriendo = plancha (dive to prone); saltar agachado/tumbado sube un nivel de postura.
enum { ST_STAND, ST_CROUCH, ST_PRONE, ST_DTP };
struct PlayerIn { bool init = false; float holdT = 0, holdAng = 0; bool sprintBtn = false; double msAcc = 0;
                  int stance = ST_STAND, stPos = ST_STAND; bool stHeld = false, stBtn = false, jBtn = false, jKey = false, pBtn = false; double clk = 0, stTime = 0; };
// H5d: con el servidor ORIGINAL activo (h5_play.cpp) el usercmd armado aqui va al jugador del servidor y el estado (camara) sale
// de su playerState; pmove_h4 no se usa entonces (una sola fisica). Sin servidor (H4 solo, app) es nullptr y todo sigue igual.
bool (*g_h4PlayerSink)(const H4PmCmd &c, int msec, H4PmState *out);
static PlayerIn g_pl; static bool g_cmReady, g_fly; static H4PmState g_pst; static float g_spawnEye[3]; static bool g_haveSpawn;
static const int kPlayerMask = 0x2810011;                        // MASK_PLAYERSOLID (solo para colocar al jugador al aparecer)
static const float kMinsS[3] = {-15, -15, 0}, kMaxsS[3] = {15, 15, 70};
static bool CmInit()
{
    if (g_cmReady) return true;
    doublesided_raycasts = _Dvar_RegisterBool("doublesided_raycasts", 0, 0x80u, "turn on double sided ray casts");
    memset(&g_ti, 0, sizeof g_ti); memset(&g_dbrush, 0, sizeof g_dbrush); memset(&g_dmodel, 0, sizeof g_dmodel); g_dgeoms = nullptr;
    int n = cm.partitionCount > 0 ? cm.partitionCount : 1; int *cc = (int *)Ptr32_ArenaAlloc(4 * n, 64); memset(cc, 0, 4 * n);
    g_ti.checkcount.partitions = cc; g_ti.box_brush = &g_dbrush; g_ti.box_model = &g_dmodel; g_ti.geoms = &g_dgeoms;
    Sys_SetValue(3, &g_ti); g_cmReady = true; return true;
}
static void PTrace(trace_t *t, const float *a, const float *b)
{
    col_context_t ctx(kPlayerMask);
    CM_BoxTrace(t, a, b, kMinsS, kMaxsS, kPlayerMask, &ctx);
}
static const float *TNormal(const trace_t &t) { return (const float *)&t.normal.vec; }
static void PlayerSpawn(const float *eye)
{
    CmInit(); g_pl = PlayerIn(); g_pl.init = true;
    if (getenv("H4_CMDBG")) {
        float top[3] = {eye[0], eye[1], eye[2]}, bot[3] = {eye[0], eye[1], eye[2] - 800.0f}; const float z3[3] = {0, 0, 0}; unsigned masks[] = {0xFFFFFFFFu, 1u, 0x2810011u, 0x10000u, 0xFFFFu};
        for (unsigned m : masks) { trace_t t; col_context_t c(m); CM_BoxTrace(&t, top, bot, z3, z3, m, &c); fprintf(stderr, "[CMDBG] punto mask=%08x fraction=%.4f hitType=%d startsolid=%d\n", m, t.fraction, (int)t.hitType, (int)t.startsolid);
            trace_t t2; col_context_t c2(m); CM_BoxTrace(&t2, top, bot, kMinsS, kMaxsS, m, &c2); fprintf(stderr, "[CMDBG] caja  mask=%08x fraction=%.4f startsolid=%d\n", m, t2.fraction, (int)t2.startsolid); }
        fprintf(stderr, "[CMDBG] cm: numSubModels=%u cmodels=%p brushes=%p leafs=%p nodes=%p leafbrushNodes=%p planes=%p\n", cm.numSubModels, (void *)cm.cmodels.get(), (void *)cm.brushes.get(), (void *)cm.leafs.get(), (void *)cm.nodes.get(), (void *)cm.leafbrushNodes.get(), (void *)cm.planes.get());
    }
    float top[3] = {eye[0], eye[1], eye[2] + 60.0f}, bot[3] = {eye[0], eye[1], eye[2] - 600.0f};
    trace_t t; PTrace(&t, top, bot);
    float z = top[2] + (bot[2] - top[2]) * t.fraction;
    float feet[3] = {eye[0], eye[1], (t.fraction < 1.0f && !t.startsolid) ? z : eye[2] - 60.0f};
    H4Pm_Spawn(feet, g_cam.ang[1]);
    H4Pm_Get(&g_pst);
    fprintf(stderr, "[H4] jugador (Pmove original): ojo (%.1f,%.1f,%.1f) -> pies z=%.1f (fraction %.3f startsolid=%d allsolid=%d)\n", eye[0], eye[1], eye[2], feet[2], t.fraction, (int)t.startsolid, (int)t.allsolid);
}
static void PlayerMove(const H4Input &in, float dt)
{
    PlayerIn &p = g_pl;
    // sprint automatico: analogico casi a tope y en la misma direccion ~1.2 s -> boton de sprint pulsado (el motor decide si puede)
    float mag = sqrtf(in.fwd * in.fwd + in.strafe * in.strafe); if (mag > 1) mag = 1;
    float ang = mag > 0.05f ? atan2f(in.strafe, in.fwd) : 0;
    if (mag > 0.85f && in.fwd > 0.3f && (p.holdT == 0 || fabsf(ang - p.holdAng) < 0.45f)) { if (p.holdT == 0) p.holdAng = ang; p.holdT += dt; } else { p.holdT = 0; p.sprintBtn = false; }
    if (p.holdT > 1.2f) p.sprintBtn = true;
    // postura (ver PlayerIn): boton de postura = up < -0.5, salto = up > 0.5, in.prone = boton de tumbarse (IN_ToggleProne)
    p.clk += dt * 1000.0;
    const bool sprinting = g_pst.viewHeight >= 60.0f && (g_pst.pmFlags & 0x8000) != 0;   // CG_IsSprinting
    const bool stNow = in.up < -0.5f, jNow = in.up > 0.5f;
    if (stNow && !p.stBtn) {   // IN_Stance_Down
        p.stHeld = true; p.stTime = p.clk;
        if (!sprinting) { p.stPos = p.stance; if (p.stance != ST_CROUCH) p.stance = ST_CROUCH; }
    } else if (!stNow && p.stBtn) {   // IN_Stance_Up
        if (sprinting) { p.stance = ST_CROUCH; p.stHeld = false; }
        if (p.stHeld && p.stPos == ST_CROUCH) p.stance = ST_STAND;
        p.stHeld = false;
    }
    p.stBtn = stNow;
    if (p.stHeld && p.clk - p.stTime >= (sprinting ? 200 : 300)) {   // CL_StanceButtonUpdate (cl_dtpHoldTime / cl_stanceHoldTime)
        p.stance = sprinting ? ST_DTP : (p.stPos == ST_PRONE ? ST_STAND : ST_PRONE); p.stHeld = false;
    }
    if (jNow && !p.jBtn) {   // IN_UpDown: agachado -> de pie, tumbado -> agachado, de pie -> tecla de salto
        if (p.stance == ST_CROUCH) p.stance = ST_STAND; else if (p.stance == ST_PRONE) p.stance = ST_CROUCH; else p.jKey = true;
    }
    if (!jNow) p.jKey = false;
    p.jBtn = jNow;
    if (in.prone && !p.pBtn) {   // CL_ToggleStance(CL_STANCE_PRONE)
        int pref = sprinting ? ST_DTP : ST_PRONE; p.stance = pref != p.stance ? pref : ST_STAND;
    }
    p.pBtn = in.prone;
    H4PmCmd c; memset(&c, 0, sizeof c);
    c.fwd = in.fwd; c.strafe = in.strafe; c.yaw = g_cam.ang[1]; c.pitch = g_cam.ang[0];
    // CL_AddCurrentStanceToCmd
    c.jump = p.jKey; c.crouch = p.stance == ST_CROUCH; c.prone = p.stance == ST_PRONE || p.stance == ST_DTP; c.dive = p.stance == ST_DTP; c.sprint = p.sprintBtn || in.sprint;
    // usercmd de milisegundos enteros, como CL_CreateCmd (el resto se acumula al frame siguiente)
    p.msAcc += dt * 1000.0; int msec = (int)p.msAcc; p.msAcc -= msec; if (msec > 200) msec = 200;
    if (g_h4PlayerSink) { g_h4PlayerSink(c, msec, &g_pst); }
    else { H4Pm_Move(c, msec); H4Pm_Get(&g_pst); }
    if (g_pst.forcedStance >= 0) p.stance = g_pst.forcedStance;   // EV_STANCE_FORCE_* -> CL_SetStance (cg_event.cpp)
    if (g_pst.pmFlags & 0x400) p.stance = ST_STAND;                // cg_predict_mp.cpp: pm_flags 0x400 -> CL_SetStance(STAND)
    if (!g_h4PlayerSink && g_pst.origin[2] < -3000.0f) { fprintf(stderr, "[H4] jugador cayo al vacio: reaparece\n"); g_pl.init = false; }
}
void H4_Update(const H4Input &in, float dt)
{
    g_cam.ang[1] += in.dyaw; g_cam.ang[0] += in.dpitch;
    if (g_cam.ang[0] > 89.0f) g_cam.ang[0] = 89.0f; if (g_cam.ang[0] < -89.0f) g_cam.ang[0] = -89.0f;
    if (!g_fly && cm.numLeafs > 0) {
        if (g_h4PlayerSink) g_pl.init = true;   // H5d: el jugador lo hace aparecer el servidor (ClientSpawn)
        if (!g_pl.init) { if (!g_haveSpawn) { memcpy(g_spawnEye, g_cam.org, 12); g_haveSpawn = true; } PlayerSpawn(g_spawnEye); }   // al caer al vacio: vuelve al punto inicial
        PlayerMove(in, dt);
        g_cam.org[0] = g_pst.origin[0]; g_cam.org[1] = g_pst.origin[1]; g_cam.org[2] = g_pst.origin[2] + g_pst.viewHeight;
        g_cam.ang[0] = g_pst.viewangles[0]; g_cam.ang[1] = g_pst.viewangles[1];   // el motor limita el cabeceo (player_view_pitch_up/down)
        return;
    }
    float ax[3][3]; AnglesToAxis(g_cam.ang, ax);
    float sp = (getenv("H4_SPEED") ? (float)atof(getenv("H4_SPEED")) : 300.0f) * dt;   // pulgadas/s (carrera ~190)
    for (int i = 0; i < 3; ++i) g_cam.org[i] += (ax[0][i] * in.fwd + ax[1][i] * -in.strafe) * sp;
    g_cam.org[2] += in.up * sp;
}
void H4_GetCam(float *org, float *ang) { memcpy(org, g_cam.org, 12); memcpy(ang, g_cam.ang, 12); }

bool H4_Setup(int W, int H)
{
    GfxWorld *w = H4_FindWorld(); if (!w) return false;
    { extern void H7_LoadHighMips() __attribute__((weak)); if (H7_LoadHighMips) H7_LoadHighMips(); extern void H7_LightmapInfo(GfxWorld *) __attribute__((weak)); if (H7_LightmapInfo) H7_LightmapInfo(w); extern void H7_DumpRaw() __attribute__((weak)); if (H7_DumpRaw) H7_DumpRaw(); extern void H7_ApplyVision(GfxWorld *) __attribute__((weak)); if (H7_ApplyVision) H7_ApplyVision(w); extern void H7_FindConst() __attribute__((weak)); if (H7_FindConst) H7_FindConst(); }   // OLA 7g: niveles de streaming (h7_gfx.cpp)
    { extern void H7C_Setup(GfxWorld *) __attribute__((weak)); if (H7C_Setup) H7C_Setup(w); }   // OLA 7c (h7c_gfx.cpp)
    { extern void H7E_Setup(GfxWorld *) __attribute__((weak)); if (H7E_Setup) H7E_Setup(w); }   // OLA 7e: luz de la sala (h7e_light.cpp)
    { extern void H9_Setup(GfxWorld *) __attribute__((weak)); if (H9_Setup) H9_Setup(w); }   // OLA 9 (h9_shadow.cpp)
    R_BeginFrame(); R_BeginSharedCmdList(); R_AddCmdProjectionSet2D(); R_EndFrame(); R_IssueRenderCommands(3u);   // marco vacio previo: estado inicial del backend
    rgp.world = w; g_worldDraw = &w->draw; g_worldDpvs = &w->dpvs; g_worldDpvsPlanes = &w->dpvsPlanes;
    // OLA M3: tercer flujo de vertices del mundo (R_ReloadWorldVertexBuffers, r_bsp.cpp: R_VertexStream2_Init(vertexStream2DataSize)).
    // Las superficies con stream2ByteOffset >= 0 (suelo y paredes por capas de Nacht) lo usan; sin el, v9 omite el draw ("stream 2 sin
    // buffer") y no se ve el suelo. Buffer a cero = sin quemaduras de terreno. Five y Kino no tienen esas superficies.
    if (g_worldDraw->vertexStream2DataSize) { R_VertexStream2_Init(g_worldDraw->vertexStream2DataSize); fprintf(stderr, "[H4] flujo 2 de vertices del mundo: %u bytes\n", g_worldDraw->vertexStream2DataSize); }
    for (unsigned i = 0; i < g_worldDraw->reflectionProbeCount; ++i)
        g_worldDraw->reflectionProbeTextures[i].basemap = g_worldDraw->reflectionProbes[i].reflectionImage->texture.basemap;
    if (const char *e = getenv("H15_PROBE")) for (unsigned i = 0, k = (unsigned)atoi(e); i < g_worldDraw->reflectionProbeCount && k < g_worldDraw->reflectionProbeCount; ++i)   // OLA 15B (diagnostico): todas las sondas = la k
        g_worldDraw->reflectionProbeTextures[i].basemap = g_worldDraw->reflectionProbes[k].reflectionImage->texture.basemap;
    if (getenv("H15_PROBEFILL")) for (unsigned i = 0; i < g_worldDraw->reflectionProbeCount; ++i) {   // OLA 15B (diagnostico): cada cara de cada sonda de un color (+X rojo -X verde +Y azul -Y amarillo +Z blanco -Z negro)
        GfxImage *im = g_worldDraw->reflectionProbes[i].reflectionImage; if (!im || !im->texture.cubemap) continue;
        static const unsigned col[6] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFFFFFF, 0xFF000000 };
        struct TCubeV : IDirect3DCubeTexture9 { void *g; int id; UINT e, levels; D3DFORMAT fmt; };   // misma forma que TCube (trace.cpp): GetLevelDesc/GetLevelCount del cubo no estan implementados
        const D3DFORMAT f = static_cast<TCubeV *>(im->texture.cubemap.get())->fmt; const unsigned lv = static_cast<TCubeV *>(im->texture.cubemap.get())->levels;   // GetLevelCount del cubo no esta implementado: LockRect falla fuera de rango
        for (int fc = 0; fc < 6; ++fc) for (unsigned l = 0; l < lv; ++l) { D3DLOCKED_RECT lr; if (im->texture.cubemap->LockRect((D3DCUBEMAP_FACES)fc, l, &lr, nullptr, 0) != 0) continue;
            const unsigned W = std::max(1u, (unsigned)im->width >> l), v = col[fc]; const unsigned short c = (unsigned short)(((v >> 19) & 31) << 11 | ((v >> 10) & 63) << 5 | ((v >> 3) & 31));
            if (f == D3DFMT_DXT1 || f == D3DFMT_DXT5) { const unsigned bs = f == D3DFMT_DXT1 ? 8 : 16; unsigned char b[16] = {}; if (bs == 16) b[0] = b[1] = 255; unsigned char *cb = b + bs - 8; cb[0] = cb[2] = c & 255; cb[1] = cb[3] = c >> 8;
                for (unsigned by = 0; by < (W + 3) / 4; ++by) for (unsigned bx = 0; bx < (W + 3) / 4; ++bx) memcpy((char *)lr.pBits + by * lr.Pitch + bx * bs, b, bs); }
            else for (unsigned y = 0; y < W; ++y) for (unsigned x = 0; x < W; ++x) ((unsigned *)((char *)lr.pBits + y * lr.Pitch))[x] = v;
            im->texture.cubemap->UnlockRect((D3DCUBEMAP_FACES)fc, l); }
        if (i < 3) fprintf(stderr, "[H15] PROBEFILL sonda %u '%s' fmt %d %ux%ux%u mapType %u levelCount %u mips %u tipo %d streaming %u pixels %p\n", i, im->name.get(), (int)f, im->width, im->height, im->depth,
            im->mapType, im->levelCount, lv, (int)im->texture.basemap->GetType(), im->streaming, (void *)im->pixels.get());
    }
    for (int i = 0; i < g_worldDraw->lightmapCount; ++i) {
        g_worldDraw->lightmapPrimaryTextures[i].basemap = g_worldDraw->lightmaps[i].primary->texture.basemap;
        g_worldDraw->lightmapSecondaryTextures[i].basemap = g_worldDraw->lightmaps[i].secondary->texture.basemap;
        g_worldDraw->lightmapSecondaryTexturesB[i].basemap = g_worldDraw->lightmaps[i].secondaryB->texture.basemap;
    }
    BuildBatches(w);
    { const char *e = getenv("H4_CULL"); g_cullOn = !(e && !atoi(e)); int bad = 0;
      for (int i = 0; i < w->surfaceCount; ++i) { const GfxSurface *sf = &w->dpvs.surfaces[i]; for (int k = 0; k < 3; ++k) if (sf->bounds[0][k] > sf->bounds[1][k] + 1e-3f || fabsf(sf->bounds[0][k] - sf->tris.mins[k]) > 1.0f || fabsf(sf->bounds[1][k] - sf->tris.maxs[k]) > 1.0f) { ++bad; break; } }
      { int trisOut = 0, bndOut = 0, inv = 0; for (int i = 0; i < w->surfaceCount; ++i) { const GfxSurface *sf = &w->dpvs.surfaces[i]; bool a = false, b = false;
          for (int k = 0; k < 3; ++k) { if (sf->tris.mins[k] < sf->bounds[0][k] - 1.0f || sf->tris.maxs[k] > sf->bounds[1][k] + 1.0f) a = true; if (sf->bounds[0][k] < sf->tris.mins[k] - 1.0f || sf->bounds[1][k] > sf->tris.maxs[k] + 1.0f) b = true; if (sf->tris.mins[k] > sf->tris.maxs[k]) ++inv; }
          trisOut += a; bndOut += b; }
        fprintf(stderr, "[H4] bounds vs tris.mins/maxs: tris fuera de bounds=%d, bounds fuera de tris=%d, tris invertidos=%d\n", trisOut, bndOut, inv); }
      fprintf(stderr, "[H4] culling %s; surfaces con bounds distinto de tris.mins/maxs: %d de %d\n", g_cullOn ? "ON" : "OFF", bad, w->surfaceCount);
      BspStatic_Build(&w->draw); }
    g_world = w; g_W = W; g_H = H;
    memset(&g_cam, 0, sizeof g_cam);
    g_cam.org[0] = -498.1f; g_cam.org[1] = 2582.6f; g_cam.org[2] = 88.5f; g_cam.ang[1] = 188.2f;   // "initial_spawn_points" (script_struct) del mapa + altura de ojos
    if (const char *c = getenv("H4_CAM")) sscanf(c, "%f,%f,%f,%f,%f", &g_cam.org[0], &g_cam.org[1], &g_cam.org[2], &g_cam.ang[1], &g_cam.ang[0]);
    if (getenv("H4_TECH")) g_tech = (unsigned char)atoi(getenv("H4_TECH"));
    { extern float H7C_FovX(int, int) __attribute__((weak)); if (H7C_FovX) { float f = H7C_FovX(W, H); if (f > 0) g_fov = f; } }   // OLA 7c: fov del original (cg_fov 65 a 4:3, CG_CalcFov)
    if (getenv("H4_FOV")) g_fov = (float)atof(getenv("H4_FOV"));
    g_fly = getenv("H4_FLY") != nullptr;
    return true;
}
static int g_lastDraws;
#ifndef BO1_APP
// GFX (auditoria grafica): GFX_CAMS="x,y,z,yaw,pitch;..." y GFX_AT=frame -> en ese frame se dibuja cada camara GFX_REP veces (def. 60),
// se mide el tiempo medio de H4_Frame y se captura <P4_OUT>_gK.png. Misma posicion en cada prueba (antes/despues).
static void H4_Frame1();
extern float g_h5wViewOrg[3], g_h5wViewAng[3];
__attribute__((weak)) float g_h5wViewOrg[3], g_h5wViewAng[3];
static void GfxCams()
{
    static int frame = 0; static const char *cams = getenv("GFX_CAMS"); static int at = getenv("GFX_AT") ? atoi(getenv("GFX_AT")) : 60;
    if (!cams || frame++ != at) return;
    int rep = getenv("GFX_REP") ? atoi(getenv("GFX_REP")) : 60; Cam save = g_cam; float so[3], sa[3]; memcpy(so, g_h5wViewOrg, 12); memcpy(sa, g_h5wViewAng, 12);
    const char *p = cams; int k = 0;
    while (p && *p) {
        Cam c; memset(&c, 0, sizeof c);
        if (sscanf(p, "%f,%f,%f,%f,%f", &c.org[0], &c.org[1], &c.org[2], &c.ang[1], &c.ang[0]) == 5) {
            g_cam = c; memcpy(g_h5wViewOrg, c.org, 12); memcpy(g_h5wViewAng, c.ang, 12);
            for (int i = 0; i < 8; ++i) H4_Frame1();   // calentar (texturas perezosas, pipelines)
            timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
            for (int i = 0; i < rep; ++i) H4_Frame1();
            clock_gettime(CLOCK_MONOTONIC, &b);
            double ms = ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6) / rep;
            char tag[16]; snprintf(tag, sizeof tag, "_g%d", k); H4_Shot(tag);
            fprintf(stderr, "[GFX] cam %d (%.1f,%.1f,%.1f yaw %.1f pitch %.1f): %.3f ms/frame (media de %d) draws=%d lotes=%d\n", k, c.org[0], c.org[1], c.org[2], c.ang[1], c.ang[0], ms, rep, g_lastDraws, g_drawn);
            if (const char *px = getenv("H15_PIXEL")) {   // OLA 15B (diagnostico): rayo por los pixeles "x,y;x,y" de la captura -> triangulos del mundo que corta
                extern void H15_PickRay(const float *, const float *) __attribute__((weak));
                for (const char *q = px; q && *q; q = strchr(q, ';') ? strchr(q, ';') + 1 : nullptr) { float sx, sy; if (sscanf(q, "%f,%f", &sx, &sy) != 2 || !H15_PickRay) break;
                    const float tx = tanf(g_fov * 0.5f * (float)M_PI / 180.0f), ty = tx * (float)g_H / (float)g_W, u = (2 * sx / g_W - 1) * tx, v = (1 - 2 * sy / g_H) * ty;
                    float d[3]; for (int j = 0; j < 3; ++j) d[j] = g_vp.axis[0][j] - u * g_vp.axis[1][j] + v * g_vp.axis[2][j];   // axis: adelante, izquierda, arriba
                    fprintf(stderr, "[H15] pixel %.0f,%.0f:\n", sx, sy); H15_PickRay(g_vp.origin, d); } }
        }
        ++k; p = strchr(p, ';'); if (p) ++p;
    }
    g_cam = save; memcpy(g_h5wViewOrg, so, 12); memcpy(g_h5wViewAng, sa, 12);
}
void H4_Frame() { GfxCams(); H4_Frame1(); }
static void H4_Frame1()
#else
void H4_Frame()
#endif
{
    int d0 = g_ts.draws;
    if (g_h10FovX > 0) g_fov = g_h10FovX;
    RenderFrame(g_world, g_cam, g_W, g_H, g_fov, g_tech);
    g_lastDraws = g_ts.draws - d0;
    if (g_h4Draw2D)
    {   // H5f: HUD con la lista 2D del renderer (como CL_UpdateScreen: R_BeginFrame ... R_EndFrame; R_IssueRenderCommands presenta)
        // R_BeginFrame reordena rgp.sortedMaterials (Material_Sort, r_material_load_obj.cpp, no enlazado) si se registro un material
        // nuevo; H4 dibuja el mundo con su propio orden (RenderFrame) y la lista 2D no usa ese orden, asi que no hace falta aqui.
        rgp.needSortMaterials = 0;
        R_BeginFrame(); R_BeginSharedCmdList(); R_AddCmdProjectionSet2D();
        g_h4Draw2D(g_W, g_H);
        R_EndFrame(); R_IssueRenderCommands(3u);
        return;
    }
    dx.device->Present(0, 0, 0, 0);
}
void (*g_h4Draw2D)(int W, int H);
void H4_SetCam(const float *org, const float *ang) { memcpy(g_cam.org, org, 12); memcpy(g_cam.ang, ang, 12); }
int H4_LastDrawn() { return g_drawn; }
int H4_LastPasses() { return g_passes; }

// OLA 6c/7b (tambien en la app) (h5_fx.cpp): vista del frame en curso (BuildView ya hecho cuando corre g_h4PreFrame) para la camara de los FX
void H4_GetView(float *org, float (*axis)[3], float *tanHalfX, float *tanHalfY)
{
    memcpy(org, g_vp.origin, 12); memcpy(axis, g_vp.axis, 36);
    *tanHalfX = tanf(g_fov * 0.5f * (float)M_PI / 180.0f); *tanHalfY = *tanHalfX * (float)g_H / (float)g_W;
}
#ifndef BO1_APP
#include <time.h>
static double NowS() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void Shot(const char *tag)
{
#ifdef H4_G9
    if (!g9::Active()) return;
    const char *o = getenv("P4_OUT"); std::string path = o ? o : "h4.png"; if (tag && *tag) path.insert(path.rfind('.'), tag);
    int ww, hh; bool ok = g9::ReadbackPNG(path.c_str(), &ww, &hh);
    fprintf(stderr, "[H4] PNG %s %dx%d ok=%d (%s)\n", path.c_str(), ww, hh, ok, g9::Info());
#endif
}
void H4_Shot(const char *tag) { Shot(tag); }
// Harness: recorrido automatico (giro de 360 grados en el sitio + paseo hacia delante) de H4_FRAMES frames; capturas y tiempo medio
int H4_Run(int W, int H)
{
    if (!H4_Setup(W, H)) return 1;
    if (getenv("H4_PHYS")) {   // prueba de fisica con entrada escrita (60 Hz): mide velocidades, salto y alturas con el Pmove original
        H4Pm_PrintDvars();
        if (getenv("H4_PHYS_STEP")) {   // escalones: busca un bordillo de altura en [lo,hi] delante de un suelo libre y anda contra el (subida = escalon que acepta el motor)
            float lo = 4, hi = 18; sscanf(getenv("H4_PHYS_STEP"), "%f,%f", &lo, &hi);
            CmInit(); GfxWorld *w = g_world; int found = 0;
            for (float x = w->mins[0]; x < w->maxs[0] && found < 3; x += 64) for (float y = w->mins[1]; y < w->maxs[1] && found < 3; y += 64) for (float z0 = w->mins[2] + 100; z0 < w->maxs[2] && found < 3; z0 += 400) {
                float a[3] = {x, y, z0 + 400}, b[3] = {x, y, z0}; trace_t t; PTrace(&t, a, b); if (t.startsolid || t.fraction >= 1.0f || TNormal(t)[2] < 0.95f) continue;
                float f[3] = {x, y, a[2] + (b[2] - a[2]) * t.fraction + 0.25f}; trace_t tc; PTrace(&tc, f, f); if (tc.startsolid) continue;
                for (int d = 0; d < 8 && found < 3; ++d) { float yaw = d * 45.0f, cx = cosf(yaw * (float)M_PI / 180), sy = sinf(yaw * (float)M_PI / 180);
                    float s1[3] = {f[0], f[1], f[2] + 1}, e1[3] = {f[0] + cx * 40, f[1] + sy * 40, f[2] + 1}; trace_t t1; PTrace(&t1, s1, e1); if (t1.fraction >= 1.0f || t1.fraction < 0.05f || TNormal(t1)[2] > 0.3f) continue;
                    float s2[3] = {f[0], f[1], f[2] + hi + 2}, e2[3] = {f[0] + cx * 64, f[1] + sy * 64, f[2] + hi + 2}; trace_t t2; PTrace(&t2, s2, e2); if (t2.fraction < 1.0f || t2.startsolid) continue;
                    float d2[3] = {e2[0], e2[1], f[2]}; trace_t t3; PTrace(&t3, e2, d2); if (t3.fraction >= 1.0f || TNormal(t3)[2] < 0.95f) continue;
                    float h = e2[2] + (d2[2] - e2[2]) * t3.fraction - f[2]; if (h < lo || h > hi) continue;
                    ++found; H4Pm_Spawn(f, yaw); g_pl.init = true; g_cam.ang[1] = yaw; H4Pm_Get(&g_pst); float z0f = g_pst.origin[2], zm = z0f; float p0[2] = {f[0], f[1]};
                    for (int k = 0; k < 60; ++k) { H4Input in; memset(&in, 0, sizeof in); in.fwd = 1; H4_Update(in, 1.0f / 60); if (g_pst.origin[2] > zm) zm = g_pst.origin[2]; }
                    float dx = g_pst.origin[0] - p0[0], dy = g_pst.origin[1] - p0[1];
                    fprintf(stderr, "[PHYSSTEP] bordillo h=%.1f en (%.0f,%.0f,%.1f) yaw %.0f: subida=%.1f avance=%.1f -> %s\n", h, f[0], f[1], f[2], yaw, zm - z0f, sqrtf(dx * dx + dy * dy), zm - z0f > h - 1 ? "SUBE" : "NO SUBE");
                } }
            if (!found) fprintf(stderr, "[PHYSSTEP] ningun bordillo en [%.0f,%.0f]\n", lo, hi);
            return 0;
        }
        if (getenv("H4_PHYS_FIND")) {   // busca un suelo despejado: rejilla sobre el mundo, suelo por traza hacia abajo y carrera libre de la caja del jugador
            CmInit(); float best = 0, bp[3] = {0, 0, 0}, by = 0; GfxWorld *w = g_world;
            for (float x = w->mins[0]; x < w->maxs[0]; x += 128) for (float y = w->mins[1]; y < w->maxs[1]; y += 128) for (float z0 = w->mins[2] + 100; z0 < w->maxs[2]; z0 += 400) {
                float a[3] = {x, y, z0 + 400}, b[3] = {x, y, z0}; trace_t t; PTrace(&t, a, b); if (t.startsolid || t.allsolid || t.fraction >= 1.0f || TNormal(t)[2] < 0.9f) continue;
                float f[3] = {x, y, a[2] + (b[2] - a[2]) * t.fraction + 1.0f}; trace_t tc; PTrace(&tc, f, f); if (tc.startsolid) continue;
                for (int d = 0; d < 4; ++d) { float yaw = d * 90.0f, e[3] = {f[0] + cosf(yaw * (float)M_PI / 180) * 1500, f[1] + sinf(yaw * (float)M_PI / 180) * 1500, f[2] + 18}; float f2[3] = {f[0], f[1], f[2] + 18}; trace_t th; PTrace(&th, f2, e);
                    float run = th.fraction * 1500, ok = 0; for (float r = 32; r < run; r += 32) { float q[3] = {f2[0] + (e[0] - f2[0]) * r / 1500, f2[1] + (e[1] - f2[1]) * r / 1500, f2[2]}, q2[3] = {q[0], q[1], q[2] - 40}; trace_t td; PTrace(&td, q, q2);
                        if (td.fraction >= 1.0f || TNormal(td)[2] < 0.9f || fabsf(q[2] - 18 + (q2[2] - q[2]) * td.fraction + 18 - f[2]) > 2.0f) break; ok = r; }
                    float up[3] = {f[0], f[1], f[2] + 400}; trace_t tu; PTrace(&tu, f, up); if (tu.fraction >= 1.0f) continue;   // con techo: dentro del mapa
                    if (ok > best) { best = ok; memcpy(bp, f, 12); by = yaw; } } }
            fprintf(stderr, "[PHYSFIND] mejor carrera libre %.0f en (%.1f,%.1f,%.1f) yaw %.0f -> H4_CAM=%.1f,%.1f,%.1f,%.0f,0\n", best, bp[0], bp[1], bp[2], by, bp[0], bp[1], bp[2] + 60, by);
            return 0;
        }
        // tp = volver al punto inicial antes del tramo (conservando la postura); yaw = giro respecto a la direccion despejada (H4_CAM)
        // up se aplica en los frames [u0,u1) (u1 = -1: todo el tramo). Postura con el mando de consola: toque / mantener 300 ms / 200 ms corriendo
        struct Seg { const char *n; int frames; float fwd, strafe, up; bool prone, autoSprint, tp; float yaw; int u0, u1; } segs[] = {
            {"reposo", 60, 0, 0, 0, false, false, false, 0, 0, -1}, {"andar", 150, 1.0f, 0, 0, false, false, true, 0, 0, -1}, {"parar", 60, 0, 0, 0, false, false, false, 0, 0, -1},
            {"correr", 330, 1.0f, 0, 0, false, true, true, 0, 0, -1}, {"parar2", 60, 0, 0, 0, false, false, false, 0, 0, -1},
            {"saltar", 70, 0, 0, 1.0f, false, false, false, 0, 0, 3}, {"quieto", 30, 0, 0, 0, false, false, false, 0, 0, -1},
            {"agachar(toque)", 40, 0, 0, -1.0f, false, false, false, 0, 0, 6}, {"andar_agachado", 150, 1.0f, 0, 0, false, false, true, 0, 0, -1},
            {"levantar(toque)", 40, 0, 0, -1.0f, false, false, false, 0, 0, 6},
            {"tumbar(mant.)", 90, 0, 0, -1.0f, false, false, false, 0, 0, 40}, {"andar_tumbado", 180, 1.0f, 0, 0, false, false, true, 0, 0, -1},
            {"levantar(mant.)", 90, 0, 0, -1.0f, false, false, false, 0, 0, 40},
            {"lateral", 120, 0, 1.0f, 0, false, false, true, 90, 0, -1}, {"atras", 120, -1.0f, 0, 0, false, false, true, 180, 0, -1},
            {"salto_corriendo", 80, 1.0f, 0, 1.0f, false, false, true, 0, 0, 3},
            {"plancha(dtp)", 200, 1.0f, 0, -1.0f, false, true, true, 0, 110, 130}, {"levantar3", 90, 0, 0, -1.0f, false, false, false, 0, 0, 40},
            {"caida_128", 90, 0, 0, 0, false, false, false, 0, 0, -1} };
        int f = 0; float zGround = 0;
        for (auto &sg : segs) {
            static float baseFeet[3], baseYaw; if (!strcmp(sg.n, "reposo")) { baseYaw = g_cam.ang[1]; }
            if (!strcmp(sg.n, "andar")) memcpy(baseFeet, g_pst.origin, 12);
            if (sg.tp) { H4Pm_Teleport(baseFeet); g_cam.ang[1] = baseYaw + sg.yaw; H4Pm_Get(&g_pst); }
            if (!strcmp(sg.n, "caida_128")) { float hi[3] = {baseFeet[0], baseFeet[1], baseFeet[2] + 128}; H4Pm_Teleport(hi); H4Pm_Get(&g_pst); }   // caida libre de 128 (bg_fallDamageMinHeight)
            float vmax = 0, vsum = 0, zmax = -1e9f, zmin = 1e9f; int nsum = 0; float z0 = g_pst.origin[2]; float p0[2] = {g_pst.origin[0], g_pst.origin[1]}; int tStop = -1; bool sprinted = false; int sprintFrames = 0;
            for (int k = 0; k < sg.frames; ++k, ++f) {
                H4Input in; memset(&in, 0, sizeof in); in.fwd = sg.fwd; in.strafe = sg.strafe; in.prone = sg.prone;
                in.up = (k >= sg.u0 && (sg.u1 < 0 || k < sg.u1)) ? sg.up : 0;
                if (!sg.autoSprint) g_pl.holdT = 0;
                H4_Update(in, 1.0f / 60);
                float v = sqrtf(g_pst.velocity[0] * g_pst.velocity[0] + g_pst.velocity[1] * g_pst.velocity[1]);
                if (v > vmax) vmax = v; if (k >= sg.frames - 30) { vsum += v; ++nsum; }
                if (g_pst.origin[2] > zmax) zmax = g_pst.origin[2]; if (g_pst.origin[2] < zmin) zmin = g_pst.origin[2];
                if (tStop < 0 && v < 0.5f && sg.fwd == 0 && sg.strafe == 0) tStop = k;
                if (g_pst.sprinting) { sprinted = true; ++sprintFrames; }
                if (k % 30 == 29 || k == 0) fprintf(stderr, "[PHYS] %-16s f=%4d pos=(%8.1f,%8.1f,%7.1f) vel=(%6.1f,%6.1f,%6.1f) |v|=%6.1f suelo=%d agach=%d tumb=%d sprint=%d ojo=%.1f\n", sg.n, f, g_pst.origin[0], g_pst.origin[1], g_pst.origin[2], g_pst.velocity[0], g_pst.velocity[1], g_pst.velocity[2], v, (int)g_pst.onGround, (int)g_pst.crouched, (int)g_pst.prone, (int)g_pst.sprinting, g_pst.viewHeight);
            }
            if (!strcmp(sg.n, "reposo")) zGround = g_pst.origin[2];
            float dx = g_pst.origin[0] - p0[0], dy = g_pst.origin[1] - p0[1];
            fprintf(stderr, "[PHYSRES] %-15s |v|max=%6.1f |v|final(30f)=%6.1f dist=%6.1f z0=%.2f zmax-z0=%.2f zmin-z0=%.2f parada_en=%d f sprint=%d (%d f) ojo=%.1f\n", sg.n, vmax, nsum ? vsum / nsum : 0, sqrtf(dx * dx + dy * dy), z0, zmax - z0, zmin - z0, tStop, (int)sprinted, sprintFrames, g_pst.viewHeight);
        }
        (void)zGround;
        return 0;
    }
    int N = getenv("H4_FRAMES") ? atoi(getenv("H4_FRAMES")) : 1; int shots = getenv("H4_SHOTS") ? atoi(getenv("H4_SHOTS")) : 0;
    double t0 = NowS(), tlast = t0, worst = 0; double sumFrame = 0;
    for (int f = 0; f < N; ++f) {
        H4Input in; memset(&in, 0, sizeof in);
        if (N > 1) { if (f < N / 2) in.dyaw = 360.0f / (N / 2); else in.fwd = 0.6f; }
        H4_Update(in, 1.0f / 60);
        double a = NowS(); H4_Frame(); double b = NowS();
        sumFrame += b - a; if (b - a > worst) worst = b - a;
        if (shots && N > 1 && f % std::max(1, N / shots) == 0) { char t[16]; snprintf(t, sizeof t, "_f%03d", f); Shot(t); }
        (void)tlast;
    }
    double dt = NowS() - t0;
    fprintf(stderr, "[H4] frame: lotes dibujados=%d pasadas=%d\n", g_drawn, g_passes);
    if (g_cullCheck) fprintf(stderr, "[H4] cullcheck (ultimo frame): visibles por matriz real=%d ocultas=%d | FALSOS DESCARTES (mio oculta, matriz ve)=%d | mio ve y la matriz no=%d (conservador)\n", g_ccClipVisible, g_ccClipHidden, g_ccFalseCull, g_ccMineVisClipHidden);
    fprintf(stderr, "[H4] cull: surfaces visibles=%d/%d lotes=%d/%zu draws(DrawIndexed)/frame=%d draws por IB estatico=%ld (%s)\n", g_surfVis, g_surfTot, g_batVis, g_batches.size(), g_lastDraws, g_sDraws, g_cullOn ? "cull ON" : "cull OFF");
    fprintf(stderr, "[H4] %d frames en %.2f s: %.1f fps, %.2f ms/frame (peor %.1f ms)\n", N, dt, N / dt, 1000 * dt / N, worst * 1000);
    Shot(shots ? "_fin" : "");
    return 0;
}
#endif
