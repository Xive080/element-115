// OLA M7: cliente de zombie_temple (Shangri-La). Reproduce sin VM de CSC lo que hacen sus clientscripts (rawfiles del ff:
// clientscripts/zombie_temple.csc, zombie_temple_fx.csc, zombie_temple_amb.csc, zombie_temple_maze.csc, createfx/zombie_temple_fx.csc,
// _zombiemode_ai_napalm.csc y _zombiemode_ai_sonic.csc), con sus mismos numeros. El servidor es el GSC original sin tocar.
//  - Perros: NO (zombie_temple_fx.csc: "we dont have dogs"; nadie llama a enable_dog_rounds). Rondas especiales: monos ladrones de
//    power-ups (zombie_temple_ai_monkey.gsc) y zombis Napalm y Shrieker (sonic) que salen en rondas normales.
//  - vision: pila de _zombiemode.csc (zombie_vision_set_apply/remove: gana la prioridad mas alta, en empate la primera):
//    zombie_temple (1, 0,1 s al conectar/aparecer), zombie_temple_caves (2, 2 s) con el disparador de cliente cave_vision_trig,
//    zombie_temple_eclipse y zombie_temple_eclipseCave (3) con "ec"; "db" los quita. El CS 1550 del servidor se sigue aplicando si cambia.
//  - niebla (setVolFog de zombie_temple_fx.csc): cave_fog_off al empezar y al salir de la cueva (5 s, r_exposureValue 1.1), cave_fog_on
//    al entrar (5 s, 1.15), eclipse_fog_on (0 s) en el eclipse, y la de la cascada (waterfall_fog_change, 2 s).
//  - createfx: 334 oneshotfx, 14 loopfx y 162 exploders leidos del rawfile. Exploders por numero (level._exploder_ids como _utility.csc):
//    '@' del servidor (h7e_light, sysState "@exploder") y los del cliente: "db" activa el 401 (dia: mariposas, libelulas...) y quita el
//    402; "ec" al reves. cannon_effect: espera delay, borra los anteriores y SpawnFx + triggerfx.
//  - corriente (ZPO): el generador vibra (RotateRoll +-0,25 cada 0,1 s), a los 4,5 s "pl1" (luces de _lights.csc) y las lamparas
//    model_lights_on cambian de modelo; wwr/wwl giran las ruedas de agua (RotatePitch 120 en 2,2/1,8 s, la primera acelerando).
//  - PaP: sistema pap_indicator_spinners (bits 3..0 = grupos pap_spinner4..1): bit a 1 = vuelve a su angulo inicial y para, a 0 = gira
//    sin fin (1,7/1,5/1,2/0,8 s por vuelta); solo con corriente. Angulo inicial al azar (0, 90*(n-1)+rand(10,80), 0). Sonidos
//    evt_pap_spinner0N en bucle y evt_pap_timer_stop.
//  - trampas: clientflag 3 de scriptmover = lanzas (las spear_trap_spear de su fila, |dx|<21) salen 90-120 u en 0,08-0,22 s con
//    punji_dust y vuelven en 0,1-0,2 s; clientflag 4 = pared del laberinto mas cercana baja 128 u en 0,25 s / sube en 1 s con su polvo.
//  - tablas flotantes plank_water: suben y bajan 2,5-3 u en 3,5-4,5 s y giran +-360 en 60-90 s.
//  - Napalm: clientflag 0 de actor = fuego en J_Wrist_RI/LE, J_SpineLower (+10 en y), J_Head y calor en tag_origin, bucle
//    evt_napalm_zombie_loop; 2 = brasas antes de explotar; 4 = mojado (vapor en J_SpineLower). Shrieker: clientflag 3 = bucle
//    evt_sonic_ambient_loop.
//  - sonido: snd_play_auto_fx(fx_ztem_torch, amb_fire_medium); "ec" evt_sq_gen_transition + bucle evt_sq_gen_bg, "db" lo quita.
//    Salas, reverb y musica ya los lleva h5_audio_cl (ParseAmb).
// No se hace (anotado en docs/evidencia/m7/LEEME.md): r_skyTransition/SetSunlight se ponen como dvar pero el renderizador del harness
// no tiene cielo de transicion; vapor de napalm a la altura del agua (getwaterheight), r_flameScaler, sombreado de brillo de napalm
// (setshaderconstant), cadenas fxanim (animacion), rastros de cristales y estela de agua del huevo (clientflags 6-9), chorros de la
// cascada del huevo (WF), vibracion del mando en el laberinto, muneco falso del geiser (solo cooperativo) y la caja de cliente
// (_init_magic_box esta comentada en el original).
//   H19_TEMPLE=0 lo apaga.
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <universal/dvar.h>
#include <database/db_registry.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <bgame/bg_local.h>
#include <clientscript/cscr_stringlist.h>
#include <EffectsCore/fx_system.h>
#include <EffectsCore/fx_unique_handle.h>
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
#include <functional>
#include <map>
#include <mutex>
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

namespace {
const char kName[] = "Shangri-La";
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
float RandRange(float a, float b) { return a + (b - a) * H19_Rand01(); }   // randomfloatrange del CSC
void SetDvar(const char *n, const char *v) { if (Dvar_FindVar(n)) Dvar_SetFromStringByName(n, (char *)v); }
void SetDvarF(const char *n, float f) { char b[32]; snprintf(b, sizeof b, "%g", f); SetDvar(n, b); }

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

// ---- sonido pedido desde el hilo de render; lo reproduce AudioFrame (hilo de servidor). slot = entidad falsa propia (se crea alli)
struct Snd { int kind; int slot; float org[3]; std::string alias; float fade; };   // kind: 0 playsound en org, 1 bucle en slot, 2 parar bucle, 3 mover slot
std::mutex s_sndMu; std::vector<Snd> s_sndQ;
void QSnd(int kind, int slot, const float *org, const char *alias, float fade = 0)
{
    Snd s; s.kind = kind; s.slot = slot; if (org) memcpy(s.org, org, 12); else memset(s.org, 0, 12); s.alias = alias ? alias : ""; s.fade = fade;
    std::lock_guard<std::mutex> g(s_sndMu); s_sndQ.push_back(s);
}

// ===================================================================================================== cliente grafico
std::map<std::string, std::string> s_effect;   // level._effect[clave] -> ruta de LoadFx
void ParseEffects(const char *raw)
{
    const std::string t = Raw(raw), l = Lower(t);
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
unsigned Fx(const char *key, const float *org, const float *fwd, const float *up)   // PlayFX(client, level._effect[key], org, fwd, up)
{
    auto it = s_effect.find(key); if (it == s_effect.end()) return 0;
    return H19_PlayFx(it->second.c_str(), org, fwd, up);
}
unsigned FxDef(const char *key, const float *org)   // PlayFX sin direccion: angulos (270, 0, 0) como CScr_SetFxAngles sin ejes
{
    auto it = s_effect.find(key); if (it == s_effect.end()) return 0;
    static const float def[3] = { 270, 0, 0 };
    return H19_PlayFxAng(it->second.c_str(), org, def);
}

// ---- vision: CS 1550 y pila de _zombiemode.csc
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
    fprintf(stderr, "[H19] %s: vision del servidor (CS 1550): '%s' en %d ms\n", kName, name.c_str(), ms);
    H7_VisionSetNaked(name.c_str(), ms / 1000.0f);
}
struct Vis { std::string name; int prio; };
std::vector<Vis> s_vis;   // self._zombie_visionset_list
void VisionApplyTop(float sec)
{
    const char *best = nullptr; int hs = 0;
    for (const Vis &v : s_vis) if (v.prio > hs) { hs = v.prio; best = v.name.c_str(); }   // zombie_highest_vision_set_apply (estricta)
    if (!best || !H7_VisionSetNaked || !H19_EnvOn("H7E_VISION")) return;
    fprintf(stderr, "[H19] %s: vision '%s' (prioridad %d) en %.1f s\n", kName, best, hs, sec);
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

// ---- niebla (zombie_temple_fx.csc / zombie_temple.csc)
enum { FOG_CAVE_OFF, FOG_CAVE_ON, FOG_ECLIPSE, FOG_WATERFALL };
void Fog(int which)
{
    // setVolFog(start, halfwayDist, halfwayHeight, baseHeight, r, g, b, colorScale, sunR, sunG, sunB, dirX, dirY, dirZ, startAng, endAng, time, maxOpacity)
    static const float off[18] = { 151.081f, 526.479f, 461.457f, -652.425f, 0.0784314f, 0.129412f, 0.105882f, 5.97369f, 1, 0.380392f, 0.109804f,
                                   -0.893398f, -0.219239f, 0.392141f, 11.4807f, 94.7812f, 5, 0.604413f };
    static const float on[18] = { 683.36f, 2628.91f, 1277.89f, 1540.34f, 0.345098f, 0.47451f, 0.541176f, 4.0001f, 0.301961f, 0.380392f, 0.427451f,
                                  -0.889059f, 0.215284f, 0.404014f, 11.4807f, 37.3192f, 5, 0.369586f };
    static const float ec[18] = { 151.622f, 136.124f, 392.122f, -626.865f, 0.168627f, 0.168627f, 0.168627f, 4.39448f, 0.376471f, 0.470588f, 0.513726f,
                                  -0.893398f, -0.219239f, 0.392141f, 11.4807f, 88.2474f, 0, 0.593122f };
    static const float wf[18] = { 257.581f, 3581.88f, 461.457f, -652.425f, 0.0784314f, 0.129412f, 0.105882f, 5.97369f, 1, 0.380392f, 0.109804f,
                                  -0.893398f, -0.219239f, 0.392141f, 11.4807f, 91.2748f, 2, 0.590773f };
    const float *v = which == FOG_CAVE_ON ? on : which == FOG_ECLIPSE ? ec : which == FOG_WATERFALL ? wf : off;
    if (H7C_ClientVolFog) H7C_ClientVolFog(v, (int)(v[16] * 1000.0f));
    if (which == FOG_CAVE_OFF || which == FOG_CAVE_ON) { SetDvar("r_exposureTweak", "1"); SetDvar("r_exposureValue", which == FOG_CAVE_ON ? "1.15" : "1.1"); }
    static const char *const nm[] = { "exterior (cave_fog_off)", "cueva (cave_fog_on)", "eclipse (eclipse_fog_on)", "cascada (waterfall)" };
    fprintf(stderr, "[H19] %s: niebla %s, %.0f s\n", kName, nm[which], v[16]);
}

// ---- disparadores de cliente (trigger_multiple con spawnflags 128): trigger_thread enter/leave del jugador local
struct Trig { std::string tn; unsigned model; float org[3]; bool world; bool inside; };
std::vector<Trig> s_trigs;
void LoadTrigs()
{
    for (const H19Kv &k : H19_MapEnts()) {
        const char *tn = H19_Str(k, "targetname"), *m = H19_Str(k, "model"), *cn = H19_Str(k, "classname");
        if (!tn || !m || m[0] != '*' || !cn || strcmp(cn, "trigger_multiple")) continue;
        if (strcmp(tn, "cave_vision_trig") && strcmp(tn, "waterfall_fog_change")) continue;
        Trig t; t.tn = tn; t.model = (unsigned)atoi(m + 1); t.inside = false; memset(t.org, 0, 12); H19_Vec(k, "origin", t.org);
        if (!t.model || t.model >= cm.numSubModels) continue;
        // brocha en coordenadas de mundo o relativas al origen: si el origen cae dentro de sus limites, es de mundo
        const cmodel_t &c = cm.cmodels[t.model];
        t.world = t.org[0] >= c.mins[0] - 1 && t.org[0] <= c.maxs[0] + 1 && t.org[1] >= c.mins[1] - 1 && t.org[1] <= c.maxs[1] + 1 && t.org[2] >= c.mins[2] - 1 && t.org[2] <= c.maxs[2] + 1;
        s_trigs.push_back(t);
    }
}
bool Touching(const Trig &t, const float *org, const float *mins, const float *maxs)   // istouching: caja del jugador contra la brocha
{
    trace_t tr; memset(&tr, 0, sizeof tr); const float zero[3] = { 0, 0, 0 };
    CM_TransformedBoxTrace(&tr, org, org, mins, maxs, t.model, -1, t.world ? zero : t.org, zero);
    return tr.startsolid || tr.allsolid;
}

// ---- createfx: oneshotfx, loopfx y exploders
struct CfxEnt { float org[3], ang[3], delay; std::string fxid; int exploder; bool loop; int state; unsigned h; int waitUntil; std::vector<unsigned> fx; };
std::vector<CfxEnt> s_cfx;
std::map<int, int> s_exploderId;   // level._exploder_ids[numero] = id (1, 2...)
int s_fxT0 = -1, s_fxPlayed, s_fxMissing, s_fxLoops, s_expOn;
void ParseCreateFx()
{
    const std::string t = Raw("clientscripts/createfx/zombie_temple_fx.csc");
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
        if (ln.find("createOneshotEffect") != std::string::npos || ln.find("createExploder") != std::string::npos) { flush(); open = true; cur.delay = -15; cur.loop = false; continue; }
        if (ln.find("createLoopEffect") != std::string::npos) { flush(); open = true; cur.delay = 0.5f; cur.loop = true; continue; }
        size_t k = ln.find("ent.v[");
        if (!open || k == std::string::npos) continue;
        std::string key, val; size_t e = 0; if (!Quoted(ln, k, key, &e)) continue;
        size_t eq = ln.find('=', e); if (eq == std::string::npos) continue;
        const char *r = ln.c_str() + eq + 1;
        if (key == "origin" || key == "angles") { float *d = key == "origin" ? cur.org : cur.ang; sscanf(r, " ( %f , %f , %f )", &d[0], &d[1], &d[2]); }
        else if (key == "delay") cur.delay = (float)atof(r);
        else if (key == "fxid") { if (Quoted(ln, eq, val)) cur.fxid = val; }
        else if (key == "exploder") cur.exploder = atoi(r);
        else if (key == "type") { if (Quoted(ln, eq, val) && val != "oneshotfx" && val != "exploder" && val != "loopfx") open = false; }
    }
    flush();
}
#ifdef H5FX
unsigned PlayCfx(const CfxEnt &o, const FxEffectDef *fx, int startMsec)
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
    if (!fx_systemPool || s_cfx.empty()) return;
    const int now = FX_GetSystem(0)->system.msecNow;
    if (s_fxT0 < 0) { if (now <= 0) return; s_fxT0 = now; fprintf(stderr, "[H19] %s: createfx: reloj de FX en marcha, t0=%d\n", kName, now); }
    for (CfxEnt &o : s_cfx) {
        if (o.state == 2 || o.exploder >= 0) continue;
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
        if (now < o.waitUntil) continue;   // loopfx: while (isfxplaying) wait 0.1; playFx de nuevo; realWait(delay)
        if (!FX_IsEffectDone(0, o.h)) { o.waitUntil = now + 100; continue; }
        o.h = PlayCfx(o, fx, now); ++s_fxLoops;
        o.waitUntil = now + (int)(o.delay * 1000.0f);
    }
    static bool logged;
    if (!logged) { bool all = true; for (const CfxEnt &o : s_cfx) all = all && (o.state != 0 || o.exploder >= 0);
        if (all) { logged = true; fprintf(stderr, "[H19] %s: createfx: %d efectos lanzados (%d sin FX en los datos)\n", kName, s_fxPlayed, s_fxMissing); } }
#endif
}
void ExploderNum(int num, bool on, int now)   // activate_exploder / deactivate_exploder (_fx.csc) por numero
{
    int n = 0;
    for (CfxEnt &o : s_cfx) {
        if (o.exploder != num) continue;
        for (unsigned &h : o.fx) H19_KillFx(h);   // cannon_effect / deactivate_exploder: borra los loopFX anteriores
        o.fx.clear();
        ++n;
        if (!on) continue;
        CfxEnt *po = &o;
        After(now + (o.delay > 0 ? (int)(o.delay * 1000.0f) : 0), [po](int) {   // exploder_delay
            float f[3], r[3], u[3]; AngleVectors(po->ang, f, r, u);
            auto it = s_effect.find(po->fxid); if (it == s_effect.end()) return;
            po->fx.push_back(H19_PlayFx(it->second.c_str(), po->org, f, u));
        });
    }
    s_expOn += on ? 1 : 0;
    fprintf(stderr, "[H19] %s: exploder %d %s: %d efectos t=%d\n", kName, num, on ? "activado" : "desactivado", n, now);
}
void ExploderId(int id, int now)   // '@' del servidor: id = orden de aparicion del numero (1, 2...); negativo = quitar
{
    int num = -1; for (auto &e : s_exploderId) if (e.second == (id < 0 ? -id : id)) num = e.first;
    if (num < 0) { fprintf(stderr, "[H19] %s: exploder id %d desconocido\n", kName, id); return; }
    ExploderNum(num, id > 0, now);
}

// ---- movimiento de script_model de cliente (moveto / rotateto / rotatepitch con aceleracion y frenado)
float Prog(float t, float T, float a, float d)   // fraccion recorrida con perfil trapezoidal de velocidad (MoveTo/RotateTo con accel/decel)
{
    if (T <= 0 || t >= T) return 1.0f; if (t <= 0) return 0.0f;
    if (a + d > T) { const float s = T / (a + d); a *= s; d *= s; }
    const float v = 1.0f / (T - 0.5f * a - 0.5f * d);
    if (t < a) return 0.5f * v * t * t / a;
    if (t <= T - d) return v * (0.5f * a + (t - a));
    const float r = T - t; return 1.0f - 0.5f * v * r * r / d;
}
struct Move { bool on; int t0; float dur, acc, dec; float org0[3], dOrg[3], ang0[3], dAng[3]; std::function<void(int)> done; };
std::map<int, Move> s_mv;   // por indice de script_model de cliente (h7e)
void StartMove(int j, const float *dOrg, const float *dAng, float sec, float acc, float dec, int now, std::function<void(int)> done = nullptr)
{
    Move m; m.on = true; m.t0 = now; m.dur = sec; m.acc = acc; m.dec = dec; H19_ModelGet(j, m.org0, m.ang0);
    for (int k = 0; k < 3; ++k) { m.dOrg[k] = dOrg ? dOrg[k] : 0; m.dAng[k] = dAng ? dAng[k] : 0; }
    m.done = std::move(done); s_mv[j] = std::move(m);
}
void StopMove(int j) { s_mv.erase(j); }   // notify("stop_...") / un MoveTo nuevo sustituye al anterior
bool Moving(int j) { auto it = s_mv.find(j); return it != s_mv.end() && it->second.on; }
void MovesFrame(int now)
{
    std::vector<std::pair<int, std::function<void(int)>>> fin;
    for (auto &kv : s_mv) {
        Move &m = kv.second; if (!m.on) continue;
        const float f = Prog((now - m.t0) / 1000.0f, m.dur, m.acc, m.dec);
        float o[3], a[3]; for (int k = 0; k < 3; ++k) { o[k] = m.org0[k] + m.dOrg[k] * f; a[k] = m.ang0[k] + m.dAng[k] * f; }
        for (int k = 0; k < 3; ++k) a[k] = fmodf(a[k], 360.0f);   // los angulos del script_mover se quedan en (-360, 360)
        H19_ModelPose(kv.first, o, a);
        if (f >= 1.0f) { m.on = false; fin.push_back({ kv.first, std::move(m.done) }); }   // "movedone" / "rotatedone"
    }
    for (auto &f : fin) if (f.second) f.second(now);
}
std::vector<int> ModelsByTn(const char *tn)   // GetEntArray(client, tn, "targetname") de los script_model de cliente
{
    std::vector<int> r; for (int j = 0; j < H19_ModelCount(); ++j) if (!strcmp(H19_ModelTn(j), tn)) r.push_back(j);
    return r;
}
float Org(int j, int k) { float o[3], a[3]; H19_ModelGet(j, o, a); return o[k]; }

// ---- corriente: generador, ruedas de agua y lamparas
bool s_power;
void GeneratorStep(int j, float roll, int now)   // generator_move: RotateRoll(-0.25, 0.1), luego +0.5 / -0.5 sin fin
{
    const float d[3] = { 0, 0, roll };
    StartMove(j, nullptr, d, 0.1f, 0, 0, now, [j, roll](int t) { GeneratorStep(j, roll < 0 ? 0.5f : -0.5f, t); });
}
void WheelStep(int j, float rot, float time, bool first, int now)   // rotateWheel: RotatePitch(rot, time, time-0.5, 0.1) y luego sin aceleracion
{
    const float d[3] = { rot, 0, 0 };
    StartMove(j, nullptr, d, time, first ? time - 0.5f : 0, first ? 0.1f : 0, now, [j, rot, time](int t) { WheelStep(j, rot, time, false, t); });
}
void PowerOn(int now)
{
    if (s_power) return;
    s_power = true;   // power_watch: level.power = true
    for (int j : ModelsByTn("power_generator")) GeneratorStep(j, -0.25f, now);
    After(now + 4500, [](int t) {   // temple_light_model_swap_init: realwait(4.5) -> notify pl1 y SetModel de model_lights_on
        H19_LevelNotify("pl1", t);
        int n = 0;
        for (const H19Kv &k : H19_MapEnts()) {   // script_string manda; si no, el modelo encendido segun el apagado
            const char *tn = H19_Str(k, "targetname"), *ss = H19_Str(k, "script_string"); float o[3];
            if (!tn || strcmp(tn, "model_lights_on") || !ss || !H19_Vec(k, "origin", o)) continue;
            for (int j : ModelsByTn("model_lights_on")) { float mo[3], ma[3]; H19_ModelGet(j, mo, ma);
                if (fabsf(mo[0] - o[0]) < 1 && fabsf(mo[1] - o[1]) < 1 && fabsf(mo[2] - o[2]) < 1) { H19_SetModel(j, ss); ++n; } }
        }
        n += H19_SwapModelByTn("model_lights_on", "p_ztem_power_hanging_light_off", "p_ztem_power_hanging_light");
        n += H19_SwapModelByTn("model_lights_on", "p_lights_cagelight02_off", "p_lights_cagelight02_on");
        fprintf(stderr, "[H19] %s: pl1 (luces de la corriente) y %d lamparas encendidas t=%d\n", kName, n, t);
    });
    fprintf(stderr, "[H19] %s: corriente (ZPO): generador en marcha t=%d\n", kName, now);
}

// ---- Pack-a-Punch: ruletas pap_spinner1..4
struct Spinner { int j, list; float startYaw; bool forever, toStart; int slot; };
std::vector<Spinner> s_spin;
float SpinTime(int list) { return list == 2 ? 1.5f : list == 3 ? 1.2f : list == 4 ? 0.8f : 1.7f; }   // spinner_get_spin_time
void SpinLoop(int j, float T, float acc, int now)
{
    const float d[3] = { 0, 360, 0 };
    StartMove(j, nullptr, d, T, acc, 0, now, [j, T](int t) { SpinLoop(j, T, 0, t); });
}
void SpinForever(Spinner &s, int now)
{
    if (!s_power || s.forever) return;
    s.forever = true; s.toStart = false;
    QSnd(1, s.slot, nullptr, (std::string("evt_pap_spinner0") + char('0' + s.list)).c_str());   // start_spinner_sound
    SpinLoop(s.j, SpinTime(s.list), 0.25f, now);   // primera vuelta acelerando (rotateYaw(360, t, .25)) y luego sin fin
}
void SpinToStart(Spinner &s, int now)
{
    if (!s_power || s.toStart) return;
    s.forever = false; s.toStart = true;
    float o[3], a[3]; H19_ModelGet(s.j, o, a);
    float delta = s.startYaw - a[1]; while (delta < 0) delta += 360.0f; delta = fmodf(delta, 360.0f);
    const float T = SpinTime(s.list) * (delta / 360.0f);
    Spinner *ps = &s;
    auto stop = [ps](int) {   // stop_spinner_sound + angles = startAngles
        QSnd(2, ps->slot, nullptr, nullptr); float o2[3], a2[3]; H19_ModelGet(ps->j, o2, a2); QSnd(0, -1, o2, "evt_pap_timer_stop");
        a2[1] = ps->startYaw; StopMove(ps->j); H19_ModelPose(ps->j, o2, a2);
    };
    if (T > 0) { const float d[3] = { 0, delta, 0 }; StartMove(s.j, nullptr, d, T, 0, 0, now, stop); }
    else { StopMove(s.j); stop(now); }
}
int s_spinState = -1;
void SetNumVisibleSpinners(int num, int now)   // _set_num_visible_spinners: bit i a 1 = grupo i+1 a su sitio, 0 = gira
{
    s_spinState = num;
    for (int i = 3; i >= 0; --i) {
        const int pw = 1 << i; const bool bit = num >= pw; if (bit) num -= pw;
        for (Spinner &s : s_spin) if (s.list == i + 1) { if (bit) SpinToStart(s, now); else SpinForever(s, now); }
    }
}

// ---- trampas: lanzas (clientflag 3) y paredes del laberinto (clientflag 4)
struct Spear { int j; float start[3], dir[3]; bool assigned; };
std::vector<Spear> s_spears;
struct Wall { int j; float start[3]; float activeZ; bool assigned, active; };
std::vector<Wall> s_walls;
std::map<int, std::vector<int>> s_spearRow;   // entnum del scriptmover del servidor -> sus lanzas (set_trap_spears)
std::map<int, int> s_wallOf;                  // entnum -> pared (set_maze_trap_wall)
int s_spearMoves, s_wallMoves;
void SpikeTrap(int ent, const float *eorg, bool active, int now)
{
    if (!s_spearRow.count(ent)) {   // set_trap_spears: las de su fila (todas alineadas en y), solo se asignan una vez
        std::vector<int> &row = s_spearRow[ent];
        for (size_t i = 0; i < s_spears.size(); ++i) { Spear &s = s_spears[i]; if (s.assigned) continue;
            if (fabsf(eorg[0] - s.start[0]) < 21.0f) { s.assigned = true; row.push_back((int)i); } }
    }
    const std::vector<int> &row = s_spearRow[ent];
    for (size_t k = 0; k < row.size(); ++k) {
        Spear &s = s_spears[row[k]]; const bool snd = k == 0;
        float o[3], a[3]; H19_ModelGet(s.j, o, a);
        if (active) {
            if (snd) QSnd(0, -1, o, "evt_spiketrap_warn");
            const float dist = RandRange(90, 120);
            const float end[3] = { s.start[0] + s.dir[0] * dist, s.start[1] + s.dir[1] * dist, s.start[2] + s.dir[2] * dist };
            FxDef("punji_dust", end);
            QSnd(0, -1, o, "evt_spiketrap");
            const float d[3] = { end[0] - o[0], end[1] - o[1], end[2] - o[2] };
            StartMove(s.j, d, nullptr, RandRange(0.08f, 0.22f), 0, 0, now);
        } else {
            if (snd) QSnd(0, -1, o, "evt_spiketrap_retract");
            const float d[3] = { s.start[0] - o[0], s.start[1] - o[1], s.start[2] - o[2] };
            StartMove(s.j, d, nullptr, RandRange(0.1f, 0.2f), 0, 0, now);
        }
        ++s_spearMoves;
    }
}
void MazeWall(int ent, const float *eorg, bool active, int now)
{
    if (!s_wallOf.count(ent)) {   // set_maze_trap_wall: la mas cercana sin asignar
        int best = -1; float bd = 0;
        for (size_t i = 0; i < s_walls.size(); ++i) { const Wall &w = s_walls[i]; if (w.assigned) continue;
            float o[3], a[3]; H19_ModelGet(w.j, o, a);
            const float d = (o[0] - eorg[0]) * (o[0] - eorg[0]) + (o[1] - eorg[1]) * (o[1] - eorg[1]) + (o[2] - eorg[2]) * (o[2] - eorg[2]);
            if (best < 0 || d < bd) { bd = d; best = (int)i; } }
        s_wallOf[ent] = best; if (best >= 0) s_walls[best].assigned = true;
    }
    const int wi = s_wallOf[ent]; if (wi < 0) return;
    Wall &w = s_walls[wi];
    // move_maze_wall: init_maze_mover(-128, 0.25, 1.0): activa baja 128 en 0,25 s, inactiva vuelve en 1 s; a medio camino, tiempo proporcional
    float o[3], a[3]; H19_ModelGet(w.j, o, a);
    const float goalZ = active ? w.activeZ : w.activeZ + 128.0f;
    float T = active ? 0.25f : 1.0f;
    if (Moving(w.j)) T *= fabsf(goalZ - o[2]) / 128.0f;
    if (T == 0) T = 0.01f;
    w.active = active;
    const float d[3] = { 0, 0, goalZ - o[2] };
    Wall *pw = &w;
    StartMove(w.j, d, nullptr, T, 0, 0, now, [pw](int) {   // _maze_mover_move: al acabar, polvo en org + (0,0,-60)
        float o2[3], a2[3]; H19_ModelGet(pw->j, o2, a2); o2[2] -= 60.0f;
        float f[3], r[3], u[3]; AngleVectors(a2, f, r, u); const float up[3] = { 0, 0, 1 };
        Fx(pw->active ? "maze_wall_impact" : "maze_wall_raise", o2, f, up);
    });
    ++s_wallMoves;
}

// ---- tablas flotantes (floating_boards_init / float_board)
void BoardBob(int j, float minZ, float maxZ, float T, bool down, int now)
{
    const float z = Org(j, 2), d[3] = { 0, 0, (down ? minZ : maxZ) - z };
    StartMove(j, d, nullptr, T, 0, 0, now, [j, minZ, maxZ, T, down](int t) { BoardBob(j, minZ, maxZ, T, !down, t); });
}
// la tabla hace dos movimientos a la vez (MoveZ y RotateYaw): el giro va aparte con su propio reloj
struct BoardRot { int j; int t0; float dur, yaw0, dYaw; };
std::vector<BoardRot> s_boardRot;
void BoardsFrame(int now)
{
    for (BoardRot &b : s_boardRot) {
        if (b.t0 < 0) continue;
        float f = (now - b.t0) / (b.dur * 1000.0f); if (f > 1) f = 1;
        float o[3], a[3]; H19_ModelGet(b.j, o, a); a[1] = fmodf(b.yaw0 + b.dYaw * f, 360.0f); H19_ModelPose(b.j, o, a);
        if (f >= 1) { b.yaw0 = a[1]; b.dYaw = RandRange(-360, 360); b.dur = RandRange(60, 90); b.t0 = now; }   // board_rotate
    }
}

// ---- Napalm y Shrieker (clientflags de actor)
struct JointFx { const char *tag; const char *fx; float off[3]; unsigned h; const FxEffectDef *def; };
struct Actor { bool napalm, explode, wet, sonic; std::vector<JointFx> fx, fxEnd; int slot; int seenUse; };
std::map<int, Actor> s_act;
int s_napalmSeen, s_sonicSeen;
const char *kNapalmFx[][2] = {
    { "napalm_fire_forearm", "maps/zombie_temple/fx_ztem_napalm_zombie_forearm" }, { "napalm_fire_torso", "maps/zombie_temple/fx_ztem_napalm_zombie_torso" },
    { "napalm_distortion", "maps/zombie_temple/fx_ztem_napalm_zombie_heat" },
    { "napalm_fire_forearm_end", "maps/zombie_temple/fx_ztem_napalm_zombie_torso_end" },   // asi en el original (cruzados)
    { "napalm_fire_torso_end", "maps/zombie_temple/fx_ztem_napalm_zombie_forearm_end" },
    { "napalm_steam", "maps/zombie_temple/fx_ztem_zombie_torso_steam_runner" } };
bool TagMatrix(gentity_s *e, const char *tag, float m[4][3])
{
    const unsigned t = SL_FindLowercaseString(tag, SCRIPTINSTANCE_SERVER);
    if (t && G_DObjGetWorldTagMatrix(e, t, m)) return true;
    AnglesToAxis(e->r.currentAngles, m); memcpy(m[3], e->r.currentOrigin, 12); return false;
}
void JointPos(gentity_s *e, const JointFx &f, float m[4][3])   // LinkTo(self, joint, offset): offset en ejes del hueso
{
    TagMatrix(e, f.tag, m);
    for (int k = 0; k < 3; ++k) m[3][k] += f.off[0] * m[0][k] + f.off[1] * m[1][k] + f.off[2] * m[2][k];
}
void StartJointFx(gentity_s *e, std::vector<JointFx> &v, const std::vector<std::pair<const char *, const char *>> &list)   // _zombie_SetupFXOnJoint
{
    for (auto &p : list) {
        JointFx f; f.tag = p.first; f.fx = p.second; f.off[0] = f.off[2] = 0; f.off[1] = !strcmp(p.first, "j_spinelower") ? 10.0f : 0.0f; f.h = 0; f.def = nullptr;
        float m[4][3]; JointPos(e, f, m);
        auto it = s_effect.find(f.fx);
        if (it != s_effect.end()) { f.h = H19_PlayFx(it->second.c_str(), m[3], m[0], m[2]);
#ifdef H5FX
            f.def = FX_Register((char *)it->second.c_str());
#endif
        }
        v.push_back(f);
    }
}
void KillJointFx(std::vector<JointFx> &v) { for (JointFx &f : v) H19_KillFx(f.h); v.clear(); }
void FollowJointFx(gentity_s *e, std::vector<JointFx> &v)   // PlayFXOnTag sobre un tag_origin enlazado: el efecto sigue al hueso
{
#ifdef H5FX
    if (!fx_systemPool) return;
    for (JointFx &f : v) {
        if (!f.h || !f.def) continue;
        FxEffectContainer *c = UniqueHandleToEffect(&FX_GetSystem(0)->system, f.h);   // h = uniqueHandle (como FX_ThroughWithEffect)
        if (!c || c->effect.def != f.def) { f.h = 0; continue; }   // el efecto ya no es el nuestro
        float m[4][3]; JointPos(e, f, m); float ax[3][3]; memcpy(ax, m, sizeof ax);
        memcpy(c->effect.frameNow.origin, m[3], 12); AxisToQuat(ax, c->effect.frameNow.quat);
    }
#else
    (void)e; (void)v;
#endif
}
const std::vector<std::pair<const char *, const char *>> kRun = { { "j_wrist_ri", "napalm_fire_forearm" }, { "j_wrist_le", "napalm_fire_forearm" },
    { "j_spinelower", "napalm_fire_torso" }, { "j_head", "napalm_fire_forearm" }, { "tag_origin", "napalm_distortion" } };
const std::vector<std::pair<const char *, const char *>> kEnd = { { "j_elbow_le", "napalm_fire_forearm_end" }, { "j_elbow_ri", "napalm_fire_forearm_end" },
    { "j_clavicle_ri", "napalm_fire_forearm_end" }, { "j_clavicle_le", "napalm_fire_forearm_end" }, { "j_spinelower", "napalm_fire_torso_end" } };
const std::vector<std::pair<const char *, const char *>> kWet = { { "j_spinelower", "napalm_steam" } };
int s_nextSlot = 100;
void ActorsFrame(int now)
{
    std::map<int, bool> alive;
    for (int n = 0; n < level.num_entities; ++n) {
        gentity_s *e = &g_entities[n];
        if (!e->r.inuse || e->s.eType != ET_ACTOR) continue;
        const unsigned fl = e->s.lerp.eFlags2;
        const bool napalm = fl & 1, explode = (fl >> 2) & 1, sonic = (fl >> 3) & 1, wet = (fl >> 4) & 1;
        auto it = s_act.find(n);
        if (it != s_act.end() && it->second.seenUse != e->s.lerp.useCount) { KillJointFx(it->second.fx); KillJointFx(it->second.fxEnd); QSnd(2, it->second.slot, nullptr, nullptr); s_act.erase(it); it = s_act.end(); }
        if (it == s_act.end()) { if (!napalm && !sonic) continue; Actor a{}; a.slot = s_nextSlot++; a.seenUse = e->s.lerp.useCount; it = s_act.emplace(n, a).first; }
        Actor &a = it->second; alive[n] = true;
        if (napalm != a.napalm) {   // napalm_zombie_spawn(set): fuego en los huesos y bucle; (clear): stop_fx
            a.napalm = napalm;
            KillJointFx(a.fx); KillJointFx(a.fxEnd);
            if (napalm) { StartJointFx(e, a.fx, a.wet ? kWet : kRun); QSnd(1, a.slot, e->r.currentOrigin, "evt_napalm_zombie_loop", 2); ++s_napalmSeen;
                fprintf(stderr, "[H19] %s: zombi Napalm (ent %d) t=%d\n", kName, n, now); }
            else QSnd(2, a.slot, nullptr, nullptr, 0.25f);
        }
        if (explode && !a.explode && a.napalm) { StartJointFx(e, a.fxEnd, kEnd); fprintf(stderr, "[H19] %s: Napalm a punto de explotar (ent %d) t=%d\n", kName, n, now); }   // napalm_zombie_explode_callback
        a.explode = explode;
        if (wet != a.wet && a.napalm) {   // napalm_start_wet_fx / napalm_end_wet_fx: stop_fx y el otro juego de efectos
            KillJointFx(a.fx); KillJointFx(a.fxEnd); StartJointFx(e, a.fx, wet ? kWet : kRun);
            if (wet) QSnd(2, a.slot, nullptr, nullptr, 0.25f); else QSnd(1, a.slot, e->r.currentOrigin, "evt_napalm_zombie_loop", 2);
        }
        a.wet = wet;
        if (sonic != a.sonic) {   // sonic_zombie_callback: PlayLoopSound("evt_sonic_ambient_loop", 1)
            a.sonic = sonic;
            if (sonic) { QSnd(1, a.slot, e->r.currentOrigin, "evt_sonic_ambient_loop", 1); ++s_sonicSeen; fprintf(stderr, "[H19] %s: zombi Shrieker (ent %d) t=%d\n", kName, n, now); }
        }
        FollowJointFx(e, a.fx); FollowJointFx(e, a.fxEnd);
        if (a.napalm || a.sonic) QSnd(3, a.slot, e->r.currentOrigin, nullptr);
    }
    for (auto it = s_act.begin(); it != s_act.end();) {   // el actor murio o se borro: sus efectos enlazados se van con el
        if (alive.count(it->first)) { ++it; continue; }
        KillJointFx(it->second.fx); KillJointFx(it->second.fxEnd); QSnd(2, it->second.slot, nullptr, nullptr, 0.25f); it = s_act.erase(it);
    }
}

// ---- clientflags de scriptmover del servidor (register_clientflag_callback "scriptmover")
std::map<int, unsigned> s_moverFl;
void MoversFrame(int now)
{
    for (int n = 0; n < level.num_entities; ++n) {
        gentity_s *e = &g_entities[n];
        if (!e->r.inuse || e->s.eType != ET_SCRIPTMOVER) { s_moverFl.erase(n); continue; }
        const unsigned fl = e->s.lerp.eFlags2 & ((1u << 3) | (1u << 4));
        auto it = s_moverFl.find(n); const unsigned prev = it == s_moverFl.end() ? 0 : it->second;
        if (it != s_moverFl.end() && fl == prev) continue;
        s_moverFl[n] = fl;
        if ((fl ^ prev) & (1u << 3)) SpikeTrap(n, e->r.currentOrigin, fl & (1u << 3), now);
        if ((fl ^ prev) & (1u << 4)) MazeWall(n, e->r.currentOrigin, fl & (1u << 4), now);
    }
}

// ---- eclipse / amanecer (ec / db) y time_travel
bool s_eclipse, s_inCave, s_firstSky = true; int s_timeTravel;
void StrobeSky(float endVal, int now)   // strobe_sky_and_transition
{
    float val = endVal; int t = now; float delay = 0.016f;
    for (int i = 0; i < 10; ++i) { const float v = val; After(t, [v](int) { SetDvarF("r_skyTransition", v); }); val = 1.0f - val; t += (int)(delay * 1000.0f); delay *= 1.1f; }
    float step = 1.0f / 120.0f; val = 1.0f - endVal; if (val == 0) step = -step;
    for (int i = 0; i < 120; ++i) { const float v = val; After(t, [v](int) { SetDvarF("r_skyTransition", v); }); val -= step; t += 16; }
    After(t, [endVal](int) { SetDvarF("r_skyTransition", endVal); });
}
void TimeTravel(int now)   // time_travel_vision (la primera se ignora)
{
    if (++s_timeTravel == 1) return;
    SetDvarF("r_poisonFX_debug_amount", 0); SetDvar("r_poisonFX_debug_enable", "1"); SetDvarF("r_poisonFX_pulse", 2);
    SetDvarF("r_poisonFX_warpX", -0.3f); SetDvarF("r_poisonFX_warpY", 0.15f); SetDvarF("r_poisonFX_dvisionA", 0); SetDvarF("r_poisonFX_dvisionX", 0);
    SetDvarF("r_poisonFX_dvisionY", 0); SetDvarF("r_poisonFX_blurMin", 0); SetDvarF("r_poisonFX_blurMax", 3); SetDvarF("r_poisonFX_debug_amount", 1);
    int t = now + 1750; float amount = 1;
    while (amount > 0) { amount = amount - 0.064f > 0 ? amount - 0.064f : 0; const float a = amount; After(t, [a](int) { SetDvarF("r_poisonFX_debug_amount", a); }); t += 16; }
    After(t, [](int) { SetDvarF("r_poisonFX_debug_amount", 0); SetDvar("r_poisonFX_debug_enable", "0"); });
}
void Eclipse(int now)   // eclipse_watcher
{
    s_eclipse = true; TimeTravel(now);
    ExploderNum(402, true, now); ExploderNum(401, false, now);
    Fog(FOG_ECLIPSE);
    VisionApply("zombie_temple_eclipse", 3, 0.1f);
    if (s_inCave) { VisionApply("zombie_temple_caves", 2, 2.0f); VisionApply("zombie_temple_eclipseCave", 3, 2.0f); }
    SetDvar("r_lightTweakSunLight", "11");   // SetSunlight(0.5426, 0.6538, 0.7657): sin equivalente en el harness
    StrobeSky(1.0f, now);
    QSnd(0, -1, nullptr, "evt_sq_gen_transition"); QSnd(1, 1, nullptr, "evt_sq_gen_bg", 2);   // zombie_temple_amb.csc waitfor_ec
    fprintf(stderr, "[H19] %s: eclipse (ec) t=%d\n", kName, now);
}
void Daybreak(int now)   // daybreak_watcher
{
    const bool first = s_firstSky;
    s_eclipse = false; TimeTravel(now);
    ExploderNum(401, true, now); ExploderNum(402, false, now);
    VisionRemove("zombie_temple_eclipse", 2.0f); VisionRemove("zombie_temple_eclipseCave", 2.0f);
    Fog(s_inCave ? FOG_CAVE_ON : FOG_CAVE_OFF);
    SetDvar("r_lightTweakSunLight", "13");
    if (s_firstSky) { s_firstSky = false; SetDvarF("r_skyTransition", 0); } else StrobeSky(0.0f, now);
    if (!first) { QSnd(0, -1, nullptr, "evt_sq_gen_transition"); QSnd(2, 1, nullptr, nullptr, 2); }   // waitfor_return
    fprintf(stderr, "[H19] %s: amanecer (db)%s t=%d\n", kName, first ? " inicial" : "", now);
}

// ---- disparadores: cueva y cascada
void TriggersFrame()
{
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return;
    for (Trig &t : s_trigs) {
        const bool in = Touching(t, pl->r.currentOrigin, pl->r.mins, pl->r.maxs); if (in == t.inside) continue;
        t.inside = in;
        if (t.tn == "cave_vision_trig") {
            s_inCave = in;
            if (in) { VisionApply("zombie_temple_caves", 2, 2.0f); if (s_eclipse) VisionApply("zombie_temple_eclipseCave", 3, 2.0f); Fog(s_eclipse ? FOG_ECLIPSE : FOG_CAVE_ON); }   // cave_vision_on
            else { VisionRemove("zombie_temple_caves", 2.0f); if (s_eclipse) VisionRemove("zombie_temple_eclipseCave", 2.0f); Fog(s_eclipse ? FOG_ECLIPSE : FOG_CAVE_OFF); }   // cave_vision_off
        } else if (in) Fog(FOG_WATERFALL);   // waterfall_fog_trigger (el original la repite mientras sigue dentro; aqui al entrar)
    }
}

// ---- arranque
std::mutex s_evMu; std::vector<std::pair<std::string, std::string>> s_evQ;   // (sistema, estado); sistema "" = levelNotify
bool s_started;
void Init(const std::vector<H19Kv> &)
{
    ParseEffects("clientscripts/zombie_temple_fx.csc");
    for (auto &p : kNapalmFx) s_effect[p[0]] = p[1];   // _zombiemode_ai_napalm.csc napalm_fx()
    ParseCreateFx();
    LoadTrigs();
    for (int j : ModelsByTn("spear_trap_spear")) {   // spear_init: start = origen, moveDir = -AnglesToRight(angles)
        Spear s; s.j = j; s.assigned = false; float a[3]; H19_ModelGet(j, s.start, a); float f[3], r[3], u[3]; AngleVectors(a, f, r, u);
        for (int k = 0; k < 3; ++k) s.dir[k] = -r[k];
        s_spears.push_back(s);
    }
    for (int j : ModelsByTn("maze_trap_wall")) { Wall w; w.j = j; float a[3]; H19_ModelGet(j, w.start, a); w.activeZ = w.start[2] - 128.0f; w.assigned = w.active = false; s_walls.push_back(w); }
    for (int l = 1; l <= 4; ++l) {   // _init_pap_spinners / init_spinner
        char tn[16]; snprintf(tn, sizeof tn, "pap_spinner%d", l);
        for (int j : ModelsByTn(tn)) {
            Spinner s; s.j = j; s.list = l; s.forever = s.toStart = false; s.slot = 10 + (int)s_spin.size();
            float o[3], a[3]; H19_ModelGet(j, o, a); s.startYaw = a[1];
            a[0] = 0; a[1] = 90.0f * (l - 1) + RandRange(10, 80); a[2] = 0; H19_ModelPose(j, o, a);
            s_spin.push_back(s);
        }
    }
    size_t nexp = 0, nloop = 0; for (const CfxEnt &o : s_cfx) { nexp += o.exploder >= 0; nloop += o.loop; }
    int nt = 0; for (const Trig &t : s_trigs) nt += t.world;
    fprintf(stderr, "[H19] %s: %zu level._effect, createfx %zu (%zu en bucle, %zu exploders en %zu numeros), disparadores %zu (%d de mundo), "
        "lanzas %zu, paredes %zu, ruletas %zu, tablas %zu, ruedas %zu, generador %zu, niebla de cliente %s\n", kName, s_effect.size(), s_cfx.size(), nloop, nexp,
        s_exploderId.size(), s_trigs.size(), nt, s_spears.size(), s_walls.size(), s_spin.size(), ModelsByTn("plank_water").size(),
        ModelsByTn("water_wheel_left").size() + ModelsByTn("water_wheel_right").size(), ModelsByTn("power_generator").size(), H7C_ClientVolFog ? "si" : "NO");
}
void Start(int now)   // lo de main()/connect/spawn que no espera a nadie
{
    s_started = true;
    VisionPoll();
    Fog(FOG_CAVE_OFF);   // zombie_temple_fx.csc main: level thread cave_fog_off()
    VisionApply("zombie_temple", 1, 0.1f);   // temple_player_connect / temple_player_spawned
    VisionRemove("zombie_temple_caves", 2.0f);
    for (int j : ModelsByTn("plank_water")) {   // floating_boards_init / float_board: wait(RandomFloat(1)), board_bob y board_rotate
        After(now + (int)(RandRange(0, 1) * 1000.0f), [j](int t) {
            float o[3], a[3]; H19_ModelGet(j, o, a);
            const float dist = RandRange(2.5f, 3.0f), T = RandRange(3.5f, 4.5f);
            BoardBob(j, o[2] - dist, o[2] + dist, T, true, t);
            s_boardRot.push_back({ j, t, RandRange(60, 90), a[1], RandRange(-360, 360) });
        });
    }
}
void Notify(const std::string &ev, int now)
{
    if (ev == "ZPO") PowerOn(now);
    else if (ev == "wwr" || ev == "wwl") {   // watch_power_water
        const bool r = ev == "wwr"; for (int j : ModelsByTn(r ? "water_wheel_right" : "water_wheel_left")) WheelStep(j, 120, r ? 2.2f : 1.8f, true, now);
        fprintf(stderr, "[H19] %s: rueda de agua %s t=%d\n", kName, r ? "derecha" : "izquierda", now);
    }
    else if (ev == "ec") Eclipse(now);
    else if (ev == "db") Daybreak(now);
}
bool LevelNotify(const std::string &ev, int)
{
    std::lock_guard<std::mutex> g(s_evMu); s_evQ.push_back({ "", ev });
    return false;   // sigue a los mezcladores genericos (_lights.csc)
}
bool SysState(const char *sys, const std::string &st, int)
{
    if (strcmp(sys, "pap_indicator_spinners") && strcmp(sys, "@exploder")) return false;
    std::lock_guard<std::mutex> g(s_evMu); s_evQ.push_back({ sys, st });
    return true;
}
void Frame(int now)
{
    if (!s_started) Start(now);
    VisionPoll();
    CreateFxFrame();
    std::vector<std::pair<std::string, std::string>> q; { std::lock_guard<std::mutex> g(s_evMu); q.swap(s_evQ); }
    std::string last;
    for (auto &e : q) {
        if (e.first + "=" + e.second == last) continue;   // el gancho de h7e puede ir encadenado dos veces: mismo comando repetido
        last = e.first + "=" + e.second;
        if (e.first.empty()) Notify(e.second, now);
        else if (e.first == "@exploder") ExploderId(atoi(e.second.c_str()), now);
        else if (e.first == "pap_indicator_spinners") { fprintf(stderr, "[H19] %s: ruletas del PaP = %s t=%d\n", kName, e.second.c_str(), now); SetNumVisibleSpinners(atoi(e.second.c_str()), now); }
    }
    RunTasks(now);
    TriggersFrame();
    MoversFrame(now);
    ActorsFrame(now);
    MovesFrame(now);
    BoardsFrame(now);
}

// ===================================================================================================== sonido
#ifdef H6SND
std::map<int, int> s_slotFake;   // slot propio -> entidad falsa de h5_audio_cl
int s_torches, s_sndPlays;
int FakeOf(int slot, const float *org)
{
    auto it = s_slotFake.find(slot); if (it != s_slotFake.end()) return it->second;
    const int f = H19A_FakeAt(org); s_slotFake[slot] = f; return f;
}
void AudioStart()
{
    for (const CfxEnt &o : s_cfx) if (o.fxid == "fx_ztem_torch") { const int f = H19A_FakeAt(o.org); if (f) { H19A_PlayLoop(f, "amb_fire_medium", 0.5f); ++s_torches; } }   // snd_play_auto_fx
    fprintf(stderr, "[H19] %s sonido: %d antorchas con amb_fire_medium\n", kName, s_torches);
}
void AudioFrame(bool)
{
    std::vector<Snd> q; { std::lock_guard<std::mutex> g(s_sndMu); q.swap(s_sndQ); }
    for (Snd &s : q) {
        if (s.kind == 0) { static const float zero[3] = { 0, 0, 0 }; H19A_WorldSound(s.org[0] || s.org[1] || s.org[2] ? s.org : zero, s.alias.c_str()); ++s_sndPlays; }
        else if (s.kind == 1) { const int f = FakeOf(s.slot, s.org); if (f) { memcpy((float *)H19A_FakeOrg(f), s.org, 12); H19A_PlayLoop(f, s.alias.c_str(), s.fade); } }
        else if (s.kind == 2) { auto it = s_slotFake.find(s.slot); if (it != s_slotFake.end() && it->second) H19A_StopLoop(it->second, s.fade); }
        else if (s.kind == 3) { auto it = s_slotFake.find(s.slot); if (it != s_slotFake.end() && it->second) memcpy((float *)H19A_FakeOrg(it->second), s.org, 12); }   // el bucle sigue al actor
    }
}
void AudioStatus(std::string &out)
{
    char b[200]; snprintf(b, sizeof b, "; %s: antorchas %d, sonidos %d, napalm %d, shrieker %d, lanzas %d, paredes %d, exploders %d, ruletas %d",
        kName, s_torches, s_sndPlays, s_napalmSeen, s_sonicSeen, s_spearMoves, s_wallMoves, s_expOn, s_spinState);
    out += b;
}
#endif
} // namespace

extern const H19MapCl kH19_zombie_temple = {
    .zone = "zombie_temple", .offEnv = "H19_TEMPLE",
    .estado = "Shangri-La: vision por prioridades y cueva, nieblas, createfx con exploders 401/402, corriente (generador, ruedas, luces), ruletas del PaP, lanzas, laberinto, tablas, Napalm y Shrieker (OLA M7)",
    .init = Init, .levelNotify = LevelNotify, .sysState = SysState, .frame = Frame, .clockHand = nullptr, .screensFrame = nullptr,
    .levelEffect = nullptr,
#ifdef H6SND
    .audioStart = AudioStart, .audioNotify = nullptr, .audioSys = nullptr, .audioFrame = AudioFrame, .audioStatus = AudioStatus,
#endif
    .doorEnt = nullptr,
};
#endif
