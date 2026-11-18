// OLA M3: cliente de zombie_cod5_prototype (Nacht der Untoten). Reproduce a mano sus clientscripts (rawfiles del ff); el servidor es el
// GSC original sin tocar (caja fija, armas de pared, escombros, puerta, barricadas, radio y huevo los lleva el GSC). Nacht NO tiene
// perros (su GSC no llama a enable_dog_rounds y el mapa no trae zombie_dog_spawner), ni electricidad, ni ventajas, ni Pack-a-Punch,
// ni trampas.
//   - vision: maps/createart/zombie_cod5_prototype_art.gsc hace VisionSetNaked("zombie_prototype", 0) y el servidor lo escribe en el
//     configstring 1550 (Scr_VisionSetNaked / G_SP_VisionSetNaked). El cliente original lo lee (CG_VisionSetConfigString_Naked); el
//     harness solo carga vision/<mapname>.vision, que aqui no existe. Se lee el CS 1550 y se aplica con H7_VisionSetNaked. Sin vision
//     el postfx queda apagado y con el el RESOLVED_POST_SUN que necesita el agua (world_h4).
//   - createfx (clientscripts/createfx/zombie_cod5_prototype_fx.csc + _fx.csc fx_init/oneshotfxthread/create_triggerfx): 112
//     oneshotfx. Se leen del rawfile (no se copian numeros): si delay > 0 se espera delay s; luego
//     playFx(0, fx, origin, anglestoforward, anglestoup, delay) con inicio delay*1000 ms (CScr_PlayFX con 6 parametros), es decir
//     delay -15 = el efecto ya llevaba 15 s al empezar el nivel. level._effect[fxid] sale de zombie_cod5_prototype_fx.csc (LoadFx).
//   - zombify (zombie_cod5_prototype.csc zombifyHandler/zombie_monitor): con estado "1" playfx(zombie_grain) en el origen del jugador
//     cada 0.1 s; "0" lo para.
//   - sonido (zombie_cod5_prototype_amb.csc): salas, reverb, contexto y musica ya los lleva h5_audio_cl (ParseAmb); aqui
//     addAmbientElement("amb_spooky_2d", 5, 8, 300, 2000) (_ambientpackage.csc ambientElementThread), play_fire_loops (fire_med) y la
//     radio de _waw_zombiemode_radio.csc (clientflag 14 del scriptmover "kzmb", ktr/rrd).
//   H19_NACHT=0 lo apaga. Pruebas: H19_NACHT_RADIO="ms,ms..." dispara "damage" en la radio en esos level.time (como dispararle).
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
#ifdef H6SND
#include <cgame/cg_sound.h>
#include <sound/snd_bank.h>
#include <sound/snd_public_async.h>
#endif
#include <zlib.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include "h19_mapcl.h"

bool H7_VisionSetNaked(const char *name, float seconds) __attribute__((weak));   // h7_gfx.cpp
void __cdecl SV_GetConfigstring(unsigned int index, char *buffer, int bufferSize);
void H4_GetCam(float *org, float *ang);                                         // world_h4.cpp
const FxEffectDef *__cdecl FX_Register(char *name);                             // h5_fx.cpp
extern FxSystemContainer *fx_systemPool;                                        // fx_system.cpp (lo reserva h5_fx)

namespace {
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
// "abc" a partir de p (primera cadena entre comillas)
bool Quoted(const std::string &t, size_t p, std::string &out, size_t *end = nullptr)
{
    size_t a = t.find('"', p); if (a == std::string::npos) return false;
    size_t b = t.find('"', a + 1); if (b == std::string::npos) return false;
    out = t.substr(a + 1, b - a - 1); if (end) *end = b + 1; return true;
}

// ---------------------------------------------------------------- vision (configstring 1550)
std::string s_csVis;   // ultimo valor visto
void VisionPoll()
{
    if (!H7_VisionSetNaked || !H19_EnvOn("H7E_VISION")) return;
    char cs[1024]; cs[0] = 0; SV_GetConfigstring(1550, cs, sizeof cs);
    if (s_csVis == cs) return;
    s_csVis = cs;
    fprintf(stderr, "[H19N] CS 1550 = '%s'\n", cs);
    // MP: '"nombre" ms'; SP (g_sp_vision): cadena info con una clave por cliente ("0" = jugador local)
    std::string v = cs;
    if (!v.empty() && v[0] == '\\') { char *x = Info_ValueForKey(cs, "0"); v = x ? x : ""; }
    // Info_SetValueForKey quita las comillas (no admite \\ ; "): en SP queda 'nombre ms'
    std::string name; size_t e = 0;
    if (!Quoted(v, 0, name, &e)) { size_t a = v.find_first_not_of(' '); if (a == std::string::npos) return;
        e = v.find(' ', a); name = v.substr(a, e == std::string::npos ? std::string::npos : e - a); if (e == std::string::npos) e = v.size(); }
    if (name.empty()) return;
    const int ms = atoi(v.c_str() + e);
    fprintf(stderr, "[H19N] vision del servidor (CS 1550): '%s' en %d ms\n", name.c_str(), ms);
    H7_VisionSetNaked(name.c_str(), ms / 1000.0f);
}

// ---------------------------------------------------------------- createfx
struct OneShot { float org[3], ang[3], delay; std::string fxid; bool done; };
std::vector<OneShot> s_fx;
std::map<std::string, std::string> s_effect;   // level._effect[fxid] -> ruta de LoadFx
int s_fxT0 = -1, s_fxPlayed, s_fxMissing;
void ParseEffects()
{
    const std::string t = Raw("clientscripts/zombie_cod5_prototype_fx.csc"), l = Lower(t);
    size_t p = 0;
    while ((p = l.find("level._effect[", p)) != std::string::npos) {
        std::string key, path; size_t e = 0;
        if (!Quoted(t, p, key, &e)) break;
        size_t eq = t.find('=', e), lf = l.find("loadfx", e), nl = t.find('\n', e);
        p = e;
        if (eq == std::string::npos || lf == std::string::npos || (nl != std::string::npos && lf > nl)) continue;
        if (Quoted(t, lf, path)) s_effect[key] = path;
    }
}
void ParseCreateFx()
{
    const std::string t = Raw("clientscripts/createfx/zombie_cod5_prototype_fx.csc");
    size_t p = 0; OneShot cur{}; bool open = false;
    auto flush = [&]() { if (open && !cur.fxid.empty()) s_fx.push_back(cur); cur = OneShot{}; open = false; };
    while (p < t.size()) {
        size_t q = t.find('\n', p); if (q == std::string::npos) q = t.size();
        const std::string ln = t.substr(p, q - p); p = q + 1;
        if (ln.find("createOneshotEffect") != std::string::npos) { flush(); open = true; cur.delay = -15; continue; }   // _fx.csc createOneshotEffect
        size_t k = ln.find("ent.v[");
        if (!open || k == std::string::npos) continue;
        std::string key, val; size_t e = 0; if (!Quoted(ln, k, key, &e)) continue;
        size_t eq = ln.find('=', e); if (eq == std::string::npos) continue;
        const char *r = ln.c_str() + eq + 1;
        if (key == "origin" || key == "angles") { float *d = key == "origin" ? cur.org : cur.ang; sscanf(r, " ( %f , %f , %f )", &d[0], &d[1], &d[2]); }
        else if (key == "delay") cur.delay = (float)atof(r);
        else if (key == "fxid") { if (Quoted(ln, eq, val)) cur.fxid = val; }
        else if (key == "type") { if (Quoted(ln, eq, val) && val != "oneshotfx") open = false; }
    }
    flush();
}
void CreateFxFrame()
{
#ifdef H5FX
    // h5_fx inicia el FxSystem en su primer frame (FX_InitSystem, dvars fx_*): antes FX_SpawnEffect cae. Hasta que el reloj de FX
    // corre no se lanza nada; t0 = ese primer instante (el original lo hace en fx_init, al empezar el nivel)
    if (!fx_systemPool || s_fx.empty()) return;
    const int now = FX_GetSystem(0)->system.msecNow;
    if (s_fxT0 < 0) { if (now <= 0) return; s_fxT0 = now; fprintf(stderr, "[H19N] createfx: reloj de FX en marcha, t0=%d\n", now); }
    for (OneShot &o : s_fx) {
        if (o.done) continue;
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
    if (!logged) { bool all = true; for (const OneShot &o : s_fx) all = all && o.done;
        if (all) { logged = true; fprintf(stderr, "[H19N] createfx: %d de %zu efectos lanzados (%d sin FX en los datos)\n", s_fxPlayed, s_fx.size(), s_fxMissing); } }
#endif
}

// ---------------------------------------------------------------- zombify
bool s_zombify; int s_grainAt;

void Init(const std::vector<H19Kv> &)
{
    VisionPoll();
    ParseEffects(); ParseCreateFx();
    fprintf(stderr, "[H19N] Nacht: %zu level._effect, %zu oneshotfx de createfx\n", s_effect.size(), s_fx.size());
}
bool SysState(const char *sys, const std::string &st, int now)
{
    if (strcmp(sys, "zombify")) return false;
    const bool on = st == "1";
    if (on != s_zombify) fprintf(stderr, "[H19N] zombify %s t=%d\n", on ? "on" : "off", now);
    if (st == "1" || st == "0") s_zombify = on;
    s_grainAt = now;
    return true;
}
void Frame(int now)
{
    VisionPoll();
    CreateFxFrame();
    if (s_zombify && now >= s_grainAt) {   // zombie_monitor: playfx(zombie_grain, self.origin); realwait(0.1)
        s_grainAt = now + 100;
        auto it = s_effect.find("zombie_grain");
        static const float fwd[3] = { 1, 0, 0 }, up[3] = { 0, 0, 1 };
        if (it != s_effect.end()) H19_PlayFx(it->second.c_str(), g_entities[0].r.currentOrigin, fwd, up);
    }
}

// ---------------------------------------------------------------- sonido
#ifdef H6SND
unsigned s_rng = 0x6e616368u;   // secuencia propia (no gasta la de fire_flicker)
float RandF(float a, float b) { s_rng = s_rng * 1664525u + 1013904223u; return a + (b - a) * ((s_rng >> 8) / 16777216.0f); }
int RandI(int a, int b) { if (b <= a) return a; const int v = a + (int)RandF(0, (float)(b - a)); return v < b ? v : b - 1; }   // randomintrange: [a, b)
int Play(const float *org, const char *alias)
{
    float p[3]; if (org) { p[0] = org[0]; p[1] = org[1]; p[2] = org[2]; }
    return CG_PlaySoundWithHandle(0, 1022, org ? p : nullptr, 0, 0, 1.0f, SND_FindAliasId(alias));
}

// addAmbientElement("<sala>", "amb_spooky_2d", 5, 8, 300, 2000): las tres salas llevan el mismo elemento, asi que un solo hilo
double s_ambAt = -1, s_ambStart; int s_ambId = -1, s_ambPlays;
void AmbientElement(double now)
{
    if (s_ambId >= 0) {   // espera a que acabe (soundId != -1); con duracion conocida, esa; tope de 20 s por si el dispositivo no avisa
        int len = 0; const bool known = SND_GetKnownLength(s_ambId, &len) && len > 0;
        if ((known ? now < s_ambStart + len : SND_IsPlaying(s_ambId)) && now < s_ambStart + 20000.0) return;
        s_ambId = -1; s_ambAt = now + RandF(5, 8) * 1000.0;
    }
    if (s_ambAt < 0) { s_ambAt = now + RandF(5, 8) * 1000.0; return; }
    if (now < s_ambAt) return;
    float eye[3], ang[3]; H4_GetCam(eye, ang);
    const float dist = (float)RandI(300, 2000), a = (float)RandI(0, 359) + ang[1];
    float f[3], yaw[3] = { 0, a, 0 }; AngleVectors(yaw, f, nullptr, nullptr);
    const float pos[3] = { eye[0] + f[0] * dist, eye[1] + f[1] * dist, eye[2] + f[2] * dist };
    s_ambId = Play(pos, "amb_spooky_2d"); s_ambStart = now; ++s_ambPlays;
    if (s_ambId < 0) s_ambAt = now + RandF(5, 8) * 1000.0;
}

// _waw_zombiemode_radio.csc
const char *const kSongs[] = { "wtf", "abra_macabre", "uhf", "dusk", "underwater", "slasher_flick", "maskwalk", "sand", "temple", "" };
int s_radioEnt = -2, s_radioIdx, s_radioId = -1, s_radioPlays; bool s_radioFlag, s_stopRadio; double s_songAt = -1, s_stopAt = -1; int s_stopId = -1;
void FindRadio()
{
    s_radioEnt = -1;
    for (int e = 0; e < level.num_entities; ++e) { gentity_s *ent = &g_entities[e];
        if (ent->r.inuse && ent->targetname && !strcmp(SL_ConvertToString(ent->targetname, SCRIPTINSTANCE_SERVER), "kzmb")) { s_radioEnt = e; break; } }
    fprintf(stderr, "[H19N] radio kzmb: %s\n", s_radioEnt >= 0 ? "encontrada" : "NO esta");
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
    if (const char *e = getenv("H19_NACHT_RADIO")) {   // prueba: el jugador le dispara (zombie_radio_play: waittill("damage"))
        static std::vector<int> q; static bool parsed;
        if (!parsed) { parsed = true; for (const char *p = e; *p; ) { q.push_back(atoi(p)); const char *c = strchr(p, ','); if (!c) break; p = c + 1; } }
        for (int &t : q) if (t >= 0 && level.time >= t) { t = -1; Scr_Notify(ent, scr_const.damage, 0); fprintf(stderr, "[H19N] H19_NACHT_RADIO: damage en la radio t=%d\n", level.time); }
    }
    const bool f = (ent->s.lerp.eFlags2 >> 14) & 1;
    if (f && !s_radioFlag && !s_stopRadio) NextSong(now, ent->r.currentOrigin);
    s_radioFlag = f;
    if (s_stopAt >= 0 && now >= s_stopAt) { SND_StopPlayback(s_stopId); s_stopAt = -1; s_stopId = -1; }
    if (s_songAt >= 0 && now >= s_songAt) {
        s_songAt = -1;
        const char *song = kSongs[s_radioIdx];
        s_radioId = *song ? Play(ent->r.currentOrigin, song) : -1; ++s_radioPlays;
        fprintf(stderr, "[H19N] radio: cancion %d '%s' (id %d) t=%d\n", s_radioIdx, song, s_radioId, level.time);
        s_radioIdx = (s_radioIdx + 1) % (int)(sizeof kSongs / sizeof *kSongs);
    }
}

int s_fireEnt = -1;
void AudioStart()
{
    static const float fire[3] = { 164.8f, -63.5f, 127.1f };   // play_fire_loops: playloopat(0, "fire_med", (164.8, -63.5, 127.1))
    s_fireEnt = H19A_FakeAt(fire); H19A_PlayLoop(s_fireEnt, "fire_med", 0);
}
void AudioNotify(const std::string &n)
{
    if (n == "ktr") { s_stopRadio = true; if (s_radioId >= 0) SND_StopPlayback(s_radioId); s_songAt = -1; fprintf(stderr, "[H19N] radio: ktr (para)\n"); }
    else if (n == "rrd") { s_stopRadio = false; fprintf(stderr, "[H19N] radio: rrd (sigue)\n"); }
}
void AudioFrame(bool)
{
    const double now = level.time;   // wait()/realwait() del CSC: tiempo de juego (el harness puede ir mas rapido o lento que el reloj)
    AmbientElement(now);
    RadioFrame(now);
}
void AudioStatus(std::string &out)
{
    char b[160]; snprintf(b, sizeof b, " nacht: spooky=%d radio=%d/%s fuego=%d", s_ambPlays, s_radioPlays, s_stopRadio ? "parada" : "lista", s_fireEnt);
    out += b;
}
#endif
} // namespace

extern const H19MapCl kH19_zombie_cod5_prototype = {
    .zone = "zombie_cod5_prototype", .offEnv = "H19_NACHT", .estado = "Nacht der Untoten: vision CS1550, createfx, zombify, spooky, fuego y radio (OLA M3)",
    .init = Init, .levelNotify = nullptr, .sysState = SysState, .frame = Frame, .clockHand = nullptr, .screensFrame = nullptr,
    .levelEffect = nullptr,
#ifdef H6SND
    .audioStart = AudioStart, .audioNotify = AudioNotify, .audioSys = nullptr, .audioFrame = AudioFrame, .audioStatus = AudioStatus,
#endif
    .doorEnt = nullptr,
};
#endif
