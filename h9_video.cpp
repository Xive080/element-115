// OLA 9: "video" de las pantallas de Pentagon.
//
// Lo que hace el original (comprobado en los scripts del ff, no hay VM de CSC en el port):
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
// Aqui se reproduce eso con las mismas tablas y tiempos (level.time del servidor). El sonido de las pantallas (amb_tv_static /
// mus_fire_sale, play_magic_box_tv_audio) NO se reproduce todavia.
//   H9_SCREENS=0 lo desactiva (las pantallas se quedan con el modelo del mapa). Coste: ~0 (un cambio de modelo cada 3 s; los vertices
//   del modelo nuevo se deforman una vez y se cachean en h7e, ~unos KB por modelo).
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

int H7E_ClientModelByTn(const char *tn, int ordinal, const char *name) __attribute__((weak));   // h7e_light.cpp

namespace {
const char *kTn = "model_pentagon_box_screens";
const std::vector<std::string> kOff = { "p_zom_monitor_screen_off" }, kOn = { "p_zom_monitor_screen_on" },
    kFsale = { "p_zom_monitor_screen_fsale1", "p_zom_monitor_screen_fsale2" }, kLogo = { "p_zom_monitor_screen_logo" };
const std::vector<std::string> kLoc[6] = {
    { "p_zom_monitor_screen_lobby0", "p_zom_monitor_screen_lobby1" }, { "p_zom_monitor_screen_lobby0", "p_zom_monitor_screen_lobby2" },
    { "p_zom_monitor_screen_warroom0", "p_zom_monitor_screen_warroom1" }, { "p_zom_monitor_screen_labs0", "p_zom_monitor_screen_labs1" },
    { "p_zom_monitor_screen_labs0", "p_zom_monitor_screen_labs2" }, { "p_zom_monitor_screen_labs0", "p_zom_monitor_screen_labs3" } };

struct Screen { const std::vector<std::string> *arr = nullptr; int step = 0, next = -1; bool logo = false; };
std::vector<Screen> s_scr;
bool s_power, s_inited, s_on = true;
std::mutex s_mx; std::vector<std::pair<std::string, std::string>> s_q;   // (sistema, estado) recibidos por h7e_light.cpp (ServerCmd)
unsigned s_rng = 0x9E3779B9u;
int RandomInt(int n) { s_rng = s_rng * 1664525u + 1013904223u; return (int)((s_rng >> 8) % (unsigned)n); }

void Set(int i, const std::string &m) { if (H7E_ClientModelByTn) H7E_ClientModelByTn(kTn, i, m.c_str()); }

void Switch(const std::string &state, int now)   // pentagon_screen_switch
{
    const int n = H7E_ClientModelByTn ? H7E_ClientModelByTn(kTn, -1, nullptr) : 0; if (!n) return;
    int t = now;
    if (!s_inited) {
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
        s_inited = true; s_scr.assign(n, Screen()); for (int i = 0; i < n; ++i) Set(i, kOff[0]); t += 100 * n; }   // pentagon_tv_init
    const std::vector<std::string> *arr;
    if (state == "n") arr = s_power ? &kOn : &kOff;
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
} // namespace

// llamado por el '9' (GScr_ClientSysSetState) que ya recibe h7e_light.cpp: un solo gancho de comandos, sin cadena propia
void H9V_SysState(const char *sys, const char *state)
{
    if (!sys || !state || !s_on) return;
    std::lock_guard<std::mutex> l(s_mx); s_q.push_back({ sys, state });
}
void H9V_Frame()
{
    static bool first = true;
    if (first) { first = false; const char *e = getenv("H9_SCREENS"); s_on = !(e && !atoi(e)); }
    if (!s_on || !H7E_ClientModelByTn) return;
    std::vector<std::pair<std::string, std::string>> q; { std::lock_guard<std::mutex> l(s_mx); q.swap(s_q); }
    const int now = level.time;
    std::string last;
    for (auto &e : q) {
        if (e.first + "=" + e.second == last) continue;   // el gancho de h7e puede ir encadenado dos veces: mismo comando repetido
        last = e.first + "=" + e.second;
        if (e.first == "levelNotify") { if (e.second == "ZPO") s_power = true; continue; }   // pentagon_ZPO_listener: level._power_on
        if (e.first == "box_indicator") Switch(e.second, now);
    }
    for (size_t i = 0; i < s_scr.size(); ++i) Tick(s_scr[i], (int)i, now);
}
#else
void H9V_Frame() {}
void H9V_SysState(const char *, const char *) {}
#endif
