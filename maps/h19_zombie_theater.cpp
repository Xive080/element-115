// OLA M2: cliente de zombie_theater (Kino der Toten), separado de los comunes. Mismo codigo y numeros que la OLA M1 tenia en
// h7e_light.cpp y h5_audio_cl.cpp; solo cambia el sitio. Reproduce sin VM de CSC lo que hacen sus clientscripts (rawfiles del ff):
//  - zombie_theater.csc: theatre_ZPO_listener (level._power_on, luces del tablero de la caja, notify local "pl1"),
//    theater_light_model_swap (model_lights_on: lights_hang_single -> lights_hang_single_on_nonflkr, zombie_zapper_cagelight ->
//    zombie_zapper_cagelight_on), init_board_lights / theater_box_monitor (sistema "box_indicator": 9 tableros magic_box_loc_light,
//    -1 apagadas, 99 parpadeo 0,25 s al moverse, 98 parpadeo 0,3 s en fire sale, n = verde + FX boxlight_light_ready si hay corriente).
//  - zombie_theater_fx.csc: trap_fx_monitor (e1..e4 electricas = misc/fx_zombie_electric_trap, f1 fuego = fire_trap_med, en los
//    script_struct sin modelo de cada trampa; "<loc>1" enciende, "<loc>0" apaga), projector_screen_fx ("sip": haz del proyector y
//    pantalla; "ps1".."ps3" cambian la bobina), level_fog_init + dog_start_monitor / dog_stop_monitor (setVolFog, h7c_gfx.cpp).
//  - zombie_theater_teleporter.csc: wait_for_black_box (vision "default" mientras dura), wait_for_teleport_aftereffect (uno al azar de
//    fov / shellshock / shellshock_electric / bw / red / flashy / flare), pack_clock_init (reloj del PaP, gancho clockHand).
//  - zombie_theater_amb.csc power_on_all (tras "pl1", que el cliente se notifica a si mismo al llegar "ZPO"): teleportation_audio
//    ("tpa": evt_<script_sound>_warmup en cada struct telepad y _cooldown 2 s despues), teleport_2d ("t2d"), teleport_2d_nopad ("t2dn":
//    warmup 2D y a los 1,3 s los dos 2D), teleport_beam_fx_2d ("t2bfx"), teleport_specialroom_start/go ("tss"/"tsg").
// No se hace: la extracam (isExtraCam: pantalla del proyector en vivo) ni los vision sets de las salas del huevo de pascua.
//   H19_KINO=0 lo desactiva (vuelve el comportamiento generico). H19_AE=n fija el efecto tras teletransporte (prueba).
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <game_mp/g_main_mp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "h19_mapcl.h"

bool H7_VisionSetNaked(const char *name, float seconds) __attribute__((weak));   // h7_gfx.cpp
const char *H7_VisionNakedName() __attribute__((weak));                           // h7_gfx.cpp
void H7C_ClientVolFog(const float *v, int transitionMs) __attribute__((weak));     // h7c_gfx.cpp: setVolFog del CSC

namespace {
// ===================================================================================================== cliente grafico
struct KPt { float org[3], ang[3]; unsigned h = 0; };
struct KTrap { std::string loc, fx; std::vector<KPt> pts; };
std::vector<KTrap> s_ktraps;
struct KBox { int model; int loc; float org[3], ang[3]; unsigned fx = 0; };
std::vector<KBox> s_kbox;
const char *kBoxLocs[9] = { "start_chest_loc", "foyer_chest_loc", "crematorium_chest_loc", "alleyway_chest_loc", "control_chest_loc",
                            "stage_chest_loc", "dressing_chest_loc", "dining_chest_loc", "theater_chest_loc" };
bool s_power, s_packClock;
int s_kboxInd = -1, s_kflashMs = 0, s_kflashNext = 0; bool s_kflashGreen;
KPt s_kproj, s_kscreen; bool s_kprojOk, s_kscreenOk, s_kprojOn; unsigned s_kprojFx, s_kscreenFx;
struct KStep { int at; std::string vis; float sec; float fov; };   // pasos de los efectos tras teletransporte
std::vector<KStep> s_ksteps; std::string s_ksavedVis;
const char *H7Naked() { const char *n = H7_VisionNakedName ? H7_VisionNakedName() : nullptr; return n && *n ? n : "zombie_theater"; }

void KinoFog(int which, float sec)   // 0 = level_fog_init / dog_stop_monitor, 1 = dog_start_monitor
{
    if (!H7C_ClientVolFog) return;
    // setVolFog(start, halfwayDist, halfwayHeight, baseHeight, r, g, b, colorScale, sunR, sunG, sunB, dirX, dirY, dirZ, startAng, endAng, time, maxOpacity)
    static const float lv[18] = { 155.625f, 2332.79f, 677.241f, 23.2733f, 0.639216f, 0.639216f, 0.639216f, 1, 0.243137f, 0.270588f, 0.270588f, 0.291692f, -0.720765f, 0.628819f, 0, 0, 0, 1 };
    static const float dg[18] = { 0, 302.622f, 306.395f, 344.622f, 0.458824f, 0.556863f, 0.619608f, 1, 0.243137f, 0.270588f, 0.270588f, 0.291692f, -0.720765f, 0.628819f, 0, 0, 0, 0.65f };
    H7C_ClientVolFog(which ? dg : lv, (int)(sec * 1000.0f));
}
void KBoxSet(int i, int color /*0 off 1 green 2 red*/, bool fx)
{
    KBox &b = s_kbox[i]; H19_KillFx(b.fx);
    H19_SetModel(b.model, color == 1 ? "zombie_zapper_cagelight_green" : color == 2 ? "zombie_zapper_cagelight_red" : "zombie_zapper_cagelight");
    if (fx && color) { float o[3] = { b.org[0], b.org[1], b.org[2] - 10.0f }; b.fx = H19_PlayFxAng("maps/zombie/fx_zombie_theater_lightboard_green", o, b.ang); }
}
void KBoxLoc(int loc, int color, bool fx) { if (loc < 0 || loc >= 9) return; for (size_t i = 0; i < s_kbox.size(); ++i) if (s_kbox[i].loc == loc) KBoxSet((int)i, color, fx); }
void KBoxAllOff() { s_kflashMs = 0; for (size_t i = 0; i < s_kbox.size(); ++i) KBoxSet((int)i, 0, false); }
void KBoxMonitor(int st, int now)   // theater_box_monitor
{
    if (st == -1) KBoxAllOff();
    else if (st == 99 || st == 98) { s_kflashMs = st == 99 ? 250 : 300; s_kflashNext = now + s_kflashMs; s_kflashGreen = true; }   // flash_lights: wait, verdes, wait, apagadas
    else { if (st < 0 || st > 9) return; KBoxAllOff(); s_kboxInd = st; if (s_power) KBoxLoc(st, 1, true); }
}
void Init(const std::vector<H19Kv> &ents)
{
    std::map<std::string, std::string> trapFx = { { "dressing_room_trap", "e4" }, { "vip_room_trap", "e2" }, { "foyer_room_trap", "e1" }, { "control_room_trap", "e3" }, { "crematorium_room_trap", "f1" } };
    for (auto &t : trapFx) { KTrap k; k.loc = t.second; k.fx = t.second[0] == 'f' ? "maps/zombie/fx_zombie_fire_trap_med" : "misc/fx_zombie_electric_trap"; s_ktraps.push_back(k); }
    for (const H19Kv &k : ents) {
        const char *cn = H19_Str(k, "classname"), *tn = H19_Str(k, "targetname"); if (!cn || !tn) continue;
        if (!strcmp(cn, "script_struct")) {
            KPt p; H19_Vec(k, "origin", p.org); memset(p.ang, 0, 12); H19_Vec(k, "angles", p.ang);
            auto it = trapFx.find(tn);
            if (it != trapFx.end() && !H19_Str(k, "model")) for (auto &t : s_ktraps) if (t.loc == it->second) t.pts.push_back(p);
            if (!strcmp(tn, "magic_box_loc_light") && H19_Str(k, "script_noteworthy")) {   // init_board_lights: spawn script_model zombie_zapper_cagelight
                int loc = -1; for (int i = 0; i < 9; ++i) if (!strcmp(kBoxLocs[i], H19_Str(k, "script_noteworthy"))) loc = i;
                const int m = H19_AddModel("zombie_zapper_cagelight", p.org, p.ang, "magic_box_loc_light");
                if (m >= 0) { KBox b; b.model = m; b.loc = loc; memcpy(b.org, p.org, 12); memcpy(b.ang, p.ang, 12); s_kbox.push_back(b); }
            }
            if (!strcmp(tn, "struct_theater_projector_beam")) { s_kproj = p; s_kprojOk = true; }
            if (!strcmp(tn, "struct_theater_screen")) { s_kscreen = p; s_kscreenOk = true; }
        }
    }
    size_t np = 0; for (auto &t : s_ktraps) np += t.pts.size();
    KinoFog(0, 0.1f);   // level_fog_init
    fprintf(stderr, "[H19] Kino: %zu trampas (%zu puntos de FX), %zu luces de tablero de la caja, proyector %s, pantalla %s, niebla de cliente %s\n",
        s_ktraps.size(), np, s_kbox.size(), s_kprojOk ? "si" : "NO", s_kscreenOk ? "si" : "NO", H7C_ClientVolFog ? "si" : "NO");
}
void Notify(const std::string &ev, int now)
{
    if (ev == "ZPO") {   // theatre_ZPO_listener + theater_light_model_swap
        s_power = true;
        H19_SwapModelByTn("model_lights_on", "lights_hang_single", "lights_hang_single_on_nonflkr");
        H19_SwapModelByTn("model_lights_on", "zombie_zapper_cagelight", "zombie_zapper_cagelight_on");
        if (s_kboxInd != -1) KBoxMonitor(s_kboxInd, now);
        H19_MixerActivate(ev, now);
        Notify("pl1", now);   // level notify("pl1"): luces de corriente (mezcladores pl1)
        return;
    }
    if (ev.size() == 3 && (ev[0] == 'e' || ev[0] == 'f') && isdigit((unsigned char)ev[1]) && (ev[2] == '0' || ev[2] == '1')) {   // trap_fx_monitor
        const std::string loc = ev.substr(0, 2); const bool on = ev[2] == '1';
        for (auto &t : s_ktraps) if (t.loc == loc) for (auto &p : t.pts) {
            H19_KillFx(p.h);
            if (on) { float f[3], r[3], u[3]; AngleVectors(p.ang, f, r, u); p.h = H19_PlayFx(t.fx.c_str(), p.org, f, u); }
        }
        fprintf(stderr, "[H19] trampa %s %s\n", loc.c_str(), on ? "encendida" : "apagada");
        return;
    }
    if (ev == "sip" && !s_kprojOn) {   // projector_screen_fx
        s_kprojOn = true;
        if (s_kprojOk) s_kprojFx = H19_PlayFxAng("maps/zombie/fx_zombie_theater_projector_beam", s_kproj.org, s_kproj.ang);
        if (s_kscreenOk) s_kscreenFx = H19_PlayFxAng("maps/zombie/fx_zombie_theater_screen_0", s_kscreen.org, s_kscreen.ang);
    }
    if ((ev == "ps1" || ev == "ps2" || ev == "ps3") && s_kprojOn && s_kscreenOk)   // projector_reel_swap: PlayFXOnTag encima del anterior
        H19_PlayFxAng(("maps/zombie/fx_zombie_theater_screen_" + ev.substr(2)).c_str(), s_kscreen.org, s_kscreen.ang);
    if (ev == "dog_start") KinoFog(1, 7.0f);
    if (ev == "dog_stop") KinoFog(0, 7.0f);
    if (ev == "pack_clock_start") s_packClock = true;
    if (H7_VisionSetNaked && H19_EnvOn("H7E_VISION")) {
        if (ev == "black_box_start") { s_ksavedVis = H7Naked(); H7_VisionSetNaked("default", 0); }
        else if (ev == "black_box_end" && !s_ksavedVis.empty()) { H7_VisionSetNaked(s_ksavedVis.c_str(), 0); s_ksavedVis.clear(); }
        else if (ev == "teleport_ae_start") {   // wait_for_teleport_aftereffect: uno al azar (theaterAftereffectOverride = -1)
            const std::string sv = H7Naked(); s_ksteps.clear();
            int pick = (int)(H19_Rand01() * 7.0f); if (pick > 6) pick = 6;
            if (const char *e = getenv("H19_AE")) pick = atoi(e);
            static const char *names[7] = { "fov", "shellshock", "shellshock_electric", "bw", "red", "flashy", "flare" };
            fprintf(stderr, "[H19] efecto tras teletransporte: %s\n", names[pick]);
            if (pick == 0) for (int i = 0; i <= 30; ++i) s_ksteps.push_back({ now + i * 17, "", 0, 30.0f + 35.0f * (i * 0.017f / 0.5f > 1 ? 1 : i * 0.017f / 0.5f) });
            else if (pick == 3) { s_ksteps.push_back({ now, "cheat_bw_invert_contrast", 0.4f, 0 }); s_ksteps.push_back({ now + 1250, sv, 1, 0 }); }
            else if (pick == 4) { s_ksteps.push_back({ now, "zombie_turned", 0.4f, 0 }); s_ksteps.push_back({ now + 1250, sv, 1, 0 }); }
            else if (pick == 5) { const char *v[4] = { "cheat_bw_invert_contrast", "cheat_bw_contrast", "cheat_invert_contrast", "cheat_contrast" };
                for (int i = 0; i < 4; ++i) s_ksteps.push_back({ now + 400 * i, v[i], 0.1f, 0 }); s_ksteps.push_back({ now + 1600, sv, 5, 0 }); }
            else if (pick == 6) { s_ksteps.push_back({ now, "flare", 0.4f, 0 }); s_ksteps.push_back({ now + 1250, sv, 1, 0 }); }
        }
    }
    H19_MixerActivate(ev, now);   // mixer_level_activate (tl*, sip, ft1...)
}
bool LevelNotify(const std::string &ev, int now) { Notify(ev, now); return true; }   // Kino lo lleva todo (mezcladores incluidos)
bool SysState(const char *sys, const std::string &st, int now)
{
    if (I_stricmp(sys, "box_indicator")) return false;
    KBoxMonitor(atoi(st.c_str()), now); return true;
}
void Frame(int now)
{
    if (s_kflashMs && now >= s_kflashNext) {
        for (int i = 0; i < 9; ++i) KBoxLoc(i, s_kflashGreen ? 1 : 0, false);
        s_kflashGreen = !s_kflashGreen; s_kflashNext = now + s_kflashMs;
    }
    for (auto &k : s_ksteps) if (k.at >= 0 && now >= k.at) {
        if (!k.vis.empty() && H7_VisionSetNaked) H7_VisionSetNaked(k.vis.c_str(), k.sec);
        if (k.fov > 0) { char b[32]; snprintf(b, sizeof b, "%.2f", k.fov); Dvar_SetFromStringByName("cg_fov", b); }
        k.at = -1;
    }
}
// zombie_theater_teleporter.csc pack_clock_init / pack_clock_run: reloj del Pack-a-Punch, desde "pack_clock_start", RotatePitch
// NEGATIVO de hand_time * rotate + first_rotate y luego rotate_bit por segundo (sin segundero: no tiene spawnflags 2)
bool ClockHand(const char *tn, const H19Clock &c, float *add)
{
    if (!s_packClock) return false;
    if (!strcmp(tn, "zom_clock_hour_hand")) { *add = -(c.h12 * 30.0f + (c.lm * 60 + c.ls) * (30.0f / 3600.0f) + c.el * (30.0f / 3600.0f)); return true; }
    if (!strcmp(tn, "zom_clock_minute_hand")) { *add = -(c.lm * 6.0f + c.ls * 0.1f + c.el * 0.1f); return true; }
    return false;
}

// ===================================================================================================== sonido (zombie_theater_amb.csc)
#ifdef H6SND
struct KSnd { double at; float org[3]; std::string alias; };
std::vector<KSnd> s_kSnd; bool s_kPl1;
void AudioNotify(const std::string &n)
{
    const float zero[3] = { 0, 0, 0 };
    if (n == "ZPO") s_kPl1 = true;
    if (!s_kPl1) return;
    const double now = H19A_NowMs();
    if (n == "tpa") {
        for (const H19Kv &k : H19_MapEnts()) {   // structs telepad con script_sound
            const char *tn = H19_Str(k, "targetname"), *ss = H19_Str(k, "script_sound");
            if (!tn || strcmp(tn, "telepad") || !ss || !*ss) continue;
            KSnd a; a.org[0] = a.org[1] = a.org[2] = 0; if (const char *o = H19_Str(k, "origin")) sscanf(o, "%f %f %f", &a.org[0], &a.org[1], &a.org[2]);
            H19A_WorldSound(a.org, ("evt_" + std::string(ss) + "_warmup").c_str()); a.at = now + 2000.0; a.alias = "evt_" + std::string(ss) + "_cooldown"; s_kSnd.push_back(a);
        }
    }
    else if (n == "t2d" || n == "tsg") { H19A_WorldSound(zero, "evt_teleport_2d_fnt"); H19A_WorldSound(zero, "evt_teleport_2d_rear"); }
    else if (n == "t2dn") { H19A_WorldSound(zero, "evt_pad_warmup_2d"); KSnd a{}; a.at = now + 1300.0; a.alias = "evt_teleport_2d_fnt|evt_teleport_2d_rear"; s_kSnd.push_back(a); }
    else if (n == "t2bfx") H19A_WorldSound(zero, "evt_beam_fx_2d");
    else if (n == "tss") H19A_WorldSound(zero, "evt_pad_warmup_2d");
}
void AudioFrame(bool)
{
    if (s_kSnd.empty()) return;
    const double now = H19A_NowMs();
    for (auto &a : s_kSnd) if (a.at > 0 && now >= a.at) {
        size_t bar = a.alias.find('|');
        if (bar == std::string::npos) H19A_WorldSound(a.org, a.alias.c_str());
        else { H19A_WorldSound(a.org, a.alias.substr(0, bar).c_str()); H19A_WorldSound(a.org, a.alias.substr(bar + 1).c_str()); }
        a.at = 0;
    }
    s_kSnd.erase(std::remove_if(s_kSnd.begin(), s_kSnd.end(), [](const KSnd &a) { return a.at == 0; }), s_kSnd.end());
}
#endif
} // namespace

extern const H19MapCl kH19_zombie_theater = {
    .zone = "zombie_theater", .offEnv = "H19_KINO", .estado = "Kino: niebla y perros, tablero de la caja, trampas, proyector, teletransporte, reloj del PaP (OLA M1)",
    .init = Init, .levelNotify = LevelNotify, .sysState = SysState, .frame = Frame, .clockHand = ClockHand, .screensFrame = nullptr,
    .levelEffect = nullptr,
#ifdef H6SND
    .audioStart = nullptr, .audioNotify = AudioNotify, .audioSys = nullptr, .audioFrame = AudioFrame, .audioStatus = nullptr,
#endif
};
#endif
