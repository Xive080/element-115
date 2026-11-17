// OLA 12: barricadas de ventana de zombie_pentagon (diagnostico y pruebas; la logica es el GSC original de
// maps\_zombiemode_blockers / _zombiemode_spawner, aqui no se reescribe nada).
//   H12_LOG=1      -> [H12W] estado de los tablones de cada ventana (exterior_goal) en cada cambio, [H12A] anim scripted de cada
//                     zombi (animscripted: ai_zombie_board_tear_*, window_attack, taunts), [H12N] notetracks de esas anims (board, end...)
//   H12_CAM=auto | x,y,z,yaw,pitch | wN -> camara fija para las capturas (auto: la ventana con el ultimo evento; wN: ventana N)
//   H12_SHOTS=n    -> hasta n capturas por evento (cambio de tablon) con 4 frames: +0, +150, +400, +800 ms (necesita P4_OUT)
//   H12_SHOT_EVERY=ms -> ademas una captura cada ms de juego
//   H12_REPAIR=N[,t] -> prueba de reponer: a partir de t ms (def. 0) y cuando la ventana N tiene tablones arrancados, el jugador se
//                     coloca en el trigger (trigger_location del GSC) mirando a la ventana y mantiene USAR (H5PlayerInput)
//   H12_GOD=1      -> el jugador no muere durante la prueba (solo diagnostico; vuelve a 100 de salud cada frame)
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <server_mp/server_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <game_mp/g_misc_mp.h>
#include <game_mp/g_scr_main_mp.h>
#include <game_sp/actor_sp_ext.h>
#include <clientscript/cscr_stringlist.h>
#include <xanim/xanim.h>
#include <xanim/dobj.h>
#include <database/db_registry.h>
#include <qcommon/cm_load_obj.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include "h5_input.h"
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_init.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <array>
#include <glass/glass_server.h>

void H4_SetCam(const float *org, const float *ang);
void H4_GetCam(float *org, float *ang);
void H4_Shot(const char *tag) __attribute__((weak));   // no existe en la app (BO1_APP)

static const char *S(unsigned short s) { return s ? SL_ConvertToString(s, SCRIPTINSTANCE_SERVER) : ""; }
static int EnvI(const char *n, int d) { const char *e = getenv(n); return e ? atoi(e) : d; }

struct H12Chunk { int ent; std::string sn, tgt; float og[3], oa[3]; int sec; std::string last; };
struct H12Win { std::string target; float org[3], ang[3], trig[3]; bool hasTrig; std::vector<H12Chunk> ch; float center[3]; std::string last; };
static std::vector<H12Win> g_w;
static bool g_init;
static int g_lastEventWin = -1, g_lastEventT = -100000;

// ---- MapEnts: los exterior_goal (script_struct, no son entidades) y su trigger_location (struct con radius/height)
struct KV { std::map<std::string, std::string> m; };
static std::vector<KV> g_ents;
static void H12_EnumEnts(XAssetHeader h, void *) {
    const char *p = h.mapEnts->entityString.get(); const char *e = p + h.mapEnts->numEntityChars;
    KV cur; bool in = false;
    while (p < e && *p) {
        if (*p == '{') { cur.m.clear(); in = true; ++p; continue; }
        if (*p == '}') { if (in) g_ents.push_back(cur); in = false; ++p; continue; }
        if (*p == '"') { const char *k0 = ++p; while (p < e && *p != '"') ++p; std::string k(k0, p - k0); ++p;
            while (p < e && *p != '"') ++p; const char *v0 = ++p; while (p < e && *p != '"') ++p; std::string v(v0, p - v0); ++p; cur.m[k] = v; continue; }
        ++p;
    }
}
static void Vec(const std::string &s, float *v) { v[0] = v[1] = v[2] = 0; sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]); }

static int FindEntByTargetname(const std::string &tn, int after = -1) {
    for (int i = after + 1; i < level.num_entities; ++i) { const gentity_s *e = &g_entities[i]; if (e->r.inuse && e->targetname && tn == S(e->targetname)) return i; }
    return -1;
}
// H12_FXDUMP=texto: lista los FxEffectDef cuyo nombre contiene el texto, con cada elemento (tipo y modelo si es FX_ELEM_TYPE_MODEL = 7)
#include <gfx_d3d/fxprimitives.h>
#include <xanim/xmodel.h>
static void H12_FxDumpOne(XAssetHeader h, void *ctx)
{
    const FxEffectDef *fx = h.fx; const char *nm = fx->name.get(); if (!nm || !strstr(nm, (const char *)ctx)) return;
    const int n = fx->elemDefCountLooping + fx->elemDefCountOneShot + fx->elemDefCountEmission;
    fprintf(stderr, "[H12FX] efecto %s: %d elementos\n", nm, n);
    for (int i = 0; i < n; ++i) { const FxElemDef *el = &fx->elemDefs.get()[i];
        std::string vis;
        for (int k = 0; k < el->visualCount && k < 6; ++k) { const FxElemVisuals v = el->visualCount > 1 ? el->visuals.array.get()[k] : el->visuals.instance;
            if (el->elemType == 7 && v.model.get()) { vis += " "; vis += v.model.get()->name.get(); } }
        fprintf(stderr, "[H12FX]   elem %d tipo %u visuales %d%s\n", i, el->elemType, el->visualCount, vis.c_str()); }
}
static void H12_Init()
{
    g_init = true;
    DB_EnumXAssets_FastFile(ASSET_TYPE_MAP_ENTS, H12_EnumEnts, nullptr, false);
    if (const char *fd = getenv("H12_FXDUMP")) DB_EnumXAssets_FastFile(ASSET_TYPE_FX, H12_FxDumpOne, (void *)fd, false);
    for (const KV &g : g_ents) {
        auto it = g.m.find("targetname"); if (it == g.m.end() || it->second != "exterior_goal" || !g.m.count("target")) continue;
        H12Win w; w.target = g.m.at("target"); Vec(g.m.count("origin") ? g.m.at("origin") : "", w.org); Vec(g.m.count("angles") ? g.m.at("angles") : "", w.ang); w.hasTrig = false;
        for (const KV &t : g_ents) if (t.m.count("targetname") && t.m.at("targetname") == w.target && t.m.count("classname") && t.m.at("classname") == "script_struct") { Vec(t.m.at("origin"), w.trig); w.hasTrig = true; }
        for (int i = FindEntByTargetname(w.target); i >= 0; i = FindEntByTargetname(w.target, i)) {
            const gentity_s *e = &g_entities[i]; if (!strcmp(S(e->script_noteworthy), "clip")) continue;
            H12Chunk c; c.ent = i; c.sn = S(e->script_noteworthy); c.tgt = S(e->target); memcpy(c.og, e->r.currentOrigin, 12); memcpy(c.oa, e->r.currentAngles, 12);
            c.sec = c.tgt.empty() ? -1 : FindEntByTargetname(c.tgt); w.ch.push_back(c);
        }
        w.center[0] = w.center[1] = w.center[2] = 0;
        for (auto &c : w.ch) for (int k = 0; k < 3; ++k) w.center[k] += c.og[k] / (w.ch.empty() ? 1 : w.ch.size());
        g_w.push_back(w);
    }
    fprintf(stderr, "[H12] %zu ventanas (exterior_goal)\n", g_w.size());
    for (size_t i = 0; i < g_w.size(); ++i) { const H12Win &w = g_w[i]; std::string d;
        for (auto &c : w.ch) { char t[96]; const gentity_s *s = c.sec >= 0 ? &g_entities[c.sec] : nullptr;
            snprintf(t, sizeof t, " %s:e%d*%d%s", c.sn.c_str(), c.ent, g_entities[c.ent].s.index.brushmodel, s ? (std::string("+") + S(s->script_noteworthy) + "*" + std::to_string(s->s.index.brushmodel)).c_str() : ""); d += t; }
        fprintf(stderr, "[H12] ventana %zu %s goal (%.0f %.0f %.0f) yaw %.0f centro (%.0f %.0f %.0f) trigger %s(%.0f %.0f %.0f) tablones %zu:%s\n", i, w.target.c_str(), w.org[0], w.org[1], w.org[2], w.ang[1],
            w.center[0], w.center[1], w.center[2], w.hasTrig ? "" : "NO ", w.trig[0], w.trig[1], w.trig[2], w.ch.size(), d.c_str());
        if (rgp.world && (int)i == EnvI("H12_BOUNDS", -1)) for (auto &c : w.ch) for (int k = 0; k < 2; ++k) {
            const int ei = k ? c.sec : c.ent; if (ei < 0) continue; const int mi = g_entities[ei].s.index.brushmodel; if (mi <= 0 || mi >= rgp.world->modelCount) continue;
            const GfxBrushModel *bm = &rgp.world->models[mi];
            fprintf(stderr, "[H12] ventana %zu %s e%d *%d org (%.1f %.1f %.1f) ang (%.0f %.0f %.0f) bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f) surfs %u\n", i, k ? "seccion" : "tablon", ei, mi,
                g_entities[ei].r.currentOrigin[0], g_entities[ei].r.currentOrigin[1], g_entities[ei].r.currentOrigin[2], g_entities[ei].r.currentAngles[0], g_entities[ei].r.currentAngles[1], g_entities[ei].r.currentAngles[2],
                bm->bounds[0][0], bm->bounds[0][1], bm->bounds[0][2], bm->bounds[1][0], bm->bounds[1][1], bm->bounds[1][2], bm->surfaceCount);
            for (unsigned q = 0; q < bm->surfaceCount; ++q) { const Material *m = rgp.world->dpvs.surfaces[bm->startSurfIndex + q].material.get();
                if (m) { std::string sb; for (int tt : { 0, 1, 4, 10, 11, 12, 13, 14 }) { char b[48]; if (m->stateBitsEntry[tt] < m->stateBitsCount) { const GfxStateBits *g = &m->stateBitsTable.get()[m->stateBitsEntry[tt]];
                        snprintf(b, sizeof b, " t%d:%08x/%08x", tt, g->loadBits[0], g->loadBits[1]); } else snprintf(b, sizeof b, " t%d:-", tt); sb += b; }
                    fprintf(stderr, "[H12]    superficie %u material %s sortKey %u bits%s\n", q, m->info.name.get(), (unsigned)m->info.drawSurf.fields.primarySortKey, sb.c_str()); } } } }
}
// ---- H12_GLASSORDER (por defecto 1): orden de dibujo del cristal de las ventanas. Las secciones "glass" (unbroken_section) son brushmodels con
// wc/global_glass_frosted, material translucido (primarySortKey 43). En el original R_SortDrawSurfs ordena por primarySortKey y lo translucido
// va DESPUES de lo opaco, actores incluidos: un zombi detras del cristal se ve a traves de el, tintado. El harness dibujaba los brushmodels
// con el mundo (H7C_DrawWorldExtra) y los actores despues, asi que el zombi tapaba el cristal como si estuviera delante. Aqui esas secciones
// se quitan del pase de brushmodels (H7C_BMODEL_SKIP, que ya lee h7c_gfx.cpp) y se dibujan en g_h4DrawExtra tras los actores, con las
// mismas condiciones que DrawBModels (enlazada, sin EF_NODRAW ni SVF_NOCLIENT). H12_GLASSORDER=0 lo desactiva; H12_TRANSKEY cambia el umbral.
void H4_DrawSurfsPlaced(Material *m, const unsigned short *surfs, int n, unsigned char prim, const GfxScaledPlacement *pl, unsigned char techType);
struct H12TransGroup { Material *m; unsigned char prim; std::vector<unsigned short> s; };
static std::vector<std::pair<int, std::vector<H12TransGroup>>> g_trans;   // entidad de la seccion -> grupos por material
static void (*g_h12PrevDraw)(unsigned char);
static GfxScaledPlacement g_transPl[512];
static void H12_DrawTrans(unsigned char techType)
{
    if (g_h12PrevDraw) g_h12PrevDraw(techType);
    int n = 0;
    for (auto &t : g_trans) { if (n >= 512) break;
        gentity_s *ent = &g_entities[t.first];
        if (!ent->r.inuse || ent->s.solid != 0xFFFFFF || (ent->s.lerp.eFlags & 0x20) || !ent->r.linked || (ent->r.svFlags & 1)) continue;
        GfxScaledPlacement *pl = &g_transPl[n++];
        memcpy(pl->base.origin, ent->r.currentOrigin, 12); AnglesToQuat(ent->r.currentAngles, pl->base.quat); pl->scale = 1.0f;
        for (auto &g : t.second) H4_DrawSurfsPlaced(g.m, g.s.data(), (int)g.s.size(), g.prim, pl, techType); }
}
#include <xanim/xmodel_utils.h>
static void H12_DiagSModelBits()
{
    if (!EnvI("H12_SMBITS", 0) || !rgp.world) return;
    std::map<std::string, int> h; const GfxStaticModelDrawInst *di = rgp.world->dpvs.smodelDrawInsts.get();
    for (unsigned i = 0; i < rgp.world->dpvs.smodelCount; ++i) { XModel *mo = di[i].model.get(); if (!mo) continue; PTR32(Material) *sk = XModelGetSkins(mo, 0); if (!sk) continue;
        Material *m = sk[0].get(); if (!m) continue; char b[160]; std::string t;
        for (int tt : { 4, 10, 11, 121 }) { char x[40]; if (m->stateBitsEntry[tt] < m->stateBitsCount) snprintf(x, sizeof x, " t%d:%08x", tt, m->stateBitsTable.get()[m->stateBitsEntry[tt]].loadBits[1]); else snprintf(x, sizeof x, " t%d:-", tt); t += x; }
        snprintf(b, sizeof b, "%s", t.c_str()); h[b]++; }
    for (auto &k : h) fprintf(stderr, "[H12] smodel bits%s : %d\n", k.first.c_str(), k.second);
    if (g_w.size() > 13) for (unsigned i = 0; i < rgp.world->dpvs.smodelCount; ++i) { const float *o = di[i].placement.origin; const float *c = g_w[13].center;
        const float d = sqrtf((o[0] - c[0]) * (o[0] - c[0]) + (o[1] - c[1]) * (o[1] - c[1])); if (d < 260 && di[i].model.get())
            fprintf(stderr, "[H12] smodel %u %s en (%.0f %.0f %.0f) a %.0f u de la ventana 13\n", i, di[i].model.get()->name.get(), o[0], o[1], o[2], d); }
}
static void H12_SetupTransOrder()
{
    H12_DiagSModelBits();
    if (!EnvI("H12_GLASSORDER", 1) || !rgp.world) return;
    const unsigned key = (unsigned)EnvI("H12_TRANSKEY", 32);
    std::string skip = getenv("H7C_BMODEL_SKIP") ? getenv("H7C_BMODEL_SKIP") : ",";
    if (skip.empty() || skip.back() != ',') skip += ",";
    // H12_LATEBOARDS=1 (diagnostico): tambien los tablones van al pase tardio, para comprobar la profundidad tablon/zombi
    const bool lateBoards = EnvI("H12_LATEBOARDS", 0) != 0;
    for (auto &w : g_w) for (auto &c : w.ch) for (int k = 0; k < (lateBoards ? 2 : 1); ++k) { const int ei = k ? c.ent : c.sec; if (ei < 0) continue;
        const int mi = g_entities[ei].s.index.brushmodel; if (mi <= 0 || mi >= rgp.world->modelCount) continue;
        const GfxBrushModel *bm = &rgp.world->models[mi]; bool trans = false; std::vector<H12TransGroup> gr;
        for (unsigned k = 0; k < bm->surfaceCount; ++k) { const int si = bm->startSurfIndex + k; GfxSurface *sf = &rgp.world->dpvs.surfaces[si]; Material *m = sf->material.get();
            if (!m || !sf->tris.triCount) continue; if (m->info.drawSurf.fields.primarySortKey >= key) trans = true;
            auto it = std::find_if(gr.begin(), gr.end(), [&](const H12TransGroup &x) { return x.m == m && x.prim == sf->primaryLightIndex; });
            if (it == gr.end()) { gr.push_back({ m, sf->primaryLightIndex, {} }); it = gr.end() - 1; } it->s.push_back((unsigned short)si); }
        if (!trans && !k) continue;
        std::stable_sort(gr.begin(), gr.end(), [](const H12TransGroup &a, const H12TransGroup &b) { return a.m->info.drawSurf.fields.primarySortKey < b.m->info.drawSurf.fields.primarySortKey; });
        g_trans.push_back({ ei, gr }); skip += std::to_string(mi) + ","; }
    if (g_trans.empty()) return;
    setenv("H7C_BMODEL_SKIP", skip.c_str(), 1);
    // OLA 15B: en el pase translucido (world_h4.cpp g_h4DrawTranslucent), antes del viewmodel. Antes envolvia g_h4DrawExtra y quedaba
    // DESPUES del viewmodel, que borra la profundidad: los cristales de cualquier ventana se veian a traves de suelos y paredes.
    extern void (*g_h4DrawTranslucent)(unsigned char techType); extern void (*g_h4DrawExtra)(unsigned char techType);
    if (EnvI("H15_OLDORDER", 0)) { g_h12PrevDraw = g_h4DrawExtra; g_h4DrawExtra = H12_DrawTrans; }   // diagnostico: orden anterior (tras el viewmodel) para comparar
    else { g_h12PrevDraw = g_h4DrawTranslucent; g_h4DrawTranslucent = H12_DrawTrans; }
    fprintf(stderr, "[H12] orden translucido: %zu secciones de cristal se dibujan despues de los actores y antes del viewmodel (H12_GLASSORDER=0 lo quita)\n", g_trans.size());
}

// estado visible de un tablon: V = se dibuja, h = oculto (Hide: EF_NODRAW 0x20), x = borrado; m = fuera de su sitio. Seccion intacta: U/u/-
static std::string ChunkState(H12Chunk &c, bool *moved)
{
    const gentity_s *e = &g_entities[c.ent];
    std::string s = !e->r.inuse ? "x" : (e->s.lerp.eFlags & 0x20) ? "h" : "V";
    float d = 0; for (int k = 0; k < 3; ++k) d += (e->r.currentOrigin[k] - c.og[k]) * (e->r.currentOrigin[k] - c.og[k]);
    *moved = d > 1.0f; if (*moved) s += "m";
    if (c.sec >= 0) { const gentity_s *u = &g_entities[c.sec];
        const bool alive = u->r.inuse && u->targetname && c.tgt == S(u->targetname);
        s += !alive ? "-" : (u->s.lerp.eFlags & 0x20) ? "u" : "U"; if (!alive) c.sec = -1; }
    else s += "-";
    return s;
}

// ---- anims scripted de los zombis y notetracks
static std::map<int, unsigned> g_actAnim;
static const char *AnimName(gentity_s *e, unsigned idx)
{
    XAnimTree_s *tree = GScr_GetEntAnimTree(e); if (!tree) return "?";
    return XAnimGetAnimDebugName(XAnimGetAnims(tree), idx);
}
static int NearWin(const float *p, float *dist)
{
    int b = -1; float bd = 1e30f;
    for (size_t i = 0; i < g_w.size(); ++i) { float dx = p[0] - g_w[i].center[0], dy = p[1] - g_w[i].center[1], dz = p[2] - g_w[i].center[2]; float d = sqrtf(dx * dx + dy * dy + dz * dz); if (d < bd) { bd = d; b = (int)i; } }
    *dist = bd; return b;
}
static bool g_h12log;
void H12_NoteHook(int entnum, unsigned notifyName, unsigned noteName);
void (*g_h12NoteHook)(int, unsigned, unsigned);
static int g_storyWin = -1, g_storyTraverseT = -1;
void H12_NoteHook(int entnum, unsigned notifyName, unsigned noteName)
{
    if (entnum < 0 || entnum >= level.num_entities) return;
    const char *nn = S((unsigned short)notifyName);
    if (g_storyWin >= 0 && g_storyTraverseT < 0 && !strcmp(nn, "traverseAnim")) { float d; if (NearWin(g_entities[entnum].r.currentOrigin, &d) == g_storyWin && d < 90) g_storyTraverseT = level.time; }
    if (!g_h12log) return;
    if (!strstr(nn, "tear") && !strstr(nn, "window") && !strstr(nn, "taunt") && !strstr(nn, "traverse") && !strstr(nn, "jump") && !getenv("H12_NOTEALL")) return;
    gentity_s *e = &g_entities[entnum]; float d; const int w = NearWin(e->r.currentOrigin, &d);
    fprintf(stderr, "[H12N] t=%d ent %d %s: nota '%s' (ventana %d a %.0f u)\n", level.time, entnum, nn, S((unsigned short)noteName), w, d);
}

// ---- prueba de reponer: entrada del jugador por H5PlayerInput (la misma API que la app)
static int g_repWin = -1, g_repFrom = 0; static bool g_repOn;
// ---- H12_GLASSSHOOT=i[,t]: prueba de los cristales Glass del motor (svGlasses, 46 en Pentagon, glass_dec_glass02). Desde t (5000 ms) el
// jugador se coloca a 110 u del panel i por su cara delgada, mirandolo, y dispara a golpes (la bala original: bullet.cpp -> GlassSv_Damage).
// Se registran los cambios de estado del panel ([H12G]) y se capturan frames tras romperse.
static std::vector<std::pair<int, std::string>> g_pendG;
static int g_gsGlass = -1, g_gsFrom = 5000; static bool g_gsFire, g_gsAimOn; static float g_gsAim[3];
void H12_GlassClFrame();
void H15E_Frame();
static void H12_GlassShootFrame()
{
    if (g_gsGlass < 0 || g_gsGlass >= (int)svGlasses.numGlasses || !rgp.world) return;
    const GlassServer *gs = &svGlasses.glasses[g_gsGlass]; const Glass *g = gs->glass; if (!g) return;
    static bool placed = false; static unsigned last = 99; static float aimAng[3];
    gentity_s *pl = &g_entities[0]; const int t = level.time;
    if (!placed && t >= g_gsFrom && pl->r.inuse && pl->client && (int)g->brushModel > 0 && (int)g->brushModel < rgp.world->modelCount) {
        const GfxBrushModel *bm = &rgp.world->models[g->brushModel]; int thin = 0; float ext[3];
        for (int k = 0; k < 3; ++k) ext[k] = bm->bounds[1][k] - bm->bounds[0][k];
        thin = ext[0] < ext[1] ? 0 : 1;   // eje horizontal mas delgado = normal del panel
        static const int side = EnvI("H12_GLASSSIDE", 1);
        float c[3]; for (int k = 0; k < 3; ++k) c[k] = g->origin[k] + 0.5f * (bm->bounds[0][k] + bm->bounds[1][k]);   // centro del panel (bounds locales)
        float o[3] = { c[0], c[1], 26 }; o[thin] += 110.0f * side;
        const float eye[3] = { o[0], o[1], o[2] + 60 }, d[3] = { c[0] - eye[0], c[1] - eye[1], c[2] - eye[2] };
        fprintf(stderr, "[H12G] panel %d: centro (%.0f %.0f %.0f) tamano (%.0f %.0f %.0f)\n", g_gsGlass, c[0], c[1], c[2], ext[0], ext[1], ext[2]);
        aimAng[0] = -atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) * 57.29578f; aimAng[1] = atan2f(d[1], d[0]) * 57.29578f; aimAng[2] = 0;
        TeleportPlayer(pl, o, aimAng); placed = true; memcpy(g_gsAim, aimAng, 12); g_gsAimOn = true;
        fprintf(stderr, "[H12G] t=%d jugador a 110 u del cristal %d (%s) por el eje %d en (%.0f %.0f %.0f), mirando (%.1f %.1f); salud del panel %.0f\n", t, g_gsGlass, g->glassDef->name.get(), thin, o[0], o[1], o[2], aimAng[0], aimAng[1], gs->health);
    }
    const unsigned st = gs->state.val.i & 0xF;
    if (st != last) { fprintf(stderr, "[H12G] t=%d cristal %d estado %u (%s) salud %.0f\n", t, g_gsGlass, st, st == 0 ? "intacto" : st == 1 ? "agrietado" : st == 2 ? "roto" : "?", gs->health);
        if (last != 99 && H4_Shot) { static const int dts[5] = { 0, 50, 150, 400, 1200 }; for (int k = 0; k < 5; ++k) { char tag[48]; snprintf(tag, sizeof tag, "_g%d_s%u_%d", g_gsGlass, st, k); g_pendG.push_back({ t + dts[k], tag }); } }
        last = st; }
    g_gsFire = placed && st < 2 && t >= g_gsFrom + 1500 && ((t / 200) & 1);
}
static void H12_Input(H5PlayerInput *in, float)
{
    memset(in, 0, sizeof *in);
    if (g_repOn) in->buttons |= H5IN_USE;
    if (g_gsFire) in->buttons |= H5IN_FIRE;
    if (g_gsAimOn && g_entities[0].client) {   // la capa de entrada lleva su propia vista: se gira con lookYaw/lookPitch hasta el angulo pedido
        const float *va = g_entities[0].client->ps.viewangles;
        float dy = g_gsAim[1] - va[1]; while (dy > 180) dy -= 360; while (dy < -180) dy += 360;
        float dp = g_gsAim[0] - va[0]; while (dp > 180) dp -= 360; while (dp < -180) dp += 360;
        in->lookYaw = dy; in->lookPitch = dp; }
}

// ---- camara y capturas
static int g_camWin = -2; static float g_camFix[5]; static bool g_camFixOn;
static int g_shotLeft, g_shotEvery; static std::vector<std::pair<int, std::string>> g_pend;
static void CamForWin(int wi, float *org, float *ang)
{
    const H12Win &w = g_w[wi]; float fwd[3]; const float a[3] = { 0, w.ang[1], 0 }; AngleVectors(a, fwd, nullptr, nullptr);
    // el exterior_goal mira hacia dentro del edificio (yaw del struct): camara dentro, a 150 u del centro de los tablones
    static const float dist = getenv("H12_CAMDIST") ? (float)atof(getenv("H12_CAMDIST")) : 150.0f;
    // H12_CAMSIDE / H12_CAMUP: desplazamiento lateral y vertical; la camara sigue mirando al centro de la ventana
    static const float side = getenv("H12_CAMSIDE") ? (float)atof(getenv("H12_CAMSIDE")) : 0.0f, up = getenv("H12_CAMUP") ? (float)atof(getenv("H12_CAMUP")) : 6.0f;
    const float right[2] = { fwd[1], -fwd[0] };
    org[0] = w.center[0] + fwd[0] * dist + right[0] * side; org[1] = w.center[1] + fwd[1] * dist + right[1] * side; org[2] = w.center[2] + up;
    const float d[3] = { w.center[0] - org[0], w.center[1] - org[1], w.center[2] - org[2] };
    ang[1] = atan2f(d[1], d[0]) * 57.29578f; ang[0] = -atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) * 57.29578f; ang[2] = 0;
}


// ---- H12_STORY=N: secuencia rotulada de una ventana (A..H) para la evidencia. Cada captura lleva un rotulo dibujado con la lista 2D
// del renderer (H5P_DrawLabel) en el mismo frame que se captura, y una linea [H12S] con tiempo, tablones en pie y paneles intactos.
void H5P_DrawRect(float x, float y, float w, float h, const float *rgba);
void H5P_DrawLabel(const char *t, float x, float y, float scale, const float *rgba);
static std::string g_label; static void (*g_prev2D)(int, int);
static void H12_Draw2D(int W, int H)
{
    if (g_prev2D) g_prev2D(W, H);
    if (g_label.empty()) return;
    const float sc = H / 720.0f, bg[4] = { 0, 0, 0, 0.72f }, fg[4] = { 1, 0.95f, 0.6f, 1 };
    H5P_DrawRect(0, H - 64 * sc, W, 64 * sc, bg);
    H5P_DrawLabel(g_label.c_str(), 24 * sc, H - 20 * sc, 1.3f * sc, fg);
}
struct H12Shot { int t; std::string id, text; };
static std::vector<H12Shot> g_story; static int g_armed = -1;
static void StoryAdd(int t, const char *id, const char *text) { g_story.push_back({ t, id, text }); fprintf(stderr, "[H12S] programada %s en t=%d: %s\n", id, t, text); }
static void StoryCounts(const H12Win &w, int *boards, int *panes)
{
    *boards = *panes = 0;
    for (const auto &c : w.ch) { const gentity_s *e = &g_entities[c.ent];
        float d = 0; for (int k = 0; k < 3; ++k) d += (e->r.currentOrigin[k] - c.og[k]) * (e->r.currentOrigin[k] - c.og[k]);
        if (e->r.inuse && !(e->s.lerp.eFlags & 0x20) && d < 1.0f) ++*boards;
        if (c.sec >= 0) ++*panes; }
}
static void StoryFrame()
{
    static int stage = 0, boards0 = -1, repStart = -1;
    H12Win &w = g_w[g_storyWin]; int boards, panes; StoryCounts(w, &boards, &panes);
    const int t = level.time;
    auto zombieAt = [&]() { for (int e = 0; e < level.num_entities; ++e) { gentity_s *a = &g_entities[e]; if (!a->r.inuse || a->s.eType != ET_ACTOR) continue;
        const scripted_anim_sp_t *sa = G_SP_GetScriptedAnim(a); float d; if (sa && sa->anim && NearWin(a->r.currentOrigin, &d) == g_storyWin && d < 100) return true; } return false; };
    // H12_PARK=f,s: mientras no repone, el jugador espera dentro de la sala en centro + f*delante + s*derecha (fuera del encuadre),
    // para que los zombis que ya han entrado no tapen la ventana al ir a por el
    static bool parked = false;
    if (!parked && t >= 3000 && getenv("H12_PARK")) { float pf = 0, ps = 0; sscanf(getenv("H12_PARK"), "%f,%f", &pf, &ps); gentity_s *pl = &g_entities[0];
        if (pl->r.inuse && pl->client) { float fwd[3]; const float a0[3] = { 0, w.ang[1], 0 }; AngleVectors(a0, fwd, nullptr, nullptr);
            float o[3] = { w.center[0] + fwd[0] * pf + fwd[1] * ps, w.center[1] + fwd[1] * pf - fwd[0] * ps, w.trig[2] + 2 }, a[3] = { 0, w.ang[1] + 180, 0 };
            TeleportPlayer(pl, o, a); parked = true; fprintf(stderr, "[H12S] jugador aparcado en (%.0f %.0f %.0f)\n", o[0], o[1], o[2]); } }
    if (stage == 0 && t >= EnvI("H12_A_T", 12000)) { StoryAdd(t, "A", "A: ventana intacta (cristal entero, sin tablones: inicio de Pentagon)"); stage = 1; }
    if (stage == 1 && zombieAt()) { StoryAdd(t + 150, "B", "B: el zombi llega y golpea el cristal (ai_zombie_door_pound)"); stage = 2; }
    if (stage == 2 && panes < 6) { StoryAdd(t + 100, "C", "C: el zombi rompe el 1er panel (nota board: efecto glass_break)"); stage = 3; }
    if (stage == 3 && panes == 0) { StoryAdd(t + 500, "D", "D: los 6 paneles rotos, sin tablones: hueco abierto"); stage = 4; }
    if (stage == 4 && g_storyTraverseT >= 0) { StoryAdd(g_storyTraverseT + 350, "E", "E: el zombi cruza el hueco (traverseAnim)"); stage = 5; }
    if (stage == 5 && t >= g_storyTraverseT + 2500) { g_repWin = g_storyWin; g_repFrom = t; repStart = t; boards0 = boards; stage = 6; }
    if (stage == 6 && repStart >= 0 && boards >= 1) { StoryAdd(t + 250, "F1", "F: el jugador mantiene USAR: 1er tablon puesto, +10 puntos"); stage = 7; }
    if (stage == 7 && boards >= 3) { StoryAdd(t + 250, "F2", "F: reponiendo: 3 tablones en pie, +10 por tablon"); stage = 8; }
    if (stage == 8 && boards == 6) { StoryAdd(t + 400, "G", "G: ventana completa otra vez: 6 tablones"); stage = 9; }
    if (stage == 9 && boards < 6) { StoryAdd(t + 150, "H1", "H: un zombi arranca un tablon repuesto (boardtear)"); StoryAdd(t + 450, "H2", "H: el tablon sale despedido (fake_physicslaunch)"); stage = 10; }
    (void)boards0;
    // capturas: el rotulo se pone una vuelta antes (ese frame se dibuja con el) y en la siguiente se guarda el frame presentado
    if (g_armed >= 0) { const H12Shot &s = g_story[g_armed]; char tag[32]; snprintf(tag, sizeof tag, "_h12_%s", s.id.c_str()); if (H4_Shot) H4_Shot(tag);
        const gentity_s *pl = &g_entities[0];
        fprintf(stderr, "[H12S] %s t=%d tablones en pie %d/6, paneles de cristal %d/6, puntos %d: %s\n", s.id.c_str(), t, boards, panes, pl->client ? pl->client->sess.cs.score.score : -1, s.text.c_str());
        g_story.erase(g_story.begin() + g_armed); g_armed = -1; g_label.clear(); }
    for (size_t k = 0; k < g_story.size(); ++k) if (t >= g_story[k].t) { g_armed = (int)k; g_label = g_story[k].text; break; }
}

void H12_Frame()
{
    static int f = 0; ++f;
    if (f < 3) return;
    if (!g_init) {
        // OLA 15E: H12_SEED=n fija la semilla de G_rand (RandomInt/RandomFloat del GSC; el servidor la siembra con el reloj en
        // SV_SpawnServer). Con el paso fijo del arnes la partida se repite igual: asi se elige la ventana a la que va el primer zombi.
        if (const char *sd = getenv("H12_SEED")) { void __cdecl G_srand(unsigned int); G_srand((unsigned)strtoul(sd, nullptr, 10)); fprintf(stderr, "[H12] semilla G_rand fija: %s\n", sd); }
        H12_Init(); H12_SetupTransOrder(); g_h12log = EnvI("H12_LOG", 0) != 0; if (g_h12log) g_h12NoteHook = H12_NoteHook;
        g_shotLeft = EnvI("H12_SHOTS", 0); g_shotEvery = EnvI("H12_SHOT_EVERY", 0);
        if (const char *c = getenv("H12_CAM")) { if (c[0] == 'w') g_camWin = atoi(c + 1); else if (!strcmp(c, "auto")) g_camWin = -1;
            else if (sscanf(c, "%f,%f,%f,%f,%f", &g_camFix[0], &g_camFix[1], &g_camFix[2], &g_camFix[3], &g_camFix[4]) == 5) g_camFixOn = true; }
        if (const char *st = getenv("H12_STORY")) { g_storyWin = atoi(st); g_camWin = g_storyWin; g_h12NoteHook = H12_NoteHook; extern void (*g_h5pInputProvider)(H5PlayerInput *, float); g_h5pInputProvider = H12_Input;
            extern void (*g_h4Draw2D)(int, int); g_prev2D = g_h4Draw2D; g_h4Draw2D = H12_Draw2D; }
        if (const char *gsx = getenv("H12_GLASSSHOOT")) { g_gsGlass = atoi(gsx); const char *c = strchr(gsx, ','); if (c) g_gsFrom = atoi(c + 1); extern void (*g_h5pInputProvider)(H5PlayerInput *, float); g_h5pInputProvider = H12_Input; }
        if (const char *r = getenv("H12_REPAIR")) { g_repWin = atoi(r); const char *c = strchr(r, ','); g_repFrom = c ? atoi(c + 1) : 0; extern void (*g_h5pInputProvider)(H5PlayerInput *, float); g_h5pInputProvider = H12_Input; }
    }
    gentity_s *pl = &g_entities[0];
    H15E_Frame();   // OLA 15E: partida guionizada del HUD (h15e_gameplay.cpp, solo con EXTRA; si no, la debil de abajo)
    if (g_storyWin >= 0 && g_storyWin < (int)g_w.size()) StoryFrame();
    H12_GlassShootFrame();
    H12_GlassClFrame();   // cliente de cristal original (h12_glasscl.cpp, solo con H12GLASS=1 en build.sh)
    for (size_t k = 0; k < g_pendG.size();) { if (level.time >= g_pendG[k].first) { if (H4_Shot) H4_Shot(g_pendG[k].second.c_str()); g_pendG.erase(g_pendG.begin() + k); } else ++k; }
    // H12_GOD: modo dios del motor (FL_GODMODE, como el comando god) para que el jugador no caiga mientras se graba la secuencia
    if (EnvI("H12_GOD", 0) && pl->r.inuse && pl->client) { pl->flags |= 0x1; if (pl->health > 0 && pl->health < 100) pl->health = 100; }
    // ventanas
    for (size_t i = 0; i < g_w.size(); ++i) {
        H12Win &w = g_w[i]; std::string st; int down = 0;
        for (auto &c : w.ch) { bool mv; std::string s = ChunkState(c, &mv); st += s + " "; if (s[0] != 'V' || mv) ++down;
            if (g_h12log && s != c.last && !c.last.empty()) { const gentity_s *e = &g_entities[c.ent];
                fprintf(stderr, "[H12W] t=%d ventana %zu tablon %s (e%d): %s -> %s en (%.1f %.1f %.1f) ang (%.0f %.0f %.0f) [orig (%.0f %.0f %.0f)]\n", level.time, i, c.sn.c_str(), c.ent, c.last.c_str(), s.c_str(),
                    e->r.currentOrigin[0], e->r.currentOrigin[1], e->r.currentOrigin[2], e->r.currentAngles[0], e->r.currentAngles[1], e->r.currentAngles[2], c.og[0], c.og[1], c.og[2]); }
            c.last = s; }
        if (st != w.last) {
            if (!w.last.empty()) { g_lastEventWin = (int)i; g_lastEventT = level.time;
                if (g_h12log) fprintf(stderr, "[H12W] t=%d ventana %zu: %s(%d fuera de sitio) pts=%d\n", level.time, i, st.c_str(), down, pl->client ? pl->client->sess.cs.score.score : -1);
                if (g_shotLeft > 0 && ((g_camWin == -1) || g_camWin == (int)i)) { --g_shotLeft; char t[64];
                    const int dts[4] = { 0, 150, 400, 800 }; for (int k = 0; k < 4; ++k) { snprintf(t, sizeof t, "_w%zu_t%06d_%d", i, level.time, k); g_pend.push_back({ level.time + dts[k], t }); } } }
            w.last = st; }
        // reponer
        if ((int)i == g_repWin && level.time >= g_repFrom && w.hasTrig) {
            const bool need = down > 0;
            if (need && !g_repOn && pl->r.inuse && pl->client) {
                float o[3] = { w.trig[0], w.trig[1], w.trig[2] + 2 }, a[3] = { 10, w.ang[1] + 180, 0 };
                TeleportPlayer(pl, o, a); g_repOn = true;
                fprintf(stderr, "[H12R] t=%d jugador al trigger de la ventana %zu (%.0f %.0f %.0f), USAR mantenido; pts=%d\n", level.time, i, o[0], o[1], o[2], pl->client->sess.cs.score.score);
            }
            if (g_repOn && !need) { g_repOn = false; fprintf(stderr, "[H12R] t=%d ventana %zu completa; suelta USAR; pts=%d\n", level.time, i, pl->client ? pl->client->sess.cs.score.score : -1); }
        }
    }
    // OLA 15E: H12_TRACK=N -> seguimiento del vuelo de los tablones de la ventana N (o la de H12_STORY/H12_REPAIR): cada frame en que un
    // tablon se mueve (MoveTo/RotateTo del GSC: replace_chunk / tear) se registra [H12T] con origen y angulos; H12_TRACKSHOTS=n hace
    // hasta n capturas, una cada H12_TRACKMS (60) ms de juego mientras haya movimiento, desde H12_TRACKFROM (ms de juego, 0 por defecto).
    {
        static const int trackWin = getenv("H12_TRACK") ? atoi(getenv("H12_TRACK")) : -1; static int shotsLeft = EnvI("H12_TRACKSHOTS", 0), lastShot = -100000;
        static std::map<int, std::array<float, 6>> prev;
        if (trackWin >= 0 && trackWin < (int)g_w.size()) { bool moving = false;
            for (auto &c : g_w[trackWin].ch) { const gentity_s *e = &g_entities[c.ent]; if (!e->r.inuse) continue;
                std::array<float, 6> cur = { e->r.currentOrigin[0], e->r.currentOrigin[1], e->r.currentOrigin[2], e->r.currentAngles[0], e->r.currentAngles[1], e->r.currentAngles[2] };
                auto it = prev.find(c.ent); bool ch = it == prev.end(); if (!ch) for (int k = 0; k < 6; ++k) ch |= fabsf(cur[k] - it->second[k]) > 0.01f;
                if (ch && it != prev.end()) { moving = true; float d = 0; for (int k = 0; k < 3; ++k) d += (cur[k] - c.og[k]) * (cur[k] - c.og[k]);
                    fprintf(stderr, "[H12T] t=%d ventana %d tablon %s (e%d) %s origen (%.1f %.1f %.1f) ang (%.0f %.0f %.0f) a %.1f u de su sitio trType %d\n", level.time, trackWin, c.sn.c_str(), c.ent,
                        (e->s.lerp.eFlags & 0x20) ? "oculto" : "visible", cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], sqrtf(d), (int)e->s.lerp.pos.trType); }
                prev[c.ent] = cur; }
            if (moving && shotsLeft > 0 && level.time >= EnvI("H12_TRACKFROM", 0) && level.time - lastShot >= EnvI("H12_TRACKMS", 60) && H4_Shot) { --shotsLeft; lastShot = level.time; char t[48]; snprintf(t, sizeof t, "_trk_t%06d", level.time); H4_Shot(t); }
        }
    }
    // zombis: anim scripted
    if (g_h12log) for (int e = 0; e < level.num_entities; ++e) {
        gentity_s *a = &g_entities[e]; if (!a->r.inuse || a->s.eType != ET_ACTOR) { g_actAnim.erase(e); continue; }
        const scripted_anim_sp_t *sa = G_SP_GetScriptedAnim(a); const unsigned an = sa ? sa->anim : 0;
        auto it = g_actAnim.find(e); if (it != g_actAnim.end() && it->second == an) continue;
        g_actAnim[e] = an; if (!an) continue;
        float d; const int w = NearWin(a->r.currentOrigin, &d);
        fprintf(stderr, "[H12A] t=%d zombi %d animscripted '%s' en (%.0f %.0f %.0f) yaw %.0f; ventana %d a %.0f u\n", level.time, e, AnimName(a, an), a->r.currentOrigin[0], a->r.currentOrigin[1], a->r.currentOrigin[2], a->r.currentAngles[1], w, d);
    }
    // camara
    float org[3], ang[3];
    if (g_camFixOn) { memcpy(org, g_camFix, 12); ang[0] = g_camFix[4]; ang[1] = g_camFix[3]; ang[2] = 0; H4_SetCam(org, ang); }
    else if (g_camWin >= 0 && g_camWin < (int)g_w.size()) { CamForWin(g_camWin, org, ang); H4_SetCam(org, ang); }
    else if (g_camWin == -1 && g_lastEventWin >= 0) { CamForWin(g_lastEventWin, org, ang); H4_SetCam(org, ang); }
    // capturas (H4_Shot lee el ultimo frame presentado: el de la vuelta anterior)
    for (size_t k = 0; k < g_pend.size();) { if (level.time >= g_pend[k].first) { if (H4_Shot) H4_Shot(g_pend[k].second.c_str()); fprintf(stderr, "[H12] captura %s\n", g_pend[k].second.c_str()); g_pend.erase(g_pend.begin() + k); } else ++k; }
    static int lastEvery = 0;
    if (g_shotEvery > 0 && level.time - lastEvery >= g_shotEvery) { lastEvery = level.time; char t[48]; snprintf(t, sizeof t, "_e_t%06d", level.time); if (H4_Shot) H4_Shot(t); }
}

// world_h4.cpp llama (debil) a H7E_Setup de la OLA 7e (h7e_light.cpp, aun fuera de build.sh): sin esta definicion debil genstubs
// la convierte en un stub que aborta. Cuando 7e entre en build.sh, su definicion fuerte sustituye a esta.
__attribute__((weak)) void H15E_Frame() {}   // h15e_gameplay.cpp (OLA 15E, EXTRA en build.sh)
__attribute__((weak)) void H12_GlassClFrame() {}   // fuerte en h12_glasscl.cpp (build.sh H12GLASS=1)
__attribute__((weak)) void H12_GlassGenerateVerts() {}   // idem; la llama h5_fx.cpp antes de subir el codemesh
struct GfxWorld; __attribute__((weak)) void H7E_Setup(GfxWorld *) {}
__attribute__((weak)) bool H13_DrawHud(int, int) { return false; }   // la ola 13 la define si se compila con H13=1; sin ella genstubs pondria un stub que aborta
__attribute__((weak)) void Sys_HeadlessTimeline(const char *) {}   // la define win_main.cpp (ola 11) cuando se enlaza; aqui solo evita el stub que aborta
__attribute__((weak)) void H7E_ClientFrame() {}   // h7e_light.cpp (ola 7e) no esta en build.sh: sin esto genstubs pondria un stub que aborta
__attribute__((weak)) void H7E_DrawClientModels(unsigned char) {}   // idem (ola 7e, h7e_light.cpp)
__attribute__((weak)) void H7E_PrepareClientModels() {}
