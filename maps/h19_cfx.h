// OLA F1: createfx de cliente compartido por los mapas que aun no lo tenian (Five y Kino). Reproduce clientscripts/_fx.csc:
//   - level._effect[clave] = LoadFx("ruta") del <zona>_fx.csc (se lee del rawfile, no se copian numeros);
//   - createOneshotEffect: oneshotfxthread (si delay > 0 espera delay s) + create_triggerfx (playFx de 6 parametros: inicio = delay*1000 ms
//     desde el arranque; delay -15 = el efecto ya llevaba 15 s);
//   - createLoopEffect: loopfxthread (playfx cada "delay" s, minimo 0,05) como en _fx.csc;
//   - createExploder: los activa el servidor con ActivateClientExploder (h7e_light.cpp los pasa al mapa como sysState("@exploder", id),
//     id = orden de primera aparicion del numero en el createfx, desde 1, como level._exploder_ids); cannon_effect borra los anteriores y
//     SpawnFx(fx, origen, delay, forward, up) + triggerfx tras exploder_delay; id negativo = deactivate_exploder.
// Los otros mapas llevan su propia copia de esto (M3..M10); no se tocan.
#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct H19Cfx {
    struct Ent { float org[3], ang[3], delay; std::string fxid; int exploder = -1; bool loop = false, done = false; int nextLoop = -1; std::vector<unsigned> fx; };
    struct Task { int at; size_t ent; };
    std::string tag;                              // nombre corto para el log
    std::map<std::string, std::string> effect;    // level._effect[clave] -> ruta de LoadFx
    std::vector<Ent> ents;
    std::map<int, int> exploderId;                // numero -> id (1, 2...)
    std::vector<Task> tasks;
    std::mutex mu; std::vector<int> pending;      // ids de exploder recibidos (hilo de comandos) -> frame
    int t0 = -1, played = 0, missing = 0, loops = 0, explodersOn = 0;
    bool logged = false;
    // _zombiemode.csc init_perk_machines_fx / perk_start_up: parpadeo de las luces en los script_struct "perksacola" con la corriente
    struct Perk { float org[3]; std::string fx; };
    std::vector<Perk> perks; int perkStep = -1, perkAt = 0; float perkDur = 0, perkTimer = 0; std::vector<std::pair<unsigned, int>> perkFx;
};

std::string H19_RawText(const char *name);                                          // rawfile de script descomprimido ("" si no esta)
// carga level._effect de fxCsc (puede llamarse varias veces con varios ficheros) y los createfx de createfxCsc
void H19Cfx_Load(H19Cfx &c, const char *tag, const char *fxCsc, const char *createfxCsc);
void H19Cfx_Frame(H19Cfx &c, int now);                                               // en el frame de render (now = level.time)
void H19Cfx_QueueExploder(H19Cfx &c, const std::string &st);                         // sysState("@exploder", st)
void H19Cfx_PerkInit(H19Cfx &c, const std::vector<std::map<std::string, std::string>> &ents);   // perksacola + level._effect de _zombiemode.csc
void H19Cfx_PerkStart(H19Cfx &c, int now);                                           // level notify("power_on") (ZPO_listener)
const char *H19Cfx_Path(const H19Cfx &c, const char *key);                           // ruta de level._effect[key] o nullptr
