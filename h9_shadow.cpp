// OLA 9: sombras de focos (spot shadow maps) del ORIGINAL en el port H4 (Pentagon: 91 focos primarios, 76 con canUseShadowMap).
//
// Lo que hace el original en PC (decomp): con mapas de sombra por hardware (gfxMetrics.shadowmapFormatPrimary = D24S8, color R5G6B5,
// shadowmapBuildTechType 2 = BUILD_SHADOWMAP_DEPTH, sampler 98) elige cada frame hasta sm_maxLights (4) focos con
// R_ShadowedSpotLightScore (r_primarylights.cpp) y una historia con fundido (sm_spotShadowFadeTime), dibuja en el render target
// SHADOWMAP_SPOT 512x2048 (4 casillas de 512 en vertical, R_InitShadowmapRenderTarget) la geometria de shadowGeom[luz] (superficies del
// mundo + smodels) y las entidades, con la vista de la luz (R_SetViewParmsForLight) y CONST_SRC_CODE_SHADOWMAP_POLYGON_OFFSET; y en el
// pase principal R_SetShadowableLight pone la tecnica LIT_SPOT_SHADOW (14), la imagen de codigo 7, la matriz R_GetSpotShadowLookupMatrix,
// SPOT_SHADOWMAP_PIXEL_ADJUST y el fundido (R_SetLightProperties con LIGHT_HAS_SHADOWMAP).
// El pixel shader 14 (desensamblado, H9_DUMPPS) hace 4 texldp sobre el sampler de la imagen 7 (desplazados con PIXEL_ADJUST), media 0.25
// y lrp(fundido, sombra, lightmap.y): con fundido 1 la sombra del lightmap la sustituye la del mapa.
//
// Aqui se reproduce igual (seleccion, historia, matrices y constantes copiadas del decomp) con dos diferencias de backend:
//  - MojoShader no tiene samplers de comparacion (PCF por hardware): cada "texldp rD, rS, sN" del sampler de sombra se reescribe en el
//    bytecode D3D9 como rcp/mul (division perspectiva), texld de la profundidad, add (prof - z) y cmp -> 1 iluminado / 0 en sombra
//    (comparacion LESSEQUAL como el hardware D3D9). La media de 4 muestras del shader original da el mismo suavizado de borde.
//  - Casters: superficies del mundo de shadowGeom (como R_AddAllBspDrawSurfacesSpotShadow) y, si estan, los smodels de shadowGeom
//    (h7c, H7C_DrawSModelList) y los actores (h5_game, H5V_DrawActorsShadow).
// Variables: H9_SHADOW=n (0 apagado; n focos, 1..4; sin ella mandan sm_enable/sm_maxLights del perfil), H9_SHADOW_RES=256|512 (casilla),
//   H9_SHADOW_CASTERS=bits (1 mundo, 2 smodels, 4 actores; def. 7), H9_SHADOW_FADE=x (fuerza el fundido, prueba), H9_DIAG/H9_DUMPPS
//   (diagnostico), H9_SHADOW_DBG=bits (1 sin tecnica 14, 2 sin pase, 4 sin clear, 8 solo cambio de target, 2048 mapa a 0, 4096 vista
//   de la camara: pruebas de la OLA 9).
#include <database/db_registry.h>
#include <universal/ptr32.h>
#include <universal/com_math.h>
#include <universal/dvar.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_state.h>
#include <gfx_d3d/r_state_utils.h>
#include <gfx_d3d/r_rendertarget.h>
#include <gfx_d3d/r_draw_shadowablelight.h>
#include <gfx_d3d/r_foliage.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/rb_draw3d.h>
#include <gfx_d3d/rb_backend.h>
#include <gfx_d3d/rb_shade.h>
#include <d3d9.h>
#include <qcommon/com_bsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include "world_h4.h"

GfxLight *H5L_Lights() __attribute__((weak));

static void H9_Diag(GfxWorld *w)
{
    GfxLight *L = H5L_Lights ? H5L_Lights() : nullptr; const unsigned n = w->primaryLightCount;
    int spot = 0, spotSm = 0, omni = 0, omniSm = 0;
    for (unsigned i = 1; L && i < n; ++i) {
        const GfxLight &l = L[i];
        if (l.type == GFX_LIGHT_TYPE_SPOT) { ++spot; spotSm += l.canUseShadowMap; } else if (l.type == GFX_LIGHT_TYPE_OMNI) { ++omni; omniSm += l.canUseShadowMap; }
        if (getenv("H9_DIAG") && atoi(getenv("H9_DIAG")) > 1)
            fprintf(stderr, "[H9] luz %u tipo %d sm %d radio %.0f org (%.0f,%.0f,%.0f) cos %.3f geom surf %u smodel %u\n", i, l.type, l.canUseShadowMap, l.radius, l.origin[0], l.origin[1], l.origin[2],
                l.cosHalfFovOuter, w->shadowGeom ? w->shadowGeom[i].surfaceCount : 0, w->shadowGeom ? w->shadowGeom[i].smodelCount : 0);
    }
    fprintf(stderr, "[H9] luces primarias %u: foco %d (sombra %d) omni %d (sombra %d); shadowGeom %p\n", n, spot, spotSm, omni, omniSm, (void *)w->shadowGeom.get());
    int t2 = 0, t13 = 0, t14 = 0, t16 = 0, tot = 0, dumped = 0; std::set<const void *> seen;
    const char *dir = getenv("H9_DUMPPS");
    for (unsigned i = 0; i < (unsigned)w->materialMemoryCount; ++i) {
        Material *m = w->materialMemory[i].material.get(); if (!m) continue; ++tot;
        t2 += !!Material_GetTechnique(m, 2); t13 += !!Material_GetTechnique(m, 13); t16 += !!Material_GetTechnique(m, 16);
        if (dir && i < 400) if (const MaterialTechnique *t2p = Material_GetTechnique(m, 2)) { static int d2 = 0; const MaterialPass *ps = &t2p->passArray[0];
            if (d2 < 2 && ps->vertexShader.get()) { char fn[512]; snprintf(fn, sizeof fn, "%s/vs2_%d.bin", dir, d2);
                if (FILE *f = fopen(fn, "wb")) { fwrite(ps->vertexShader.get()->prog.loadDef.program.get(), 4, ps->vertexShader.get()->prog.loadDef.programSize, f); fclose(f); }
                if (ps->pixelShader.get()) { snprintf(fn, sizeof fn, "%s/ps2_%d.bin", dir, d2); if (FILE *f = fopen(fn, "wb")) { fwrite(ps->pixelShader.get()->prog.loadDef.program.get(), 4, ps->pixelShader.get()->prog.loadDef.programSize, f); fclose(f); } }
                fprintf(stderr, "[H9] tech2_%d '%s' '%s' stateBits %08x %08x args:", d2, m->info.name.get(), t2p->name.get(), m->stateBitsTable.get() ? m->stateBitsTable.get()[m->stateBitsEntry[2]].loadBits[0] : 0, m->stateBitsTable.get() ? m->stateBitsTable.get()[m->stateBitsEntry[2]].loadBits[1] : 0);
                const int na = ps->perPrimArgCount + ps->perObjArgCount + ps->stableArgCount;
                for (int q = 0; q < na; ++q) { const MaterialShaderArgument *g = &ps->args[q]; fprintf(stderr, " [t%d d%d i%d]", g->type, g->dest, (g->type == 3 || g->type == 5) ? g->u.codeConst.index : (int)g->u.codeSampler); }
                fprintf(stderr, "\n"); ++d2; } }
        const MaterialTechnique *t = Material_GetTechnique(m, 14); if (!t) continue; ++t14;
        if (!dir || dumped >= 6) continue;
        const MaterialPass *ps = &t->passArray[0]; const MaterialPixelShader *px = ps->pixelShader.get();
        if (!px || !seen.insert(px).second) continue;
        char fn[512]; snprintf(fn, sizeof fn, "%s/ps14_%d.bin", dir, dumped);
        if (FILE *f = fopen(fn, "wb")) { fwrite(px->prog.loadDef.program.get(), 4, px->prog.loadDef.programSize, f); fclose(f); }
        fprintf(stderr, "[H9] ps14_%d '%s' tech '%s' ps '%s' %u palabras args:", dumped, m->info.name.get(), t->name.get(), px->name.get(), px->prog.loadDef.programSize);
        const int na = ps->perPrimArgCount + ps->perObjArgCount + ps->stableArgCount;
        for (int a = 0; a < na; ++a) { const MaterialShaderArgument *g = &ps->args[a]; fprintf(stderr, " [t%d d%d i%d]", g->type, g->dest, (g->type == 3 || g->type == 5) ? g->u.codeConst.index : (int)g->u.codeSampler); }
        fprintf(stderr, "\n"); ++dumped;
    }
    fprintf(stderr, "[H9] materiales del mundo %d: build_shadowmap_depth %d, lit_spot %d, lit_spot_shadow %d, lit_omni_shadow %d\n", tot, t2, t13, t14, t16);
}


// ---------------------------------------------------------------------------------------------------------------------------------
const GfxViewParms *H4_ViewParms();                      // world_h4.cpp
void H4_LightsUsed(unsigned char *used, unsigned n);    // world_h4.cpp
extern unsigned char (*g_h4WorldPrimaryTech)(unsigned char);
void H4_DrawSurfsPlaced(Material *m, const unsigned short *surfs, int n, unsigned char prim, const GfxScaledPlacement *pl, unsigned char techType);
void H7C_DrawSModelList(const unsigned short *idx, int n, unsigned char techType) __attribute__((weak));   // h7c_gfx.cpp
void H5V_DrawActorsShadow(const float *org, float radius, unsigned char techType) __attribute__((weak));   // h5_game.cpp
// reloj del render (como sceneDef.time del cliente): level.time del servidor va a 20 Hz y se repetiria en varios frames
static int NowGameMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
namespace {
GfxWorld *s_w;
int s_maxLights = 0, s_tile = 512, s_casters = 7, s_state = 0;   // s_state: 0 sin iniciar, 1 activo, -1 apagado
float s_forceFade = -1.0f;
GfxImage *s_img;
unsigned char (*s_prevTech)(unsigned char);
struct Hist { unsigned light; float fade; bool fadingOut; };
std::vector<Hist> s_hist; int s_lastTime = -1;
int s_tileOf[256]; float s_fadeOf[256];
GfxViewParms s_vp[4]; GfxMatrix s_lookup[4]; unsigned s_tileLight[4]; int s_tiles;
const float kPixelAdjust[4] = { 0.00048828125f, 0.00024414062f, 0.0009765625f, -0.00012207031f };   // R_AddSpotShadowsForLight (512)
float s_pixelAdjust[4];
double s_msPass, s_msAcc; int s_frames, s_patched, s_patchFail;
double NowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

// ---- reescritura del pixel shader (bytecode D3D9 ps_3_0) ----
inline unsigned RegType(unsigned p) { return ((p >> 28) & 7) | ((p >> 8) & 0x18); }
inline unsigned SrcT(unsigned r, unsigned sw, unsigned mod = 0) { return 0x80000000u | r | (sw << 16) | (mod << 24); }
inline unsigned DstT(unsigned r, unsigned mask) { return 0x80000000u | r | (mask << 16); }
inline unsigned Ins(unsigned op, unsigned len, unsigned spec = 0) { return op | (spec << 16) | (len << 24); }
// Devuelve el bytecode nuevo (vacio si no hay nada que cambiar o no se puede)
std::vector<unsigned> PatchShadowSampler(const unsigned *in, unsigned n, unsigned sampler, int *count)
{
    std::vector<unsigned> out; *count = 0;
    if (n < 2 || (in[0] & 0xFFFF0000u) != 0xFFFF0000u || ((in[0] >> 8) & 0xFF) != 3) return out;   // solo ps_3_0
    int maxTemp = -1; bool c223 = false;
    for (unsigned i = 1; i < n && in[i] != 0x0000FFFFu;) {   // pasada 1: temporales y constantes usados
        const unsigned t = in[i], op = t & 0xFFFF;
        if (op == 0xFFFE) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
        const unsigned len = (t >> 24) & 0xF;
        unsigned first = 1, last = len;
        if (op == 0x51 || op == 0x30 || op == 0x2F) last = 1; else if (op == 0x1F) first = 2;
        for (unsigned k = first; k <= last && i + k < n; ++k) { const unsigned p = in[i + k]; if (!(p & 0x80000000u)) continue;
            if (RegType(p) == 0) maxTemp = std::max(maxTemp, (int)(p & 0x7FF)); if (RegType(p) == 2 && (p & 0x7FF) == 223) c223 = true; }
        i += 1 + len;
    }
    if (maxTemp + 2 >= 32 || c223) return out;
    const unsigned T1 = maxTemp + 1, T2 = maxTemp + 2, K = 223;
    out.push_back(in[0]);
    out.push_back(Ins(0x51, 5)); out.push_back(0x80000000u | 0x20000000u | K | (0xFu << 16));   // def c223, 1, 0, 0, 0
    const float kv[4] = { 1, 0, 0, 0 }; for (float f : kv) { unsigned u; memcpy(&u, &f, 4); out.push_back(u); }
    unsigned i = 1;
    for (; i < n && in[i] != 0x0000FFFFu;) {
        const unsigned t = in[i], op = t & 0xFFFF;
        if (op == 0xFFFE) { const unsigned l = 1 + ((t >> 16) & 0x7FFF); out.insert(out.end(), in + i, in + i + l); i += l; continue; }
        const unsigned len = (t >> 24) & 0xF;
        if (op == 0x42 && ((t >> 16) & 0xFF) == 1 && len == 3 && RegType(in[i + 3]) == 10 && (in[i + 3] & 0x7FF) == sampler) {
            const unsigned D = in[i + 1], Sr = in[i + 2], Smp = in[i + 3];
            const unsigned sw = (Sr >> 16) & 0xFF, w = (sw >> 6) & 3, srcBase = Sr & ~(0xFFu << 16);
            out.push_back(Ins(0x06, 2)); out.push_back(DstT(T1, 0x8)); out.push_back(srcBase | ((w * 0x55u) << 16));        // rcp T1.w, S.w
            out.push_back(Ins(0x05, 3)); out.push_back(DstT(T1, 0x7)); out.push_back(Sr); out.push_back(SrcT(T1, 0xFF));    // mul T1.xyz, S, T1.w
            out.push_back(Ins(0x42, 3)); out.push_back(DstT(T2, 0xF)); out.push_back(SrcT(T1, 0xE4)); out.push_back(Smp);  // texld T2, T1, sN
            out.push_back(Ins(0x02, 3)); out.push_back(DstT(T2, 0x1)); out.push_back(SrcT(T2, 0x00)); out.push_back(SrcT(T1, 0xAA, 1));   // add T2.x, T2.x, -T1.z
            out.push_back(Ins(0x58, 4)); out.push_back(D); out.push_back(SrcT(T2, 0x00));                                     // cmp D, T2.x, c223.x, c223.y
            out.push_back(0x80000000u | 0x20000000u | K | (0x00u << 16)); out.push_back(0x80000000u | 0x20000000u | K | (0x55u << 16));
            ++*count; i += 1 + len; continue;
        }
        out.insert(out.end(), in + i, in + i + 1 + len); i += 1 + len;
    }
    out.push_back(0x0000FFFFu);
    if (!*count) out.clear();
    return out;
}
int CodeSamplerDest(const MaterialPass *ps, unsigned codeImage)
{
    const int na = ps->perPrimArgCount + ps->perObjArgCount + ps->stableArgCount;
    for (int a = 0; a < na; ++a) if (ps->args[a].type == 4 && ps->args[a].u.codeSampler == codeImage) return ps->args[a].dest;
    return -1;
}
// Tecnica 14 de todos los materiales del mundo: pixel shader con el sampler de sombra reescrito (se comparten por techniqueSet)
void PatchMaterials(GfxWorld *w)
{
    std::map<const MaterialPixelShader *, MaterialPixelShader *> done; std::set<const MaterialTechnique *> seenT;
    const char *dump = getenv("H9_DUMPPS"); int dumped = 0;
    for (unsigned i = 0; i < (unsigned)w->materialMemoryCount; ++i) {
        Material *m = w->materialMemory[i].material.get(); if (!m) continue;
        const MaterialTechnique *t = Material_GetTechnique(m, 14); if (!t || !seenT.insert(t).second) continue;
        for (unsigned p = 0; p < t->passCount; ++p) {
            MaterialPass *ps = const_cast<MaterialPass *>(&t->passArray[p]);
            const MaterialPixelShader *px = ps->pixelShader.get(); const int smp = CodeSamplerDest(ps, 7); if (!px || smp < 0) continue;
            auto it = done.find(px);
            if (it == done.end()) {
                int cnt = 0; std::vector<unsigned> nw = PatchShadowSampler(px->prog.loadDef.program.get(), px->prog.loadDef.programSize, (unsigned)smp, &cnt);
                MaterialPixelShader *np = nullptr;
                if (!nw.empty()) {
                    IDirect3DPixelShader9 *obj = nullptr;
                    if (dx.device->CreatePixelShader((const DWORD *)nw.data(), &obj) >= 0 && obj) {
                        np = (MaterialPixelShader *)Ptr32_ArenaAlloc(sizeof(MaterialPixelShader), 16);
                        DWORD *prog = (DWORD *)Ptr32_ArenaAlloc(nw.size() * 4, 16); memcpy(prog, nw.data(), nw.size() * 4);
                        np->name = px->name; np->prog.loadDef.program = prog; np->prog.loadDef.programSize = (unsigned short)nw.size(); np->prog.ps = obj;
                        ++s_patched;
                        if (dump && dumped < 2) { char fn[512]; snprintf(fn, sizeof fn, "%s/ps14_patched_%d.bin", dump, dumped++);
                            if (FILE *f = fopen(fn, "wb")) { fwrite(nw.data(), 4, nw.size(), f); fclose(f); } }
                    }
                }
                if (!np) ++s_patchFail;
                it = done.insert({ px, np }).first;
            }
            if (it->second) ps->pixelShader = it->second;
        }
    }
    fprintf(stderr, "[H9] sombras: %d pixel shaders LIT_SPOT_SHADOW reescritos (texldp de comparacion -> texld + cmp), %d sin reescribir\n", s_patched, s_patchFail);
}

// ---- copia del decomp: R_SetViewParmsForLight / R_GetSpotShadowLookupMatrix (r_spotshadow.cpp, no compilado en el port) ----
void SetViewParmsForLight(const GfxLight *light, GfxViewParms *vp, float nearPlaneBias)
{
    vp->axis[0][0] = -light->dir[0]; vp->axis[0][1] = -light->dir[1]; vp->axis[0][2] = -light->dir[2];
    PerpendicularVector(vp->axis[0], vp->axis[2]);
    Vec3Cross(vp->axis[2], vp->axis[0], vp->axis[1]);
    vp->origin[0] = light->origin[0]; vp->origin[1] = light->origin[1]; vp->origin[2] = light->origin[2]; vp->origin[3] = 1.0f;
    GfxMatrix spotMatrix, rot; MatrixForViewer(vp->origin, vp->axis, spotMatrix.m);
    memset(&rot, 0, sizeof rot); const float s = sinf(light->angles[2]), c = cosf(light->angles[2]);
    rot.m[0][0] = c; rot.m[0][1] = -s; rot.m[1][0] = s; rot.m[1][1] = c; rot.m[2][2] = 1.0f; rot.m[3][3] = 1.0f;
    MatrixMultiply44(spotMatrix.m, rot.m, vp->viewMatrix.m);
    float cosO = light->cosHalfFovOuter; cosO = cosO > 0.999f ? 0.999f : cosO < 0.001f ? 0.001f : cosO;
    const float tanHalf = sqrtf(1.0f - cosO * cosO) / cosO;
    FinitePerspectiveMatrix(tanHalf, tanHalf, nearPlaneBias + 1.0f, light->radius, vp->projectionMatrix.m);
    R_SetupViewProjectionMatrices(vp, 0);
    vp->depthHackNearClip = vp->projectionMatrix.m[3][2];
}
void SpotShadowLookupMatrix(const GfxViewParms *vp, unsigned idx, unsigned tileCount, GfxMatrix *L)
{
    const float x0 = 0.0f, x1 = 1.0f, y1 = (float)idx * (1.0f / tileCount), y0 = 1.0f / tileCount + y1;
    const float xScale = (x1 - x0) * 0.5f, xShift = (x1 + x0) * 0.5f, yScale = (y1 - y0) * 0.5f, yShift = (y1 + y0) * 0.5f;
    const float (*M)[4] = vp->viewProjectionMatrix.m;
    for (int r = 0; r < 4; ++r) { L->m[r][0] = M[r][0] * xScale + M[r][3] * xShift; L->m[r][1] = M[r][1] * yScale + M[r][3] * yShift; L->m[r][2] = M[r][2]; L->m[r][3] = M[r][3]; }
}
// R_ShadowedSpotLightScore
float Score(const GfxViewParms *v, const GfxLight *l)
{
    const float e = sm_lightScore_eyeProjectDist->current.value, k = -l->radius * sm_lightScore_spotProjectFrac->current.value; float d[3];
    for (int j = 0; j < 3; ++j) d[j] = k * l->dir[j] + (l->origin[j] - (e * v->axis[0][j] + v->origin[j]));
    return l->radius * (l->color[0] * 0.299f + l->color[1] * 0.587f + l->color[2] * 0.114f) / (sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1.0f);
}

// ---- pase principal: R_SetShadowableLight con mapa de sombra ----
unsigned char ShadowTech(unsigned char prim)
{
    const int tile = s_tileOf[prim]; GfxLight *L = H5L_Lights ? H5L_Lights() : nullptr;
    if (tile < 0 || !L || !s_prevTech) return s_prevTech ? s_prevTech(prim) : 10;
    GfxLight *l = &L[prim]; if (!l->def || l->radius <= 0.0f || l->type != 2) return s_prevTech(prim);
    GfxCmdBufSourceState *src = gfxCmdBufContext.source; const GfxViewMode vm = src->viewMode; src->viewMode = VIEW_MODE_3D;   // como TechFor (h5_light.cpp)
    src->input.codeImageSamplerStates[7] = 98;   // gfxMetrics.shadowmapSamplerState
    R_SetCodeImageTexture(src, 7u, s_img);
    R_UpdateCodeConstantFromVec4(src, CONST_SRC_CODE_SPOT_SHADOWMAP_PIXEL_ADJUST, s_pixelAdjust);
    R_SetShadowLookupMatrix(src, &s_lookup[tile]);
    R_SetLightProperties(src, l, l->def, LIGHT_HAS_SHADOWMAP, s_forceFade >= 0 ? s_forceFade : s_fadeOf[prim]);
    src->viewMode = vm;
    return 14;
}

bool Init()
{
    if (s_maxLights <= 0 || !s_w || !H5L_Lights || !H5L_Lights()) return false;
    if (const char *r = getenv("H9_SHADOW_RES")) s_tile = atoi(r) <= 256 ? 256 : 512;
    if (const char *c = getenv("H9_SHADOW_CASTERS")) s_casters = atoi(c);
    if (const char *f = getenv("H9_SHADOW_FADE")) s_forceFade = (float)atof(f);
    for (int k = 0; k < 4; ++k) s_pixelAdjust[k] = kPixelAdjust[k] * (512.0f / s_tile);
    // r_init.cpp (R_SetShadowmapFormats) con mapas de sombra por hardware
    gfxMetrics.shadowmapFormatPrimary = D3DFMT_D24S8; gfxMetrics.shadowmapFormatSecondary = D3DFMT_R5G6B5;
    gfxMetrics.shadowmapBuildTechType = 2; gfxMetrics.hasHardwareShadowmap = 1; gfxMetrics.shadowmapSamplerState = 98;
    // Como el prepase floatz (h5_fx.cpp): con R_AllocateMinimalResources el FRAME_BUFFER no tiene depthStencil (usa el del dispositivo). Si
    // se cambia a otro render target con depth propio y se vuelve, se quedaria sin depth (o, peor, h5_fx tomaria como "depth del dispositivo"
    // el del mapa de sombra). Se fija el depth del dispositivo en esos render targets ANTES del primer cambio.
    for (GfxRenderTargetId id : { R_RENDERTARGET_FRAME_BUFFER, R_RENDERTARGET_SCENE })   // FLOAT_Z lo crea h5_fx (asserta si ya tiene algo)
        if (!gfxRenderTargets[id].surface.depthStencil) { IDirect3DSurface9 *ds = nullptr; dx.device->GetDepthStencilSurface(&ds); gfxRenderTargets[id].surface.depthStencil = ds; }
    GfxRenderTarget &rt = gfxRenderTargets[R_RENDERTARGET_SHADOWMAP_SPOT];
    if (!rt.image) R_InitShadowmapRenderTarget(1, (unsigned short)s_tile, 4u, &rt, SHADOWMAP_RT_TILE_VERTICAL);
    if (!rt.image || !rt.surface.depthStencil) { fprintf(stderr, "[H9] sombras: no se pudo crear SHADOWMAP_SPOT\n"); return false; }
    s_img = rt.image;
    PatchMaterials(s_w);
    if (!s_patched) return false;
    for (int i = 0; i < 256; ++i) s_tileOf[i] = -1;
    s_prevTech = g_h4WorldPrimaryTech; g_h4WorldPrimaryTech = ShadowTech;
    fprintf(stderr, "[H9] sombras de focos: %d focos, casilla %d (render target %ux%u D24S8 + color R5G6B5 = %.1f MB), casters %d\n", s_maxLights, s_tile,
        rt.width, rt.height, rt.width * rt.height * (4 + 2) / 1048576.0, s_casters);
    return true;
}

// Seleccion de focos (R_AddPotentiallyShadowedLight + historia con fundido de r_primarylights.cpp)
void SelectLights()
{
    GfxLight *L = H5L_Lights(); const unsigned n = s_w->primaryLightCount < 256 ? s_w->primaryLightCount : 256;
    const int now = NowGameMs(); const int dt = s_lastTime < 0 ? 16 : now - s_lastTime;
    s_lastTime = now;
    float fadeDelta = 0.001f / sm_spotShadowFadeTime->current.value * (float)dt; if (fadeDelta > 1.0f) fadeDelta = 1.0f;
    for (size_t k = 0; k < s_hist.size();) {   // R_FadeOutShadowHistoryEntries
        Hist &h = s_hist[k];
        if (h.fadingOut) { h.fade -= fadeDelta; if (h.fade < 0.0099999998f) { h = s_hist.back(); s_hist.pop_back(); continue; } }
        else h.fadingOut = true;
        ++k;
    }
    const GfxViewParms *v = H4_ViewParms();
    static unsigned char used[256]; H4_LightsUsed(used, 256);
    struct Cand { unsigned i; float s; }; std::vector<Cand> cand;
    for (unsigned i = 1; i < n; ++i) {
        const GfxLight &l = L[i]; if (l.type != 2 || !l.canUseShadowMap || !l.def || l.radius <= 0) continue;
        float d[3]; for (int j = 0; j < 3; ++j) d[j] = l.origin[j] - v->origin[j];
        const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l.cullDist > 0 && dist > l.cullDist) continue;   // scene.shadowableLightIsUsed: cullDist
        if (dist > l.radius && d[0] * v->axis[0][0] + d[1] * v->axis[0][1] + d[2] * v->axis[0][2] < -l.radius) continue;   // la esfera de la luz queda detras
        if (!used[i]) continue;   // alguna superficie visible del frame anterior usa esta luz primaria
        cand.push_back({ i, Score(v, &l) });
    }
    std::sort(cand.begin(), cand.end(), [](const Cand &a, const Cand &b) { return a.s > b.s; });
    if ((int)cand.size() > s_maxLights) cand.resize(s_maxLights);
    for (const Cand &c : cand) {   // R_AddShadowedLightToShadowHistory
        bool found = false;
        for (Hist &h : s_hist) if (h.light == c.i) { h.fadingOut = false; h.fade = std::min(1.0f, h.fade + fadeDelta); found = true; break; }
        if (!found && (int)s_hist.size() < s_maxLights) s_hist.push_back({ c.i, fadeDelta, false });
    }
}

// ---- casters ----

void DrawCasters(unsigned light)
{
    const GfxShadowGeometry &g = s_w->shadowGeom[light];
    static GfxScaledPlacement ident; static bool idOk; if (!idOk) { idOk = true; memset(&ident, 0, sizeof ident); ident.base.quat[3] = 1.0f; ident.scale = 1.0f; }
    if ((s_casters & 1) && g.surfaceCount) {
        const unsigned short *si = g.sortedSurfIndex.get(); static std::vector<unsigned short> run;
        Material *cur = nullptr; run.clear();
        for (unsigned k = 0; k <= g.surfaceCount; ++k) {
            Material *m = k < g.surfaceCount ? s_w->dpvs.surfaces[si[k]].material.get() : nullptr;
            if (k == g.surfaceCount || m != cur) { if (cur && !run.empty()) H4_DrawSurfsPlaced(cur, run.data(), (int)run.size(), 0, &ident, 2); run.clear(); cur = m; }
            if (k < g.surfaceCount) run.push_back(si[k]);
        }
    }
    if ((s_casters & 2) && g.smodelCount && H7C_DrawSModelList) H7C_DrawSModelList(g.smodelIndex.get(), g.smodelCount, 2);
    if ((s_casters & 4) && H5V_DrawActorsShadow) H5V_DrawActorsShadow(H5L_Lights()[light].origin, H5L_Lights()[light].radius, 2);
}
int s_dbg;
void ShadowPass(int W, int H)
{
    for (int i = 0; i < 256; ++i) s_tileOf[i] = -1;
    s_tiles = 0;
    for (const Hist &h : s_hist) { if (s_tiles >= 4) break; const int t = s_tiles++; s_tileLight[t] = h.light; s_tileOf[h.light] = t; s_fadeOf[h.light] = h.fade;
        SetViewParmsForLight(&H5L_Lights()[h.light], &s_vp[t], 0.0f); SpotShadowLookupMatrix(&s_vp[t], t, 4, &s_lookup[t]); }
    if (!s_tiles) return;
    if (getenv("H9_DIAG")) { static int fr; if (fr++ % 300 == 0) for (int t = 0; t < s_tiles; ++t) { const GfxLight &l = H5L_Lights()[s_tileLight[t]];
        fprintf(stderr, "[H9] casilla %d luz %d org (%.0f,%.0f,%.0f) dir (%.2f,%.2f,%.2f) radio %.0f cos %.3f surfs %u smodels %u fade %.2f\n", t, s_tileLight[t], l.origin[0], l.origin[1], l.origin[2],
            l.dir[0], l.dir[1], l.dir[2], l.radius, l.cosHalfFovOuter, s_w->shadowGeom[s_tileLight[t]].surfaceCount, s_w->shadowGeom[s_tileLight[t]].smodelCount, s_fadeOf[s_tileLight[t]]); } }
    const double t0 = NowMs();
    GfxCmdBufContext ctx = gfxCmdBufContext;
    const GfxViewport keepVp = ctx.state->viewport, keepScene = ctx.source->sceneViewport;
    R_SetRenderTargetSize(ctx.source, R_RENDERTARGET_SHADOWMAP_SPOT);
    R_SetRenderTarget(ctx, R_RENDERTARGET_SHADOWMAP_SPOT);
    static const float clr[4] = { 1, 1, 1, 1 };
    if (s_dbg & 8) { R_SetRenderTargetSize(ctx.source, R_RENDERTARGET_FRAME_BUFFER); R_SetRenderTarget(ctx, R_RENDERTARGET_FRAME_BUFFER); return; }
    if (!(s_dbg & 4)) R_ClearScreen(ctx.state->prim.device, 3u, clr, (s_dbg & 2048) ? 0.0f : 1.0f, 0, 0);   // (2048: depth a 0 = todo en sombra, prueba del muestreo)   // RB_SpotShadowMaps: clearScreen en la casilla 0 (shadowmapClearColor)
    if (!(s_dbg & 16)) R_UpdateCodeConstant(ctx.source, CONST_SRC_CODE_SHADOWMAP_POLYGON_OFFSET, sm_polygonOffsetBias->current.value * 0.25f, sm_polygonOffsetScale->current.value, 0.0f, 0.0f);
    for (int t = 0; t < s_tiles && !(s_dbg & 256); ++t) {
        GfxSceneDef sd; memset(&sd, 0, sizeof sd);
        if (!(s_dbg & 32)) R_BeginView(ctx.source, &sd, (s_dbg & 4096) ? H4_ViewParms() : &s_vp[t]);   // (4096: vista de la camara, prueba de escritura)
        GfxViewport vpt = { 0, t * s_tile, s_tile, s_tile };
        if (!(s_dbg & 128)) R_SetViewportStruct(ctx.source, &vpt);
        if (!(s_dbg & 64)) R_SetViewport(ctx.state, &vpt);
        DrawCasters(s_tileLight[t]);
        if (tess.indexCount) RB_EndTessSurface();
    }
    // El clear de H4 y el de floatz van ANTES de su R_SetViewport (que ademas se salta si el estado ya coincide) y el viewport
    // de la casilla 3 (y=1536) se quedaba puesto: el frame salia vacio. Ademas, en el backend vk un SetViewport hecho justo despues
    // de volver al FRAME_BUFFER no llegaba a los draws del mundo (comprobado: solo funciona restaurandolo antes del cambio de target).
    // Se restaura el viewport del frame con el target de sombras aun ligado y luego se vuelve al FRAME_BUFFER.
    GfxViewport full = { 0, 0, W, H }; if (keepVp.width > 0 && keepVp.height > 0) full = keepVp;
    R_SetViewport(ctx.state, &full);
    R_SetRenderTargetSize(ctx.source, R_RENDERTARGET_FRAME_BUFFER); R_SetRenderTarget(ctx, R_RENDERTARGET_FRAME_BUFFER);   // deja el depth del dispositivo enlazado
    R_SetViewportStruct(ctx.source, keepScene.width > 0 && keepScene.height > 0 ? &keepScene : &full);
    s_msAcc += NowMs() - t0;
    if (++s_frames % 600 == 0) { fprintf(stderr, "[H9] sombras: 600 frames, %d focos en el ultimo, pase %.3f ms/frame (CPU)\n", s_tiles, s_msAcc / 600); s_msAcc = 0; }
}
} // namespace

static GfxWorld *s_wSetup;
void H9_Setup(GfxWorld *w) { s_wSetup = w; s_w = w; }

void H9V_Frame() __attribute__((weak));   // h9_video.cpp
void H9R_Install() __attribute__((weak));   // h9_reflect.cpp
void H9_PreFrame(int W, int H)
{
    if (H9V_Frame) H9V_Frame();
    { static bool r; if (!r) { r = true; if (H9R_Install) H9R_Install(); } }   // h9_reflect.cpp
    static bool diag; if (!diag && s_wSetup) { diag = true; if (getenv("H9_DIAG") || getenv("H9_DUMPPS")) H9_Diag(s_wSetup); }   // primer frame: las luces de H5L ya estan
    // numero de focos con sombra: H9_SHADOW=n manda (prueba); si no, las dvars ORIGINALES sm_enable y sm_maxLights, que fija el perfil de
    // calidad (h10_settings.cpp) y la fila SOMBRAS del menu grafico.
    // 0 = apagado: no se crea nada (sin coste); al subirlo se crea el render target la primera vez y se queda.
    static const int env = getenv("H9_SHADOW") ? atoi(getenv("H9_SHADOW")) : -1;
    int want = env >= 0 ? env : (sm_enable && sm_enable->current.enabled && sm_maxLights ? sm_maxLights->current.integer : 0); want = want < 0 ? 0 : want > 4 ? 4 : want;
    if (env < 0) {   // hasta que h10_settings aplique el perfil (config.cfg) las dvars tienen el valor de serie (4): no crear nada antes
        static const bool h10 = !(getenv("H10") && !atoi(getenv("H10"))); static bool ready = false;
        if (h10 && !ready) { if (!Dvar_FindVar("h10_quality")) want = 0; else ready = true; } }
    if (want != s_maxLights) { fprintf(stderr, "[H9] sombras: %d focos (%s)\n", want, env >= 0 ? "H9_SHADOW" : "sm_enable/sm_maxLights del perfil"); s_maxLights = want; }
    if (!want) { if (s_state > 0) { s_hist.clear(); for (int i = 0; i < 256; ++i) s_tileOf[i] = -1; } return; }
    if (!s_state) s_state = Init() ? 1 : -1;
    if (s_state < 0) return;
    if (g_h4WorldPrimaryTech != ShadowTech) { s_prevTech = g_h4WorldPrimaryTech; g_h4WorldPrimaryTech = ShadowTech; }
    static const int dbg = getenv("H9_SHADOW_DBG") ? atoi(getenv("H9_SHADOW_DBG")) : 0;   // diagnostico: 1 sin tecnica 14, 2 sin pase de sombra, 4 sin clear, 8 solo cambio de target, 2048 mapa a 0
    s_dbg = dbg;
    SelectLights();
    if (!(dbg & 2)) ShadowPass(W, H); else { for (int i = 0; i < 256; ++i) s_tileOf[i] = -1; }
    if (dbg & 1) for (int i = 0; i < 256; ++i) s_tileOf[i] = -1;
}
