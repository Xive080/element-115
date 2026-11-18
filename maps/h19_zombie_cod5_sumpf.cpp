// OLA M5: cliente de zombie_cod5_sumpf (Shi No Numa). Reproduce sin VM de CSC lo que hacen sus clientscripts (rawfiles del ff), con sus
// mismos numeros. El servidor es el GSC original sin tocar: chozas con ventaja al azar (zombie_cod5_sumpf_perks.gsc), caja que se mueve
// (zombie_cod5_sumpf_magic_box.gsc), 4 trampas electricas (zombie_cod5_sumpf_trap_perk_electric.gsc), trampa del tronco
// (zombie_cod5_sumpf_trap_pendulum.gsc), tirolina (zombie_cod5_sumpf_zipline.gsc), perros (enable_dog_rounds) y huevos de radio.
// Shi No Numa NO tiene corriente ni Pack-a-Punch (level.has_pack_a_punch = false) ni monos (el mono de platillos es solo un arma).
//  - vision: zombie_cod5_sumpf.gsc y createart/zombie_cod5_sumpf_art.gsc hacen VisionSetNaked("zombie_sumpf", 0); el servidor lo deja en
//    el configstring 1550 y el cliente original lo lee (CG_VisionSetConfigString_Naked). El harness solo carga vision/<mapname>.vision
//    (no existe), asi que se lee el CS 1550 y se aplica con H7_VisionSetNaked (mismo metodo que Nacht, OLA M3).
//  - zombie_cod5_sumpf_fx.csc: level_fog_init (setVolFog, tiempo 0) y dog_start_monitor / dog_stop_monitor (7 s; h7c_gfx.cpp);
//    trap_fx_monitor(nombre, lado) de las 4 trampas: con "<zona>_tgt" FX "zapper" (misc/fx_zombie_electric_trap) en cada script_struct
//    de ese targetname con SpawnFx(origen, anglestoforward, anglestoup) + triggerfx; "<zona>_elec_lightoff" los borra.
//    (zapper_switch_fx no se lanza en main(): no se reproduce.)
//  - createfx (clientscripts/createfx/zombie_cod5_sumpf_fx.csc + _fx.csc fx_init): 303 oneshotfx y 6 loopfx (niebla baja del pantano,
//    luciernagas, insectos, libelulas, hojas, antorchas, murcielagos, rayos de luz y meteorito). Se leen del rawfile, no se copian:
//    oneshotfxthread (si delay > 0 espera delay s) + create_triggerfx (playFx con 6 parametros: inicio = delay*1000 desde el arranque,
//    asi delay -15 = el efecto ya llevaba 15 s); loopfx: primer playFx con ese inicio, realWait(delay), espera a que acabe
//    (isfxplaying) y lo relanza. level._effect[fxid] sale de zombie_cod5_sumpf_fx.csc (LoadFx).
//  - zombie_cod5_sumpf_amb.csc: salas, reverb, contexto y musica ya los lleva h5_audio_cl (ParseAmb). Aqui: los paquetes
//    (declareAmbientPackage/addAmbientElement de _ambientpackage.csc, con sus disparadores "ambient_package" de script_ambientpackage,
//    activateAmbientPackage(0, "zombies", 0) y el sistema ambientPackageCmd A/D): cada elemento es un hilo que espera
//    randomfloatrange(spawnMin, spawnMax), toma una de las 5 entidades del pool y suena a randomintrange(distMin, distMax) en un angulo
//    al azar desde el ojo del jugador, y no repite hasta que acaba (scriptOriginPoolThread); play_meteor_loop (meteor_loop en
//    (11264, -1920, -592)) y la radio de _waw_zombiemode_radio.csc (clientflag 14 del script_model "sumpf_kzmb", ktr/rrd).
//    start_lights no se llama en main(): no se reproduce.
// No se hace: zombify (el GSC de este mapa no lo usa), swamp_german_safe (solo version alemana) ni footsteps (setFootstepEffect).
//   H19_SUMPF=0 lo apaga. Pruebas: H19_SUMPF_RADIO="ms,ms..." dispara "damage" en la radio en esos level.time (como dispararle).
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
#include <map>
#include <string>
#include <vector>
#include "h19_mapcl.h"

bool H7_VisionSetNaked(const char *name, float seconds) __attribute__((weak));   // h7_gfx.cpp
void H7C_ClientVolFog(const float *v, int transitionMs) __attribute__((weak));     // h7c_gfx.cpp: setVolFog del CSC
void __cdecl SV_GetConfigstring(unsigned int index, char *buffer, int bufferSize);
void H4_GetCam(float *org, float *ang);                                         // world_h4.cpp
const FxEffectDef *__cdecl FX_Register(char *name);                             // h5_fx.cpp
extern FxSystemContainer *fx_systemPool;                                        // fx_system.cpp (lo reserva h5_fx)

namespace {
// rawfile de script descomprimido ([len][zlen][zlib], como RawText de h5_audio_cl)
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
void LoadStructs()
{
    if (!s_structs.empty()) return;
    for (const H19Kv &k : H19_MapEnts()) {
        const char *cn = H19_Str(k, "classname"), *tn = H19_Str(k, "targetname");
        if (!cn || !tn || strcmp(cn, "script_struct")) continue;
        Pt p; memset(&p, 0, sizeof p); H19_Vec(k, "origin", p.org); H19_Vec(k, "angles", p.ang); s_structs[tn].push_back(p);
    }
}

// ===================================================================================================== cliente grafico
// ---- vision (configstring 1550)
std::string s_csVis;
void VisionPoll()
{
    if (!H7_VisionSetNaked || !H19_EnvOn("H7E_VISION")) return;
    char cs[1024]; cs[0] = 0; SV_GetConfigstring(1550, cs, sizeof cs);
    if (s_csVis == cs) return;
    s_csVis = cs;
    // MP: '"nombre" ms'; SP (g_sp_vision): cadena info con una clave por cliente ("0" = jugador local), sin comillas
    std::string v = cs;
    if (!v.empty() && v[0] == '\\') { char *x = Info_ValueForKey(cs, "0"); v = x ? x : ""; }
    std::string name; size_t e = 0;
    if (!Quoted(v, 0, name, &e)) { size_t a = v.find_first_not_of(' '); if (a == std::string::npos) return;
        e = v.find(' ', a); name = v.substr(a, e == std::string::npos ? std::string::npos : e - a); if (e == std::string::npos) e = v.size(); }
    if (name.empty()) return;
    const int ms = atoi(v.c_str() + e);
    fprintf(stderr, "[H19] Shi No Numa: vision del servidor (CS 1550): '%s' en %d ms\n", name.c_str(), ms);
    H7_VisionSetNaked(name.c_str(), ms / 1000.0f);
}

// ---- niebla (level_fog_init / dog_start_monitor / dog_stop_monitor)
void Fog(int which, float sec)   // 0 = level_fog_init, 1 = dog_start_monitor, 2 = dog_stop_monitor
{
    if (!H7C_ClientVolFog) return;
    // setVolFog(start, halfwayDist, halfwayHeight, baseHeight, r, g, b, colorScale, sunR, sunG, sunB, dirX, dirY, dirZ, startAng, endAng, time, maxOpacity)
    static const float lv[18] = { 452.915f, 161.557f, 426.143f, -675.164f, 0.419608f, 0.564706f, 0.513726f, 1, 0.827451f, 0.839216f, 0.827451f,
                                  -0.114925f, -0.322134f, 0.939693f, 0, 0, 0, 1 };
    static const float dg[18] = { 1622.47f, 2126.6f, 288.118f, 399.766f, 0.478431f, 0.623529f, 0.572549f, 1.25f, 0.827451f, 0.839216f, 0.827451f,
                                  -0.114925f, -0.322134f, 0.939693f, 0, 0, 7, 1 };
    static const float st[18] = { 452.915f, 161.557f, 426.143f, -675.164f, 0.419608f, 0.564706f, 0.513726f, 1, 0.827451f, 0.839216f, 0.827451f,
                                  -0.114925f, -0.322134f, 0.939693f, 0, 0, 7, 1 };
    H7C_ClientVolFog(which == 1 ? dg : which == 2 ? st : lv, (int)(sec * 1000.0f));
    fprintf(stderr, "[H19] Shi No Numa: niebla %s (%.0f s)\n", which == 1 ? "de perros (dog_start)" : which == 2 ? "normal (dog_stop)" : "inicial", sec);
}

// ---- createfx
struct CFx { float org[3], ang[3], delay; std::string fxid; bool loop; int state; unsigned h; int waitUntil; };   // state: 0 sin lanzar, 1 en marcha, 2 acabado
std::vector<CFx> s_fx;
std::map<std::string, std::string> s_effect;   // level._effect[fxid] -> ruta de LoadFx
int s_fxT0 = -1, s_fxPlayed, s_fxMissing, s_fxLoops;
void ParseEffects()
{
    const std::string t = Raw("clientscripts/zombie_cod5_sumpf_fx.csc"), l = Lower(t);
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
void ParseCreateFx()
{
    const std::string t = Raw("clientscripts/createfx/zombie_cod5_sumpf_fx.csc");
    size_t p = 0; CFx cur{}; bool open = false;
    auto flush = [&]() { if (open && !cur.fxid.empty()) s_fx.push_back(cur); cur = CFx{}; open = false; };
    while (p < t.size()) {
        size_t q = t.find('\n', p); if (q == std::string::npos) q = t.size();
        const std::string ln = t.substr(p, q - p); p = q + 1;
        if (ln.find("createOneshotEffect") != std::string::npos) { flush(); open = true; cur.delay = -15; cur.loop = false; continue; }   // _fx.csc
        if (ln.find("createLoopEffect") != std::string::npos) { flush(); open = true; cur.delay = 0.5f; cur.loop = true; continue; }
        size_t k = ln.find("ent.v[");
        if (!open || k == std::string::npos) continue;
        std::string key, val; size_t e = 0; if (!Quoted(ln, k, key, &e)) continue;
        size_t eq = ln.find('=', e); if (eq == std::string::npos) continue;
        const char *r = ln.c_str() + eq + 1;
        if (key == "origin" || key == "angles") { float *d = key == "origin" ? cur.org : cur.ang; sscanf(r, " ( %f , %f , %f )", &d[0], &d[1], &d[2]); }
        else if (key == "delay") cur.delay = (float)atof(r);
        else if (key == "fxid") { if (Quoted(ln, eq, val)) cur.fxid = val; }
        else if (key == "type") { if (Quoted(ln, eq, val) && val != "oneshotfx" && val != "loopfx") open = false; }
    }
    flush();
}
#ifdef H5FX
unsigned PlayCfx(const CFx &o, const FxEffectDef *fx, int startMsec)
{
    float f[3], r[3], u[3], ax[3][3]; AngleVectors(o.ang, f, r, u);   // set_forward_and_up_vectors + CScr_SetFxAngles(2): eje izquierdo = up x fwd
    memcpy(ax[0], f, 12); memcpy(ax[2], u, 12);
    ax[1][0] = u[1] * f[2] - u[2] * f[1]; ax[1][1] = u[2] * f[0] - u[0] * f[2]; ax[1][2] = u[0] * f[1] - u[1] * f[0];
    return FX_PlayOrientedEffect(0, fx, startMsec, o.org, ax);
}
#endif
void CreateFxFrame()
{
#ifdef H5FX
    // h5_fx inicia el FxSystem en su primer frame: hasta que su reloj corre no se lanza nada; t0 = ese instante (fx_init al empezar el nivel)
    if (!fx_systemPool || s_fx.empty()) return;
    const int now = FX_GetSystem(0)->system.msecNow;
    if (s_fxT0 < 0) { if (now <= 0) return; s_fxT0 = now; fprintf(stderr, "[H19] Shi No Numa: createfx: reloj de FX en marcha, t0=%d\n", now); }
    for (CFx &o : s_fx) {
        if (o.state == 2) continue;
        auto it = s_effect.find(o.fxid);
        const FxEffectDef *fx = it == s_effect.end() ? nullptr : FX_Register((char *)it->second.c_str());
        if (o.state == 0) {
            if (!o.loop && o.delay > 0 && now < s_fxT0 + (int)(o.delay * 1000.0f)) continue;   // oneshotfxthread: realWait(delay)
            if (!fx) { o.state = 2; ++s_fxMissing; continue; }
            o.h = PlayCfx(o, fx, s_fxT0 + (int)(o.delay * 1000.0f));   // create_triggerfx / loopfx: playFx(..., delay)
            ++s_fxPlayed;
            if (!o.loop) { o.state = 2; continue; }
            o.state = 1; o.waitUntil = now + (int)(o.delay * 1000.0f);   // loopfx: realWait(delay)
            continue;
        }
        // loopfx: while (isfxplaying) wait 0.1; playFx de nuevo (inicio = ahora); luego otra vez realWait(delay)
        if (now < o.waitUntil) continue;
        if (!FX_IsEffectDone(0, o.h)) { o.waitUntil = now + 100; continue; }
        o.h = PlayCfx(o, fx, now); ++s_fxLoops;
        o.waitUntil = now + (int)(o.delay * 1000.0f);
    }
    static bool logged;
    if (!logged) { bool all = true; for (const CFx &o : s_fx) all = all && o.state != 0;
        if (all) { logged = true; fprintf(stderr, "[H19] Shi No Numa: createfx: %d de %zu efectos lanzados (%d sin FX en los datos)\n", s_fxPlayed, s_fx.size(), s_fxMissing); } }
#endif
}

// ---- trampas electricas (trap_fx_monitor / electric_trap_fx)
struct Trap { std::string name, side; std::vector<unsigned> fx; };
std::vector<Trap> s_traps;

void Init(const std::vector<H19Kv> &)
{
    LoadStructs();
    VisionPoll();
    ParseEffects(); ParseCreateFx();
    static const char *tr[4][2] = { { "north_west_tgt", "north_west_elec_light" }, { "south_west_tgt", "south_west_elec_light" },
                                    { "north_east_tgt", "north_east_elec_light" }, { "south_east_tgt", "south_east_elec_light" } };
    size_t np = 0;
    for (auto &t : tr) { Trap x; x.name = t[0]; x.side = t[1]; s_traps.push_back(x); np += s_structs.count(t[0]) ? s_structs[t[0]].size() : 0; }
    int nl = 0; for (const CFx &o : s_fx) nl += o.loop;
    Fog(0, 0);   // level_fog_init (time 0)
    fprintf(stderr, "[H19] Shi No Numa: %zu level._effect, %zu createfx (%d en bucle), trampas %zu (%zu puntos), niebla de cliente %s\n",
        s_effect.size(), s_fx.size(), nl, s_traps.size(), np, H7C_ClientVolFog ? "si" : "NO");
}

bool LevelNotify(const std::string &ev, int now)
{
    for (Trap &t : s_traps) {
        if (ev == t.name) {   // electric_trap_fx: borra los anteriores y lanza el zapper en cada struct
            for (auto &h : t.fx) H19_KillFx(h); t.fx.clear();
            for (const Pt &p : s_structs[t.name]) { float f[3], r[3], u[3]; AngleVectors(p.ang, f, r, u); t.fx.push_back(H19_PlayFx("misc/fx_zombie_electric_trap", p.org, f, u)); }
            fprintf(stderr, "[H19] Shi No Numa: trampa %s encendida (%zu FX) t=%d\n", t.name.c_str(), t.fx.size(), now);
        }
        if (ev == t.side + "off") { for (auto &h : t.fx) H19_KillFx(h); t.fx.clear(); fprintf(stderr, "[H19] Shi No Numa: trampa %s apagada t=%d\n", t.name.c_str(), now); }
    }
    if (ev == "dog_start") Fog(1, 7.0f);
    if (ev == "dog_stop") Fog(2, 7.0f);
    return false;   // los mezcladores genericos siguen
}

void Frame(int)
{
    VisionPoll();
    CreateFxFrame();
}

// ===================================================================================================== sonido
#ifdef H6SND
}   // namespace
int __cdecl CG_PlaySoundWithHandle(int localClientNum, int entitynum, float *origin, int fadeMs, bool doNotify, float volume, unsigned int id);   // h5_audio_cl.cpp
namespace {
unsigned s_rng = 0x73756d70u;   // secuencia propia (no gasta la de fire_flicker)
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
// zombie_cod5_sumpf_amb.csc main(): declareAmbientPackage + addAmbientElement (angulos por defecto 0..359)
std::vector<Package> s_pk = {
    { "zombies", { { "amb_spooky_2d", 5, 8, 300, 2000 }, { "amb_spooky_2d", 5, 8, 300, 2000 } }, {} },
    { "shack", { { "amb_spooky_2d", 5, 8, 300, 2000 }, { "wood_creak", 5, 12, 10, 450 } }, {} },
    { "wood_shack", { { "amb_spooky_2d", 5, 8, 300, 2000 }, { "wood_creak", 5, 12, 10, 450 } }, {} },
    { "hut_downstairs", { { "amb_spooky_2d", 10, 18, 300, 2000 }, { "wood_creak", 5, 12, 10, 450 } }, {} },
    { "perks_hut", { { "amb_spooky_2d", 5, 8, 300, 2000 }, { "wood_creak", 5, 12, 10, 450 } }, {} },
    { "perks_hut_semi_open", { { "amb_spooky_2d", 5, 8, 300, 2000 }, { "wood_creak", 5, 12, 10, 450 } }, {} },
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
void PkUpdate(double now)   // updateActiveAmbientPackage: la de prioridad mas alta con cuenta (estricta: en empate se queda la primera)
{
    int best = -1, bp = -1;
    for (size_t i = 0; i < s_pk.size(); ++i) for (auto &r : s_pk[i].refcount) if (r.second && r.first > bp) { bp = r.first; best = (int)i; }
    if (best < 0 || best == s_active) return;
    s_active = best;   // killambientElementThread<antigua>: los hilos viejos mueren (un sonido en curso sigue y su entidad se libera sola)
    s_th.assign(s_pk[best].el.size(), Thread{ -1, -1 });
    for (size_t i = 0; i < s_th.size(); ++i) s_th[i].at = now + RandF(s_pk[best].el[i].spawnMin, s_pk[best].el[i].spawnMax) * 1000.0;
    fprintf(stderr, "[H19] Shi No Numa: paquete de ambiente '%s' (prioridad %d)\n", s_pk[best].name, bp);
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

// ---- radio (_waw_zombiemode_radio.csc, level.kzmb_name = "sumpf_kzmb")
const char *const kSongs[] = { "wtf", "abra_macabre", "uhf", "dusk", "underwater", "slasher_flick", "maskwalk", "sand", "temple", "" };
int s_radioEnt = -2, s_radioIdx, s_radioId = -1, s_radioPlays; bool s_radioFlag, s_stopRadio; double s_songAt = -1, s_stopAt = -1; int s_stopId = -1;
void FindRadio()
{
    s_radioEnt = -1;
    for (int e = 0; e < level.num_entities; ++e) { gentity_s *ent = &g_entities[e];
        if (ent->r.inuse && ent->targetname && !strcmp(SL_ConvertToString(ent->targetname, SCRIPTINSTANCE_SERVER), "sumpf_kzmb")) { s_radioEnt = e; break; } }
    fprintf(stderr, "[H19] Shi No Numa: radio sumpf_kzmb: %s\n", s_radioEnt >= 0 ? "encontrada" : "NO esta");
}
void NextSong(double now, const float *org)   // next_song(set = 1)
{
    Play(org, "static");
    if (s_radioId >= 0 && SND_IsPlaying(s_radioId)) {   // fade(level.radio_id, 1)
        SND_SetPlaybackAttenuationRate(s_radioId, 1.0f); SND_SetPlaybackAttenuation(s_radioId, 0.0f);
        s_stopId = s_radioId; s_stopAt = now + 1000.0; s_songAt = now + 1000.0;
    } else s_songAt = now + 500.0;   // wait(.5)
}
void RadioFrame(double now)
{
    if (s_radioEnt == -2) FindRadio();
    if (s_radioEnt < 0) return;
    gentity_s *ent = &g_entities[s_radioEnt];
    if (const char *e = getenv("H19_SUMPF_RADIO")) {   // prueba: el jugador le dispara (zombie_radio_play: waittill("damage"))
        static std::vector<int> q; static bool parsed;
        if (!parsed) { parsed = true; for (const char *p = e; *p; ) { q.push_back(atoi(p)); const char *c = strchr(p, ','); if (!c) break; p = c + 1; } }
        for (int &t : q) if (t >= 0 && level.time >= t) { t = -1; Scr_Notify(ent, scr_const.damage, 0); fprintf(stderr, "[H19] H19_SUMPF_RADIO: damage en la radio t=%d\n", level.time); }
    }
    const bool f = (ent->s.lerp.eFlags2 >> 14) & 1;   // clientflag 14 (SetClientFlag del scriptmover)
    if (f && !s_radioFlag && !s_stopRadio) NextSong(now, ent->r.currentOrigin);
    s_radioFlag = f;
    if (s_stopAt >= 0 && now >= s_stopAt) { SND_StopPlayback(s_stopId); s_stopAt = -1; s_stopId = -1; }
    if (s_songAt >= 0 && now >= s_songAt) {
        s_songAt = -1;
        const char *song = kSongs[s_radioIdx];
        s_radioId = *song ? Play(ent->r.currentOrigin, song) : -1; ++s_radioPlays;
        fprintf(stderr, "[H19] Shi No Numa: radio: cancion %d '%s' (id %d) t=%d\n", s_radioIdx, song, s_radioId, level.time);
        s_radioIdx = (s_radioIdx + 1) % (int)(sizeof kSongs / sizeof *kSongs);
    }
}

int s_meteor;
void AudioStart()
{
    static const float met[3] = { 11264, -1920, -592 };   // play_meteor_loop: playloopat(0, "meteor_loop", (11264, -1920, -592))
    s_meteor = H19A_FakeAt(met); if (s_meteor) H19A_PlayLoop(s_meteor, "meteor_loop", 0);
    for (PoolEnt &p : s_pool) { p.inuse = false; p.id = -1; p.start = 0; }
    LoadPkTrigs();
    PkActivate("zombies", 0, true, level.time);   // activateAmbientPackage(0, "zombies", 0)
    fprintf(stderr, "[H19] Shi No Numa sonido: meteor_loop %s, %zu paquetes, %zu disparadores de paquete\n", s_meteor ? "si" : "NO", s_pk.size(), s_trigs.size());
}
void AudioNotify(const std::string &n)
{
    if (n == "ktr") { s_stopRadio = true; if (s_radioId >= 0) SND_StopPlayback(s_radioId); s_songAt = -1; fprintf(stderr, "[H19] Shi No Numa: radio: ktr (para)\n"); }
    else if (n == "rrd") { s_stopRadio = false; fprintf(stderr, "[H19] Shi No Numa: radio: rrd (sigue)\n"); }
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
    AmbientFrame(now);
    RadioFrame(now);
}
void AudioStatus(std::string &out)
{
    char b[200]; snprintf(b, sizeof b, "; Shi No Numa: paquete %s, elementos %d (sin entidad %d), radio %d/%s, meteorito %d",
        s_active >= 0 ? s_pk[s_active].name : "-", s_ambPlays, s_ambMissed, s_radioPlays, s_stopRadio ? "parada" : "lista", s_meteor);
    out += b;
}
#endif
} // namespace

extern const H19MapCl kH19_zombie_cod5_sumpf = {
    .zone = "zombie_cod5_sumpf", .offEnv = "H19_SUMPF",
    .estado = "Shi No Numa: vision CS1550, niebla y perros, createfx del pantano, trampas electricas, paquetes de ambiente, meteorito y radio (OLA M5)",
    .init = Init, .levelNotify = LevelNotify, .sysState = nullptr, .frame = Frame, .clockHand = nullptr, .screensFrame = nullptr,
    .levelEffect = nullptr,
#ifdef H6SND
    .audioStart = AudioStart, .audioNotify = AudioNotify, .audioSys = AudioSys, .audioFrame = AudioFrame, .audioStatus = AudioStatus,
#endif
    .doorEnt = nullptr,
};
#endif
