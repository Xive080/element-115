// OLA S1: script_model (y demas entidades con modelo) del SERVIDOR GSC dibujados en el cliente, comun a todos los mapas.
//
// En el original el cliente recibe cada entidad por el snapshot y la dibuja en CG_ScriptMover / CG_General (cg_ents_mp.cpp):
//   - no llega al cliente si no esta enlazada (SV_LinkEntity), con SVF_NOCLIENT (svFlags & 1) o con su bit en clientMask
//     (setinvisibletoplayer), como sv_snapshot_mp.cpp;
//   - EF_NODRAW (eFlags & 0x20: Hide) no se dibuja; solid == SOLID_BMODEL es un brushmodel (lo dibuja h7c_gfx.cpp DrawBModels);
//     ET_SCRIPTMOVER con eFlags & 0x200 tampoco;
//   - el resto: R_AddDObjToScene(DObj de gameModels[index.brushmodel] + modelos atados) con lightingOrigin = centro de
//     pose.absmin/absmax (CG_GetLightingOrigin).
// Aqui el DObj es el del servidor (G_DObjUpdate lo arma en SetModel / Attach / Detach con los mismos modelos que el cliente), la
// pose sale de G_DObjCalcPose (base, o la animacion del servidor si tiene UseAnimTree/SetAnim) y la colocacion de r.currentOrigin /
// r.currentAngles, que el servidor ya mueve con MoveTo/RotateTo (G_RunMover) y LinkTo (G_GeneralLink). SetModel, Show/Hide y Delete
// se ven solos: cambian el DObj, eFlags o inuse. Sin cg_entities no hay interpolacion entre snapshots (el servidor corre a 20 Hz).
// Se deforman en CPU como los actores (h5_game.cpp H5V_DrawActors: base pose inversa * hueso * entidad, R_SkinXSurfaceSkinned) con LOD
// por distancia (XModelGetLodForDist, como R_SkinXModel) y partes ocultas (HideTags / useweaponhidetags: H7C_SurfHidden). Los vertices
// en mundo se guardan por entidad y solo se recalculan cuando cambia la pose (modelos, LOD, origen/angulos o huesos): una maquina de
// ventajas quieta no cuesta skinning. Luz de modelo del lightgrid en el lightingOrigin (H5L_Prepare/H5L_Bind con la clave = entnum).
// Tambien proyectan sombra de los focos (h9_shadow: H5V_DrawActorsShadow llama a H20_DrawShadow).
//   H20_SVMODELS=0 lo apaga.   H20_LOG=1 lista por cambio lo que se dibuja ([H20]).   H20_TRACE=ms (prueba): a ese level.time traza
//   desde el ojo del jugador al centro de cada modelo visible con la mascara de bala (G_LocationalTraceAllowChildren 0x280E833, como
//   Bullet_Fire) y dice que entidad toca.
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <server_mp/server_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <clientscript/cscr_stringlist.h>
#include <xanim/xanim.h>
#include <xanim/dobj.h>
#include <xanim/dobj_utils.h>
#include <xanim/xmodel.h>
#include <xanim/xmodel_utils.h>
#include <qcommon/dobj_management.h>
#include <EffectsCore/fx_marks.h>
#include <gfx_d3d/r_model_skin.h>
#include <gfx_d3d/r_dobj_skin.h>
#include <server/sv_world.h>
#include <game_mp/g_combat_mp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <vector>
#include <string>
#include <algorithm>
#include <game_mp/g_misc_mp.h>
#include "world_h4.h"

void H5L_Prepare(int key, const float *lightingOrigin, const float *half, bool hero) __attribute__((weak));
void H5L_Bind(int key) __attribute__((weak));
bool H7C_SurfHidden(const DObj *, const XSurface *, int) __attribute__((weak));
extern void (*g_h5lPrepareAll)();
extern bool g_h5eViewResync, g_h5eShotReq;   // h5_weapon.cpp (guion H5_TIMED)
extern float actorLocationalMins[3], actorLocationalMaxs[3];   // cg_world (fuera del namespace anonimo: si no, genstubs le pone un stub)

namespace {
const int kMaxBones = 256, kMaxSurfs = 128;
struct Cache {
    int use = -1;                     // s.lerp.useCount de la entidad (entidad nueva en el mismo numero = cache nueva)
    std::vector<XModel *> models; std::vector<int> lods;
    std::vector<DObjAnimMat> mats; float org[3] = { 0, 0, 0 }, ang[3] = { 0, 0, 0 }; unsigned hide[5] = { 0, 0, 0, 0, 0 };
    std::vector<std::vector<GfxPackedVertex>> verts;   // por superficie de todos los modelos, en mundo
    std::vector<XSurface *> surf; std::vector<Material *> mtl; std::vector<unsigned char> hidden;
    bool valid = false; unsigned lastFrame = 0;
};
Cache s_c[MAX_GENTITIES];
std::vector<int> s_vis;               // entidades a dibujar este frame (las decide Prepare, antes del pase)
bool s_on, s_init; unsigned s_frame; void (*s_prevPrepare)(); void (*s_prevDraw)(unsigned char);
int s_log;
DObjSkelMat s_skel[kMaxBones] __attribute__((aligned(16)));
GfxPackedVertex *s_tmp; unsigned s_tmpCap;
size_t s_bytes;                       // memoria de vertices en cache
int s_skinned, s_drawn;

const char *Sl(unsigned s) { return s ? SL_ConvertToString(s, SCRIPTINSTANCE_SERVER) : ""; }

// Como el snapshot + CG_ScriptMover/CG_General: hay que dibujarla?
bool Wanted(const gentity_s *e)
{
    if (!e->r.inuse || !e->r.linked || (e->r.svFlags & 1) || (e->r.clientMask[0] & 1)) return false;
    const int t = e->s.eType;
    if (t != ET_SCRIPTMOVER && t != ET_GENERAL && t != ET_PLANE) return false;
    if (e->s.solid == 0xFFFFFF || e->r.bmodel) return false;            // brushmodel: h7c_gfx.cpp
    if (e->s.lerp.eFlags & 0x20) return false;                           // EF_NODRAW (Hide)
    if (t == ET_SCRIPTMOVER && (e->s.lerp.eFlags & 0x200)) return false;
    if (!e->model) return false;                                         // script_origin y demas sin modelo
    return Com_GetServerDObj(e->s.number) != nullptr;
}

// CG_CalcWorldBounds (cg_world.cpp), rama de modelo: limites del DObj girados con los angulos de la entidad (los r.absmin/absmax del
// servidor son una caja de radio centrada en el origen y no sirven de lightingOrigin)
void WorldBounds(gentity_s *e, float *absmin, float *absmax)
{
    DObj *obj = Com_GetServerDObj(e->s.number);
    if (e->s.surfType == 7 && !DObjHasCollmap(obj)) {   // "character" de CG_CalcWorldBounds
        for (int k = 0; k < 3; ++k) { absmin[k] = e->r.currentOrigin[k] + actorLocationalMins[k]; absmax[k] = e->r.currentOrigin[k] + actorLocationalMaxs[k]; }
        return; }
    float ax[3][3], mn[3], mx[3]; AnglesToAxis(e->r.currentAngles, ax); DObjCalcBounds(obj, mn, mx);
    for (int k = 0; k < 3; ++k) { absmin[k] = FLT_MAX; absmax[k] = -FLT_MAX; }
    for (int k = 0; k < 8; ++k) { const float v[3] = { (k & 1) ? mn[0] : mx[0], (k & 2) ? mn[1] : mx[1], (k & 4) ? mn[2] : mx[2] }; float w[3]; MatrixTransformVector(v, ax, w);
        for (int j = 0; j < 3; ++j) { if (w[j] < absmin[j]) absmin[j] = w[j]; if (w[j] > absmax[j]) absmax[j] = w[j]; } }
    for (int k = 0; k < 3; ++k) { absmin[k] += e->r.currentOrigin[k]; absmax[k] += e->r.currentOrigin[k]; }
}
float s_bmin[MAX_GENTITIES][3], s_bmax[MAX_GENTITIES][3];   // limites de mundo del frame (Prepare)

void Prepare()
{
    if (s_prevPrepare) s_prevPrepare();
    s_vis.clear(); ++s_frame;
    if (!s_on) return;
    float cam[3], ang[3], fwd[3]; H4_GetCam(cam, ang); AngleVectors(ang, fwd, nullptr, nullptr);
    for (int n = 0; n < level.num_entities && n < MAX_GENTITIES; ++n) {
        gentity_s *e = &g_entities[n];
        if (s_log >= 2 && e->r.inuse && e->s.eType == ET_SCRIPTMOVER) {   // H20_LOG=2: estado de cada script mover por cambio (por que no se dibuja)
            static std::vector<std::string> last(MAX_GENTITIES); char t[256];
            snprintf(t, sizeof t, "linked %d svf 0x%x mask %x eFl 0x%x solid 0x%x model %d dobj %d", (int)e->r.linked, e->r.svFlags, e->r.clientMask[0], e->s.lerp.eFlags, e->s.solid, e->model, Com_GetServerDObj(n) ? 1 : 0);
            if (last[n] != t) { last[n] = t; fprintf(stderr, "[H20] t=%d mover %d %s tn=%s: %s -> %s\n", level.time, n, Sl(e->classname), Sl(e->targetname), t, Wanted(e) ? "se dibuja" : "no"); } }
        if (!Wanted(e)) continue;
        // cono de vision amplio con la esfera de los limites (R_CullDObj recorta por el frustum; aqui un cono que cubre el FOV del port)
        WorldBounds(e, s_bmin[n], s_bmax[n]);
        float c[3], half[3]; for (int k = 0; k < 3; ++k) { c[k] = (s_bmin[n][k] + s_bmax[n][k]) * 0.5f; half[k] = (s_bmax[n][k] - s_bmin[n][k]) * 0.5f; }
        const float r = sqrtf(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]) + 8.0f;
        const float dx = c[0] - cam[0], dy = c[1] - cam[1], dz = c[2] - cam[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > r && dx * fwd[0] + dy * fwd[1] + dz * fwd[2] < 0.3f * d - r) continue;
        s_vis.push_back(n);
        if (H5L_Prepare) { const float hm[3] = { half[0] > 1 ? half[0] : 1, half[1] > 1 ? half[1] : 1, half[2] > 1 ? half[2] : 1 }; H5L_Prepare(n, c, hm, false); }   // CG_GetLightingOrigin
    }
}

// Pose actual de la entidad y, si cambio, vertices en mundo de todas sus superficies
bool Update(int n, const float *cam)
{
    gentity_s *e = &g_entities[n]; Cache &c = s_c[n];
    DObj *obj = Com_GetServerDObj(n); if (!obj) return false;
    if (c.use != e->s.lerp.useCount) { c = Cache(); c.use = e->s.lerp.useCount; }
    c.lastFrame = s_frame;
    int partBits[5] = { -1, -1, -1, -1, -1 };
    { bgs_t *const prev = bgs; bgs = &level_bgs; G_DObjCalcPose(e, partBits); bgs = prev; }   // bgs es nulo fuera de G_RunFrame
    const DObjAnimMat *mats = DObjGetRotTransArray(obj); if (!mats) return false;
    const int nm = DObjGetNumModels(obj);
    int nbTot = 0; for (int m = 0; m < nm; ++m) nbTot += XModelNumBones(DObjGetModel(obj, m));
    if (nbTot > kMaxBones) return false;
    const float dist = Vec3Distance(cam, e->r.currentOrigin);
    unsigned hide[5]; DObjGetHidePartBits(obj, hide);
    bool same = c.valid && (int)c.models.size() == nm && (int)c.mats.size() == nbTot && !memcmp(c.org, e->r.currentOrigin, 12) && !memcmp(c.ang, e->r.currentAngles, 12)
        && !memcmp(c.hide, hide, sizeof hide) && !memcmp(c.mats.data(), mats, nbTot * sizeof(DObjAnimMat));
    std::vector<int> lods(nm);
    for (int m = 0; m < nm; ++m) { XModel *xm = DObjGetModel(obj, m); lods[m] = XModelGetLodForDist(xm, dist, dist, false);
        if (same && (c.models[m] != xm || c.lods[m] != lods[m])) same = false; }
    if (same) return true;
    // pose nueva: matrices de deformado como R_SkinXModelCmd (base pose inversa * hueso * entidad) y vertices en mundo
    DObjSkelMat entMat; memset(&entMat, 0, sizeof entMat);
    { float axis[3][3]; AnglesToAxis(e->r.currentAngles, axis);
      for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) entMat.axis[i][j] = axis[i][j];
      for (int j = 0; j < 3; ++j) entMat.origin[j] = e->r.currentOrigin[j]; entMat.origin[3] = 1.0f; }
    for (auto &v : c.verts) s_bytes -= v.capacity() * sizeof(GfxPackedVertex);
    c.verts.clear(); c.surf.clear(); c.mtl.clear(); c.hidden.clear();
    int boneIndex = 0;
    for (int m = 0; m < nm; ++m) {
        XModel *xm = DObjGetModel(obj, m); const int nb = XModelNumBones(xm); const DObjAnimMat *base = XModelGetBasePose(xm);
        for (int j = 0; j < nb; ++j) {
            DObjSkelMat inv, bone, bw; ConvertQuatToInverseSkelMat(&base[j], &inv); ConvertQuatToSkelMat(&mats[boneIndex + j], &bone);
            R_MultiplySkelMat(&inv, &bone, &bw); R_MultiplySkelMat(&bw, &entMat, &s_skel[boneIndex + j]);
            DObjSkelMat &o = s_skel[boneIndex + j]; o.axis[0][3] = o.axis[1][3] = o.axis[2][3] = 0.0f; o.origin[3] = 1.0f;
        }
        if (lods[m] >= 0) {
            XSurface *xs; const int ns = XModelGetSurfaces(xm, &xs, lods[m]); PTR32(Material) *skins = XModelGetSkins(xm, lods[m]);
            for (int s = 0; s < ns && (int)c.surf.size() < kMaxSurfs; ++s) {
                Material *mt = skins ? skins[s].get() : nullptr; if (!mt || !xs[s].vertCount) continue;
                if (xs[s].vertCount > s_tmpCap) { free(s_tmp); s_tmpCap = xs[s].vertCount + 1024; if (posix_memalign((void **)&s_tmp, 16, s_tmpCap * sizeof(GfxPackedVertex))) { s_tmp = nullptr; s_tmpCap = 0; return false; } }
                R_SkinXSurfaceSkinned(&xs[s], &s_skel[boneIndex], s_tmp); ++s_skinned;
                c.verts.emplace_back(s_tmp, s_tmp + xs[s].vertCount); s_bytes += c.verts.back().capacity() * sizeof(GfxPackedVertex);
                c.surf.push_back(&xs[s]); c.mtl.push_back(mt);
                c.hidden.push_back(H7C_SurfHidden && H7C_SurfHidden(obj, &xs[s], boneIndex) ? 1 : 0);
            }
        }
        boneIndex += nb;
    }
    c.models.resize(nm); for (int m = 0; m < nm; ++m) c.models[m] = DObjGetModel(obj, m);
    c.lods = lods; c.mats.assign(mats, mats + nbTot); memcpy(c.org, e->r.currentOrigin, 12); memcpy(c.ang, e->r.currentAngles, 12); memcpy(c.hide, hide, sizeof hide);
    c.valid = true;
    if (s_log) {
        std::string ms; for (int m = 0; m < nm; ++m) { ms += m ? "+" : ""; ms += DObjGetModel(obj, m)->name.get(); }
        static std::vector<std::string> last(MAX_GENTITIES);
        char t[512]; snprintf(t, sizeof t, "%s lod%d", ms.c_str(), nm ? lods[0] : -1);
        if (last[n] != t) { last[n] = t;
            fprintf(stderr, "[H20] t=%d ent %d tipo %d cn=%s tn=%s modelo %s en (%.0f %.0f %.0f) ang (%.0f %.0f %.0f) superficies %zu contents 0x%x\n", level.time, n, e->s.eType,
                Sl(e->classname), Sl(e->targetname), t, e->r.currentOrigin[0], e->r.currentOrigin[1], e->r.currentOrigin[2], e->r.currentAngles[0], e->r.currentAngles[1], e->r.currentAngles[2], c.surf.size(), e->r.contents); }
    }
    return true;
}

void DrawOne(int n, unsigned char techType)
{
    Cache &c = s_c[n];
    for (size_t s = 0; s < c.surf.size(); ++s) if (!c.hidden[s]) { H4_DrawSkinnedSurf(c.mtl[s], c.surf[s], c.verts[s].data(), techType); ++s_drawn; }
}

// Vertices de entidades que ya no se ven en 10 s: fuera (tope de memoria)
void Evict()
{
    if (s_frame % 64) return;
    for (int n = 0; n < MAX_GENTITIES; ++n) { Cache &c = s_c[n]; if (!c.valid || s_frame - c.lastFrame < 600) continue;
        for (auto &v : c.verts) s_bytes -= v.capacity() * sizeof(GfxPackedVertex); c = Cache(); }
}

void TraceTest()
{
    // dos trazas por modelo con la mascara de bala: desde el ojo (con lo que tape el mundo) y "de cerca" (desde fuera de sus limites, en la
    // direccion del ojo, hasta atravesar el centro): esta mide solo la colision del propio modelo (collSurfs del XModel, DObjGeomTraceline)
    static int at = getenv("H20_TRACE") ? atoi(getenv("H20_TRACE")) : -1; if (at < 0 || level.time < at) return; at = -1;
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return;
    float eye[3] = { pl->client->ps.origin[0], pl->client->ps.origin[1], pl->client->ps.origin[2] + pl->client->ps.viewHeightCurrent };
    int hitsEye = 0, hitsNear = 0, hitsSolo = 0, tried = 0, withColl = 0;
    for (int n = 0; n < level.num_entities; ++n) { gentity_s *e = &g_entities[n]; if (!Wanted(e)) continue;
        float bmn[3], bmx[3]; WorldBounds(e, bmn, bmx);
        float c[3], h2 = 0; for (int k = 0; k < 3; ++k) { c[k] = (bmn[k] + bmx[k]) * 0.5f; const float h = (bmx[k] - bmn[k]) * 0.5f; h2 += h * h; }
        const float dist = Vec3Distance(c, eye); if (dist > 3000.0f || dist < 1.0f) continue; ++tried;
        DObj *obj = Com_GetServerDObj(n); const int ncs = obj ? (int)DObjGetModel(obj, 0)->numCollSurfs : 0; withColl += ncs > 0;
        float end[3], ns[3], ne[3]; const float r = sqrtf(h2) + 4.0f;
        for (int k = 0; k < 3; ++k) { const float d = (c[k] - eye[k]) / dist; end[k] = eye[k] + (c[k] - eye[k]) * 1.5f; ns[k] = c[k] - d * r; ne[k] = c[k] + d * r; }
        trace_t tr, tn; memset(&tr, 0, sizeof tr); memset(&tn, 0, sizeof tn);
        G_LocationalTraceAllowChildren(&tr, eye, end, 0, 0x280E833, bulletPriorityMap);
        G_LocationalTraceAllowChildren(&tn, ns, ne, 0, 0x280E833, bulletPriorityMap);
        const int hit = tr.hitType == TRACE_HITTYPE_ENTITY ? (int)tr.hitId : -1, hitN = tn.hitType == TRACE_HITTYPE_ENTITY ? (int)tn.hitId : -1;
        hitsEye += hit == n; hitsNear += hitN == n;
        // la misma prueba que SV_TracePointToEntity hace con esta entidad sola (sin el mundo): ejes de la entidad + DObjGeomTraceline
        float fr = 1.0f;
        if (obj && (e->r.svFlags & 4) && DObjHasContents(obj, 0x280E833)) {
            float ax[4][3], ls[3], le[3]; AnglesToAxis(e->r.currentAngles, ax); memcpy(ax[3], e->r.currentOrigin, 12);
            MatrixTransposeTransformVector43(ns, ax, ls); MatrixTransposeTransformVector43(ne, ax, le);
            int pb[5]; DObjGeomTracelinePartBits(obj, 0x280E833, pb); { bgs_t *const prev = bgs; bgs = &level_bgs; G_DObjCalcPose(e, pb); bgs = prev; }
            DObjTrace_s ot; memset(&ot, 0, sizeof ot); ot.fraction = 1.0f; DObjGeomTraceline(obj, ls, le, 0x280E833, &ot); fr = ot.fraction;
            if (fr >= 1.0f && getenv("H20_TRACE_V")) { const XModel *xm = DObjGetModel(obj, 0);
                fprintf(stderr, "[H20]    ent %d sin impacto: collLod %d, local (%.1f %.1f %.1f) -> (%.1f %.1f %.1f), limites (%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n", n, (int)xm->collLod, ls[0], ls[1], ls[2], le[0], le[1], le[2],
                    e->r.mins[0], e->r.mins[1], e->r.mins[2], e->r.maxs[0], e->r.maxs[1], e->r.maxs[2]);
                for (unsigned q = 0; q < xm->numCollSurfs && q < 8; ++q) { const XModelCollSurf_s *cs = &xm->collSurfs[q];
                    fprintf(stderr, "[H20]      collSurf %u: tris %d hueso %d contents 0x%x caja (%.1f %.1f %.1f)-(%.1f %.1f %.1f) tri0 plano (%.2f %.2f %.2f %.1f)\n", q, cs->numCollTris, cs->boneIdx, cs->contents,
                        cs->mins[0], cs->mins[1], cs->mins[2], cs->maxs[0], cs->maxs[1], cs->maxs[2], cs->numCollTris ? cs->collTris[0].plane[0] : 0.f, cs->numCollTris ? cs->collTris[0].plane[1] : 0.f, cs->numCollTris ? cs->collTris[0].plane[2] : 0.f, cs->numCollTris ? cs->collTris[0].plane[3] : 0.f); } } }
        hitsSolo += fr < 1.0f;
        fprintf(stderr, "[H20] traza ent %d (%s, tn=%s, contents 0x%x, modelo 0x%x, collSurfs %d) a %.0f u: desde el ojo %s, de cerca %s (fraccion %.3f, tipo %d id %d), el modelo solo %s (%.3f)\n", n, obj ? DObjGetModel(obj, 0)->name.get() : "-", Sl(e->targetname),
            e->r.contents, obj ? DObjGetContents(obj) : 0, ncs, dist, hit == n ? "LE DA" : hit >= 0 && hit < ENTITYNUM_WORLD ? "otra entidad" : tr.fraction < 1 ? "tapada" : "nada",
            hitN == n ? "LE DA" : hitN >= 0 && hitN < ENTITYNUM_WORLD ? "otra entidad" : tn.fraction < 1 ? "mundo" : "nada", tn.fraction, (int)tn.hitType, (int)tn.hitId, fr < 1 ? "LE DA" : "no", fr);
    }
    fprintf(stderr, "[H20] traza: %d modelos a < 3000 u (%d con collSurfs): la bala les da %d desde el ojo y %d de cerca; contra el modelo solo %d\n", tried, withColl, hitsEye, hitsNear, hitsSolo);
}

// H20_TOUR=ms (prueba de capturas, comun a todos los mapas): desde ese level.time, cada H20_TOUR_DT ms (1500) pone al jugador delante del
// siguiente modelo de servidor (uno por nombre de modelo, los H20_TOUR_N = 10 de mayor radio, sin tag_origin) mirando a su centro y pide
// captura a mitad del intervalo (g_h5eShotReq, solo con P4_OUT). El sitio: la direccion libre (traza contra el mundo) mas larga de las 8
// horizontales desde el centro, a 2,2 radios + 48 u. Sale [H20] gira ... con la entidad de cada captura. Mejor con H5_PLAY_SCRIPT=0.
void Tour()
{
    static int at = getenv("H20_TOUR") ? atoi(getenv("H20_TOUR")) : -1; if (at < 0 || level.time < at) return;
    static const int dt = getenv("H20_TOUR_DT") ? atoi(getenv("H20_TOUR_DT")) : 1500, maxN = getenv("H20_TOUR_N") ? atoi(getenv("H20_TOUR_N")) : 10;
    static std::vector<int> list; static bool built; static int idx = -1, next = 0; static bool shotDone = true;
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return;
    if (!built) { built = true; std::vector<std::pair<float, int>> c; std::vector<std::string> seen;
        const std::string match = getenv("H20_TOUR_MATCH") ? std::string(",") + getenv("H20_TOUR_MATCH") + "," : "";   // "a,b": solo modelos/targetname que contengan alguna
        for (int n = 0; n < level.num_entities; ++n) { gentity_s *e = &g_entities[n]; if (!Wanted(e)) continue;
            const XModel *xm = DObjGetModel(Com_GetServerDObj(n), 0); const std::string nm = xm->name.get(); if (nm == "tag_origin") continue;
            if (!match.empty()) { bool ok = false; size_t p0 = 1; while (p0 < match.size()) { const size_t p1 = match.find(',', p0); const std::string w = match.substr(p0, p1 - p0); p0 = p1 + 1;
                if (!w.empty() && (nm.find(w) != std::string::npos || std::string(Sl(e->targetname)).find(w) != std::string::npos)) ok = true; } if (!ok) continue; }
            bool dup = false; for (auto &q : seen) dup |= q == nm; if (dup) continue; seen.push_back(nm);
            float mn[3], mx[3]; WorldBounds(e, mn, mx); float r2 = 0; for (int k = 0; k < 3; ++k) r2 += (mx[k] - mn[k]) * (mx[k] - mn[k]) * 0.25f;
            c.push_back({ sqrtf(r2), n }); }
        std::sort(c.begin(), c.end(), [](const std::pair<float, int> &a, const std::pair<float, int> &b) { return a.first > b.first; });
        for (auto &q : c) if ((int)list.size() < maxN) list.push_back(q.second);
        fprintf(stderr, "[H20] gira: %zu modelos de servidor (de %zu nombres distintos)\n", list.size(), c.size()); next = level.time; }
    if (!shotDone && level.time >= next - dt / 2) { shotDone = true; g_h5eShotReq = true; }
    if (level.time < next) return;
    if (++idx >= (int)list.size()) { at = -1; return; }
    next = level.time + dt; shotDone = false;
    const int n = list[idx]; gentity_s *e = &g_entities[n]; if (!e->r.inuse) return;
    float mn[3], mx[3], c[3], r2 = 0; WorldBounds(e, mn, mx); for (int k = 0; k < 3; ++k) { c[k] = (mn[k] + mx[k]) * 0.5f; r2 += (mx[k] - mn[k]) * (mx[k] - mn[k]) * 0.25f; }
    // sitio: de las 8 direcciones horizontales, la que deja ver el centro (traza de punto desde el centro, sin el propio modelo) mas lejos
    const float want = 2.2f * sqrtf(r2) + 64.0f; float best = -1, bo[3] = { c[0], c[1], c[2] };
    for (int k = 0; k < 8; ++k) { const float a = k * (float)M_PI / 4.0f, d[3] = { cosf(a), sinf(a), 0 };
        const float en[3] = { c[0] + d[0] * want, c[1] + d[1] * want, c[2] };
        trace_t tr; memset(&tr, 0, sizeof tr); col_context_t ctx; const float z3[3] = { 0, 0, 0 };
        G_TraceCapsule(&tr, c, z3, z3, en, n, 0x2810011, &ctx);
        const float got = tr.fraction * want - 20.0f; if (got > best) { best = got; for (int j = 0; j < 3; ++j) bo[j] = c[j] + d[j] * (got > 0 ? got : 0); } }
    { const float hm[3] = { -15, -15, 0 }, hx[3] = { 15, 15, 70 }, from[3] = { bo[0], bo[1], bo[2] }, to[3] = { bo[0], bo[1], bo[2] - 300.0f };
      trace_t tr; memset(&tr, 0, sizeof tr); col_context_t ctx; G_TraceCapsule(&tr, from, hm, hx, to, 0, 0x2810011, &ctx); if (!tr.startsolid) bo[2] = from[2] + (to[2] - from[2]) * tr.fraction + 1.0f; }
    float dir[3] = { c[0] - bo[0], c[1] - bo[1], c[2] - (bo[2] + 60.0f) }, ang[3]; vectoangles(dir, ang); ang[2] = 0;
    TeleportPlayer(pl, bo, ang); g_h5eViewResync = true;
    fprintf(stderr, "[H20] gira %d t=%d: ent %d %s (tn=%s) en (%.0f %.0f %.0f), jugador en (%.0f %.0f %.0f) a %.0f u\n", idx, level.time, n, DObjGetModel(Com_GetServerDObj(n), 0)->name.get(), Sl(e->targetname),
        c[0], c[1], c[2], bo[0], bo[1], bo[2], best);
}

void Draw(unsigned char techType)
{
    if (s_prevDraw) s_prevDraw(techType);
    if (!s_on) return;
    float cam[3], ang[3]; H4_GetCam(cam, ang);
    s_drawn = 0;
    for (int n : s_vis) { if (!Update(n, cam)) continue; if (H5L_Bind) H5L_Bind(n); DrawOne(n, techType); }
    Evict();
    TraceTest();
    Tour();
    static int st = 0; if (s_log && ++st % 600 == 0)
        fprintf(stderr, "[H20] frame %u: %zu modelos de servidor visibles, %d superficies, %d deformadas desde el ultimo aviso, cache %.1f MB\n", s_frame, s_vis.size(), s_drawn, s_skinned, s_bytes / 1048576.0), s_skinned = 0;
}
} // namespace

void H20_Install()
{
    if (s_init) return; s_init = true;
    s_on = !(getenv("H20_SVMODELS") && !atoi(getenv("H20_SVMODELS")));
    s_log = getenv("H20_LOG") ? atoi(getenv("H20_LOG")) : 0;
    if (!s_on) { fprintf(stderr, "[H20] modelos de servidor apagados (H20_SVMODELS=0)\n"); return; }
    s_prevPrepare = g_h5lPrepareAll; g_h5lPrepareAll = Prepare;
    s_prevDraw = g_h4DrawExtra; g_h4DrawExtra = Draw;
    fprintf(stderr, "[H20] modelos de servidor (script_model/ET_GENERAL) en el render: CG_ScriptMover/CG_General con el DObj del servidor\n");
}
// h9_shadow (via H5V_DrawActorsShadow): proyectores de sombra de un foco, con los vertices ya deformados de este frame
void H20_DrawShadow(const float *org, float radius, unsigned char techType)
{
    if (!s_on) return;
    for (int n : s_vis) { const Cache &c = s_c[n]; if (!c.valid || s_frame - c.lastFrame > 1) continue;   // el pase de sombra puede ir antes que el principal
        float d2 = 0, rr = 0;
        for (int k = 0; k < 3; ++k) { const float t = (s_bmin[n][k] + s_bmax[n][k]) * 0.5f - org[k], h = (s_bmax[n][k] - s_bmin[n][k]) * 0.5f; d2 += t * t; rr += h * h; }
        const float lim = radius + sqrtf(rr); if (d2 > lim * lim) continue;
        DrawOne(n, techType); }
}
#else
void H20_Install() {}
void H20_DrawShadow(const float *, float, unsigned char) {}
#endif
