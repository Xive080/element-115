// OLA M9: cliente de zombie_cosmodrome (Ascension). Reproduce sin VM de CSC lo que hacen sus clientscripts (rawfiles del ff:
// clientscripts/zombie_cosmodrome.csc, zombie_cosmodrome_fx.csc, zombie_cosmodrome_amb.csc y createfx/zombie_cosmodrome_fx.csc), con sus
// mismos numeros. El servidor (lander, centrifuga, cohete, PaP, trampas, monos) es el GSC original sin tocar.
//  - Perros: NO (como el original: nadie llama a enable_dog_rounds; level.dog_spawn_func se pone pero no hay rondas de perros). Ronda
//    especial: monos espaciales (_zombiemode_ai_monkey, roban ventajas; empiezan 1-3 rondas despues de corriente + primera ventaja y luego
//    cada 4-5). El gorila cosmico (_zombiemode_ai_ape.gsc) esta en el ff pero ningun script lo inicia (contenido cortado).
//  - vision (pila de _zombiemode.csc zombie_vision_set_apply/remove: gana la prioridad mas alta): nopower 1, powerUP 2, powerON 3,
//    monkey 4, begin 5, flare 6 (aterrizaje del mono), nopower 8 (grito de Samantha "sia"). Al conectar begin + nopower (0,1 s); "ZID"
//    quita begin en 8,5 s; "ZPO" powerUP (0,1 s) y al segundo powerON (2,5 s). El CS 1550 del servidor se sigue aplicando si cambia.
//  - niebla (setVolFog de set_fog): pila normal 1 / lander 2 (bandera 0 del jugador) / monkey 3 (monkey_start/monkey_stop); level_fog_init
//    al empezar (0 s) y setup_fog al segundo (normal, 5 s).
//  - createfx: oneshotfx, loopfx y exploders (5401 corriente, 5501-5503 luces del cohete, 5601/5701 PaP) leidos del rawfile; '@' del servidor.
//  - pantallas de la caja (model_cosmodrome_box_screens, box_indicator "n"/"f"/0..7) con sus modelos, 3 % de logo y el sonido de la tele.
//  - lander: puertas de las 4 estaciones (CW_/BE_/S_/CF_ + O/C, CWD/BED/SOD/CFD, LL, LG) con MoveTo 1 s a sus script_struct y
//    zmb_lander_door; luces de las puertas (L_R / L_G); pantallas del lander (ZPO -> _on; LAxx -> punto verde en la etiqueta de la
//    estacion; bandera 9 -> el punto viaja 10 s hacia LLxx); carteles del cohete (banderas 4/5/6 -> tras LG + 2 s parpadeo 3 x 0,35 s
//    a p_zom_rocket_sign_02..04); motores (bandera 1: tag_engine01-04 + tag_bellow, que siguen al lander), luz de estado (bandera 2) y
//    sonido de suelo zmb_lander_ground_sounds.
//  - cohete: bandera 0 = rocket_blast_trail en tag_engine; "RX" = lluvia de 10 escombros (rocket_explode_debris) con estela y polvo.
//  - centrifuga: bandera 11 = luces de aviso (tag_light_*_top), chispas (*_bttm) y vapor al segundo (tag_vent_*), pegadas a la centrifuga.
//  - monos: bandera 12 del lander de mono (whoosh, estela a los 2,5 s; al aterrizar polvo, zmb_ape_intro_land, MLO y MLF a los 0,5 s);
//    "MRS" zmb_ape_intro_sonicboom_fnt.
//  - trampas de fuego: "f11"/"f10" y "f21"/"f20" = fire_trap_cosmo en sus script_struct sin modelo.
//  - antenas de radar (RotateYaw 360 en 10 s sin fin) y level._effect["test_spin_fx"] para las luces giratorias de _lights.csc.
//  - sonido: snd_play_auto_fx de 11 tipos de FX, alarmas del gantry tras "power_on" (evt_alarm_a / evt_alarm_b_loop), minis/minie.
//  - OLA E3a: sacudidas de cliente (Earthquake de los .csc): lander_rumble_and_quake (bandera 7 del jugador: al subir una, luego cada
//    0,1 s segun la distancia al lander, y al aterrizar dos mas) y el grito de Samantha ("sia", 0,4 durante 10 s). Agujero negro (Gersh)
//    en maps/h19_weap_black_hole_bomb.cpp (comun con Moon); aqui solo su vision zombie_black_hole (prioridad 10, 2 s).
//    Validado en el Mac (docs/evidencia/e3a): lander y viajes, cohete y RX, PaP, centrifuga, radios, osos, munecas y la cadena de
//    Samantha 1-6 entera (recompensa Death Machine y "sia"). La "superficie lila" no es del render: es la camara dentro de un solido.
// No se hace (anotado en docs/evidencia/m9/LEEME.md): PlayRumbleOnEntity (sin
// vibracion), snd_set_snapshot del grito de Samantha, soul_pull de la bandera 0 de actor (level.nml_spark_pull no esta en los datos, el
// original tambien falla), barreras electricas de No Man's Land (nml_build_sparks no estan en los datos y el servidor no manda eb+),
// ojos de mono (level._effect["eye_glow"]: el port no pinta ojos por script) y los aviones/cohetes de fondo (comentados en el original).
//   H19_COSMO=0 lo apaga.
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
#include <xanim/xmodel_utils.h>
#include <xanim/dobj.h>
#include <EffectsCore/fx_system.h>
#include <EffectsCore/fx_unique_handle.h>
#include <qcommon/cm_trace.h>
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
#ifdef H6SND
#include <sound/snd_public_async.h>   // OLA F1: SND_SetSnapshot (setgroupsnapshot del CSC)
#include <sound/snd_bank.h>
#include <sound/snd_utils.h>
#endif

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
int H19_BhbInside();   // OLA E3a: maps/h19_weap_black_hole_bomb.cpp
void H5FX_Earthquake(float scale, float seconds, const float *org, float radius) __attribute__((weak));   // h5_fx.cpp: Earthquake() de cliente

namespace {
const char kName[] = "Ascension";
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
int RandInt(int n) { int r = (int)(H19_Rand01() * n); return r >= n ? n - 1 : r; }   // randomint(n)
int s_quakes;
void Quake(float scale, float sec, const float *org, float radius)   // player Earthquake() del CSC (OLA E3a; sin H5FX_Earthquake no hace nada)
{
    if (!H5FX_Earthquake || !org) return;
    H5FX_Earthquake(scale, sec, org, radius); ++s_quakes;
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
// hilos que esperan un level notify (level waittill("LL") / ("LG")): se despiertan todos a la vez
std::map<std::string, std::vector<std::function<void(int)>>> s_waiters;
void WaitFor(const char *ev, std::function<void(int)> fn) { s_waiters[ev].push_back(std::move(fn)); }
void Wake(const std::string &ev, int now)
{
    auto it = s_waiters.find(ev); if (it == s_waiters.end()) return;
    std::vector<std::function<void(int)>> w; w.swap(it->second);
    for (auto &f : w) f(now);
}

// ---- sonido pedido desde el hilo de render; lo reproduce AudioFrame (hilo de servidor). slot = entidad falsa propia (se crea alli)
struct Snd { int kind; int slot; float org[3]; std::string alias; float fade; };   // kind: 0 playsound en org, 1 bucle en slot, 2 parar bucle, 3 mover slot, 4 snapshot de mezcla (alias = nombre)
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
        if (lf == std::string::npos || (nl != std::string::npos && lf > nl)) continue;   // level._effect["x"] = level._effect["y"] (alias)
        if (Quoted(t, lf, path)) s_effect[key] = path;
    }
}
const FxEffectDef *FxDefOf(const char *key)
{
#ifdef H5FX
    auto it = s_effect.find(key); return it == s_effect.end() ? nullptr : FX_Register((char *)it->second.c_str());
#else
    (void)key; return nullptr;
#endif
}
unsigned Fx(const char *key, const float *org, const float *fwd, const float *up)   // PlayFX / SpawnFx+triggerfx con ejes
{
    auto it = s_effect.find(key); if (it == s_effect.end()) return 0;
    return H19_PlayFx(it->second.c_str(), org, fwd, up);
}
unsigned FxAng(const char *key, const float *org, const float *ang)
{
    auto it = s_effect.find(key); if (it == s_effect.end()) return 0;
    return H19_PlayFxAng(it->second.c_str(), org, ang);
}
unsigned FxDef(const char *key, const float *org)   // PlayFX sin direccion: angulos (270, 0, 0) como CScr_SetFxAngles sin ejes
{
    static const float def[3] = { 270, 0, 0 };
    return FxAng(key, org, def);
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
struct Prio { std::string name; int prio; };
// zombie_vision_set_apply / fog_apply: si ya esta, cambia la prioridad; gana la mas alta (estricta: en empate la primera)
const char *Top(const std::vector<Prio> &v, int *hp = nullptr)
{
    const char *best = nullptr; int hs = 0;
    for (const Prio &p : v) if (p.prio > hs) { hs = p.prio; best = p.name.c_str(); }
    if (hp) *hp = hs; return best;
}
void ListSet(std::vector<Prio> &v, const char *name, int prio) { for (Prio &p : v) if (p.name == name) { p.prio = prio; return; } v.push_back({ name, prio }); }
void ListDel(std::vector<Prio> &v, const char *name) { for (size_t i = 0; i < v.size(); ++i) if (v[i].name == name) { v.erase(v.begin() + i); return; } }
std::vector<Prio> s_vis;   // self._zombie_visionset_list
void VisionTop(float sec)
{
    int hs = 0; const char *best = Top(s_vis, &hs);
    if (!best || !H7_VisionSetNaked || !H19_EnvOn("H7E_VISION")) return;   // lista vacia: VisionSetNaked("undefined") no cambia nada
    fprintf(stderr, "[H19] %s: vision '%s' (prioridad %d) en %.1f s\n", kName, best, hs, sec);
    H7_VisionSetNaked(best, sec);
}
void VisionApply(const char *name, int prio, float sec = 1.0f) { ListSet(s_vis, name, prio); VisionTop(sec); }   // tiempo por defecto 1 s
void VisionRemove(const char *name, float sec = 1.0f) { ListDel(s_vis, name); VisionTop(sec); }

// ---- niebla: set_fog de zombie_cosmodrome.csc y level_fog_init de zombie_cosmodrome_fx.csc
// setVolFog(start, halfwayDist, halfwayHeight, baseHeight, r, g, b, colorScale, sunR, sunG, sunB, dirX, dirY, dirZ, startAng, endAng, time, maxOpacity)
const float kFogInit[18] = { 2013.52f, 2400.04f, 640.408f, 805, 0.054902f, 0.0862f, 0.0862f, 3.29585f, 0.341176f, 0.368627f, 0.388235f,
                             -0.492497f, 0.584479f, 0.64485f, 14.832f, 113.6f, 0, 1 };
const float kFogNormal[18] = { 2013.52f, 2400.04f, 640.408f, 805, 0.055f, 0.0862f, 0.0862f, 3.29585f, 0.341176f, 0.368627f, 0.388235f,
                               -0.492497f, 0.584479f, 0.64485f, 14.832f, 113.6f, 5, 1 };
const float kFogMonkey[18] = { 335.113f, 512.821f, 913.4f, 539.71f, 0.231373f, 0.176471f, 0.254902f, 3.57142f, 0.431373f, 0, 0,
                               0, 0, -1, 0, 118.502f, 3, 0.999887f };
const float kFogLander[18] = { 767.866f, 512.821f, 913.4f, 539.71f, 0.054902f, 0.0823529f, 0.0901961f, 3.29585f, 0.439216f, 0.466667f, 0.486275f,
                               -0.290644f, 0.728615f, 0.620199f, 0, 62.865f, 3, 1 };
void SetVolFog(const float *v, const char *nm)
{
    if (H7C_ClientVolFog) H7C_ClientVolFog(v, (int)(v[16] * 1000.0f));
    fprintf(stderr, "[H19] %s: niebla %s, %.0f s\n", kName, nm, v[16]);
}
std::vector<Prio> s_fog;   // self._zombie_fog_list
void FogTop()   // set_fog(get_fog_by_priority())
{
    const char *f = Top(s_fog); if (!f) return;
    if (!strcmp(f, "normal")) SetVolFog(kFogNormal, "normal");
    else if (!strcmp(f, "monkey")) SetVolFog(kFogMonkey, "monkey (ronda de monos)");
    else if (!strcmp(f, "lander")) SetVolFog(kFogLander, "lander");
}
void FogApply(const char *name, int prio) { ListSet(s_fog, name, prio); FogTop(); }
void FogRemove(const char *name) { ListDel(s_fog, name); FogTop(); }

// ---- createfx: oneshotfx, loopfx y exploders
struct CfxEnt { float org[3], ang[3], delay; std::string fxid; int exploder; bool loop; int state; unsigned h; int waitUntil; std::vector<unsigned> fx; };
std::vector<CfxEnt> s_cfx;
std::map<int, int> s_exploderId;   // level._exploder_ids[numero] = id (1, 2...)
int s_fxT0 = -1, s_fxPlayed, s_fxMissing, s_fxLoops, s_expOn;
void ParseCreateFx()
{
    const std::string t = Raw("clientscripts/createfx/zombie_cosmodrome_fx.csc");
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

// ---- movimiento de script_model de cliente (moveto / rotateto / rotateyaw)
float Prog(float t, float T, float a, float d)   // fraccion recorrida con perfil trapezoidal de velocidad (MoveTo/RotateTo con accel/decel)
{
    if (T <= 0 || t >= T) return 1.0f; if (t <= 0) return 0.0f;
    if (a + d > T) { const float s = T / (a + d); a *= s; d *= s; }
    const float v = 1.0f / (T - 0.5f * a - 0.5f * d);
    if (t < a) return 0.5f * v * t * t / a;
    if (t <= T - d) return v * (0.5f * a + (t - a));
    const float r = T - t; return 1.0f - 0.5f * v * r * r / d;
}
// un movimiento de origen y otro de angulos pueden ir a la vez (MoveTo y RotateTo de los escombros)
struct Move { bool on; int t0; float dur, acc, dec; float v0[3], dv[3]; std::function<void(int)> done; };
std::map<int, Move> s_mvOrg, s_mvAng;   // por indice de script_model de cliente (h7e)
void StartMove(std::map<int, Move> &mv, int j, const float *delta, float sec, float acc, float dec, int now, std::function<void(int)> done = nullptr)
{
    Move m; m.on = true; m.t0 = now; m.dur = sec; m.acc = acc; m.dec = dec;
    float o[3], a[3]; H19_ModelGet(j, o, a); memcpy(m.v0, &mv == &s_mvOrg ? o : a, 12);
    for (int k = 0; k < 3; ++k) m.dv[k] = delta ? delta[k] : 0;
    m.done = std::move(done); mv[j] = std::move(m);
}
void MoveTo(int j, const float *dst, float sec, int now, std::function<void(int)> done = nullptr)
{
    float o[3], a[3]; H19_ModelGet(j, o, a); const float d[3] = { dst[0] - o[0], dst[1] - o[1], dst[2] - o[2] };
    StartMove(s_mvOrg, j, d, sec, 0, 0, now, std::move(done));
}
void MovesFrame(int now)
{
    std::vector<std::function<void(int)>> fin;
    std::map<int, std::pair<bool, bool>> touched;
    for (int pass = 0; pass < 2; ++pass) {
        std::map<int, Move> &mv = pass ? s_mvAng : s_mvOrg;
        for (auto &kv : mv) {
            Move &m = kv.second; if (!m.on) continue;
            const float f = Prog((now - m.t0) / 1000.0f, m.dur, m.acc, m.dec);
            float o[3], a[3]; H19_ModelGet(kv.first, o, a);
            float *dst = pass ? a : o;
            for (int k = 0; k < 3; ++k) dst[k] = m.v0[k] + m.dv[k] * f;
            if (pass) for (int k = 0; k < 3; ++k) a[k] = fmodf(a[k], 360.0f);   // los angulos del script_mover se quedan en (-360, 360)
            H19_ModelPose(kv.first, o, a);
            if (f >= 1.0f) { m.on = false; if (m.done) fin.push_back(std::move(m.done)); }   // "movedone" / "rotatedone"
        }
    }
    for (auto &f : fin) f(now);
}
std::vector<int> ModelsByTn(const char *tn)   // GetEntArray(client, tn, "targetname") de los script_model de cliente
{
    std::vector<int> r; for (int j = 0; j < H19_ModelCount(); ++j) if (!strcmp(H19_ModelTn(j), tn)) r.push_back(j);
    return r;
}
const H19Kv *EntFor(int j)   // la entidad del mapa del script_model de cliente j (mismo targetname, modelo y origen)
{
    float o[3], a[3]; H19_ModelGet(j, o, a);
    for (const H19Kv &k : H19_MapEnts()) {
        const char *tn = H19_Str(k, "targetname"), *m = H19_Str(k, "model"); float eo[3];
        if (!tn || !m || strcmp(tn, H19_ModelTn(j)) || !H19_Vec(k, "origin", eo)) continue;
        if (fabsf(eo[0] - o[0]) < 1 && fabsf(eo[1] - o[1]) < 1 && fabsf(eo[2] - o[2]) < 1) return &k;
    }
    return nullptr;
}
const H19Kv *StructByTn(const char *tn)   // getstruct(tn, "targetname")
{
    if (!tn) return nullptr;
    for (const H19Kv &k : H19_MapEnts()) { const char *t = H19_Str(k, "targetname"), *c = H19_Str(k, "classname"); if (t && c && !strcmp(t, tn) && !strcmp(c, "script_struct")) return &k; }
    return nullptr;
}
// GetTagOrigin de un script_model de cliente: hueso de la pose base del XModel, en los ejes del modelo (el port no tiene DObj de cliente)
bool ClientTag(int j, const char *tag, float *out)
{
    float o[3], a[3]; if (!H19_ModelGet(j, o, a)) return false;
    memcpy(out, o, 12);
    XModel *xm = DB_FindXAssetHeader(ASSET_TYPE_XMODEL, (char *)H19_ModelName(j), 1, -1).model; if (!xm) return false;
    const unsigned nm = SL_FindLowercaseString(tag, SCRIPTINSTANCE_SERVER); unsigned char bi = 0xFF;
    if (!nm || !XModelGetBoneIndex(xm, nm, 0, &bi) || bi == 0xFF) return false;
    const DObjAnimMat *bp = XModelGetBasePose(xm); if (!bp) return false;
    float ax[3][3]; AnglesToAxis(a, ax);
    for (int k = 0; k < 3; ++k) out[k] = o[k] + bp[bi].trans[0] * ax[0][k] + bp[bi].trans[1] * ax[1][k] + bp[bi].trans[2] * ax[2][k];
    return true;
}
void Hide(int j) { float o[3], a[3]; H19_ModelGet(j, o, a); o[2] -= 100000.0f; H19_ModelPose(j, o, a); }   // Hide() / Delete(): fuera del mundo

// ---- efectos pegados (PlayFxOnTag): a una etiqueta de una entidad del servidor o a un script_model de cliente / punto que se mueve
struct Att { int ent; int use; std::string tag; int cm; float *pt; unsigned h; const FxEffectDef *def; };
std::vector<Att> s_att;
bool TagMatrix(gentity_s *e, const char *tag, float m[4][3])
{
    const unsigned t = SL_FindLowercaseString(tag, SCRIPTINSTANCE_SERVER);
    if (t && G_DObjGetWorldTagMatrix(e, t, m)) return true;
    AnglesToAxis(e->r.currentAngles, m); memcpy(m[3], e->r.currentOrigin, 12); return false;
}
bool AttPose(const Att &a, float m[4][3])
{
    if (a.ent >= 0) { gentity_s *e = &g_entities[a.ent]; if (!e->r.inuse || e->s.lerp.useCount != a.use) return false; TagMatrix(e, a.tag.c_str(), m); return true; }
    if (a.cm >= 0) { float o[3], an[3]; if (!H19_ModelGet(a.cm, o, an)) return false; AnglesToAxis(an, m); memcpy(m[3], o, 12); return true; }
    if (a.pt) { static const float id[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }; memcpy(m, id, 36); memcpy(m[3], a.pt, 12); return true; }
    return false;
}
unsigned Attach(const char *key, int ent, const char *tag, int cm = -1, float *pt = nullptr)
{
    Att a; a.ent = ent; a.use = ent >= 0 ? g_entities[ent].s.lerp.useCount : 0; a.tag = tag ? tag : "tag_origin"; a.cm = cm; a.pt = pt; a.def = FxDefOf(key);
    float m[4][3]; if (!AttPose(a, m)) return 0;
    a.h = Fx(key, m[3], m[0], m[2]); if (!a.h) return 0;
    s_att.push_back(a); return a.h;
}
void Detach(unsigned &h)   // StopFX / DeleteFX
{
    if (!h) return;
    for (size_t i = 0; i < s_att.size(); ++i) if (s_att[i].h == h) { s_att.erase(s_att.begin() + i); break; }
    H19_KillFx(h);
}
void AttFrame()   // el efecto sigue a su etiqueta: se escribe frameNow (como FX_UpdateEffectBolt)
{
#ifdef H5FX
    if (!fx_systemPool) return;
    for (size_t i = 0; i < s_att.size();) {
        Att &a = s_att[i]; float m[4][3];
        FxEffectContainer *c = UniqueHandleToEffect(&FX_GetSystem(0)->system, a.h);
        if (!c || c->effect.def != a.def || !AttPose(a, m)) { if (c && c->effect.def == a.def) H19_KillFx(a.h); s_att.erase(s_att.begin() + i); continue; }   // la entidad se fue: el efecto con ella
        float ax[3][3]; memcpy(ax, m, sizeof ax);
        memcpy(c->effect.frameNow.origin, m[3], 12); AxisToQuat(ax, c->effect.frameNow.quat);
        ++i;
    }
#endif
}

// ---- pantallas de la caja: init_cosmodrome_box_screens / cosmodrome_screen_switch / magic_box_screen_swap
const char *kTvTn = "model_cosmodrome_box_screens";
const std::vector<std::string> kTvOff = { "p_zom_monitor_csm_screen_off" }, kTvOn = { "p_zom_monitor_csm_screen_on" },
    kTvFsale = { "p_zom_monitor_csm_screen_fsale1", "p_zom_monitor_csm_screen_fsale2" }, kTvLogo = { "p_zom_monitor_csm_screen_logo" };
const std::vector<std::string> kTvLoc[8] = { { "p_zom_monitor_csm_screen_obsdeck" }, { "p_zom_monitor_csm_screen_labs" },
    { "p_zom_monitor_csm_screen_centrifuge" }, { "p_zom_monitor_csm_screen_enter" }, { "p_zom_monitor_csm_screen_storage" },
    { "p_zom_monitor_csm_screen_catwalk" }, { "p_zom_monitor_csm_screen_topack" }, { "p_zom_monitor_csm_screen_warehouse" } };
struct Tv { int j; const std::vector<std::string> *arr; int step, next; bool logo; };
std::vector<Tv> s_tv; bool s_tvInited, s_power;
int s_tvSwitches;
void TvSwitch(const std::string &state, int now)
{
    int t = now;
    if (!s_tvInited) { s_tvInited = true; for (int j : ModelsByTn(kTvTn)) { H19_SetModel(j, kTvOff[0].c_str()); s_tv.push_back({ j, nullptr, 0, -1, false }); t += 100; } }   // cosmodrome_tv_init
    const std::vector<std::string> *arr;
    if (state == "n") arr = s_power ? &kTvOn : &kTvOff;   // "n": sin corriente o caja moviendose (level._power_on)
    else if (state == "f") arr = &kTvFsale;
    else { const int k = atoi(state.c_str()); if (k < 0 || k > 7) return; arr = &kTvLoc[k]; }
    for (Tv &v : s_tv) { t += 200; v.arr = arr; v.step = 0; v.next = t; v.logo = false; }   // notify stop + wait 0.2 + thread magic_box_screen_swap
    const char *alias = state == "n" ? (s_power ? "amb_tv_static" : nullptr) : state == "f" ? "mus_fire_sale" : "amb_tv_static";   // play_magic_box_tv_audio
    for (size_t i = 0; i < s_tv.size(); ++i) { float o[3], a[3]; H19_ModelGet(s_tv[i].j, o, a); if (alias) QSnd(1, 20 + (int)i, o, alias, 0.5f); else QSnd(2, 20 + (int)i, o, nullptr, 0.5f); }
    ++s_tvSwitches;
    fprintf(stderr, "[H19] %s: box_indicator '%s' t=%d -> %s (%zu pantallas)\n", kName, state.c_str(), now, (*arr)[0].c_str(), s_tv.size());
}
void TvFrame(int now)
{
    for (Tv &s : s_tv) while (s.arr && s.next >= 0 && now >= s.next) {
        if (s.logo) { s.logo = false; s.next += 1000; continue; }   // tras el logo: wait 1.0
        if (s.step < (int)s.arr->size()) { H19_SetModel(s.j, (*s.arr)[s.step].c_str()); ++s.step; s.next += 3000; continue; }
        s.step = 0;
        if (3 > RandInt(100)) { H19_SetModel(s.j, kTvLogo[0].c_str()); s.logo = true; s.next += 2000; }   // 3 > RandomInt(100)
        else s.next += 1000;
    }
}

// ---- lander: puertas de las estaciones (open_lander_bay_doors / _only / close_lander_bay_doors)
struct Door { int j; std::string tn; bool cap; float open[3], start[3]; };
std::vector<Door> s_doors;
int s_doorMoves;
void DoorsMove(const char *tn, bool caps, bool toOpen, int now)   // las de ese nombre: caps = solo las de script_noteworthy (tapa del pozo)
{
    bool snd = false;
    for (Door &d : s_doors) {
        if (d.tn != tn || d.cap != caps) continue;
        MoveTo(d.j, toOpen ? d.open : d.start, 1.0f, now); ++s_doorMoves;
        if (!snd) { snd = true; float o[3], a[3]; H19_ModelGet(d.j, o, a); QSnd(0, -1, o, "zmb_lander_door"); }
    }
}
void DoorsOpen(const std::string &tn, int now)   // open_lander_bay_doors: puertas a start, y con "LL" la tapa a open
{
    DoorsMove(tn.c_str(), false, false, now);
    WaitFor("LL", [tn](int t) { DoorsMove(tn.c_str(), true, true, t); });
}
void DoorsClose(const std::string &tn, int now)   // close_lander_bay_doors: tapa a start, y con "LG" las puertas a start
{
    DoorsMove(tn.c_str(), true, false, now);
    WaitFor("LG", [tn](int t) { DoorsMove(tn.c_str(), false, false, t); });
}
struct Station { const char *o, *c, *only, *tn; bool waitC; };
Station s_st[4] = { { "CW_O", "CW_C", "CWD", "catwalk_zip_door", false }, { "BE_O", "BE_C", "BED", "base_entry_zip_door", false },
                    { "S_O", "S_C", "SOD", "storage_zip_door", false }, { "CF_O", "CF_C", "CFD", "centrifuge_zip_door", false } };
// toggle_lander_lights: L_R / L_G en las puertas p_zom_lander_doors_right/left (PlayFxOnTag tag_origin: siguen a la puerta)
std::map<int, unsigned> s_doorFx;
void DockLights(bool red, int)
{
    int n = 0;
    for (Door &d : s_doors) {
        const char *m = H19_ModelName(d.j); const bool r = !strcmp(m, "p_zom_lander_doors_right"), l = !strcmp(m, "p_zom_lander_doors_left");
        if (!r && !l) continue;
        Detach(s_doorFx[d.j]);
        s_doorFx[d.j] = Attach(r ? (red ? "lander_red_right" : "lander_green_right") : (red ? "lander_red_left" : "lander_green_left"), -1, nullptr, d.j);
        ++n;
    }
    fprintf(stderr, "[H19] %s: luces de las estaciones del lander en %s (%d puertas)\n", kName, red ? "ROJO" : "VERDE", n);
}

// ---- pantallas del lander (lander_screens) y punto que viaja (lander_at_station / lander_station_move_lander_marker)
struct LScreen { int j; unsigned panelFx, landerFx; float loc[3]; bool hasLoc, hasEnt; float ent[3], from[3], to[3]; int t0; };
std::vector<LScreen> s_lscr;
std::string s_landerDest;
const char *StationTag(const std::string &st) { return st == "baseentry" || st == "base" ? "tag_location_3" : st == "storage" ? "tag_location_1" : st == "catwalk" ? "tag_location_2" : "tag_home"; }
void LanderAtStation(const std::string &st, int)
{
    for (LScreen &s : s_lscr) {
        Detach(s.panelFx); Detach(s.landerFx);
        float o[3]; ClientTag(s.j, StationTag(st), o);
        const float up[3] = { 0, 0, 1 }, fw[3] = { 1, 0, 0 };
        s.panelFx = Fx("panel_green", o, fw, up); memcpy(s.loc, o, 12); s.hasLoc = true;
    }
    fprintf(stderr, "[H19] %s: lander en la estacion %s (%zu pantallas)\n", kName, st.c_str(), s_lscr.size());
}
void LanderMarker(int now)   // bandera 9: el punto verde va de la estacion actual a la de destino en 10 s
{
    for (LScreen &s : s_lscr) {
        Detach(s.landerFx); Detach(s.panelFx);
        if (!s.hasEnt) { if (!s.hasLoc) continue; s.hasEnt = true; memcpy(s.ent, s.loc, 12); }
        s.landerFx = Attach("panel_green", -1, nullptr, -1, s.ent);
        if (s_landerDest.empty()) continue;   // level.lander_dest_station sin definir: en el original MoveTo(undefined) no mueve
        memcpy(s.from, s.ent, 12); ClientTag(s.j, StationTag(s_landerDest), s.to); s.t0 = now;
    }
    fprintf(stderr, "[H19] %s: punto del lander viajando hacia %s t=%d\n", kName, s_landerDest.empty() ? "(sin destino)" : s_landerDest.c_str(), now);
}
void LScreensFrame(int now)
{
    for (LScreen &s : s_lscr) if (s.hasEnt && s.t0 >= 0) {
        const float f = Prog((now - s.t0) / 1000.0f, 10.0f, 0, 0);
        for (int k = 0; k < 3; ++k) s.ent[k] = s.from[k] + (s.to[k] - s.from[k]) * f;
        if (f >= 1) s.t0 = -1;
    }
}

// ---- carteles del cohete (rocket_launch_display / update_rocket_display)
int s_rocketNum;
void RocketDisplay()
{
    WaitFor("LG", [](int t) {
        After(t + 2000, [](int t2) {
            std::vector<int> v = ModelsByTn("rocket_launch_sign"); if (v.empty()) return;
            std::string model = H19_ModelName(v[0]);
            if (s_rocketNum == 1) model = "p_zom_rocket_sign_02"; else if (s_rocketNum == 2) model = "p_zom_rocket_sign_03"; else if (s_rocketNum == 3) model = "p_zom_rocket_sign_04";
            for (int j : v) {
                const std::string old = H19_ModelName(j);
                for (int i = 0; i < 3; ++i) { After(t2 + i * 700, [j, model](int) { H19_SetModel(j, model.c_str()); }); After(t2 + i * 700 + 350, [j, old](int) { H19_SetModel(j, old.c_str()); }); }
                After(t2 + 2100, [j, model](int) { H19_SetModel(j, model.c_str()); });
            }
            fprintf(stderr, "[H19] %s: carteles del cohete -> %s (cohete %d) t=%d\n", kName, model.c_str(), s_rocketNum, t2);
        });
    });
}

// ---- antenas de radar (radar_dish_rotate: wait 0.1 y RotateYaw(360, 10) sin fin)
void DishSpin(int j, int now) { const float d[3] = { 0, 360, 0 }; StartMove(s_mvAng, j, d, 10.0f, 0, 0, now, [j](int t) { DishSpin(j, t); }); }

// ---- escombros del cohete (init_rocket_debris / rain_debris / debris_crash_and_burn)
std::vector<std::string> s_debrisModels;
int s_debrisN;
bool GroundBelow(const float *p, float *out)   // bullettrace(point, point - (0,0,10000)): solo mundo
{
    trace_t tr; memset(&tr, 0, sizeof tr); const float z[3] = { 0, 0, 0 }; const float b[3] = { p[0], p[1], p[2] - 10000.0f };
    col_context_t ctx(1); CM_BoxTrace(&tr, p, b, z, z, 1, &ctx);
    for (int k = 0; k < 3; ++k) out[k] = p[k] + (b[k] - p[k]) * tr.fraction;
    return tr.fraction < 1.0f;
}
void DebrisOne(int now)
{
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client || s_debrisModels.empty()) return;
    const float *po = pl->r.currentOrigin; float ang[3] = { 0, pl->client->ps.viewangles[1], 0 }, f[3], r[3], u[3];
    AngleVectors(pl->client->ps.viewangles, f, r, u);
    const float fd = (float)(1000 + RandInt(2500)); const float sd = (float)(50 + RandInt(950)) * (RandInt(100) > 50 ? 1.0f : -1.0f);   // get_random_spot_in_player_view
    float p[3]; for (int k = 0; k < 3; ++k) p[k] = po[k] + f[k] * fd + r[k] * sd;
    float spot[3]; GroundBelow(p, spot);
    const float start[3] = { spot[0], spot[1], spot[2] + 10000.0f };
    float a0[3] = { (float)RandInt(360), (float)RandInt(360), (float)RandInt(360) }; (void)ang;
    const int j = H19_AddModel(s_debrisModels[RandInt((int)s_debrisModels.size())].c_str(), start, a0, "rocket_debris_fall"); if (j < 0) return;
    ++s_debrisN;
    unsigned trail = Attach("debris_trail", -1, nullptr, j);
    MoveTo(j, spot, 3.1f, now);
    for (int i = 0; i < 10; ++i) After(now + i * 300, [j](int t) {   // rotateto(random, 0.3) cada 0,3 s
        float o[3], a[3]; H19_ModelGet(j, o, a); const float d[3] = { RandInt(360) - a[0], RandInt(360) - a[1], RandInt(360) - a[2] };
        StartMove(s_mvAng, j, d, 0.3f, 0, 0, t); });
    float sp[3]; memcpy(sp, spot, 12);
    After(now + 3000 + 3100, [j, sp, trail](int t) {   // wait(3.1) tras los giros: earthquake (no hay) + debris_hit; wait(1) y delete
        FxDef("debris_hit", sp);
        After(t + 1000, [j, trail](int) { unsigned h = trail; Detach(h); Hide(j); });
    });
}
void RocketExplode(int now)   // "RX"
{
    for (int i = 0, t = now; i < 10; ++i) { After(t, [](int tt) { DebrisOne(tt); }); t += (int)(RandRange(0.5f, 1.5f) * 1000.0f); }
    fprintf(stderr, "[H19] %s: RX: el cohete explota, lluvia de escombros t=%d\n", kName, now);
}

// ---- trampas de fuego (trap_fx_monitor / trap_play_fx)
struct TrapPt { float org[3], ang[3]; std::string fx; };
struct Trap { const char *loc; std::vector<TrapPt> pts; std::vector<unsigned> fx; };
Trap s_traps[4] = { { "f1", {}, {} }, { "f2", {}, {} }, { "r1", {}, {} }, { "r2", {}, {} } };
const char *kTrapTn[4] = { "fire_trap_group1", "fire_trap_group2", "rocket_trap_group1", "rocket_trap_group2" };
int s_trapOn;
void TrapFx(Trap &tr, bool on, int now)
{
    for (unsigned &h : tr.fx) H19_KillFx(h); tr.fx.clear();
    if (!on) return;
    for (TrapPt &p : tr.pts) { float f[3], r[3], u[3]; AngleVectors(p.ang, f, r, u); tr.fx.push_back(Fx(p.fx.c_str(), p.org, f, u)); }
    ++s_trapOn;
    fprintf(stderr, "[H19] %s: trampa %s encendida (%zu FX) t=%d\n", kName, tr.loc, tr.pts.size(), now);
}

// ---- banderas de scriptmover, actor y jugador
struct LanderFx { unsigned eng[5], status, steam[2]; std::vector<unsigned> cfLights; unsigned monkeyTrail; bool grounds; };
std::map<int, LanderFx> s_ent;   // por entnum
std::map<int, unsigned> s_moverFl;
std::map<int, int> s_moverUse;
int s_flagEvents, s_landerQuake = -1, s_landerQuakeT;
void CentrifugeLightsOff(int n, int now)   // centrifuge_warning_lights_off: wait 0.2 y borra los modelos de las luces (y sus FX)
{
    std::vector<unsigned> v; v.swap(s_ent[n].cfLights);
    After(now + 200, [v](int) mutable { for (unsigned &h : v) Detach(h); });
}
void MoverFlag(int n, gentity_s *e, int bit, bool set, int now)
{
    LanderFx &L = s_ent[n]; ++s_flagEvents;
    switch (bit) {
    case 0: if (set) { Attach("rocket_blast_trail", n, "tag_engine"); fprintf(stderr, "[H19] %s: cohete (ent %d) despega t=%d\n", kName, n, now); } break;   // rocket_fx
    case 1:   // lander_engine_fx
        for (unsigned &h : L.eng) Detach(h);
        if (set) { static const char *tg[4] = { "tag_engine01", "tag_engine02", "tag_engine03", "tag_engine04" };
            for (int i = 0; i < 4; ++i) L.eng[i] = Attach("lunar_lander_thruster_leg", n, tg[i]);
            L.eng[4] = Attach("lunar_lander_thruster_bellow", n, "tag_bellow"); L.grounds = true; }
        fprintf(stderr, "[H19] %s: motores del lander (ent %d) %s t=%d\n", kName, n, set ? "ENCENDIDOS" : "apagados", now);
        break;
    case 2: Detach(L.status); L.status = Attach(set ? "lander_red" : "lander_green", n, "tag_origin"); break;   // lander_status_light
    case 3: Detach(L.status); L.status = Attach(set ? "panel_red" : "panel_green", n, "tag_home"); break;        // launch_panel_centrifuge_status
    case 4: case 5: case 6: if (set) { ++s_rocketNum; RocketDisplay(); } break;   // launch_panel_*_status
    case 7: {   // lander_rumble_and_quake (sin vibracion: PlayRumbleOnEntity no esta en el harness)
        gentity_s *pl = &g_entities[0];
        if (set) {
            if (pl->r.inuse) Quake(RandRange(0.2f, 0.3f), RandRange(2.0f, 2.5f), pl->r.currentOrigin, 150);   // al llamar al lander
            s_landerQuake = n; s_landerQuakeT = now;   // do_lander_rumble_quake: cada 0,1 s mientras vuela (QuakeFrame)
        } else {
            if (L.grounds) { L.grounds = false; QSnd(2, 40 + (n & 7), nullptr, nullptr, 2); }   // end_ground_sounds
            Quake(RandRange(0.3f, 0.4f), RandRange(0.5f, 0.6f), e->r.currentOrigin, 150);   // al aterrizar
            float o[3]; memcpy(o, e->r.currentOrigin, 12);
            After(now + 600, [o](int) { Quake(RandRange(0.1f, 0.2f), RandRange(0.2f, 0.3f), o, 150); });
            s_landerQuake = -1;   // level notify stop_lander_rumble
        }
        break; }
    case 9: if (set) LanderMarker(now); break;   // lander_move_fx
    case 11: {   // centrifuge_warning_lights_init: apaga las que haya y, con la bandera, luces arriba, chispas abajo y vapor al segundo
        CentrifugeLightsOff(n, now);
        if (!set) break;
        After(now + 200, [n](int t) {
            LanderFx &C = s_ent[n];
            for (const char *tg : { "tag_light_bk_top", "tag_light_fnt_top" }) C.cfLights.push_back(Attach("centrifuge_warning_light", n, tg));
            for (const char *tg : { "tag_light_bk_bttm", "tag_light_fnt_bttm" }) C.cfLights.push_back(Attach("centrifuge_light_spark", n, tg));
            After(t + 1000, [n](int) { for (const char *tg : { "tag_vent_bk_btm", "tag_vent_top_btm" }) Attach("centrifuge_start_steam", n, tg); });
            fprintf(stderr, "[H19] %s: centrifuga (ent %d): luces de aviso t=%d\n", kName, n, t);
        });
        break; }
    case 12:   // monkey_lander_fx
        if (set) {
            QSnd(0, -1, e->r.currentOrigin, "zmb_ape_intro_whoosh");
            After(now + 2500, [n](int) { LanderFx &M = s_ent[n]; Detach(M.monkeyTrail); M.monkeyTrail = Attach("monkey_trail", n, "tag_origin"); });
            fprintf(stderr, "[H19] %s: lander de mono (ent %d) cae t=%d\n", kName, n, now);
        } else {
            FxDef("monkey_spawn", e->r.currentOrigin); QSnd(0, -1, e->r.currentOrigin, "zmb_ape_intro_land");
            H19_LevelNotify("MLO", now); VisionApply("flare", 6, 0.5f);   // level notify MLO (monkey_land_on)
            After(now + 500, [](int t) { H19_LevelNotify("MLF", t); VisionRemove("flare", 3.0f); });
            After(now + 5000, [n](int) { Detach(s_ent[n].monkeyTrail); });   // monkey_lander_delay_fx_off
            fprintf(stderr, "[H19] %s: lander de mono (ent %d) aterriza t=%d\n", kName, n, now);
        }
        break;
    default: break;   // 8 = vibracion de la centrifuga (sin vibracion en el harness), 10 = bomba de agujero negro (comun)
    }
}
void QuakeFrame(int now)   // do_lander_rumble_quake
{
    if (s_landerQuake < 0 || now - s_landerQuakeT < 100) return;
    s_landerQuakeT = now;
    gentity_s *e = &g_entities[s_landerQuake], *pl = &g_entities[0];
    if (!e->r.inuse || !pl->r.inuse) return;
    float d2 = 0; for (int k = 0; k < 3; ++k) d2 += (pl->r.currentOrigin[k] - e->r.currentOrigin[k]) * (pl->r.currentOrigin[k] - e->r.currentOrigin[k]);
    if (d2 > 1500.0f * 1500.0f) return;
    if (d2 > 750.0f * 750.0f) Quake(RandRange(0.1f, 0.15f), RandRange(0.15f, 0.16f), e->r.currentOrigin, 1000);
    else Quake(RandRange(0.15f, 0.2f), RandRange(0.15f, 0.16f), e->r.currentOrigin, 750);
}
void LanderGroundSound(int n, gentity_s *e)   // start_ground_sounds: bucle en el suelo bajo tag_bellow + 30
{
    float m[4][3]; TagMatrix(e, "tag_bellow", m); float g[3];
    if (!GroundBelow(m[3], g)) return;
    g[2] += 30.0f; QSnd(1, 40 + (n & 7), g, "zmb_lander_ground_sounds", 3);
}
void MoversFrame(int now)
{
    for (int n = 0; n < level.num_entities; ++n) {
        gentity_s *e = &g_entities[n];
        if (!e->r.inuse || e->s.eType != ET_SCRIPTMOVER) { s_moverFl.erase(n); continue; }
        const unsigned fl = e->s.lerp.eFlags2 & 0x1BFFu;   // bits 0..12 menos el 10 (agujero negro, comun)
        auto it = s_moverFl.find(n);
        if (it != s_moverFl.end() && s_moverUse[n] != e->s.lerp.useCount) { s_ent.erase(n); it->second = 0; }   // entidad nueva en el mismo numero
        s_moverUse[n] = e->s.lerp.useCount;
        const unsigned prev = it == s_moverFl.end() ? 0 : it->second;
        if (fl != prev) {
            s_moverFl[n] = fl;
            for (int b = 0; b <= 12; ++b) if ((fl ^ prev) & (1u << b)) MoverFlag(n, e, b, (fl >> b) & 1, now);
        } else if (it == s_moverFl.end()) s_moverFl[n] = fl;
        auto L = s_ent.find(n);
        if (L != s_ent.end() && L->second.grounds && (now / 150) != ((now - 16) / 150)) LanderGroundSound(n, e);   // cada 0,15 s
    }
}
bool s_landerFog;
void PlayerFrame()   // bandera 0 del jugador: player_lander_fog
{
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return;
    const bool f = pl->client->ps.eFlags2 & 1;
    if (f == s_landerFog) return;
    s_landerFog = f;
    if (f) FogApply("lander", 2); else FogRemove("lander");
}

// ---- notificaciones del servidor
std::mutex s_evMu; std::vector<std::pair<std::string, std::string>> s_evQ;   // (sistema, estado); sistema "" = levelNotify
bool s_started;
void PowerOn(int now)   // cosmodrome_ZPO_listener, cosmodrome_power_vision_set_swap, ZPO_listener ("power_on") y alarmas del gantry
{
    if (s_power) return;
    s_power = true;
    for (int j : ModelsByTn("lander_screens")) H19_SetModel(j, "p_zom_cosmo_lunar_control_panel_dlc_on");   // setup_lander_screens
    VisionApply("zombie_cosmodrome_powerUP", 2, 0.1f);
    After(now + 1000, [](int) { VisionApply("zombie_cosmodrome_powerON", 3, 2.5f); });
    // zombie_cosmodrome_amb.csc alarm_a_timer / alarm_b_timer (con "power_on")
    std::vector<std::vector<float>> sir, bell;
    for (const H19Kv &k : H19_MapEnts()) { const char *tn = H19_Str(k, "targetname"); float o[3]; if (!tn || !H19_Vec(k, "origin", o)) continue;
        if (!strcmp(tn, "amb_warning_siren")) sir.push_back({ o[0], o[1], o[2] }); else if (!strcmp(tn, "amb_warning_bell")) bell.push_back({ o[0], o[1], o[2] }); }
    for (int t = now + 2500; t < now + 2500 + 21000; t += 1100)   // play_alarm_a: playsound cada 1,1 s hasta alarm_a_Off (21 s)
        After(t, [sir](int) { for (auto &o : sir) QSnd(0, -1, o.data(), "evt_alarm_a"); });
    After(now + 8500, [bell](int t) {   // play_alarm_b: bucle 8,8 s, final y para en 0,6 s
        for (size_t i = 0; i < bell.size(); ++i) QSnd(1, 60 + (int)i, bell[i].data(), "evt_alarm_b_loop", 0.8f);
        After(t + 8800, [bell](int t2) { for (auto &o : bell) QSnd(0, -1, o.data(), "evt_alarm_b_end");
            After(t2 + 100, [bell](int) { for (size_t i = 0; i < bell.size(); ++i) QSnd(2, 60 + (int)i, nullptr, nullptr, 0.6f); }); });
    });
    fprintf(stderr, "[H19] %s: corriente (ZPO): visiones powerUP/powerON, %zu pantallas del lander, %zu sirenas y %zu campanas t=%d\n", kName,
        ModelsByTn("lander_screens").size(), sir.size(), bell.size(), now);
}
void Notify(const std::string &ev, int now)
{
    Wake(ev, now);   // hilos en level waittill (LL, LG)
    if (ev == "ZPO") PowerOn(now);
    else if (ev == "ZID") VisionRemove("zombie_cosmodrome_begin", 8.5f);   // cosmodrome_first_vision_set
    else if (ev == "monkey_start") { VisionApply("zombie_cosmodrome_monkey", 4); FogApply("monkey", 3); fprintf(stderr, "[H19] %s: ronda de monos t=%d\n", kName, now); }
    else if (ev == "monkey_stop") { VisionRemove("zombie_cosmodrome_monkey"); FogRemove("monkey"); fprintf(stderr, "[H19] %s: fin de la ronda de monos t=%d\n", kName, now); }
    else if (ev == "MRS") QSnd(0, -1, nullptr, "zmb_ape_intro_sonicboom_fnt");   // cosmodrome_monkey_round_start_listener (sin sacudida)
    else if (ev == "sia") {   // samantha_is_angry_earthquake_and_rumbles
        QSnd(4, 0, nullptr, "zmb_samantha_scream");   // snd_set_snapshot("zmb_samantha_scream") -> setgroupsnapshot (OLA F1)
        if (g_entities[0].r.inuse) Quake(0.4f, 10.0f, g_entities[0].r.currentOrigin, 150);   // self Earthquake(.4, 10, self.origin, 150)
        // do_that_sam_rumble: PlayRumbleOnEntity(0, "damage_heavy") cada 0,1 s durante 4 s. No-op: el Mac no tiene vibracion y el arnes no
        // lleva rumble de mando (en el Fold seria el Vibrator del sistema; no se ha pedido)
        fprintf(stderr, "[H19] %s: Samantha enfadada (sia): sacudida 10 s, vision nopower y snapshot zmb_samantha_scream 6 s (rumble no-op) t=%d\n", kName, now);
        VisionApply("zombie_cosmodrome_nopower", 8);
        After(now + 6000, [](int) { VisionRemove("zombie_cosmodrome_nopower", 3.0f); QSnd(4, 0, nullptr, "default"); });   // wait(6); snd_set_snapshot("default")
    }
    else if (ev == "RX") RocketExplode(now);
    else if (ev == "L_R" || ev == "L_G") DockLights(ev == "L_R", now);
    else if (ev == "LACF" || ev == "LABE" || ev == "LASS" || ev == "LACW") LanderAtStation(ev == "LACF" ? "centrifuge" : ev == "LABE" ? "baseentry" : ev == "LASS" ? "storage" : "catwalk", now);
    else if (ev == "LLCF" || ev == "LLBE" || ev == "LLSS" || ev == "LLCW") s_landerDest = ev == "LLCF" ? "centrifuge" : ev == "LLBE" ? "base" : ev == "LLSS" ? "storage" : "catwalk";
    else if (ev == "minis") QSnd(1, 2, nullptr, "zmb_insta_kill_loop");   // play_minigun_loop
    else if (ev == "minie") { QSnd(0, -1, nullptr, "zmb_insta_kill"); QSnd(2, 2, nullptr, nullptr, 0.5f); }
    for (Station &s : s_st) {   // catwalk_lander_doors y compania: espera O, luego C; *_doors_only con su notify
        if (ev == s.only) {   // open_lander_bay_doors_only: puertas (sin tapa) a open
            bool snd = false;
            for (Door &d : s_doors) if (d.tn == s.tn && !d.cap) { MoveTo(d.j, d.open, 1.0f, now); ++s_doorMoves;
                if (!snd) { snd = true; float o[3], a[3]; H19_ModelGet(d.j, o, a); QSnd(0, -1, o, "zmb_lander_door"); } }
        }
        else if (!s.waitC && ev == s.o) { s.waitC = true; DoorsOpen(s.tn, now); }
        else if (s.waitC && ev == s.c) { s.waitC = false; DoorsClose(s.tn, now); }
    }
    if (ev.size() == 3 && (ev[0] == 'f' || ev[0] == 'r') && (ev[2] == '0' || ev[2] == '1'))   // trap_fx_monitor: <loc>1 enciende, <loc>0 apaga
        for (Trap &t : s_traps) if (!ev.compare(0, 2, t.loc)) TrapFx(t, ev[2] == '1', now);
}

// ---- arranque
void Init(const std::vector<H19Kv> &)
{
    ParseEffects("clientscripts/zombie_cosmodrome_fx.csc");
    ParseCreateFx();
    for (int j : ModelsByTn("lander_screens")) { LScreen s{}; s.j = j; s.t0 = -1; s_lscr.push_back(s); }
    for (const char *tn : { "catwalk_zip_door", "base_entry_zip_door", "storage_zip_door", "centrifuge_zip_door" })
        for (int j : ModelsByTn(tn)) {
            const H19Kv *k = EntFor(j); if (!k) continue;
            const H19Kv *op = StructByTn(H19_Str(*k, "target")); const H19Kv *st = op ? StructByTn(H19_Str(*op, "target")) : nullptr;
            Door d; d.j = j; d.tn = tn; d.cap = H19_Str(*k, "script_noteworthy") != nullptr;
            if (!op || !st || !H19_Vec(*op, "origin", d.open) || !H19_Vec(*st, "origin", d.start)) continue;
            s_doors.push_back(d);
        }
    for (int i = 0; i < 4; ++i)
        for (const H19Kv &k : H19_MapEnts()) {
            const char *tn = H19_Str(k, "targetname"), *cn = H19_Str(k, "classname");
            if (!tn || strcmp(tn, kTrapTn[i]) || !cn || strcmp(cn, "script_struct") || H19_Str(k, "model")) continue;   // sin los de script_model
            TrapPt p; memset(p.org, 0, 12); memset(p.ang, 0, 12); H19_Vec(k, "origin", p.org); H19_Vec(k, "angles", p.ang);
            const char *ss = H19_Str(k, "script_string"); p.fx = ss ? ss : "fire_trap_cosmo"; s_traps[i].pts.push_back(p);
        }
    for (int j : ModelsByTn("rocket_explode_debris")) { s_debrisModels.push_back(H19_ModelName(j)); Hide(j); }   // init_rocket_debris: hide()
    size_t nexp = 0, nloop = 0; for (const CfxEnt &o : s_cfx) { nexp += o.exploder >= 0; nloop += o.loop; }
    fprintf(stderr, "[H19] %s: %zu level._effect, createfx %zu (%zu en bucle, %zu exploders en %zu numeros), pantallas caja %zu, lander %zu, "
        "puertas %zu, carteles %zu, antenas %zu, escombros %zu, trampas %zu+%zu, niebla de cliente %s\n", kName, s_effect.size(), s_cfx.size(), nloop, nexp,
        s_exploderId.size(), ModelsByTn(kTvTn).size(), s_lscr.size(), s_doors.size(), ModelsByTn("rocket_launch_sign").size(),
        ModelsByTn("zombie_cosmodrome_radar_dish").size(), s_debrisModels.size(), s_traps[0].pts.size(), s_traps[1].pts.size(), H7C_ClientVolFog ? "si" : "NO");
}
void Start(int now)   // lo de main()/connect/spawn que no espera a nadie
{
    s_started = true;
    VisionPoll();
    SetVolFog(kFogInit, "inicial (level_fog_init)");
    VisionApply("zombie_cosmodrome_begin", 5, 0.1f); VisionApply("zombie_cosmodrome_nopower", 1, 0.1f);   // cosmodrome_first_vision_set
    VisionApply("zombie_cosmodrome_nopower", 1, 0);   // cosmodrome_vision_set (sin corriente)
    FogApply("normal", 1);   // cosmo_on_player_spawned
    After(now + 1000, [](int) { FogApply("normal", 1); });   // setup_fog: wait(1)
    for (int j : ModelsByTn("zombie_cosmodrome_radar_dish")) After(now + 100, [j](int t) { DishSpin(j, t); });
}
bool LevelNotify(const std::string &ev, int)
{
    std::lock_guard<std::mutex> g(s_evMu); s_evQ.push_back({ "", ev });
    return false;   // sigue a los mezcladores genericos (_lights.csc)
}
bool SysState(const char *sys, const std::string &st, int)
{
    if (strcmp(sys, "box_indicator") && strcmp(sys, "@exploder")) return false;
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
        else if (e.first == "box_indicator") TvSwitch(e.second, now);
    }
    RunTasks(now);
    {   // OLA E3a: black_hole_visionset_switch (maps/h19_weap_black_hole_bomb.cpp): zombie_black_hole prioridad 10, 2 s al entrar y al salir
        static int bh = 0; const int in = H19_BhbInside();
        if (in != bh) { bh = in; if (in) VisionApply("zombie_black_hole", 10, 2.0f); else VisionRemove("zombie_black_hole", 2.0f); }
    }
    MoversFrame(now);
    QuakeFrame(now);
    PlayerFrame();
    TvFrame(now);
    LScreensFrame(now);
    MovesFrame(now);
    AttFrame();
}
const char *LevelEffect(const char *key) { auto it = s_effect.find(key); return it == s_effect.end() ? nullptr : it->second.c_str(); }   // test_spin_fx

// ===================================================================================================== sonido
#ifdef H6SND
std::map<int, int> s_slotFake;   // slot propio -> entidad falsa de h5_audio_cl
int s_autoFx, s_sndPlays;
int FakeOf(int slot, const float *org)
{
    auto it = s_slotFake.find(slot); if (it != s_slotFake.end()) return it->second;
    const int f = H19A_FakeAt(org); s_slotFake[slot] = f; return f;
}
void AudioStart()
{
    static const char *kAuto[][2] = { { "fx_fire_line_xsm", "amb_fire_medium" }, { "fx_fire_line_sm", "amb_fire_large" }, { "fx_fire_wall_back_sm", "amb_fire_large" },
        { "fx_fire_destruction_lg", "amb_fire_extreme" }, { "fx_zmb_fire_sm_smolder", "amb_fire_medium" }, { "fx_elec_terminal", "amb_break_arc" },
        { "fx_zmb_elec_terminal_bridge", "amb_break_arc" }, { "fx_zmb_pipe_steam_md", "amb_steam_medium" }, { "fx_zmb_pipe_steam_md_runner", "amb_steam_medium" },
        { "fx_zmb_steam_hallway_md", "amb_steam_medium" }, { "fx_zmb_water_spray_leak_sm", "amb_water_spray_small" } };   // spawn_fx_loopers
    for (const CfxEnt &o : s_cfx) for (auto &a : kAuto) if (o.fxid == a[0]) { const int f = H19A_FakeAt(o.org); if (f) { H19A_PlayLoop(f, a[1], 0.5f); ++s_autoFx; } }   // snd_play_auto_fx
    fprintf(stderr, "[H19] %s sonido: %d bucles de snd_play_auto_fx\n", kName, s_autoFx);
}
void AudioFrame(bool)
{
    std::vector<Snd> q; { std::lock_guard<std::mutex> g(s_sndMu); q.swap(s_sndQ); }
    for (Snd &s : q) {
        if (s.kind == 0) { static const float zero[3] = { 0, 0, 0 }; H19A_WorldSound(s.org[0] || s.org[1] || s.org[2] ? s.org : zero, s.alias.c_str()); ++s_sndPlays; }
        else if (s.kind == 1) { const int f = FakeOf(s.slot, s.org); if (f) { memcpy((float *)H19A_FakeOrg(f), s.org, 12); if (!H19A_FakeLoopIs(f, s.alias.c_str())) H19A_PlayLoop(f, s.alias.c_str(), s.fade); } }
        else if (s.kind == 2) { auto it = s_slotFake.find(s.slot); if (it != s_slotFake.end() && it->second) H19A_StopLoop(it->second, s.fade); }
        else if (s.kind == 3) { auto it = s_slotFake.find(s.slot); if (it != s_slotFake.end() && it->second) memcpy((float *)H19A_FakeOrg(it->second), s.org, 12); }
        else if (s.kind == 4) {   // OLA F1: snd_set_snapshot -> snd_snapshot_think -> setgroupsnapshot = CScr_SetGroupSnapshot (cg_scr_main.cpp)
            SND_SetSnapshot(SND_SNAPSHOT_SCRIPT, s.alias.c_str(), 0.0f, 1.0f);
            fprintf(stderr, "[H19] %s sonido: snapshot de script '%s'%s\n", kName, s.alias.c_str(), SND_GetSnapshotById(SND_HashName(s.alias.c_str())) ? "" : " (NO esta en los datos: queda default)");
        }
    }
}
void AudioStatus(std::string &out)
{
    char b[320]; snprintf(b, sizeof b, "; %s: bucles auto %d, sonidos %d, puertas %d, banderas %d, exploders %d, trampas %d, pantallas %d, escombros %d, cohete %d, sacudidas %d",
        kName, s_autoFx, s_sndPlays, s_doorMoves, s_flagEvents, s_expOn, s_trapOn, s_tvSwitches, s_debrisN, s_rocketNum, s_quakes);
    out += b;
}
#endif
} // namespace

extern const H19MapCl kH19_zombie_cosmodrome = {
    .zone = "zombie_cosmodrome", .offEnv = "H19_COSMO",
    .estado = "Ascension: vision y niebla por prioridades, createfx con exploders, pantallas de la caja, lander (puertas, luces, pantallas, motores), cohete, centrifuga, monos, trampas de fuego y antenas (OLA M9)",
    .init = Init, .levelNotify = LevelNotify, .sysState = SysState, .frame = Frame, .clockHand = nullptr, .screensFrame = nullptr,
    .levelEffect = LevelEffect,
#ifdef H6SND
    .audioStart = AudioStart, .audioNotify = nullptr, .audioSys = nullptr, .audioFrame = AudioFrame, .audioStatus = AudioStatus,
#endif
    .doorEnt = nullptr,
};
#endif
