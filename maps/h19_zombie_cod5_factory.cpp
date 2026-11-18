// OLA M4: cliente de zombie_cod5_factory (Der Riese). Reproduce sin VM de CSC lo que hacen sus clientscripts (rawfiles del ff), con
// sus mismos numeros. El servidor (generadores, teletransportadores, PaP, trampas, caja que se mueve, perros) es el GSC original.
//  - zombie_cod5_factory.csc: factory_ZPO_listener ("ZPO" -> notify local "pl1") y el tipo de luz "light_electric"
//    (triggered_lights_think: con "pl1" set_light_intensity(script_float o 1,5); en el BSP vienen a 0,001 = apagadas hasta la corriente).
//  - zombie_cod5_factory_fx.csc: level_fog_init + dog_start_monitor / dog_stop_monitor (setVolFog, 7 s; h7c_gfx.cpp),
//    trap_fx_monitor (warehouse_trap / wuen_trap / bridge_trap: FX "zapper" = misc/fx_zombie_electric_trap en sus script_struct;
//    "<lado>off" los quita), light_model_swap (con "pl1": smodel_light_electric -> lights_indlight_on, _milit ->
//    lights_milit_lamp_single_int_on, _tinhatlamp -> lights_tinhatlamp_on), teleporter_map_light (sm_light_tp_0..2: con "pl1" roja +
//    FX zapper_light_notready, con "t01"/"t11"/"t21" verde + zapper_light_ready; angulos del map_fx_guide_struct mas cercano),
//    teleporter_map_light_receiver (sm_light_tp_r: roja con "pl1", parpadeo 0,5 s con "TRf" hasta "TRs", verde 1,5 s despues de "pap1"),
//    perk_wire_fx ("pw0".."pw2": chispa maps/zombie/fx_zombie_wire_spark que recorre la cadena pad_N_wire a 0,1 s por tramo y al acabar
//    notifica "tN1" en local).
//  - zombie_cod5_factory_teleporter.csc: wait_for_black_box (vision "default" mientras dura) y wait_for_teleport_aftereffect ("tae":
//    uno al azar de fov / shellshock / shellshock_electric / bw / red / flashy / flare; dvar factoryAftereffectOverride).
//  - zombie_cod5_factory_amb.csc: power_audio_2d (power_down_2d a los 2 s, power_up_2d con "pl1"), start_lights (dyn_light:
//    light_start + bucle "light"; switch_progress por script_noteworthy 1..5 = 0,5..2,5 s; dyn_generator: a los 3 s switch_progress +
//    gen_start + bucle gen_loop; dyn_breakers: break_start + bucle break_loop), teleport_pad_init ("tp<N>": bucle power_loop en
//    telepad_N_looper; "tpw<N>": _warmup/_cooldown de cada script_sound de telepad_N y homepad), teleport_2d ("t2d"), megafonia pa_system
//    (pa_level_start, pa_power_on, cuenta atras "pac<N>" de 30 s con clock_tick_1sec y avisos 20/15/<=10, "scd<N>" exito, "tpc<N>"
//    fin de viaje, trampas "warehouse"/"wuen"/"bridge" con 48,5 s; pa_play_dialog no pisa una frase que aun suena), pole_fx_audio
//    (tras "scd<N>": FX zombie_elec_pole_terminal + pole_spark cada 2..6 s en pole_fx_N), homepad_loop y linkall_2d ("pap1"),
//    y el sonido de la chispa del cable (tele_spark_hit + bucle tele_spark_loop que sigue a la chispa).
// No se hace: flytrap_lev_objects ("ag1", objetos del huevo de pascua que flotan: el port no mueve script_model de cliente), zombify
// (zombie_grain; el GSC de este mapa no lo usa) ni la radio/fonografo de _waw_zombiemode_radio.
//   H19_FACTORY=0 lo apaga. H19_AE=n fija el efecto tras teletransporte (prueba).
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <game_mp/g_main_mp.h>
#include <gfx_d3d/r_bsp.h>
#include <qcommon/com_bsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "h19_mapcl.h"
#ifdef H6SND
#include <sound/snd_bank.h>
#include <sound/snd_public_async.h>
#endif

bool H7_VisionSetNaked(const char *name, float seconds) __attribute__((weak));   // h7_gfx.cpp
const char *H7_VisionNakedName() __attribute__((weak));                           // h7_gfx.cpp
void H7C_ClientVolFog(const float *v, int transitionMs) __attribute__((weak));     // h7c_gfx.cpp: setVolFog del CSC
GfxLight *H5L_Lights() __attribute__((weak));                                      // h5_light.cpp: luces primarias del render
int H7E_ClientModelByTn(const char *tn, int ordinal, const char *name);             // h7e_light.cpp: SetModel en los script_model de cliente con ese targetname
unsigned int __cdecl Com_GetPrimaryLightCount();                                    // cgame_mp/cg_ents_mp.h
const ComPrimaryLight *__cdecl Com_GetPrimaryLight(unsigned int primaryLightIndex);

namespace {
struct Pt { float org[3], ang[3]; };
std::map<std::string, std::vector<Pt>> s_structs;                       // script_struct por targetname (getstructarray)
std::map<std::string, std::string> s_target;                            // targetname -> target del primero (cadenas getstruct(target))
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
std::vector<Pt> Chain(const char *first)   // perk_wire_fx_client / hover_spots: getstruct y sus target uno tras otro
{
    std::vector<Pt> out; std::string n = first;
    for (int guard = 0; guard < 256; ++guard) {
        auto it = s_structs.find(n); if (it == s_structs.end() || it->second.empty()) break;
        out.push_back(it->second[0]);
        auto t = s_target.find(n); if (t == s_target.end()) break; n = t->second;
    }
    return out;
}

// ===================================================================================================== cliente grafico
std::mutex s_sndMu; std::vector<std::pair<std::vector<float>, std::string>> s_sndQ;   // sonidos que pide el cliente grafico (pole_spark)
void QueueSound(const float *org, const char *alias) { std::lock_guard<std::mutex> g(s_sndMu); s_sndQ.push_back({ { org[0], org[1], org[2] }, alias }); }

bool s_power;
// light_electric (triggered_lights_think): indice de luz primaria, color normalizado (Byte4) e intensidad con corriente
struct ELight { int pl; float color[3]; float on; };
std::vector<ELight> s_elights;
void Byte4(const float *in, float *out) { for (int k = 0; k < 3; ++k) { int v = (int)(255.0f * in[k] + 9.313225746154785e-10f); v = v > 255 ? 255 : v < 0 ? 0 : v; out[k] = v / 255.0f; } }
void ElectricLightsOn()
{
    GfxLight *gl = H5L_Lights ? H5L_Lights() : nullptr; if (!gl) return;
    for (const ELight &l : s_elights) {   // CG_PrimaryLight: color = Byte4(color) * intensidad (lo mismo que Apply de h7e_light.cpp)
        if (l.pl <= 0 || (unsigned)l.pl >= Com_GetPrimaryLightCount()) continue;
        GfxLight &g = gl[l.pl];
        for (int k = 0; k < 3; ++k) g.color[k] = g.diffuseColor[k] = g.specularColor[k] = l.color[k] * l.on;
        g.diffuseColor[3] = g.specularColor[3] = 1.0f;
    }
}

// luces de los mapas de los teletransportadores (script_model de cliente sm_light_tp_*), con su FX
struct MapLamp { std::string tn; float org[3]; float fwd[3], up[3]; unsigned fx = 0; };
std::vector<MapLamp> s_lamps;
std::vector<Pt> s_guides;
void GuideAxes(const float *org, float *fwd, float *up)   // get_guide_struct_angles: el map_fx_guide_struct mas cercano
{
    float ang[3] = { 0, 0, 0 }, best = -1, rt[3];
    for (const Pt &g : s_guides) { const float d = Vec3DistanceSq(org, g.org); if (best < 0 || d < best) { best = d; memcpy(ang, g.ang, 12); } }
    AngleVectors(ang, fwd, rt, up);
}
void LampSet(const char *tn, int color /*0 apagada 1 verde 2 roja*/)
{
    H7E_ClientModelByTn(tn, -1, color == 1 ? "zombie_zapper_cagelight_green" : color == 2 ? "zombie_zapper_cagelight_red" : "zombie_zapper_cagelight");
    for (MapLamp &l : s_lamps) if (l.tn == tn) {
        H19_KillFx(l.fx);
        if (color) l.fx = H19_PlayFx(color == 1 ? "maps/zombie/fx_zombie_light_glow_green" : "maps/zombie/fx_zombie_light_glow_red", l.org, l.fwd, l.up);
    }
}
int s_rxFlashNext = -1; bool s_rxFlashRed; int s_rxGreenAt = -1; bool s_rxPap;   // teleporter_map_light_receiver(_flash/_stop)

// trampas electricas (trap_fx_monitor / electric_trap_fx)
struct Trap { std::string name, side; std::vector<Pt> pts; std::vector<unsigned> fx; };
std::vector<Trap> s_traps;

// chispa del cable de cada pad (perk_wire_fx_client)
struct Wire { std::vector<Pt> path; int step = -1, next = 0; unsigned fx = 0; };
Wire s_wire[3];

// postes con chispas tras enlazar (pole_fx_audio: el FX y el sonido van juntos)
struct Pole { Pt p; int next; };
std::vector<Pole> s_poles[3]; bool s_polesOn[3];

// efectos tras teletransporte (como Kino)
struct Step { int at; std::string vis; float sec; float fov; };
std::vector<Step> s_steps; std::string s_savedVis;
const char *Naked() { const char *n = H7_VisionNakedName ? H7_VisionNakedName() : nullptr; return n && *n ? n : "zombie_factory"; }

void Fog(int which, float sec)   // 0 = level_fog_init, 1 = dog_start_monitor, 2 = dog_stop_monitor
{
    if (!H7C_ClientVolFog) return;
    // setVolFog(start, halfwayDist, halfwayHeight, baseHeight, r, g, b, colorScale, sunR, sunG, sunB, dirX, dirY, dirZ, startAng, endAng, time, maxOpacity)
    static const float lv[18] = { 440, 3200, 225, 64, 0.219608f, 0.403922f, 0.686275f, 1, 0.0313726f, 0.0470588f, 0.0823529f, -0.1761f, 0.689918f, 0.702141f, 0, 0, 0, 1 };
    static const float dg[18] = { 229, 200, 380, 200, 0.0117647f, 0.0156863f, 0.0235294f, 5.5f, 0.0313726f, 0.0470588f, 0.0823529f, -0.1761f, 0.689918f, 0.702141f, 0, 49.8549f, 7, 1 };
    static const float st[18] = { 440, 3200, 225, 64, 0.533f, 0.717f, 1, 1, 0.0313726f, 0.0470588f, 0.0823529f, -0.1761f, 0.689918f, 0.702141f, 0, 0, 7, 1 };
    H7C_ClientVolFog(which == 1 ? dg : which == 2 ? st : lv, (int)(sec * 1000.0f));
}

void Init(const std::vector<H19Kv> &ents)
{
    LoadStructs();
    for (const H19Kv &k : ents) {
        const char *cn = H19_Str(k, "classname"), *tn = H19_Str(k, "targetname"); if (!cn || !tn) continue;
        if (!strcmp(cn, "light") && !strcmp(tn, "light_electric") && H19_Str(k, "pl#")) {
            ELight l; l.pl = atoi(H19_Str(k, "pl#")); if (l.pl <= 0 || (unsigned)l.pl >= Com_GetPrimaryLightCount()) continue;
            float nc[3]; ColorNormalize(Com_GetPrimaryLight(l.pl)->color, nc); Byte4(nc, l.color);   // CG_CreateLight: GetLightColor normalizado
            l.on = H19_Str(k, "script_float") ? (float)atof(H19_Str(k, "script_float")) : 1.5f;
            s_elights.push_back(l);
        }
        if (!strcmp(cn, "script_model") && !strncmp(tn, "sm_light_tp_", 12)) { MapLamp l; l.tn = tn; memset(l.org, 0, 12); H19_Vec(k, "origin", l.org); s_lamps.push_back(l); }
    }
    if (s_structs.count("map_fx_guide_struct")) s_guides = s_structs["map_fx_guide_struct"];
    for (MapLamp &l : s_lamps) GuideAxes(l.org, l.fwd, l.up);
    static const char *tr[3][2] = { { "warehouse_trap", "warehouse" }, { "wuen_trap", "wuen" }, { "bridge_trap", "bridge" } };
    for (auto &t : tr) { Trap x; x.name = t[0]; x.side = t[1]; if (s_structs.count(t[0])) x.pts = s_structs[t[0]]; x.fx.assign(x.pts.size(), 0); s_traps.push_back(x); }
    for (int i = 0; i < 3; ++i) {
        s_wire[i].path = Chain(("pad_" + std::to_string(i) + "_wire").c_str());
        for (const Pt &p : s_structs["pole_fx_" + std::to_string(i)]) s_poles[i].push_back({ p, 0 });
    }
    size_t np = 0; for (auto &t : s_traps) np += t.pts.size();
    Fog(0, 0);   // level_fog_init (time 0)
    fprintf(stderr, "[H19] Der Riese: %zu luces light_electric, %zu luces de mapa de teletransporte, %zu guias, trampas %zu (%zu puntos), cables %zu/%zu/%zu tramos, postes %zu/%zu/%zu, niebla de cliente %s\n",
        s_elights.size(), s_lamps.size(), s_guides.size(), s_traps.size(), np, s_wire[0].path.size(), s_wire[1].path.size(), s_wire[2].path.size(),
        s_poles[0].size(), s_poles[1].size(), s_poles[2].size(), H7C_ClientVolFog ? "si" : "NO");
}

void PowerOn(int now)   // "pl1"
{
    if (s_power) return;
    s_power = true;
    ElectricLightsOn();
    const int n0 = H7E_ClientModelByTn("smodel_light_electric", -1, "lights_indlight_on");
    const int n1 = H7E_ClientModelByTn("smodel_light_electric_milit", -1, "lights_milit_lamp_single_int_on");
    const int n2 = H7E_ClientModelByTn("smodel_light_electric_tinhatlamp", -1, "lights_tinhatlamp_on");
    for (const char *tn : { "sm_light_tp_0", "sm_light_tp_1", "sm_light_tp_2", "sm_light_tp_r" }) LampSet(tn, 2);
    fprintf(stderr, "[H19] Der Riese: corriente: %zu luces light_electric encendidas, lamparas %d/%d/%d, mapas de teletransporte en rojo (t=%d)\n", s_elights.size(), n0, n1, n2, now);
}

void Notify(const std::string &ev, int now)
{
    if (ev == "ZPO") { PowerOn(now); return; }   // factory_ZPO_listener: revive_on, fast_reload_on, doubletap_on, jugger_on y "pl1"
    if (ev == "pl1") { PowerOn(now); return; }
    for (Trap &t : s_traps) {
        if (ev == t.name) {   // electric_trap_fx: borra los anteriores y lanza el zapper en cada struct
            for (size_t i = 0; i < t.pts.size(); ++i) { H19_KillFx(t.fx[i]); float f[3], r[3], u[3]; AngleVectors(t.pts[i].ang, f, r, u); t.fx[i] = H19_PlayFx("misc/fx_zombie_electric_trap", t.pts[i].org, f, u); }
            fprintf(stderr, "[H19] trampa %s encendida (%zu FX)\n", t.name.c_str(), t.pts.size());
        }
        if (ev == t.side + "off") { for (auto &h : t.fx) H19_KillFx(h); fprintf(stderr, "[H19] trampa %s apagada\n", t.name.c_str()); }
    }
    if (ev.size() == 3 && ev[0] == 'p' && ev[1] == 'w' && ev[2] >= '0' && ev[2] <= '2') {   // perk_wire_fx
        Wire &w = s_wire[ev[2] - '0'];
        if (w.step < 0 && !w.path.empty()) { w.step = 0; w.next = now; fprintf(stderr, "[H19] cable del pad %c: chispa (%zu tramos)\n", ev[2], w.path.size()); }
    }
    if (ev.size() == 3 && ev[0] == 't' && ev[1] >= '0' && ev[1] <= '2' && ev[2] == '1' && s_power) {   // teleporter_map_light: enlazado
        const std::string tn = std::string("sm_light_tp_") + ev[1]; LampSet(tn.c_str(), 1);
        fprintf(stderr, "[H19] mapa del teletransportador %c en verde\n", ev[1]);
    }
    if (ev == "TRf" && !s_rxPap && s_rxFlashNext < 0) { s_rxFlashNext = now; s_rxFlashRed = true; }
    if (ev == "TRs" && !s_rxPap && s_rxFlashNext >= 0) { s_rxFlashNext = -1; LampSet("sm_light_tp_r", 2); }
    if (ev == "pap1" && !s_rxPap) { s_rxPap = true; s_rxFlashNext = -1; s_rxGreenAt = now + 1500; }   // wait(1.5): pausa dramatica
    if (ev.size() == 4 && !ev.compare(0, 3, "scd") && ev[3] >= '0' && ev[3] <= '2') {   // pole_fx_audio
        const int p = ev[3] - '0'; if (!s_polesOn[p]) { s_polesOn[p] = true; for (Pole &q : s_poles[p]) q.next = now; }
    }
    if (ev == "dog_start") Fog(1, 7.0f);
    if (ev == "dog_stop") Fog(2, 7.0f);
    if (H7_VisionSetNaked && H19_EnvOn("H7E_VISION")) {
        if (ev == "black_box_start") { s_savedVis = Naked(); H7_VisionSetNaked("default", 0); }
        else if (ev == "black_box_end" && !s_savedVis.empty()) { H7_VisionSetNaked(s_savedVis.c_str(), 0); s_savedVis.clear(); }
        else if (ev == "tae") {   // wait_for_teleport_aftereffect (un jugador local: la lista empieza por fov)
            const std::string sv = Naked(); s_steps.clear();
            int pick = (int)(H19_Rand01() * 7.0f); if (pick > 6) pick = 6;
            const char *ov = Dvar_FindVar("factoryAftereffectOverride") ? Dvar_GetString("factoryAftereffectOverride") : nullptr;
            if (ov && *ov && strcmp(ov, "-1")) pick = atoi(ov);
            if (const char *e = getenv("H19_AE")) pick = atoi(e);
            if (pick < 0 || pick > 6) pick = 0;
            static const char *names[7] = { "fov", "shellshock", "shellshock_electric", "bw", "red", "flashy", "flare" };
            fprintf(stderr, "[H19] efecto tras teletransporte: %s\n", names[pick]);
            if (pick == 0) for (int i = 0; i <= 30; ++i) s_steps.push_back({ now + i * 17, "", 0, 30.0f + 35.0f * (i * 0.017f / 0.5f > 1 ? 1 : i * 0.017f / 0.5f) });
            else if (pick == 3) { s_steps.push_back({ now, "cheat_bw_invert_contrast", 0.4f, 0 }); s_steps.push_back({ now + 1250, sv, 1, 0 }); }
            else if (pick == 4) { s_steps.push_back({ now, "zombie_turned", 0.4f, 0 }); s_steps.push_back({ now + 1250, sv, 1, 0 }); }
            else if (pick == 5) { const char *v[4] = { "cheat_bw_invert_contrast", "cheat_bw_contrast", "cheat_invert_contrast", "cheat_contrast" };
                for (int i = 0; i < 4; ++i) s_steps.push_back({ now + 400 * i, v[i], 0.1f, 0 }); s_steps.push_back({ now + 1600, sv, 5, 0 }); }
            else if (pick == 6) { s_steps.push_back({ now, "flare", 0.4f, 0 }); s_steps.push_back({ now + 1250, sv, 1, 0 }); }
        }
    }
}
bool LevelNotify(const std::string &ev, int now) { Notify(ev, now); return false; }   // false: los mezcladores genericos siguen (este mapa no tiene)

void Frame(int now)
{
    for (int i = 0; i < 3; ++i) {   // chispa del cable: MoveTo(target, 0,1) por tramo; el FX va en el tag del mover (aqui se relanza en cada tramo)
        Wire &w = s_wire[i]; if (w.step < 0 || now < w.next) continue;
        if (w.step >= (int)w.path.size()) {
            H19_KillFx(w.fx); w.step = -2;   // mover Delete(); no se repite (el GSC solo lo manda una vez)
            const std::string done = "t" + std::to_string(i) + "1"; fprintf(stderr, "[H19] cable del pad %d: fin -> %s\n", i, done.c_str());
            H19_LevelNotify(done, now);   // level notify(done_notify): enlace completo, luz verde
            continue;
        }
        H19_KillFx(w.fx); const Pt &p = w.path[w.step];
        const float f[3] = { 1, 0, 0 }, u[3] = { 0, 0, 1 }; w.fx = H19_PlayFx("maps/zombie/fx_zombie_wire_spark", p.org, f, u);
        ++w.step; w.next = now + 100;
    }
    if (s_rxFlashNext >= 0 && now >= s_rxFlashNext) { LampSet("sm_light_tp_r", s_rxFlashRed ? 2 : 0); s_rxFlashRed = !s_rxFlashRed; s_rxFlashNext = now + 500; }
    if (s_rxGreenAt >= 0 && now >= s_rxGreenAt) { s_rxGreenAt = -1; LampSet("sm_light_tp_r", 1); fprintf(stderr, "[H19] receptor del PaP en verde\n"); }
    for (int i = 0; i < 3; ++i) if (s_polesOn[i]) for (Pole &q : s_poles[i]) if (now >= q.next) {   // realwait(randomintrange(2,7)) = 2..6 s
        float f[3], r[3], u[3]; AngleVectors(q.p.ang, f, r, u); unsigned h = H19_PlayFx("maps/zombie/fx_zombie_elec_pole_terminal", q.p.org, f, u); (void)h;
        QueueSound(q.p.org, "pole_spark");
        int s = 2 + (int)(H19_Rand01() * 5.0f); if (s > 6) s = 6; q.next = now + s * 1000;
    }
    for (auto &k : s_steps) if (k.at >= 0 && now >= k.at) {
        if (!k.vis.empty() && H7_VisionSetNaked) H7_VisionSetNaked(k.vis.c_str(), k.sec);
        if (k.fov > 0) { char b[32]; snprintf(b, sizeof b, "%.2f", k.fov); Dvar_SetFromStringByName("cg_fov", b); }
        k.at = -1;
    }
}

// ===================================================================================================== sonido (zombie_cod5_factory_amb.csc)
#ifdef H6SND
}   // namespace
int __cdecl CG_PlaySoundWithHandle(int localClientNum, int entitynum, float *origin, int fadeMs, bool doNotify, float volume, unsigned int id);   // h5_audio_cl.cpp
namespace {
int PlayAt(const float *o, const char *alias)   // playsound(0, alias, origin): devuelve el id para SoundPlaying
{
    float p[3] = { o[0], o[1], o[2] }; return CG_PlaySoundWithHandle(0, 1022, p, 0, 0, 1.0f, SND_FindAliasId(alias));
}
const float kZero[3] = { 0, 0, 0 };
struct Ev { double at; std::string tag; int pa; float org[3]; std::string alias; bool dialog; };   // dialog = pa_play_dialog (no pisa)
std::vector<Ev> s_ev;
std::vector<Pt> s_pa; std::vector<int> s_paId;   // structs pa_system y el id de la frase que dice cada uno
bool s_aPower, s_tpOn[3], s_pacBusy[3], s_scd[3];
double s_tpwUntil[3], s_trapUntil[3];
void At(double t, const float *o, const std::string &alias, const std::string &tag = "", int pa = -1, bool dialog = false)
{
    Ev e; e.at = t; e.tag = tag; e.pa = pa; memcpy(e.org, o, 12); e.alias = alias; e.dialog = dialog; s_ev.push_back(e);
}
void PaAll(double t0, const char *buzz, double dt, const std::string &dialog, const std::string &tag = "")   // pa_buzz y, dt s despues, la frase
{
    for (size_t i = 0; i < s_pa.size(); ++i) { if (buzz) At(t0, s_pa[i].org, buzz, tag); if (!dialog.empty()) At(t0 + dt * 1000.0, s_pa[i].org, dialog, tag, (int)i, true); }
}
void Loop(const float *o, const char *alias, float fade) { const int f = H19A_FakeAt(o); if (f) H19A_PlayLoop(f, alias, fade); }   // _audio::playloopat
const std::vector<Pt> &S(const std::string &tn) { static const std::vector<Pt> none; auto it = s_structs.find(tn); return it == s_structs.end() ? none : it->second; }
// sonido de la chispa del cable (perk_wire_fx_client + tele_spark_audio_mover)
struct AWire { std::vector<Pt> path; int step = -1; double next = 0; int fake = 0; };
AWire s_aw[3];

void AudioStart()
{
    LoadStructs();
    s_pa = S("pa_system"); s_paId.assign(s_pa.size(), -1);
    const double now = H19A_NowMs();
    At(now + 2000.0, kZero, "power_down_2d");                  // power_audio_2d
    PaAll(now + 2000.0, "pa_buzz", 1.2, "pa_level_start");     // pa_level_start
    for (int i = 0; i < 3; ++i) s_aw[i].path = Chain(("pad_" + std::to_string(i) + "_wire").c_str());
    fprintf(stderr, "[H19] Der Riese sonido: %zu altavoces pa_system, %zu dyn_light, %zu dyn_generator, %zu dyn_breakers, %zu switch_progress\n",
        s_pa.size(), S("dyn_light").size(), S("dyn_generator").size(), S("dyn_breakers").size(), S("switch_progress").size());
}
void AudioNotify(const std::string &n)
{
    const double now = H19A_NowMs();
    if ((n == "ZPO" || n == "pl1") && !s_aPower) {   // "pl1": start_lights, power_audio_2d, pa_power_on
        s_aPower = true;
        for (const Pt &p : S("dyn_light")) { PlayAt(p.org, "light_start"); Loop(p.org, "light", 0); }
        for (const H19Kv &k : H19_MapEnts()) {   // switch_progress_sound: espera segun script_noteworthy
            const char *tn = H19_Str(k, "targetname"), *sn = H19_Str(k, "script_noteworthy"); if (!tn || strcmp(tn, "switch_progress") || !sn) continue;
            const int v = atoi(sn); const double t = v >= 1 && v <= 5 ? v * 500.0 : 0.0; Pt p = StructOf(k); At(now + t, p.org, "switch_progress");
        }
        for (const Pt &p : S("dyn_generator")) { At(now + 3000.0, p.org, "switch_progress"); At(now + 3000.0, p.org, "gen_start"); At(now + 3000.0, p.org, "@loop:gen_loop:1"); }
        for (const Pt &p : S("dyn_breakers")) { PlayAt(p.org, "break_start"); Loop(p.org, "break_loop", 2); }
        PlayAt(kZero, "power_up_2d");
        PaAll(now, "pa_buzz", 1.2, "pa_power_on");
        return;
    }
    if (n.size() == 3 && n[0] == 't' && n[1] == 'p' && n[2] >= '0' && n[2] <= '2') {   // teleport_pad_init
        const int p = n[2] - '0'; if (s_tpOn[p]) return; s_tpOn[p] = true;
        for (const Pt &q : S("telepad_" + std::to_string(p) + "_looper")) Loop(q.org, "power_loop", 1);
        return;
    }
    if (n.size() == 4 && !n.compare(0, 3, "tpw") && n[3] >= '0' && n[3] <= '2') {   // teleportation_audio
        const int p = n[3] - '0'; if (!s_tpOn[p] || now < s_tpwUntil[p]) return; s_tpwUntil[p] = now + 2000.0;
        for (const H19Kv &k : H19_MapEnts()) {
            const char *tn = H19_Str(k, "targetname"), *ss = H19_Str(k, "script_sound"); if (!tn || !ss || !*ss) continue;
            const Pt q = StructOf(k); const std::string s = ss;
            if (tn == "telepad_" + std::to_string(p)) { PlayAt(q.org, (s + "_warmup").c_str()); At(now + 2000.0, q.org, s + "_cooldown"); }
            else if (!strcmp(tn, "homepad")) { At(now + 2000.0, q.org, s + "_warmup"); At(now + 2000.0, q.org, s + "_cooldown"); }
        }
        return;
    }
    if (n.size() == 4 && !n.compare(0, 3, "pac") && n[3] >= '0' && n[3] <= '2') {   // pa_countdown (endon "scd<N>")
        const int p = n[3] - '0'; if (s_scd[p] || s_pacBusy[p]) return; s_pacBusy[p] = true;
        const std::string tag = "pac" + std::to_string(p);
        PaAll(now, "pa_buzz", 0, "pa_audio_link_start", tag);
        for (int c = 30, i = 0; c > 0; --c, ++i) {
            const bool play = c == 20 || c == 15 || c <= 10;
            for (const Pt &q : s_pa) { if (play) At(now + i * 1000.0, q.org, "pa_audio_link_" + std::to_string(c), tag); At(now + i * 1000.0, kZero, "clock_tick_1sec", tag); }
        }
        PaAll(now + 30000.0, "pa_buzz", 1.2, "pa_audio_link_fail", tag);
        At(now + 31200.0, kZero, "@free:" + tag, tag);   // vuelve a esperar "pac<N>"
        return;
    }
    if (n.size() == 4 && !n.compare(0, 3, "scd") && n[3] >= '0' && n[3] <= '2') {   // pa_countdown_success (una vez) y fin de la cuenta atras
        const int p = n[3] - '0'; if (s_scd[p]) return; s_scd[p] = true;
        const std::string tag = "pac" + std::to_string(p);
        s_ev.erase(std::remove_if(s_ev.begin(), s_ev.end(), [&](const Ev &e) { return e.tag == tag; }), s_ev.end());
        PaAll(now, "pa_buzz", 1.2, "pa_audio_act_pad_" + std::to_string(p));
        return;
    }
    if (n.size() == 4 && !n.compare(0, 3, "tpc") && n[3] >= '0' && n[3] <= '2') { PaAll(now + 1000.0, "pa_buzz", 1.2, "pa_teleport_finish"); return; }   // pa_teleport
    static const char *trapLoc[3] = { "warehouse", "wuen", "bridge" };
    for (int i = 0; i < 3; ++i) if (n == trapLoc[i]) {   // pa_electric_trap
        if (now < s_trapUntil[i]) return; s_trapUntil[i] = now + 1200.0 + 48500.0 + 1200.0;
        PaAll(now, "pa_buzz", 1.2, std::string("pa_trap_inuse_") + trapLoc[i]);
        PaAll(now + 1200.0 + 48500.0, "pa_buzz", 1.2, std::string("pa_trap_active_") + trapLoc[i]);
        return;
    }
    if (n == "t2d") { PlayAt(kZero, "teleport_2d_fnt"); PlayAt(kZero, "teleport_2d_rear"); return; }   // teleport_2d
    if (n == "pap1") {   // homepad_loop + linkall_2d
        for (const Pt &q : S("homepad_power_looper")) Loop(q.org, "homepad_power_loop", 1);
        for (const Pt &q : S("homepad_breaker")) Loop(q.org, "break_arc", 1);
        PlayAt(kZero, "linkall_2d");
        return;
    }
    if (n.size() == 3 && n[0] == 'p' && n[1] == 'w' && n[2] >= '0' && n[2] <= '2') {   // chispa del cable
        AWire &w = s_aw[n[2] - '0']; if (w.step != -1 || w.path.empty()) return;
        w.fake = H19A_FakeAt(w.path[0].org); PlayAt(w.path[0].org, "tele_spark_hit"); if (w.fake) H19A_PlayLoop(w.fake, "tele_spark_loop", 0);
        w.step = 1; w.next = now + 100.0;
    }
}
void AudioFrame(bool)
{
    const double now = H19A_NowMs();
    { std::vector<std::pair<std::vector<float>, std::string>> q; { std::lock_guard<std::mutex> g(s_sndMu); q.swap(s_sndQ); }
      for (auto &s : q) PlayAt(s.first.data(), s.second.c_str()); }
    for (AWire &w : s_aw) if (w.step >= 0 && now >= w.next) {   // setfakeentorg cada 0,05 s siguiendo al mover (aqui, al llegar a cada tramo)
        if (w.step >= (int)w.path.size()) { if (w.fake) H19A_StopLoop(w.fake, 0); w.step = -2; continue; }   // deletefakeent
        if (w.fake) memcpy((float *)H19A_FakeOrg(w.fake), w.path[w.step].org, 12);
        ++w.step; w.next = now + 100.0;
    }
    if (s_ev.empty()) return;
    std::vector<Ev> due; for (const Ev &e : s_ev) if (now >= e.at) due.push_back(e);
    if (due.empty()) return;
    s_ev.erase(std::remove_if(s_ev.begin(), s_ev.end(), [&](const Ev &e) { return now >= e.at; }), s_ev.end());
    for (const Ev &e : due) {
        if (!e.alias.compare(0, 6, "@free:")) { const int p = e.alias.back() - '0'; if (p >= 0 && p < 3) s_pacBusy[p] = false; continue; }
        if (!e.alias.compare(0, 6, "@loop:")) {   // "@loop:alias:fade"
            const size_t c = e.alias.find(':', 6); Loop(e.org, e.alias.substr(6, c - 6).c_str(), (float)atof(e.alias.c_str() + c + 1)); continue; }
        if (e.dialog && e.pa >= 0 && e.pa < (int)s_paId.size()) {   // pa_play_dialog: si este altavoz aun habla, la frase nueva se pierde
            if (s_paId[e.pa] >= 0 && SND_IsPlaying(s_paId[e.pa])) continue;
            s_paId[e.pa] = PlayAt(e.org, e.alias.c_str()); continue;
        }
        PlayAt(e.org, e.alias.c_str());
    }
}
void AudioStatus(std::string &out)
{
    char b[160]; snprintf(b, sizeof b, "; Der Riese: corriente %s, pads %d%d%d, enlazados %d%d%d, eventos pendientes %zu", s_aPower ? "si" : "no",
        s_tpOn[0], s_tpOn[1], s_tpOn[2], s_scd[0], s_scd[1], s_scd[2], s_ev.size());
    out += b;
}
#endif
} // namespace

extern const H19MapCl kH19_zombie_cod5_factory = {
    .zone = "zombie_cod5_factory", .offEnv = "H19_FACTORY",
    .estado = "Der Riese: niebla y perros, luces electricas, mapas y cables de los teletransportadores, trampas, postes, megafonia y sonido de corriente (OLA M4)",
    .init = Init, .levelNotify = LevelNotify, .sysState = nullptr, .frame = Frame, .clockHand = nullptr, .screensFrame = nullptr,
    .levelEffect = nullptr,
#ifdef H6SND
    .audioStart = AudioStart, .audioNotify = AudioNotify, .audioSys = nullptr, .audioFrame = AudioFrame, .audioStatus = AudioStatus,
#endif
    .doorEnt = nullptr,
};
#endif
