// OLA F3 (comun): emisores de sonido de cliente de clientscripts/_audio.csc, para TODOS los mapas. En el original los arranca
// audio_init (lo llama clientscripts/_zombiemode.csc en cualquier mapa de zombis):
//  - startSoundRandoms  -> soundRandom_Thread: GetStructArray("random", "script_label"); cada uno espera
//    RandomFloatRange(script_wait_min, script_wait_max) (por defecto 1 y 3 s) y hace playsound(0, script_sound, origin), sin fin;
//  - startSoundLoops    -> soundLoopThink: GetStructArray("looper", "script_label"); soundloopemitter(script_sound, origin) si
//    script_int no es 0 (por defecto arranca); con script_string, cada level notify de ese nombre lo apaga / enciende;
//  - startLineEmitters  -> soundLineThink: GetStructArray("line_emitter", "script_label"); soundLineEmitter(script_sound, origin,
//    getstruct(target).origin), con el mismo script_int / script_string. El motor coloca el sonido en el punto de la linea mas
//    cercano al oyente.
// Hasta E2a solo Shangri-La lo tenia (dentro de maps/h19_zombie_temple.cpp); aqui queda comun. Vive en maps/ porque build.sh compila
// maps/h19_*.cpp con un comodin; no tiene entrada en kTable. Lo llama h5_audio_cl.cpp (ClientSysStart, LevelNotify, ClientSysFrame).
// Los structs salen de las MapEnts del mapa (script_struct). La distancia la corta el propio motor de sonido (distReverbMax del alias).
// OLA S (selftests): looper y line_emitter van por los emisores del motor como CScr_SoundLoopEmitter / CScr_SoundLineEmitter
// (SND_PlayLoopAt / SND_PlayLineAt: 256 y 128 ranuras que SND_UpdateStaticSounds toca cada frame sin gastar playbacks; el punto de
// la linea mas cercano al oyente lo calcula el motor). Antes eran playloopsound en entidades falsas: cada uno gastaba un playback y
// los 213 bucles de Five agotaban los 0x94 ("ran out of playbacks").
// H19_SNDEMIT=0 lo apaga.
#if defined(H5G) && defined(H6SND)
#include <universal/q_shared.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <bgame/bg_local.h>
#include <sound/snd_public_async.h>
#include <sound/snd_bank.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "h19_mapcl.h"

namespace {
struct Emit { int kind; std::string snd, notify; float a[3], b[3]; bool on; unsigned id; float wmin, wmax; double next; };   // kind 0 random, 1 looper, 2 linea
std::vector<Emit> s_emit;
int s_count[3], s_plays, s_toggles;
bool s_started;
float ARand(float a, float b) { return a + (b - a) * (float)rand() / (float)RAND_MAX; }   // RandomFloatRange
void Toggle(Emit &e)
{
    if (e.kind == 1) { if (e.on) SND_StopLoopAt(e.id, e.a); else SND_PlayLoopAt(e.id, e.a); }   // soundstoploopemitter / soundloopemitter
    else if (e.kind == 2) { if (e.on) SND_StopLineAt(e.id, e.a, e.b); else SND_PlayLineAt(e.id, e.a, e.b); }   // soundstoplineemitter / soundlineemitter
    e.on = !e.on; ++s_toggles;
}
} // namespace

void H19S_EmittersStart()
{
    if (s_started) return;
    s_started = true;
    if (!H19_EnvOn("H19_SNDEMIT") || !H19_EnvOn("H19_MAPCL")) { fprintf(stderr, "[H19] emisores de _audio.csc: apagados\n"); return; }
    std::map<std::string, const H19Kv *> byTn;   // getstruct(target, "targetname"): el ultimo con ese nombre, como level.struct_class_names
    for (const H19Kv &k : H19_MapEnts()) if (const char *tn = H19_Str(k, "targetname")) byTn[tn] = &k;
    for (const H19Kv &k : H19_MapEnts()) {
        const char *lab = H19_Str(k, "script_label"), *snd = H19_Str(k, "script_sound"); if (!lab) continue;
        Emit e{}; e.kind = !strcmp(lab, "random") ? 0 : !strcmp(lab, "looper") ? 1 : !strcmp(lab, "line_emitter") ? 2 : -1;
        if (e.kind < 0 || !snd || !H19_Vec(k, "origin", e.a)) continue;
        e.snd = snd; memcpy(e.b, e.a, 12); if (const char *ss = H19_Str(k, "script_string")) e.notify = ss;
        if (e.kind == 2) { const char *t = H19_Str(k, "target"); auto it = t ? byTn.find(t) : byTn.end(); if (it == byTn.end() || !H19_Vec(*it->second, "origin", e.b)) continue; }
        if (e.kind == 0) { const char *a = H19_Str(k, "script_wait_min"), *b = H19_Str(k, "script_wait_max"); e.wmin = a ? (float)atof(a) : 1; e.wmax = b ? (float)atof(b) : 3;
            e.notify.clear(); e.next = H19A_NowMs() + ARand(e.wmin, e.wmax) * 1000.0; }
        else { const char *si = H19_Str(k, "script_int"); const bool started = !si || atoi(si) != 0; e.id = SND_FindAliasId(e.snd.c_str());
            if (started) { if (e.kind == 1) SND_PlayLoopAt(e.id, e.a); else SND_PlayLineAt(e.id, e.a, e.b); e.on = true; } }
        ++s_count[e.kind]; s_emit.push_back(e);
    }
    std::string ns; for (const Emit &e : s_emit) if (!e.notify.empty() && ns.find(" " + e.notify + " ") == std::string::npos) ns += " " + e.notify + " ";
    fprintf(stderr, "[H19] emisores de _audio.csc: %d random, %d looper, %d line_emitter; notifies:%s\n", s_count[0], s_count[1], s_count[2], ns.empty() ? " (ninguno)" : ns.c_str());
}

// level waittill(script_string) de soundLoopThink / soundLineThink: cada notify con ese nombre alterna el emisor
void H19S_EmittersNotify(const std::string &n)
{
    for (Emit &e : s_emit) if (!e.notify.empty() && e.notify == n) Toggle(e);
}

void H19S_EmittersFrame()
{
    if (s_emit.empty()) return;
    const double now = H19A_NowMs();
    for (Emit &e : s_emit)
        if (e.kind == 0 && now >= e.next) { H19A_WorldSound(e.a, e.snd.c_str()); ++s_plays; e.next = now + ARand(e.wmin, e.wmax) * 1000.0; }   // soundRandom_Thread
}

void H19S_EmittersStatus(std::string &out)
{
    int on = 0; for (const Emit &e : s_emit) on += e.kind != 0 && e.on;
    char b[160]; snprintf(b, sizeof b, "; emisores _audio.csc %zu (%d bucles sonando, %d aleatorios sonados, %d alternados)", s_emit.size(), on, s_plays, s_toggles);
    out += b;
}
#endif
