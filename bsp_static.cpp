// T10: indice estatico del mundo + culling por frustum (AABB de las surfaces) para el camino H4.
// Se compila incluido desde world_h4.cpp (unidad unica): build.sh no cambia. Sin esto, R_DrawBspTris copia los indices de
// cada draw a un IB dinamico (R_SetIndexData): 1948 locks por frame. Aqui el IB del mundo se crea UNA vez con g_worldDraw->indices
// y R_DrawBspTris (gancho g_bspTrisHook en zdec r_draw_bsp.cpp) dibuja con baseIndex = tris->baseIndex.
// Variables: H4_STATICIB=0 desactiva el IB estatico; H4_CULL=0 desactiva el culling (la traza debe ser identica a la de antes).
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static IDirect3DIndexBuffer9 *g_sIB; static unsigned g_sIdxCount; static long g_sDraws;

static bool BspStatic_Hook(GfxCmdBufPrimState *state, const srfTriangles_t *tris, unsigned int triCount)
{
    if (!g_sIB) return false;
    if ((unsigned)tris->baseIndex + 3u * triCount > g_sIdxCount) return false;   // fuera de rango: camino dinamico de siempre
    GfxDrawPrimArgs args; args.vertexCount = tris->vertexCount; args.triCount = triCount; args.baseIndex = tris->baseIndex;
    if (state->indexBuffer != g_sIB) R_ChangeIndices(state, g_sIB);
    R_DrawIndexedPrimitive(state, &args);
    if (state->primStats) state->frameStats.geoIndexCount += 3 * triCount;   // estatico: no cuenta como dynamicIndexCount
    ++g_sDraws;
    return true;
}

// Crea el IB estatico (una vez). Devuelve false si no se pudo; entonces todo sigue por el camino dinamico.
static bool BspStatic_Build(const GfxWorldDraw *d)
{
    if (g_sIB) return true;
    const char *e = getenv("H4_STATICIB"); if (e && !atoi(e)) return false;
    if (!d->indices || d->indexCount <= 0) return false;
    IDirect3DIndexBuffer9 *ib = 0; unsigned bytes = (unsigned)d->indexCount * 2;
    if (dx.device->CreateIndexBuffer(bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, 0) < 0 || !ib) return false;
    void *p = 0; if (ib->Lock(0, bytes, &p, 0) < 0 || !p) return false;
    memcpy(p, d->indices.get(), bytes); ib->Unlock();
    g_sIB = ib; g_sIdxCount = (unsigned)d->indexCount; g_bspTrisHook = BspStatic_Hook;
    fprintf(stderr, "[H4] IB estatico del mundo: %d indices (%.2f MB), baseIndex directo\n", d->indexCount, bytes / 1048576.0);
    return true;
}

// ---- culling: 5 planos laterales/cercano (la proyeccion es infinita) construidos del eje de la camara, sin depender del convenio de la matriz
struct CullPlane { float n[3], k; };   // dentro si n.p + k >= 0
struct Frustum { CullPlane p[5]; };
static void BspStatic_MakeFrustum(Frustum *f, const float org[3], const float axis[3][3], float tx, float ty)
{
    const float *F = axis[0], *L = axis[1], *U = axis[2];   // adelante, izquierda, arriba (AnglesToAxis)
    float nn[5][3];
    for (int i = 0; i < 3; ++i) {
        nn[0][i] = tx * F[i] - L[i];   // lado izquierdo:  l <= tx*d
        nn[1][i] = tx * F[i] + L[i];   // lado derecho:   -l <= tx*d
        nn[2][i] = ty * F[i] - U[i];   // arriba
        nn[3][i] = ty * F[i] + U[i];   // abajo
        nn[4][i] = F[i];               // detras de la camara
    }
    for (int j = 0; j < 5; ++j) { for (int i = 0; i < 3; ++i) f->p[j].n[i] = nn[j][i]; f->p[j].k = -(nn[j][0] * org[0] + nn[j][1] * org[1] + nn[j][2] * org[2]); }
}
static inline bool BspStatic_BoxVisible(const Frustum &f, const float mn[3], const float mx[3])
{
    for (int j = 0; j < 5; ++j) {
        const CullPlane &p = f.p[j];
        float s = p.k;   // maximo de n.p en la caja (vertice "positivo")
        for (int i = 0; i < 3; ++i) s += p.n[i] * (p.n[i] >= 0 ? mx[i] : mn[i]);
        if (s < 0) return false;
    }
    return true;
}
