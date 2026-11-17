// OLA 7c: fidelidad frente al juego original (shot14.png, sala del Pentagono).
// Diagnosticos y arreglos activables por variables H7C_*. Se engancha por simbolos debiles desde world_h4.cpp.
#include <database/db_registry.h>
#include <database/db_assetnames.h>
#include <universal/ptr32.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_image.h>
#include <xanim/xmodel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>

static bool UsesCodeSampler(Material *m, int lo, int hi, int *which)
{
    if (!m || !m->techniqueSet) return false; MaterialTechniqueSet *ts = m->techniqueSet.get();
    for (int t = 0; t < 130; ++t) { MaterialTechnique *te = ts->techniques[t].get(); if (!te) continue;
        for (int p = 0; p < te->passCount; ++p) { const MaterialPass &ps = te->passArray[p]; const int n = ps.perPrimArgCount + ps.perObjArgCount + ps.stableArgCount;
            for (int a = 0; a < n; ++a) { const MaterialShaderArgument &ar = ps.args.get()[a];
                if (ar.type == 4 && (int)ar.u.codeSampler >= lo && (int)ar.u.codeSampler <= hi) { if (which) *which = ar.u.codeSampler; return true; } } } }
    return false;
}

// H7C_DIAG=1: materiales del mundo y de los smodels que muestrean texturas de cinematica (code 0x17..0x1A) o cuyo nombre
// sugiere una pantalla; con sus limites.
static void ListAsset(XAssetHeader h, void *pat)
{
    const char *nm = h.material ? h.material->info.name.get() : nullptr;   // Material y MenuDef empiezan por el nombre (puntero de zona)
    if (nm && strstr(nm, (const char *)pat)) fprintf(stderr, "[H7C] asset '%s'\n", nm);
}
void H7C_Diag(GfxWorld *w)
{
    if (const char *p = getenv("H7C_LISTMAT")) DB_EnumXAssets_FastFile(ASSET_TYPE_MATERIAL, ListAsset, (void *)p, false);
    if (!getenv("H7C_DIAG") || !w) return;
    std::map<std::string, int> cnt; std::map<std::string, std::string> info;
    for (int i = 0; i < w->surfaceCount; ++i) {
        GfxSurface *s = &w->dpvs.surfaces[i]; Material *m = s->material.get(); if (!m) continue;
        const char *nm = m->info.name.get(); int wh = -1;
        bool cin = UsesCodeSampler(m, 0x17, 0x1A, &wh);
        bool byName = strstr(nm, "screen") || strstr(nm, "monitor") || strstr(nm, "tv") || strstr(nm, "cin") || strstr(nm, "video") || strstr(nm, "pent");
        if (!cin && !byName) continue;
        if (cnt[nm]++ < 3) fprintf(stderr, "[H7C] surf %d mat '%s' cin=%d(code %d) bounds (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) prim=%d lm=%d\n", i, nm, cin, wh,
            s->bounds[0][0], s->bounds[0][1], s->bounds[0][2], s->bounds[1][0], s->bounds[1][1], s->bounds[1][2], s->primaryLightIndex, s->lightmapIndex);
    }
    for (auto &kv : cnt) fprintf(stderr, "[H7C] mundo: %s x%d\n", kv.first.c_str(), kv.second);
    int smc = 0;
    for (unsigned i = 0; i < w->dpvs.smodelCount; ++i) {
        GfxStaticModelDrawInst *si = &w->dpvs.smodelDrawInsts[i]; XModel *xm = si->model.get(); if (!xm) continue;
        const char *mn = xm->name.get(); bool hit = false; int wh = -1;
        for (int k = 0; k < xm->numsurfs; ++k) { Material *m = xm->materialHandles.get()[k].get(); if (UsesCodeSampler(m, 0x17, 0x1A, &wh) || (m && (strstr(m->info.name.get(), "screen") || strstr(m->info.name.get(), "monitor")))) { hit = true; if (smc < 40) fprintf(stderr, "[H7C] smodel %u '%s' mat '%s' cin code %d en (%.0f,%.0f,%.0f)\n", i, mn, m->info.name.get(), wh, si->placement.origin[0], si->placement.origin[1], si->placement.origin[2]); } }
        smc += hit;
    }
    fprintf(stderr, "[H7C] smodels=%u con pantalla/cinematica=%d\n", w->dpvs.smodelCount, smc);
    for (int i = 0; i < w->modelCount; ++i) { const GfxBrushModel *b = &w->models[i]; if (!b->surfaceCount) continue;
        Material *m = w->dpvs.surfaces[b->startSurfIndex].material.get();
        fprintf(stderr, "[H7C] bmodel %d surfs %u..+%u bounds (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) mat0 '%s'\n", i, b->startSurfIndex, b->surfaceCount,
            b->bounds[0][0], b->bounds[0][1], b->bounds[0][2], b->bounds[1][0], b->bounds[1][1], b->bounds[1][2], m ? m->info.name.get() : "-"); }
}

// ---- 1. Brushmodels (script_brushmodel / submodelos *N) con la colocacion de su entidad.
// H4 metia TODAS las superficies del GfxWorld en los lotes estaticos, pero solo las del modelo 0 son mundo; las de los modelos
// 1..N estan en coordenadas LOCALES y el original las dibuja por entidad (CG_ScriptMover: solid == SOLID_BMODEL y sin EF 0x20
// -> R_AddBrushModelToSceneFromAngles -> R_TessBModel con R_ChangeObjectPlacementRemote). El muro de 6 pantallas de la sala
// inicial son los submodelos 78 (pent_art_tv_4x4_*: imagenes) y 79 (pent_art_monitor_static*: ruido), que el GSC alterna.
//   H7C_BMODEL=0 vuelve a lo de antes (todas en el mundo, sin entidad).  H7C_BMODEL_LOG=1 lista las entidades dibujadas.
static bool s_bmOn = true; static GfxWorld *s_w; static float s_smFovCos = 0.5f;   // coseno del cono de vision para los smodels (H7C_FovX)
bool H7C_SkipWorldSurf(GfxWorld *w, int i)
{
#ifdef H5G
    return s_bmOn && w->modelCount > 1 && (unsigned)i >= w->models[0].surfaceCount;
#else
    return false;
#endif
}
#ifdef H5G
#include <game_mp/g_main_mp.h>
#include <universal/com_math_anglevectors.h>
#include <vector>
#include <algorithm>
void H4_DrawSurfsPlaced(Material *m, const unsigned short *surfs, int n, unsigned char prim, const GfxScaledPlacement *pl, unsigned char techType);
struct BmGroup { Material *m; unsigned char prim; std::vector<unsigned short> s; };
static std::vector<std::vector<BmGroup>> s_bmGroups; static GfxScaledPlacement s_bmPl[1024];
static const std::vector<BmGroup> &BmGroups(int mi)
{
    if ((int)s_bmGroups.size() < s_w->modelCount) s_bmGroups.resize(s_w->modelCount);
    std::vector<BmGroup> &g = s_bmGroups[mi];
    if (g.empty()) {
        const GfxBrushModel *b = &s_w->models[mi];
        for (unsigned k = 0; k < b->surfaceCount; ++k) { const int si = b->startSurfIndex + k; GfxSurface *sf = &s_w->dpvs.surfaces[si]; Material *m = sf->material.get();
            if (!m || !sf->tris.triCount) continue;
            auto it = std::find_if(g.begin(), g.end(), [&](const BmGroup &x) { return x.m == m && x.prim == sf->primaryLightIndex; });
            if (it == g.end()) { g.push_back({ m, sf->primaryLightIndex, {} }); it = g.end() - 1; }
            it->s.push_back((unsigned short)si); }
        std::stable_sort(g.begin(), g.end(), [](const BmGroup &a, const BmGroup &b) { return a.m->info.drawSurf.fields.primarySortKey < b.m->info.drawSurf.fields.primarySortKey; });
    }
    return g;
}
static void DrawBModels(unsigned char techType)
{
    static int logged = 0; const bool log = getenv("H7C_BMODEL_LOG") && logged < 3;
    int n = 0;
    for (int e = 0; e < level.num_entities && n < 1024; ++e) {
        gentity_s *ent = &g_entities[e];
        if (!ent->r.inuse || ent->s.solid != 0xFFFFFF || (ent->s.lerp.eFlags & 0x20)) continue;
        if (!ent->r.linked || (ent->r.svFlags & 1)) continue;   // como el snapshot (sv_snapshot_mp.cpp): sin enlazar o SVF_NOCLIENT no llegan al cliente
        const int mi = ent->s.index.brushmodel; if (mi <= 0 || mi >= s_w->modelCount || !s_w->models[mi].surfaceCount) continue;
        GfxScaledPlacement *pl = &s_bmPl[n++];
        memcpy(pl->base.origin, ent->r.currentOrigin, 12); AnglesToQuat(ent->r.currentAngles, pl->base.quat); pl->scale = 1.0f;
        if (log) fprintf(stderr, "[H7C] bmodel ent %d tipo %d modelo *%d en (%.0f,%.0f,%.0f) ang (%.0f,%.0f,%.0f) svf %x linked %d mat0 %s\n", e, ent->s.eType, mi, pl->base.origin[0], pl->base.origin[1], pl->base.origin[2], ent->r.currentAngles[0], ent->r.currentAngles[1], ent->r.currentAngles[2], ent->r.svFlags, (int)ent->r.linked, BmGroups(mi).empty() ? "-" : BmGroups(mi)[0].m->info.name.get());
        if (const char *sk = getenv("H7C_BMODEL_SKIP")) { char t[16]; snprintf(t, sizeof t, ",%d,", mi); if (strstr(sk, t)) continue; }
        for (const BmGroup &g : BmGroups(mi)) H4_DrawSurfsPlaced(g.m, g.s.data(), (int)g.s.size(), g.prim, pl, techType);
    }
    if (log) { ++logged; fprintf(stderr, "[H7C] brushmodels dibujados: %d\n", n); }
}
// ---- 3. Viewmodel.
// a) Partes ocultas: el original (R_SkinXModel, r_dobj_skin.cpp) no dibuja las superficies cuyos huesos estan en hidePartBits del
//    DObj (CG_SetWeaponHidePartBits pone los hideTags del arma: el silenciador del t5_weapon_1911_viewmodel). El puente de skinning
//    los dibujaba todos.  H7C_HIDEPARTS=0 lo desactiva.
#include <xanim/dobj.h>
#include <xanim/dobj_utils.h>
#include <xanim/xanim.h>
bool H7C_SurfHidden(const DObj *obj, const XSurface *surface, int boneIndex)
{
    static const bool on = !(getenv("H7C_HIDEPARTS") && !atoi(getenv("H7C_HIDEPARTS")));
    if (!on) return false;
    unsigned hide[5]; DObjGetHidePartBits(obj, hide);
    if (!(hide[0] | hide[1] | hide[2] | hide[3] | hide[4])) return false;
    unsigned sp[9] = { 0, 0, 0, 0 }; for (int m = 0; m < 5; ++m) sp[m + 4] = (unsigned)surface->partBits[m];
    const int d = boneIndex >> 5, md = boneIndex & 31, rm = 32 - md; unsigned c[5];
    if (md) { c[0] = sp[4 - d] >> md; for (int k = 1; k < 5; ++k) c[k] = (sp[4 + k - d] >> md) | (sp[3 + k - d] << rm); }
    else for (int k = 0; k < 5; ++k) c[k] = sp[4 + k - d];
    unsigned v = 0; for (int n = 0; n < 5; ++n) v |= c[n] & hide[n];
    return v != 0;
}
// b) Brazos: el GSC de cada personaje hace SetViewModel (pentagon: brazos de JFK/McNamara/Nixon/Castro, piel desnuda) y el cliente
//    lo aplica en CG_UpdateHandViewmodels (configstring viewmodelIndex + 1568 -> ChangeViewmodelDobj en cada arma). Ese camino lo
//    dispara el snapshot del cliente, que el harness no tiene, asi que se quedaban las manos por defecto del arma (handXModel =
//    viewhands_usmc, guantes verdes). Aqui se hace lo mismo con el playerState del servidor.  H7C_HANDS=0 lo desactiva.
#include <cgame/cg_local.h>
#include <cgame/cg_weapons.h>
#include <bgame/bg_misc.h>
#include <server_mp/server_mp.h>
void __cdecl SV_GetConfigstring(unsigned int index, char *buffer, int bufferSize);
void H7C_UpdateHands(const playerState_s *ps)
{
    static const bool on = !(getenv("H7C_HANDS") && !atoi(getenv("H7C_HANDS")));
    if (!on || ps->viewmodelIndex <= 0) return;
    char name[256]; name[0] = 0; SV_GetConfigstring(ps->viewmodelIndex + 1568, name, sizeof name);
    if (!name[0]) return;
    XModel *hands = DB_FindXAssetHeader(ASSET_TYPE_XMODEL, name, 1, -1).model;
    if (!hands) return;
    static XModel *said;
    for (int slot = 0; slot < 15; ++slot) {
        const unsigned w = ps->heldWeapons[slot].weapon; if (!w) continue;
        weaponInfo_s *wi = CG_GetLocalClientWeaponInfo(0, w);
        if (!wi->registered || wi->handModel == hands) continue;
        ChangeViewmodelDobj(0, ps, w, BG_GetPlayerWeaponModel(ps, w), hands, wi->gogglesModel, wi->rocketModel, 1, 0);
        if (said != hands) { said = hands; fprintf(stderr, "[H7C] brazos del viewmodel: '%s' (viewmodelIndex %d)\n", name, ps->viewmodelIndex); }
    }
}
// ---- 4b. Modelos estaticos (sillas, mesas, atriles, jarras...). H4 no los dibujaba: el original los anade en
// R_AddAllStaticModelSurfacesCamera (r_add_staticmodel.cpp: distancia de corte cullDist, LOD por XModelGetLodForDist con la escala)
// y los dibuja rigidos con su GfxPackedPlacement (R_TessStaticModelRigidDrawSurfList). Aqui: mismo corte y LOD, los vertices se pasan
// a mundo con el puente de skinning (todas las matrices de hueso = la colocacion, que es inv(base)*base*colocacion) y se dibujan con
// H4_DrawSkinnedSurf; la luz de modelo sale del lightgrid original (H5L_Prepare con GfxStaticModelInst::lightingOrigin) mientras
// queden bloques en el atlas (los actores van primero). Cache por superficie (vertices en mundo) con tope H7C_SMODEL_MB (8 MB).
//   H7C_SMODEL=0 los desactiva; H7C_SMODEL_LOG=1 cuenta.
#include <xanim/xmodel_utils.h>
#include <gfx_d3d/r_model_skin.h>
void H4_DrawSkinnedSurf(Material *m, XSurface *xsurf, GfxPackedVertex *verts, unsigned char techType);
void H4_GetCam(float *org, float *ang);
void H5L_Prepare(int key, const float *lightingOrigin, const float *half, bool hero) __attribute__((weak));
void H5L_PrepareStatic(int key, const float *lightingOrigin, int primaryLightIndex) __attribute__((weak));   // OLA 7e (h5_light.cpp)
int H5L_MaxKey() __attribute__((weak));
static bool H7E_SmLight() { static const bool on = !(getenv("H7E_SMLIGHT") && !atoi(getenv("H7E_SMLIGHT"))); return on; }
void H5L_Bind(int key) __attribute__((weak));
extern void (*g_h5lPrepareAll)() __attribute__((weak));
static bool s_smOn = true;
static std::vector<int> s_smVis;   // visibles en el frame (los rellena el PrepareAll envuelto, una vez por frame)
static void (*s_prevPrepareAll)();
static std::map<std::pair<int, int>, std::vector<GfxPackedVertex>> s_smCache; static size_t s_smCacheBytes;
static void SModelVisible()
{
    s_smVis.clear();
    static const float maxD = getenv("H7C_SMODEL_DIST") ? (float)atof(getenv("H7C_SMODEL_DIST")) : 1200.0f;
    float cam[3], ang[3], fwd[3]; H4_GetCam(cam, ang); AngleVectors(ang, fwd, nullptr, nullptr);
    const GfxStaticModelDrawInst *di = s_w->dpvs.smodelDrawInsts.get(); const GfxStaticModelInst *si = s_w->dpvs.smodelInsts.get();
    if (!di || !si) return;
    for (unsigned i = 0; i < s_w->dpvs.smodelCount; ++i) {
        const float *mn = si[i].mins, *mx = si[i].maxs;
        float c[3], r2 = 0; for (int k = 0; k < 3; ++k) { c[k] = 0.5f * (mn[k] + mx[k]) - cam[k]; const float h = 0.5f * (mx[k] - mn[k]); r2 += h * h; }
        const float d = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), rad = sqrtf(r2);
        if (di[i].cullDist > 0 && d - rad > di[i].cullDist) continue;
        if (maxD > 0 && d - rad > maxD) continue;   // H4 no tiene portales (dpvs): tope de distancia en vez de la visibilidad por celdas
        if (d > rad && (c[0] * fwd[0] + c[1] * fwd[1] + c[2] * fwd[2]) < s_smFovCos * d - rad) continue;   // fuera del cono de vision
        s_smVis.push_back((int)i);
    }
}
static void PrepareAllWithSModels()
{
    if (s_prevPrepareAll) s_prevPrepareAll();
    { extern void H7E_PrepareClientModels() __attribute__((weak)); if (H7E_PrepareClientModels) H7E_PrepareClientModels(); }   // OLA 7e: modelos de cliente
    { extern void H15F_PrepareDynEnts() __attribute__((weak)); if (H15F_PrepareDynEnts) H15F_PrepareDynEnts(); }   // OLA 15F: DynEnts (h15f_dynent.cpp)
    if (!s_smOn || !s_w) return;
    SModelVisible();
    if (!H5L_Prepare) return;
    static const float half[3] = { 16, 16, 16 };
    const GfxStaticModelInst *si = s_w->dpvs.smodelInsts.get();
    // el atlas tiene 64 bloques por frame y los actores van antes: solo los H7C_SMODEL_LIT (24) mas cercanos piden bloque, para no
    // desalojar entradas cada frame (recalculo continuo del lightgrid); el resto usa la luz neutra
    static const int lit = getenv("H7C_SMODEL_LIT") ? atoi(getenv("H7C_SMODEL_LIT")) : 24;
    float cam[3], ang[3]; H4_GetCam(cam, ang);
    std::vector<std::pair<float, int>> byDist; byDist.reserve(s_smVis.size());
    if (H7E_SmLight() && H5L_PrepareStatic && H5L_MaxKey) {
        // OLA 7e: como R_SetStaticModelLighting: cada smodel visible con su luz (lightgrid en lightingOrigin + SU primaria precalculada,
        // GfxStaticModelDrawInst::primaryLightIndex), cacheada mientras siga visible. Antes: solo los 24 mas cercanos y con indice < 1023
        // (las sillas, 44xx, caian siempre en la luz neutra 0x80 y salian salmon en vez de amarillas). H7E_SMLIGHT=0 vuelve a lo de antes.
        const GfxStaticModelDrawInst *di = s_w->dpvs.smodelDrawInsts.get(); const int maxKey = H5L_MaxKey();
        for (int i : s_smVis) if (1024 + i <= maxKey) H5L_PrepareStatic(1024 + i, si[i].lightingOrigin, di[i].primaryLightIndex);
        return;
    }
    for (int i : s_smVis) if (1024 + i < 2047) byDist.push_back({ Vec3DistanceSq(si[i].lightingOrigin, cam), i });
    const size_t n = std::min(byDist.size(), (size_t)std::max(lit, 0));
    std::partial_sort(byDist.begin(), byDist.begin() + n, byDist.end());
    for (size_t k = 0; k < n; ++k) H5L_Prepare(1024 + byDist[k].second, si[byDist[k].second].lightingOrigin, half, false);
}
static void *H7C_Aligned(size_t n) { void *p = nullptr; return posix_memalign(&p, 16, (n + 15) & ~(size_t)15) ? nullptr : p; }   // aligned_alloc no existe con la API 26 del NDK
static void DrawSModels(unsigned char techType)
{
    static const size_t capBytes = (size_t)(getenv("H7C_SMODEL_MB") ? atof(getenv("H7C_SMODEL_MB")) : 8.0) * 1048576;
    static GfxPackedVertex *tmp; static int tmpCap;
    static DObjSkelMat *skel = (DObjSkelMat *)H7C_Aligned(128 * sizeof(DObjSkelMat));
    float cam[3], ang[3]; H4_GetCam(cam, ang);
    const GfxStaticModelDrawInst *di = s_w->dpvs.smodelDrawInsts.get();
    int drawn = 0, surfs = 0;
    for (int i : s_smVis) {
        const GfxStaticModelDrawInst &in = di[i]; XModel *model = in.model.get(); if (!model) continue;
        const float sc = in.placement.scale >= 1.0f ? in.placement.scale : 1.0f;
        const float dist = Vec3Distance(in.placement.origin, cam);
        const int lod = XModelGetLodForDist(model, dist / sc, dist / sc, false); if (lod < 0) continue;
        const int nb = XModelNumBones(model); if (nb > 128) continue;
        XSurface *xsurfs; const int ns = XModelGetSurfaces(model, &xsurfs, lod);
        PTR32(Material) *skins = XModelGetSkins(model, lod);
        bool built = false;
        if (H5L_Bind) H5L_Bind(H7E_SmLight() && H5L_MaxKey ? (1024 + i <= H5L_MaxKey() ? 1024 + i : -2) : (1024 + i < 2047 ? 1024 + i : 2046));
        for (int s = 0; s < ns; ++s) {
            XSurface *xs = &xsurfs[s]; Material *mtl = skins ? skins[s].get() : nullptr;
            if (!mtl || !xs->vertCount) continue;
            auto key = std::make_pair(i, lod * 256 + s); auto it = s_smCache.find(key);
            GfxPackedVertex *v;
            if (it != s_smCache.end()) v = it->second.data();
            else {
                if (!built) { built = true;
                    for (int j = 0; j < nb; ++j) { DObjSkelMat &o = skel[j];
                        for (int a = 0; a < 3; ++a) { for (int b = 0; b < 3; ++b) o.axis[a][b] = in.placement.axis[a][b] * in.placement.scale; o.axis[a][3] = 0; }
                        memcpy(o.origin, in.placement.origin, 12); o.origin[3] = 1.0f; } }
                if ((int)xs->vertCount > tmpCap) { free(tmp); tmpCap = xs->vertCount + 1024; tmp = (GfxPackedVertex *)H7C_Aligned(tmpCap * sizeof(GfxPackedVertex)); }
                R_SkinXSurfaceSkinned(xs, skel, tmp); v = tmp;
                const size_t bytes = xs->vertCount * sizeof(GfxPackedVertex);
                if (s_smCacheBytes + bytes <= capBytes) { s_smCache[key].assign(tmp, tmp + xs->vertCount); s_smCacheBytes += bytes; }
            }
            H4_DrawSkinnedSurf(mtl, xs, v, techType); ++surfs;
        }
        ++drawn;
    }
    static int logged; if (getenv("H7C_SMODEL_LOG") && (++logged % 40) == 0) { fprintf(stderr, "[H7C] smodels: %u en el mapa, %zu visibles, %d dibujados, %d superficies, cache %.1f MB\n", s_w->dpvs.smodelCount, s_smVis.size(), drawn, surfs, s_smCacheBytes / 1048576.0); }
}
#endif
void H7C_DrawWorldExtra(unsigned char techType)
{
#ifdef H5G
    if (s_bmOn && s_w) DrawBModels(techType);
    if (s_smOn && s_w) {
        // engancha el preparado de luz del frame (h5_light.cpp, corre antes del pase); sin el, la visibilidad se calcula aqui
        if (&g_h5lPrepareAll && g_h5lPrepareAll && g_h5lPrepareAll != PrepareAllWithSModels) { s_prevPrepareAll = g_h5lPrepareAll; g_h5lPrepareAll = PrepareAllWithSModels; SModelVisible(); }
        else if (!&g_h5lPrepareAll || !g_h5lPrepareAll) SModelVisible();
        DrawSModels(techType);
    }
    { extern void H7E_DrawClientModels(unsigned char) __attribute__((weak)); if (H7E_DrawClientModels) H7E_DrawClientModels(techType); }   // OLA 7e: script_model de cliente (h7e_light.cpp)
    { extern void H15F_DrawDynEnts(unsigned char) __attribute__((weak)); if (H15F_DrawDynEnts) H15F_DrawDynEnts(techType); }   // OLA 15F: DynEnts de modelo (papeles, carpetas)
#endif
}

// ---- 5. Campo de vision. H4 usaba 90 grados horizontales fijos para cualquier pantalla. El original (CG_CalcFov, cg_camera.cpp):
// tanHalfFovX = tan(cg_fov/2) * 0.75 * aspecto, con cg_fov = 65 (los scripts de zombis hacen SetClientDvar("cg_fov", "65")).
// En el Fold desplegado (2208x1840, 1,2:1) eso son 59,6 grados horizontales: el arma y la sala se ven mas grandes.
//   H7C_FOV=0 deja los 90 de antes; H7C_FOV=<n> usa cg_fov = n.
#include <universal/dvar.h>
#include <math.h>
float H7C_FovX(int W, int H)
{
    const char *e = getenv("H7C_FOV"); if (e && !atof(e)) return 0;
    float fov = e ? (float)atof(e) : 65.0f;
    if (!e) if (const dvar_s *d = Dvar_FindVar("cg_fov")) fov = d->current.value;
    const float t = tanf(fov * 0.5f * (float)M_PI / 180.0f) * 0.75f * (float)W / (float)H;
    const float fx = 2.0f * atanf(t) * 180.0f / (float)M_PI;
    fprintf(stderr, "[H7C] fov: cg_fov %.1f a 4:3 -> %.1f grados horizontales en %dx%d (CG_CalcFov)\n", fov, fx, W, H);
    { const float ty = t * (float)H / (float)W; s_smFovCos = cosf(atanf(sqrtf(t * t + ty * ty))); }   // cono que envuelve el frustum (smodels)
    return fx;
}

void H7C_Setup(GfxWorld *w)
{
    s_w = w;
    { const char *e = getenv("H7C_BMODEL"); s_bmOn = !(e && !atoi(e)); }
#ifdef H5G
    { const char *e = getenv("H7C_SMODEL"); s_smOn = !(e && !atoi(e)); }
#endif
    H7C_Diag(w);
}

// ---------------------------------------------------------------------------------------------
// OLA 7c: niebla del original. H4 ponia FOG/FOG2 a cero. El original: setVolFog (GSC) -> CS 10 ->
// CG_ParseFog -> R_SetFogFromServer/R_SwitchFog (rg.clientFogs) -> data->fogSettings -> R_SetFrameFog.
// Aqui se lee el mismo CS 10 con el mismo orden de campos, se rellena el GfxFog como R_SetFogFromServer
// (maxDensity = 100*density, transicion terminada) y se llama al R_SetFrameFog original. H7C_FOG=0 lo apaga.
#ifdef H5G
#include <gfx_d3d/rb_fog.h>
void H7C_Fog(GfxCmdBufInput *in, const float *org)
{
    static const bool on = !(getenv("H7C_FOG") && !atoi(getenv("H7C_FOG")));
    if (!on || !in || !in->data) return;
    static char last[512]; static GfxFog fog; static bool have = false;
    char cs[512]; SV_GetConfigstring(10, cs, sizeof cs);
    if (strcmp(cs, last)) {
        strncpy(last, cs, sizeof last - 1);
        float v[18] = {0}; int n = 0; const char *p = cs;
        while (*p && n < 18) { char *e; float f = strtof(p, &e); if (e == p) break; v[n++] = f; p = e; while (*p == ' ') ++p; }
        have = n >= 17;
        memset(&fog, 0, sizeof fog);
        if (have) {   // start density heightDensity baseHeight r g b transition colorScale sunR sunG sunB dirX dirY dirZ startAng endAng maxOpacity
            fog.fogStart = v[0]; fog.density = v[1]; fog.heightDensity = v[2]; fog.baseHeight = v[3];
            fog.color[0] = v[4]; fog.color[1] = v[5]; fog.color[2] = v[6]; fog.color[3] = v[8];
            fog.sunFogColor[0] = v[9]; fog.sunFogColor[1] = v[10]; fog.sunFogColor[2] = v[11];
            fog.sunFogDir[0] = v[12]; fog.sunFogDir[1] = v[13]; fog.sunFogDir[2] = v[14];
            fog.sunFogStartAng = v[15]; fog.sunFogEndAng = v[16]; fog.sunFogColor[3] = n > 17 ? v[17] : 1.0f;
            fog.maxDensity = 100.0f * fog.density;
        }
        fprintf(stderr, "[H7C] niebla CS10 (%d campos): \"%.200s\"\n", n, cs);
    }
    if (!have) return;
    memcpy((void *)&in->data->fogSettings, &fog, sizeof fog);   // data es const en GfxCmdBufInput; el original lo escribe el front-end
    static GfxViewInfo *vi = (GfxViewInfo *)calloc(1, sizeof(GfxViewInfo));
    memcpy(vi->cullViewInfo.viewParms.origin, org, 12);
    R_SetFrameFog(in, vi);
}
#endif

// ---------------------------------------------------------------------------------------------
// OLA 7c, punto 2: HUD con el arte original de zombies. El HUD de verdad son menus de UI con ownerdraws (Ola 9, sin portar); mientras,
// los mismos valores se dibujan sobre los materiales originales de la zona, colocados como en la captura de referencia (shot14):
// puntos sobre scorebar_zom_1 (mancha roja), municion sobre hud_dpad_blood, circulo hud_dpad, ronda con hud_chalk_1..5 (tiza roja;
// numero en rojo a partir de 6). Unidades: alto de pantalla = 720, anclado abajo a la derecha / izquierda.
//   H7C_HUD=0 vuelve al HUD minimo de H5f. La linea de depuracion (salud/zombis) solo sale con H7C_HUD_DEBUG=1 (en h5_play.cpp).
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_font.h>
bool H7C_DrawHud(int W, int H, int round, int score, int clip, int stock)
{
    static const bool on = !(getenv("H7C_HUD") && !atoi(getenv("H7C_HUD")));
    if (!on) return false;
    static Font_s *font, *big; static Material *scoreBar, *dpadBlood, *dpad, *chalk[5]; static bool init;
    if (!init) { init = true;
        font = R_RegisterFont("fonts/normalFont", 3); big = R_RegisterFont("fonts/bigFont", 3); if (!big) big = font;
        scoreBar = Material_RegisterHandle("scorebar_zom_1", 3); dpadBlood = Material_RegisterHandle("hud_dpad_blood", 3); dpad = Material_RegisterHandle("hud_dpad", 3);
        for (int i = 0; i < 5; ++i) { char n[32]; snprintf(n, sizeof n, "hud_chalk_%d", i + 1); chalk[i] = Material_RegisterHandle(n, 3); }
        fprintf(stderr, "[H7C] HUD original: scorebar %p dpad_blood %p dpad %p chalk %p font %p\n", (void *)scoreBar, (void *)dpadBlood, (void *)dpad, (void *)chalk[0], (void *)font);
    }
    if (!font) return false;
    const float u = (float)H / 720.0f, R = (float)W, B = (float)H;
    const float white[4] = { 1, 1, 1, 1 }, ammoCol[4] = { 0.78f, 0.88f, 0.95f, 1 }, red[4] = { 0.62f, 0.04f, 0.03f, 1 }, blood[4] = { 0.55f, 0.02f, 0.02f, 1 }, ring[4] = { 0.35f, 0.35f, 0.38f, 0.55f };   // los menus tinen las manchas (forecolor)
    char b[32];
    // municion: mancha de sangre del dpad y circulo (debajo de los numeros)
    if (dpadBlood) R_AddCmdDrawStretchPic(R - 205 * u, B - 128 * u, 203 * u, 121 * u, 0, 0, 1, 1, blood, dpadBlood);
    if (dpad) R_AddCmdDrawStretchPic(R - 109 * u, B - 120 * u, 106 * u, 106 * u, 0, 0, 1, 1, ring, dpad);
    // puntos
    if (scoreBar) R_AddCmdDrawStretchPic(R - 135 * u, B - 165 * u, 129 * u, 33 * u, 0, 0, 1, 1, blood, scoreBar);
    snprintf(b, sizeof b, "%d", score);
    R_AddCmdDrawText(b, 0x7fffffff, font, R - 131 * u, B - 137 * u, 0.85f * u, 0.85f * u, 0, white, 3);
    // municion: cargador grande, reserva pequena
    snprintf(b, sizeof b, "%d", clip);
    R_AddCmdDrawText(b, 0x7fffffff, big, R - 172 * u, B - 36 * u, 0.72f * u, 0.72f * u, 0, ammoCol, 3);
    snprintf(b, sizeof b, "/ %d", stock);
    R_AddCmdDrawText(b, 0x7fffffff, font, R - 172 * u + (clip >= 10 ? 32 : 19) * u, B - 36 * u, 0.62f * u, 0.62f * u, 0, ammoCol, 3);
    // ronda: tiza (abajo a la izquierda)
    if (round >= 1 && round <= 5 && chalk[round - 1]) R_AddCmdDrawStretchPic(18 * u, B - 92 * u, 80 * u, 80 * u, 0, 0, 1, 1, red, chalk[round - 1]);
    else if (round > 5) { snprintf(b, sizeof b, "%d", round); R_AddCmdDrawText(b, 0x7fffffff, big, 22 * u, B - 22 * u, 1.6f * u, 1.6f * u, 0, red, 3); }
    return true;
}
