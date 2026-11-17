// OLA 12: cliente ORIGINAL de cristal (glass/glass_client.cpp, glass_renderer.cpp, glass_shard.cpp, glass_allocator.cpp) sobre
// el servidor del harness. En el original el servidor manda el estado de los Glass en cada snapshot
// (GlassesServer::WriteSnapshotToClient) y el cliente lo lee (GlassCl_ReadGameState / GlassCl_ParseSnapshot): agrietar ->
// material agrietado + crackSound; romper -> AddShatterAction (fragmentos GlassShard), shatterShound y FX_MarkGlassDetachAll.
// Aqui se hace ese mismo viaje con un msg_t en memoria (sin red), una vez por frame de servidor.
// Los vertices de los fragmentos los genera GlassCl_GenerateVerts (R_ReserveCodeMeshVerts + R_AddGlassDrawSurf, region 9)
// desde h5_fx.cpp justo antes de subir el codemesh (RB_UpdateDynamicBuffers), como R_ProcessCmd_GenerateGlassVerts.
// Solo entra en el build con H12GLASS=1 (build.sh). En ejecucion: H12_GLASSCL=0 lo apaga.
#include <universal/q_shared.h>
#include <qcommon/msg_mp.h>
#include <game_mp/g_main_mp.h>
#include <glass/glass_server.h>
#include <glass/glass_client.h>
#include <glass/glass_renderer.h>
#include <glass/glass_load_obj.h>
#include <glass/glass.h>
#include <universal/dvar.h>
#include <ui/ui_main.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_drawsurf.h>
#include <gfx_d3d/r_state.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void H4_GetCam(float *org, float *ang);
static void H12_PhysDvars();
static int g_clOn = -1, g_lastSent = -1;
static bool On() { if (g_clOn < 0) { const char *e = getenv("H12_GLASSCL"); g_clOn = (!e || atoi(e)) ? 1 : 0; } return g_clOn != 0; }

void H12_GlassClFrame()
{
    if (!On() || !svGlasses.inited || !svGlasses.numGlasses || !rgp.world) return;   // sin mundo de render (P3_BACKEND=trace) no hay cliente
    if (!clGlasses) {
        // Glasses::workMemory es un puntero de runtime (PtrRt32) que el cargador del port no rellena (basura del .ff). El cliente solo
        // lo guarda en GlassesClient::allocator (sus asignaciones van por Z_Malloc): se deja nulo antes de crear el cliente.
        if (Glasses *gl = GetGlasses()) gl->workMemory = nullptr;
        // GlassRenderer lee ui_mapname (dvar de la UI de cliente, que el harness no registra): se registra con el mapa en curso.
        if (!ui_mapname) ui_mapname = _Dvar_RegisterString("ui_mapname", (char *)Dvar_GetString("mapname"), 0, "Current map name");
        H12_PhysDvars();
        GlassCl_AllocateMemory();
        fprintf(stderr, "[H12GC] cliente de cristal %s: %u cristales\n", clGlasses ? "creado" : "NO creado", svGlasses.numGlasses);
        if (!clGlasses) { g_clOn = 0; return; }
    }
    static unsigned char buf[32768];
    msg_t msg; MSG_Init(&msg, buf, sizeof buf);
    const int since = g_lastSent < 0 ? 0 : g_lastSent;   // 0 = estado completo (gamestate)
    svGlasses.WriteSnapshotToClient(&msg, since);
    MSG_BeginReading(&msg);
    if (g_lastSent < 0) GlassCl_ReadGameState(0, &msg); else GlassCl_ParseSnapshot(0, &msg);
    g_lastSent = level.time;
    // GlassCl_Update (GlassClient::Update) solo hace R_AddGlassBrushToScene de los paneles no rotos: eso ya lo dibuja H8_DrawGlass
    // (h8_doors.cpp) con el mismo material; los fragmentos se actualizan en GlassRenderer::Update desde GlassCl_GenerateVerts.
}

void H12_GlassGenerateVerts()
{
    if (!On() || !clGlasses) return;
    // ShardGroup::GenerateVerts elige el LOD de los fragmentos por la distancia a rg.viewOrg (R_SetupViewParms en el original);
    // H4 no la rellena: se le da la camara del frame.
    float org[3], ang[3]; H4_GetCam(org, ang); rg.viewOrg[0] = org[0]; rg.viewOrg[1] = org[1]; rg.viewOrg[2] = org[2];
    // R_ReserveCodeMeshVerts exige g_processCodeMesh: se abre la ventana del codemesh como hace FX_GenerateVerts (los fragmentos van
    // detras de los FX del frame en el mismo codemesh, que h5_fx sube justo despues)
    R_BeginCodeMeshVerts();
    GlassCl_GenerateVerts(0, 0);
    R_EndCodeMeshVerts();
    static const int diag = getenv("H12_GLASSDIAG") ? atoi(getenv("H12_GLASSDIAG")) : 0;
    static int lastT = -100000;
    if (diag && level.time - lastT >= diag) {
        lastT = level.time; const GlassRenderer *r = clGlasses->renderer;
        if (r->prevStat.numVisShards || r->prevStat.numMovingShards)
            { extern int g_h12GlassHits, g_h12GlassRest; fprintf(stderr, "[H12GC] t=%d dt=%.4f grupos=%zu visGrupos=%d visFrag=%d moviendose=%d OOM=%d fisicas=%zu choques=%d posados=%d\n", level.time, r->deltaTime,
                    r->groupsAllocator->used.size(), r->prevStat.numVisGroups, r->prevStat.numVisShards, r->prevStat.numMovingShards,
                    r->prevStat.numOOMGroups, r->physicsAllocator->used.size(), g_h12GlassHits, g_h12GlassRest); }
    }
}

// Pegamento de cliente que el harness no tiene (en el original lo dan splitscreen.cpp, cg_scr_main.cpp y la fisica):
// un solo cliente local (0) activo; sin CSC (CScr_GlassSmash notifica al script de cliente, que el harness no corre);
// create_aabb_gjk_geom solo alimenta Phys_ObjCreate, que en el harness (h8_doors.cpp) devuelve nulo para estos cuerpos y
// glass_shard.cpp cae entonces en GlassPhysics. Debiles: si otra ola los define de verdad, manda la suya.
#include <physics/phys_colgeom.h>
__attribute__((weak)) char __cdecl CL_LocalClient_IsActive(int localClientNum) { return localClientNum == 0; }
__attribute__((weak)) char __cdecl CL_LocalClient_IsFirstActive(int localClientNum) { return localClientNum == 0; }
__attribute__((weak)) void __cdecl CScr_GlassSmash(float *, float *) {}
__attribute__((weak)) gjk_aabb_t *create_aabb_gjk_geom(const float *, const float *, int, gjk_collision_visitor *)
{
    alignas(16) static unsigned char geom[1024]; memset(geom, 0, sizeof geom); return (gjk_aabb_t *)geom;
}
// tl_system (libreria de perfiles del original) no esta en el harness: GlassTimer del renderer de cristal la usa para medir.
#include <tl/tl_system.h>
#include <time.h>
__attribute__((weak)) _LARGE_INTEGER __cdecl tlPcGetTick()
{
    _LARGE_INTEGER r; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); r.QuadPart = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec; return r;
}
// R_CullBoxCurDpvs_SceneSelect (r_dpvs.cpp, el DPVS de la escena del original) no esta en el harness: 0 = caja visible, todos los
// grupos de fragmentos se generan (lo mismo que cullGlassShards 0).
__attribute__((weak)) int __cdecl R_CullBoxCurDpvs_SceneSelect(const float *, unsigned int, unsigned int) { return 0; }
// R_GetAdjustedLodDist (r_model_lod.cpp): con rg.lodParms valido aplica la rampa rigida; sin ella (H4 no la monta) la distancia tal cual.
#include <gfx_d3d/r_model_lod.h>
__attribute__((weak)) double __cdecl R_GetAdjustedLodDist(float dist, XModelLodRampType t)
{
    if (!rg.lodParms.valid) return dist;
    return dist * rg.lodParms.ramp[t].scale + rg.lodParms.ramp[t].bias;
}
// R_AllocModelLighting (r_model_lighting.cpp, iluminacion de modelos por la rejilla de luz) no esta en el harness: el lote de
// fragmentos necesita un handle distinto de 0 para dibujarse (ShardGroup::GenerateVerts). Se da 1 y sin sonda de reflejo.
#include <gfx_d3d/r_model_lighting.h>
// OLA 15F: cada grupo de fragmentos pide su luz del lightgrid en su origen (atlas de h5_light, como actores y smodels) en vez de la
// neutra/la ultima enlazada: handle 0xF000 | n (n = 1..4095, cacheado en ShardGroup::lightingHandle como el original) -> clave de
// H5L = H5L_MaxKey() - 4096 + n. H15F_GLASSLIGHT=0 vuelve a lo de antes (handle 1, coordenadas que hubiera).
void H5L_Prepare(int key, const float *lightingOrigin, const float *half, bool hero) __attribute__((weak));
void H5L_Bind(int key) __attribute__((weak));
int H5L_MaxKey() __attribute__((weak));
static bool H15F_GlassLight() { static const int on = getenv("H15F_GLASSLIGHT") ? atoi(getenv("H15F_GLASSLIGHT")) : 1; return on && H5L_Prepare && H5L_Bind && H5L_MaxKey; }
__attribute__((weak)) unsigned int __cdecl R_AllocModelLighting(float *origin, float, bool, unsigned short *cached, unsigned int (__cdecl *)(const void *), const void *, GfxLightingInfo *out)
{
    if (out) memset(out, 0, sizeof *out);
    if (!H15F_GlassLight() || !origin) { if (cached) *cached = 1; return 1; }
    static unsigned next;
    unsigned short h = cached ? *cached : 0;
    if ((h & 0xF000) != 0xF000 || !(h & 0x0FFF)) { next = next % 4095 + 1; h = (unsigned short)(0xF000 | next); if (cached) *cached = h; }
    static const float half[3] = { 8, 8, 8 };
    H5L_Prepare(H5L_MaxKey() - 4096 + (h & 0x0FFF), origin, half, false);
    return h;
}

// ---- Matematica de fisica que usa GlassPhysics (la fisica propia de los fragmentos). Los ficheros donde vive en el decomp
// (physics/phys_main.cpp, phys_util.cpp, phys_render.cpp, vehicle/nitrous_vehicle.cpp) no compilan en LP64 o duplican la fisica
// sustituta de h8_doors.cpp; aqui van copias literales de esas funciones (debiles) y el registro de los dvars de Phys_Init que
// usa la integracion (gravedad y arrastre, mismos valores por defecto).
#include <physics/phys_local.h>
#include <physics/phys_main.h>
#include <vehicle/nitrous_vehicle.h>
#include <physics/phys_util.h>
__attribute__((weak)) const dvar_t *phys_gravity;
__attribute__((weak)) const dvar_t *phys_gravity_dir;
__attribute__((weak)) const dvar_t *phys_dragLinear;
__attribute__((weak)) const dvar_t *phys_dragAngular;
static void H12_PhysDvars()
{
    if (!phys_gravity) phys_gravity = _Dvar_RegisterFloat("phys_gravity", -800.0, -3.4028235e38, 3.4028235e38, 0x1000u, "Physics gravity in units/sec^2.");
    if (!phys_gravity_dir) phys_gravity_dir = _Dvar_RegisterVec3("phys_gravity_dir", 0.0, 0.0, 1.0, -1.0, 1.0, 0x1000u, "Sets the gravity direction.");
    if (!phys_dragLinear) phys_dragLinear = _Dvar_RegisterFloat("phys_dragLinear", 0.029999999, 0.0, 3.4028235e38, 0, "The amount of linear drag, applied globally");
    if (!phys_dragAngular) phys_dragAngular = _Dvar_RegisterFloat("phys_dragAngular", 0.5, 0.0, 3.4028235e38, 0, "The amount of angular drag, applied globally");
}
// vehicle/nitrous_vehicle.cpp
__attribute__((weak)) double __cdecl AbsSquared(const phys_vec3 *a) { return a->x * a->x + a->y * a->y + a->z * a->z; }
__attribute__((weak)) phys_vec3 *phys_cross(phys_vec3 *result, const phys_vec3 *a, const phys_vec3 *b)
{
    const float v4 = (float)(a->z * b->x) - (float)(a->x * b->z), v5 = (float)(a->x * b->y) - (float)(a->y * b->x);
    result->x = (float)(a->y * b->z) - (float)(a->z * b->y); result->y = v4; result->z = v5; return result;
}
__attribute__((weak)) void phys_multiply_mat(phys_mat44 *dest, const phys_mat44 *left, const phys_mat44 *right)
{
    if (dest == left) { phys_mat44 v4; memcpy(&v4, left, sizeof(v4)); phys_multiply_mat(dest, &v4, right); return; }
    phys_vec3 t; const phys_vec3 *r;
    r = phys_multiply(&t, left, &right->x); dest->x.x = r->x; dest->x.y = r->y; dest->x.z = r->z;
    r = phys_multiply(&t, left, &right->y); dest->y.x = r->x; dest->y.y = r->y; dest->y.z = r->z;
    r = phys_multiply(&t, left, &right->z); dest->z.x = r->x; dest->z.y = r->y; dest->z.z = r->z;
}
__attribute__((weak)) void phys_full_multiply_mat(phys_mat44 *dest, const phys_mat44 *left, const phys_mat44 *right)
{
    if (dest == left) { phys_mat44 v4; memcpy(&v4, left, sizeof(v4)); phys_full_multiply_mat(dest, &v4, right); return; }
    phys_vec3 t; const phys_vec3 *r;
    r = phys_multiply(&t, left, &right->x); dest->x.x = r->x; dest->x.y = r->y; dest->x.z = r->z;
    r = phys_multiply(&t, left, &right->y); dest->y.x = r->x; dest->y.y = r->y; dest->y.z = r->z;
    r = phys_multiply(&t, left, &right->z); dest->z.x = r->x; dest->z.y = r->y; dest->z.z = r->z;
    r = phys_full_multiply(&t, left, &right->w); dest->w.x = r->x; dest->w.y = r->y; dest->w.z = r->z;
}
__attribute__((weak)) void Phys_NitrousMat44ToVec33(const phys_mat44 *m, float (*o)[3])
{
    o[0][0] = m->x.x; o[0][1] = m->x.y; o[0][2] = m->x.z; o[1][0] = m->y.x; o[1][1] = m->y.y; o[1][2] = m->y.z; o[2][0] = m->z.x; o[2][1] = m->z.y; o[2][2] = m->z.z;
}
// physics/phys_main.cpp
__attribute__((weak)) void phys_full_inverse(phys_mat44 *dest, const phys_mat44 *source)
{
    if (dest == source) { phys_mat44 mat; phys_full_inverse(&mat, source); *dest = mat; return; }
    phys_transpose(dest, source);
    phys_vec3 v11; const phys_vec3 *v10 = phys_multiply(&v11, dest, &source->w);
    dest->w.x = -v10->x; dest->w.y = -v10->y; dest->w.z = -v10->z;
}
__attribute__((weak)) unsigned int rigid_body::is_stable() { return this->m_flags & 4; }
// physics/phys_util.cpp
__attribute__((weak)) void __cdecl nuge::calc_box_inertia(const phys_vec3 *dim, phys_vec3 *unit_inertia, float *volume)
{
    *volume = dim->x * dim->y * dim->z;
    const float m4 = dim->x * dim->x, m8 = dim->y * dim->y, m12 = dim->z * dim->z, v13 = dim->x * 0.0833333358168602 * dim->y * dim->z;
    const float v4 = v13 * m4, v5 = m8 * v13, v6 = v13 * m12;
    unit_inertia->x = v5 + v6; unit_inertia->y = v6 + v4; unit_inertia->z = v5 + v4;
}
// physics/phys_render.cpp
static void H12_MakeRotate(phys_mat44 &m, const phys_vec3 *u, float ca, float sa)
{
    const float xy = (u->x * u->y) * (1.0f - ca), xz = (u->x * u->z) * (1.0f - ca), yy = (u->y * u->y) * (1.0f - ca), yz = (u->y * u->z) * (1.0f - ca),
                zz = (u->z * u->z) * (1.0f - ca), xsa = u->x * sa, ysa = u->y * sa, zsa = u->z * sa;
    m.x.x = (u->x * u->x) * (1.0f - ca) + ca; m.y.x = xy - zsa; m.z.x = xz + ysa; m.w.x = 0.0f;
    m.x[1] = xy + zsa; m.y[1] = yy + ca; m.z[1] = yz - xsa; m.w[1] = 0.0f;
    m.x[2] = xz - ysa; m.y[2] = yz + xsa; m.z[2] = zz + ca; m.w[2] = 0.0f;
    m.fix_w_column();
}
__attribute__((weak)) void make_rotate(phys_mat44 *mat, const phys_vec3 *v, float theta_factor, float max_rotation_radians)
{
    float len = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
    if (len >= 0.0000099999997) {
        phys_vec3 v8; v8.x = (1.0 / len) * v->x; v8.y = (1.0 / len) * v->y; v8.z = (1.0 / len) * v->z;
        len = len * theta_factor; if (len > max_rotation_radians) len = max_rotation_radians;
        H12_MakeRotate(*mat, &v8, cosf(len), sinf(len));
    } else *mat = PHYS_IDENTITY_MATRIX;
}
// gfx_d3d/r_model_lighting.cpp / r_stream.cpp: sin rejilla de luz ni streaming en el harness
__attribute__((weak)) unsigned int __cdecl R_PassbackPrimaryLightCallback(const void *userData) { return *(const unsigned int *)userData; }
__attribute__((weak)) void __cdecl R_StreamTouchMaterial(Material *) {}
// R_SetModelLightingCoordsForSource (r_model_lighting.cpp, sin portar: atlas del lightgrid por handle): el codemesh de los fragmentos
// pide la luz de modelo; se usan las coordenadas del volumen neutro/atlas del arnes (world_h4/h5_light), como los modelos.
extern float g_h4ModelLightCoords[4];
__attribute__((weak)) void __cdecl R_SetModelLightingCoordsForSource(unsigned __int16 h, GfxCmdBufSourceState *source)
{
    if ((h & 0xF000) == 0xF000 && (h & 0x0FFF) && H15F_GlassLight()) H5L_Bind(H5L_MaxKey() - 4096 + (h & 0x0FFF));   // OLA 15F: luz del grupo de fragmentos
    memcpy(source->input.consts[CONST_SRC_CODE_BASE_LIGHTING_COORDS], g_h4ModelLightCoords, 16);
    ++source->constVersions[CONST_SRC_CODE_BASE_LIGHTING_COORDS];
}

// ---- OLA 15E: colision simple de los fragmentos que van por GlassPhysics (sin Phys_ObjCreate no chocan con nada y atravesaban
// el suelo). glass_shard.cpp llama a este gancho tras cada paso: se traza el centro del fragmento del paso anterior al nuevo contra
// los solidos del mundo (CM_BoxTrace de punto, CONTENTS_SOLID). Al tocar: se coloca en el punto de impacto, la velocidad rebota
// (restitucion H12_GLASS_BOUNCE, 0,3) con rozamiento tangencial y se frena el giro. Si toca un suelo (normal z > 0,6) a poca
// velocidad, se tumba en el plano del suelo y devuelve 1: el original hace lo mismo cuando el cuerpo Nitrous queda estable
// (DestroyPhysicsObj y el fragmento queda quieto en el suelo). Coste: una traza de punto por fragmento en movimiento y frame.
// H12_GLASSCOLL=0 lo apaga (vuelve al comportamiento de la ola 12).
#include <qcommon/cm_trace.h>
#include <glass/glass_shard.h>
void __cdecl CM_BoxTrace(trace_t *results, const float *start, const float *end, const float *mins, const float *maxs, int brushmask, col_context_t *context);
int g_h12GlassHits, g_h12GlassRest;
int GlassShard_PortCollide(GlassShard *s, const float *prevOrigin)
{
    static const int on = getenv("H12_GLASSCOLL") ? atoi(getenv("H12_GLASSCOLL")) : 1;
    static const float e = getenv("H12_GLASS_BOUNCE") ? (float)atof(getenv("H12_GLASS_BOUNCE")) : 0.3f;
    GlassPhysics *gp = s->glassPhysics; if (!on || !gp) return 0;
    float mn[3], mx[3]; s->GetLocalBBox(mn, mx);
    float lc[3]; for (int k = 0; k < 3; ++k) lc[k] = 0.5f * (mn[k] + mx[k]);
    float off[3]; for (int k = 0; k < 3; ++k) off[k] = lc[0] * s->axis[0][k] + lc[1] * s->axis[1][k] + lc[2] * s->axis[2][k];
    float a[3], b[3]; for (int k = 0; k < 3; ++k) { a[k] = prevOrigin[k] + off[k]; b[k] = s->origin[k] + off[k]; }
    if ((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]) < 1e-6f) return 0;
    trace_t tr; memset(&tr, 0, sizeof tr); const float z[3] = { 0, 0, 0 };
    col_context_t ctx(1); CM_BoxTrace(&tr, a, b, z, z, 1, &ctx);
    if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f) return 0;
    const float *tn = (const float *)&tr.normal; const float n[3] = { tn[0], tn[1], tn[2] };
    float hit[3]; for (int k = 0; k < 3; ++k) hit[k] = a[k] + (b[k] - a[k]) * tr.fraction + n[k] * 0.5f;
    ++g_h12GlassHits;
    float v[3] = { gp->m_t_vel.x, gp->m_t_vel.y, gp->m_t_vel.z }; const float vn = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
    if (vn < 0) for (int k = 0; k < 3; ++k) { const float vt = v[k] - vn * n[k]; v[k] = vt * 0.55f - e * vn * n[k]; }
    const float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const bool rest = n[2] > 0.6f && sp < 60.0f;
    if (rest) {
        // tumbado: la normal del fragmento (eje 2, el del grosor) pasa a ser la del suelo, conservando hacia donde apuntaba
        float ax[3][3]; memcpy(ax, s->axis, sizeof ax);
        const float sg = (ax[2][0] * n[0] + ax[2][1] * n[1] + ax[2][2] * n[2]) < 0 ? -1.0f : 1.0f;
        for (int k = 0; k < 3; ++k) ax[2][k] = n[k] * sg;
        const float d = ax[0][0] * ax[2][0] + ax[0][1] * ax[2][1] + ax[0][2] * ax[2][2];
        for (int k = 0; k < 3; ++k) ax[0][k] -= d * ax[2][k];
        float l = sqrtf(ax[0][0] * ax[0][0] + ax[0][1] * ax[0][1] + ax[0][2] * ax[0][2]);
        if (l < 1e-3f) { ax[0][0] = 1; ax[0][1] = ax[0][2] = 0; l = 1; }
        for (int k = 0; k < 3; ++k) ax[0][k] /= l;
        ax[1][0] = ax[2][1] * ax[0][2] - ax[2][2] * ax[0][1]; ax[1][1] = ax[2][2] * ax[0][0] - ax[2][0] * ax[0][2]; ax[1][2] = ax[2][0] * ax[0][1] - ax[2][1] * ax[0][0];
        memcpy(s->axis, ax, sizeof ax);
        for (int k = 0; k < 3; ++k) off[k] = lc[0] * ax[0][k] + lc[1] * ax[1][k] + lc[2] * ax[2][k];
        v[0] = v[1] = v[2] = 0; gp->m_a_vel.x = gp->m_a_vel.y = gp->m_a_vel.z = 0; ++g_h12GlassRest;
        gp->m_mat.x.x = ax[0][0]; gp->m_mat.x.y = ax[0][1]; gp->m_mat.x.z = ax[0][2];
        gp->m_mat.y.x = ax[1][0]; gp->m_mat.y.y = ax[1][1]; gp->m_mat.y.z = ax[1][2];
        gp->m_mat.z.x = ax[2][0]; gp->m_mat.z.y = ax[2][1]; gp->m_mat.z.z = ax[2][2];
    } else { gp->m_a_vel.x *= 0.5f; gp->m_a_vel.y *= 0.5f; gp->m_a_vel.z *= 0.5f; }
    for (int k = 0; k < 3; ++k) s->origin[k] = hit[k] - off[k];
    gp->m_mat.w.x = s->origin[0]; gp->m_mat.w.y = s->origin[1]; gp->m_mat.w.z = s->origin[2];
    gp->m_t_vel.x = v[0]; gp->m_t_vel.y = v[1]; gp->m_t_vel.z = v[2];
    return rest ? 1 : 0;
}
