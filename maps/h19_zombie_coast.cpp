// OLA M8: cliente de zombie_coast (Call of the Dead). Reproduce sin VM de CSC lo que hacen sus clientscripts (rawfiles del ff), con sus
// mismos numeros. El servidor es el GSC original sin tocar: corriente (use_elec_switch), puertas y escombros, caja que se mueve, 7 ventajas,
// faro con Pack-a-Punch que cambia de sitio, tirolina, lanzador (flinger), torretas, agua helada y el jefe George Romero (director).
// Call of the Dead NO tiene perros (level.dogs_enabled = false; custom_ai_type = director + faller).
//  - vision (zombie_coast.csc): pila de zombie_vision_set_apply/remove de _zombiemode.csc (gana la prioridad mas alta; VisionSetNaked con
//    el tiempo de la llamada): zombie_coast_2 (1) al conectar; zombie_coast_lighthouse (2, 5 s) con "FI" / quitar con "FO";
//    zombie_coast_powerOn (5) con "ZPO" (power_on_burst: 0,1 s y quitar en 6 s); zombie_coast_rovingEye (7) con "lhfo" (0,1 s, 1,5 s).
//  - niebla (zombie_coast_fx.csc): coast_fog_blizzard es el callback del clientflag 2 (level._COAST_FOG_BLIZZARD) del JUGADOR (ps.eFlags2):
//    con 1 la niebla de la zona donde esta (exterior / faro / proa / popa / hielo, 10 s); con 0 la niebla "clara" (10 s). Los disparadores de
//    zona (lighthouseFog_interior, shipFrontDeck_interior, shipMedium_interior, shipIce_interior, exteriorfog_trigger) ponen .inside y, con
//    ventisca, cambian la niebla al entrar (fog_trigger + trigger_thread). setVolFog de cliente por H7C_ClientVolFog (h7c_gfx.cpp).
//  - createfx (clientscripts/createfx/zombie_coast_fx.csc + zombie_coast_fx.csc): 217 oneshotfx y 84 FX de exploder (101/102 ventisca,
//    110 fin de ventisca, 201-203 y 211-213 Pack-a-Punch, 301 faro al dar la corriente, 310 rayos, 900 agua del jefe, 5xx/7xx huevos).
//    Se leen del rawfile, no se copian; los exploders llegan como sysState("@exploder", id) (h7e_light.cpp, OLA M6).
//  - faro (zombie_coast_lighthouse.csc): con "LHL" FX fx_zmb_coast_spotlight_main en pack_light girando (rotateto +5 grados en 0,2 s cada
//    0,1 s = 25 grados/s); "pp0"/"pp1"/"pp2" lo apuntan en 1 s a su pap_location y se queda quieto hasta "PPH" (vuelve a pitch/roll 0 en
//    1 s y sigue girando); "lhfo" lo vuelve loco (rotateto aleatorio de 0,5-1 s) hasta "lhfd", con freakout_sun_reaction (SetSunLight
//    aleatorio 1,5 s). El FX no tiene DObj al que atornillarse: se gira escribiendo frameNow del efecto (lo que hace FX_UpdateEffectBolt).
//  - corriente ("ZPO"): coast_sun_light_flashes (secuencia de SetSunLight del original) y power_on_burst.
//  - solaris_flash (huevo: "lmc" .. "slc"): codigo Morse con el FX lighthouse_morse_code en struct_musical_chairs_lighthouse y pitidos.
//  - manage_player_rain: copos (player_rain) en el jugador cada 0,3 s durante RandomFloat(10, 30) s, y descanso igual.
//  - flare_effects: FX flare / flare_no_dlight (script_int 0/1) en tag_fx de los coast_flare.
//  - zombie_coast_amb.csc: salas, reverb y musica los lleva h5_audio_cl (ParseAmb). Aqui: play_poweron_sounds y lighthouse_looper ("LHL"),
//    indoor_outdoor_transitions (46 blizzard_transitions: amb_blizzard_rand_heavy/light cada 1-1,5 s segun la ventisca) y
//    snd_play_auto_fx (amb_fire en los fx_fire_line_xsm).
// No se hace (ver docs): escarcha de pantalla (filtro frost de _filter.csc), FX de electrocutado en zombis/jugador (clientflag de actor),
// luz y pasos del director, jugador falso de tirolina/lanzador (animaciones de cliente), fxanims del barco (grua y botes), snd_futz del director.
//   H19_COAST=0 lo apaga. Pruebas: H19_COAST_NOTIFY="ms:ev,..." inyecta levelNotify de cliente en ese level.time (como el servidor).
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <database/db_registry.h>
#include <game_mp/g_main_mp.h>
#include <server_mp/server_mp.h>
#include <EffectsCore/fx_system.h>
#include <gfx_d3d/r_bsp.h>
#include <xanim/xmodel.h>
#include <xanim/xmodel_utils.h>
#include <gfx_d3d/r_model_skin.h>
#include <clientscript/cscr_stringlist.h>
#include <qcommon/cm_trace.h>
#include <qcommon/cm_tracebox.h>
#include <qcommon/cm_load.h>
#ifdef H6SND
#include <sound/snd_bank.h>
#include <sound/snd_public_async.h>
#endif
#include <zlib.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "h19_mapcl.h"

bool H7_VisionSetNaked(const char *name, float seconds) __attribute__((weak));   // h7_gfx.cpp
void H7C_ClientVolFog(const float *v, int transitionMs) __attribute__((weak));     // h7c_gfx.cpp: setVolFog del CSC
void H4_GetCam(float *org, float *ang);                                         // world_h4.cpp
GfxWorld *H4_FindWorld();                                                       // world_h4.cpp
const FxEffectDef *__cdecl FX_Register(char *name);                             // h5_fx.cpp
extern FxSystemContainer *fx_systemPool;                                        // fx_system.cpp (lo reserva h5_fx)
FxEffectContainer *__cdecl UniqueHandleToEffect(FxSystem *system, unsigned int handle);   // fx_system.cpp (el handle de FX_PlayOrientedEffect)

namespace {
// ===================================================================================================== utilidades
std::string Raw(const char *name)   // rawfile de script descomprimido ([len][zlen][zlib])
{
    RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)name, 0, -1).rawfile;
    if (!rf || !rf->buffer.get()) return {};
    const unsigned *hd = (const unsigned *)rf->buffer.get(); uLongf outlen = hd[0];
    std::vector<unsigned char> out(outlen + 1);
    if (uncompress(out.data(), &outlen, (const unsigned char *)(hd + 2), hd[1]) != Z_OK) return {};
    return std::string((const char *)out.data(), outlen);
}
std::string Lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
bool Quoted(const std::string &t, size_t p, std::string &out, size_t *end = nullptr)
{
    size_t a = t.find('"', p); if (a == std::string::npos) return false;
    size_t b = t.find('"', a + 1); if (b == std::string::npos) return false;
    out = t.substr(a + 1, b - a - 1); if (end) *end = b + 1; return true;
}
float AngDelta(float a, float b) { float d = fmodf(a - b, 360.0f); if (d > 180.0f) d -= 360.0f; if (d < -180.0f) d += 360.0f; return d; }   // AngleDelta

struct Pt { float org[3], ang[3]; std::string ss; };
std::map<std::string, std::vector<Pt>> s_structs;   // script_struct por targetname (getstructarray)
void LoadStructs()
{
    if (!s_structs.empty()) return;
    for (const H19Kv &k : H19_MapEnts()) {
        const char *cn = H19_Str(k, "classname"), *tn = H19_Str(k, "targetname");
        if (!cn || !tn || strcmp(cn, "script_struct")) continue;
        Pt p; memset(p.org, 0, 12); memset(p.ang, 0, 12); H19_Vec(k, "origin", p.org); H19_Vec(k, "angles", p.ang);
        if (const char *s = H19_Str(k, "script_string")) p.ss = s;
        s_structs[tn].push_back(p);
    }
}
const std::vector<Pt> &S(const std::string &tn) { static const std::vector<Pt> none; auto it = s_structs.find(tn); return it == s_structs.end() ? none : it->second; }

// sonidos que pide el cliente grafico (los reproduce el frame de sonido)
struct Snd { float org[3]; std::string alias; };
std::mutex s_sndMu; std::vector<Snd> s_sndQ;
void QueueSound(const float *org, const char *alias) { Snd s; memcpy(s.org, org, 12); s.alias = alias; std::lock_guard<std::mutex> g(s_sndMu); s_sndQ.push_back(s); }

std::atomic<bool> s_blizzard{ false };   // level.blizzard (clientflag 2), tambien lo lee el sonido
std::atomic<bool> s_lhl{ false };        // "LHL" visto (sonido del faro)

// ===================================================================================================== efectos y createfx
std::map<std::string, std::string> s_effect;   // level._effect[clave] -> ruta de LoadFx (zombie_coast_fx.csc)
void ParseEffects()
{
    const std::string t = Raw("clientscripts/zombie_coast_fx.csc"), l = Lower(t);
    size_t p = 0;
    while ((p = l.find("level._effect[", p)) != std::string::npos) {
        std::string key, path; size_t e = 0;
        if (!Quoted(t, p, key, &e)) break;
        size_t lf = l.find("loadfx", e), nl = t.find('\n', e);
        p = e;
        if (lf == std::string::npos || (nl != std::string::npos && lf > nl)) continue;
        if (Quoted(t, lf, path)) s_effect[key] = path;
    }
}
int s_fxT0 = -1, s_fxPlayed, s_fxMissing;   // s_fxT0 >= 0: FxSystem en marcha (FX_SpawnEffect antes del primer frame de h5_fx cae, M3)
const char *EffectPath(const char *key) { if (s_fxT0 < 0) return nullptr; auto it = s_effect.find(key); return it == s_effect.end() ? nullptr : it->second.c_str(); }
unsigned Fx(const char *key, const float *org, const float *ang = nullptr)   // playfx: sin direccion = (270, 0, 0)
{
    const char *p = EffectPath(key); if (!p) return 0;
    static const float def[3] = { 270, 0, 0 };
    return H19_PlayFxAng(p, org, ang ? ang : def);
}

struct CfxEnt { float org[3], ang[3], delay; std::string fxid; int exploder; bool done; std::vector<unsigned> fx; };
std::vector<CfxEnt> s_cfx;
std::map<int, int> s_exploderId;   // level._exploder_ids[numero] = id (orden de primera aparicion, desde 1)
std::once_flag s_cfxOnce;
void ParseCreateFx()
{
    const std::string t = Raw("clientscripts/createfx/zombie_coast_fx.csc");
    size_t p = 0; CfxEnt cur{}; bool open = false; cur.exploder = -1;
    auto flush = [&]() {
        if (open && !cur.fxid.empty()) {
            if (cur.exploder >= 0 && !s_exploderId.count(cur.exploder)) { const int id = (int)s_exploderId.size() + 1; s_exploderId[cur.exploder] = id; }
            s_cfx.push_back(cur);
        }
        cur = CfxEnt{}; cur.exploder = -1; open = false;
    };
    while (p < t.size()) {
        size_t q = t.find('\n', p); if (q == std::string::npos) q = t.size();
        const std::string ln = t.substr(p, q - p); p = q + 1;
        if (ln.find("createOneshotEffect") != std::string::npos || ln.find("createExploder") != std::string::npos) { flush(); open = true; cur.delay = -15; continue; }
        size_t k = ln.find("ent.v[");
        if (!open || k == std::string::npos) continue;
        std::string key, val; size_t e = 0; if (!Quoted(ln, k, key, &e)) continue;
        size_t eq = ln.find('=', e); if (eq == std::string::npos) continue;
        const char *r = ln.c_str() + eq + 1;
        if (key == "origin" || key == "angles") { float *d = key == "origin" ? cur.org : cur.ang; sscanf(r, " ( %f , %f , %f )", &d[0], &d[1], &d[2]); }
        else if (key == "delay") cur.delay = (float)atof(r);
        else if (key == "fxid") { if (Quoted(ln, eq, val)) cur.fxid = val; }
        else if (key == "exploder") cur.exploder = atoi(r);
        else if (key == "type") { if (Quoted(ln, eq, val) && val != "oneshotfx" && val != "exploder") open = false; }
    }
    flush();
}
void EnsureCreateFx() { std::call_once(s_cfxOnce, []() { ParseEffects(); ParseCreateFx(); }); }
void CreateFxFrame()
{
#ifdef H5FX
    if (!fx_systemPool || s_cfx.empty()) return;
    const int now = FX_GetSystem(0)->system.msecNow;
    if (s_fxT0 < 0) { if (now <= 0) return; s_fxT0 = now; fprintf(stderr, "[H19] Coast: createfx: reloj de FX en marcha, t0=%d\n", now); }
    for (CfxEnt &o : s_cfx) {
        if (o.done || o.exploder >= 0) continue;
        if (o.delay > 0 && now < s_fxT0 + (int)(o.delay * 1000.0f)) continue;   // oneshotfxthread: realWait(delay)
        o.done = true;
        auto ip = s_effect.find(o.fxid); const char *path = ip == s_effect.end() ? nullptr : ip->second.c_str();
        const FxEffectDef *fx = path ? FX_Register((char *)path) : nullptr;
        if (!fx) { ++s_fxMissing; continue; }
        float f[3], r[3], u[3], ax[3][3]; AngleVectors(o.ang, f, r, u);   // set_forward_and_up_vectors
        memcpy(ax[0], f, 12); memcpy(ax[2], u, 12);
        ax[1][0] = u[1] * f[2] - u[2] * f[1]; ax[1][1] = u[2] * f[0] - u[0] * f[2]; ax[1][2] = u[0] * f[1] - u[1] * f[0];
        FX_PlayOrientedEffect(0, fx, s_fxT0 + (int)(o.delay * 1000.0f), o.org, ax);   // create_triggerfx: inicio = delay*1000 desde el arranque
        ++s_fxPlayed;
    }
    static bool logged;
    if (!logged) { bool all = true; for (const CfxEnt &o : s_cfx) all = all && (o.done || o.exploder >= 0);
        if (all) { logged = true; fprintf(stderr, "[H19] Coast: createfx: %d oneshotfx lanzados (%d sin FX en los datos)\n", s_fxPlayed, s_fxMissing); } }
#endif
}

// ---- tareas con espera (wait/realwait de los hilos del CSC)
struct Task { int at; std::function<void(int)> fn; };
std::vector<Task> s_tasks;
void After(int at, std::function<void(int)> fn) { s_tasks.push_back({ at, std::move(fn) }); }
void RunTasks(int now)
{
    for (int pass = 0; pass < 4; ++pass) {
        std::vector<Task> due;
        for (size_t i = 0; i < s_tasks.size();) { if (now >= s_tasks[i].at) { due.push_back(std::move(s_tasks[i])); s_tasks.erase(s_tasks.begin() + i); } else ++i; }
        if (due.empty()) return;
        for (Task &t : due) t.fn(now);
    }
}
float RandRange(float a, float b) { return a + (b - a) * H19_Rand01(); }
int RandInt(int a, int b) { int v = a + (int)(H19_Rand01() * (float)(b - a)); return v >= b ? b - 1 : v; }   // randomintrange [a, b)

void Exploder(int id, int now)   // callback_activate_exploder / callback_deactivate_exploder (_fx.csc)
{
    int num = -1; for (auto &e : s_exploderId) if (e.second == (id < 0 ? -id : id)) num = e.first;
    if (num < 0) { fprintf(stderr, "[H19] Coast: exploder id %d desconocido\n", id); return; }
    int n = 0;
    for (CfxEnt &o : s_cfx) {
        if (o.exploder != num) continue;
        for (unsigned &h : o.fx) H19_KillFx(h);   // cannon_effect / deactivate_exploder: borra los anteriores
        o.fx.clear();
        if (id < 0) continue;
        CfxEnt *po = &o;
        After(now + (o.delay > 0 ? (int)(o.delay * 1000.0f) : 0), [po](int) {   // exploder_delay
            float f[3], r[3], u[3]; AngleVectors(po->ang, f, r, u);
            if (const char *p = EffectPath(po->fxid.c_str())) po->fx.push_back(H19_PlayFx(p, po->org, f, u));
        });
        ++n;
    }
    fprintf(stderr, "[H19] Coast: exploder %d (id %d) %s: %d efectos t=%d\n", num, id, id > 0 ? "activado" : "desactivado", n, now);
}

// ===================================================================================================== vision (pila de _zombiemode.csc)
std::map<std::string, int> s_vis;   // vision_set -> prioridad (self._zombie_visionset_list)
std::string s_visCur;
void VisApply(const char *name, int prio, float sec, bool add)
{
    if (add) s_vis[name] = prio; else s_vis.erase(name);
    std::string best; int bp = 0;
    for (auto &v : s_vis) if (v.second > bp) { bp = v.second; best = v.first; }   // zombie_highest_vision_set_apply (estricto: gana el primero)
    if (best.empty()) return;   // "undefined": el harness se queda con la ultima
    fprintf(stderr, "[H19] Coast: vision %s '%s' (%d) -> '%s' en %.1f s\n", add ? "pon" : "quita", name, prio, best.c_str(), sec);
    s_visCur = best;
    if (H7_VisionSetNaked && H19_EnvOn("H7E_VISION")) H7_VisionSetNaked(best.c_str(), sec);
}

// ===================================================================================================== sol (SetSunLight / ResetSunlight)
float s_sun0[3]; int s_sunHave = -1;
void SetSun(float v)
{
    GfxWorld *w = H4_FindWorld(); if (!w || !w->sunLight || !H19_EnvOn("H19_COAST_SUN")) return;
    GfxLight *sun = (GfxLight *)w->sunLight.get();
    if (s_sunHave < 0) { memcpy(s_sun0, sun->diffuseColor, 12); s_sunHave = 1; }
    if (v < 0) { memcpy(sun->diffuseColor, s_sun0, 12); return; }   // ResetSunlight
    sun->diffuseColor[0] = sun->diffuseColor[1] = sun->diffuseColor[2] = v;
}
void SunSequence(int now)   // coast_sun_light_flashes: SetSunLight(v); wait(t) ... ResetSunlight()
{
    static const float seq[][2] = { { .6f, .05f }, { .8f, .1f }, { .4f, .05f }, { .7f, .1f }, { 2, .15f }, { 1.5f, .2f }, { .7f, .1f }, { 2, .2f }, { 1, .1f },
        { 2, .05f }, { .7f, .1f }, { 2.2f, .15f }, { 2, .05f }, { 1, .05f }, { 1.2f, .1f }, { 1.5f, .15f }, { 1, .1f }, { .8f, .05f }, { .4f, .1f },
        { 1.2f, .1f }, { 2, .7f }, { .4f, .1f }, { .5f, .3f }, { .3f, .1f } };
    int t = now;
    for (auto &s : seq) { const float v = s[0]; After(t, [v](int) { SetSun(v); }); t += (int)(s[1] * 1000.0f); }
    After(t, [](int) { SetSun(-1); });
}
void SunFreakout(int now)   // freakout_sun_reaction: 1,5 s de SetSunLight aleatorio cada 0,05 s
{
    static const float a[6] = { 1.5f, 0.8f, 0.2f, 2.2f, 1.1f, 0.5f };
    for (int t = 0; t < 1500; t += 50) After(now + t, [](int) { SetSun(a[RandInt(0, 6)]); });
    After(now + 1500, [](int) { SetSun(-1); });
}

// ===================================================================================================== niebla (coast_fog_blizzard)
// setVolFog(start, halfwayDist, halfwayHeight, baseHeight, r, g, b, colorScale, sunR, sunG, sunB, dirX, dirY, dirZ, startAng, endAng, time, maxOpacity)
const float kFogClear[18] = { 500, 1700, 697.379f, 1000.6f, 0.235294f, 0.290196f, 0.337255f, 10, 0.760784f, 0.796079f, 0.807843f, -0.862899f, 0.264579f, 0.430586f, 0, 0, 10, 1 };
const float kFogZone[5][18] = {
    { 120, 250, 700.242f, 976.119f, 0.137255f, 0.192157f, 0.239216f, 7, 0.760784f, 0.796079f, 0.807843f, -0.862899f, 0.264579f, 0.430586f, 0, 0, 10, 1 },          // exterior
    { 251.982f, 700, 697.379f, 1000.6f, 0.235294f, 0.290196f, 0.337255f, 10, 0.760784f, 0.796079f, 0.807843f, -0.862899f, 0.264579f, 0.430586f, 0, 0, 10, 0.97f },  // faro
    { 80, 1700, 697.379f, 1000.6f, 0.235294f, 0.290196f, 0.337255f, 10, 0.760784f, 0.796079f, 0.807843f, -0.862899f, 0.264579f, 0.430586f, 0, 0, 10, 0.97f },     // proa
    { 251.982f, 700, 697.379f, 1000.6f, 0.235294f, 0.290196f, 0.337255f, 10, 0.760784f, 0.796079f, 0.807843f, -0.862899f, 0.264579f, 0.430586f, 0, 0, 10, 0.97f },  // popa
    { 220, 2300, 697.379f, 1000.6f, 0.235294f, 0.290196f, 0.337255f, 10, 0.760784f, 0.796079f, 0.807843f, -0.862899f, 0.264579f, 0.430586f, 0, 0, 10, 0.97f },    // hielo
};
const char *const kZoneName[5] = { "exterior", "faro", "proa", "popa", "hielo" };
struct FogTrig { unsigned model; float org[3]; int inside; bool touching; };
std::vector<FogTrig> s_fogTrigs;
int s_inside = -1;   // who.inside (sin definir hasta el primer disparador)
void ZoneFog(int z) { if (H7C_ClientVolFog) H7C_ClientVolFog(kFogZone[z], 10000); fprintf(stderr, "[H19] Coast: niebla de ventisca (%s)\n", kZoneName[z]); }
void LoadFogTrigs()   // coast_fog_triggers_init
{
    static const std::pair<const char *, int> kT[] = { { "lighthouseFog_interior", 1 }, { "shipFrontDeck_interior", 2 }, { "shipMedium_interior", 3 },
        { "shipIce_interior", 4 }, { "exteriorfog_trigger", 0 } };
    for (const H19Kv &k : H19_MapEnts()) {
        const char *tn = H19_Str(k, "targetname"), *m = H19_Str(k, "model"); if (!tn || !m || m[0] != '*') continue;
        for (auto &t : kT) if (!strcmp(tn, t.first)) {
            FogTrig f; f.model = (unsigned)atoi(m + 1); f.inside = t.second; f.touching = false; memset(f.org, 0, 12); H19_Vec(k, "origin", f.org);
            if (f.model && f.model < cm.numSubModels) s_fogTrigs.push_back(f);
        }
    }
}
bool Touching(unsigned model, const float *torg, const float *org, const float *mins, const float *maxs)
{
    trace_t tr; memset(&tr, 0, sizeof tr); const float zero[3] = { 0, 0, 0 };
    CM_TransformedBoxTrace(&tr, org, org, mins, maxs, model, -1, torg, zero);
    return tr.startsolid || tr.allsolid;
}
bool s_flag2;   // ultimo valor del clientflag 2 visto
void FogFrame(int)
{
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return;
    for (FogTrig &t : s_fogTrigs) {   // fog_trigger: waittill("trigger") -> inside; trigger_thread -> change_func al entrar
        const bool in = Touching(t.model, t.org, pl->r.currentOrigin, pl->r.mins, pl->r.maxs);
        if (in) s_inside = t.inside;
        if (in && !t.touching && s_blizzard) ZoneFog(t.inside);
        t.touching = in;
    }
    const bool f = (pl->client->ps.eFlags2 >> 2) & 1;   // level._COAST_FOG_BLIZZARD = 2 (SetClientFlag del jugador: ps.eFlags2)
    if (f == s_flag2) return;
    s_flag2 = f; s_blizzard = f;
    if (f) ZoneFog(s_inside >= 1 && s_inside <= 4 ? s_inside : 0);   // .inside 1-4 su zona; si no, exterior
    else { if (H7C_ClientVolFog) H7C_ClientVolFog(kFogClear, 10000); fprintf(stderr, "[H19] Coast: niebla clara (fin de ventisca)\n"); }
}

// ===================================================================================================== faro (zombie_coast_lighthouse.csc)
struct Rot { float from[3], to[3]; int t0, dur; };
float s_lhAng[3]; Rot s_lhRot; bool s_lhRotOn;
unsigned s_lhFx; float s_lhOrg[3]; bool s_lhOn, s_papActive, s_freak;
int s_lhNextStep, s_freakNext, s_papBackDone = -1;
void RotateTo(const float *ang, float sec, int now)   // rotateto: interpolacion lineal de cada angulo por el camino corto
{
    memcpy(s_lhRot.from, s_lhAng, 12); memcpy(s_lhRot.to, ang, 12); s_lhRot.t0 = now; s_lhRot.dur = (int)(sec * 1000.0f); s_lhRotOn = true;
}
void RotFrame(int now)
{
    if (!s_lhRotOn) return;
    const float t = s_lhRot.dur <= 0 ? 1.0f : fminf(1.0f, (float)(now - s_lhRot.t0) / (float)s_lhRot.dur);
    for (int k = 0; k < 3; ++k) s_lhAng[k] = s_lhRot.from[k] + AngDelta(s_lhRot.to[k], s_lhRot.from[k]) * t;
    if (t >= 1.0f) s_lhRotOn = false;
}
void LhFxOrient()   // el efecto sigue al tag_origin del modelo: frameNow = orientacion del modelo (FX_UpdateEffectBolt)
{
#ifdef H5FX
    if (!s_lhFx || !fx_systemPool) return;
    FxSystem *sys = &FX_GetSystem(0)->system;
    FxEffectContainer *c = UniqueHandleToEffect(sys, s_lhFx); if (!c) return;
    float q[4]; AnglesToQuat(s_lhAng, q);
    memcpy(c->effect.frameNow.quat, q, 16); memcpy(c->effect.frameNow.origin, s_lhOrg, 12);
#endif
}
void LighthouseOn(int now)   // lighthouse_light: spawn(tag_origin) en pack_light + playfxontag(fx_zmb_coast_spotlight_main)
{
    if (s_lhOn || S("pack_light").empty()) return;
    s_lhOn = true; memcpy(s_lhOrg, S("pack_light")[0].org, 12); memset(s_lhAng, 0, 12);
    const char *p = EffectPath("fx_zmb_coast_spotlight_main");
    float f[3] = { 1, 0, 0 }, u[3] = { 0, 0, 1 };
    s_lhFx = p ? H19_PlayFx(p, s_lhOrg, f, u) : 0;
    s_lhNextStep = now; s_lhl = true;
    fprintf(stderr, "[H19] Coast: faro encendido (LHL) en %.0f %.0f %.0f, FX %s t=%d\n", s_lhOrg[0], s_lhOrg[1], s_lhOrg[2], s_lhFx ? "ok" : "NO", now);
}
void PapFocus(const std::string &pp, int now)   // lighthouse_search_logic: apunta a la pap_location de script_string pp en 1 s
{
    if (!s_lhOn) return;
    for (const Pt &p : S("pap_location")) if (p.ss == pp) {
        float d[3] = { p.org[0] - s_lhOrg[0], p.org[1] - s_lhOrg[1], p.org[2] - s_lhOrg[2] }, a[3]; vectoangles(d, a);
        s_papActive = true;
        After(now + 200, [a](int t) { RotateTo(a, 1.0f, t); });   // wait(.2); rotateto(angles, 1)
        fprintf(stderr, "[H19] Coast: faro apunta al Pack-a-Punch %s (%.0f %.0f %.0f) t=%d\n", pp.c_str(), p.org[0], p.org[1], p.org[2], now);
    }
}
void LighthouseFrame(int now)
{
    if (!s_lhOn) return;
    if (s_freak) {   // lighthouse_freak_out: rotateto aleatorio de 0,5-1 s mientras dure
        if (now >= s_freakNext) { const float tm = RandRange(0.5f, 1.0f);
            const float a[3] = { (float)RandInt(-90, 90), (float)RandInt(-180, 180), (float)RandInt(-90, 90) }; RotateTo(a, tm, now); s_freakNext = now + (int)(tm * 1000.0f); }
    } else if (s_papActive) {
        s_papBackDone = -1;
    } else if (s_papBackDone < 0 && (s_lhAng[0] != 0 || s_lhAng[2] != 0)) {   // wait_for_packapunch_to_finish: vuelve a (0, yaw, 0) en 1 s
        const float a[3] = { 0, s_lhAng[1], 0 }; RotateTo(a, 1.0f, now); s_papBackDone = now + 1000;
    } else if (s_papBackDone >= 0 && now < s_papBackDone) {
    } else if (now >= s_lhNextStep) {   // rotateto(yaw + 5, .2); wait(.1)
        s_papBackDone = -1;
        const float a[3] = { s_lhAng[0], s_lhAng[1] + 5.0f, s_lhAng[2] }; RotateTo(a, 0.2f, now); s_lhNextStep = now + 100;
    }
    RotFrame(now);
    LhFxOrient();
}

// ===================================================================================================== Morse del faro (solaris_flash)
std::vector<std::vector<int>> s_morse;   // flash_setup: listas de 0/1; vacia = palabra (wait 1.4)
bool s_morseOn; size_t s_mi, s_mj; int s_mAt; unsigned s_mFx; int s_mPhase;
void MorseSetup()
{
    static const char *const m[] = { "0100", "0", "0010", "1", "", "000", "0100", "111", "011", "", "110011", "", "010", "00", "110", "0000", "1", "",
        "0010", "001", "0100", "0100", "", "110011", "", "00111", "", "010", "00", "110", "0000", "1", "", "010101" };
    for (const char *s : m) { std::vector<int> v; for (const char *c = s; *c; ++c) v.push_back(*c - '0'); s_morse.push_back(v); }
}
void MorseFrame(int now)
{
    if (!s_morseOn || now < s_mAt || S("struct_musical_chairs_lighthouse").empty()) return;
    const float *o = S("struct_musical_chairs_lighthouse")[0].org;
    if (s_mi >= s_morse.size()) s_mi = 0;   // while (level._light_message == 0)
    const std::vector<int> &w = s_morse[s_mi];
    if (w.empty()) { s_mAt = now + 1400; ++s_mi; s_mj = 0; return; }
    if (s_mPhase == 0) {   // solaris_ball + solaris_howl
        if (const char *p = EffectPath("lighthouse_morse_code")) { const float f[3] = { 1, 0, 0 }, u[3] = { 0, 0, 1 }; s_mFx = H19_PlayFx(p, o, f, u); }
        QueueSound(o, w[s_mj] ? "zmb_beepbadeepbadoop" : "zmb_beepbadoopbadeep");
        s_mAt = now + (w[s_mj] ? 600 : 200); s_mPhase = 1; return;
    }
    H19_KillFx(s_mFx);   // solaris_destroy (el modelo se borra; el FX muere con el)
    s_mPhase = 0;
    if (++s_mj >= w.size()) { s_mj = 0; ++s_mi; s_mAt = now + 200 + 600; } else s_mAt = now + 200;
}

// ===================================================================================================== copos y bengalas
bool s_rainOn; int s_rainToggle = -1, s_rainNext;
void RainFrame(int now)   // manage_player_rain + start_player_rain (RandomFloat(10, 30) = RandomFloat(10): 0-10 s, como el original)
{
    if (s_rainToggle < 0) { s_rainOn = true; s_rainToggle = now + (int)(RandRange(0, 10) * 1000.0f); }
    if (now >= s_rainToggle) { s_rainOn = !s_rainOn; s_rainToggle = now + (int)(RandRange(0, 10) * 1000.0f); }
    if (!s_rainOn || now < s_rainNext) return;
    s_rainNext = now + 300;
    const float f[3] = { 1, 0, 0 }, u[3] = { 0, 0, 1 };
    if (const char *p = EffectPath("player_rain")) H19_PlayFx(p, g_entities[0].r.currentOrigin, f, u);   // PlayFxOnTag(players[0], "tag_origin")
}
int s_flares;
void Flares()   // flare_effects: PlayFXOnTag(flare | flare_no_dlight, coast_flare, "tag_fx")
{
    for (const H19Kv &k : H19_MapEnts()) {
        const char *tn = H19_Str(k, "targetname"), *mn = H19_Str(k, "model"); if (!tn || strcmp(tn, "coast_flare") || !mn) continue;
        float org[3] = { 0, 0, 0 }, ang[3] = { 0, 0, 0 }, ax[3][3], tag[3]; H19_Vec(k, "origin", org); H19_Vec(k, "angles", ang); AnglesToAxis(ang, ax);
        memcpy(tag, org, 12);
        const XModel *xm = DB_FindXAssetHeader(ASSET_TYPE_XMODEL, (char *)mn, 1, -1).model;
        unsigned char bi = 0xFF; const unsigned nm = SL_FindString("tag_fx", SCRIPTINSTANCE_SERVER);
        if (xm && nm && XModelGetBoneIndex(xm, nm, 0, &bi) && bi != 0xFF) if (const DObjAnimMat *bp = XModelGetBasePose(xm))
            for (int c = 0; c < 3; ++c) tag[c] = org[c] + bp[bi].trans[0] * ax[0][c] + bp[bi].trans[1] * ax[1][c] + bp[bi].trans[2] * ax[2][c];
        const char *si = H19_Str(k, "script_int");
        const char *p = EffectPath(si && atoi(si) == 1 ? "flare_no_dlight" : "flare");
        if (p && H19_PlayFx(p, tag, ax[0], ax[2])) ++s_flares;
    }
    fprintf(stderr, "[H19] Coast: %d bengalas (coast_flare)\n", s_flares);
}

// ===================================================================================================== avisos del servidor
std::mutex s_evMu; std::vector<std::pair<std::string, std::string>> s_evQ;   // (sistema, estado); sistema "" = levelNotify
bool s_power;
void Notify(const std::string &ev, int now)
{
    if (ev == "ZPO" && !s_power) {   // coast_ZPO_listener -> coast_power_on: power_on_burst + coast_sun_light_flashes
        s_power = true;
        VisApply("zombie_coast_powerOn", 5, 0.1f, true);
        After(now + 100, [](int) { VisApply("zombie_coast_powerOn", 5, 6.0f, false); });
        SunSequence(now);
        fprintf(stderr, "[H19] Coast: corriente (ZPO): destellos del sol y vision powerOn t=%d\n", now);
    }
    else if (ev == "LHL") LighthouseOn(now);
    else if (ev == "pp0" || ev == "pp1" || ev == "pp2") PapFocus(ev, now);
    else if (ev == "PPH") { s_papActive = false; fprintf(stderr, "[H19] Coast: Pack-a-Punch escondido (PPH): el faro vuelve a girar t=%d\n", now); }
    else if (ev == "lhfo" && s_lhOn) {   // lighthouse_freak_out
        SunFreakout(now);
        VisApply("zombie_coast_rovingEye", 7, 0.1f, true);
        After(now + 1500, [](int t) { VisApply("zombie_coast_rovingEye", 7, 1.5f, false); s_freak = true; s_papActive = true; s_freakNext = t; });
        fprintf(stderr, "[H19] Coast: el faro se vuelve loco (lhfo) t=%d\n", now);
    }
    else if (ev == "lhfd") { s_freak = false; fprintf(stderr, "[H19] Coast: fin del faro loco (lhfd) t=%d\n", now); }
    else if (ev == "FI") VisApply("zombie_coast_lighthouse", 2, 5.0f, true);    // fog_visionset_handler
    else if (ev == "FO") VisApply("zombie_coast_lighthouse", 2, 5.0f, false);
    else if (ev == "lmc" && !s_morseOn) { s_morseOn = true; s_mAt = now; fprintf(stderr, "[H19] Coast: codigo Morse del faro (lmc)\n"); }
    else if (ev == "slc") { s_morseOn = false; H19_KillFx(s_mFx); fprintf(stderr, "[H19] Coast: fin del Morse (slc)\n"); }
}

void Init(const std::vector<H19Kv> &)
{
    LoadStructs(); EnsureCreateFx(); LoadFogTrigs(); MorseSetup();
    size_t nexp = 0; for (const CfxEnt &o : s_cfx) nexp += o.exploder >= 0;
    fprintf(stderr, "[H19] Coast: %zu level._effect, createfx %zu (%zu exploders en %zu numeros), %zu disparadores de niebla, pap_location %zu, niebla de cliente %s\n",
        s_effect.size(), s_cfx.size(), nexp, s_exploderId.size(), s_fogTrigs.size(), S("pap_location").size(), H7C_ClientVolFog ? "si" : "NO");
}
bool LevelNotify(const std::string &ev, int)
{
    std::lock_guard<std::mutex> g(s_evMu); s_evQ.push_back({ "", ev });
    return false;   // sigue a los mezcladores genericos
}
bool SysState(const char *sys, const std::string &st, int)
{
    if (strcmp(sys, "@exploder")) return false;
    std::lock_guard<std::mutex> g(s_evMu); s_evQ.push_back({ sys, st });
    return true;
}
void Frame(int now)
{
    static bool first = true;
    if (first) { first = false; VisApply("zombie_coast_2", 1, 0.1f, true); }   // coast_player_connect -> coast_set_visionset
    if (const char *e = getenv("H19_COAST_NOTIFY")) {   // prueba: "ms:ev,..." como levelNotify del servidor
        static std::vector<std::pair<int, std::string>> q; static bool parsed;
        if (!parsed) { parsed = true; std::string a = e; size_t p = 0;
            while (p < a.size()) { size_t c = a.find(',', p); if (c == std::string::npos) c = a.size(); std::string it = a.substr(p, c - p); size_t k = it.find(':');
                if (k != std::string::npos) q.push_back({ atoi(it.c_str()), it.substr(k + 1) }); p = c + 1; } }
        for (auto &x : q) if (x.first >= 0 && now >= x.first) { x.first = -1; fprintf(stderr, "[H19] H19_COAST_NOTIFY: '%s' t=%d\n", x.second.c_str(), now); std::lock_guard<std::mutex> g(s_evMu); s_evQ.push_back({ "", x.second }); }
    }
    CreateFxFrame();
    static bool flares; if (!flares && s_fxT0 >= 0) { flares = true; Flares(); }   // flare_effects (con el FxSystem en marcha)
    std::vector<std::pair<std::string, std::string>> q; { std::lock_guard<std::mutex> g(s_evMu); q.swap(s_evQ); }
    std::string last;
    for (auto &e : q) {
        if (e.first + "=" + e.second == last) continue;   // el gancho de h7e puede ir encadenado dos veces
        last = e.first + "=" + e.second;
        if (e.first.empty()) Notify(e.second, now);
        else if (e.first == "@exploder") Exploder(atoi(e.second.c_str()), now);
    }
    RunTasks(now);
    FogFrame(now);
    LighthouseFrame(now);
    MorseFrame(now);
    RainFrame(now);
}
const char *LevelEffect(const char *key) { auto it = s_effect.find(key); return it == s_effect.end() ? nullptr : it->second.c_str(); }

// ===================================================================================================== sonido (zombie_coast_amb.csc)
#ifdef H6SND
}   // namespace
int __cdecl CG_PlaySoundWithHandle(int localClientNum, int entitynum, float *origin, int fadeMs, bool doNotify, float volume, unsigned int id);   // h5_audio_cl.cpp
namespace {
unsigned s_rng = 0x636f6173u;   // secuencia propia del hilo de sonido
float RandF(float a, float b) { s_rng = s_rng * 1664525u + 1013904223u; return a + (b - a) * ((s_rng >> 8) / 16777216.0f); }
int Play(const float *org, const char *alias)
{
    float p[3]; if (org) { p[0] = org[0]; p[1] = org[1]; p[2] = org[2]; }
    return CG_PlaySoundWithHandle(0, 1022, org ? p : nullptr, 0, 0, 1.0f, SND_FindAliasId(alias));
}
struct Trans { float org[3]; double at; };
std::vector<Trans> s_trans;   // indoor_outdoor_transitions (blizzard_transitions)
int s_transPlays, s_shots, s_fires; bool s_lhlSnd; double s_lhlAt = -1; int s_lhlStep;
void AudioStart()
{
    LoadStructs(); EnsureCreateFx();
    for (const Pt &p : S("blizzard_transitions")) { Trans t; memcpy(t.org, p.org, 12); t.at = level.time + RandF(0.5f, 2.0f) * 1000.0; s_trans.push_back(t); }
    for (const CfxEnt &o : s_cfx) if (o.fxid == "fx_fire_line_xsm") {   // snd_play_auto_fx("fx_fire_line_xsm", "amb_fire")
        const int f = H19A_FakeAt(o.org); if (f) { H19A_PlayLoop(f, "amb_fire", 0.5f); ++s_fires; } }
    fprintf(stderr, "[H19] Coast sonido: %zu transiciones de ventisca, %d fuegos con amb_fire\n", s_trans.size(), s_fires);
}
void AudioFrame(bool)
{
    const double now = level.time;
    { std::vector<Snd> q; { std::lock_guard<std::mutex> g(s_sndMu); q.swap(s_sndQ); }
      for (const Snd &s : q) { Play(s.org, s.alias.c_str()); ++s_shots; } }
    for (Trans &t : s_trans) if (now >= t.at) {   // amb_blizzard_rand_heavy / _light segun level.blizzard; wait(1..1.5)
        Play(t.org, s_blizzard ? "amb_blizzard_rand_heavy" : "amb_blizzard_rand_light"); ++s_transPlays; t.at = now + RandF(1.0f, 1.5f) * 1000.0; }
    if (s_lhl && !s_lhlSnd) {   // play_poweron_sounds + lighthouse_looper
        s_lhlSnd = true; s_lhlAt = now; s_lhlStep = 0;
        static const float a[3] = { -739, -876, 682 }, b[3] = { -687, -1124, 709 }, lh[3] = { -61, 1359, 1049 };
        Play(a, "evt_flip_sparks_left"); Play(b, "evt_flip_sparks_right");
        Play(lh, "zmb_lighthouse_start"); const int f = H19A_FakeAt(lh); if (f) H19A_PlayLoop(f, "zmb_lighthouse_loop", 1.0f);
    }
    if (s_lhlSnd && s_lhlStep == 0 && now >= s_lhlAt + 750) {   // wait(.75)
        static const float o[3] = { -52, 1353, 2503 }, z[3] = { 0, 0, 0 };
        Play(o, "zmb_lighthouse_light_flash"); Play(z, "evt_poweron_front"); s_lhlStep = 1;
    }
    if (s_lhlSnd && s_lhlStep == 1 && now >= s_lhlAt + 5750) { static const float o[3] = { -694, -990, 1025 }; Play(o, "zmb_ship_horn_poweron"); s_lhlStep = 2; }   // wait(5)
}
void AudioStatus(std::string &out)
{
    char b[200]; snprintf(b, sizeof b, "; Coast: transiciones %d (%s), fuegos %d, sonidos %d, faro %s", s_transPlays, s_blizzard ? "ventisca" : "calma", s_fires, s_shots,
        s_lhlSnd ? "si" : "no");
    out += b;
}
#endif
} // namespace

extern const H19MapCl kH19_zombie_coast = {
    .zone = "zombie_coast", .offEnv = "H19_COAST",
    .estado = "Call of the Dead: pila de visiones, niebla de ventisca por zonas, createfx y exploders, faro giratorio con Pack-a-Punch, sol, Morse, copos, bengalas y sonido (OLA M8)",
    .init = Init, .levelNotify = LevelNotify, .sysState = SysState, .frame = Frame, .clockHand = nullptr, .screensFrame = nullptr,
    .levelEffect = LevelEffect,
#ifdef H6SND
    .audioStart = AudioStart, .audioNotify = nullptr, .audioSys = nullptr, .audioFrame = AudioFrame, .audioStatus = AudioStatus,
#endif
    .doorEnt = nullptr,
};
#endif
