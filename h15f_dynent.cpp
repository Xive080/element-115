// OLA 15F: DynEnts de modelo (pilas de papeles, carpetas, libros, cajas, tapas de cajas electricas...) que el port no dibujaba.
// Flujo del original: DynEntCl_InitEntities copia dynEntDef->pose a la pose de cliente (memcpy 0x1C), pone health = dynEntDef->health y
// enlaza el modelo (DynEntCl_LinkModel -> celdas); R_DrawAllDynEnt (r_dpvs.cpp) recorre dpvsDyn.dynEntClientCount[0], toma el modelo de
// DynEntCl_GetCurrentXModel (xModel, o destroyedxModel con la salud agotada) y lo pinta rigido con R_SkinXModel en su pose; la luz de
// modelo sale del lightgrid en su origen (como los smodels/script_model de cliente del port).
// Aqui: la misma pose inicial, el mismo modelo y los mismos LOD, dibujados como los smodels del port (h7c_gfx.cpp DrawSModels):
// vertices en mundo cacheados por superficie, tope de distancia/cono en vez de la visibilidad por celdas (H4 no tiene dpvs).
// NO hay fisica (Nitrous sin portar): no caen ni salen volando al disparar ni se rompen (DynEntCl_Damage); se quedan en su pose del mapa,
// que es como el original los muestra hasta que algo los toca.
//   H15F_DYNENT=0 los quita; H15F_DYNENT_DIST (1200) tope de distancia; H15F_DYNENT_MB (4) tope de la cache de vertices;
//   H15F_DYNENT_LOG=1 registra cuantos se dibujan.
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <qcommon/cm_load_obj.h>
#include <qcommon/cm_load.h>
#include <DynEntity/DynEntity_client.h>
#include <xanim/xmodel.h>
#include <xanim/xmodel_utils.h>
#include <gfx_d3d/r_model_skin.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_init.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "world_h4.h"
#include <database/db_registry.h>
#include <database/db_assetnames.h>
#include <sys/stat.h>

void H5L_Prepare(int key, const float *lightingOrigin, const float *half, bool hero) __attribute__((weak));
void H5L_Bind(int key) __attribute__((weak));
int H5L_MaxKey() __attribute__((weak));

namespace {
struct Dyn { XModel *m; float org[3], axis[3][3], radius; int key; bool vis; int lod; std::vector<std::vector<GfxPackedVertex>> v; };
std::vector<Dyn> s_d; bool s_init, s_on; size_t s_bytes, s_cap; float s_maxD;
DObjSkelMat *s_skel; GfxPackedVertex *s_tmp; int s_tmpCap;

void Init()
{
    s_init = true; const char *e = getenv("H15F_DYNENT"); s_on = !(e && !atoi(e)); if (!s_on) return;
    s_maxD = getenv("H15F_DYNENT_DIST") ? (float)atof(getenv("H15F_DYNENT_DIST")) : 1200.0f;
    s_cap = (size_t)((getenv("H15F_DYNENT_MB") ? atof(getenv("H15F_DYNENT_MB")) : 4.0) * 1048576);
    const DynEntityDef *L = cm.dynEntDefList[DYNENT_DRAW_MODEL].get(); const unsigned n = L ? cm.dynEntCount[DYNENT_DRAW_MODEL] : 0;
    // claves de luz de modelo detras de las de smodels (1024 + i) y de los script_model de cliente (h7e: 1024 + smodelCount + j)
    const int base = 1024 + (int)(rgp.world ? rgp.world->dpvs.smodelCount : 0) + 512, maxKey = H5L_MaxKey ? H5L_MaxKey() : 0;
    int lit = 0;
    for (unsigned i = 0; i < n; ++i) {
        const DynEntityDef &de = L[i];
        XModel *m = de.health > 0 || !de.destroyedxModel.get() ? de.xModel.get() : de.destroyedxModel.get();   // DynEntCl_GetCurrentXModel con health = dynEntDef->health
        if (!m) continue;   // ranuras de piezas extra (destructibles) vacias: tipo 0 sin modelo
        Dyn d; d.m = m; memcpy(d.org, de.pose.origin, 12); UnitQuatToAxis(de.pose.quat, d.axis); d.radius = XModelGetRadius(m);
        d.key = base + (int)s_d.size() <= maxKey ? base + (int)s_d.size() : -2; if (d.key >= 0) ++lit;
        d.vis = false; d.lod = -1; s_d.push_back(std::move(d));
    }
    if (posix_memalign((void **)&s_skel, 16, 128 * sizeof(DObjSkelMat))) { s_on = false; return; }
    fprintf(stderr, "[H15F] dynents: %zu de modelo con xModel (de %u ranuras; %d con luz propia)\n", s_d.size(), n, lit);
}
} // namespace

// H15F_RAWDUMP="dir[:texto]" (diagnostico): vuelca a dir (FUERA del repo: son datos del juego) los rawfiles cuyo nombre contiene texto
// (por defecto clientscripts/), para leer el CSC/GSC original (relojes, giros, client flags).
static void RawDump()
{
    const char *e = getenv("H15F_RAWDUMP"); if (!e) return;
    static std::string dir, sub; dir = e; sub = "clientscripts/"; const size_t c = dir.find(':'); if (c != std::string::npos) { sub = dir.substr(c + 1); dir.resize(c); }
    DB_EnumXAssets_FastFile(ASSET_TYPE_RAWFILE, [](XAssetHeader h, void *) {
        if (!h.rawfile || !h.rawfile->name.get() || !strstr(h.rawfile->name.get(), sub.c_str()) || !h.rawfile->buffer.get()) return;
        std::string n = h.rawfile->name.get(); for (auto &ch : n) if (ch == '/' || ch == '\\') ch = '_';
        FILE *f = fopen((dir + "/" + n).c_str(), "wb"); if (!f) return; fwrite(h.rawfile->buffer.get(), 1, h.rawfile->len, f); fclose(f);
        fprintf(stderr, "[H15F] rawfile %s (%d)\n", h.rawfile->name.get(), h.rawfile->len); }, nullptr, false);
}
void H15F_PrepareDynEnts()
{
    if (!s_init) { Init(); RawDump(); }
    if (!s_on || s_d.empty()) return;
    float cam[3], ang[3], fwd[3]; H4_GetCam(cam, ang); AngleVectors(ang, fwd, nullptr, nullptr);
    static const float half[3] = { 8, 8, 8 };
    for (Dyn &d : s_d) {
        const float dx = d.org[0] - cam[0], dy = d.org[1] - cam[1], dz = d.org[2] - cam[2], dist = sqrtf(dx * dx + dy * dy + dz * dz), r = d.radius + 8.0f;
        d.vis = dist - r <= s_maxD && (dist <= r || dx * fwd[0] + dy * fwd[1] + dz * fwd[2] >= 0.3f * dist - r);
        if (d.vis && d.key >= 0 && H5L_Prepare) H5L_Prepare(d.key, d.org, half, false);
    }
}

void H15F_DrawDynEnts(unsigned char techType)
{
    if (!s_on || s_d.empty()) return;
    float cam[3], ang[3]; H4_GetCam(cam, ang);
    int drawn = 0, surfs = 0;
    for (Dyn &d : s_d) {
        if (!d.vis) continue;
        const float dist = Vec3Distance(d.org, cam);
        const int lod = XModelGetLodForDist(d.m, dist, dist, false); if (lod < 0) continue;
        const int nb = XModelNumBones(d.m); if (nb > 128) continue;
        XSurface *xs; const int ns = XModelGetSurfaces(d.m, &xs, lod); PTR32(Material) *skins = XModelGetSkins(d.m, lod);
        if (d.lod != lod) {   // otro LOD: suelta la cache del anterior
            for (auto &v : d.v) { s_bytes -= v.capacity() * sizeof(GfxPackedVertex); std::vector<GfxPackedVertex>().swap(v); }
            d.v.assign(ns, {}); d.lod = lod;
        }
        if (H5L_Bind) H5L_Bind(d.key);
        bool built = false;
        for (int s = 0; s < ns; ++s) {
            Material *mtl = skins ? skins[s].get() : nullptr; if (!mtl || !xs[s].vertCount) continue;
            GfxPackedVertex *v;
            if (!d.v[s].empty()) v = d.v[s].data();
            else {
                if (!built) { built = true; for (int b = 0; b < nb; ++b) { DObjSkelMat &o = s_skel[b];
                    for (int a = 0; a < 3; ++a) { for (int c = 0; c < 3; ++c) o.axis[a][c] = d.axis[a][c]; o.axis[a][3] = 0; }
                    memcpy(o.origin, d.org, 12); o.origin[3] = 1.0f; } }
                if ((int)xs[s].vertCount > s_tmpCap) { free(s_tmp); s_tmpCap = xs[s].vertCount + 256;
                    if (posix_memalign((void **)&s_tmp, 16, s_tmpCap * sizeof(GfxPackedVertex))) { s_tmp = nullptr; s_tmpCap = 0; return; } }
                R_SkinXSurfaceSkinned(&xs[s], s_skel, s_tmp); v = s_tmp;
                const size_t bytes = xs[s].vertCount * sizeof(GfxPackedVertex);
                if (s_bytes + bytes <= s_cap) { d.v[s].assign(s_tmp, s_tmp + xs[s].vertCount); s_bytes += bytes; }
            }
            H4_DrawSkinnedSurf(mtl, &xs[s], v, techType); ++surfs;
        }
        ++drawn;
    }
    static int n; if (getenv("H15F_DYNENT_LOG") && (++n % 120) == 0)
        fprintf(stderr, "[H15F] dynents: %d dibujados, %d superficies, cache %.2f MB\n", drawn, surfs, s_bytes / 1048576.0);
}
#endif
