// OLA M6: cliente de zombie_cod5_asylum (Verruckt). Reproduce sin VM de CSC lo que hacen sus clientscripts (rawfiles del ff), con sus
// mismos numeros. El servidor es el GSC original sin tocar: interruptor de la corriente (master_electric_switch), puertas y escombros,
// caja que se mueve con su luz (magic_box_light), 4 ventajas, 2 trampas electricas (electric_trap_think), retrete y silla (huevos),
// radio y barricadas. Verruckt NO tiene perros (level.dogs_enabled = false) ni Pack-a-Punch (level.has_pack_a_punch = false).
//  - vision: createart/zombie_cod5_asylum_art.gsc hace VisionSetNaked("zombie_asylum", 0); el servidor lo deja en el configstring 1550 y
//    el cliente original lo lee (CG_VisionSetConfigString_Naked). El harness solo carga vision/<mapname>.vision (no existe): se lee el
//    CS 1550 y se aplica con H7_VisionSetNaked (mismo metodo que Nacht, OLA M3). La niebla (setVolFog del servidor) va por el CS 10 (h7c).
//  - createfx (clientscripts/createfx/zombie_cod5_asylum_fx.csc + _fx.csc): 204 oneshotfx y 18 exploders (numero 101: rayos de luz del
//    techo light_ceiling_dspot que se encienden 6,3 s despues de la corriente con exploder(101)). Se leen del rawfile, no se copian:
//    oneshotfxthread (si delay > 0 espera delay s) + create_triggerfx (playFx de 6 parametros: inicio = delay*1000 desde el arranque).
//    Los exploders llegan del servidor como '@' <id> (ActivateClientExploder; id = orden de aparicion del numero, empezando en 1, como
//    level._exploder_ids de _utility.csc); h7e_light.cpp los pasa como sysState("@exploder", id). cannon_effect: borra los anteriores
//    y SpawnFx(fx, origen, delay, forward, up) + triggerfx; id negativo = deactivate_exploder (los borra).
//  - zombie_cod5_asylum_fx.csc:
//    trap_fx_monitor("auto246", "north") / ("auto242", "south"): con ese levelNotify FX "zapper" en cada script_struct de ese
//    targetname (SpawnFx con anglestoforward/anglestoup); "<lado>off" los borra.
//    zapper_switch_fx(lado): con "north"/"south" FX zapper_wall en zapper_switch_fx_<lado> y zapper_fx en zapper_fx_<lado>, 30 s.
//    electric_trap_wire_sparks(lado): con "north"/"south", cada 10..14 s recorre la cadena de trap_wire_sparks_<lado> (desde su target)
//    con electric_short_oneshot al 50 % cada 0,075 s, mientras no pasen 30 s (electric_trap_wire_sparks_stop).
//    perk_wire_fx: con "ZPO" (_zombiemode.csc ZPO_listener -> revive_on, middle_door_open, fast_reload_on, doubletap_on, jugger_on)
//    electric_short_oneshot en cada struct de revive_electric_wire, electric_middle_door, electric_fast_reload, electric_double_tap y
//    electric_juggernog (este no esta en el mapa: no hace nada, como en el original) cada 0,075 s.
//  - zombie_cod5_asylum.csc: zombify (zombie_grain cada 0,1 s) y swinging_lamp_init (no hace nada: todo comentado en el original).
//  - zombie_cod5_asylum_amb.csc: salas, reverb, contexto y musica ya los lleva h5_audio_cl (ParseAmb). Aqui: los paquetes de ambiente
//    (zombies, bathroom con amb_water_drips_rand, outside; disparadores "ambient_package" y ambientPackageCmd), start_lights (con
//    "start_lights": circuit_sound, light_sound + run_sparks_loop en electrical_surge y light_ceiling, buzz_sound, oneshot_sparks_loop,
//    play_elec_room_sweets, zmb_turn_on, player_ambience y la risa), play_random_generator_sparks (hasta "switch_flipped_generator") y
//    la radio de _waw_zombiemode_radio.csc (clientflag 14 del script_model "kzmb", ktr/rrd).
// No se hace: footsteps (setFootstepEffect) ni la lampara de la morgue (comentada en el original).
//   H19_VERRUCKT=0 lo apaga. Pruebas: H19_VERRUCKT_RADIO="ms,ms..." dispara "damage" en la radio en esos level.time (como dispararle).
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <database/db_registry.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_spawn_mp.h>
#include <server_mp/server_mp.h>
#include <clientscript/scr_const.h>
#include <clientscript/cscr_stringlist.h>
#include <EffectsCore/fx_system.h>
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
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "h19_mapcl.h"

bool H7_VisionSetNaked(const char *name, float seconds) __attribute__((weak));   // h7_gfx.cpp
void __cdecl SV_GetConfigstring(unsigned int index, char *buffer, int bufferSize);
void H4_GetCam(float *org, float *ang);                                         // world_h4.cpp
const FxEffectDef *__cdecl FX_Register(char *name);                             // h5_fx.cpp
extern FxSystemContainer *fx_systemPool;                                        // fx_system.cpp (lo reserva h5_fx)

namespace {
// ===================================================================================================== utilidades
// rawfile de script descomprimido (mismo formato que RawText de h5_audio_cl: [len][zlen][zlib])
std::string Raw(const char *name)
{
    RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)name, 0, -1).rawfile;
    if (!rf || !rf->buffer.get()) return {};
    const unsigned *hd = (const unsigned *)rf->buffer.get(); uLongf outlen = hd[0];
    std::vector<unsigned char> out(outlen + 1);
    if (uncompress(out.data(), &outlen, (const unsigned char *)(hd + 2), hd[1]) != Z_OK) return {};
    return std::string((const char *)out.data(), outlen);
}
std::string Lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
bool Quoted(const std::string &t, size_t p, std::string &out, size_t *end = nullptr)   // primera cadena entre comillas desde p
{
    size_t a = t.find('"', p); if (a == std::string::npos) return false;
    size_t b = t.find('"', a + 1); if (b == std::string::npos) return false;
    out = t.substr(a + 1, b - a - 1); if (end) *end = b + 1; return true;
}

struct Pt { float org[3], ang[3]; };
std::map<std::string, std::vector<Pt>> s_structs;   // script_struct por targetname (getstructarray)
std::map<std::string, std::string> s_target;        // targetname -> target del primero (getstruct(x.target))
Pt StructOf(const H19Kv &k) { Pt p; memset(&p, 0, sizeof p); H19_Vec(k, "origin", p.org); H19_Vec(k, "angles", p.ang); return p; }
void LoadStructs()
{
    if (!s_structs.empty()) return;
    for (const H19Kv &k : H19_MapEnts()) {
        const char *cn = H19_Str(k, "classname"), *tn = H19_Str(k, "targetname");
        if (!cn || !tn || strcmp(cn, "script_struct")) continue;
        s_structs[tn].push_back(StructOf(k));
        if (const char *t = H19_Str(k, "target")) if (!s_target.count(tn)) s_target[tn] = t;
    }
}
const std::vector<Pt> &S(const std::string &tn) { static const std::vector<Pt> none; auto it = s_structs.find(tn); return it == s_structs.end() ? none : it->second; }
std::vector<Pt> Chain(std::string n)   // targ = getstruct(n); while (isDefined(targ)) { ...; targ = getstruct(targ.target) }
{
    std::vector<Pt> out;
    for (int guard = 0; guard < 256 && !n.empty(); ++guard) {
        auto it = s_structs.find(n); if (it == s_structs.end() || it->second.empty()) break;
        out.push_back(it->second[0]);
        auto t = s_target.find(n); if (t == s_target.end()) break; n = t->second;
    }
    return out;
}

// sonidos que pide el cliente grafico (los reproduce el frame de sonido): playsound / playloopat
struct Snd { float org[3]; std::string alias; bool loop; };
std::mutex s_sndMu; std::vector<Snd> s_sndQ;
void QueueSound(const float *org, const char *alias, bool loop = false)
{
    Snd s; memcpy(s.org, org, 12); s.alias = alias; s.loop = loop;
    std::lock_guard<std::mutex> g(s_sndMu); s_sndQ.push_back(s);
}
const float kZero[3] = { 0, 0, 0 };

// ===================================================================================================== cliente grafico
std::map<std::string, std::string> s_effect;   // level._effect[clave] -> ruta de LoadFx (zombie_cod5_asylum_fx.csc)
void ParseEffects()
{
    const std::string t = Raw("clientscripts/zombie_cod5_asylum_fx.csc"), l = Lower(t);
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
// playfx / spawnfx+triggerfx de level._effect[clave]; sin direccion = angulos (270, 0, 0) como CScr_SetFxAngles sin ejes
unsigned Fx(const char *key, const float *org, const float *ang = nullptr)
{
    auto it = s_effect.find(key); if (it == s_effect.end()) return 0;
    static const float def[3] = { 270, 0, 0 };
    return H19_PlayFxAng(it->second.c_str(), org, ang ? ang : def);
}

// ---- tareas con espera (los wait/realwait de los hilos del CSC)
struct Task { int at; std::function<void(int)> fn; };
std::vector<Task> s_tasks;
void After(int at, std::function<void(int)> fn) { s_tasks.push_back({ at, std::move(fn) }); }
void RunTasks(int now)
{
    for (int pass = 0; pass < 4; ++pass) {   // una tarea puede encolar otra ya vencida
        std::vector<Task> due;
        for (size_t i = 0; i < s_tasks.size();) { if (now >= s_tasks[i].at) { due.push_back(std::move(s_tasks[i])); s_tasks.erase(s_tasks.begin() + i); } else ++i; }
        if (due.empty()) return;
        for (Task &t : due) t.fn(now);
    }
}
float RandRange(float a, float b) { return a + (b - a) * H19_Rand01(); }            // randomfloatrange
int RandInt(int a, int b) { int v = a + (int)(H19_Rand01() * (float)(b - a)); return v >= b ? b - 1 : v; }   // randomintrange: [a, b)

// ---- vision (configstring 1550)
std::string s_csVis;
void VisionPoll()
{
    if (!H7_VisionSetNaked || !H19_EnvOn("H7E_VISION")) return;
    char cs[1024]; cs[0] = 0; SV_GetConfigstring(1550, cs, sizeof cs);
    if (s_csVis == cs) return;
    s_csVis = cs;
    // MP: '"nombre" ms'; SP (g_sp_vision): cadena info con una clave por cliente ("0" = jugador local), sin comillas: 'nombre ms'
    std::string v = cs;
    if (!v.empty() && v[0] == '\\') { char *x = Info_ValueForKey(cs, "0"); v = x ? x : ""; }
    std::string name; size_t e = 0;
    if (!Quoted(v, 0, name, &e)) { size_t a = v.find_first_not_of(' '); if (a == std::string::npos) return;
        e = v.find(' ', a); name = v.substr(a, e == std::string::npos ? std::string::npos : e - a); if (e == std::string::npos) e = v.size(); }
    if (name.empty()) return;
    const int ms = atoi(v.c_str() + e);
    fprintf(stderr, "[H19] Verruckt: vision del servidor (CS 1550): '%s' en %d ms\n", name.c_str(), ms);
    H7_VisionSetNaked(name.c_str(), ms / 1000.0f);
}

// ---- createfx: oneshotfx y exploders
struct CfxEnt { float org[3], ang[3], delay; std::string fxid; int exploder; bool done; std::vector<unsigned> fx; };
std::vector<CfxEnt> s_cfx;
std::map<int, int> s_exploderId;   // level._exploder_ids[numero] = id (1, 2...)
int s_fxT0 = -1, s_fxPlayed, s_fxMissing, s_expOn;
void ParseCreateFx()
{
    const std::string t = Raw("clientscripts/createfx/zombie_cod5_asylum_fx.csc");
    size_t p = 0; CfxEnt cur{}; bool open = false;
    auto flush = [&]() {
        if (open && !cur.fxid.empty()) {
            if (cur.exploder >= 0 && !s_exploderId.count(cur.exploder)) { const int id = (int)s_exploderId.size() + 1; s_exploderId[cur.exploder] = id; }
            s_cfx.push_back(cur);
        }
        cur = CfxEnt{}; cur.exploder = -1; open = false;
    };
    cur.exploder = -1;
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
void CreateFxFrame()
{
#ifdef H5FX
    // h5_fx inicia el FxSystem en su primer frame: antes FX_SpawnEffect cae. t0 = primer instante con el reloj de FX en marcha
    if (!fx_systemPool || s_cfx.empty()) return;
    const int now = FX_GetSystem(0)->system.msecNow;
    if (s_fxT0 < 0) { if (now <= 0) return; s_fxT0 = now; fprintf(stderr, "[H19] Verruckt: createfx: reloj de FX en marcha, t0=%d\n", now); }
    for (CfxEnt &o : s_cfx) {
        if (o.done || o.exploder >= 0) continue;
        if (o.delay > 0 && now < s_fxT0 + (int)(o.delay * 1000.0f)) continue;   // oneshotfxthread: realWait(delay)
        o.done = true;
        auto it = s_effect.find(o.fxid);
        const FxEffectDef *fx = it == s_effect.end() ? nullptr : FX_Register((char *)it->second.c_str());
        if (!fx) { ++s_fxMissing; continue; }
        float f[3], r[3], u[3], ax[3][3]; AngleVectors(o.ang, f, r, u);   // set_forward_and_up_vectors
        memcpy(ax[0], f, 12); memcpy(ax[2], u, 12);
        ax[1][0] = u[1] * f[2] - u[2] * f[1]; ax[1][1] = u[2] * f[0] - u[0] * f[2]; ax[1][2] = u[0] * f[1] - u[1] * f[0];
        FX_PlayOrientedEffect(0, fx, s_fxT0 + (int)(o.delay * 1000.0f), o.org, ax);   // create_triggerfx: inicio = delay*1000 desde el arranque
        ++s_fxPlayed;
    }
    static bool logged;
    if (!logged) { bool all = true; for (const CfxEnt &o : s_cfx) all = all && (o.done || o.exploder >= 0);
        if (all) { logged = true; fprintf(stderr, "[H19] Verruckt: createfx: %d oneshotfx lanzados (%d sin FX en los datos)\n", s_fxPlayed, s_fxMissing); } }
#endif
}
void Exploder(int id, int now)   // callback_activate_exploder / callback_deactivate_exploder (_callbacks.csc -> _fx.csc)
{
    int num = -1; for (auto &e : s_exploderId) if (e.second == (id < 0 ? -id : id)) num = e.first;
    if (num < 0) { fprintf(stderr, "[H19] Verruckt: exploder id %d desconocido\n", id); return; }
    int n = 0;
    for (CfxEnt &o : s_cfx) {
        if (o.exploder != num) continue;
        for (unsigned &h : o.fx) H19_KillFx(h);   // cannon_effect / deactivate_exploder: borra los loopFX anteriores
        o.fx.clear();
        if (id < 0) continue;
        CfxEnt *po = &o;
        After(now + (o.delay > 0 ? (int)(o.delay * 1000.0f) : 0), [po](int) {   // exploder_delay
            float f[3], r[3], u[3]; AngleVectors(po->ang, f, r, u);
            auto it = s_effect.find(po->fxid); if (it == s_effect.end()) return;
            po->fx.push_back(H19_PlayFx(it->second.c_str(), po->org, f, u));
        });
        ++n;
    }
    s_expOn += id > 0 ? 1 : 0;
    fprintf(stderr, "[H19] Verruckt: exploder %d (id %d) %s: %d efectos t=%d\n", num, id, id > 0 ? "activado" : "desactivado", n, now);
}

// ---- trampas electricas (zombie_cod5_asylum_fx.csc)
struct Trap { const char *name, *side; std::vector<Pt> pts; std::vector<unsigned> fx; };
Trap s_traps[2] = { { "auto246", "north", {}, {} }, { "auto242", "south", {}, {} } };
struct Zap { const char *side; Pt sw, zp; bool haveSw, haveZp; unsigned fxSw = 0, fxZp = 0; int until = -1; int wireUntil = -1; bool wireBusy; std::vector<Pt> wire; };
Zap s_zap[2] = { { "north", {}, {}, false, false }, { "south", {}, {}, false, false } };
void TrapOn(Trap &t)   // electric_trap_fx: borra los anteriores y SpawnFx(zapper, origen, 0, forward, up) + triggerfx
{
    for (size_t i = 0; i < t.pts.size(); ++i) {
        H19_KillFx(t.fx[i]);
        float f[3], r[3], u[3]; AngleVectors(t.pts[i].ang, f, r, u);
        auto it = s_effect.find("zapper"); if (it != s_effect.end()) t.fx[i] = H19_PlayFx(it->second.c_str(), t.pts[i].org, f, u);
    }
    fprintf(stderr, "[H19] Verruckt: trampa %s (%s) encendida: %zu FX\n", t.side, t.name, t.pts.size());
}
void WireStep(int z, size_t i, int now)   // electric_trap_wire_sparks: una pasada por la cadena, al 50 % cada 0,075 s; luego 10..14 s
{
    Zap &q = s_zap[z];
    if (i < q.wire.size()) {
        if (RandInt(0, 100) > 50) Fx("electric_short_oneshot", q.wire[i].org);   // randomint(100) > 50
        After(now + 75, [z, i](int t) { WireStep(z, i + 1, t); });
        return;
    }
    After(now + RandInt(10, 15) * 1000, [z](int t) { Zap &w = s_zap[z]; if (t < w.wireUntil) WireStep(z, 0, t); else w.wireBusy = false; });   // while (isDefined(ent.fx))
}
void ZapSide(int z, int now)   // "north" / "south": zapper_switch_fx + electric_trap_wire_sparks
{
    Zap &q = s_zap[z];
    if (q.until < 0) {   // zapper_switch_fx: el hilo esta en realwait(30) y no oye otro aviso hasta que acaba
        if (q.haveSw) q.fxSw = Fx("zapper_wall", q.sw.org, q.sw.ang);
        if (q.haveZp) q.fxZp = Fx("zapper_fx", q.zp.org, q.zp.ang);
        q.until = now + 30000;
        After(q.until, [z](int) { Zap &w = s_zap[z]; H19_KillFx(w.fxSw); H19_KillFx(w.fxZp); w.until = -1; });
    }
    if (!q.wireBusy && !q.wire.empty()) { q.wireBusy = true; q.wireUntil = now + 30000; WireStep(z, 0, now); }   // ent.fx = 1; ..._stop: realwait(30)
    fprintf(stderr, "[H19] Verruckt: interruptor de la trampa %s: FX de pared y caja, chispas del cable (%zu tramos) t=%d\n", q.side, q.wire.size(), now);
}

// ---- perk_wire_fx (con "ZPO")
const char *const kPerkWires[5] = { "revive_electric_wire", "electric_middle_door", "electric_fast_reload", "electric_double_tap", "electric_juggernog" };
bool s_power;
void PerkWire(std::shared_ptr<std::vector<Pt>> path, size_t i, int now)
{
    if (i >= path->size()) return;
    Fx("electric_short_oneshot", (*path)[i].org);
    After(now + 75, [path, i](int t) { PerkWire(path, i + 1, t); });
}

// ---- start_lights (zombie_cod5_asylum_amb.csc): el FX va aqui y el sonido se encola al frame de sonido
bool s_lightsStarted; int s_sparkLoops;
void SparksLoop(Pt p, int now)   // run_sparks_loop
{
    After(now + (int)(RandRange(4, 15) * 1000.0f), [p](int t) {
        if (RandRange(0, 1) < 0.5f) { Fx("electric_short_oneshot", p.org); QueueSound(p.org, "evt_electrical_surge"); }
        After(t + RandInt(1, 4) * 1000, [p](int t2) { SparksLoop(p, t2); });
    });
}
void LightSound(Pt p, int now)   // light_sound
{
    After(now + (int)(RandRange(1, 4) * 1000.0f), [p](int t) {
        QueueSound(p.org, "evt_electrical_surge"); Fx("electric_short_oneshot", p.org);
        After(t + (int)(RandRange(1, 2) * 1000.0f), [p](int t2) { QueueSound(p.org, "light", true); ++s_sparkLoops; SparksLoop(p, t2); });
    });
}
void OneshotSparks(Pt p, int now)   // play_oneshot_sparks_loop
{
    After(now + (int)(RandRange(0.25f, 0.5f) * 1000.0f), [p](int t) { Fx("wire_sparks_oneshot", p.org); QueueSound(p.org, "arc_spark_light"); OneshotSparks(p, t); });
}
void ElecRoomSweets(int now)   // play_elec_room_sweets
{
    static const float o[3] = { -584, -392, 344 };
    After(now + (int)(RandRange(0.25f, 1) * 1000.0f), [](int t) { QueueSound(o, "elec_room_sweets"); ElecRoomSweets(t); });
}
void StartLights(int now)
{
    if (s_lightsStarted) return;
    s_lightsStarted = true;
    fprintf(stderr, "[H19] Verruckt: start_lights t=%d (circuitos %zu, chispazos %zu+%zu, zumbidos %zu, chispas %zu)\n", now, S("electrical_circuit").size(),
        S("electrical_surge").size(), S("light_ceiling").size(), S("low_buzz").size(), S("oneshot_sparks_loop").size());
    After(now + 2000, [](int t) {   // wait(2.0)
        for (const Pt &p : S("electrical_circuit")) After(t + 1000, [p](int) { QueueSound(p.org, "zmb_circuit"); });   // circuit_sound
        After(t + 3000, [](int t2) {   // wait(3.0)
            for (const Pt &p : S("electrical_surge")) LightSound(p, t2);
            for (const Pt &p : S("low_buzz")) QueueSound(p.org, "low_arc", true);   // buzz_sound
            for (const Pt &p : S("oneshot_sparks_loop")) OneshotSparks(p, t2);
            for (const Pt &p : S("light_ceiling")) LightSound(p, t2);
            ElecRoomSweets(t2);
            After(t2 + 7000, [](int t3) {   // wait(7)
                QueueSound(kZero, "zmb_turn_on");
                QueueSound(kZero, "player_ambience", true);
                After(t3 + 2000, [](int) { static const float o[3] = { -16, 184, 952 }; QueueSound(o, "zmb_ann_vox_laugh_l"); });
            });
        });
    });
}

// ---- zombify
bool s_zombify; int s_grainAt;

// avisos del servidor (gancho de comandos de h7e) -> frame
std::mutex s_evMu; std::vector<std::pair<std::string, std::string>> s_evQ;   // (sistema, estado); sistema "" = levelNotify

void Init(const std::vector<H19Kv> &)
{
    LoadStructs();
    VisionPoll();
    ParseEffects(); ParseCreateFx();
    for (Trap &t : s_traps) { t.pts = S(t.name); t.fx.assign(t.pts.size(), 0); }
    for (Zap &z : s_zap) {
        const std::string sd = z.side;
        if (!S("zapper_switch_fx_" + sd).empty()) { z.sw = S("zapper_switch_fx_" + sd)[0]; z.haveSw = true; }
        if (!S("zapper_fx_" + sd).empty()) { z.zp = S("zapper_fx_" + sd)[0]; z.haveZp = true; }
        auto t = s_target.find("trap_wire_sparks_" + sd);   // targ = getstruct(ent.target)
        if (t != s_target.end()) z.wire = Chain(t->second);
    }
    size_t nexp = 0; for (const CfxEnt &o : s_cfx) nexp += o.exploder >= 0;
    fprintf(stderr, "[H19] Verruckt: %zu level._effect, createfx %zu (%zu exploders en %zu numeros), trampas %zu/%zu puntos, cables de trampa %zu/%zu tramos\n",
        s_effect.size(), s_cfx.size(), nexp, s_exploderId.size(), s_traps[0].pts.size(), s_traps[1].pts.size(), s_zap[0].wire.size(), s_zap[1].wire.size());
}
void Notify(const std::string &ev, int now)
{
    if (ev == "ZPO" && !s_power) {   // ZPO_listener -> revive_on, middle_door_open, fast_reload_on, doubletap_on, jugger_on
        s_power = true; size_t n = 0;
        for (const char *w : kPerkWires) { auto p = std::make_shared<std::vector<Pt>>(Chain(w)); n += p->size(); PerkWire(p, 0, now); }
        fprintf(stderr, "[H19] Verruckt: corriente (ZPO): chispas por los cables de ventajas y puerta (%zu puntos) t=%d\n", n, now);
    }
    if (ev == "start_lights") StartLights(now);
    for (Trap &t : s_traps) {
        if (ev == t.name) TrapOn(t);
        if (ev == std::string(t.side) + "off") { for (auto &h : t.fx) H19_KillFx(h); fprintf(stderr, "[H19] Verruckt: trampa %s apagada t=%d\n", t.side, now); }
    }
    for (int z = 0; z < 2; ++z) if (ev == s_zap[z].side) ZapSide(z, now);
}
bool LevelNotify(const std::string &ev, int)
{
    std::lock_guard<std::mutex> g(s_evMu); s_evQ.push_back({ "", ev });
    return false;   // sigue a los mezcladores genericos
}
bool SysState(const char *sys, const std::string &st, int)
{
    if (strcmp(sys, "zombify") && strcmp(sys, "@exploder")) return false;
    std::lock_guard<std::mutex> g(s_evMu); s_evQ.push_back({ sys, st });
    return true;
}
void Frame(int now)
{
    VisionPoll();
    CreateFxFrame();
    std::vector<std::pair<std::string, std::string>> q; { std::lock_guard<std::mutex> g(s_evMu); q.swap(s_evQ); }
    std::string last;
    for (auto &e : q) {
        if (e.first + "=" + e.second == last) continue;   // el gancho de h7e puede ir encadenado dos veces: mismo comando repetido
        last = e.first + "=" + e.second;
        if (e.first.empty()) Notify(e.second, now);
        else if (e.first == "@exploder") Exploder(atoi(e.second.c_str()), now);
        else if (e.first == "zombify") {
            const bool on = e.second == "1";
            if (on != s_zombify) fprintf(stderr, "[H19] Verruckt: zombify %s t=%d\n", on ? "on" : "off", now);
            if (e.second == "1" || e.second == "0") s_zombify = on;
            s_grainAt = now;
        }
    }
    RunTasks(now);
    if (s_zombify && now >= s_grainAt) { s_grainAt = now + 100; Fx("zombie_grain", g_entities[0].r.currentOrigin); }   // zombie_monitor
}

// ===================================================================================================== sonido (zombie_cod5_asylum_amb.csc)
#ifdef H6SND
}   // namespace
int __cdecl CG_PlaySoundWithHandle(int localClientNum, int entitynum, float *origin, int fadeMs, bool doNotify, float volume, unsigned int id);   // h5_audio_cl.cpp
namespace {
unsigned s_rng = 0x76657272u;   // secuencia propia del hilo de sonido (no gasta la de fire_flicker)
float RandF(float a, float b) { s_rng = s_rng * 1664525u + 1013904223u; return a + (b - a) * ((s_rng >> 8) / 16777216.0f); }
int RandI(int a, int b) { if (b <= a) return a; const int v = a + (int)RandF(0, (float)(b - a)); return v < b ? v : b - 1; }   // randomintrange: [a, b)
int Play(const float *org, const char *alias)
{
    float p[3]; if (org) { p[0] = org[0]; p[1] = org[1]; p[2] = org[2]; }
    return CG_PlaySoundWithHandle(0, 1022, org ? p : nullptr, 0, 0, 1.0f, SND_FindAliasId(alias));
}
bool Playing(int id, double start, double now)   // soundplaying(id); con duracion conocida, esa; tope de 20 s por si el dispositivo no avisa
{
    if (id < 0) return false;
    int len = 0; const bool known = SND_GetKnownLength(id, &len) && len > 0;
    return (known ? now < start + len : SND_IsPlaying(id)) && now < start + 20000.0;
}

// ---- paquetes de ambiente (_ambientpackage.csc)
struct Elem { const char *alias; float spawnMin, spawnMax; int distMin, distMax; };
struct Package { const char *name; std::vector<Elem> el; std::map<int, int> refcount; };
std::vector<Package> s_pk = {   // main(): declareAmbientPackage + addAmbientElement (angulos por defecto 0..359)
    { "zombies", { { "amb_spooky_2d", 5, 8, 300, 2000 } }, {} },
    { "bathroom", { { "amb_spooky_2d", 5, 8, 300, 2000 }, { "amb_water_drips_rand", 0.2f, 0.5f, 50, 350 } }, {} },
    { "outside", { { "amb_spooky_2d", 5, 8, 300, 2000 } }, {} },
};
int s_active = -1;                                       // level.activeAmbientPackage
struct Thread { double at; int pool; };                  // ambientElementThread: proximo disparo y entidad del pool que espera
std::vector<Thread> s_th;
struct PoolEnt { bool inuse; int id; double start; };    // level.ambientPackageScriptOriginPool (5)
PoolEnt s_pool[5];
int s_ambPlays, s_ambMissed;
struct PkTrig { unsigned model; float org[3]; int pk, prio; bool inside; };
std::vector<PkTrig> s_trigs;
int PkIndex(const std::string &n) { for (size_t i = 0; i < s_pk.size(); ++i) if (n == s_pk[i].name) return (int)i; return -1; }
void PkUpdate(double now)   // updateActiveAmbientPackage: la de prioridad mas alta con cuenta
{
    int best = -1, bp = -1;
    for (size_t i = 0; i < s_pk.size(); ++i) for (auto &r : s_pk[i].refcount) if (r.second && r.first > bp) { bp = r.first; best = (int)i; }
    if (best < 0 || best == s_active) return;
    s_active = best;   // killambientElementThread<antigua>: los hilos viejos mueren (un sonido en curso sigue y su entidad se libera sola)
    s_th.assign(s_pk[best].el.size(), Thread{ -1, -1 });
    for (size_t i = 0; i < s_th.size(); ++i) s_th[i].at = now + RandF(s_pk[best].el[i].spawnMin, s_pk[best].el[i].spawnMax) * 1000.0;
    fprintf(stderr, "[H19] Verruckt: paquete de ambiente '%s' (prioridad %d)\n", s_pk[best].name, bp);
}
void PkActivate(const std::string &n, int prio, bool on, double now)
{
    const int i = PkIndex(n); if (i < 0) return;
    int &rc = s_pk[i].refcount[prio]; if (on) ++rc; else if (rc > 0) --rc;
    PkUpdate(now);
}
void LoadPkTrigs()   // init_ambient_package_triggers: disparadores "ambient_package" con script_ambientpackage declarado
{
    for (const H19Kv &k : H19_MapEnts()) {
        const char *tn = H19_Str(k, "targetname"), *pk = H19_Str(k, "script_ambientpackage"), *m = H19_Str(k, "model");
        if (!tn || strcmp(tn, "ambient_package") || !pk || !m || m[0] != '*') continue;
        PkTrig t; t.pk = PkIndex(pk); t.model = (unsigned)atoi(m + 1); t.inside = false;
        t.prio = H19_Str(k, "script_ambientpriority") ? atoi(H19_Str(k, "script_ambientpriority")) : 1;
        memset(t.org, 0, 12); H19_Vec(k, "origin", t.org);
        if (t.pk < 0 || !t.model || t.model >= cm.numSubModels) continue;
        s_trigs.push_back(t);
    }
}
bool Touching(const PkTrig &t, const float *org, const float *mins, const float *maxs)   // istouching: caja del jugador contra las brochas
{
    trace_t tr; memset(&tr, 0, sizeof tr); const float zero[3] = { 0, 0, 0 };
    CM_TransformedBoxTrace(&tr, org, org, mins, maxs, t.model, -1, t.org, zero);
    return tr.startsolid || tr.allsolid;
}
void AmbientFrame(double now)
{
    if (gentity_s *pl = H19A_PlayerEnt()) for (PkTrig &t : s_trigs) {   // trigger_thread: trig_enter / trig_leave
        const bool in = Touching(t, pl->r.currentOrigin, pl->r.mins, pl->r.maxs); if (in == t.inside) continue;
        t.inside = in; PkActivate(s_pk[t.pk].name, t.prio, in, now);
    }
    for (PoolEnt &p : s_pool) if (p.inuse && !Playing(p.id, p.start, now)) { p.inuse = false; p.id = -1; }   // scriptOriginPoolThread
    if (s_active < 0) return;
    for (size_t i = 0; i < s_th.size(); ++i) {
        Thread &th = s_th[i]; const Elem &e = s_pk[s_active].el[i];
        if (th.pool >= 0) { if (s_pool[th.pool].inuse) continue; th.pool = -1; th.at = now + RandF(e.spawnMin, e.spawnMax) * 1000.0; continue; }
        if (now < th.at) continue;
        int idx = -1; for (int k = 0; k < 5; ++k) if (!s_pool[k].inuse) { idx = k; break; }   // getScriptOriginPoolIndex
        if (idx < 0) { ++s_ambMissed; th.at = now + RandF(e.spawnMin, e.spawnMax) * 1000.0; continue; }
        float eye[3], ang[3]; H4_GetCam(eye, ang);   // getlocalclienteyepos / getlocalclientangles
        const float dist = (float)RandI(e.distMin, e.distMax), a = (float)RandI(0, 359) + ang[1];
        float f[3], yaw[3] = { 0, a, 0 }; AngleVectors(yaw, f, nullptr, nullptr);
        const float pos[3] = { eye[0] + f[0] * dist, eye[1] + f[1] * dist, eye[2] + f[2] * dist };
        s_pool[idx].inuse = true; s_pool[idx].id = Play(pos, e.alias); s_pool[idx].start = now; ++s_ambPlays;
        th.pool = idx;   // while (soundId != -1) wait
    }
}

// ---- radio (_waw_zombiemode_radio.csc; level.kzmb_name por defecto "kzmb")
const char *const kSongs[] = { "wtf", "abra_macabre", "uhf", "dusk", "underwater", "slasher_flick", "maskwalk", "sand", "temple", "" };
int s_radioEnt = -2, s_radioIdx, s_radioId = -1, s_radioPlays; bool s_radioFlag, s_stopRadio; double s_songAt = -1, s_stopAt = -1; int s_stopId = -1;
void FindRadio()
{
    s_radioEnt = -1;
    for (int e = 0; e < level.num_entities; ++e) { gentity_s *ent = &g_entities[e];
        if (ent->r.inuse && ent->targetname && !strcmp(SL_ConvertToString(ent->targetname, SCRIPTINSTANCE_SERVER), "kzmb")) { s_radioEnt = e; break; } }
    fprintf(stderr, "[H19] Verruckt: radio kzmb: %s\n", s_radioEnt >= 0 ? "encontrada" : "NO esta");
}
void NextSong(double now, const float *org)   // next_song(set = 1)
{
    Play(org, "static");
    if (s_radioId >= 0 && SND_IsPlaying(s_radioId)) {   // fade(level.radio_id, 1): wait(1) y stopsound; luego la nueva
        SND_SetPlaybackAttenuationRate(s_radioId, 1.0f); SND_SetPlaybackAttenuation(s_radioId, 0.0f);
        s_stopId = s_radioId; s_stopAt = now + 1000.0; s_songAt = now + 1000.0;
    } else s_songAt = now + 500.0;   // wait(.5)
}
void RadioFrame(double now)
{
    if (s_radioEnt == -2) FindRadio();
    if (s_radioEnt < 0) return;
    gentity_s *ent = &g_entities[s_radioEnt];
    if (const char *e = getenv("H19_VERRUCKT_RADIO")) {   // prueba: el jugador le dispara (zombie_radio_play: waittill("damage"))
        static std::vector<int> q; static bool parsed;
        if (!parsed) { parsed = true; for (const char *p = e; *p; ) { q.push_back(atoi(p)); const char *c = strchr(p, ','); if (!c) break; p = c + 1; } }
        for (int &t : q) if (t >= 0 && level.time >= t) { t = -1; Scr_Notify(ent, scr_const.damage, 0); fprintf(stderr, "[H19] H19_VERRUCKT_RADIO: damage en la radio t=%d\n", level.time); }
    }
    const bool f = (ent->s.lerp.eFlags2 >> 14) & 1;   // clientflag 14 (SetClientFlag del scriptmover)
    if (f && !s_radioFlag && !s_stopRadio) NextSong(now, ent->r.currentOrigin);
    s_radioFlag = f;
    if (s_stopAt >= 0 && now >= s_stopAt) { SND_StopPlayback(s_stopId); s_stopAt = -1; s_stopId = -1; }
    if (s_songAt >= 0 && now >= s_songAt) {
        s_songAt = -1;
        const char *song = kSongs[s_radioIdx];
        s_radioId = *song ? Play(ent->r.currentOrigin, song) : -1; ++s_radioPlays;
        fprintf(stderr, "[H19] Verruckt: radio: cancion %d '%s' (id %d) t=%d\n", s_radioIdx, song, s_radioId, level.time);
        s_radioIdx = (s_radioIdx + 1) % (int)(sizeof kSongs / sizeof *kSongs);
    }
}

// ---- play_random_generator_sparks (hasta "switch_flipped_generator") y sonidos encolados por el cliente grafico
bool s_genOn = true; double s_genAt = -1; int s_genPlays, s_loops, s_shots;
void AudioStart()
{
    for (PoolEnt &p : s_pool) { p.inuse = false; p.id = -1; p.start = 0; }
    LoadPkTrigs();
    PkActivate("zombies", 0, true, level.time);   // activateAmbientPackage(0, "zombies", 0)
    fprintf(stderr, "[H19] Verruckt sonido: %zu paquetes, %zu disparadores de paquete, chispas del generador\n", s_pk.size(), s_trigs.size());
}
void AudioNotify(const std::string &n)
{
    if (n == "switch_flipped_generator" && s_genOn) { s_genOn = false; fprintf(stderr, "[H19] Verruckt: chispas del generador paradas (%d)\n", s_genPlays); }
    else if (n == "ktr") { s_stopRadio = true; if (s_radioId >= 0) SND_StopPlayback(s_radioId); s_songAt = -1; fprintf(stderr, "[H19] Verruckt: radio: ktr (para)\n"); }
    else if (n == "rrd") { s_stopRadio = false; fprintf(stderr, "[H19] Verruckt: radio: rrd (sigue)\n"); }
}
void AudioSys(const std::string &sys, const std::string &st)   // ambientPackageCmd "A|D paquete prioridad" (ambientPackageCmdHandler)
{
    if (sys != "ambientPackageCmd" || st.empty()) return;
    char c[8], pk[64]; int pr = 0;
    if (sscanf(st.c_str(), "%7s %63s %d", c, pk, &pr) != 3) return;
    PkActivate(pk, pr, c[0] == 'A', level.time);
}
void AudioFrame(bool)
{
    const double now = level.time;   // wait()/realwait() del CSC: tiempo de juego
    { std::vector<Snd> q; { std::lock_guard<std::mutex> g(s_sndMu); q.swap(s_sndQ); }
      for (const Snd &s : q) {
          if (s.loop) { const int f = H19A_FakeAt(s.org); if (f) { H19A_PlayLoop(f, s.alias.c_str(), 0); ++s_loops; } }   // _audio::playloopat
          else { Play(s.org, s.alias.c_str()); ++s_shots; }
      } }
    if (s_genOn) {   // wait(randomfloatrange(0.45, 0.85)); playsound(0, "elec_arc_generator", (-672, -264, 296))
        static const float o[3] = { -672, -264, 296 };
        if (s_genAt < 0) s_genAt = now + RandF(0.45f, 0.85f) * 1000.0;
        else if (now >= s_genAt) { Play(o, "elec_arc_generator"); ++s_genPlays; s_genAt = now + RandF(0.45f, 0.85f) * 1000.0; }
    }
    AmbientFrame(now);
    RadioFrame(now);
}
void AudioStatus(std::string &out)
{
    char b[240]; snprintf(b, sizeof b, "; Verruckt: paquete %s, elementos %d (sin entidad %d), generador %d%s, bucles %d, sonidos %d, radio %d/%s",
        s_active >= 0 ? s_pk[s_active].name : "-", s_ambPlays, s_ambMissed, s_genPlays, s_genOn ? "" : " (parado)", s_loops, s_shots,
        s_radioPlays, s_stopRadio ? "parada" : "lista");
    out += b;
}
#endif
} // namespace

extern const H19MapCl kH19_zombie_cod5_asylum = {
    .zone = "zombie_cod5_asylum", .offEnv = "H19_VERRUCKT",
    .estado = "Verruckt: vision CS1550, createfx y exploder de la corriente, trampas electricas, cables, start_lights, paquetes de ambiente y radio (OLA M6)",
    .init = Init, .levelNotify = LevelNotify, .sysState = SysState, .frame = Frame, .clockHand = nullptr, .screensFrame = nullptr,
    .levelEffect = nullptr,
#ifdef H6SND
    .audioStart = AudioStart, .audioNotify = AudioNotify, .audioSys = AudioSys, .audioFrame = AudioFrame, .audioStatus = AudioStatus,
#endif
    .doorEnt = nullptr,
};
#endif
