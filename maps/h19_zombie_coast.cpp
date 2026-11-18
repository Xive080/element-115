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
// OLA E2b (completo):
//  - escarcha del agua helada (clientflags de jugador 5/6/7: init_frost / frost_invade / frost_retreat): filtro generic_filter_frost
//    (_filter.csc, pase 0 del filtro 0, destino escena, muestreador escena) con la opacidad que sube y baja como el .csc (H7_FilterPass).
//  - tirolina (clientflag 0 del jugador): FOV 65 -> 55 en 0,75 s, temblor cada 0,1-0,15 s y vuelta al FOV del usuario 2 s despues.
//  - electrocutado: jugador (flag 8: rumble), zombis (flag de actor 2: buff_electrified en J_SpineLower).
//  - director: ciclo de la luz de tag_light (flag 3), pasos fly_step_director_ (flag 4, via H19_AIFootPrepend), FX de muerte (flag 5),
//    "ZDA" (Earthquake 0,4 / 1,7 s), snd_futz con "dclm"/"dmad". Los FX atornillados a hueso renacen si el actor se teletransporta.
//  - fxanims (grua y botes): DObj de cliente con el arbol fxanim_props_dlc3, animado en bucle y dibujado con sus huesos (H19_ModelBones).
// No aplica en solo: jugador falso de la tirolina y del lanzador (flags 1/3/4 solo para los OTROS jugadores). Sin ragdoll de cliente:
// launch_zombie solo se cuenta.
//   H19_COAST=0 lo apaga. Pruebas: H19_COAST_NOTIFY="ms:ev,..." inyecta levelNotify de cliente en ese level.time (como el servidor).
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <universal/dvar.h>
#include <database/db_registry.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <server_mp/server_mp.h>
#include <EffectsCore/fx_system.h>
#include <gfx_d3d/r_bsp.h>
#include <xanim/xmodel.h>
#include <xanim/xmodel_utils.h>
#include <xanim/xanim.h>
#include <xanim/dobj.h>
#include <xanim/dobj_utils.h>
#include <xanim/dobj_skel.h>
#include <EffectsCore/fx_marks.h>
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
void H7_FilterPass(int f, int p, const char *material, int target, int s0, int s1, bool on) __attribute__((weak));   // h7_gfx.cpp (OLA E2b)
void H7_FilterConst(int f, int p, int c, float v) __attribute__((weak));
void H5FX_Earthquake(float scale, float seconds, const float *org, float radius) __attribute__((weak));   // h5_fx.cpp: Earthquake() de cliente
void H4_GetView(float *org, float (*axis)[3], float *tanHalfX, float *tanHalfY);   // world_h4.cpp
float H7C_FovX(int W, int H) __attribute__((weak));                                // h7c_gfx.cpp (CG_CalcFov)
extern float g_h10FovX;                                                            // world_h4.cpp: FOV horizontal en caliente
int H19_ModelCount(); const char *H19_ModelTn(int j); bool H19_ModelGet(int j, float *org, float *ang);   // h7e_light.cpp (script_model de cliente)
void H19_ModelBones(int j, const float *mats, int nb) __attribute__((weak));                             // h7e_light.cpp (OLA E2b)

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
    for (const char *file : { "clientscripts/zombie_coast_fx.csc", "clientscripts/_zombiemode_ai_director.csc" }) {   // OLA E2b: + init() del director
    const std::string t = Raw(file), l = Lower(t);
    size_t p = 0;
    while ((p = l.find("level._effect[", p)) != std::string::npos) {
        std::string key, path; size_t e = 0;
        if (!Quoted(t, p, key, &e)) break;
        size_t lf = l.find("loadfx", e), nl = t.find('\n', e);
        p = e;
        if (lf == std::string::npos || (nl != std::string::npos && lf > nl)) continue;
        if (Quoted(t, lf, path) && !s_effect.count(key)) s_effect[key] = path;
    }
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

// ===================================================================================================== OLA E2b: escarcha del agua helada
// zombie_coast.csc: clientflags del JUGADOR (ps.eFlags2) 5 = _CF_PLAYER_WATER_FROST (coast_water_frost: con 1 coast_water_frost_invade,
// con 0 coast_water_frost_retreat), 6 = _CF_PLAYER_WATER_FREEZE (coast_water_freeze: al quitarse, al jugador local _frost_curr_time = 0;
// el FX waterfreeze y el freezegun_shatter son solo para los OTROS jugadores) y 7 = _CF_PLAYER_WATER_FROST_REMOVE (quita el filtro).
// El filtro es el de _filter.csc: init_filter_frost / enable_filter_frost = set_filter_pass_material(0, 0, generic_filter_frost,
// targerid_scene 3, targerid_scene 3, targetid_none 0) + set_filter_pass_enabled; set_filter_frost_opacity = set_filter_pass_constant(0, 0, 0, op).
// invade: opacidad = _frost_curr_time / 30000 (de 0 a 1) cada 0,1 s sumando el tiempo real; retreat: espera 0,5 s y resta hasta <= 0.
const float kFrostMax = 30000.0f;
float s_frostOp, s_frostCur; int s_frostMode, s_frostAt, s_frostLast; bool s_frostFirst;   // modo 0 nada, 1 invade, 2 retreat
unsigned s_plFlags; bool s_plFlagsInit;
int s_frostSets, s_freezeSets, s_frostRemoves, s_elecSets;
void FrostFilter(bool on, float op)
{
    if (!H7_FilterPass || !H7_FilterConst) return;
    if (on) { H7_FilterPass(0, 0, "generic_filter_frost", 3, 3, 0, true); H7_FilterConst(0, 0, 0, op); }
    else H7_FilterPass(0, 0, nullptr, 0, 0, 0, false);
}
void FrostInvade(int now)   // coast_water_frost_invade
{
    if (s_frostOp > 0) s_frostCur = s_frostOp * kFrostMax;
    FrostFilter(true, s_frostOp);
    s_frostMode = 1; s_frostAt = now; s_frostFirst = true;
}
void FrostFrame(int now)
{
    if (!s_frostMode || now < s_frostAt) return;
    const float diff = s_frostFirst ? 0.0f : (float)(now - s_frostLast);   // diff = nextTime - startTime (0 la primera vuelta)
    s_frostFirst = false; s_frostLast = now; s_frostAt = now + 100;   // wait(0.1)
    if (s_frostMode == 1) {
        s_frostOp = fminf(1.0f, fmaxf(0.0f, s_frostCur / kFrostMax)); s_frostCur += diff;
        if (H7_FilterConst) H7_FilterConst(0, 0, 0, s_frostOp);
        static int lastLog = -1; const int pct = (int)(s_frostOp * 10.0f);
        if (pct != lastLog) { lastLog = pct; fprintf(stderr, "[H19] Coast: escarcha subiendo, opacidad %.2f t=%d\n", s_frostOp, now); }
    } else {
        if (s_frostOp <= 0) { FrostFilter(false, 0); s_frostMode = 0; fprintf(stderr, "[H19] Coast: escarcha quitada t=%d\n", now); return; }
        s_frostOp = s_frostCur / kFrostMax; s_frostCur -= diff;
        if (H7_FilterConst) H7_FilterConst(0, 0, 0, s_frostOp);
        if (s_frostOp <= 0) { FrostFilter(false, 0); s_frostMode = 0; fprintf(stderr, "[H19] Coast: escarcha quitada t=%d\n", now); }
    }
}
// ---- jugador: zipline_rumble_and_quake (clientflag 0) y electrocutado (8)
float s_fovFrom, s_fovTo, s_fovUser = -1; int s_fovT0 = -1, s_fovDur; int s_zipQuakeAt = -1, s_zipEndAt = -1;
void SetFov(float fov)   // SetClientDvar("cg_fov", v) + el FOV horizontal del render (CG_CalcFov)
{
    if (const dvar_s *d = Dvar_FindVar("cg_fov")) { char b[16]; snprintf(b, sizeof b, "%g", fov); Dvar_SetFromStringByName("cg_fov", b); (void)d; }
    if (!H7C_FovX) return;
    float o[3], ax[3][3], tx, ty; H4_GetView(o, ax, &tx, &ty);
    if (tx > 0 && ty > 0) g_h10FovX = H7C_FovX((int)(1000.0f * tx / ty), 1000);
}
void LerpFov(float from, float to, float sec, int now) { s_fovFrom = from; s_fovTo = to; s_fovT0 = now; s_fovDur = (int)(sec * 1000.0f); }   // lerp_fov(.75, base, dest)
void FovFrame(int now)
{
    if (s_fovT0 < 0) return;
    const int incs = s_fovDur / 50; if (incs <= 0) { s_fovT0 = -1; return; }   // incs = int(time / .05), un paso cada 0,05 s
    const int k = (now - s_fovT0) / 50 + 1;
    if (k >= incs) { SetFov(s_fovTo); s_fovT0 = -1; return; }
    SetFov(s_fovFrom + (s_fovTo - s_fovFrom) / incs * k);
}
void PlayerFlagsFrame(int now)
{
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return;
    const unsigned f = (unsigned)pl->client->ps.eFlags2 & ((1u << 0) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8));
    if (!s_plFlagsInit) { s_plFlagsInit = true; s_plFlags = 0; FrostFilter(false, 0); }   // coast_reset_frost al conectar
    const unsigned ch = f ^ s_plFlags; s_plFlags = f;
    if (ch & (1u << 5)) {   // coast_water_frost
        if (f & (1u << 5)) { ++s_frostSets; FrostInvade(now); fprintf(stderr, "[H19] Coast: agua helada: escarcha (frost_invade) t=%d\n", now); }
        else { s_frostMode = 2; s_frostAt = now + 500; s_frostFirst = true; fprintf(stderr, "[H19] Coast: fuera del agua: la escarcha se retira (frost_retreat) t=%d\n", now); }
    }
    if (ch & (1u << 6)) {   // coast_water_freeze (jugador local)
        if (f & (1u << 6)) { ++s_freezeSets; fprintf(stderr, "[H19] Coast: jugador congelado (water_freeze) t=%d\n", now); }
        else { s_frostCur = 0; fprintf(stderr, "[H19] Coast: fin de la congelacion: _frost_curr_time = 0 t=%d\n", now); }
    }
    if ((ch & (1u << 7)) && (f & (1u << 7))) { ++s_frostRemoves; s_frostMode = 0; s_frostOp = 0; FrostFilter(false, 0); fprintf(stderr, "[H19] Coast: escarcha quitada a la fuerza (frost_remove) t=%d\n", now); }
    if ((ch & (1u << 8)) && (f & (1u << 8))) { ++s_elecSets; fprintf(stderr, "[H19] Coast: jugador electrocutado (PlayRumbleOnEntity explosion_generic) t=%d\n", now); }
    if (ch & 1u) {   // zipline_rumble_and_quake: do_zipline_fx / stop_zipline_fx
        if (f & 1u) {
            if (s_fovUser < 0) { const dvar_s *d = Dvar_FindVar("cg_fov"); s_fovUser = d ? d->current.value : 65.0f; }
            LerpFov(65.0f, 55.0f, 0.75f, now); s_zipQuakeAt = now; s_zipEndAt = -1;   // lerp_fov(.75, default_fov 65, zipline_fov 55)
            fprintf(stderr, "[H19] Coast: tirolina: FOV 65 -> 55 y temblor t=%d\n", now);
        } else { s_zipQuakeAt = -1; s_zipEndAt = now + 2000; fprintf(stderr, "[H19] Coast: fin de la tirolina t=%d\n", now); }   // wait(2) y lerp de vuelta
    }
    if (s_zipQuakeAt >= 0 && now >= s_zipQuakeAt) {   // Earthquake(RandomFloatRange(.15, .22), RandomFloatRange(.15, .22), origin, 100); wait(.1-.15)
        if (H5FX_Earthquake) H5FX_Earthquake(RandRange(0.15f, 0.22f), RandRange(0.15f, 0.22f), pl->r.currentOrigin, 100.0f);
        s_zipQuakeAt = now + (int)(RandRange(0.1f, 0.15f) * 1000.0f);
    }
    if (s_zipEndAt >= 0 && now >= s_zipEndAt) { s_zipEndAt = -1; LerpFov(55.0f, s_fovUser > 0 ? s_fovUser : 65.0f, 0.75f, now); }   // vuelta (65 en el original)
}

// ===================================================================================================== OLA E2b: clientflags de actor
// register_clientflag_callback("actor", ...) de zombie_coast.csc (bits de s.lerp.eFlags2):
//   2 coast_zombie_electrified: buff_electrified en J_SpineLower mientras este puesto (zombis buffados por el director);
//   3 zombie_director_light_update (_zombiemode_ai_director.csc): CADA cambio pasa health_state pristine -> health_full -> damage_one ->
//     damage_two -> (sin definir) y vuelve a lanzar en tag_light docile / light_big / light / light_blink (con "sin definir" repite el ultimo);
//   4 director_footsteps: self.footstepPrepend = "fly_step_director_" (lo usa _footsteps.csc playAIFootstep, h5_audio_cl);
//   5 zombie_director_death: director_death_head en j_neck y director_death_torso en J_SpineLower (al quitarse, se borran);
//   0 launch_zombie (zombie_coast_flinger.csc): LaunchRagdoll; el arnes no tiene ragdoll de cliente: solo se cuenta.
struct JointFx { std::string tag, key; unsigned h; const FxEffectDef *def; float last[3]; };
struct ActorSt { int use; unsigned fl; int lightState; std::vector<JointFx> elec, light, death; };
std::map<int, ActorSt> s_act;
std::atomic<int> s_dirSteps[1024];   // useCount + 1 de los actores con director_footsteps (lo lee el hilo de sonido)
int s_elecZombies, s_lightFlips, s_deathFx, s_ragdolls;
bool TagMatrix(gentity_s *e, const char *tag, float m[4][3])
{
    const unsigned t = SL_FindLowercaseString(tag, SCRIPTINSTANCE_SERVER);
    if (t && G_DObjGetWorldTagMatrix(e, t, m)) return true;
    AnglesToAxis(e->r.currentAngles, m); memcpy(m[3], e->r.currentOrigin, 12); return false;
}
void TagFx(gentity_s *e, std::vector<JointFx> &v, const char *tag, const char *key)   // PlayFXOnTag(fx, self, tag)
{
    JointFx f; f.tag = tag; f.key = key; f.h = 0; f.def = nullptr;
    float m[4][3]; const bool ok = TagMatrix(e, tag, m); memcpy(f.last, m[3], 12);
    if (const char *p = EffectPath(key)) { f.h = H19_PlayFx(p, m[3], m[0], m[2]);
#ifdef H5FX
        f.def = FX_Register((char *)p);
#endif
    }
    static int logs; if (logs++ < 12) fprintf(stderr, "[H19] Coast: FX %s en %s del ent %d (%s) %s\n", key, tag, e->s.number, ok ? "hueso" : "sin hueso: origen", f.h ? "ok" : "NO");
    v.push_back(f);
}
void KillTagFx(std::vector<JointFx> &v) { for (JointFx &f : v) H19_KillFx(f.h); v.clear(); }
void FollowTagFx(gentity_s *e, std::vector<JointFx> &v)   // el ent de FX atornillado al hueso: frameNow = matriz del hueso (FX_UpdateEffectBolt)
{
#ifdef H5FX
    if (!fx_systemPool) return;
    for (JointFx &f : v) {
        if (!f.h || !f.def) continue;
        FxEffectContainer *c = UniqueHandleToEffect(&FX_GetSystem(0)->system, f.h);
        if (!c || c->effect.def != f.def) { f.h = 0; continue; }
        float m[4][3]; TagMatrix(e, f.tag.c_str(), m); float ax[3][3]; memcpy(ax, m, sizeof ax);
        const float dx = m[3][0] - f.last[0], dy = m[3][1] - f.last[1], dz = m[3][2] - f.last[2], d2 = dx * dx + dy * dy + dz * dz;
        if (!(d2 < 1e30f)) continue;   // NaN o infinito: no se mueve
        if (d2 > 256.0f * 256.0f) {   // teletransporte (el director sale del agua por otro sitio): como un ent nuevo, el FX vuelve a nacer
            H19_KillFx(f.h); f.h = 0;   // un rastro (trail) que salta miles de unidades en un frame genera elementos sin fin
            if (const char *p = EffectPath(f.key.c_str())) f.h = H19_PlayFx(p, m[3], m[0], m[2]);
            memcpy(f.last, m[3], 12); continue;
        }
        memcpy(f.last, m[3], 12);
        memcpy(c->effect.frameNow.origin, m[3], 12); AxisToQuat(ax, c->effect.frameNow.quat);
    }
#else
    (void)e; (void)v;
#endif
}
void ActorsFrame(int now)
{
    std::map<int, bool> alive;
    for (int n = 0; n < level.num_entities && n < 1024; ++n) {
        gentity_s *e = &g_entities[n];
        if (!e->r.inuse || e->s.eType != ET_ACTOR) continue;
        const unsigned fl = (unsigned)e->s.lerp.eFlags2 & 0x3Du;   // bits 0, 2, 3, 4, 5
        auto it = s_act.find(n);
        if (it != s_act.end() && it->second.use != e->s.lerp.useCount) { KillTagFx(it->second.elec); KillTagFx(it->second.light); KillTagFx(it->second.death); s_act.erase(it); it = s_act.end(); }
        if (it == s_act.end()) { if (!fl) continue; ActorSt a; a.use = e->s.lerp.useCount; a.fl = 0; a.lightState = -1; it = s_act.emplace(n, a).first; }
        ActorSt &a = it->second; alive[n] = true;
        const unsigned ch = fl ^ a.fl; a.fl = fl;
        if (ch & (1u << 2)) {   // coast_zombie_electrified
            KillTagFx(a.elec);
            if (fl & (1u << 2)) { TagFx(e, a.elec, "j_spinelower", "buff_electrified"); ++s_elecZombies; }
        }
        if (ch & (1u << 3)) {   // zombie_director_light_update: un paso por cada cambio de la bandera
            static const char *const kFx[5] = { "director_weapon_docile", "director_weapon_light_big", "director_weapon_light", "director_weapon_light_blink", "director_weapon_light_blink" };
            static const char *const kSt[5] = { "pristine", "health_full", "health_damage_one", "health_damage_two", "(sin definir)" };
            a.lightState = (a.lightState + 1) % 5; ++s_lightFlips;
            KillTagFx(a.light); TagFx(e, a.light, "tag_light", kFx[a.lightState]);
            fprintf(stderr, "[H19] Coast: luz del director (ent %d): %s -> %s t=%d\n", n, kSt[a.lightState], kFx[a.lightState], now);
        }
        if ((ch & (1u << 4)) && (fl & (1u << 4))) { s_dirSteps[n] = e->s.lerp.useCount + 1; fprintf(stderr, "[H19] Coast: pasos del director (fly_step_director_) ent %d\n", n); }
        if (ch & (1u << 5)) {   // zombie_director_death
            KillTagFx(a.death);
            if (fl & (1u << 5)) { TagFx(e, a.death, "j_neck", "director_death_head"); TagFx(e, a.death, "j_spinelower", "director_death_torso"); ++s_deathFx;
                fprintf(stderr, "[H19] Coast: el director cae (director_death) ent %d t=%d\n", n, now); }
        }
        if ((ch & 1u) && (fl & 1u)) { ++s_ragdolls; fprintf(stderr, "[H19] Coast: zombi lanzado por el flinger (launch_zombie, sin ragdoll) ent %d t=%d\n", n, now); }
        FollowTagFx(e, a.elec); FollowTagFx(e, a.light); FollowTagFx(e, a.death);
    }
    for (auto it = s_act.begin(); it != s_act.end();) {   // entityshutdown: los FX atornillados se van con el
        if (alive.count(it->first)) { ++it; continue; }
        KillTagFx(it->second.elec); KillTagFx(it->second.light); KillTagFx(it->second.death); it = s_act.erase(it);
    }
}

// ===================================================================================================== OLA E2b: fxanims (grua y botes)
// setup_fx_anims / fxanim_init / fxanim_think (zombie_coast.csc): los script_model de cliente con targetname "fxanim" y script_noteworthy
// hook_anim / boat_anim hacen UseAnimTree(fxanim_props_dlc3) y, tras RandomFloatRange(1, 2) s, SetAnim(level.fxanims[nombre], 1, 0, speed 1)
// (los nombres de animacion se leen de level.fxanims[...] = %... del rawfile). Sin VM de CSC: un DObj propio con un arbol root -> anim,
// XAnimSetCompleteGoalWeight como SetAnim, DObjUpdateClientInfo con el tiempo del frame y DObjCalcSkel; la pose va a h7e
// (H19_ModelBones) como base pose inversa * hueso * entidad. H19_COAST_FXANIM=0 los deja quietos.
struct FxAnimObj { int j; std::string anim; XModel *xm; XAnim_s *anims; XAnimTree_s *tree; DObj *obj; DObjAnimMat *skel; int startAt, last; bool on; float org[3], ang[3]; };
std::vector<FxAnimObj> s_fxa; bool s_fxaInit; int s_fxaTs = 1, s_fxaFrames;
void *FxaAlloc(unsigned n) { return calloc(1, n); }
void FxAnimInit(int now)
{
    s_fxaInit = true;
    if (!H19_ModelBones || !H19_EnvOn("H19_COAST_FXANIM")) return;
    std::map<std::string, std::string> names;   // fxanim_init: level.fxanims["hook_anim"] = %fxanim_zom_ship_crane01_hook_anim; ...
    { const std::string t = Raw("clientscripts/zombie_coast.csc"); size_t p = 0;
      while ((p = t.find("level.fxanims[", p)) != std::string::npos) { std::string k; size_t e = 0; if (!Quoted(t, p, k, &e)) break; p = e;
          size_t pc = t.find('%', e), sc = t.find(';', e); if (pc == std::string::npos || sc == std::string::npos || pc > sc) continue;
          std::string a = t.substr(pc + 1, sc - pc - 1); while (!a.empty() && isspace((unsigned char)a.back())) a.pop_back(); names[k] = a; } }
    for (const H19Kv &k : H19_MapEnts()) {
        const char *tn = H19_Str(k, "targetname"), *nw = H19_Str(k, "script_noteworthy"), *mn = H19_Str(k, "model");
        if (!tn || strcmp(tn, "fxanim") || !nw || !mn || !names.count(nw)) continue;   // fxanim_think: sin nombre o desconocido, nada
        float org[3] = { 0, 0, 0 }, ang[3] = { 0, 0, 0 }; H19_Vec(k, "origin", org); H19_Vec(k, "angles", ang);
        int j = -1; for (int i = 0; i < H19_ModelCount(); ++i) { float o[3]; const char *mt = H19_ModelTn(i);
            if (mt && !strcmp(mt, "fxanim") && H19_ModelGet(i, o, nullptr) && fabsf(o[0] - org[0]) + fabsf(o[1] - org[1]) + fabsf(o[2] - org[2]) < 0.5f) { j = i; break; } }
        XModel *xm = DB_FindXAssetHeader(ASSET_TYPE_XMODEL, (char *)mn, 1, -1).model;
        if (j < 0 || !xm || XModelNumBones(xm) > 128) { fprintf(stderr, "[H19] Coast: fxanim %s sin modelo de cliente\n", mn); continue; }
        FxAnimObj f{}; f.j = j; f.anim = names[nw]; f.xm = xm; memcpy(f.org, org, 12); memcpy(f.ang, ang, 12);
        f.anims = XAnimCreateAnims("coast_fxanim", 2, FxaAlloc);
        XAnimBlend(f.anims, 0, "root", 1, 1, 0);
        XAnimCreate(f.anims, 1, (char *)f.anim.c_str());
        if (!f.anims->entries[1].parts) { fprintf(stderr, "[H19] Coast: fxanim: animacion %s no esta\n", f.anim.c_str()); continue; }
        f.tree = XAnimCreateTree(f.anims, FxaAlloc);
        DObjModel_s dm; memset(&dm, 0, sizeof dm); dm.model = xm;
        f.obj = (DObj *)calloc(1, sizeof(DObj));
        DObjCreateExt(&dm, 1, f.tree, (unsigned char *)f.obj, 1023, true, false, 0);
        if (posix_memalign((void **)&f.skel, 16, DObjGetAllocSkelSize(f.obj) + 64)) continue;
        f.startAt = now + (int)(RandRange(1.0f, 2.0f) * 1000.0f); f.last = now;   // wait(RandomFloatRange(1, 2))
        s_fxa.push_back(f);
        fprintf(stderr, "[H19] Coast: fxanim %s (%s) en %.0f %.0f %.0f: %d huesos, %.2f s\n", mn, f.anim.c_str(), org[0], org[1], org[2], XModelNumBones(xm),
            (float)XAnimGetLength(f.anims, 1));
    }
}
void FxAnimFrame(int now)
{
    if (!s_fxaInit) FxAnimInit(now);
    if (s_fxa.empty()) return;
    static DObjSkelMat *out; if (!out && posix_memalign((void **)&out, 16, 128 * sizeof(DObjSkelMat))) return;
    ++s_fxaTs;
    for (FxAnimObj &f : s_fxa) {
        if (!f.on) { if (now < f.startAt) continue;
            XAnimSetCompleteGoalWeight(f.obj, 1, 1.0f, 0.0f, 1.0f, 0, 0, 0, 0); f.on = true; f.last = now; }   // SetAnim(anim, 1.0, 0.0, speed)
        const float dt = (float)(now - f.last) * 0.001f; f.last = now;
        if (dt > 0) DObjUpdateClientInfo(f.obj, dt, 0);
        if (!DObjSkelExists(f.obj, s_fxaTs)) DObjCreateSkel(f.obj, (char *)f.skel, s_fxaTs);
        int bits[5] = { -1, -1, -1, -1, -1 }; DObjCalcSkel(f.obj, bits);
        const DObjAnimMat *mats = DObjGetRotTransArray(f.obj); if (!mats) continue;
        const int nb = XModelNumBones(f.xm); const DObjAnimMat *base = XModelGetBasePose(f.xm);
        DObjSkelMat ent; memset(&ent, 0, sizeof ent); float ax[3][3]; AnglesToAxis(f.ang, ax);
        for (int a = 0; a < 3; ++a) for (int c = 0; c < 3; ++c) ent.axis[a][c] = ax[a][c];
        memcpy(ent.origin, f.org, 12); ent.origin[3] = 1.0f;
        for (int b = 0; b < nb; ++b) {
            DObjSkelMat inv, bone, bw; ConvertQuatToInverseSkelMat(&base[b], &inv); ConvertQuatToSkelMat(&mats[b], &bone);
            R_MultiplySkelMat(&inv, &bone, &bw); R_MultiplySkelMat(&bw, &ent, &out[b]);
            out[b].axis[0][3] = out[b].axis[1][3] = out[b].axis[2][3] = 0.0f; out[b].origin[3] = 1.0f;
        }
        H19_ModelBones(f.j, (const float *)out, nb);
    }
    if (++s_fxaFrames == 200) fprintf(stderr, "[H19] Coast: %zu fxanims animados\n", s_fxa.size());
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
    else if (ev == "ZDA") {   // zombie_director_aggro: PlayRumbleOnPosition + Earthquake(0.4, 1.7, player.origin, 1000)
        if (H5FX_Earthquake) H5FX_Earthquake(0.4f, 1.7f, g_entities[0].r.currentOrigin, 1000.0f);
        fprintf(stderr, "[H19] Coast: el director se enfada (ZDA): temblor 0,4 / 1,7 s%s t=%d\n", H5FX_Earthquake ? "" : " (sin H5FX_Earthquake)", now);
    }
    else if (ev == "dclm" || ev == "dmad") {   // zombie_coast_amb.csc director_futz_change_on/off: setdvarfloat("snd_futz", 0 / 1)
        Dvar_SetFloatByName("snd_futz", ev == "dmad" ? 1.0f : 0.0f);
        fprintf(stderr, "[H19] Coast: snd_futz %d (%s) t=%d\n", ev == "dmad", ev.c_str(), now);
    }
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
    PlayerFlagsFrame(now);   // OLA E2b: escarcha, tirolina, electrocutado
    FrostFrame(now);
    FovFrame(now);
    if (s_fxT0 >= 0) ActorsFrame(now);   // OLA E2b: clientflags de actor (director, zombis electrificados, flinger)
    FxAnimFrame(now);        // OLA E2b: fxanims de la grua y los botes
    {   // resumen periodico
        static int next = 60000;
        if (now >= next) { next = now + 60000;
            fprintf(stderr, "[H19] Coast E2b: escarcha %d (congelado %d, quitada %d, opacidad %.2f), electrocutado %d, zombis electrificados %d, luz del director %d, muerte del director %d, lanzados %d\n",
                s_frostSets, s_freezeSets, s_frostRemoves, s_frostOp, s_elecSets, s_elecZombies, s_lightFlips, s_deathFx, s_ragdolls); }
    }
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

// OLA E2b: director_footsteps (self.footstepPrepend) para h5_audio_cl PlayAIFootstep; solo en Call of the Dead
const char *H19_AIFootPrepend(int entnum)
{
    if (entnum < 0 || entnum >= 1024 || !s_dirSteps[entnum]) return nullptr;
    const H19MapCl *mc = H19_Map(); if (!mc || strcmp(mc->zone, "zombie_coast")) return nullptr;
    if (s_dirSteps[entnum] != g_entities[entnum].s.lerp.useCount + 1) return nullptr;   // otra entidad en el mismo numero
    return "fly_step_director_";
}

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
