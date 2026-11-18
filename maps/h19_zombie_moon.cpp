// OLA M10: cliente de zombie_moon (Moon). Reproduce sin VM de CSC lo que hacen sus clientscripts (rawfiles del ff), con sus mismos
// numeros. El servidor es el GSC original sin tocar: No Man's Land (Area 51, perros), teletransporte a la luna, gravedad baja, oxigeno y
// casco, excavadoras, astronauta, quads, Pack-a-Punch, caja y huevos.
//  - vision (zombie_moon.csc / zombie_moon_fx.csc): el servidor pone zombie_moonHanger18 por el CS 1550 al conectar; despues la pila de
//    _zombiemode.csc con moon_vision_set(viejo, nuevo, tiempo): si viejo == nuevo no hace nada; quita el viejo y pone el nuevo (prioridad 5):
//    zme zombie_moon, zmi zombie_moonInterior, zmb zombie_moonBioDome, zmt zombie_moonTunnels, zmh zombie_moonHanger18, zmhe
//    zombie_moon_hellEarth. _previous_vision sin definir = el nuevo (la primera llamada no cambia nada, como el original).
//  - moon_nml_transition: callback del clientflag 0 del JUGADOR (ps.eFlags2): con 1 niebla y sol de No Man's Land (o de la Tierra
//    destruida con level._dte_done) y vision zmh/zmhe; con 0 niebla de la luna y zme. setVolFog por H7C_ClientVolFog (h7c_gfx.cpp).
//  - disparadores de niebla (moon_fog_triggers_init, 3 s despues de conectar): zombie_moonExterior / Interior / Biodome / Tunnels /
//    zombie_nmlVision; al entrar (trigger_thread) su niebla, sus dvars y su vision con script_int como tiempo.
//  - dvars del CSC (r_lightTweakSun*, sm_sunSampleSizeNear, r_skyColorTemp, r_lightGrid*, r_sky_intensity_*, r_skyTransition con NMS/MMS):
//    se ponen si existen; el renderizador del harness no los usa (igual que Shangri-La, M7).
//  - createfx (clientscripts/createfx/zombie_moon_fx.csc): 204 oneshotfx y 34 FX de exploder. Los del servidor llegan por
//    sysState("@exploder"); los del cliente: Az1 -> 300, Az4a -> 320, Az4b -> 340 (+ sl0 / sl5 sl6 / sl4 con la corriente), tafx -> 121 131,
//    bmfx -> 122 132.
//  - corriente ("ZPO" = power_on de _zombiemode.csc): jump_pad_active en las 11 jump_pads, puertas de la bahia de carga (MoveTo
//    +script_vector en 3 s), 17 pantallas moon_comp_screens (ocultas hasta la corriente), airlock_fx en los 21 airlock_jambs_fx.
//  - dome_malfunction_pad: clientflag 3 del scriptmover: la jump_pad mas cercana parpadea 4-6 veces y se apaga / se enciende.
//  - radar_dish_rotate: 6 antenas giran 360 grados en RandomFloatRange(60, 120) s sin fin.
//  - excavadoras (zombie_moon_digger.csc): consolas tunnel/hangar/biodome con panel_off a los 10 s y panel_on con TCA/HCA/BCA (TCO/HCO/BCO
//    apagan); BIO cambia el modelo de la cupula a p_zom_moon_biodome_hole_broken y enciende sl9 / sl10.
//  - Tierra (S_E/H_E/SDE/HDE): spawn del modelo p_zom_moon_earth(_dest) en (-22060.8, -121800, 34463.4); el harness no lo dibuja (tope de
//    1200 u de los modelos de cliente).
//  - ZEG (fin de partida): vuelve a poner la niebla de la luna o la de No Man's Land segun la ultima vision.
//  - zombie_moon_amb.csc: las salas (vacio = instantanea zmb_moon_airless sin tono) las lleva h5_audio_cl. Aqui: con la corriente las zonas no
//    rotas recuperan su tono (script_string del disparador) y quitan la instantanea (evt_air_repressurize); "Az<zona>" las vuelve a dejar sin
//    aire (evt_air_release, evt_dig_wheel_breakthrough[_bio]); pitidos amb_random_beeps (24, cada 10-30 s); alarmas y temblores de zona con
//    Dz<n> / Dz<n>e; gmsk / gmsk2 (casco); sonidos del teletransporte (tafx: clicks y warmup/cooldown; cafx los cancela; bmfx: rayo);
//    snd_play_auto_fx (amb_lights en los focos); puertas de la bahia de carga.
// No se hace (ver docs/evidencia/m10/LEEME.md): cielo (r_skyTransition: el harness no dibuja el cielo), filtros del casco (clientflags 8/9,
// comunes), vibraciones y earthquake, FX de las excavadoras en sus etiquetas (modelos de servidor sin dibujar), gravedad del ragdoll, huevos.
//   H19_MOON=0 lo apaga. Pruebas: H19_MOON_NOTIFY="ms:ev,..." inyecta levelNotify de cliente en ese level.time (grafico y sonido).
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <universal/dvar.h>
#include <database/db_registry.h>
#include <game_mp/g_main_mp.h>
#include <bgame/bg_local.h>
#include <clientscript/cscr_stringlist.h>
#include <EffectsCore/fx_system.h>
#include <qcommon/cm_trace.h>
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
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "h19_mapcl.h"

bool H7_VisionSetNaked(const char *name, float seconds) __attribute__((weak));   // h7_gfx.cpp
void H7C_ClientVolFog(const float *v, int transitionMs) __attribute__((weak));     // h7c_gfx.cpp: setVolFog del CSC
void __cdecl SV_GetConfigstring(unsigned int index, char *buffer, int bufferSize);
const FxEffectDef *__cdecl FX_Register(char *name);                             // h5_fx.cpp
extern FxSystemContainer *fx_systemPool;                                        // fx_system.cpp (lo reserva h5_fx)
// h7e_light.cpp (OLA M7): pose de los script_model de cliente
int H19_ModelCount();
const char *H19_ModelTn(int j);
const char *H19_ModelName(int j);
bool H19_ModelGet(int j, float *org, float *ang);
void H19_ModelPose(int j, const float *org, const float *ang);
#ifdef H6SND
// h5_audio_cl.cpp (OLA M10): setAmbientRoomTone/Snapshot en juego y "level.activeAmbientRoom = ''" + updateActiveAmbientRoom
void H19A_RoomSet(const char *room, const char *tone, const char *snapshot, float fadeIn, float fadeOut);
void H19A_RoomRefresh();
#endif

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
void SetDvar(const char *n, const char *v) { if (Dvar_FindVar(n)) Dvar_SetFromStringByName(n, (char *)v); }
void SetDvarF(const char *n, float f) { char b[32]; snprintf(b, sizeof b, "%g", f); SetDvar(n, b); }
void SetDvarV(const char *n, float a, float b, float c) { char s[64]; snprintf(s, sizeof s, "%g %g %g", a, b, c); SetDvar(n, s); }
float RandRange(float a, float b) { return a + (b - a) * H19_Rand01(); }
int RandInt(int a, int b) { int v = a + (int)(H19_Rand01() * (float)(b - a)); return v >= b ? b - 1 : v; }   // randomintrange [a, b)

struct Pt { float org[3], ang[3]; std::string ss, sn, si; };
std::map<std::string, std::vector<Pt>> s_structs;   // script_struct por targetname (getstructarray)
std::once_flag s_structOnce;
void LoadStructs()
{
    std::call_once(s_structOnce, []() {
        for (const H19Kv &k : H19_MapEnts()) {
            const char *cn = H19_Str(k, "classname"), *tn = H19_Str(k, "targetname");
            if (!cn || !tn || strcmp(cn, "script_struct")) continue;
            Pt p; memset(p.org, 0, 12); memset(p.ang, 0, 12); H19_Vec(k, "origin", p.org); H19_Vec(k, "angles", p.ang);
            if (const char *s = H19_Str(k, "script_string")) p.ss = s;
            if (const char *s = H19_Str(k, "script_noteworthy")) p.sn = s;
            if (const char *s = H19_Str(k, "script_int")) p.si = s;
            s_structs[tn].push_back(p);
        }
    });
}
const std::vector<Pt> &S(const std::string &tn) { static const std::vector<Pt> none; auto it = s_structs.find(tn); return it == s_structs.end() ? none : it->second; }

// sonidos que pide el cliente grafico (los reproduce el frame de sonido)
struct Snd { float org[3]; std::string alias; };
std::mutex s_sndMu; std::vector<Snd> s_sndQ;
void QueueSound(const float *org, const char *alias) { Snd s; memcpy(s.org, org, 12); s.alias = alias; std::lock_guard<std::mutex> g(s_sndMu); s_sndQ.push_back(s); }

// istouching: caja del jugador contra las brochas del disparador (traza de caja sin recorrido)
bool Touching(unsigned model, const float *torg, const float *org, const float *mins, const float *maxs)
{
    trace_t tr; memset(&tr, 0, sizeof tr); const float zero[3] = { 0, 0, 0 };
    CM_TransformedBoxTrace(&tr, org, org, mins, maxs, model, -1, torg, zero);
    return tr.startsolid || tr.allsolid;
}
struct Trig { unsigned model; float org[3]; std::string tn, room, zone, tone; int si; };
std::vector<Trig> LoadTrigs(const std::set<std::string> &names)
{
    std::vector<Trig> v;
    for (const H19Kv &k : H19_MapEnts()) {
        const char *tn = H19_Str(k, "targetname"), *m = H19_Str(k, "model"); if (!tn || !m || m[0] != '*' || !names.count(tn)) continue;
        Trig t; t.model = (unsigned)atoi(m + 1); memset(t.org, 0, 12); H19_Vec(k, "origin", t.org); t.tn = tn;
        const char *si = H19_Str(k, "script_int"); t.si = si ? atoi(si) : 0;
        if (const char *s = H19_Str(k, "script_ambientroom")) t.room = s;
        if (const char *s = H19_Str(k, "script_noteworthy")) t.zone = s;
        if (const char *s = H19_Str(k, "script_string")) t.tone = s;
        if (t.model && t.model < cm.numSubModels) v.push_back(t);
    }
    return v;
}

// pruebas: H19_MOON_NOTIFY="ms:ev,..." (cada hilo lleva su propia copia)
struct TestQ { std::vector<std::pair<int, std::string>> q; bool parsed = false; };
void TestNotify(TestQ &t, int now, const std::function<void(const std::string &)> &fn, const char *who)
{
    const char *e = getenv("H19_MOON_NOTIFY"); if (!e) return;
    if (!t.parsed) { t.parsed = true; std::string a = e; size_t p = 0;
        while (p < a.size()) { size_t c = a.find(',', p); if (c == std::string::npos) c = a.size(); std::string it = a.substr(p, c - p); size_t k = it.find(':');
            if (k != std::string::npos) t.q.push_back({ atoi(it.c_str()), it.substr(k + 1) }); p = c + 1; } }
    for (auto &x : t.q) if (x.first >= 0 && now >= x.first) { x.first = -1; fprintf(stderr, "[H19] H19_MOON_NOTIFY (%s): '%s' t=%d\n", who, x.second.c_str(), now); fn(x.second); }
}

// ===================================================================================================== efectos y createfx
std::map<std::string, std::string> s_effect;   // level._effect[clave] -> ruta de LoadFx (zombie_moon_fx.csc)
void ParseEffects()
{
    const std::string t = Raw("clientscripts/zombie_moon_fx.csc"), l = Lower(t);
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
int s_fxT0 = -1, s_fxPlayed, s_fxMissing;   // s_fxT0 >= 0: FxSystem en marcha
const char *EffectPath(const char *key) { if (s_fxT0 < 0) return nullptr; auto it = s_effect.find(key); return it == s_effect.end() ? nullptr : it->second.c_str(); }
unsigned FxAng(const char *key, const float *org, const float *ang) { const char *p = EffectPath(key); return p ? H19_PlayFxAng(p, org, ang) : 0; }

struct CfxEnt { float org[3], ang[3], delay; std::string fxid; int exploder; bool done; std::vector<unsigned> fx; };
std::vector<CfxEnt> s_cfx;
std::map<int, int> s_exploderId;   // level._exploder_ids[numero] = id (orden de primera aparicion, desde 1)
std::once_flag s_cfxOnce;
void ParseCreateFx()
{
    const std::string t = Raw("clientscripts/createfx/zombie_moon_fx.csc");
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
    if (s_fxT0 < 0) { if (now <= 0) return; s_fxT0 = now; fprintf(stderr, "[H19] Moon: createfx: reloj de FX en marcha, t0=%d\n", now); }
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
        FX_PlayOrientedEffect(0, fx, s_fxT0 + (int)(o.delay * 1000.0f), o.org, ax);   // create_triggerfx
        ++s_fxPlayed;
    }
    static bool logged;
    if (!logged) { bool all = true; for (const CfxEnt &o : s_cfx) all = all && (o.done || o.exploder >= 0);
        if (all) { logged = true; fprintf(stderr, "[H19] Moon: createfx: %d oneshotfx lanzados (%d sin FX en los datos)\n", s_fxPlayed, s_fxMissing); } }
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

void ExploderNum(int num, bool on, int now)   // activate_exploder / deactivate_exploder (_fx.csc) por numero
{
    int n = 0;
    for (CfxEnt &o : s_cfx) {
        if (o.exploder != num) continue;
        for (unsigned &h : o.fx) H19_KillFx(h);
        o.fx.clear();
        if (!on) continue;
        CfxEnt *po = &o;
        After(now + (o.delay > 0 ? (int)(o.delay * 1000.0f) : 0), [po](int) {   // exploder_delay
            float f[3], r[3], u[3]; AngleVectors(po->ang, f, r, u);
            if (const char *p = EffectPath(po->fxid.c_str())) po->fx.push_back(H19_PlayFx(p, po->org, f, u));
        });
        ++n;
    }
    fprintf(stderr, "[H19] Moon: exploder %d %s: %d efectos t=%d\n", num, on ? "activado" : "desactivado", n, now);
}
void ExploderId(int id, int now)   // sysState("@exploder", id): id del servidor -> numero
{
    int num = -1; for (auto &e : s_exploderId) if (e.second == (id < 0 ? -id : id)) num = e.first;
    if (num < 0) { fprintf(stderr, "[H19] Moon: exploder id %d desconocido\n", id); return; }
    ExploderNum(num, id > 0, now);
}

// ===================================================================================================== vision (CS 1550 y pila de _zombiemode.csc)
std::string s_csVis;
void VisionPoll()
{
    if (!H7_VisionSetNaked || !H19_EnvOn("H7E_VISION")) return;
    char cs[1024]; cs[0] = 0; SV_GetConfigstring(1550, cs, sizeof cs);
    if (s_csVis == cs) return;
    s_csVis = cs;
    std::string v = cs;   // MP: '"nombre" ms'; SP: cadena info con una clave por cliente ("0" = jugador local)
    if (!v.empty() && v[0] == '\\') { char *x = Info_ValueForKey(cs, "0"); v = x ? x : ""; }
    std::string name; size_t e = 0;
    if (!Quoted(v, 0, name, &e)) { size_t a = v.find_first_not_of(' '); if (a == std::string::npos) return;
        e = v.find(' ', a); name = v.substr(a, e == std::string::npos ? std::string::npos : e - a); if (e == std::string::npos) e = v.size(); }
    if (name.empty()) return;
    const int ms = atoi(v.c_str() + e);
    fprintf(stderr, "[H19] Moon: vision del servidor (CS 1550): '%s' en %d ms\n", name.c_str(), ms);
    H7_VisionSetNaked(name.c_str(), ms / 1000.0f);
}
struct Vis { std::string name; int prio; };
std::vector<Vis> s_vis;   // self._zombie_visionset_list
void VisionApplyTop(float sec)
{
    const char *best = nullptr; int hs = 0;
    for (const Vis &v : s_vis) if (v.prio > hs) { hs = v.prio; best = v.name.c_str(); }   // zombie_highest_vision_set_apply (estricta)
    if (!best || !H7_VisionSetNaked || !H19_EnvOn("H7E_VISION")) return;
    fprintf(stderr, "[H19] Moon: vision '%s' (prioridad %d) en %.1f s\n", best, hs, sec);
    H7_VisionSetNaked(best, sec);
}
void VisionApply(const char *name, int prio, float sec)
{
    bool have = false; for (Vis &v : s_vis) if (v.name == name) { v.prio = prio; have = true; break; }
    if (!have) s_vis.push_back({ name, prio });
    VisionApplyTop(sec);
}
void VisionRemove(const char *name, float sec)
{
    for (size_t i = 0; i < s_vis.size(); ++i) if (s_vis[i].name == name) { s_vis.erase(s_vis.begin() + i); break; }
    VisionApplyTop(sec);
}
const char *VisName(const std::string &k)   // moon_vision_set_choice (todas con prioridad 5)
{
    if (k == "zme") return "zombie_moon";
    if (k == "zmi") return "zombie_moonInterior";
    if (k == "zmb") return "zombie_moonBioDome";
    if (k == "zmt") return "zombie_moonTunnels";
    if (k == "zmh") return "zombie_moonHanger18";
    if (k == "zmhe") return "zombie_moon_hellEarth";
    return nullptr;
}
std::string s_prevVis;   // ent_player._previous_vision ("" = sin definir)
void MoonVision(const std::string &nv, int trans)   // ent_player moon_vision_set(_previous_vision, nuevo, lcn, trans); _previous_vision = nuevo
{
    if (s_prevVis.empty()) s_prevVis = nv;
    if (s_prevVis != nv) {
        if (const char *o = VisName(s_prevVis)) VisionRemove(o, 0);
        if (const char *n = VisName(nv)) VisionApply(n, 5, (float)trans);
    }
    s_prevVis = nv;
}

// ===================================================================================================== niebla y sol
// setVolFog(start, halfwayDist, halfwayHeight, baseHeight, r, g, b, colorScale, sunR, sunG, sunB, dirX, dirY, dirZ, startAng, endAng, time, maxOpacity)
const float kFogMoon[18] = { 2098.71f, 1740.12f, 1332.23f, 576.887f, 0.0196078f, 0.0235294f, 0.0352941f, 4.1367f, 0.247f, 0.235f, 0.160f,
                             0.796421f, 0.425854f, 0.429374f, 0, 55, 0, 0.95f };
const float kFogNml[18] = { 1662.13f, 18604.1f, 2618.86f, -5373.56f, 0.764706f, 0.505882f, 0.231373f, 5, 0.8f, 0.435294f, 0.101961f,
                            0.796421f, 0.425854f, 0.429374f, 0, 45.87f, 0, 0.72f };
const float kFogHellEarth[18] = { 2146.77f, 14890.1f, 99.8105f, -294.011f, 0.819608f, 0.34902f, 0.176471f, 3.97095f, 0.666667f, 0.321569f, 0.141176f,
                                  0.804298f, 0.433214f, 0.406731f, 0, 58.1887f, 0, 0.72f };
const float kFogBiodome[18] = { 65.3744f, 860.241f, 35.1158f, 116.637f, 0.117647f, 0.137255f, 0.101961f, 2.96282f, 0.341176f, 0.231373f, 0.141176f,
                                0.315232f, 0.132689f, -0.939693f, 0, 44.4323f, 0, 0.836437f };
const float kFogTunnels[18] = { 1413.46f, 4300.81f, 32.2476f, -238.873f, 0.192157f, 0.137255f, 0.180392f, 3.2984f, 0.34902f, 0.129412f, 0.219608f,
                                0.954905f, 0.280395f, 0.0976461f, 0, 0, 0, 0.22f };
const char *s_fogName = "";
void Fog(const float *v, const char *name)
{
    if (H7C_ClientVolFog) H7C_ClientVolFog(v, (int)(v[16] * 1000.0f));
    if (strcmp(s_fogName, name)) fprintf(stderr, "[H19] Moon: niebla '%s'\n", name);
    s_fogName = name;
}
void Sun(float light, float dx, float dy, float dz, const float *col)   // r_lightTweakSun* (sin consumidor en el harness)
{
    SetDvarF("r_lightTweakSunLight", light); SetDvarV("r_lightTweakSunDirection", dx, dy, dz);
    if (col) SetDvarV("r_lightTweakSunColor", col[0], col[1], col[2]);
}
bool s_dteDone;   // level._dte_done (huevo final: la Tierra destruida)
void FogMoonExterior(int trans, bool interior)   // Moon_Exterior_Fog_Change / moon_interior_fog_change
{
    Fog(kFogMoon, interior ? "luna interior" : "luna exterior");
    Sun(8, -16.28f, 56.06f, 0, nullptr);
    SetDvar("sm_sunSampleSizeNear", interior ? ".8" : "1.8"); SetDvar("r_skyColorTemp", "6400");
    MoonVision(interior ? "zmi" : "zme", trans);
}
void FogBiodome(int trans)
{
    Fog(kFogBiodome, "biodomo");
    Sun(1.13f, -33.34f, -38.56f, 0, nullptr);
    SetDvar("sm_sunSampleSizeNear", "2"); SetDvar("r_sky_intensity_factor0", "2"); SetDvar("r_sky_intensity_useDebugValues", "1");
    SetDvar("r_skyColorTemp", "3600"); SetDvar("r_lightGridEnableTweaks", "1"); SetDvar("r_lightGridIntensity", "1.25"); SetDvar("r_lightGridContrast", ".45");
    MoonVision("zmb", trans);
}
void FogTunnels(int trans)
{
    Fog(kFogTunnels, "tuneles");
    MoonVision("zmt", trans);
    SetDvar("r_skyColorTemp", "6400");
}
void FogNml(int trans)   // moon_nml_fog_change
{
    if (s_dteDone) return;
    static const float col[3] = { .924f, .775f, .651f };
    SetDvar("sm_sunSampleSizeNear", "1.18");
    MoonVision("zmh", trans);
    Fog(kFogNml, "No Man's Land");
    Sun(5, -16, 56.06f, 0, col);
    SetDvar("r_skyColorTemp", "6400");
}
void NmlTransition(bool set)   // moon_nml_transition (clientflag 0 del jugador)
{
    if (set) {
        static const float colNml[3] = { .924f, .775f, .651f }, colHell[3] = { .905f, .203f, 0 };
        Fog(s_dteDone ? kFogHellEarth : kFogNml, s_dteDone ? "Tierra destruida" : "No Man's Land");
        SetDvar("sm_sunSampleSizeNear", "1.18"); SetDvar("r_skyColorTemp", "6400");
        MoonVision(s_dteDone ? "zmhe" : "zmh", 0);
        Sun(5, -16, 56.06f, 0, s_dteDone ? colHell : colNml);
    } else {
        static const float col[3] = { 0.655f, 0.768f, 0.817f };
        Fog(kFogMoon, "luna");
        SetDvar("sm_sunSampleSizeNear", "1.8"); SetDvar("r_lightGridEnableTweaks", "1"); SetDvar("r_lightGridIntensity", "2"); SetDvar("r_lightGridContrast", ".4");
        MoonVision("zme", 0);
        Sun(8, -16.28f, 56.06f, 0, col);
    }
    fprintf(stderr, "[H19] Moon: moon_nml_transition(%d)\n", set ? 1 : 0);
}
std::vector<Trig> s_fogTrigs;
std::vector<bool> s_fogIn;
int s_fogStart = -1;   // fog_triggers_setup: waitforclient + wait(3)
bool s_flag0, s_flag0Seen;
void FogFrame(int now)
{
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return;
    const bool f = pl->client->ps.eFlags2 & 1;   // _CLIENTFLAG_PLAYER_SKY_TRANSITION = 0 (ps.eFlags2)
    if (f != s_flag0) { s_flag0 = f; s_flag0Seen = true; NmlTransition(f); }   // callback solo al cambiar (de 0 al conectar no se llama)
    if (s_fogStart < 0) s_fogStart = now + 3000;
    if (now < s_fogStart) return;
    if (s_fogIn.size() != s_fogTrigs.size()) s_fogIn.assign(s_fogTrigs.size(), false);
    for (size_t i = 0; i < s_fogTrigs.size(); ++i) {   // fog_trigger + trigger_thread: change_func al entrar
        const Trig &t = s_fogTrigs[i];
        const bool in = Touching(t.model, t.org, pl->r.currentOrigin, pl->r.mins, pl->r.maxs);
        if (in && !s_fogIn[i]) {
            if (t.tn == "zombie_moonExterior") FogMoonExterior(t.si, false);
            else if (t.tn == "zombie_moonInterior") FogMoonExterior(t.si, true);
            else if (t.tn == "zombie_moonBiodome") FogBiodome(t.si);
            else if (t.tn == "zombie_moonTunnels") FogTunnels(t.si);
            else if (t.tn == "zombie_nmlVision") FogNml(t.si);
        }
        s_fogIn[i] = in;
    }
}

// ===================================================================================================== modelos de cliente
std::vector<int> ModelsByTn(const char *tn) { std::vector<int> v; for (int j = 0; j < H19_ModelCount(); ++j) if (!strcmp(H19_ModelTn(j), tn)) v.push_back(j); return v; }
void Hide(int j, bool hide)   // Hide()/Show(): el harness no tiene ocultar; se aparca el modelo lejos (fuera del tope de distancia)
{
    float o[3], a[3]; if (!H19_ModelGet(j, o, a)) return;
    const bool hidden = o[2] < -50000.0f;
    if (hide == hidden) return;
    o[2] += hide ? -100000.0f : 100000.0f; H19_ModelPose(j, o, a);
}
// radar_dish_rotate: rotateyaw(360, RandomFloatRange(60, 120)) sin fin
struct Dish { int j; float org[3], ang[3], yaw0; int t0, dur; };
std::vector<Dish> s_dishes;
void DishFrame(int now)
{
    for (Dish &d : s_dishes) {
        if (d.t0 < 0) { d.t0 = now + 100; d.dur = (int)(RandRange(60, 120) * 1000.0f); }   // wait(0.1)
        if (now < d.t0) continue;
        if (now >= d.t0 + d.dur) { d.yaw0 = fmodf(d.yaw0 + 360.0f, 360.0f); d.t0 += d.dur; d.dur = (int)(RandRange(60, 120) * 1000.0f); }
        float a[3] = { d.ang[0], d.yaw0 + 360.0f * (float)(now - d.t0) / (float)d.dur, d.ang[2] };
        H19_ModelPose(d.j, d.org, a);
    }
}
// receiving_bay_doors: MoveTo(origin + script_vector, 3)
struct Door { int j; float from[3], to[3], ang[3]; };
std::vector<Door> s_doors; int s_doorT0 = -1;
void DoorFrame(int now)
{
    if (s_doorT0 < 0) return;
    const float t = fminf(1.0f, (float)(now - s_doorT0) / 3000.0f);
    for (const Door &d : s_doors) { float o[3]; for (int k = 0; k < 3; ++k) o[k] = d.from[k] + (d.to[k] - d.from[k]) * t; H19_ModelPose(d.j, o, d.ang); }
    if (t >= 1.0f) s_doorT0 = -2;
}
// jump pads (jump_pad_start_fx) y dome_malfunction_pad
struct Pad { float org[3], ang[3]; unsigned glow; };
std::vector<Pad> s_pads; bool s_padsOn;
void PadGlow(Pad &p, bool on)
{
    H19_KillFx(p.glow);
    if (on) p.glow = FxAng("jump_pad_active", p.org, p.ang);   // PlayFXOnTag(tag_origin de un script_model en el origen y angulos de la pad)
}
int ClosestPad(const float *o)
{
    int best = -1; float bd = 999999.0f;
    for (size_t i = 0; i < s_pads.size(); ++i) { const float dx = o[0] - s_pads[i].org[0], dy = o[1] - s_pads[i].org[1], d = sqrtf(dx * dx + dy * dy); if (d < bd) { bd = d; best = (int)i; } }
    return best;
}
void PadMalfunction(const float *org, bool set, int now)
{
    const int i = ClosestPad(org); if (i < 0 || !s_padsOn) return;
    int t = now; const int n = RandInt(4, 7);
    for (int k = 0; k < n; ++k) {   // set: StopFX, espera, surge + FX, espera ... y StopFX; clear: surge + FX, espera, StopFX, espera ... y FX
        if (set) {
            After(t, [i](int) { PadGlow(s_pads[i], false); }); t += (int)(RandRange(.05f, .15f) * 1000.0f);
            After(t, [i](int) { QueueSound(s_pads[i].org, "evt_electrical_surge"); PadGlow(s_pads[i], true); }); t += (int)(RandRange(.05f, .15f) * 1000.0f);
        } else {
            After(t, [i](int) { QueueSound(s_pads[i].org, "evt_electrical_surge"); PadGlow(s_pads[i], true); }); t += (int)(RandRange(.05f, .15f) * 1000.0f);
            After(t, [i](int) { PadGlow(s_pads[i], false); }); t += (int)(RandRange(.05f, .15f) * 1000.0f);
        }
    }
    After(t, [i, set](int) { PadGlow(s_pads[i], !set); });
    fprintf(stderr, "[H19] Moon: jump pad %d (%.0f %.0f) %s (%d parpadeos) t=%d\n", i, s_pads[i].org[0], s_pads[i].org[1], set ? "averiada" : "reparada", n, now);
}
std::map<int, unsigned> s_moverFl;
void MoversFrame(int now)   // clientflags de scriptmover del servidor: 3 = dome_malfunction_pad
{
    for (int n = 0; n < level.num_entities; ++n) {
        gentity_s *e = &g_entities[n];
        if (!e->r.inuse || e->s.eType != ET_SCRIPTMOVER) { s_moverFl.erase(n); continue; }
        const unsigned fl = e->s.lerp.eFlags2 & (1u << 3);
        auto it = s_moverFl.find(n); const unsigned prev = it == s_moverFl.end() ? 0 : it->second;
        if (it != s_moverFl.end() && fl == prev) continue;
        s_moverFl[n] = fl;
        if (fl != prev) PadMalfunction(e->r.currentOrigin, fl != 0, now);
    }
}
// consolas de las excavadoras (excavator_console)
struct Console { const char *name, *on, *off; int j; float org[3], ang[3]; unsigned fx; bool lit; };
Console s_cons[3] = { { "tunnel", "TCA", "TCO", -1, {}, {}, 0, false }, { "hangar", "HCA", "HCO", -1, {}, {}, 0, false }, { "biodome", "BCA", "BCO", -1, {}, {}, 0, false } };
int s_consAt = -1; bool s_consInit;
void ConsoleFx(Console &c, bool on) { H19_KillFx(c.fx); c.fx = FxAng(on ? "panel_on" : "panel_off", c.org, c.ang); c.lit = on; }
// Tierra (S_E / H_E / SDE / HDE)
int s_earth = -1;
void Earth(const char *model)
{
    static const float org[3] = { -22060.8f, -121800, 34463.4f }, ang[3] = { 18, 78, 22 }, park[3] = { -22060.8f, -121800, -100000 };
    if (!model) { if (s_earth >= 0) H19_ModelPose(s_earth, park, ang); return; }
    if (s_earth < 0) s_earth = H19_AddModel(model, org, ang, "moon_earth");
    else { H19_SetModel(s_earth, model); H19_ModelPose(s_earth, org, ang); }
    fprintf(stderr, "[H19] Moon: Tierra '%s' %s (a %.0f u: fuera del tope de dibujo de los modelos de cliente)\n", model, s_earth >= 0 ? "creada" : "NO esta en los datos",
        sqrtf(org[0] * org[0] + org[1] * org[1] + org[2] * org[2]));
}

// ===================================================================================================== avisos del servidor
std::mutex s_evMu; std::vector<std::pair<std::string, std::string>> s_evQ;   // (sistema, estado); sistema "" = levelNotify
bool s_power;
int s_breach[3] = { -1, -1, -1 };   // Az1 / Az4a / Az4b vistos: esperan la corriente para su sl
std::vector<int> s_screens; std::vector<Pt> s_airlocks;
void PowerOn(int now)   // ZPO -> power_on (ZPO_listener de _zombiemode.csc)
{
    if (s_power) return;
    s_power = true;
    s_padsOn = true; for (Pad &p : s_pads) PadGlow(p, true);   // jump_pad_activate
    s_doorT0 = now;   // receiving_bay_doors (los sonidos los pone el hilo de sonido)
    for (int j : s_screens) Hide(j, false);   // computer_screens_power
    int nair = 0;
    for (const Pt &a : s_airlocks) {   // airlock_fx: PlayFX(fx, origin, AnglesToForward(angles)) = angulos (pitch, yaw, 0)
        float f[3], r[3], u[3], ang[3]; AngleVectors(a.ang, f, r, u); vectoangles(f, ang);
        nair += FxAng("airlock_fx", a.org, ang) ? 1 : 0;
    }
    static const char *const sl[3][2] = { { "sl0", nullptr }, { "sl5", "sl6" }, { "sl4", nullptr } };
    for (int k = 0; k < 3; ++k) if (s_breach[k] >= 0) for (const char *e : sl[k]) if (e) H19_MixerActivate(e, now);
    fprintf(stderr, "[H19] Moon: corriente (ZPO): %zu jump pads, %zu puertas de la bahia, %zu pantallas, %d campos de las esclusas t=%d\n",
        s_pads.size(), s_doors.size(), s_screens.size(), nair, now);
}
void Notify(const std::string &ev, int now)
{
    static const char *const kAz[3] = { "Az1", "Az4a", "Az4b" }; static const int kAzEx[3] = { 300, 320, 340 };
    static const char *const sl[3][2] = { { "sl0", nullptr }, { "sl5", "sl6" }, { "sl4", nullptr } };
    if (ev == "ZPO") PowerOn(now);
    else if (ev == "NMS") { if (!s_dteDone) SetDvarF("r_skyTransition", 1); }   // no_mans_land_sky
    else if (ev == "MMS") SetDvarF("r_skyTransition", 0);                       // moon_sky
    else if (ev == "tafx") { ExploderNum(121, true, now); ExploderNum(131, true, now); }   // teleporter_audio_sfx (el sonido en su hilo)
    else if (ev == "bmfx") { ExploderNum(122, true, now); ExploderNum(132, true, now); }   // beam_fx_audio
    else if (ev == "S_E" || ev == "SDE") Earth(ev == "S_E" ? "p_zom_moon_earth" : "p_zom_moon_earth_dest");
    else if (ev == "H_E" || ev == "HDE") Earth(nullptr);
    else if (ev == "BIO") {   // wait_for_biodome_breach
        const int n = H19_SwapModelByTn("biodome_breached", "p_zom_moon_biodome_hole_filled", "p_zom_moon_biodome_hole_broken");
        H19_MixerActivate("sl9", now); H19_MixerActivate("sl10", now);
        fprintf(stderr, "[H19] Moon: cupula del biodomo rota (BIO): %d modelo(s) cambiados t=%d\n", n, now);
    }
    else if (ev == "ZEG") {   // game_over_fog_and_vision_fix
        if (s_prevVis == "zme" || s_prevVis == "zmi" || s_prevVis == "zmb" || s_prevVis == "zmt") FogMoonExterior(0, false); else FogNml(0);
    }
    else if (ev == "dte") s_dteDone = true;   // dte_watcher (huevo final) solo marca level._dte_done
    else {
        for (Console &c : s_cons) { if (ev == c.on && c.j >= 0) ConsoleFx(c, true); else if (ev == c.off && c.j >= 0 && s_consInit) ConsoleFx(c, false); }
        for (int k = 0; k < 3; ++k) if (ev == kAz[k] && s_breach[k] < 0) {   // breach_*_fx
            s_breach[k] = now; ExploderNum(kAzEx[k], true, now);
            if (s_power) for (const char *e : sl[k]) if (e) H19_MixerActivate(e, now);
        }
    }
}

int s_padModels;
void Init(const std::vector<H19Kv> &)
{
    LoadStructs(); EnsureCreateFx();
    s_fogTrigs = LoadTrigs({ "zombie_moonExterior", "zombie_moonInterior", "zombie_moonBiodome", "zombie_moonTunnels", "zombie_nmlVision" });
    for (int j : ModelsByTn("jump_pads")) { Pad p; H19_ModelGet(j, p.org, p.ang); p.glow = 0; s_pads.push_back(p); }
    s_screens = ModelsByTn("moon_comp_screens"); for (int j : s_screens) Hide(j, true);
    for (int j : ModelsByTn("zombie_cosmodrome_radar_dish")) { Dish d; d.j = j; H19_ModelGet(j, d.org, d.ang); d.yaw0 = d.ang[1]; d.t0 = -1; d.dur = 60000; s_dishes.push_back(d); }
    for (Console &c : s_cons) { const std::string tn = std::string(c.name) + "_console"; std::vector<int> v = ModelsByTn(tn.c_str());
        if (!v.empty()) { c.j = v[0]; H19_ModelGet(c.j, c.org, c.ang); } }
    // receiving_bay_doors: script_vector de la entidad del mapa (en orden con los modelos del mismo targetname)
    std::vector<int> dm = ModelsByTn("receiving_bay_doors"); size_t di = 0;
    for (const H19Kv &k : H19_MapEnts()) {
        const char *tn = H19_Str(k, "targetname"); if (!tn || strcmp(tn, "receiving_bay_doors") || di >= dm.size()) continue;
        const int j = dm[di++]; float sv[3]; if (!H19_Vec(k, "script_vector", sv)) continue;
        Door d; d.j = j; H19_ModelGet(j, d.from, d.ang); for (int c = 0; c < 3; ++c) d.to[c] = d.from[c] + sv[c]; s_doors.push_back(d);
    }
    for (const H19Kv &k : H19_MapEnts()) {   // airlock_jambs_fx (script_origin de cliente)
        const char *tn = H19_Str(k, "targetname"); if (!tn || strcmp(tn, "airlock_jambs_fx")) continue;
        Pt p; memset(p.org, 0, 12); memset(p.ang, 0, 12); H19_Vec(k, "origin", p.org); H19_Vec(k, "angles", p.ang); s_airlocks.push_back(p);
    }
    size_t nexp = 0; for (const CfxEnt &o : s_cfx) nexp += o.exploder >= 0;
    fprintf(stderr, "[H19] Moon: %zu level._effect, createfx %zu (%zu exploders en %zu numeros), %zu disparadores de niebla, %zu jump pads, %zu puertas, "
        "%zu pantallas, %zu antenas, %zu esclusas, consolas %d/%d/%d, niebla de cliente %s\n",
        s_effect.size(), s_cfx.size(), nexp, s_exploderId.size(), s_fogTrigs.size(), s_pads.size(), s_doors.size(), s_screens.size(), s_dishes.size(),
        s_airlocks.size(), s_cons[0].j, s_cons[1].j, s_cons[2].j, H7C_ClientVolFog ? "si" : "NO");
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
// diagnostico de pruebas (H19_MOON_DIAG=ms): IA viva por classname, gravedad del jugador (ps.gravity), salud y bandera de jadeo (clientflag 2)
void DiagFrame(int now)
{
    static const int every = getenv("H19_MOON_DIAG") ? atoi(getenv("H19_MOON_DIAG")) : 0; static int next;
    if (every <= 0 || now < next) return;
    next = now + every;
    std::map<std::string, int> ai; std::map<std::string, float> near;   // cuantos y a que distancia esta el mas cercano
    const float *po = g_entities[0].r.currentOrigin;
    for (int n = 0; n < level.num_entities; ++n) { gentity_s *e = &g_entities[n];
        if (!e->r.inuse || e->s.eType != ET_ACTOR || e->health <= 0) continue;
        const std::string cn = e->classname ? SL_ConvertToString(e->classname, SCRIPTINSTANCE_SERVER) : "?"; ++ai[cn];
        const float dx = e->r.currentOrigin[0] - po[0], dy = e->r.currentOrigin[1] - po[1], dz = e->r.currentOrigin[2] - po[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!near.count(cn) || d < near[cn]) near[cn] = d; }
    std::string s; for (auto &a : ai) s += " " + a.first + "=" + std::to_string(a.second) + "(" + std::to_string((int)near[a.first]) + "u)";
    static std::vector<Trig> dogVol; static bool dv;
    if (!dv) { dv = true; dogVol = LoadTrigs({ "nml_dogs_volume" });
        for (const Trig &t : dogVol) { float mn[3], mx[3]; CM_ModelBounds(t.model, mn, mx);
            fprintf(stderr, "[H19] Moon diag: nml_dogs_volume *%u limites %.0f %.0f %.0f / %.0f %.0f %.0f sobre %.0f %.0f %.0f\n", t.model, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], t.org[0], t.org[1], t.org[2]); } }
    gentity_s *pl = &g_entities[0];
    if (!pl->r.inuse || !pl->client) return;
    bool inDogs = false; for (const Trig &t : dogVol) inDogs = inDogs || Touching(t.model, t.org, pl->r.currentOrigin, pl->r.mins, pl->r.maxs);
    fprintf(stderr, "[H19] Moon diag t=%d: jugador (%.0f %.0f %.0f) gravedad %d salud %d jadeo %d volumen de perros %d | IA:%s\n", now, pl->r.currentOrigin[0],
        pl->r.currentOrigin[1], pl->r.currentOrigin[2], pl->client->ps.gravity, pl->health, (pl->client->ps.eFlags2 >> 2) & 1, inDogs, s.empty() ? " ninguna" : s.c_str());
}
TestQ s_testG;
void Frame(int now)
{
    DiagFrame(now);
    TestNotify(s_testG, now, [](const std::string &ev) { std::lock_guard<std::mutex> g(s_evMu); s_evQ.push_back({ "", ev }); }, "grafico");
    VisionPoll();
    CreateFxFrame();
    if (s_consAt < 0) s_consAt = now + 10000;   // init_excavator_consoles: waitforallclients + wait(10)
    if (!s_consInit && now >= s_consAt && s_fxT0 >= 0) { s_consInit = true; for (Console &c : s_cons) if (c.j >= 0 && !c.lit) ConsoleFx(c, false); }
    std::vector<std::pair<std::string, std::string>> q; { std::lock_guard<std::mutex> g(s_evMu); q.swap(s_evQ); }
    std::string last;
    for (auto &e : q) {
        if (e.first + "=" + e.second == last) continue;   // el gancho de h7e puede ir encadenado dos veces
        last = e.first + "=" + e.second;
        if (e.first.empty()) Notify(e.second, now);
        else if (e.first == "@exploder") ExploderId(atoi(e.second.c_str()), now);
    }
    RunTasks(now);
    FogFrame(now);
    MoversFrame(now);
    DishFrame(now);
    DoorFrame(now);
}
const char *LevelEffect(const char *key) { auto it = s_effect.find(key); return it == s_effect.end() ? nullptr : it->second.c_str(); }

// ===================================================================================================== sonido (zombie_moon_amb.csc)
#ifdef H6SND
}   // namespace
int __cdecl CG_PlaySoundWithHandle(int localClientNum, int entitynum, float *origin, int fadeMs, bool doNotify, float volume, unsigned int id);   // h5_audio_cl.cpp
namespace {
unsigned s_rng = 0x6d6f6f6eu;   // secuencia propia del hilo de sonido
float RandF(float a, float b) { s_rng = s_rng * 1664525u + 1013904223u; return a + (b - a) * ((s_rng >> 8) / 16777216.0f); }
int RandI(int a, int b) { int v = a + (int)RandF(0, (float)(b - a)); return v >= b ? b - 1 : v; }
int s_shots;
int Play(const float *org, const char *alias)
{
    float p[3]; if (org) { p[0] = org[0]; p[1] = org[1]; p[2] = org[2]; }
    ++s_shots;
    return CG_PlaySoundWithHandle(0, 1022, org ? p : nullptr, 0, 0, 1.0f, SND_FindAliasId(alias));
}
const float kZero[3] = { 0, 0, 0 };
struct Timed { double at; float org[3]; std::string alias; int group; };   // sonido diferido; group 1 = teletransporte (cafx lo cancela)
std::vector<Timed> s_timed;
void PlayAt(double at, const float *org, const char *alias, int group = 0) { Timed t; t.at = at; memcpy(t.org, org, 12); t.alias = alias; t.group = group; s_timed.push_back(t); }
struct Beep { float org[3]; double at; };
std::vector<Beep> s_beeps;
struct Alarm { float org[3]; std::string zone; bool shake; int state; double at; };   // state 0 espera Dz, 1 sonando, 2 espera reset (2 s)
std::vector<Alarm> s_alarms; double s_alarmsAt = -1;
std::vector<Trig> s_ambTrigs; std::vector<bool> s_firstTime;
std::map<std::string, bool> s_breached;
bool s_aPower, s_ambReady; double s_ambAt = -1;
int s_autoFx, s_beepPlays, s_alarmPlays, s_resets;
std::vector<int> s_doorFakes; double s_doorStopAt = -1;
std::set<std::string> s_aPending;   // avisos que llegan antes de setup_airless_ambient_packages (wait 5)
void ResetZone(const std::string &zone, bool poweron)   // reset_ambient_packages
{
    const gentity_s *pl = H19A_PlayerEnt();
    std::vector<size_t> arr;
    for (size_t i = 0; i < s_ambTrigs.size(); ++i) {
        Trig &t = s_ambTrigs[i]; if (t.zone != zone || t.room.empty()) continue;
        if (poweron && !s_breached[zone]) { if (!t.tone.empty()) H19A_RoomSet(t.room.c_str(), t.tone.c_str(), "", .25f, .25f); else H19A_RoomSet(t.room.c_str(), nullptr, "", .25f, .25f); arr.push_back(i); }
        else { H19A_RoomSet(t.room.c_str(), "", "zmb_moon_airless", .25f, .25f); arr.push_back(i); }
    }
    for (size_t i : arr) {
        const Trig &t = s_ambTrigs[i];
        if (pl && Touching(t.model, t.org, pl->r.currentOrigin, pl->r.mins, pl->r.maxs)) {
            H19A_RoomRefresh();
            if (!s_breached[zone]) {
                if (poweron) Play(kZero, "evt_air_repressurize");
                else if (!s_firstTime[i]) {
                    if (zone == "5") Play(kZero, "evt_dig_wheel_breakthrough_bio");
                    else if (((zone == "2a" || zone == "2b") && !s_breached["2b"]) || ((zone == "3a" || zone == "3b" || zone == "3c") && !s_breached["3b"])) Play(kZero, "evt_dig_wheel_breakthrough");
                    Play(kZero, "evt_air_release");
                }
            }
        }
        s_firstTime[i] = false;
    }
    if (!poweron) s_breached[zone] = true;
    ++s_resets;
    fprintf(stderr, "[H19] Moon sonido: zona %s %s (%zu disparadores) t=%d\n", zone.c_str(), poweron ? "con aire (corriente)" : "sin aire (rota)", arr.size(), level.time);
}
void AudioPowerOn()
{
    if (s_aPower) return;
    s_aPower = true;
    const double now = level.time;
    for (const Pt &p : S("amb_random_beeps")) { Beep b; memcpy(b.org, p.org, 12); b.at = now; s_beeps.push_back(b); }   // ambience_randoms
    if (s_ambReady) for (const char *z : { "1", "2a", "2b", "3a", "3b", "3c", "4a", "4b", "5", "6" }) ResetZone(z, true);   // setup_zone_1_special
    for (const H19Kv &k : H19_MapEnts()) {   // receiving_bay_doors: evt_loading_door_start + loop (.5) ... 2,6 s: stoploopsound(.5) + evt_loading_door_end
        const char *tn = H19_Str(k, "targetname"); float o[3], sv[3]; if (!tn || strcmp(tn, "receiving_bay_doors") || !H19_Vec(k, "script_vector", sv)) continue;
        H19_Vec(k, "origin", o); Play(o, "evt_loading_door_start");
        const int f = H19A_FakeAt(o); if (f) { H19A_PlayLoop(f, "evt_loading_door_loop", .5f); s_doorFakes.push_back(f); }
        PlayAt(now + 2600, o, "evt_loading_door_end");
    }
    s_doorStopAt = now + 2600;
}
void AudioStart()
{
    LoadStructs(); EnsureCreateFx();
    for (const CfxEnt &o : s_cfx) if (o.fxid == "fx_moon_floodlight_wide" || o.fxid == "fx_moon_floodlight_narrow") {   // snd_start_autofx_audio
        const int f = H19A_FakeAt(o.org); if (f) { H19A_PlayLoop(f, "amb_lights", 0.5f); ++s_autoFx; } }
    s_ambTrigs = LoadTrigs({ "ambient_package" }); s_firstTime.assign(s_ambTrigs.size(), true);
    for (const char *z : { "1", "2a", "2b", "3a", "3b", "3c", "4a", "4b", "5", "6" }) s_breached[z] = false;
    s_ambAt = level.time + 5000.0; s_alarmsAt = level.time + 5000.0;   // setup_airless_ambient_packages / zone_alarms_setup: wait(5)
    fprintf(stderr, "[H19] Moon sonido: %d focos con amb_lights, %zu disparadores ambient_package, %zu pitidos, %zu alarmas, %zu temblores, %zu clicks de teletransporte\n",
        s_autoFx, s_ambTrigs.size(), S("amb_random_beeps").size(), S("zone_alarm").size(), S("zone_shakes").size(), S("teleporter_click_sfx").size());
}
void AudioNotifyEv(const std::string &n)
{
    const double now = level.time;
    if (n == "ZPO") { if (s_ambReady || s_ambAt < 0) AudioPowerOn(); else s_aPending.insert("ZPO"); return; }
    if (n == "gmsk2") Play(kZero, "evt_gasmask_on");   // waitfor_gasmask_on
    else if (n == "gmsk") { static bool once; if (!once) { once = true; Play(kZero, "evt_gasmask_suit_on"); } }   // waitfor_gasmask_buy (una vez)
    else if (n == "bmfx") Play(kZero, "evt_teleporter_beam_sfx");
    else if (n == "tafx") {   // play_teleporter_sounds / play_warmup_cooldown (level endon "cafx")
        for (const Pt &p : S("teleporter_click_sfx")) if (!p.si.empty() && !p.sn.empty())
            PlayAt(now + 500 + atof(p.sn.c_str()) / 2.0 * 1000.0, p.org, ("evt_teleporter_click_" + p.sn).c_str(), 1);
        for (const Pt &p : S("teleporter_warmup_sfx")) { PlayAt(now + 500, p.org, "evt_teleporter_warmup", 1); PlayAt(now + 2500, p.org, "evt_teleporter_cooldown", 1); }
    }
    else if (n == "cafx") { for (size_t i = 0; i < s_timed.size();) if (s_timed[i].group == 1) s_timed.erase(s_timed.begin() + i); else ++i; }
    else if (n.size() > 2 && n[0] == 'A' && n[1] == 'z') {   // waitfor_notify("Az<zona>")
        const std::string z = n.substr(2);
        if (s_breached.count(z)) { if (s_ambReady) { if (!s_breached[z]) ResetZone(z, false); } else s_aPending.insert(n); }
    }
    else if (n.size() > 2 && n[0] == 'D' && n[1] == 'z') {   // play_zone_alarms / play_zone_shakes
        const bool end = n.back() == 'e'; const std::string z = n.substr(2, n.size() - 2 - (end ? 1 : 0));
        for (Alarm &a : s_alarms) if (a.zone == z) {
            if (!end && a.state == 0) { a.state = 1; a.at = now; }
            else if (end && a.state == 1) { a.state = 2; a.at = now + 2000; }   // reset_*: wait(2) y vuelve a esperar Dz
            else if (end && a.state == 0) { a.state = 2; a.at = now + 2000; }
        }
    }
}
TestQ s_testA;
void AudioNotify(const std::string &n) { AudioNotifyEv(n); }
void AudioFrame(bool)
{
    const double now = level.time;
    TestNotify(s_testA, level.time, [](const std::string &ev) { AudioNotifyEv(ev); }, "sonido");
    { std::vector<Snd> q; { std::lock_guard<std::mutex> g(s_sndMu); q.swap(s_sndQ); } for (const Snd &s : q) Play(s.org, s.alias.c_str()); }
    for (size_t i = 0; i < s_timed.size();) { if (now >= s_timed[i].at) { Timed t = s_timed[i]; s_timed.erase(s_timed.begin() + i); Play(t.org, t.alias.c_str()); } else ++i; }
    if (s_doorStopAt >= 0 && now >= s_doorStopAt) { s_doorStopAt = -1; for (int f : s_doorFakes) H19A_StopLoop(f, .5f); }
    if (!s_ambReady && s_ambAt >= 0 && now >= s_ambAt) {   // setup_airless_ambient_packages
        s_ambReady = true;
        if (s_aPending.count("ZPO")) AudioPowerOn();
        for (const std::string &e : s_aPending) if (e != "ZPO") AudioNotifyEv(e);
        s_aPending.clear();
    }
    if (s_alarmsAt >= 0 && now >= s_alarmsAt) {   // zone_alarms_setup
        s_alarmsAt = -1;
        for (const Pt &p : S("zone_alarm")) { Alarm a; memcpy(a.org, p.org, 12); a.zone = p.sn; a.shake = false; a.state = 0; a.at = 0; s_alarms.push_back(a); }
        for (const Pt &p : S("zone_shakes")) { Alarm a; memcpy(a.org, p.org, 12); a.zone = p.sn; a.shake = true; a.state = 0; a.at = 0; s_alarms.push_back(a); }
    }
    for (Alarm &a : s_alarms) {
        if (a.state == 2 && now >= a.at) a.state = 0;
        if (a.state != 1 || now < a.at) continue;
        Play(a.org, a.shake ? "evt_digger_rattles_random" : "evt_zone_alarm"); ++s_alarmPlays;
        a.at = now + (a.shake ? RandF(1.2f, 2.3f) * 1000.0 : 2800.0);
    }
    for (Beep &b : s_beeps) if (now >= b.at) { Play(b.org, "amb_random_beeps"); ++s_beepPlays; b.at = now + RandI(10, 30) * 1000.0; }   // play_random_beeps
}
void AudioStatus(std::string &out)
{
    std::string br; for (auto &b : s_breached) if (b.second) br += b.first + " ";
    char b[300]; snprintf(b, sizeof b, "; Moon: corriente %s, zonas rotas [%s], reajustes %d, focos %d, pitidos %d, alarmas %d, sonidos %d",
        s_aPower ? "si" : "no", br.c_str(), s_resets, s_autoFx, s_beepPlays, s_alarmPlays, s_shots);
    out += b;
}
#endif
} // namespace

extern const H19MapCl kH19_zombie_moon = {
    .zone = "zombie_moon", .offEnv = "H19_MOON",
    .estado = "Moon: visiones por zona, niebla de luna/No Man's Land/biodomo/tuneles, createfx y exploders, jump pads, puertas, pantallas, esclusas, antenas, consolas, cupula y sonido de vacio (OLA M10)",
    .init = Init, .levelNotify = LevelNotify, .sysState = SysState, .frame = Frame, .clockHand = nullptr, .screensFrame = nullptr,
    .levelEffect = LevelEffect,
#ifdef H6SND
    .audioStart = AudioStart, .audioNotify = AudioNotify, .audioSys = nullptr, .audioFrame = AudioFrame, .audioStatus = AudioStatus,
#endif
    .doorEnt = nullptr,
};
#endif
