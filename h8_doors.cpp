// OLA 8: puertas, zonas de pago y cristales de zombie_pentagon.
//   H8_DOORLOG=1 -> registro [H8D] del estado de cada compra (puerta/debris) con los puntos del jugador
//   H8_GLASS=0   -> no dibuja los cristales (por defecto se dibujan como GlassClient::Update del original)
//   H8_GLASSLOG=1 -> registro [H8G] de estados de los cristales (intacto/agrietado/roto) al empezar y en cada cambio
//   H8_DIAG=1  -> registro [H8] del mundo (GfxWorld/modelos de brocha) y de las entidades de puertas/debris/cristal
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <server_mp/server_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <game_sp/g_sp_client.h>
#include <bgame/bg_misc.h>
#include <clientscript/cscr_stringlist.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_gfx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <set>
#include <vector>
#include <array>
#include <string>
#include <gfx_d3d/r_init.h>
#include <qcommon/cm_load_obj.h>
#include <database/db_registry.h>
#include <glass/glass_server.h>
#include <universal/com_math_anglevectors.h>
#include <gfx_d3d/r_material.h>

extern GfxWorld *g_h8World;   // world_h4.cpp (puede ser nulo sin vista)
GfxWorld *g_h8World;

static const char *H8_Str(unsigned short s) { return s ? SL_ConvertToString(s, SCRIPTINSTANCE_SERVER) : ""; }

static std::set<std::string> g_h8Targets;   // destinos de los triggers zombie_door / zombie_debris
static bool H8_Interesting(const gentity_s *e)
{
    if (e->targetname && g_h8Targets.count(H8_Str(e->targetname))) return true;
    const char *cn = H8_Str(e->classname), *tn = H8_Str(e->targetname), *sn = H8_Str(e->script_noteworthy);
    return strstr(tn, "door") || strstr(tn, "debris") || strstr(tn, "glass") || strstr(sn, "door") || strstr(sn, "debris")
        || strstr(cn, "glass") || strstr(tn, "zone") || strstr(tn, "blocker") || strstr(tn, "elevator");
}

void H8_Dump(const char *tag)
{
    int n = 0, sm = 0, smLinked = 0;
    for (int i = 0; i < G_EntEnd(); i = G_EntNext(i)) {
        const gentity_s *e = &g_entities[i];
        if (e->r.inuse && e->target && (!strcmp(H8_Str(e->targetname), "zombie_door") || !strcmp(H8_Str(e->targetname), "zombie_debris"))) g_h8Targets.insert(H8_Str(e->target));
    }
    if (rgp.world) {
        GfxWorld *w = rgp.world;
        fprintf(stderr, "[H8] %s world surfaces=%d static=%u modelCount=%d m0 start=%u n=%u\n", tag, w->surfaceCount, w->dpvs.staticSurfaceCount, w->modelCount, w->models[0].startSurfIndex, w->models[0].surfaceCount);
    }
    for (int i = 0; i < G_EntEnd(); i = G_EntNext(i))
    {
        const gentity_s *e = &g_entities[i];
        if (!e->r.inuse) continue;
        if (e->s.eType == ET_SCRIPTMOVER) { ++sm; if (e->r.linked) ++smLinked; }
        if (!H8_Interesting(e)) continue;
        if (++n > 400) continue;
        fprintf(stderr, "[H8] %s ent %d cn=%s tn=%s tgt=%s sn=%s eT=%d solid=0x%x idx=%d bmodel=%d linked=%d contents=0x%x org=(%.0f %.0f %.0f) ang=(%.0f %.0f %.0f) svf=0x%x\n",
            tag, i, H8_Str(e->classname), H8_Str(e->targetname), H8_Str(e->target), H8_Str(e->script_noteworthy), e->s.eType, e->s.solid,
            e->s.index.brushmodel, e->r.bmodel, e->r.linked, e->r.contents, e->r.currentOrigin[0], e->r.currentOrigin[1], e->r.currentOrigin[2],
            e->r.currentAngles[0], e->r.currentAngles[1], e->r.currentAngles[2], e->r.svFlags);
        if (rgp.world && e->r.bmodel && e->s.index.brushmodel > 0 && e->s.index.brushmodel < rgp.world->modelCount)
            fprintf(stderr, "[H8]    bmodel %d: surf %u..+%u\n", e->s.index.brushmodel, rgp.world->models[e->s.index.brushmodel].startSurfIndex, rgp.world->models[e->s.index.brushmodel].surfaceCount);
        if (rgp.world && e->r.bmodel && e->s.index.brushmodel > 0 && e->s.index.brushmodel < rgp.world->modelCount && rgp.world->models[e->s.index.brushmodel].surfaceCount) {
            const GfxBrushModel *bm = &rgp.world->models[e->s.index.brushmodel]; const GfxSurface *sf = &rgp.world->dpvs.surfaces[bm->startSurfIndex];
            fprintf(stderr, "[H8]    bm bounds (%.0f %.0f %.0f)-(%.0f %.0f %.0f) surf0 bounds (%.0f %.0f %.0f)-(%.0f %.0f %.0f) mat=%s\n", bm->bounds[0][0], bm->bounds[0][1], bm->bounds[0][2], bm->bounds[1][0], bm->bounds[1][1], bm->bounds[1][2],
                sf->bounds[0][0], sf->bounds[0][1], sf->bounds[0][2], sf->bounds[1][0], sf->bounds[1][1], sf->bounds[1][2], sf->material.get() ? sf->material->info.name.get() : "-"); }
    }
    fprintf(stderr, "[H8] %s resumen: %d entidades de interes, %d script movers (%d enlazados), num_entities=%d\n", tag, n, sm, smLinked, G_EntEnd());
}

// H8_DOORLOG=1: estado de cada compra (trigger zombie_door / zombie_debris y sus brushmodels) al empezar, cada 5 s y en cada cambio.
// Abierta = el GSC (maps\\_zombiemode_blockers) borro el trigger tras cobrar o movio/borro sus piezas.
struct H8Buy { int trig; float trigZ0; std::string kind, target; std::vector<int> parts; std::vector<std::array<float, 6>> org0; };
static std::vector<H8Buy> g_h8Buys;
static void H8_CollectBuys()
{
    for (int i = 0; i < G_EntEnd(); i = G_EntNext(i)) {
        const gentity_s *e = &g_entities[i]; if (!e->r.inuse || !e->target) continue;
        const char *tn = H8_Str(e->targetname); if (strcmp(tn, "zombie_door") && strcmp(tn, "zombie_debris")) continue;
        H8Buy b; b.trig = i; b.trigZ0 = e->r.currentOrigin[2]; b.kind = tn + 7; b.target = H8_Str(e->target);
        for (int j = 0; j < G_EntEnd(); j = G_EntNext(j)) { const gentity_s *p = &g_entities[j];
            if (p->r.inuse && p->targetname == e->target) { b.parts.push_back(j); b.org0.push_back({ p->r.currentOrigin[0], p->r.currentOrigin[1], p->r.currentOrigin[2], p->r.currentAngles[0], p->r.currentAngles[1], p->r.currentAngles[2] }); } }
        g_h8Buys.push_back(b);
    }
}
static std::string H8_BuyState(const H8Buy &b, int *open)
{
    const gentity_s *t = &g_entities[b.trig];
    // trigger_off (common_scripts\\utility) no borra el trigger: lo baja 10000 u
    const bool trig = t->r.inuse && fabsf(t->r.currentOrigin[2] - b.trigZ0) < 1000.0f;
    int moved = 0, gone = 0;
    for (size_t k = 0; k < b.parts.size(); ++k) { const gentity_s *p = &g_entities[b.parts[k]];
        if (!p->r.inuse || p->targetname != t->target) { ++gone; continue; }
        float d = 0; for (int a = 0; a < 3; ++a) d += (p->r.currentOrigin[a] - b.org0[k][a]) * (p->r.currentOrigin[a] - b.org0[k][a]) + (p->r.currentAngles[a] - b.org0[k][3 + a]) * (p->r.currentAngles[a] - b.org0[k][3 + a]);
        if (d > 1.0f) ++moved; }
    std::string where;
    for (size_t k = 0; k < b.parts.size(); ++k) { const gentity_s *p = &g_entities[b.parts[k]]; char t[160];
        snprintf(t, sizeof t, " %d:%s(%.0f %.0f %.0f / %.0f %.0f %.0f)", b.parts[k], p->r.inuse ? H8_Str(p->classname) : "borrada", p->r.currentOrigin[0], p->r.currentOrigin[1], p->r.currentOrigin[2], p->r.currentAngles[0], p->r.currentAngles[1], p->r.currentAngles[2]); where += t; }
    *open = !trig || moved || gone;
    char buf[1024]; snprintf(buf, sizeof buf, "%s %s(%s, trigger %d %s, piezas %zu movidas %d borradas %d:%s)", b.kind.c_str(), b.target.c_str(), *open ? "ABIERTA" : "cerrada", b.trig, trig ? "si" : "no", b.parts.size(), moved, gone, where.c_str());
    return buf;
}
static void H8_DoorLog(int timeMs)
{
    if (g_h8Buys.empty()) H8_CollectBuys();
    static std::string last;
    std::string st; int nOpen = 0;
    for (const H8Buy &b : g_h8Buys) { int o; st += " | " + H8_BuyState(b, &o); nOpen += o; }
    static int lastT = -100000;
    if (st == last && timeMs - lastT < 5000) return;
    lastT = timeMs;
    const gentity_s *pl = &g_entities[0]; const int pts = pl->r.inuse && pl->client ? pl->client->sess.cs.score.score : -1;
    fprintf(stderr, "[H8D] t=%d pts=%d abiertas=%d/%zu%s%s\n", timeMs, pts, nOpen, g_h8Buys.size(), st == last ? " (sin cambios)" : "", st == last ? "" : st.c_str());
    last = st;
}

static void H8_GlassLog(int timeMs);
// H8_ENTS=<fichero>: vuelca la cadena de entidades del mapa (MapEnts) para revisar las claves de puertas/debris (fuera del repo)
static void H8_EnumEnts(XAssetHeader h, void *f) { fwrite(h.mapEnts->entityString.get(), 1, h.mapEnts->numEntityChars, (FILE *)f); }
void H8_Frame()
{
    static const int diag = getenv("H8_DIAG") ? atoi(getenv("H8_DIAG")) : 0;
    static const int doorLog = getenv("H8_DOORLOG") ? atoi(getenv("H8_DOORLOG")) : 0;
    static int f = 0; ++f;
    if (f == 2) if (const char *fn = getenv("H8_ENTS")) if (FILE *fp = fopen(fn, "wb")) { DB_EnumXAssets_FastFile(ASSET_TYPE_MAP_ENTS, H8_EnumEnts, fp, false); fclose(fp); }
    if (diag && f == 2) H8_Dump("ini");
    if (diag && f == 200) H8_Dump("t10s");
    if (doorLog && f >= 2) H8_DoorLog(level.time);
    static const int glassLog = getenv("H8_GLASSLOG") ? atoi(getenv("H8_GLASSLOG")) : 0;
    if (glassLog && f >= 2) H8_GlassLog(level.time);
    { extern void H12_Frame() __attribute__((weak)); if (H12_Frame) H12_Frame(); }   // OLA 12: barricadas de ventana (h12_barrier.cpp)
}

// ---- Cristales. En el original no son entidades: los Glass del mapa (asset GLASSES) los lleva el servidor (svGlasses, estado
// PRISTINE/CRACKED/SHATTERED) y el cliente los dibuja en GlassClient::Update (glass_client.cpp): si no estan rotos,
// R_AddGlassBrushToScene(R_GetBrushModel(glass->brushModel), glass->origin, glass->angles) con las superficies del brushmodel en
// el material que eligio GlassClient::SetBrushMaterial (pristineMaterial intacto, crackedMaterial agrietado). Las superficies de
// los brushmodels estan en coordenadas locales: sin esa colocacion el panel no se ve en su ventana y queda el marco con los
// bordes rotos de la textura (lo que se veia en docs/evidencia/gfx/mac_antes_despues_cam2.jpg).
void H4_DrawSurfsPlaced(Material *m, const unsigned short *surfs, int n, unsigned char prim, const GfxScaledPlacement *pl, unsigned char techType);
static void (*g_h8PrevDraw)(unsigned char);
static GfxScaledPlacement g_h8GlassPl[1000];
static void H8_DrawGlass(unsigned char techType)
{
    if (g_h8PrevDraw) g_h8PrevDraw(techType);
    GfxWorld *w = rgp.world; if (!w) return;
    static std::vector<unsigned short> surfs;
    for (unsigned i = 0; i < svGlasses.numGlasses && i < 1000; ++i) {
        const GlassServer *gs = &svGlasses.glasses[i]; const Glass *g = gs->glass; if (!g || !g->glassDef) continue;
        const unsigned st = gs->state.val.i & 0xF; if (st >= 2) continue;   // SHATTERED/INVALID: no hay panel (los trozos son GlassShard)
        if ((int)g->brushModel <= 0 || (int)g->brushModel >= w->modelCount) continue;
        const GfxBrushModel *bm = &w->models[g->brushModel]; if (!bm->surfaceCount) continue;
        Material *m = st == 0 ? g->glassDef->pristineMaterial.get() : g->glassDef->crackedMaterial.get(); if (!m) continue;
        GfxScaledPlacement *pl = &g_h8GlassPl[i];
        memcpy(pl->base.origin, g->origin, 12); AnglesToQuat(g->angles, pl->base.quat); pl->scale = 1.0f;
        surfs.clear(); unsigned char prim = 0;
        for (unsigned k = 0; k < bm->surfaceCount; ++k) { const GfxSurface *sf = &w->dpvs.surfaces[bm->startSurfIndex + k]; if (!sf->tris.triCount) continue; surfs.push_back((unsigned short)(bm->startSurfIndex + k)); prim = sf->primaryLightIndex; }
        if (!surfs.empty()) H4_DrawSurfsPlaced(m, surfs.data(), (int)surfs.size(), prim, pl, techType);
    }
}
void H8_InstallDraw()
{
    if (getenv("H8_GLASS") && !atoi(getenv("H8_GLASS"))) return;
    extern void (*g_h4DrawExtra)(unsigned char techType);
    g_h8PrevDraw = g_h4DrawExtra; g_h4DrawExtra = H8_DrawGlass;
}
static void H8_GlassLog(int timeMs)
{
    static std::vector<unsigned> last; static bool first = true;
    if (last.size() != svGlasses.numGlasses) last.assign(svGlasses.numGlasses, 99);
    int n[4] = { 0, 0, 0, 0 };
    for (unsigned i = 0; i < svGlasses.numGlasses; ++i) {
        const unsigned st = svGlasses.glasses[i].state.val.i & 0xF; ++n[st < 3 ? st : 3];
        if (!first && st != last[i]) { const Glass *g = svGlasses.glasses[i].glass;
            fprintf(stderr, "[H8G] t=%d cristal %u (%s, brushmodel %u en %.0f %.0f %.0f): %u -> %u (%s) salud %.0f\n", timeMs, i, g->glassDef->name.get(), g->brushModel, g->origin[0], g->origin[1], g->origin[2], last[i], st,
                st == 0 ? "intacto" : st == 1 ? "agrietado" : "roto", svGlasses.glasses[i].health); }
        last[i] = st;
    }
    static int lastT = -100000;
    if (first) for (unsigned i = 0; i < svGlasses.numGlasses; ++i) { const Glass *g = svGlasses.glasses[i].glass; const GlassDef *d = g->glassDef.get();
        fprintf(stderr, "[H8G] cristal %u def %s brushmodel %u en (%.0f %.0f %.0f) ang (%.0f %.0f %.0f) salud %.0f mat %s / %s\n", i, d->name.get(), g->brushModel, g->origin[0], g->origin[1], g->origin[2], g->angles[0], g->angles[1], g->angles[2],
            svGlasses.glasses[i].health, d->pristineMaterial ? d->pristineMaterial->info.name.get() : "-", d->crackedMaterial ? d->crackedMaterial->info.name.get() : "-"); }
    if (first || timeMs - lastT >= 5000) { lastT = timeMs; fprintf(stderr, "[H8G] t=%d cristales=%u intactos=%d agrietados=%d rotos=%d\n", timeMs, svGlasses.numGlasses, n[0], n[1], n[2]); }
    first = false;
}

// ---------------------------------------------------------------------------------------------
// H8_PHYS: fisica sustituta minima. Nitrous no esta portado (los Phys_* eran stubs que devolvian
// 0 y G_CreatePhysicsObject acababa en DObjGetName(null) -> SIGNAL 6 al comprar una puerta
// "physics", p.ej. la de conferencias de Pentagon). Aqui: cuerpo balistico (velocidad inicial =
// la fuerza de PhysicsLaunch, gravedad 800 u/s^2, giro suave), parada con traza contra el mundo,
// y "dormido" al tocar suelo o a los 3 s. Solo para script movers (puertas/escombros).
// H8_PHYS=0 -> Phys_ObjCreate falla limpio (la pieza queda estacionaria, sin crash).
// El handle es indice+1 (LP64: el x86 usaba el puntero como int).
// ---------------------------------------------------------------------------------------------
#include <physics/phys_main.h>
#include <physics/phys_collision.h>
#include <physics/phys_colgeom.h>
#include <server/sv_world.h>
struct H8Body { bool used, asleep; int ent, t0, tl; float p[3], v[3], a[3], w[3]; PhysObjUserData ud; };
static H8Body g_h8b[64];
static bool H8_PhysOn() { static int on = -1; if (on < 0) { const char *e = getenv("H8_PHYS"); on = (!e || atoi(e)) ? 1 : 0; } return on != 0; }
static H8Body *H8_B(int id) { return (id >= 1 && id <= 64 && g_h8b[id - 1].used) ? &g_h8b[id - 1] : nullptr; }
void __cdecl Phys_CalcPreset(PhysPreset *, float *, int) {}
void __cdecl create_gjk_geom(gentity_s *, gjk_collision_visitor *, bool, unsigned int, bool) {}
PhysObjUserData *__cdecl Phys_ObjCreate(int, const float *position, const float *quat, const float *velocity, const PhysPreset *, gjk_geom_list_t *, bool, int id)
{
    if (!H8_PhysOn() || id < 0 || id >= MAX_GENTITIES || g_entities[id].s.eType != ET_SCRIPTMOVER) return nullptr;
    for (int i = 0; i < 64; ++i) if (!g_h8b[i].used) {
        H8Body &b = g_h8b[i]; memset(&b, 0, sizeof(b)); b.used = true; b.ent = id; b.t0 = b.tl = level.time;
        for (int k = 0; k < 3; ++k) { b.p[k] = position[k]; b.v[k] = velocity[k]; b.a[k] = g_entities[id].r.currentAngles[k]; }
        const float sp = sqrtf(b.v[0] * b.v[0] + b.v[1] * b.v[1] + b.v[2] * b.v[2]);
        b.w[0] = sp * 0.6f; b.w[2] = sp * 0.2f;   // vuelca hacia delante al salir despedida
        b.ud.refcount = 1;
        fprintf(stderr, "[H8P] crear cuerpo %d ent %d en (%.0f %.0f %.0f) v (%.0f %.0f %.0f)\n", i + 1, id, b.p[0], b.p[1], b.p[2], b.v[0], b.v[1], b.v[2]);
        (void)quat; return (PhysObjUserData *)(intptr_t)(i + 1);
    }
    return nullptr;
}
PhysObjUserData *__cdecl Phys_GetUserData(int id) { static PhysObjUserData dummy; H8Body *b = H8_B(id); return b ? &b->ud : &dummy; }
void __cdecl Phys_ObjDestroy(int, int id) { if (H8Body *b = H8_B(id)) { fprintf(stderr, "[H8P] destruir cuerpo %d ent %d en (%.0f %.0f %.0f)\n", id, b->ent, b->p[0], b->p[1], b->p[2]); b->used = false; } }
bool __cdecl Phys_ObjIsAsleep(int id) { H8Body *b = H8_B(id); return !b || b->asleep; }
void __cdecl Phys_ObjBulletImpact(int, const float *, const float *, float, float) {}
void Phys_ObjAddForce(int, float *, float *, bool) {}
static void H8_Step(H8Body &b)
{
    const int now = level.time; if (b.asleep || now <= b.tl) return;
    const float dt = (now - b.tl) * 0.001f; b.tl = now;
    float np[3] = { b.p[0] + b.v[0] * dt, b.p[1] + b.v[1] * dt, b.p[2] + b.v[2] * dt - 400.f * dt * dt };
    b.v[2] -= 800.f * dt;
    trace_t tr; memset(&tr, 0, sizeof(tr)); const float mn[3] = { -2, -2, -2 }, mx[3] = { 2, 2, 2 };
    col_context_t ctx; G_TraceCapsule(&tr, b.p, mn, mx, np, b.ent, 1 /*CONTENTS_SOLID*/, &ctx);
    if (tr.fraction < 1.f) { for (int k = 0; k < 3; ++k) np[k] = b.p[k] + (np[k] - b.p[k]) * tr.fraction; b.asleep = true; }
    for (int k = 0; k < 3; ++k) { b.p[k] = np[k]; b.a[k] += b.w[k] * dt; }
    if (now - b.t0 >= 3000) b.asleep = true;
}
void __cdecl Phys_ObjGetInterpolatedState(int id, float *outPos, float *outQuat)
{
    H8Body *b = H8_B(id); if (!b) return; H8_Step(*b);
    outPos[0] = b->p[0]; outPos[1] = b->p[1]; outPos[2] = b->p[2]; AnglesToQuat(b->a, outQuat);
}
void __cdecl Phys_ObjGetVelocities(int id, float *tvel, float *avel)
{
    H8Body *b = H8_B(id); for (int k = 0; k < 3; ++k) { if (tvel) tvel[k] = b ? b->v[k] : 0.f; if (avel) avel[k] = b ? b->w[k] : 0.f; }
}
void Phys_ObjGetPosition(int id, float *outPosition, float (*outRotation)[3])
{
    H8Body *b = H8_B(id); if (!b) return; for (int k = 0; k < 3; ++k) outPosition[k] = b->p[k];
    if (outRotation) AnglesToAxis(b->a, outRotation);
}
