// OLA M2: cliente de zombie_pentagon ("Five"), separado de los comunes. Mismo codigo y numeros que tenian h7e_light.cpp (OLA 7e),
// h9_video.cpp (OLA 9) y h5_audio_cl.cpp (OLA 14); solo cambia el sitio. Reproduce sin VM de CSC:
//  - zombie_pentagon.csc: levelNotify "ZPO" -> _power_on, "TLO" -> por0..4; pentagon_office_light_model_swap (ceiling_on -> ceiling,
//    tinhat_on -> tinhat_off); set_visionset_*: "vis1" -> zombie_pentagon / zombie_pentagon_offices_poweroff (con corriente),
//    "vis2" warroom (2 s), "vis3" lab (2 s), "vis4" electrician (1 s) con VisionSetNaked (h7_gfx.cpp H7_VisionSetNaked).
//  - pantallas de la caja (antes h9_video.cpp, ver abajo) y zombie_pentagon_fx.csc level._effect["test_spin_fx"].
//  - zombie_pentagon_amb.csc (ascensores, minigun), zombie_pentagon_teleporter.csc (portales por la clientflag 5 del jugador),
//    play_magic_box_tv_audio (sonido de las pantallas).
//   H19_FIVE=0 lo apaga (queda solo lo generico). H9_SCREENS=0 deja las pantallas con el modelo del mapa.
#ifdef H5G
#include <universal/q_shared.h>
#include <database/db_registry.h>
#include <xanim/xmodel_utils.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_model_skin.h>
#include <gfx_d3d/r_scene.h>
#include <game_mp/g_main_mp.h>
#include <server_mp/server_mp.h>
#include <server_mp/sv_init_mp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <mutex>
#include "h19_mapcl.h"

bool H7_VisionSetNaked(const char *name, float seconds) __attribute__((weak));   // h7_gfx.cpp
int H7E_ClientModelByTn(const char *tn, int ordinal, const char *name) __attribute__((weak));   // h7e_light.cpp

namespace {
// ===================================================================================================== luces y vision (zombie_pentagon.csc)
bool s_power;   // level._power_on del cliente
bool LevelNotify(const std::string &ev, int now)
{
    if (ev == "ZPO") {   // pentagon_ZPO_listener + pentagon_office_light_model_swap
        s_power = true;
        H19_SwapModelByTn("model_interior_office_lights", "p_pent_light_ceiling_on", "p_pent_light_ceiling");
        H19_SwapModelByTn("model_interior_office_lights", "p_pent_light_tinhat_on", "p_pent_light_tinhat_off");
        H19_LevelNotify("TLO", now); return true;
    }
    if (ev == "TLO") { for (int i = 0; i < 5; ++i) H19_LevelNotify("por" + std::to_string(i), now); return true; }   // pentagon_TLO_listener
    if (ev.size() == 4 && !ev.compare(0, 3, "vis") && H19_EnvOn("H7E_VISION") && H7_VisionSetNaked) {   // set_visionset_*
        const int f = ev[3] - '0';
        if (f == 1) H7_VisionSetNaked(s_power ? "zombie_pentagon_offices_poweroff" : "zombie_pentagon", 0.0f);
        else if (f == 2) H7_VisionSetNaked("zombie_pentagon_warroom", 2.0f);
        else if (f == 3) H7_VisionSetNaked("zombie_pentagon_lab", 2.0f);
        else if (f == 4) H7_VisionSetNaked("zombie_pentagon_electrician", 1.0f);
        return true;
    }
    return false;   // el resto (por0..4...) va a los mezcladores genericos
}
const char *LevelEffect(const char *key) { return !strcmp(key, "test_spin_fx") ? "env/light/fx_light_warning" : nullptr; }   // zombie_pentagon_fx.csc

// ===================================================================================================== pantallas de la caja (antes h9_video.cpp, OLA 9)
// Lo que hace el original (comprobado en los scripts del ff):
//  - La pared curva de la sala de guerra NO es video: son imagenes fijas wc/pent_art_tv_4x4_0X en el brushmodel *79 (OLA 7c). No hay
//    ningun Bink (.bik) en el mapa; r_cinematic.cpp (Bink) solo se usa para las cinematicas globales del menu.
//  - Los monitores de la caja magica (script_model de cliente con targetname "model_pentagon_box_screens", h7e_light.cpp) cambian de
//    MODELO por script: clientscripts/zombie_pentagon.csc init_pentagon_box_screens / pentagon_tv_init / pentagon_screen_switch /
//    magic_box_screen_swap, que reciben el estado de maps/zombie_pentagon_magic_box.gsc con setclientsysstate("box_indicator", s):
//      "n" sin corriente o caja moviendose -> p_zom_monitor_screen_off (antes de ZPO) o p_zom_monitor_screen_on (despues)
//      "f" fire sale -> fsale1 / fsale2;  "0".."5" sitio de la caja (_box_locations) -> lobby0/lobby1, lobby0/lobby2, warroom0/warroom1,
//      labs0/labs1, labs0/labs2, labs0/labs3.
//    magic_box_screen_swap: recorre la lista con wait 3.0 por modelo; despues, con 6 de 100, p_zom_monitor_screen_logo 2.0 s; y wait 1.0.
//    pentagon_screen_switch arranca cada pantalla 0.2 s despues de la anterior (wait dentro del bucle); pentagon_tv_init (la primera
//    vez) las pone en "off" con 0.1 s entre cada una.
// Se reproduce con las mismas tablas y tiempos (level.time del servidor). Coste: ~0 (un cambio de modelo cada 3 s; los vertices del
// modelo nuevo se deforman una vez y se cachean en h7e).
const char *kTn = "model_pentagon_box_screens";
const std::vector<std::string> kOff = { "p_zom_monitor_screen_off" }, kOn = { "p_zom_monitor_screen_on" },
    kFsale = { "p_zom_monitor_screen_fsale1", "p_zom_monitor_screen_fsale2" }, kLogo = { "p_zom_monitor_screen_logo" };
const std::vector<std::string> kLoc[6] = {
    { "p_zom_monitor_screen_lobby0", "p_zom_monitor_screen_lobby1" }, { "p_zom_monitor_screen_lobby0", "p_zom_monitor_screen_lobby2" },
    { "p_zom_monitor_screen_warroom0", "p_zom_monitor_screen_warroom1" }, { "p_zom_monitor_screen_labs0", "p_zom_monitor_screen_labs1" },
    { "p_zom_monitor_screen_labs0", "p_zom_monitor_screen_labs2" }, { "p_zom_monitor_screen_labs0", "p_zom_monitor_screen_labs3" } };

struct Screen { const std::vector<std::string> *arr = nullptr; int step = 0, next = -1; bool logo = false; };
std::vector<Screen> s_scr;
bool s_tvPower, s_tvInited, s_tvOn = true;
std::mutex s_mx; std::vector<std::pair<std::string, std::string>> s_q;   // (sistema, estado) recibidos por h7e_light.cpp (ServerCmd)
unsigned s_tvRng = 0x9E3779B9u;
int RandomInt(int n) { s_tvRng = s_tvRng * 1664525u + 1013904223u; return (int)((s_tvRng >> 8) % (unsigned)n); }

void Set(int i, const std::string &m) { if (H7E_ClientModelByTn) H7E_ClientModelByTn(kTn, i, m.c_str()); }

void Switch(const std::string &state, int now)   // pentagon_screen_switch
{
    const int n = H7E_ClientModelByTn ? H7E_ClientModelByTn(kTn, -1, nullptr) : 0; if (!n) return;
    int t = now;
    if (!s_tvInited) {
        int miss = 0; std::string ms;   // los modelos los precarga el mapa (csv del ff); si falta alguno se avisa una vez
        auto chk = [&](const std::vector<std::string> &v) { for (auto &m : v) if (DB_IsXAssetDefault(ASSET_TYPE_XMODEL, m.c_str())) { ++miss; ms += " " + m; } };
        chk(kOff); chk(kOn); chk(kFsale); chk(kLogo); for (auto &l : kLoc) chk(l);
        fprintf(stderr, "[H9V] pantallas de la caja: %d modelos, %d sin cargar%s\n", n, miss, ms.c_str());
        if (getenv("H9_DIAG")) for (const char *nm : { "p_zom_monitor_screen_off", "p_zom_monitor_screen_labs0", "p_zom_monitor_screen_logo" }) {
            XModel *m = DB_FindXAssetHeader(ASSET_TYPE_XMODEL, (char *)nm, 1, -1).model; if (!m) continue;
            XSurface *xs; const int ns = XModelGetSurfaces(m, &xs, 0); PTR32(Material) *sk = XModelGetSkins(m, 0);
            for (int k = 0; k < ns; ++k) { Material *mt = sk ? sk[k].get() : nullptr;
                fprintf(stderr, "[H9V]   %s surf %d verts %d mat '%s' tecnicas lit %d unlit %d emissive %d\n", nm, k, xs[k].vertCount, mt ? mt->info.name.get() : "-",
                    mt && Material_GetTechnique(mt, 10) ? 1 : 0, mt && Material_GetTechnique(mt, 4) ? 1 : 0, mt && Material_GetTechnique(mt, 5) ? 1 : 0);
                float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f }; const GfxPackedVertex *v = xs[k].verts0.get();
                for (int q = 0; v && q < xs[k].vertCount; ++q) for (int a = 0; a < 3; ++a) { lo[a] = fminf(lo[a], v[q].xyz[a]); hi[a] = fmaxf(hi[a], v[q].xyz[a]); }
                fprintf(stderr, "[H9V]     caja local (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]); } }
        s_tvInited = true; s_scr.assign(n, Screen()); for (int i = 0; i < n; ++i) Set(i, kOff[0]); t += 100 * n; }   // pentagon_tv_init
    const std::vector<std::string> *arr;
    if (state == "n") arr = s_tvPower ? &kOn : &kOff;
    else if (state == "f") arr = &kFsale;
    else { const int k = atoi(state.c_str()); if (k < 0 || k > 5) return; arr = &kLoc[k]; }
    for (int i = 0; i < n && i < (int)s_scr.size(); ++i) { t += 200; s_scr[i] = Screen(); s_scr[i].arr = arr; s_scr[i].next = t; }   // notify + wait 0.2 + thread
    fprintf(stderr, "[H9V] box_indicator '%s' t=%d -> %s (%d pantallas)\n", state.c_str(), now, (*arr)[0].c_str(), n);
}
void Tick(Screen &s, int i, int now)   // magic_box_screen_swap
{
    while (s.arr && s.next >= 0 && now >= s.next) {
        if (s.logo) { s.logo = false; s.next += 1000; continue; }   // tras el logo: wait 1.0
        if (s.step < (int)s.arr->size()) { Set(i, (*s.arr)[s.step]); ++s.step; s.next += 3000; continue; }
        s.step = 0;
        if (6 > RandomInt(100)) { Set(i, kLogo[0]); s.logo = true; s.next += 2000; }
        else s.next += 1000;
    }
}
// cualquier '9' que recibe h7e_light.cpp (ServerCmd) se encola para las pantallas; no se consume (levelNotify sigue su camino)
bool SysState(const char *sys, const std::string &st, int)
{
    if (!sys || !s_tvOn) return false;
    std::lock_guard<std::mutex> l(s_mx); s_q.push_back({ sys, st });
    return false;
}
void ScreensFrame()
{
    static bool first = true;
    if (first) { first = false; const char *e = getenv("H9_SCREENS"); s_tvOn = !(e && !atoi(e)); }
    if (!s_tvOn || !H7E_ClientModelByTn) return;
    std::vector<std::pair<std::string, std::string>> q; { std::lock_guard<std::mutex> l(s_mx); q.swap(s_q); }
    const int now = level.time;
    std::string last;
    for (auto &e : q) {
        if (e.first + "=" + e.second == last) continue;   // el gancho de h7e puede ir encadenado dos veces: mismo comando repetido
        last = e.first + "=" + e.second;
        if (e.first == "levelNotify") { if (e.second == "ZPO") s_tvPower = true; continue; }   // pentagon_ZPO_listener: level._power_on
        if (e.first == "box_indicator") Switch(e.second, now);
    }
    for (size_t i = 0; i < s_scr.size(); ++i) Tick(s_scr[i], (int)i, now);
}

// ===================================================================================================== sonido (antes h5_audio_cl.cpp, OLA 14)
#ifdef H6SND
int s_ele[2], s_mini; bool s_eleOn[2], s_miniOn;
std::vector<int> s_tele, s_tvs; int s_telePack; bool s_packtime = true, s_teleFlag, s_powerOn;
std::vector<const float *> Origins(const char *tn, std::vector<std::vector<float>> &keep)   // origenes de las entidades con ese targetname
{
    std::vector<const float *> out;
    for (const H19Kv &k : H19_MapEnts()) { const char *t = H19_Str(k, "targetname"); if (!t || strcmp(t, tn)) continue;
        std::vector<float> o(3, 0.0f); const char *s = H19_Str(k, "origin"); if (s) sscanf(s, "%f %f %f", &o[0], &o[1], &o[2]); keep.push_back(o); }
    for (auto &o : keep) out.push_back(o.data());
    return out;
}
void AudioStart()
{
    const float zero[3] = { 0, 0, 0 };
    s_ele[0] = H19A_FakeAt(zero); s_ele[1] = H19A_FakeAt(zero); s_mini = H19A_FakeAt(zero);   // spawn(0, (0,0,0), "script_origin")
    std::vector<std::vector<float>> k1, k2, k3;
    std::vector<const float *> pack = Origins("pentagon_teleport_fx_pack", k1), tele = Origins("pentagon_teleport_fx", k2), tv = Origins("model_pentagon_box_screens", k3);
    if (!pack.empty()) { s_telePack = H19A_FakeAt(pack[0]); H19A_PlayLoop(s_telePack, "evt_teleporter_loop", 1.75f); }   // teleporter_fx_setup
    for (auto t : tele) s_tele.push_back(H19A_FakeAt(t));
    for (auto t : tv) s_tvs.push_back(H19A_FakeAt(t));
    fprintf(stderr, "[H6A] sistemas de cliente: portal del pack %s, %d portales, %d pantallas de la caja\n", pack.empty() ? "NO" : "si", (int)s_tele.size(), (int)s_tvs.size());
}
void AudioNotify(const std::string &n)
{
    const float zero[3] = { 0, 0, 0 };
    static const char *eleA[2][3] = { { "evt_elevator_office_start", "evt_elevator_office_run", "evt_elevator_office_stop" },
                                      { "evt_elevator_freight_start", "evt_elevator_freight_run", "evt_elevator_freight_stop" } };
    for (int e = 0; e < 2; ++e)
    {
        const std::string on = e ? "ele2" : "ele1", off = on + "e";
        if (n == on && !s_eleOn[e]) { H19A_WorldSound(zero, eleA[e][0]); H19A_PlayLoop(s_ele[e], eleA[e][1], 1); s_eleOn[e] = true; }
        else if (n == off && s_eleOn[e]) { H19A_StopLoop(s_ele[e], 1); H19A_WorldSound(zero, eleA[e][2]); s_eleOn[e] = false; }
    }
    if (n == "minis" && !s_miniOn) { H19A_PlayLoop(s_mini, "zmb_insta_kill_loop", 0); s_miniOn = true; }
    else if (n == "minie" && s_miniOn) { H19A_WorldSound(zero, "zmb_insta_kill"); H19A_StopLoop(s_mini, 0.5f); s_miniOn = false; }
    else if (n == "pack_portal_fx_off") { for (int f : s_tele) if (H19A_FakeLoopIs(f, "mus_packapunch_special")) H19A_StopLoop(f, 1); }
    else if (n == "ZPO") s_powerOn = true;
}
// zombie_pentagon.csc pentagon_screen_switch -> play_magic_box_tv_audio (en cada pantalla)
void AudioSys(const std::string &sys, const std::string &st)
{
    if (I_stricmp(sys.c_str(), "box_indicator")) return;
    const char *alias = st == "n" ? (s_powerOn ? "amb_tv_static" : nullptr) : st == "f" ? "mus_fire_sale" : "amb_tv_static";
    for (int f : s_tvs) { if (alias) H19A_PlayLoop(f, alias, 0.5f); else H19A_StopLoop(f, 0.5f); }
}
// register_clientflag_callback("player", 5, teleporter_fx_init): al cambiar la bandera del jugador local (ps.eFlags2)
void AudioFrame(bool testFlag)
{
    if (gentity_s *pl = H19A_PlayerEnt())
    {
        const bool f = testFlag || (pl->client && ((pl->client->ps.eFlags2 >> 5) & 1));
        if (f != s_teleFlag)
        {
            s_teleFlag = f;
            if (f && s_packtime) { s_packtime = false; for (int t : s_tele) { H19A_WorldSound(H19A_FakeOrg(t), "evt_teleporter_start"); H19A_PlayLoop(t, "evt_teleporter_loop", 1.75f); } }
            else if (!f && !s_packtime) { s_packtime = true; for (int t : s_tele) H19A_PlayLoop(t, "mus_packapunch_special", 1); }
        }
    }
}
void AudioStatus(std::string &out) { out += std::string("; portal ") + (s_packtime ? "pack" : "normal") + ", energia " + (s_powerOn ? "si" : "no"); }
#endif
} // namespace

extern const H19MapCl kH19_zombie_pentagon = {
    .zone = "zombie_pentagon", .offEnv = "H19_FIVE", .estado = "Five: luces/vision por planta, pantallas de la caja, ascensores y portales (OLA 7e/9/14)",
    .init = nullptr, .levelNotify = LevelNotify, .sysState = SysState, .frame = nullptr, .clockHand = nullptr, .screensFrame = ScreensFrame,
    .levelEffect = LevelEffect,
#ifdef H6SND
    .audioStart = AudioStart, .audioNotify = AudioNotify, .audioSys = AudioSys, .audioFrame = AudioFrame, .audioStatus = AudioStatus,
#endif
};
#endif
