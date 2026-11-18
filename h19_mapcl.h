// OLA M2: cliente ESPECIFICO de cada mapa, separado de los ficheros comunes.
//
// En el original cada mapa trae sus clientscripts (<zona>.csc, <zona>_amb.csc, <zona>_fx.csc, <zona>_teleporter.csc...). El port no
// tiene VM de CSC: lo que hacen se reproduce a mano en C++. Hasta M1 ese codigo estaba mezclado en h7e_light.cpp, h5_audio_cl.cpp y
// h9_video.cpp detras de comparaciones de mapname. Desde M2:
//   - cada mapa tiene UN fichero propio maps/h19_<zona>.cpp que define `extern const H19MapCl kH19_<zona>` (struct de ganchos de abajo);
//   - h19_mapcl.cpp tiene la tabla kTable con una entrada por mapa y elige la del mapname en curso (H19_Map());
//   - los ficheros comunes (h7e_light.cpp, h5_audio_cl.cpp, h9_video.cpp, h8_doors.cpp) SOLO llaman a los ganchos de la entrada elegida;
//     lo generico (_lights.csc, _ambient.csc, _ambientpackage.csc, _load.csc, _music.csc) se queda en los comunes.
// Un mapa sin entrada (o con todos los ganchos a nullptr) usa solo el comportamiento generico.
//
// Reglas (ver docs/NUEVO_MAPA.md):
//   - Un mapa NO instala ganchos propios de comandos de servidor (g_h0ServerCmdHook). Los '9' de ClientSysSetState le llegan ya
//     decodificados por sysState / levelNotify (cliente grafico) y audioSys / audioNotify (sonido). La cadena audio -> h7e -> h13 la
//     montan los comunes una sola vez (leccion de M1: sustituir el gancho perdia el del sonido).
//   - Todo el estado del mapa vive en su fichero (namespace anonimo). Los ayudantes de abajo son la unica puerta a los comunes.
//   - Este .h lo incluyen varios comunes: tocarlo recompila TODO (build.sh sigue los .h). Solo para anadir un gancho nuevo, al final.
//   - Variables: H19_MAPCL=0 apaga todos los clientes de mapa; la variable offEnv de cada entrada (p.ej. H19_KINO=0) apaga el suyo.
#pragma once
#include <map>
#include <string>
#include <vector>
#include <stdlib.h>
#include <stdio.h>

typedef std::map<std::string, std::string> H19Kv;   // una entidad del mapa (pares clave/valor de la cadena de MapEnts)

// hora de los relojes de _ambient.csc (h7e_light.cpp ClocksFrame): hora 1..12 local, minutos y segundos al arrancar, segundos enteros desde entonces
struct H19Clock { int h12, lm, ls; float el; };

struct H19MapCl {
    const char *zone;     // mapname (p.ej. "zombie_theater")
    const char *offEnv;   // variable que con =0 apaga este cliente (nullptr: solo H19_MAPCL)
    const char *estado;   // texto corto para el log de arranque ([H19])

    // ---- cliente grafico (h7e_light.cpp, en el frame de render; solo si H7E_CLIENT no es 0)
    void (*init)(const std::vector<H19Kv> &ents);                        // tras crear luces de script y script_model de cliente del mapa
    bool (*levelNotify)(const std::string &ev, int now);                 // levelNotify del servidor; true = consumido (sin mezcladores genericos)
    bool (*sysState)(const char *sys, const std::string &st, int now);   // cualquier '9' (ClientSysSetState); true = consumido (no sigue a levelNotify)
    void (*frame)(int now);                                              // cada frame de render, tras luces y relojes genericos
    bool (*clockHand)(const char *tn, const H19Clock &c, float *add);    // agujas propias (targetname no generico); true = *add es el pitch a sumar
    void (*screensFrame)();                                              // pantallas (h9_video.cpp H9V_Frame, desde h9_shadow.cpp)
    const char *(*levelEffect)(const char *key);                         // level._effect[key] del <zona>_fx.csc (FX de luces giratorias); nullptr = no hay

    // ---- sonido (h5_audio_cl.cpp, frame de servidor; solo con sonido compilado, H6SND)
    void (*audioStart)();                                                // una vez, en el primer frame de sistemas de cliente
    void (*audioNotify)(const std::string &n);                           // levelNotify
    void (*audioSys)(const std::string &sys, const std::string &st);     // resto de sistemas (no levelNotify / musicCmd / lsm)
    void (*audioFrame)(bool sysTestFlag);                                // cada frame de servidor; sysTestFlag = bandera inyectada por H6A_SYSTEST
    void (*audioStatus)(std::string &out);                               // texto extra del log periodico [H6A] cliente

    // ---- servidor (diagnostico de h8_doors.cpp; la logica de puertas/barricadas/trampas es el GSC original)
    bool (*doorEnt)(const char *tn, const char *sn, const char *cn);    // entidades extra a seguir en el log [H8D] (puertas con nombre propio)
};

// entrada del mapa en curso (nullptr = generico). Se resuelve la primera vez que mapname tiene valor y se queda fija (cambiar de mapa
// reinicia la app, OLA M0).
const H19MapCl *H19_Map();
inline bool H19_EnvOn(const char *n) { const char *e = getenv(n); return !(e && !atoi(e)); }

// ---- entidades del mapa (h19_mapcl.cpp): MapEnts del ff ya partido, en el orden del fichero
std::vector<H19Kv> H19_ParseEnts(const char *s, int n);
const std::vector<H19Kv> &H19_MapEnts();
bool H19_Vec(const H19Kv &k, const char *key, float *v);
const char *H19_Str(const H19Kv &k, const char *key);

// ---- efectos (h19_mapcl.cpp; sin H5FX no hacen nada y devuelven 0)
unsigned H19_PlayFx(const char *name, const float *org, const float *fwd, const float *up);   // SpawnFx + triggerfx / PlayFXOnTag orientado
unsigned H19_PlayFxAng(const char *name, const float *org, const float *ang);
void H19_KillFx(unsigned &h);                                                                 // Delete() del ent de FX (las particulas nacidas siguen)

// ---- estado de cliente de h7e_light.cpp (H5G)
int H19_AddModel(const char *name, const float *org, const float *ang, const char *tn);   // spawn(script_model) de cliente; -1 si el modelo no esta
void H19_SetModel(int idx, const char *name);                                              // SetModel del CSC
int H19_SwapModelByTn(const char *tn, const char *from, const char *to);                   // SetModel(to) en los de ese targetname con modelo from
void H19_MixerActivate(const std::string &ev, int now);                                    // mixer_level_activate de _lights.csc (script_mixer_event)
void H19_LevelNotify(const std::string &ev, int now);                                      // reinyecta un levelNotify por el camino comun
float H19_Rand01();                                                                        // RandomFloat de cliente (MISMA secuencia que fire_flicker)

// ---- sonido de h5_audio_cl.cpp (H6SND): entidades falsas de script de cliente y sonidos
double H19A_NowMs();
int H19A_FakeAt(const float *org);                                    // spawnfakeent + origen
const float *H19A_FakeOrg(int f);
bool H19A_FakeLoopIs(int f, const char *alias);
void H19A_PlayLoop(int f, const char *alias, float fadeSec);          // playloopsound
void H19A_StopLoop(int f, float fadeSec);                             // stoploopsound
void H19A_WorldSound(const float *org, const char *alias);            // playsound(0, alias, origin)
struct gentity_s; gentity_s *H19A_PlayerEnt();
