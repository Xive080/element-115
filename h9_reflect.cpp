// OLA 9: sondas de reflejo (cubemaps) en los MODELOS.
//
// Lo que hace el original: cada superficie que muestrea la sonda (pase con customSamplerFlags & 1, sampler 15 = cubo de la sonda)
// llama a R_SetReflectionProbe con el indice de SU objeto:
//   - superficies BSP: GfxSurface::reflectionProbeIndex (R_DrawBspDrawSurfsLit; H4 ya lo hace desde la OLA 15B);
//   - smodels: GfxStaticModelDrawInst::reflectionProbeIndex precalculado (R_DrawStaticModelDrawSurf...);
//   - entidades/actores/viewmodel: R_CalcReflectionProbeIndex(lightingOrigin) (r_model_lighting.cpp: volumen de sonda, si no la
//     mas cercana de la celda, si no la mas cercana del mapa). r_dpvs.cpp no esta compilado en el port: se copia aqui.
// En el port los modelos (H4_DrawSkinnedSurf) no fijaban ninguna: muestreaban la ultima sonda que hubiera dejado el mundo (cualquiera).
// Aqui: el llamador puede fijar la sonda del objeto (h7c: la del smodel); si no, se calcula como el original con
// R_CalcReflectionProbeIndex en el centro de la superficie ya deformada (vertices en mundo), con una cache por posicion.
// Las sondas del mundo (los cubos del ff) ya estan cargadas: elegir la correcta no cuesta memoria y apenas CPU (una consulta BSP por
// superficie nueva; cache de 256 entradas).
//   H9_REFLECT=0 lo desactiva (comportamiento anterior). La dvar h10_reflections (gancho de la OLA 10) hace lo mismo desde el menu.
#ifdef H5G
#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_state.h>
#include <gfx_d3d/r_dpvs.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_model_skin.h>
#include <gfx_d3d/r_reflection_probe.h>
#include <gfx_d3d/rb_backend.h>
#include <universal/dvar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern void (*g_h4ModelProbe)(GfxCmdBufContext, const XSurface *, const GfxPackedVertex *) __attribute__((weak));   // world_h4.cpp

// r_dpvs.cpp no esta en el port (sus funciones son stubs): R_CalcReflectionProbeIndex y lo que usa, copiados tal cual
// (volumen de sonda -> celda del punto (R_CellForPoint) -> la mas cercana de la celda; fuera de celda, la mas cercana del mapa).
static unsigned FindProbeFromVolume(const GfxWorldDraw *wd, const float *o)
{
    for (unsigned p = 1; p < wd->reflectionProbeCount; ++p) {
        const GfxReflectionProbe *rp = &wd->reflectionProbes[p];
        for (unsigned v = 0; v < rp->probeVolumeCount; ++v) {
            bool out = false;
            for (int k = 0; k < 6 && !out; ++k) { const float *pl = rp->probeVolumes[v].volumePlanes[k]; if (pl[0] * o[0] + pl[1] * o[1] + pl[2] * o[2] + pl[3] < 0.0f) out = true; }
            if (!out) return p;
        }
    }
    return 0;
}
static unsigned NearestProbe(const GfxWorldDraw *wd, const float *o, const unsigned char *list, unsigned n)
{
    unsigned best = 0; float bd = 3.4e38f;
    const unsigned cnt = list ? n : wd->reflectionProbeCount;
    for (unsigned i = list ? 0 : 1; i < cnt; ++i) {
        const unsigned p = list ? list[i] : i; if (p >= wd->reflectionProbeCount || wd->reflectionProbes[p].probeVolumeCount) continue;
        const float *c = wd->reflectionProbes[p].origin; const float d = (c[0] - o[0]) * (c[0] - o[0]) + (c[1] - o[1]) * (c[1] - o[1]) + (c[2] - o[2]) * (c[2] - o[2]);
        if (d < bd) { bd = d; best = p; }
    }
    return best;
}
static int CellForPoint(const float *o)
{
    if (!rgp.world || !rgp.world->dpvsPlanes.nodes) return -1;
    const unsigned char *node = (const unsigned char *)rgp.world->dpvsPlanes.nodes.get();
    const int cellCount = rgp.world->dpvsPlanes.cellCount + 1;
    for (int guard = 0; guard < 100000; ++guard) {
        const mnode_t *n = (const mnode_t *)node; const int ci = n->cellIndex;
        if (ci - cellCount < 0) return ci - 1;
        const cplane_s *pl = &rgp.world->dpvsPlanes.planes[ci - cellCount];
        const bool back = o[0] * pl->normal[0] + o[1] * pl->normal[1] + o[2] * pl->normal[2] - pl->dist <= 0.0f;
        node += 2 * (back ? (n->rightChildOffset - 2) : 0) + 4;
    }
    return -1;
}
static unsigned CalcReflectionProbeIndex(const float *o)
{
    const GfxWorldDraw *wd = g_worldDraw;
    if (const unsigned p = FindProbeFromVolume(wd, o)) return p;
    const int c = CellForPoint(o);
    if (c < 0 || c >= rgp.world->dpvsPlanes.cellCount) return NearestProbe(wd, o, nullptr, 0);
    const GfxCell &cell = rgp.world->cells[c];
    return NearestProbe(wd, o, cell.reflectionProbes.get(), cell.reflectionProbeCount);
}

namespace {
int s_objProbe = -1;   // sonda fijada por el llamador para el objeto en curso (h7c: smodel); -1 = calcular
struct CacheE { int key[3]; unsigned char probe; bool used; };
CacheE s_cache[256];
unsigned s_calls, s_calc, s_set;

bool On()
{
    static const bool env = !(getenv("H9_REFLECT") && !atoi(getenv("H9_REFLECT")));
    if (!env) return false;
    static const dvar_s *d = nullptr; static bool looked = false;
    if (!looked) { looked = true; d = Dvar_FindVar("h10_reflections"); }
    return !d || d->current.integer != 0;
}
unsigned ProbeAt(const float *o)
{
    const int k[3] = { (int)floorf(o[0] / 32.0f), (int)floorf(o[1] / 32.0f), (int)floorf(o[2] / 32.0f) };   // rejilla de 32 unidades
    const unsigned h = ((unsigned)k[0] * 73856093u ^ (unsigned)k[1] * 19349663u ^ (unsigned)k[2] * 83492791u) & 255u;
    CacheE &e = s_cache[h];
    if (e.used && e.key[0] == k[0] && e.key[1] == k[1] && e.key[2] == k[2]) return e.probe;
    ++s_calc;
    const float c[3] = { (k[0] + 0.5f) * 32.0f, (k[1] + 0.5f) * 32.0f, (k[2] + 0.5f) * 32.0f };
    unsigned p = CalcReflectionProbeIndex(c);
    if (p >= g_worldDraw->reflectionProbeCount) p = 0;
    e.key[0] = k[0]; e.key[1] = k[1]; e.key[2] = k[2]; e.probe = (unsigned char)p; e.used = true;
    return p;
}
void ModelProbe(GfxCmdBufContext ctx, const XSurface *xs, const GfxPackedVertex *v)
{
    ++s_calls;
    if (!ctx.state->pass || !(ctx.state->pass->customSamplerFlags & 1) || !g_worldDraw || !g_worldDraw->reflectionProbeCount || !On()) return;
    unsigned p;
    if (s_objProbe >= 0 && (unsigned)s_objProbe < g_worldDraw->reflectionProbeCount) p = (unsigned)s_objProbe;
    else {
        if (!xs || !v || !xs->vertCount) return;
        float lo[3] = { v[0].xyz[0], v[0].xyz[1], v[0].xyz[2] }, hi[3] = { lo[0], lo[1], lo[2] };
        const unsigned step = xs->vertCount > 64 ? xs->vertCount / 64 : 1;   // caja aproximada (64 muestras)
        for (unsigned i = 0; i < xs->vertCount; i += step) for (int a = 0; a < 3; ++a) { lo[a] = fminf(lo[a], v[i].xyz[a]); hi[a] = fmaxf(hi[a], v[i].xyz[a]); }
        const float c[3] = { 0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2]) };
        p = ProbeAt(c);
    }
    if (!g_worldDraw->reflectionProbes[p].reflectionImage) return;
    R_SetReflectionProbe(ctx, p); ++s_set;
    static const bool diag = getenv("H9_DIAG") != nullptr;   // OLA F3: getenv una vez, no por superficie
    if (diag && (s_set == 1 || s_set % 20000 == 0))
        fprintf(stderr, "[H9R] sondas de modelo: %u superficies, %u con sonda fijada, %u consultas BSP (sondas del mapa %u)\n", s_calls, s_set, s_calc, g_worldDraw->reflectionProbeCount);
}
} // namespace

// h7c_gfx.cpp: sonda del smodel en curso (GfxStaticModelDrawInst::reflectionProbeIndex); -1 al terminar
void H9R_ObjectProbe(int probe) { s_objProbe = probe; }

void H9R_Install()
{
    if (&g_h4ModelProbe && !g_h4ModelProbe) {
        g_h4ModelProbe = ModelProbe;
        fprintf(stderr, "[H9R] sondas de reflejo por objeto en modelos: %s (sondas del mapa %u)\n", On() ? "activas" : "apagadas (H9_REFLECT=0 / h10_reflections 0)",
            g_worldDraw ? g_worldDraw->reflectionProbeCount : 0);
    }
}
#else
void H9R_ObjectProbe(int) {}
void H9R_Install() {}
#endif
