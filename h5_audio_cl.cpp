// H6a (sonido): la parte de CLIENTE que el sistema de sonido ORIGINAL necesita, sobre el servidor dedicado del harness (no hay
// centities ni cgame entero). Lo que hace:
//   - copias de cgame/cg_sound.cpp (CG_PlaySound, CG_PlaySoundWithHandle, CG_SndEntHandle, SND_EntHandle, CG_SoundEntInUse,
//     CG_SoundGetUseCount, CG_GetSoundEntityOrientation, CG_StopSoundAlias, CG_SndUpdate/CG_SndGameReset...) con la misma
//     logica, leyendo de g_entities (el estado que el cliente recibiria en el snapshot) en lugar de cg_entities;
//   - CG_ScriptSndLengthNotify: el "sl <ent> <msec>" que el original manda al servidor se aplica directo, como
//     G_SP_Cmd_SoundLength_f (game_sp/g_sp_ext.cpp), para que "sounddone" llegue cuando acaba el sonido de verdad;
//   - eventos tras cada frame de servidor con la semantica de CG_CheckPlayerstateEvents / CG_CheckEvents / CG_EntityEvent
//     (cgame/cg_event.cpp): playsound (EV_SOUND_ALIAS[_NOTIFY], temp ents), stop, disparo (CG_WeaponFireSingle ORIGINAL de
//     cg_weapons.cpp), recarga/armas/pickup, granadas, pasos/salto/caida (el script de cliente _footsteps.csc emulado tal cual:
//     "fly_[q]step_<mov>_plr_<suelo>", puerta de bobCycle de CG_DoFootstepEvent);
//   - bucles: s.loopSoundId de las entidades (CG_PumpEntityLoopSound) y entidades falsas de script de cliente (playloopsound);
//   - ambiente y musica de los scripts de cliente (no hay VM de cliente): se lee el rawfile clientscripts/<mapa>_amb.csc y se
//     reproduce lo que hacen _ambientpackage.csc (tono de sala por playloopsound + setReverb + setAmbientRoomContext) y
//     _music.csc (musicCmd: el '9' de ClientSysSetState del servidor, transitionOut/transitionIn de los estados declarados);
//   - Sys_AddWorkerCmdInternal / Sys_AssistAndWaitWorkerCmdInternal: el trabajador "updatesound" corre SND_Frame en el acto
//     (el harness es de un hilo; la mezcla va en el hilo del dispositivo, h5_audio.cpp).
// Se engancha a h5_play.cpp por g_h5pAudio (una llamada por frame de render).
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <qcommon/common.h>
#include <server_mp/server_mp.h>
#include <server_mp/sv_init_mp.h>
#include <bgame/bg_weapons_ammo.h>
#include <clientscript/scr_const.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <game_sp/g_sp_ext.h>
#include <bgame/bg_weapons.h>
#include <bgame/bg_weapons_def.h>
#include <bgame/bg_misc.h>
#include <game/actor.h>
#include <clientscript/cscr_stringlist.h>
#include <cgame/cg_sound.h>
#include <cgame/cg_weapons.h>
#include <cgame_mp/cg_ents_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <cgame_mp/cg_main_mp.h>
#include <sound/snd_public_async.h>
#include <sound/snd_bank.h>
#include <sound/snd_driver_xaudio2.h>
#include <universal/surfaceflags.h>
#include <universal/com_workercmds.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <database/database.h>
#include <database/db_registry.h>
#include <zlib.h>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <set>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <sound/snd.h>
#include <sound/snd_db.h>
#include <sound/snd_dvar.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <qcommon/cm_trace.h>
#include <qcommon/cm_mapkit.h>
#include <qcommon/cm_tracebox.h>
#include <qcommon/cm_load.h>
#include <qcommon/cm_load_obj.h>
#include <math.h>
#include "world_h4.h"
#include "h19_mapcl.h"   // OLA M2: sonido de cada mapa (maps/h19_<zona>.cpp)
#ifdef H6SND
#include "h6a_cgsnd.inc"   // OLA 14 (build.sh): CG_RegisterSounds/SurfaceType/ImpactType (cg_main_mp.cpp) y CG_SoundWhizby[Path] (cg_sound.cpp) tal cual
#endif
extern SndBank *g_snd_banks[SND_MAX_BANKS];
extern unsigned int g_snd_bankCount;
bool FS_H6A_Locate(const char *filename, int *fd, unsigned long long *off, unsigned int *size);   // iwd.cpp (OLA 15C: censo de streams)
__attribute__((weak)) void H10_CfgApplySound();   // h10_settings.cpp (weak: sin H10 no hay config de ajustes)

extern void (*g_h5pAudio)(bool serverFrame, const float *camOrg, const float *camAng);   // h5_play.cpp
extern bool g_h5pPause;
extern void (*g_h0ServerCmdHook)(const client_t *client, const char *cmd);              // server_mp/sv_main_mp.cpp
extern jqWorkerCmd updatesound_workerWorkerCmd;                                           // sound/snd_public_async.cpp
void H6A_CacheInfo(unsigned *files, unsigned long long *bytes);
void H6A_WmaInfo(unsigned *hit, unsigned *miss);
void __cdecl CG_SndAutoSimReset();                           // h5_audio.cpp

namespace {
double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------- entidades falsas de script de cliente (spawnfakeent)
const int FAKE_BASE = 1024, FAKE_MAX = 512;
struct FakeEnt { bool inuse; unsigned loopSoundId; int loopSoundFade; float org[3]; };
FakeEnt s_fake[FAKE_MAX];
int FakeSpawn() { for (int i = 1; i < FAKE_MAX; ++i) if (!s_fake[i].inuse) { memset(&s_fake[i], 0, sizeof s_fake[i]); s_fake[i].inuse = true; return i; } return 0; }

float s_prevOrg[MAX_GENTITIES_SV][3], s_curOrg[MAX_GENTITIES_SV][3];   // origen en los dos ultimos frames de servidor (velocidad = delta por snapshot)
int s_entSeq[MAX_GENTITIES_SV], s_entUse[MAX_GENTITIES_SV], s_tempTime[MAX_GENTITIES_SV]; bool s_entSeen[MAX_GENTITIES_SV];
int s_psPred = -1, s_psUnpred = -1, s_playerNum = -1;
bool s_init, s_initFailed;
int s_events, s_plays, s_impacts, s_whizChecks;
double s_lastFrameMs;

gentity_s *PlayerEnt()
{
    for (int i = 0; i < com_maxclients->current.integer; ++i)
    {
        client_t *c = &svs.clients[i];
        if (c->header.state == CS_ACTIVE && c->bIsTestClient && c->gentity && c->gentity->client) return c->gentity;
    }
    return nullptr;
}

unsigned AliasOf(const PTR32(const char) &p) { const char *s = p.get(); return s && *s ? SND_FindAliasId(s) : 0; }

// origen de sonido de una entidad: el de CG_PlaySound (centro de los bounds para un brushmodel, pose.origin si no)
void EntOrigin(int n, float *out)
{
    if (n >= FAKE_BASE && n < FAKE_BASE + FAKE_MAX && EntNum_IsCsc(n)) { memcpy(out, s_fake[n - FAKE_BASE].org, 12); return; }
    if (n < 0 || n >= MAX_GENTITIES_SV) { out[0] = out[1] = out[2] = 0; return; }
    const gentity_s *e = &g_entities[n];
    if (e->s.solid == 0xFFFFFF && e->r.bmodel)
        for (int k = 0; k < 3; ++k) out[k] = (e->r.absmin[k] + e->r.absmax[k]) * 0.5f;
    else memcpy(out, e->r.currentOrigin, 12);
}

// ---------------------------------------------------------------- ambiente y musica (scripts de cliente del mapa)
struct Room { std::string tone, reverb, snapshot; float fadeIn = 2, fadeOut = 2, dry = 1, wet = 0.5f, rvFade = 2; bool hasReverb = false;
              std::vector<std::pair<std::string, std::string>> context;
              int ent = 0, id = -1; std::map<int, int> refcount; };   // declareAmbientRoom: spawnfakeent(0) propio; prioridad -> cuenta
// OLA 14: disparadores "ambient_package" del mapa (_ambientpackage.csc ambientPackageTrigger + trigger_thread de _utility.csc)
struct AmbTrig { unsigned model; float org[3]; std::string room; int prio; bool inside = false; };
std::vector<AmbTrig> s_ambTrigs;
std::set<std::string> s_ambContexts;   // level.ambientContexts: todos los tipos que algun setAmbientRoomContext usa
struct MusicState { std::string loopalias, oneshotalias; float fadein = 0, fadeout = 0; int ent = 0; int id = -1; };
std::map<std::string, Room> s_rooms;
std::map<std::string, MusicState> s_music;
std::string s_activeRoom, s_musicActive, s_musicSys, s_curRoom;
struct PendingStop { int id; double atMs; };
std::vector<PendingStop> s_pendingStops;

std::vector<std::string> SplitArgs(const std::string &a)
{
    std::vector<std::string> out; std::string cur; bool q = false;
    for (char c : a)
    {
        if (c == '"') { q = !q; continue; }
        if (c == ',' && !q) { out.push_back(cur); cur.clear(); continue; }
        if (!q && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) continue;
        cur += c;
    }
    if (!cur.empty() || !out.empty()) out.push_back(cur);
    return out;
}

std::string RawText(const char *name)
{
    RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)name, 0, -1).rawfile;
    if (!rf || !rf->buffer.get()) return {};
    const unsigned *hd = (const unsigned *)rf->buffer.get(); uLongf outlen = hd[0];
    std::vector<unsigned char> out(outlen + 1);
    if (uncompress(out.data(), &outlen, (const unsigned char *)(hd + 2), hd[1]) != Z_OK) return {};
    return std::string((const char *)out.data(), outlen);
}

// H6A_DUMPRAW=<dir>: vuelca los rawfiles de scripts (clientscripts/*.csc y maps/*.gsc) descomprimidos, para estudiar lo que hacen
// los scripts de cliente que aqui se emulan (no hay VM de cliente). Solo depuracion.
} // namespace
void DB_EnumXAssets(XAssetType type, void (*func)(XAssetHeader, void *), void *in, bool ov);   // rt2.cpp
namespace {
void DumpRawOne(XAssetHeader h, void *ctx)
{
    const RawFile *rf = h.rawfile; const char *dir = (const char *)ctx;
    if (!rf || !rf->name.get() || !rf->buffer.get()) return;
    const char *n = rf->name.get();
    if (strncmp(n, "clientscripts/", 14) && strncmp(n, "maps/", 5)) return;
    std::string t = RawText(n);
    std::string out = std::string(dir) + "/"; for (const char *p = n; *p; ++p) out += (*p == '/' || *p == '\\') ? '_' : *p;
    if (FILE *f = fopen(out.c_str(), "wb")) { if (!t.empty()) fwrite(t.data(), 1, t.size(), f); else fwrite(rf->buffer.get(), 1, rf->len > 0 ? rf->len : 0, f); fclose(f); }
}

const MapEnts *MapEntsOf()
{
    MapEnts *me = nullptr;
    DB_EnumXAssets_FastFile(ASSET_TYPE_MAP_ENTS, [](XAssetHeader h, void *c) { if (!*(MapEnts **)c) *(MapEnts **)c = h.mapEnts; }, &me, false);
    return me;
}

void ParseAmb(const std::string &raw)
{
    // fuera comentarios // y /* */ y bloques de desarrollo /# #/
    std::string s; s.reserve(raw.size());
    for (size_t i = 0; i < raw.size();)
    {
        if (raw.compare(i, 2, "//") == 0) { while (i < raw.size() && raw[i] != '\n') ++i; continue; }
        if (raw.compare(i, 2, "/*") == 0) { size_t e = raw.find("*/", i + 2); i = e == std::string::npos ? raw.size() : e + 2; continue; }
        if (raw.compare(i, 2, "/#") == 0) { size_t e = raw.find("#/", i + 2); i = e == std::string::npos ? raw.size() : e + 2; continue; }
        s += raw[i++];
    }
    std::string musicDeclare; bool roomActivated = false;
    static const char *fns[] = { "declareAmbientRoom", "setAmbientRoomTone", "setAmbientRoomReverb", "setAmbientRoomContext",
        "activateAmbientRoom", "declareMusicState", "musicAliasloop", "musicAlias", "setAmbientRoomSnapshot" };
    size_t i = 0;
    while (i < s.size())
    {
        size_t best = std::string::npos; int bf = -1;
        for (int f = 0; f < 9; ++f)
        {
            // sin distinguir mayusculas (GSC no distingue) y con el nombre completo antes del parentesis
            size_t p = i;
            for (;;)
            {
                size_t k = std::string::npos; const size_t n = strlen(fns[f]);
                for (size_t j = p; j + n <= s.size(); ++j) if (!strncasecmp(s.c_str() + j, fns[f], n)) { k = j; break; }
                if (k == std::string::npos) break;
                size_t a = k + n; while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
                const bool okPrev = k == 0 || !(isalnum((unsigned char)s[k - 1]) || s[k - 1] == '_');
                if (a < s.size() && s[a] == '(' && okPrev) { if (k < best) { best = k; bf = f; } break; }
                p = k + 1;
            }
        }
        if (bf < 0) break;
        const size_t a0 = s.find('(', best), a1 = s.find(')', a0);
        if (a1 == std::string::npos) break;
        std::vector<std::string> v = SplitArgs(s.substr(a0 + 1, a1 - a0 - 1));
        i = a1 + 1;
        auto F = [&](size_t k, float d) { return k < v.size() && !v[k].empty() ? (float)atof(v[k].c_str()) : d; };
        switch (bf)
        {
        case 0: if (v.size() >= 1) s_rooms[v[0]]; break;
        case 1: if (v.size() >= 2) { Room &r = s_rooms[v[0]]; r.tone = v[1]; r.fadeIn = F(2, 2); r.fadeOut = F(3, 2); if (r.fadeIn < 0) r.fadeIn = 2; if (r.fadeOut < 0) r.fadeOut = 2; } break;
        case 2: if (v.size() >= 4) { Room &r = s_rooms[v[0]]; r.reverb = v[1]; r.dry = F(2, 1); r.wet = F(3, 0.5f); r.rvFade = F(4, 2); if (r.rvFade < 0) r.rvFade = 2; r.hasReverb = true; } break;
        case 3: if (v.size() >= 3) { s_rooms[v[0]].context.push_back({ v[1], v[2] }); s_ambContexts.insert(v[1]); } break;
        case 4: if (v.size() >= 2) { ++s_rooms[v[1]].refcount[(int)F(2, 0)]; if (!roomActivated) { s_activeRoom = v[1]; roomActivated = true; } } break;   // activateAmbientRoom(0, room, prio)
        case 8: if (v.size() >= 2) s_rooms[v[0]].snapshot = v[1]; break;
        case 5: if (v.size() >= 1) { musicDeclare = v[0]; s_music[musicDeclare]; } break;
        case 6: if (v.size() >= 1 && !musicDeclare.empty()) { MusicState &m = s_music[musicDeclare]; m.loopalias = v[0]; m.fadein = F(1, 0); m.fadeout = F(2, 0); } break;
        case 7: if (v.size() >= 1 && !musicDeclare.empty()) { MusicState &m = s_music[musicDeclare]; m.oneshotalias = v[0]; m.fadein = 0; m.fadeout = F(1, 0); } break;
        }
    }
}

// playloopsound(0, ent, alias, fade) de cg_scr_main.cpp (CScr_PlayLoopSound): estado de la entidad falsa + primera peticion
int FakePlayLoop(int fe, const char *alias, float fadeSec)
{
    FakeEnt &f = s_fake[fe];
    f.loopSoundId = SND_FindAliasId(alias);
    f.loopSoundFade = (int)(fadeSec * 1000.0f);
    return CG_PlaySoundWithHandle(0, FAKE_BASE + fe, nullptr, (__int16)f.loopSoundFade, 0, 1.0f, f.loopSoundId);
}
// stoploopsound(0, ent, fade) (CScr_StopLoopSound): fundido negativo que CG_ProcessFakeEntity cuenta hasta 0
void FakeStopLoop(int fe, float fadeSec)
{
    FakeEnt &f = s_fake[fe];
    const float ms = fadeSec * 1000.0f;
    if (ms == 0.0f) { f.loopSoundId = 0; f.loopSoundFade = 0; }
    else f.loopSoundFade = (int)-ms;
}

// _ambientpackage.csc findHighestPriorityAmbientRoom: la sala con cuenta > 0 y prioridad mas alta (estricta: empata la primera)
std::string HighestRoom()
{
    std::string room; int prio = -1;
    for (auto &r : s_rooms) for (auto &pc : r.second.refcount) if (pc.second > 0 && pc.first > prio) { room = r.first; prio = pc.first; }
    return room;
}
// _ambientpackage.csc updateActiveAmbientRoom (una vuelta): mismo tono = se intercambian las entidades y sigue sonando; si no,
// fundido cruzado (playloopsound del nuevo, stoploopsound del viejo); reverb de nivel, instantanea ambiente y contextos de la sala
void RoomUpdate()
{
    const std::string nr = HighestRoom();
    if (nr == s_curRoom) return;
    Room *o = s_curRoom.empty() || !s_rooms.count(s_curRoom) ? nullptr : &s_rooms[s_curRoom];
    Room *n = nr.empty() ? nullptr : &s_rooms[nr];
    if (o && n && !o->tone.empty() && o->tone == n->tone) std::swap(o->ent, n->ent);
    else
    {
        if (n && !n->tone.empty()) n->id = FakePlayLoop(n->ent, n->tone.c_str(), n->fadeIn);
        if (o && !o->tone.empty()) FakeStopLoop(o->ent, o->fadeOut);
    }
    if (!n || !n->hasReverb) SND_DeactivateEnvironmentEffects(1, 2000);
    else SND_SetEnvironmentEffects(1, n->reverb.c_str(), n->dry, n->wet, n->rvFade * 1000.0f > 0 ? (int)(n->rvFade * 1000.0f) : 0);
    SND_SetSnapshot(SND_SNAPSHOT_AMBIENT, n && !n->snapshot.empty() ? n->snapshot.c_str() : "default", 0.0f, 1.0f);
    if (n && !s_ambContexts.empty())
    {
        for (auto &c : s_ambContexts) SND_SetContext(c.c_str(), "");
        for (auto &c : n->context) SND_SetContext(c.first.c_str(), c.second.c_str());
    }
    fprintf(stderr, "[H6A] ambiente t=%d: sala '%s' -> '%s' (tono '%s', reverb '%s', instantanea '%s')\n", level.time, s_curRoom.c_str(), nr.c_str(),
        n ? n->tone.c_str() : "", n && n->hasReverb ? n->reverb.c_str() : "-", n && !n->snapshot.empty() ? n->snapshot.c_str() : "default");
    s_curRoom = nr;
}

// Disparadores del mapa: trigger_multiple con targetname "ambient_package" (script_ambientroom, script_ambientpriority 1 por defecto)
void LoadAmbTriggers()
{
    const MapEnts *me = MapEntsOf();
    if (!me) return;
    const char *str = me->entityString.get(); const int n = me->numEntityChars;
    std::map<std::string, std::string> kv; bool in = false;
    for (int i = 0; i < n;)
    {
        const char c = str[i];
        if (c == '{') { kv.clear(); in = true; ++i; continue; }
        if (c == '}')
        {
            ++i; if (!in) continue; in = false;
            if (kv["targetname"] != "ambient_package" || kv["script_ambientroom"].empty()) continue;   // los paquetes (sonidos sueltos) no los usa ningun mapa de zombis
            AmbTrig t; t.room = kv["script_ambientroom"]; t.prio = kv.count("script_ambientpriority") ? atoi(kv["script_ambientpriority"].c_str()) : 1;
            const std::string &m = kv["model"]; t.model = m.size() > 1 && m[0] == '*' ? (unsigned)atoi(m.c_str() + 1) : 0;
            t.org[0] = t.org[1] = t.org[2] = 0; sscanf(kv["origin"].c_str(), "%f %f %f", &t.org[0], &t.org[1], &t.org[2]);
            if (!t.model || t.model >= cm.numSubModels) { fprintf(stderr, "[H6A] ambiente: disparador de '%s' sin modelo de colision valido (%s)\n", t.room.c_str(), m.c_str()); continue; }
            if (!s_rooms.count(t.room)) { fprintf(stderr, "[H6A] ambiente: disparador de la sala '%s' no declarada\n", t.room.c_str()); continue; }
            s_ambTrigs.push_back(t);
            continue;
        }
        if (c == '"' && in)
        {
            std::string k, v; ++i; while (i < n && str[i] != '"') k += str[i++]; ++i;
            while (i < n && str[i] != '"') ++i; if (i >= n) break; ++i; while (i < n && str[i] != '"') v += str[i++]; ++i;
            kv[k] = v; continue;
        }
        ++i;
    }
}

// istouching del jugador con el disparador: la caja del jugador contra las brochas del modelo (traza de caja sin recorrido)
bool Touching(const AmbTrig &t, const float *org, const float *mins, const float *maxs)
{
    trace_t tr; memset(&tr, 0, sizeof tr);
    const float zero[3] = { 0, 0, 0 };
    CM_TransformedBoxTrace(&tr, org, org, mins, maxs, t.model, -1, t.org, zero);
    return tr.startsolid || tr.allsolid;
}

// H6A_AMBTEST=<ms> (solo prueba): en vez de la posicion del jugador se usa el centro de cada disparador por turnos, <ms> en cada
// uno y otro tanto fuera de todos; asi se oyen/miden los cambios de sala con la geometria real sin depender del piloto.
void AmbTriggerFrame()
{
    const gentity_s *pl = PlayerEnt();
    if (!pl) return;
    static const int testMs = getenv("H6A_AMBTEST") ? atoi(getenv("H6A_AMBTEST")) : 0;
    const float *org = pl->r.currentOrigin;
    if (testMs > 0 && !s_ambTrigs.empty())
    {
        const int slot = (level.time / testMs) % (2 * (int)s_ambTrigs.size());
        if (slot & 1) org = s_ambTrigs[slot / 2].org;
    }
    bool changed = false;
    for (auto &t : s_ambTrigs)
    {
        const bool in = Touching(t, org, pl->r.mins, pl->r.maxs);
        if (in == t.inside) continue;
        t.inside = in; changed = true;
        int &rc = s_rooms[t.room].refcount[t.prio];   // trig_enter / trig_leave: activateAmbientRoom / deactivateAmbientRoom
        if (in) ++rc; else if (rc > 0) --rc;
    }
    if (changed) RoomUpdate();
}

void AmbientStart()
{
    if (s_rooms.empty()) { fprintf(stderr, "[H6A] ambiente: el script del mapa no declara salas\n"); return; }
    for (auto &r : s_rooms) r.second.ent = FakeSpawn();
    LoadAmbTriggers();
    RoomUpdate();
    int selfOk = 0; const float mn0[3] = { -15, -15, 0 }, mx0[3] = { 15, 15, 72 };   // autocomprobacion: cada disparador responde en su centro
    for (auto &t : s_ambTrigs) selfOk += Touching(t, t.org, mn0, mx0) ? 1 : 0;
    fprintf(stderr, "[H6A] ambiente: %d de %d disparadores responden a la caja del jugador en su centro\n", selfOk, (int)s_ambTrigs.size());
    std::map<std::string, int> per; for (auto &t : s_ambTrigs) ++per[t.room];
    std::string lst; for (auto &p : per) lst += " " + p.first + "x" + std::to_string(p.second);
    float mn[3], mx[3]; if (!s_ambTrigs.empty()) CM_ModelBounds(s_ambTrigs[0].model, mn, mx);
    fprintf(stderr, "[H6A] ambiente: %d salas, %d contextos, %d estados de musica; %d disparadores de sala:%s (el 1o: limites %.0f %.0f %.0f / %.0f %.0f %.0f sobre origen %.0f %.0f %.0f)\n",
        (int)s_rooms.size(), (int)s_ambContexts.size(), (int)s_music.size(), (int)s_ambTrigs.size(), lst.c_str(),
        s_ambTrigs.empty() ? 0 : mn[0], s_ambTrigs.empty() ? 0 : mn[1], s_ambTrigs.empty() ? 0 : mn[2], s_ambTrigs.empty() ? 0 : mx[0],
        s_ambTrigs.empty() ? 0 : mx[1], s_ambTrigs.empty() ? 0 : mx[2], s_ambTrigs.empty() ? 0 : s_ambTrigs[0].org[0],
        s_ambTrigs.empty() ? 0 : s_ambTrigs[0].org[1], s_ambTrigs.empty() ? 0 : s_ambTrigs[0].org[2]);
}

// _music.csc: updateMusic -> transitionOut(active, next) + transitionIn(active, next) (sin stinger: ningun estado del mapa lo usa)
void MusicSet(const std::string &next)
{
    if (next == s_musicActive) return;
    if (!next.empty() && !s_music.count(next)) { fprintf(stderr, "[H6A] musica: estado '%s' desconocido (se mantiene '%s')\n", next.c_str(), s_musicActive.c_str()); return; }
    MusicState *prev = s_musicActive.empty() ? nullptr : &s_music[s_musicActive];
    MusicState *nx = next.empty() ? nullptr : &s_music[next];
    const std::string nextLoop = nx ? nx->loopalias : "", nextOne = nx ? nx->oneshotalias : "";
    if (prev)
    {
        if (!prev->loopalias.empty())
        {
            if (prev->loopalias != nextLoop || !nextOne.empty()) FakeStopLoop(prev->ent, prev->fadeout);
        }
        else if (prev->id >= 0)
        {
            // fadeOutAndStopSound(id, time)
            const float rate = prev->fadeout != 0 ? 1.0f / prev->fadeout : 0.0f;
            SND_SetPlaybackAttenuationRate(prev->id, rate);
            SND_SetPlaybackAttenuation(prev->id, 0.0f);
            s_pendingStops.push_back({ prev->id, NowMs() + prev->fadeout * 1000.0 });
        }
        if (prev->loopalias != nextLoop) prev->id = -1;
    }
    if (nx)
    {
        if (!nx->ent) nx->ent = FakeSpawn();
        if (!nx->oneshotalias.empty())
        {
            float org[3] = { 0, 0, 0 };
            nx->id = CG_PlaySoundWithHandle(0, ENTITYNUM_WORLD, org, 0, 0, 1.0f, SND_FindAliasId(nx->oneshotalias.c_str()));
        }
        if (prev && prev->loopalias == nx->loopalias && prev->id != -1 && nx->oneshotalias.empty())
        {
            nx->id = prev->id; prev->id = -1; std::swap(prev->ent, nx->ent);
        }
        else if (!nx->loopalias.empty()) nx->id = FakePlayLoop(nx->ent, nx->loopalias.c_str(), nx->fadein);
    }
    fprintf(stderr, "[H6A] musica t=%d: '%s' -> '%s' (bucle '%s', una vez '%s', id %d)\n", level.time, s_musicActive.c_str(), next.c_str(),
        nx ? nx->loopalias.c_str() : "", nx ? nx->oneshotalias.c_str() : "", nx ? nx->id : -1);
    s_musicActive = next;
}

std::vector<std::string> s_musicQ;   // los comandos llegan dentro de SV_Frame; se aplican en el frame de audio
// OLA 14: tambien 's <id>' (LocalSound: playlocalsound), 'k <id>' (LocalSoundStop), 'q <f>' (CG_SP_SoundFadeCommand) y los demas
// sistemas de cliente ('9': levelNotify, box_indicator, lsm...), que se aplican en ClientSysFrame
struct SrvCmd { char type; std::string sys, state; };
std::vector<SrvCmd> s_cmdQ;
std::map<std::string, int> s_sysSeen; int s_localSnd, s_localStop, s_fades;
void (*s_prevSrvHook)(const client_t *, const char *);
void ServerCmdHook(const client_t *client, const char *cmd)
{
    if (s_prevSrvHook) s_prevSrvHook(client, cmd);   // OLA M1: encadenado (h7e_light, h13_hud) en vez de pisar el gancho
    if (!client || !cmd || !cmd[0]) return;
    if ((cmd[0] == 's' || cmd[0] == 'k' || cmd[0] == 'q') && cmd[1] == ' ') { s_cmdQ.push_back({ cmd[0], cmd + 2, "" }); return; }
    if (cmd[0] != '9' || cmd[1] != ' ') return;
    const int idx = atoi(cmd + 2);
    char cs[1024]; SV_GetConfigstring(idx + 1538, cs, sizeof cs);
    const std::string sys = Info_ValueForKey(cs, "n");
    const char *p = strchr(cmd + 2, ' ');
    if (!p) return;
    char ss[1024]; I_strncpyz(ss, p + 1, sizeof ss);
    std::string st = Info_ValueForKey(ss, "s");
    for (auto &c : st) if (c == '!') c = ' ';   // GScr_ClientSysSetState cambia ' ' por '!'
    ++s_sysSeen[sys];
    if (!I_stricmp(sys.c_str(), "musicCmd")) s_musicQ.push_back(st);
    else s_cmdQ.push_back({ '9', sys, st });
}

// ---------------------------------------------------------------- eventos (cg_event.cpp)
void FootstepAlias(char *out, size_t n, const char *mov, int surf, bool firstperson, bool quiet)
{
    // buildMovementSoundAliasName (_footsteps.csc)
    snprintf(out, n, "fly_%s%s_%s_%s", quiet ? "q" : "", mov, firstperson ? "plr" : "npc", Com_SurfaceTypeToName(surf));
}

void EntityEvent(int number, const entityState_s *es, int event, int eventParm, bool isPlayerView, const playerState_s *ps, const float *camOrg, const float *camAng)
{
    ++s_events;
    float position[3]; EntOrigin(number, position);
    const unsigned weaponIdx = isPlayerView ? ps->weapon : es->weapon;
    const WeaponDef *weaponDef = BG_GetWeaponDef(weaponIdx);
    const unsigned meleeWeapon = isPlayerView ? (BG_IsUseAsMeleeWeapon(weaponIdx) ? es->weapon : ps->meleeWeapon) : es->weapon;
    const WeaponDef *meleeWeaponDef = BG_GetWeaponDef(meleeWeapon);
    const bool quiet = isPlayerView && ps && (ps->perks[1] & 0x40) != 0;
    char a[128]; unsigned v7 = 0; bool playV7 = false;
#define WSND(f) (isPlayerView ? AliasOf(weaponDef->f##Player) : AliasOf(weaponDef->f))
    if (event >= 111 && event <= 141)
    {
        FootstepAlias(a, sizeof a, "land", event - 111, isPlayerView, quiet);
        CG_PlaySound(0, number, nullptr, 0, 0, 1.0f, a);
        return;
    }
    if (event >= 142 && event <= 172)
    {
        FootstepAlias(a, sizeof a, "land", event - 142, isPlayerView, quiet);
        CG_PlaySound(0, number, nullptr, 0, 0, 1.0f, a);
        if (isPlayerView) CG_PlaySound(0, number, nullptr, 0, 0, 1.0f, "fly_land_damage_plr");
        return;
    }
    switch (event)
    {
    case EV_STOP_SOUND_ALIAS: SND_StopSoundAliasOnEnt(CG_SndEntHandle(0, es->otherEntityNum), es->loopSoundId); return;
    case EV_SOUND_ALIAS:
    case EV_SOUND_ALIAS_NOTIFY:
    {
        float sp[3] = { es->lerp.pos.trBase[0], es->lerp.pos.trBase[1], es->lerp.pos.trBase[2] };
        if (es->un3.item && es->otherEntityNum >= 0 && es->otherEntityNum < ENTITYNUM_WORLD) EntOrigin(es->otherEntityNum, sp);   // playsoundontag: sin DObj de cliente, el origen de la entidad
        CG_PlaySound(0, es->otherEntityNum, sp, 0, event == EV_SOUND_ALIAS_NOTIFY, 1.0f, es->loopSoundId);
        return;
    }
    case EV_STOPSOUNDS: { SndEntHandle h = CG_SndEntHandle(0, es->number); SND_StopSoundsOnEnt(h); CG_SndKillAutoSimEnt(h); return; }
    case EV_ITEM_PICKUP:
    case EV_AMMO_PICKUP:
    {
        if (eventParm < 1 || eventParm >= 2048) return;
        const WeaponDef *it = BG_GetWeaponDef(eventParm);
        const unsigned id = event == EV_ITEM_PICKUP ? (isPlayerView ? AliasOf(it->pickupSoundPlayer) : AliasOf(it->pickupSound))
                                                    : (isPlayerView ? AliasOf(it->ammoPickupSoundPlayer) : AliasOf(it->ammoPickupSound));
        CG_PlaySound(0, number, nullptr, 0, 0, 1.0f, id);
        return;
    }
    case EV_NOAMMO:
        if (!BG_WeaponIsClipOnly(weaponIdx) && !weaponDef->cancelAutoHolsterWhenEmpty) CG_PlaySound(0, number, nullptr, 0, 0, 1.0f, WSND(emptyFireSound));
        return;
    case EV_RELOAD: v7 = WSND(reloadSound); playV7 = true; break;
    case EV_RELOAD_FROM_EMPTY: v7 = WSND(reloadEmptySound); playV7 = true; break;
    case EV_RELOAD_START: v7 = WSND(reloadStartSound); playV7 = true; break;
    case EV_RELOAD_END: v7 = WSND(reloadEndSound); playV7 = true; break;
    case EV_RAISE_WEAPON: v7 = WSND(raiseSound); playV7 = true; break;
    case EV_FIRST_RAISE_WEAPON: v7 = WSND(firstRaiseSound); playV7 = true; break;
    case EV_PUTAWAY_WEAPON: v7 = WSND(putawaySound); playV7 = true; break;
    case EV_WEAPON_ALT: v7 = WSND(altSwitchSound); playV7 = true; break;
    case EV_PULLBACK_WEAPON: v7 = WSND(pullbackSound); playV7 = true; break;
    case EV_RECHAMBER_WEAPON: v7 = WSND(rechamberSound); playV7 = true; break;
    case EV_MELEE_SWIPE: v7 = isPlayerView ? AliasOf(meleeWeaponDef->meleeSwipeSoundPlayer) : AliasOf(meleeWeaponDef->meleeSwipeSound); playV7 = true; break;
    case EV_WEAPON_DEPLOYING: v7 = WSND(deploySound); playV7 = true; break;
    case EV_WEAPON_FINISH_DEPLOYING: v7 = WSND(finishDeploySound); playV7 = true; break;
    case EV_DETONATE: v7 = WSND(detonateSound); playV7 = true; break;
    case EV_MELEE_HIT:   // cg_event.cpp: perro (parm 3), meleeHitSound del arma, cuchillo (parm 1) o golpe generico (cgMedia, OLA 14)
        if (eventParm == 3) CG_PlaySound(0, es->otherEntityNum, nullptr, 0, 0, 1.0f, cgMedia.meleeDogHit);
        else if (meleeWeaponDef->meleeHitSound.get()) CG_PlaySound(0, es->otherEntityNum, nullptr, 0, 0, 1.0f, AliasOf(meleeWeaponDef->meleeHitSound));
        else CG_PlaySound(0, es->otherEntityNum, nullptr, 0, 0, 1.0f, eventParm == 1 ? cgMedia.meleeKnifeHit : cgMedia.meleeHit);
        return;
    case EV_MELEE_MISS:
        if (eventParm == 2) CG_PlaySound(0, es->otherEntityNum, position, 0, 0, 1.0f, cgMedia.meleeDogHitOther);
        else if (meleeWeaponDef->meleeMissSound.get()) CG_PlaySound(0, es->otherEntityNum, position, 0, 0, 1.0f, AliasOf(meleeWeaponDef->meleeMissSound));
        else CG_PlaySound(0, es->otherEntityNum, position, 0, 0, 1.0f, eventParm == 1 ? cgMedia.meleeKnifeHitOther : cgMedia.meleeHitOther);
        return;
    case EV_BULLET_HIT:
    {
        // CG_BulletHitEvent_Internal (cg_weapons.cpp): hitSound de CG_ImpactEffectForWeapon (cgMedia.bulletHit*/Exit*/shotgun... por
        // superficie) en la entidad 1022 en el punto del impacto. Esa funcion lee tambien cgMedia.fx (la tabla de impactos: la carga
        // H6A_ClientInit desde la OLA 15C, con o sin FX). El efecto visual lo pone h5_fx.cpp.
        const unsigned w = es->weapon;
        if (!cgMedia.fx || !w || w >= BG_GetNumWeapons() || es->surfType >= 31) return;
        const WeaponDef *wd = BG_GetWeaponDef(w);
        if (es->otherEntityNum >= 0 && es->otherEntityNum < MAX_GENTITIES_SV && g_entities[es->otherEntityNum].s.eType == ET_MISSILE
            && wd->weapType == WEAPTYPE_PROJECTILE) return;   // proyectil de un misil: sin sonido de bala
        const FxEffectDef *fx = nullptr; unsigned snd = 0;
        CG_ImpactEffectForWeapon(w, es->surfType, (char)es->un1.scale, &fx, &snd);
        if (snd) { ++s_impacts; CG_PlaySound(0, 1022, position, 0, 0, 1.0f, snd); }
        return;
    }
    case EV_FIRE_WEAPON:
    case EV_FIRE_WEAPON_LASTSHOT:
    case EV_FIRE_WEAPON_LEFT:
    case EV_FIRE_WEAPON_LASTSHOT_LEFT:
    {
        // CG_FireWeapon -> CG_WeaponFireSingle (cg_weapons.cpp ORIGINAL: autosim / CG_SndWeaponFire con el fireSound[Player])
        unsigned weapon = es->weapon; if (isPlayerView) weapon = ps->weapon;
        if (!weapon || weapon >= BG_GetNumWeapons()) return;
        const WeaponVariantDef *wv = BG_GetWeaponVariantDef(weapon);
        float org[3], dir[3];
        if (isPlayerView && camOrg) { memcpy(org, camOrg, 12); AngleVectors(camAng, dir, nullptr, nullptr); }
        else { memcpy(org, position, 12); AngleVectors(g_entities[number].r.currentAngles, dir, nullptr, nullptr); }
        const bool isLocal = number == s_playerNum;
        const unsigned tag = (event == EV_FIRE_WEAPON_LEFT || event == EV_FIRE_WEAPON_LASTSHOT_LEFT) ? scr_const.tag_flash1 : scr_const.tag_flash;
        CG_WeaponFireSingle(0, CG_SndEntHandle(0, number), weapon, isPlayerView, isLocal, event == EV_FIRE_WEAPON_LASTSHOT, org, dir, tag, wv->bRapidFire);
        // OLA 14: whizby de las balas de OTRO jugador (co-op, h11): el cliente original traza la bala de cliente (CG_BulletEndpos /
        // DrawBulletImpacts, cg_weapons.cpp) y llama CG_SoundWhizby si el tirador no es el jugador local. Aqui la bala va desde el ojo
        // del tirador por su vista hasta el primer solido del mundo (sin entidades) o 10000 u.
        if (!isLocal && !isPlayerView && g_entities[number].s.eType == ET_PLAYER && cgArray && weaponDef->weapType == WEAPTYPE_BULLET)
        {
            float st[3]; memcpy(st, position, 12); st[2] += g_entities[number].client ? g_entities[number].client->ps.viewHeightCurrent : 60.0f;
            float en[3]; for (int k = 0; k < 3; ++k) en[k] = st[k] + dir[k] * 10000.0f;
            trace_t tr; memset(&tr, 0, sizeof tr); const float z[3] = { 0, 0, 0 };
            col_context_t ctx(0x2806831); CM_BoxTrace(&tr, st, en, z, z, 0x2806831, &ctx);   // MASK_SHOT, solo el mundo
            float hit[3]; for (int k = 0; k < 3; ++k) hit[k] = st[k] + (en[k] - st[k]) * tr.fraction;
            ++s_whizChecks;
            CG_SoundWhizby(0, weaponDef, st, dir, hit);
        }
        return;
    }
    case EV_GRENADE_BOUNCE:
        if (weaponDef->bounceSound.get() && es->surfType < 31)
        {
            const char *s = weaponDef->bounceSound.get()[es->surfType].get();
            if (s && *s) CG_PlaySound(0, ENTITYNUM_WORLD, position, 0, 0, 1.0f, SND_FindAliasId(s));
        }
        return;
    case EV_GRENADE_EXPLODE:
    case EV_ROCKET_EXPLODE:
    case EV_ROCKET_EXPLODE_NOMARKS:
    case EV_FLASHBANG_EXPLODE:
    case EV_CUSTOM_EXPLODE:
    case EV_CUSTOM_EXPLODE_NOMARKS:
    case EV_DUD_EXPLODE:
        CG_PlaySound(0, ENTITYNUM_WORLD, position, 0, 0, 1.0f, AliasOf(weaponDef->projExplosionSound));
        return;
    case EV_FOOTSTEP_SPRINT:
    case EV_FOOTSTEP_RUN:
    case EV_FOOTSTEP_WALK:
    case EV_FOOTSTEP_CROUCHRUN:
    case EV_FOOTSTEP_CROUCHWALK:
    case EV_FOOTSTEP_PRONE:
    {
        // CG_DoFootstepEvent: puerta (nextSnap->ps.bobCycle & 0x7f) <= 0x3f, luego playerFootstep del script de cliente
        if (!ps || (ps->bobCycle & 0x7F) > 0x3F) return;
        static const char *mov[] = { "step_sprint", "step_run", "step_walk", "step_crouchrun", "step_crouchwalk", "step_prone" };
        FootstepAlias(a, sizeof a, mov[event - EV_FOOTSTEP_SPRINT], eventParm, isPlayerView, quiet);
        CG_PlaySound(0, number, nullptr, 0, 0, 1.0f, a);
        return;
    }
    case EV_JUMP:
        FootstepAlias(a, sizeof a, "step_run", eventParm, isPlayerView, quiet);
        CG_PlaySound(0, number, nullptr, 0, 0, 1.0f, a);
        return;
    default: return;
    }
#undef WSND
    if (playV7) CG_PlaySound(0, number, nullptr, 0, 0, 1.0f, v7);
}

void ServerFrameEvents(const float *camOrg, const float *camAng)
{
    gentity_s *pl = PlayerEnt();
    s_playerNum = pl ? pl->s.number : -1;
    // jugador: CG_CheckPlayerstateEvents (predecibles + impredecibles del ps)
    if (pl)
    {
        playerState_s *ps = &pl->client->ps;
        entityState_s es = pl->s; es.number = pl->s.number;
        const int pred = (unsigned short)ps->predictableEventSequence, unpred = (unsigned short)ps->unpredictableEventSequence;
        if (s_psPred < 0) { s_psPred = pred; s_psUnpred = unpred; }
        int d = (pred - s_psPred) & 0xFF; if (d > 4) { s_psPred = (pred - 4) & 0xFFFF; d = 4; }
        for (int k = 0; k < d; ++k) { const int i = (s_psPred + k) & 3; EntityEvent(es.number, &es, ps->predictableEvents[i], ps->predictableEventParms[i], true, ps, camOrg, camAng); }
        s_psPred = pred;
        d = (unpred - s_psUnpred) & 0xFF; if (d > 4) { s_psUnpred = (unpred - 4) & 0xFFFF; d = 4; }
        for (int k = 0; k < d; ++k) { const int i = (s_psUnpred + k) & 3; EntityEvent(es.number, &es, ps->unpredictableEvents[i], ps->unpredictableEventParms[i], true, ps, camOrg, camAng); }
        s_psUnpred = unpred;
    }
    // entidades: temporales (eType >= ET_EVENTS, un evento al aparecer) y secuencia de s.events (CG_CheckEvents)
    const playerState_s *pps = pl ? &pl->client->ps : nullptr;
    for (int n = 0; n < level.num_entities; ++n)
    {
        gentity_s *e = &g_entities[n];
        if (!e->r.inuse || !EntNum_IsNetworked(n)) { s_entSeen[n] = false; continue; }
        if (e->client) continue;
        const int use = e->s.lerp.useCount;
        if ((int)e->s.eType >= 21)
        {
            if (!s_entSeen[n] || s_entUse[n] != use || s_tempTime[n] != e->r.eventTime)
            {
                s_entSeen[n] = true; s_entUse[n] = use; s_tempTime[n] = e->r.eventTime;
                EntityEvent(n, &e->s, (int)e->s.eType - 21, e->s.eventParm, false, pps, camOrg, camAng);
            }
            continue;
        }
        if (!s_entSeen[n] || s_entUse[n] != use)
        {
            s_entSeen[n] = true; s_entUse[n] = use; s_entSeq[n] = (unsigned short)e->s.eventSequence; s_tempTime[n] = -1;
            continue;
        }
        const int seq = (unsigned short)e->s.eventSequence;
        int d = (seq - s_entSeq[n]) & 0xFF; if (d > 4) { s_entSeq[n] = (seq - 4) & 0xFFFF; d = 4; }
        for (int k = 0; k < d; ++k) { const int i = (s_entSeq[n] + k) & 3; EntityEvent(n, &e->s, e->s.events[i], e->s.eventParms[i], false, pps, camOrg, camAng); }
        s_entSeq[n] = seq;
    }
}

void LoopPump(float frameMs)
{
    // CG_PumpEntityLoopSound de las entidades del snapshot (s.loopSoundId) y de las falsas (CG_ProcessFakeEntity)
    for (int n = 0; n < level.num_entities; ++n)
    {
        const gentity_s *e = &g_entities[n];
        if (!e->r.inuse || !e->s.loopSoundId || !EntNum_IsNetworked(n) || (int)e->s.eType >= 21) continue;
        CG_PlaySound(0, n, nullptr, e->s.loopSoundFade, 0, 1.0f, e->s.loopSoundId);
    }
    for (int i = 1; i < FAKE_MAX; ++i)
    {
        FakeEnt &f = s_fake[i];
        if (!f.inuse || !f.loopSoundId) continue;
        if (f.loopSoundFade < 0)
        {
            f.loopSoundFade += (unsigned short)(int)frameMs;
            if (f.loopSoundFade >= 0) { f.loopSoundId = 0; f.loopSoundFade = 0; continue; }
        }
        CG_PlaySound(0, FAKE_BASE + i, nullptr, f.loopSoundFade, 0, 1.0f, f.loopSoundId);
    }
}

// H6A_SURVEY=1: formatos de los sonidos cargados en RAM y de los streams de los bancos (para saber que decodificadores hacen falta)
// H6A_DUMPWMA=<dir> (con H6A_SURVEY): vuelca cada sonido cargado xWMA (formato 7) como fichero RIFF XWMA con el WAVEFORMATEX que
// arma iSND_CreateVoice (0x161, bloque 4096/2230, 6000 B/s por canal) y su tabla dpds; h6a_wma_pak.py los transcodifica.
void DumpWma(const char *dir)
{
    std::set<const LoadedSound *> seen;
    char path[1024]; snprintf(path, sizeof path, "%s/index.tsv", dir);
    FILE *idx = fopen(path, "w");
    if (!idx) { fprintf(stderr, "[H6A] H6A_DUMPWMA: no se puede escribir %s\n", path); return; }
    unsigned n = 0;
    for (unsigned b = 0; b < g_snd_bankCount; ++b)
    {
        const SndBank *bank = g_snd_banks[b]; if (!bank) continue;
        for (unsigned i = 0; i < bank->aliasCount; ++i)
        {
            const snd_alias_list_t &l = bank->alias.get()[i];
            for (int k = 0; k < l.count; ++k)
            {
                const SoundFile *sf = l.head.get()[k].soundFile.get();
                if (!sf || sf->type != 1 || !sf->u.loadSnd.get()) continue;
                const LoadedSound *ls = sf->u.loadSnd.get();
                const snd_asset &as = ls->sound;
                if (as.format != SND_ASSET_FORMAT_WMA || !as.seek_table.get() || !seen.insert(ls).second) continue;
                const unsigned ch = as.channel_count, align = ch == 2 ? 4096 : 2230, avg = 6000 * ch;
                const unsigned dpds = 4 * as.seek_table_count, fmtLen = 18;
                snprintf(path, sizeof path, "%s/%05u.xwma", dir, n);
                FILE *f = fopen(path, "wb"); if (!f) continue;
                auto u32 = [&](unsigned v) { fwrite(&v, 4, 1, f); };
                auto u16 = [&](unsigned v) { unsigned short x = (unsigned short)v; fwrite(&x, 2, 1, f); };
                fwrite("RIFF", 1, 4, f); u32(4 + 8 + fmtLen + 8 + dpds + 8 + as.data_size); fwrite("XWMA", 1, 4, f);
                fwrite("fmt ", 1, 4, f); u32(fmtLen); u16(0x161); u16(ch); u32(as.frame_rate); u32(avg); u16(align); u16(16); u16(0);
                fwrite("dpds", 1, 4, f); u32(dpds); fwrite(as.seek_table.get(), 4, as.seek_table_count, f);
                fwrite("data", 1, 4, f); u32(as.data_size); fwrite(as.data.get(), 1, as.data_size, f);
                fclose(f);
                fprintf(idx, "%05u\t%s\t%u\t%u\t%u\n", n, ls->name.get(), as.frame_count, as.frame_rate, ch);
                ++n;
            }
        }
    }
    fclose(idx);
    fprintf(stderr, "[H6A] H6A_DUMPWMA: %u sonidos xWMA volcados en %s\n", n, dir);
}

// OLA 15C: censo de los alias de stream cuyo fichero no esta en los .iwd del usuario (VO de personajes y anunciador). Siempre
// imprime el recuento por prefijo; H6A_STREAMAUDIT=<fichero.tsv> vuelca la lista (alias, fichero, exists del ff, en iwd).
void StreamAudit()
{
    std::map<std::string, std::pair<unsigned, unsigned>> byPrefix;   // prefijo del alias -> (total, faltan)
    std::set<std::string> missingFiles; unsigned total = 0, missing = 0;
    FILE *out = getenv("H6A_STREAMAUDIT") ? fopen(getenv("H6A_STREAMAUDIT"), "w") : nullptr;
    if (out) fprintf(out, "alias\tfichero\texists_ff\ten_iwd\n");
    for (unsigned b = 0; b < g_snd_bankCount; ++b)
    {
        const SndBank *bank = g_snd_banks[b]; if (!bank) continue;
        for (unsigned i = 0; i < bank->aliasCount; ++i)
        {
            const snd_alias_list_t &l = bank->alias.get()[i];
            for (int k = 0; k < l.count; ++k)
            {
                const snd_alias_t &a = l.head.get()[k];
                const SoundFile *sf = a.soundFile.get();
                if (!sf || sf->type != 2) continue;
                char fn[260]; SND_AliasGetFileName(&a, fn, 256);
                std::string kname = fn; for (auto &c : kname) { c = (char)tolower((unsigned char)c); if (c == '\\') c = '/'; }
                int fd; unsigned long long off; unsigned sz;
                const bool there = FS_H6A_Locate(kname.c_str(), &fd, &off, &sz) || FS_H6A_Locate(("sound/" + kname).c_str(), &fd, &off, &sz);
                const char *an = a.name.get();
                std::string pre = an ? an : "?"; { size_t u = pre.find('_'); u = u == std::string::npos ? u : pre.find('_', u + 1); if (u != std::string::npos) pre = pre.substr(0, u); }
                auto &bp = byPrefix[pre]; ++bp.first; ++total;
                if (!there) { ++bp.second; ++missing; missingFiles.insert(kname); }
                if (out) fprintf(out, "%s\t%s\t%d\t%d\n", an ? an : "?", kname.c_str(), (int)sf->exists, there ? 1 : 0);
            }
        }
    }
    if (out) fclose(out);
    std::string worst;
    for (auto &p : byPrefix) if (p.second.second) worst += " " + p.first + " " + std::to_string(p.second.second) + "/" + std::to_string(p.second.first);
    fprintf(stderr, "[H6A] streams: %u alias, %u sin fichero en los iwd (%zu ficheros distintos); faltan por prefijo:%s\n", total, missing, missingFiles.size(), worst.c_str());
}

void Survey()
{
    unsigned cnt[2][16] = {}; unsigned long long bytes[2][16] = {}; unsigned other = 0;
    for (unsigned b = 0; b < g_snd_bankCount; ++b)
    {
        const SndBank *bank = g_snd_banks[b]; if (!bank) continue;
        for (unsigned i = 0; i < bank->aliasCount; ++i)
        {
            const snd_alias_list_t &l = bank->alias.get()[i];
            for (int k = 0; k < l.count; ++k)
            {
                const snd_alias_t &a = l.head.get()[k];
                const SoundFile *sf = a.soundFile.get();
                if (!sf) continue;
                if (sf->type == 1 && sf->u.loadSnd.get())
                {
                    const snd_asset &as = sf->u.loadSnd.get()->sound; unsigned f = as.format & 15; ++cnt[0][f]; bytes[0][f] += as.data_size;
                    if (cnt[0][f] <= 3)
                    {
                        fprintf(stderr, "[H6A] censo: ej. fmt %u '%s' ver %u frames %u rate %u ch %u hdr %u block %u buf %u flags %u/%u seek %u data %u:", f,
                            sf->u.loadSnd.get()->name.get(), as.version, as.frame_count, as.frame_rate, as.channel_count, as.header_size, as.block_size,
                            as.buffer_size, (unsigned)as.channel_flags, (unsigned)as.flags, as.seek_table_count, as.data_size);
                        const unsigned char *d = (const unsigned char *)as.data.get();
                        for (unsigned j = 0; d && j < 24 && j < as.data_size; ++j) fprintf(stderr, " %02x", d[j]);
                        if (as.seek_table_count && as.seek_table.get()) fprintf(stderr, " | seek0 %u seekN %u", as.seek_table.get()[0], as.seek_table.get()[as.seek_table_count - 1]);
                        fprintf(stderr, "\n");
                    }
                }
                else if (sf->type == 2) ++cnt[1][0];
                else ++other;
            }
        }
    }
    if (const char *dir = getenv("H6A_DUMPWMA")) DumpWma(dir);
    StreamAudit();
    for (int f = 0; f < 16; ++f) if (cnt[0][f]) fprintf(stderr, "[H6A] censo: RAM formato %d: %u alias, %.1f MB\n", f, cnt[0][f], bytes[0][f] / 1048576.0);
    fprintf(stderr, "[H6A] censo: %u alias de stream, %u otros\n", cnt[1][0], other);
}

bool InitSound()
{
    if (getenv("H6A") && !strcmp(getenv("H6A"), "0")) { fprintf(stderr, "[H6A] sonido desactivado en ejecucion (H6A=0)\n"); return false; }
    // SND_ShouldInit pide !IsDedicatedServer(): el harness corre el servidor como dedicado 2 sin cliente local; para el sistema de
    // sonido es el cliente local del juego, asi que la comprobacion se hace con dedicated 0 (solo durante SND_Init).
    dvar_s *ded = (dvar_s *)dedicated;
    const int saved = ded ? ded->current.integer : 0;
    if (ded) ded->current.integer = 0;
    SND_Init();
    if (ded) ded->current.integer = saved;
    if (!SND_Active()) { fprintf(stderr, "[H6A] SND_Init sin dispositivo: sin sonido\n"); return false; }
    if (H10_CfgApplySound) H10_CfgApplySound();   // OLA 15C: volumenes guardados (snd_menu_*) del config.cfg de la ola 10
    if (g_h0ServerCmdHook != ServerCmdHook) { s_prevSrvHook = g_h0ServerCmdHook; g_h0ServerCmdHook = ServerCmdHook; }
    CG_RegisterSounds();   // OLA 14: cgMedia (golpes cuerpo a cuerpo, impactos por superficie, whizby/crack...) como CG_Init
    // OLA 15C: la tabla de impactos (CG_RegisterGraphics -> CG_RegisterImpactEffects_FastFile) es un asset del ff, no necesita
    // render: sin ella (trace, o vk sin H5FX) EV_BULLET_HIT no tenia sonido. h5_fx.cpp pone despues el mismo puntero.
    if (!cgMedia.fx) cgMedia.fx = DB_FindXAssetHeader(ASSET_TYPE_IMPACT_FX, (char *)"", 1, -1).impactFx;
    fprintf(stderr, "[H6A] cgMedia: golpe %u/%u cuchillo %u/%u perro %u, whizby %u crack %u, impacto bala pequena (carne) %u, perdigon %u\n",
        cgMedia.meleeHit, cgMedia.meleeHitOther, cgMedia.meleeKnifeHit, cgMedia.meleeKnifeHitOther, cgMedia.meleeDogHit, cgMedia.bulletWhizby,
        cgMedia.bulletCrack, cgMedia.bulletHitSmallSound[7], cgMedia.shotgunHitSound[7]);
    CG_SndAutoSimReset();   // como CG_Init: registra snd_autoSim y vacia la simulacion de rafagas
    const char *map = Dvar_GetString("mapname");
    char nm[128]; snprintf(nm, sizeof nm, "clientscripts/%s_amb.csc", map ? map : "");
    const std::string amb = RawText(nm);
    if (amb.empty()) fprintf(stderr, "[H6A] ambiente: %s no esta en las zonas\n", nm);
    else ParseAmb(amb);
    AmbientStart();
    if (getenv("H6A_SURVEY")) Survey();
    if (const char *d = getenv("H6A_DUMPRAW"))
    {
        DB_EnumXAssets(ASSET_TYPE_RAWFILE, DumpRawOne, (void *)d, true);
        if (const MapEnts *me = MapEntsOf()) if (FILE *f = fopen((std::string(d) + "/mapents.txt").c_str(), "wb")) { fwrite(me->entityString.get(), 1, me->numEntityChars, f); fclose(f); }
    }
    return true;
}
} // namespace

// ---------------------------------------------------------------- cgame/cg_sound.cpp sobre g_entities
bool __cdecl CG_ShouldPlaySoundOnLocalClient(int, int, const float *, unsigned int) { return 1; }   // un solo cliente local: la primera rama

SndEntHandle __cdecl SND_EntHandle(char localClientNum, __int16 entIndex, char useCount, bool isInKillCam, bool isStationary, team_t team)
{
    return (SndEntHandle)(((team & 7) << 23)
                          | (isStationary << 21) & 0xFE7FFFFF
                          | (isInKillCam << 22) & 0xFE7FFFFF
                          | ((useCount & 0x7F) << 12) & 0xFE1FFFFF
                          | entIndex & 0xFFF
                          | ((localClientNum & 3) << 19) & 0xFE1FFFFF);
}

SndEntHandle __cdecl CG_SndEntHandle(int localClientNum, int entIndex)
{
    if (entIndex >= FAKE_BASE && entIndex < FAKE_BASE + FAKE_MAX && EntNum_IsCsc(entIndex))
        return SND_EntHandle(localClientNum, entIndex, 0, 0, 0, TEAM_FREE);
    if (entIndex < 0 || entIndex >= MAX_GENTITIES_SV) entIndex = ENTITYNUM_WORLD;
    const gentity_s *e = &g_entities[entIndex];
    team_t team = TEAM_FREE;
    if (e->s.eType == ET_PLAYER) team = G_GetClientTeam(e->s.number);
    else if (e->s.eType == ET_ACTOR) team = (team_t)e->s.lerp.u.actor.team;
    const bool stationary = entIndex == ENTITYNUM_WORLD || (int)e->s.eType >= 21;
    return SND_EntHandle(localClientNum, entIndex, e->s.lerp.useCount, 0, stationary, team);
}

void __cdecl CG_PlaySound(int localClientNum, int entitynum, float *origin, int fadeMs, bool doNotify, float attenuation, unsigned int id)
{
    float tmp[3];
    if (!origin) { EntOrigin(entitynum, tmp); origin = tmp; }
    if (id && CG_ShouldPlaySoundOnLocalClient(localClientNum, entitynum, origin, id))
    {
        ++s_plays;
        SND_Play(id, fadeMs, attenuation, CG_SndEntHandle(localClientNum, entitynum), origin, 0, doNotify);
    }
}
void __cdecl CG_PlaySound(int localClientNum, int entitynum, float *origin, int fadeMs, bool doNotify, float attenuation, char *name)
{
    CG_PlaySound(localClientNum, entitynum, origin, fadeMs, doNotify, attenuation, (unsigned)SND_FindAliasId(name));
}
void __cdecl CG_PlaySound(int localClientNum, float *origin, int fadeMs, bool doNotify, float attenuation, char *name)
{
    CG_PlaySound(localClientNum, ENTITYNUM_WORLD, origin, fadeMs, doNotify, attenuation, name);
}
int __cdecl CG_PlaySoundWithHandle(int localClientNum, int entitynum, float *origin, int fadeMs, bool doNotify, float volume, unsigned int id)
{
    float tmp[3];
    if (!origin) { EntOrigin(entitynum, tmp); origin = tmp; }
    if (!id || !CG_ShouldPlaySoundOnLocalClient(localClientNum, entitynum, origin, id)) return -1;
    ++s_plays;
    return SND_Playback(id, fadeMs, volume, CG_SndEntHandle(localClientNum, entitynum), origin, 0, doNotify);
}
int __cdecl CG_PlaySoundWithHandle(int localClientNum, int entitynum, float *origin, int fadeMs, bool doNotify, float attenuation, char *name)
{
    return CG_PlaySoundWithHandle(localClientNum, entitynum, origin, fadeMs, doNotify, attenuation, (unsigned)SND_FindAliasId(name));
}

void __cdecl CG_SubtitleSndLengthNotify(int msec, const char *lengthNotifyData) { (void)msec; (void)lengthNotifyData; }   // sin subtitulos en el harness

void __cdecl CG_ScriptSndLengthNotify(int msec, int ent)
{
    // el original manda "sl <ent> <msec>" (CL_AddReliableCommand) y el servidor lo aplica en G_SP_Cmd_SoundLength_f: aqui directo
    if ((unsigned)ent >= MAX_GENTITIES_SV) return;
    gentitySpExt &ext = G_EntSpExt(&g_entities[ent]);
    if (ext.soundNotifyLength < msec) { ext.soundNotifyLength = msec; ext.soundNotifyTime = svs.time; }
}

int __cdecl CG_SoundGetUseCount(SndEntHandle sndEnt)
{
    const int n = sndEnt.handle & 0xFFF;
    if (n >= FAKE_BASE && n < FAKE_BASE + FAKE_MAX && EntNum_IsCsc(n)) return 0;
    return n < MAX_GENTITIES_SV ? g_entities[n].s.lerp.useCount : 0;
}

bool __cdecl CG_SoundEntInUse(SndEntHandle sndEnt)
{
    const unsigned n = sndEnt.handle & 0xFFF;
    if (n <= 0x400 || !EntNum_IsCsc(n)) return 1;
    return n - FAKE_BASE < (unsigned)FAKE_MAX && s_fake[n - FAKE_BASE].inuse;
}

void __cdecl CG_CopySoundEntityOrientation(int localClientNum, int entIndex, float *origin_out, float (*axis_out)[3])
{
    (void)localClientNum;
    EntOrigin(entIndex, origin_out);
    float ang[3] = { 0, 0, 0 };
    if (entIndex >= 0 && entIndex < MAX_GENTITIES_SV && !(entIndex >= FAKE_BASE && EntNum_IsCsc(entIndex))) memcpy(ang, g_entities[entIndex].r.currentAngles, 12);
    AnglesToAxis(ang, axis_out);
}

void __cdecl CG_GetSoundEntityOrientation(SndEntHandle sndEnt, float *origin_out, float (*axis_out)[3], float *velocity_out)
{
    const int n = sndEnt.handle & 0xFFF;
    if (velocity_out)
    {
        velocity_out[0] = velocity_out[1] = velocity_out[2] = 0.0f;
        if (n < MAX_GENTITIES_SV && !(n >= FAKE_BASE && EntNum_IsCsc(n)) && g_entities[n].r.inuse && s_entSeen[n])
            for (int k = 0; k < 3; ++k) velocity_out[k] = s_curOrg[n][k] - s_prevOrg[n][k];
    }
    CG_CopySoundEntityOrientation(0, n, origin_out, axis_out);
}

void __cdecl CG_StopSoundAlias(int localClientNum, int entityNum, unsigned int aliasName)
{
    if (aliasName) SND_StopSoundAliasOnEnt(CG_SndEntHandle(localClientNum, entityNum), aliasName);
}

void __cdecl CG_SndUpdate() { CG_SndUpdateAutoSim(); }        // CG_SndUpdateFire: sin fuegos de mapa (CG_FireManagerUpdate no corre)
void __cdecl CG_SndGameReset() { CG_SndAutoSimReset(); }

void __cdecl CG_PlayClientSoundNoteTracks(int localClientNum, int entnum, float *origin, const char *notifyName, bool isViewArms)
{
    // cg_ents_mp.cpp: notetrack "sndnt#<alias>" (los sonidos de recarga/manipulacion del viewmodel)
    (void)isViewArms;
    if (notifyName && !I_strnicmp(notifyName, "sndnt#", 6))
        CG_PlaySound(localClientNum, entnum, origin, 0, 0, 1.0f, (unsigned)SND_FindAliasId(notifyName + 6));
}

// ---------------------------------------------------------------- trabajador "updatesound" (universal/com_workercmds.cpp)
void __cdecl Sys_AddWorkerCmdInternal(jqWorkerCmd *name, unsigned __int8 *data, WorkerCmdConditional *cond)
{
    (void)data; (void)cond;
    if (name == &updatesound_workerWorkerCmd) { SND_Frame(); return; }
    fprintf(stderr, "[H6A] Sys_AddWorkerCmdInternal: trabajador no soportado en el harness\n"); abort();
}
void __cdecl Sys_AssistAndWaitWorkerCmdInternal(jqWorkerCmd *name)
{
    if (name == &updatesound_workerWorkerCmd) return;   // SND_Frame ya corrio en Sys_AddWorkerCmdInternal
    // OLA 12/14: con un solo hilo no hay trabajo encolado de otros trabajadores: Sys_AddWorkerCmdInternal corre updatesound en el acto
    // y aborta con cualquier otro, asi que esperar es volver en el acto (jqWaitForCompletion sin lotes). Causa del choque H6SND+H5FX
    // (traza en docs/evidencia/h14/traza_choque_fx_marks.txt): esta definicion sustituye al stub 'return 0' de safe0_fx.txt y la de
    // la 6a abortaba con todo lo que no fuera updatesound; FX_InitMarksSystem (fx_marks.cpp:89) espera a fx_add_mark al arrancar.
    (void)name;
}

// ---------------------------------------------------------------- lo que snd*.cpp pide al cliente/sistema (sin TU propia en el harness)
bool __cdecl PC_StartWithNoSounds() { return !SD_Xaudio2CanInit(); }   // win_main.cpp sin G_ExitAfterToolComplete (no hay herramientas)
void __cdecl Sys_SetSndInitializedEvent() {}                            // nadie espera el evento: el harness es de un hilo
__attribute__((weak)) bool __cdecl CG_IsShowingZombieMap() { return false; }                  // mapa de zombies del menu de pausa: no existe en el harness
// Bandera 32 de la UI del cliente local = cgame activo (SCR_UpdateFrame/CL_Key, y SND_SetEntState solo actualiza entidades con
// ella). Con el jugador dentro de la partida esta puesta; el resto de banderas (menus, consola) no existen en el harness.
__attribute__((weak)) bool __cdecl CL_LocalClient_IsCUIFlagSet(int localClientNum, int flag) { return localClientNum == 0 && flag == 32 && s_init; }
void __cdecl Voice_StopClientSamples() {}                               // sin chat de voz
// Oclusion por trazas (snd_occlusion.cpp, hilo propio con CG_SightTracePoint de cg_world.cpp): sin hilo de oclusion todavia.
// cgame/cg_world.cpp CG_SightTracePoint + CG_SightTracePointInternal (no se compila cg_world entero): la traza de vista de
// SND_SetVoiceStartInfo / snd_occlusion contra el arbol de colision. Sin occlusion_overrides (los pone el CSC del mapa; aqui 0).
bool __cdecl CG_SightTracePoint(int *hitNum, const float *start, const float *end, int mask, trace_t *trace)
{
    TraceExtents ext;
    for (int k = 0; k < 3; ++k) { ext.start.vec.v[k] = start[k]; ext.end.vec.v[k] = end[k]; }
    CM_CalcTraceExtents(&ext);
    int oldHitNum = *hitNum;
    *hitNum = 0;
    if (oldHitNum > 0 && --oldHitNum < (int)cm.numBrushes && CM_SightTracePointThroughBrush(&ext, &cm.brushes[oldHitNum]))
        *hitNum = oldHitNum + 1;
    if (*hitNum) return true;
    traceWork_t tw;
    trace->fraction = 1.0f; trace->startsolid = 0; trace->allsolid = 0;
    tw.contents = mask;
    memset(&tw.size, 0, 12);
    for (int k = 0; k < 3; ++k)
    {
        tw.extents.start.vec.v[k] = start[k]; tw.extents.end.vec.v[k] = end[k];
        tw.midpoint.vec.v[k] = (float)(0.5 * tw.extents.start.vec.v[k]) + (float)(0.5 * tw.extents.end.vec.v[k]);
        tw.delta.vec.v[k] = tw.extents.end.vec.v[k] - tw.extents.start.vec.v[k];
        tw.halfDelta.vec.v[k] = 0.5 * (float)(tw.extents.end.vec.v[k] - tw.extents.start.vec.v[k]);
    }
    memset(&tw.radiusOffset, 0, 12);
    for (int k = 0; k < 3; ++k) tw.halfDeltaAbs.vec.v[k] = fabsf(tw.halfDelta.vec.v[k]);
    CM_CalcTraceExtents(&tw.extents);
    tw.deltaLenSq = (float)((float)(tw.delta.vec.v[0] * tw.delta.vec.v[0]) + (float)(tw.delta.vec.v[1] * tw.delta.vec.v[1]))
                    + (float)(tw.delta.vec.v[2] * tw.delta.vec.v[2]);
    tw.deltaLen = sqrtf(tw.deltaLenSq);
    Vec3Min(tw.extents.start.vec.v, tw.extents.end.vec.v, tw.bounds[0].vec.v);
    Vec3Max(tw.extents.start.vec.v, tw.extents.end.vec.v, tw.bounds[1].vec.v);
    tw.axialCullOnly = 0;
    tw.isPoint = 1;
    memset(&tw.radius, 0, 12);
    CM_GetTraceThreadInfo(&tw.threadInfo);
    *hitNum = CM_SightTraceThroughTree(&tw, 0, tw.extents.start.vec.v, tw.extents.end.vec.v, trace);
    if (!*hitNum) *hitNum = CM_Mapkit_SightTrace(&tw, trace);
    return *hitNum != 0;
}

// OLA 14: hilo de oclusion ORIGINAL (win_threads.cpp Sys_SpawnOcclusion/Sys_SetOcclusion/Sys_WaitForOcclusion = hilo + evento de
// auto-reinicio). SND_LosOcclusionThreadMain (snd_occlusion.cpp) corre tal cual en un std::thread con su TraceThreadInfo propio
// (TLS por hilo en rt2.cpp) y la seccion critica CRITSEC_SOUND_OCCLUSION real (rt2.cpp): SND_LosOcclusionUpdate (frame de sonido)
// copia oyente/voz y despierta al hilo; SND_LosOcclusionCmd lanza las trazas (CG_SightTracePoint, arriba) y el siguiente Update
// aplica el resultado al fader losOcclusion de cada voz. H6A_OCCTHREAD=0: la misma traza en el hilo de juego, dentro de
// Sys_SetOcclusion (equivalente a un hilo que acaba al instante; para depurar). snd_losOcclusion 0 la apaga como en el original.
void __cdecl SND_LosOcclusionCmd();
void Sys_SetValue(int, void *);   // rt2.cpp
namespace {
std::thread *s_occThread; std::mutex s_occM; std::condition_variable s_occCv; bool s_occSignal; std::atomic<bool> s_occQuit{ false };
bool s_occSync; std::atomic<unsigned> s_occCmds{ 0 }; std::atomic<unsigned long long> s_occUs{ 0 };
TraceThreadInfo s_occTi; cbrush_t s_occBrush; cmodel_t s_occModel; PhysGeomList *s_occGeoms;
void OccCmdTimed()
{
    const double t = NowMs();
    SND_LosOcclusionCmd();
    s_occUs += (unsigned long long)((NowMs() - t) * 1000.0); ++s_occCmds;
}
} // namespace
bool __cdecl Sys_QueryWin32QuitEvent() { return !s_occQuit; }   // el original: true mientras el evento de salida NO esta puesto
void __cdecl Sys_SpawnOcclusion(void (__cdecl *function)(unsigned int))
{
    s_occSync = getenv("H6A_OCCTHREAD") && !atoi(getenv("H6A_OCCTHREAD"));
    if (s_occSync || s_occThread) { fprintf(stderr, "[H6A] oclusion: trazas en el hilo de juego (H6A_OCCTHREAD=0)\n"); return; }
    // TraceThreadInfo del hilo nuevo como el de CmInit (world_h4.cpp): contadores por particion propios y caja de trazas propia
    const int n = cm.partitionCount > 0 ? cm.partitionCount : 1;
    int *cc = (int *)Ptr32_ArenaAlloc(4 * n, 64); memset(cc, 0, 4 * n);
    memset(&s_occTi, 0, sizeof s_occTi); s_occTi.checkcount.partitions = cc;
    s_occTi.box_brush = &s_occBrush; s_occTi.box_model = &s_occModel; s_occTi.geoms = &s_occGeoms;
    s_occThread = new std::thread([function] { Sys_SetValue(3, &s_occTi); function(0); });
    // el harness sale con exit() sin SND_Shutdown: parar el hilo antes de los destructores estaticos
    atexit([] {
        g_snd.occlusionRunning = 0; s_occQuit = true;
        { std::lock_guard<std::mutex> l(s_occM); s_occSignal = true; } s_occCv.notify_all();
        if (s_occThread && s_occThread->joinable()) s_occThread->join();
    });
    fprintf(stderr, "[H6A] oclusion: hilo ORIGINAL SND_LosOcclusionThreadMain lanzado (%d particiones de colision)\n", n);
}
void __cdecl Sys_SetOcclusion()
{
    if (s_occSync) { if (g_snd.occlusionRunning && snd_losOcclusion->current.enabled) OccCmdTimed(); return; }
    { std::lock_guard<std::mutex> l(s_occM); s_occSignal = true; }
    s_occCv.notify_one();
}
void __cdecl Sys_WaitForOcclusion()
{
    std::unique_lock<std::mutex> l(s_occM);
    s_occCv.wait(l, [] { return s_occSignal || s_occQuit.load(); });
    s_occSignal = false;   // evento de auto-reinicio
}
// tandas de trazas hechas (lastTraceSequence las cuenta en los dos modos) y, en el modo sin hilo, su coste en el hilo de juego
void H6A_OccInfo(unsigned *seq, unsigned *syncCmds, double *syncMs) { *seq = g_snd.lastTraceSequence; *syncCmds = s_occCmds; *syncMs = s_occUs / 1000.0; }

// ---------------------------------------------------------------- OLA 14: notetracks de las animaciones (pasos de la IA, sndnt#)
// En el original el cliente procesa los notetracks de las animaciones de sus centities (CG_ProcessClientNote, cg_ents_mp.cpp): un
// "sndnt#<alias>" suena en la entidad (CG_PlayClientSoundNoteTracks) y un "footstep_left/right*" de un actor marca el pie, que
// CG_SP_DoAIFootsteps (cg_actors_mp.cpp) toca ese frame si el actor esta dentro de footstep_sounds_cutoff del ojo: traza de 15 u
// arriba/abajo desde el hueso j_ball_le/ri (dentro de 1024 u; si no el origen) con CM_TracePointDown(0x2820011, 0x3F00000) y el
// script _footsteps.csc playAIFootstep = playsound(alias, pos) con alias level.footstepPrepend ("fly_step_zombie_" en
// _zombiemode.csc) + Com_SurfaceTypeToName; los perros "fly_dog_step_run_default". Aqui no hay animacion de cliente: el servidor
// anima las mismas XAnim con los mismos notetracks y el gancho g_h12NoteHook (xanim.cpp, NotifyServerNotetrack) los entrega.
extern void (*g_h12NoteHook)(int, unsigned, unsigned) __attribute__((weak));
int __cdecl G_DObjGetWorldTagPos(const gentity_s *ent, unsigned int tagName, float *pos);
namespace {
void (*s_prevNoteHook)(int, unsigned, unsigned);
float s_camOrg[3]; bool s_haveCam;
struct FootNote { int ent; bool left; };
std::vector<FootNote> s_footQ;
int s_aiSteps, s_sndNotes;
void NoteHook(int entnum, unsigned notifyName, unsigned noteName)
{
    if (s_prevNoteHook) s_prevNoteHook(entnum, notifyName, noteName);
    if (!s_init || entnum < 0 || entnum >= level.num_entities) return;
    const gentity_s *e = &g_entities[entnum];
    const char *note = noteName ? SL_ConvertToString((unsigned short)noteName, SCRIPTINSTANCE_SERVER) : "";
    if (!note || !*note) return;
    if (!I_strnicmp(note, "sndnt#", 6))
    {
        float org[3]; EntOrigin(entnum, org);
        ++s_sndNotes;
        CG_PlayClientSoundNoteTracks(0, entnum, org, note, false);
        return;
    }
    if (e->s.eType != ET_ACTOR) return;   // CG_SP_DoAIFootsteps corre desde CG_Actor: solo actores
    const bool dog = Actor_IsDogSpecies(e->s.lerp.u.actor.species);
    if (!I_strnicmp(note, "footstep_", 9)) s_footQ.push_back({ entnum, note[9] == 'L' || note[9] == 'l' });
    else if (dog && !I_strnicmp(note, "dogstep_", 8)) s_footQ.push_back({ entnum, note[9] == 'L' || note[9] == 'l' });
}
} // namespace
void H19_AIFootFx(const float *pos, const char *surf, bool onFire) __attribute__((weak));   // h19_mapcl.cpp (OLA E1b)
const char *H19_AIFootPrepend(int entnum) __attribute__((weak));   // OLA E2b: self.footstepPrepend del CSC del mapa (nullptr = el de nivel)
namespace {
void PlayAIFootstep(const gentity_s *e, bool left, bool dog)
{
    float foot[3]; memcpy(foot, e->r.currentOrigin, 12);
    const float dx = foot[0] - s_camOrg[0], dy = foot[1] - s_camOrg[1], dz = foot[2] - s_camOrg[2];
    if (dx * dx + dy * dy + dz * dz < 1048576.0f && !G_DObjGetWorldTagPos(e, left ? scr_const.j_ball_le : scr_const.j_ball_ri, foot))
        memcpy(foot, e->r.currentOrigin, 12);
    float st[3] = { foot[0], foot[1], foot[2] + 15.0f }, en[3] = { foot[0], foot[1], foot[2] - 15.0f };
    const int surf = CM_TracePointDown(st, en, 0x2820011, 0x3F00000, foot, nullptr, nullptr);
    char a[128];
    if (dog) snprintf(a, sizeof a, "fly_dog_step_run_default");
    else snprintf(a, sizeof a, "%s%s", H19_AIFootPrepend && H19_AIFootPrepend(e->s.number) ? H19_AIFootPrepend(e->s.number) : "fly_step_zombie_",
                  Com_SurfaceTypeToName((surf >> 20) & 0x3F));   // OLA E2b: ent.footstepPrepend del mapa (pasos del director de Coast)
    ++s_aiSteps;
    CG_PlaySound(0, ENTITYNUM_WORLD, foot, 0, 0, 1.0f, a);   // playsound(client, alias, pos): sonido en el mundo
    // OLA E1b: do_foot_effect (FX por suelo de setFootstepEffect); on_fire = eFlags2 0x200000 (cent->isBurning no existe aqui)
    if (H19_AIFootFx) H19_AIFootFx(foot, Com_SurfaceTypeToName((surf >> 20) & 0x3F), (e->s.lerp.eFlags2 & 0x200000) != 0);
}
// CG_SP_DoAIFootsteps: los pies marcados suenan si el actor esta dentro del corte; fuera se quedan marcados (como cent->leftFootstep)
void AIFootstepsFrame()
{
    if (!s_haveCam) { s_footQ.clear(); return; }
    const float cut = footstep_sounds_cutoff ? footstep_sounds_cutoff->current.value : 3000.0f;
    std::vector<FootNote> keep;
    for (auto &f : s_footQ)
    {
        const gentity_s *e = &g_entities[f.ent];
        if (!e->r.inuse || e->s.eType != ET_ACTOR) continue;
        const float dx = e->r.currentOrigin[0] - s_camOrg[0], dy = e->r.currentOrigin[1] - s_camOrg[1], dz = e->r.currentOrigin[2] - s_camOrg[2];
        if (dx * dx + dy * dy + dz * dz > cut * cut) { bool dup = false; for (auto &k : keep) dup |= k.ent == f.ent && k.left == f.left; if (!dup) keep.push_back(f); continue; }
        PlayAIFootstep(e, f.left, Actor_IsDogSpecies(e->s.lerp.u.actor.species));
    }
    s_footQ.swap(keep);
}
void NoteHookChain()
{
    if (!&g_h12NoteHook) return;   // sin el gancho de xanim.cpp (ola 12) no hay notetracks del servidor
    if (g_h12NoteHook != NoteHook) { s_prevNoteHook = g_h12NoteHook; g_h12NoteHook = NoteHook; }
}

// ---------------------------------------------------------------- OLA 14: sistemas de cliente (sin VM de CSC)
// Generico: _load.csc levelNotifyHandler / last_stand_monitor. Lo de cada mapa (<zona>_amb.csc ascensores/portales/teletransporte,
// pantallas de la caja...) esta desde la OLA M2 en maps/h19_<zona>.cpp (ganchos audio* de h19_mapcl.h).
int FakeAt(const float *o) { const int f = FakeSpawn(); if (f && o) memcpy(s_fake[f].org, o, 12); return f; }
void WorldSound(const float *o, const char *alias) { float p[3] = { o[0], o[1], o[2] }; CG_PlaySoundWithHandle(0, 1022, p, 0, 0, 1.0f, SND_FindAliasId(alias)); }   // CScr_PlaySound
int LocalSound2D(const char *alias) { return CG_PlaySoundWithHandle(0, 1022, nullptr, 0, 0, 1.0f, SND_FindAliasId(alias)); }   // playsound(client, alias) sin posicion
int s_lsLoop; bool s_clSysStarted;
bool s_lastStand; double s_lsNextBeat, s_lsGaspAt; float s_lsPause, s_lsVol;
std::map<std::string, int> s_notifySeen;
void ClientSysStart()
{
    s_clSysStarted = true;
    H19S_EmittersStart();   // OLA F3: _audio.csc audio_init (random / looper / line_emitter) comun a todos los mapas
    if (const H19MapCl *mc = H19_Map()) if (mc->audioStart) mc->audioStart();   // OLA M2: entidades falsas del mapa (antes las de Five aqui)
}
void LevelNotify(const std::string &n)
{
    ++s_notifySeen[n];
    H19S_EmittersNotify(n);   // OLA F3: level waittill(script_string) de soundLoopThink / soundLineThink
    if (const H19MapCl *mc = H19_Map()) if (mc->audioNotify) mc->audioNotify(n);   // OLA M2 (Five: ascensores/minigun/portal; Kino: teletransporte)
}
// _load.csc last_stand_monitor / last_stand_thread / end_last_stand (los cambios de bus de _busing.csc no se emulan)
void LastStand(const std::string &st)
{
    gentity_s *pl = PlayerEnt();
    if (st == "1" && !s_lastStand)
    {
        if (!s_lsLoop) s_lsLoop = FakeAt(pl ? pl->r.currentOrigin : nullptr);
        FakePlayLoop(s_lsLoop, "evt_laststand_loop", 0.3f);
        s_lastStand = true; s_lsPause = 0.5f; s_lsVol = 0.5f; s_lsNextBeat = NowMs(); s_lsGaspAt = 0;
    }
    else if (st != "1" && s_lastStand) { if (s_lsLoop) FakeStopLoop(s_lsLoop, 0.7f); s_lastStand = false; s_lsGaspAt = NowMs() + 700.0; }
}
void ClientSysFrame()
{
    if (!s_clSysStarted) ClientSysStart();
    // H6A_SYSTEST=1 (solo prueba): el piloto no llega al interruptor ni a los ascensores, asi que se inyectan los mismos estados
    // que mandaria el servidor (setClientSysState / clientnotify / setclientflag) cada 4 s desde t=10 s
    static const bool sysTest = getenv("H6A_SYSTEST") && atoi(getenv("H6A_SYSTEST"));
    static int sysStep = 0; bool testFlag = false; static bool testFlagOn = false;
    if (sysTest && level.time >= 10000 + sysStep * 4000 && sysStep < 14)
    {
        static const char *seq[14][2] = { { "levelNotify", "ZPO" }, { "box_indicator", "n" }, { "levelNotify", "ele1" }, { "levelNotify", "ele1e" },
            { "levelNotify", "ele2" }, { "levelNotify", "ele2e" }, { "levelNotify", "minis" }, { "levelNotify", "minie" }, { "box_indicator", "f" },
            { "flag", "1" }, { "flag", "0" }, { "levelNotify", "pack_portal_fx_off" }, { "lsm", "1" }, { "lsm", "0" } };
        fprintf(stderr, "[H6A] autoprueba de sistemas t=%d: %s '%s'\n", level.time, seq[sysStep][0], seq[sysStep][1]);
        if (!strcmp(seq[sysStep][0], "flag")) testFlagOn = seq[sysStep][1][0] == '1';
        else s_cmdQ.push_back({ '9', seq[sysStep][0], seq[sysStep][1] });
        ++sysStep;
    }
    testFlag = sysTest && testFlagOn;
    for (auto &c : s_cmdQ)
    {
        if (c.type == 's') { CG_PlaySound(0, PlayerEnt() ? PlayerEnt()->s.number : 0, nullptr, 0, 0, 1.0f, (unsigned)strtoll(c.sys.c_str(), nullptr, 10)); ++s_localSnd; }
        else if (c.type == 'k') { CG_StopSoundAlias(0, PlayerEnt() ? PlayerEnt()->s.number : 0, (unsigned)strtoll(c.sys.c_str(), nullptr, 10)); ++s_localStop; }
        else if (c.type == 'q') { if (1.5287900e-05 > atof(c.sys.c_str())) SND_FadeOut(); else SND_FadeIn(); ++s_fades; }
        else if (!I_stricmp(c.sys.c_str(), "levelNotify")) { if (!c.state.empty()) LevelNotify(c.state); }
        else if (!I_stricmp(c.sys.c_str(), "lsm")) LastStand(c.state);
        else if (const H19MapCl *mc = H19_Map()) { if (mc->audioSys) mc->audioSys(c.sys, c.state); }   // OLA M2 (Five: box_indicator de las pantallas)
    }
    s_cmdQ.clear();
    H19S_EmittersFrame();   // OLA F3
    if (const H19MapCl *mc = H19_Map()) if (mc->audioFrame) mc->audioFrame(testFlag);   // OLA M2 (Kino: sonidos diferidos; Five: portales por la clientflag 5)
    const double now = NowMs();
    if (s_lastStand && now >= s_lsNextBeat)
    {
        SND_SetPlaybackAttenuation(LocalSound2D("chr_heart_beat"), s_lsVol);
        s_lsNextBeat = now + s_lsPause * 1000.0;
        s_lsPause = s_lsPause * 1.05f > 2.0f ? 2.0f : s_lsPause * 1.05f; s_lsVol = s_lsVol * 1.05f > 1.0f ? 1.0f : s_lsVol * 1.05f;
    }
    if (s_lsGaspAt > 0 && now >= s_lsGaspAt) { LocalSound2D("revive_gasp"); s_lsGaspAt = 0; }
}
} // namespace

// ---------------------------------------------------------------- OLA M2: ayudantes de h19_mapcl.h para los clientes de mapa
double H19A_NowMs() { return NowMs(); }
int H19A_FakeAt(const float *org) { return FakeAt(org); }
const float *H19A_FakeOrg(int f) { return s_fake[f].org; }
bool H19A_FakeLoopIs(int f, const char *alias) { return s_fake[f].loopSoundId == SND_FindAliasId(alias); }
void H19A_PlayLoop(int f, const char *alias, float fadeSec) { FakePlayLoop(f, alias, fadeSec); }
void H19A_StopLoop(int f, float fadeSec) { FakeStopLoop(f, fadeSec); }
void H19A_WorldSound(const float *org, const char *alias) { WorldSound(org, alias); }
gentity_s *H19A_PlayerEnt() { return PlayerEnt(); }
// OLA M10 (Moon): setAmbientRoomTone / setAmbientRoomSnapshot del CSC del mapa en juego (nullptr = no cambia) y "level.activeAmbientRoom = ''" +
// updateActiveAmbientRoom (se vuelve a aplicar la sala aunque sea la misma). Se declaran en el mapa que los usa (no en h19_mapcl.h).
void H19A_RoomSet(const char *room, const char *tone, const char *snapshot, float fadeIn, float fadeOut)
{
    auto it = s_rooms.find(room); if (it == s_rooms.end()) return;
    if (tone) { it->second.tone = tone; it->second.fadeIn = fadeIn; it->second.fadeOut = fadeOut; }
    if (snapshot) it->second.snapshot = snapshot;
}
void H19A_RoomRefresh()
{
    if (!s_curRoom.empty() && s_rooms.count(s_curRoom)) FakeStopLoop(s_rooms[s_curRoom].ent, s_rooms[s_curRoom].fadeOut);
    s_curRoom.clear(); RoomUpdate();
}

// ---------------------------------------------------------------- frame de audio (h5_play.cpp, una vez por frame de render)
static void H6A_Frame(bool serverFrame, const float *camOrg, const float *camAng)
{
    if (!s_init)
    {
        if (s_initFailed || !PlayerEnt()) return;   // el sistema de sonido arranca con el mapa cargado y el jugador dentro
        s_init = InitSound();
        if (!s_init) { s_initFailed = true; return; }
        s_lastFrameMs = NowMs();
    }
    // H6A_PACE=1: frames a 60 por segundo de reloj (con H5_REALTIME el juego mide dt de verdad); para grabar el WAV sin vista,
    // donde el backend trace va mucho mas rapido que el tiempo real y la salida de audio no
    static const bool pace = getenv("H6A_PACE") && atoi(getenv("H6A_PACE"));
    if (pace)
    {
        static double next = 0; const double now = NowMs();
        if (next > now) usleep((useconds_t)((next - now) * 1000.0));
        next = (next > now ? next : now) + 1000.0 / 60.0;
    }
    const double t0 = NowMs();
    const float frameMs = (float)(t0 - s_lastFrameMs); s_lastFrameMs = t0;
    NoteHookChain();
    if (camOrg) { memcpy(s_camOrg, camOrg, 12); s_haveCam = true; }
    if (serverFrame)
    {
        ServerFrameEvents(camOrg, camAng);
        AIFootstepsFrame();
        AmbTriggerFrame();
        for (auto &st : s_musicQ) MusicSet(st);
        s_musicQ.clear();
        ClientSysFrame();
        // H6A_WHIZTEST=<ms> (solo prueba, INYECTADO): el whizby solo lo provocan balas de otro jugador (co-op). Cada <ms> se pasa a
        // CG_SoundWhizby ORIGINAL una bala del arma del jugador que nace 800 u por delante y a la derecha y cruza a 24 u de la camara.
        static const int whizTest = getenv("H6A_WHIZTEST") ? atoi(getenv("H6A_WHIZTEST")) : 0;
        static int whizNext = 0;
        if (whizTest > 0 && camOrg && camAng && level.time >= whizNext)
        {
            whizNext = level.time + whizTest;
            gentity_s *wp = PlayerEnt();
            const unsigned w = wp && wp->client ? wp->client->ps.weapon : 0;
            if (!cgArray) cgArray = (cg_s *)calloc(1, sizeof(cg_s));   // como h5_weapon.cpp/h5_fx.cpp (trace: sin viewmodel)
            if (w && w < BG_GetNumWeapons())
            {
                memcpy(cgArray->refdef.vieworg, camOrg, 12);   // CG_SoundWhizby mide desde refdef.vieworg (la camara de este frame)
                float fw[3], rt[3], up[3]; AngleVectors(camAng, fw, rt, up);
                float st[3], en[3], dir[3];
                for (int k = 0; k < 3; ++k) { st[k] = camOrg[k] + fw[k] * 800.0f + rt[k] * 120.0f; en[k] = camOrg[k] - fw[k] * 400.0f + rt[k] * 24.0f; }
                float len = 0; for (int k = 0; k < 3; ++k) { dir[k] = en[k] - st[k]; len += dir[k] * dir[k]; }
                len = sqrtf(len); for (int k = 0; k < 3; ++k) dir[k] /= len;
                ++s_whizChecks;
                CG_SoundWhizby(0, BG_GetWeaponDef(w), st, dir, en);
            }
        }
        for (int n = 0; n < level.num_entities; ++n) { memcpy(s_prevOrg[n], s_curOrg[n], 12); memcpy(s_curOrg[n], g_entities[n].r.currentOrigin, 12); }
    }
    for (size_t i = 0; i < s_pendingStops.size();)
        if (t0 >= s_pendingStops[i].atMs) { SND_StopPlayback(s_pendingStops[i].id); s_pendingStops.erase(s_pendingStops.begin() + i); } else ++i;
    SND_BeginFrame(1, g_h5pPause, 1.0f, (unsigned)level.time, 0);
    LoopPump(frameMs);
    gentity_s *pl = PlayerEnt();
    if (pl)
    {
        float axis[3][3]; AnglesToAxis(camAng, axis);
        SND_SetListener(0, pl->s.number, G_GetClientTeam(pl->s.number), camOrg, axis);
    }
    CG_SndUpdate();
    SND_EndFrame();
    SND_Update();   // -> Sys_AddWorkerCmdInternal(updatesound) -> SND_Frame (comandos, voces, SD_*)
    static int s_lastLog = -1;
    if (level.time / 5000 != s_lastLog)
    {
        s_lastLog = level.time / 5000;
        unsigned files = 0; unsigned long long bytes = 0; H6A_CacheInfo(&files, &bytes);
        unsigned wh = 0, wm = 0; H6A_WmaInfo(&wh, &wm);
        unsigned oseq = 0, ocmd = 0; double oms = 0; H6A_OccInfo(&oseq, &ocmd, &oms);
        int occV = 0; float occMax = 0;   // voces con oclusion (fader losOcclusion > 0) en este momento
        for (int i = 0; i < 0x4A; ++i) if (g_snd.voiceAliasHash[i] && g_snd.voice[i].losOcclusion.value > 0.01f) { ++occV; if (g_snd.voice[i].losOcclusion.value > occMax) occMax = g_snd.voice[i].losOcclusion.value; }
        fprintf(stderr, "[H6A] cliente t=%d: eventos %d, peticiones de sonido %d, musica '%s', sala '%s', cache de streams %u ficheros %.1f MB, xWMA %u del pak / %u sin pak, frame de audio %.3f ms, oclusion %u tandas (%d voces ocluidas, max %.2f), notas sndnt %d, pasos de IA %d, impactos %d, whizby probados %d\n",
            level.time, s_events, s_plays, s_musicActive.c_str(), s_curRoom.c_str(), files, bytes / 1048576.0, wh, wm, NowMs() - t0, oseq, occV, occMax, s_sndNotes, s_aiSteps, s_impacts, s_whizChecks);
        std::string sy, no;
        for (auto &p : s_sysSeen) sy += " " + p.first + "x" + std::to_string(p.second);
        for (auto &p : s_notifySeen) no += " " + p.first + "x" + std::to_string(p.second);
        std::string ex; H19S_EmittersStatus(ex);   // OLA F3
        if (const H19MapCl *mc = H19_Map()) if (mc->audioStatus) mc->audioStatus(ex);   // OLA M2 (Five: "; portal pack, energia no")
        fprintf(stderr, "[H6A] cliente t=%d: playlocalsound %d, stoplocalsound %d, fundidos %d, sistemas:%s; levelNotify:%s%s\n",
            level.time, s_localSnd, s_localStop, s_fades, sy.c_str(), no.c_str(), ex.c_str());
    }
}

// OLA 15C: H6A_BUYTEST="T:x,y;@targetname;..." (prueba de sonido de compras/puertas, ENTRADA guionizada, no estados inyectados): desde el
// segundo T de partida el jugador camina hacia cada punto (x,y del mapa), mirandolo, y al llegar (48 u) mantiene USAR 1,5 s; el
// resto del tiempo manda el piloto. La compra la hacen los GSC originales (Player_UpdateActivate -> trigger_use).
#include "h5_input.h"
extern void (*g_h5pInputOverride)(H5PlayerInput *in, const float *camOrg, const float *camAng);
namespace {
struct BuyPt { float x, y, z = 1e9f; std::string tn; };   // tn: "@targetname" -> el mas cercano de esos (centro de absmin/absmax)
std::vector<BuyPt> s_buyPts; int s_buyT0 = -1; size_t s_buyIdx; int s_buyArrive = -1, s_buyStart = -1;
void BuyOverride(H5PlayerInput *in, const float *, const float *camAng)
{
    gentity_s *pl = PlayerEnt();
    if (!pl || !pl->client || level.time < s_buyT0 || s_buyIdx >= s_buyPts.size()) return;
    const float *o = pl->client->ps.origin; BuyPt &b = s_buyPts[s_buyIdx];
    if (s_buyStart < 0 && !b.tn.empty())
    {
        float best = 1e30f;
        for (int e = 0; e < level.num_entities; ++e)
        {
            const gentity_s *g = &g_entities[e];
            if (!g->r.inuse || !g->targetname || strcmp(SL_ConvertToString(g->targetname, SCRIPTINSTANCE_SERVER), b.tn.c_str())) continue;
            float c[3]; for (int k = 0; k < 3; ++k) c[k] = 0.5f * (g->r.absmin[k] + g->r.absmax[k]);
            const float dd = (c[0] - o[0]) * (c[0] - o[0]) + (c[1] - o[1]) * (c[1] - o[1]) + 4.0f * (c[2] - o[2]) * (c[2] - o[2]);
            if (dd < best) { best = dd; b.x = c[0]; b.y = c[1]; b.z = c[2]; }
        }
        if (best < 1e30f) fprintf(stderr, "[H6A] compra-prueba: '%s' mas cercano en (%.0f,%.0f,%.0f)\n", b.tn.c_str(), b.x, b.y, b.z);
    }
    if (s_buyStart < 0) { s_buyStart = level.time; fprintf(stderr, "[H6A] compra-prueba: voy al punto %zu (%.0f,%.0f) desde (%.0f,%.0f), puntos %d\n", s_buyIdx, b.x, b.y, o[0], o[1], pl->client->sess.cs.score.score); }
    const float dx = b.x - o[0], dy = b.y - o[1], d = sqrtf(dx * dx + dy * dy);
    const float yaw = atan2f(dy, dx) * (180.0f / 3.14159265f);
    float dyaw = AngleDelta(yaw, camAng[1]); if (dyaw > 8) dyaw = 8; if (dyaw < -8) dyaw = -8;
    memset(in, 0, sizeof *in);
    float pitch = 0; if (b.z < 1e8f) pitch = -atan2f(b.z - (o[2] + 60.0f), d > 1 ? d : 1) * (180.0f / 3.14159265f);   // mirar al centro del disparador
    float dp = AngleDelta(pitch, camAng[0]); if (dp > 5) dp = 5; if (dp < -5) dp = -5;
    in->lookYaw = dyaw; in->lookPitch = dp;
    if (d > 48.0f && s_buyArrive < 0 && level.time - s_buyStart < 30000)
    {
        // atascado (menos de 4 u en 400 ms): rodea el obstaculo de lado 900 ms, alternando el lado en cada atasco
        static float lastO[2]; static int lastT, sideUntil, side = 1;
        if (level.time - lastT >= 400) { const float m = fabsf(o[0] - lastO[0]) + fabsf(o[1] - lastO[1]); if (m < 4.0f && level.time > sideUntil) { sideUntil = level.time + 900; side = -side; } lastO[0] = o[0]; lastO[1] = o[1]; lastT = level.time; }
        in->moveFwd = fabsf(dyaw) < 30 ? 1.0f : 0.2f;
        if (level.time < sideUntil) { in->moveFwd = -0.3f; in->moveRight = (float)side; }
        return;
    }
    if (s_buyArrive < 0) { s_buyArrive = level.time; fprintf(stderr, "[H6A] compra-prueba: punto %zu a %.0f u (%s), pulso USAR\n", s_buyIdx, d, d > 48 ? "no llego" : "llegue"); }
    if (level.time - s_buyArrive < 1500) { if ((level.time / 300) & 1) in->buttons |= H5IN_USE; return; }
    fprintf(stderr, "[H6A] compra-prueba: fin del punto %zu, puntos %d, arma %d\n", s_buyIdx, pl->client->sess.cs.score.score, pl->client->ps.weapon);
    ++s_buyIdx; s_buyArrive = -1; s_buyStart = -1;
}
struct H6AInstall
{
    H6AInstall()
    {
        g_h5pAudio = H6A_Frame;
        if (const char *b = getenv("H6A_BUYTEST"))
        {
            s_buyT0 = atoi(b) * 1000; const char *p = strchr(b, ':');
            while (p && *p)
            {
                BuyPt q;
                if (p[1] == '@') { const char *e = strchr(p + 1, ';'); q.tn.assign(p + 2, e ? (size_t)(e - p - 2) : strlen(p + 2)); q.x = q.y = 0; s_buyPts.push_back(q); }
                else if (sscanf(p + 1, "%f,%f", &q.x, &q.y) == 2) s_buyPts.push_back(q);
                p = strchr(p + 1, ';');
            }
            if (!s_buyPts.empty()) g_h5pInputOverride = BuyOverride;
        }
    }
} s_h6aInstall;
}
