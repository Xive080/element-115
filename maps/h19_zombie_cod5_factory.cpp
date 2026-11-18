// OLA M2: cliente de zombie_cod5_factory (Der Riese). PLANTILLA: aun sin cliente propio (todos los ganchos a nullptr = comportamiento generico).
// Rellenar desde sus clientscripts (rawfiles del ff: clientscripts/zombie_cod5_factory.csc, zombie_cod5_factory_amb.csc, zombie_cod5_factory_fx.csc...) con los mismos
// numeros. Pasos y reglas: docs/NUEVO_MAPA.md. Ejemplos completos: maps/h19_zombie_theater.cpp (Kino) y maps/h19_zombie_pentagon.cpp (Five).
// Este fichero es SOLO de este mapa: no tocar comunes (h7e_light.cpp, h5_audio_cl.cpp...) para cosas de zombie_cod5_factory.
//   H19_FACTORY=0 lo apaga.
#ifdef H5G
#include <universal/q_shared.h>
#include <game_mp/g_main_mp.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "h19_mapcl.h"

namespace {
// ---- cliente grafico (h7e_light.cpp, frame de render)
// void Init(const std::vector<H19Kv> &ents) { }                                  // entidades del mapa ya partidas (script_struct, script_model...)
// bool LevelNotify(const std::string &ev, int now) { return false; }             // true = consumido; false = sigue a los mezcladores genericos
// bool SysState(const char *sys, const std::string &st, int now) { return false; }   // '9' de ClientSysSetState (box_indicator...)
// void Frame(int now) { }
// bool ClockHand(const char *tn, const H19Clock &c, float *add) { return false; }
// void ScreensFrame() { }
// const char *LevelEffect(const char *key) { return nullptr; }                    // level._effect[key] de zombie_cod5_factory_fx.csc
// ---- sonido (h5_audio_cl.cpp, frame de servidor; solo con H6SND)
#ifdef H6SND
// void AudioStart() { }
// void AudioNotify(const std::string &n) { }
// void AudioSys(const std::string &sys, const std::string &st) { }
// void AudioFrame(bool sysTestFlag) { }
// void AudioStatus(std::string &out) { }
#endif
// ---- servidor (diagnostico [H8D])
// bool DoorEnt(const char *tn, const char *sn, const char *cn) { return false; }
} // namespace

extern const H19MapCl kH19_zombie_cod5_factory = {
    .zone = "zombie_cod5_factory", .offEnv = "H19_FACTORY", .estado = "Der Riese: sin cliente propio todavia (plantilla OLA M2)",
    .init = nullptr, .levelNotify = nullptr, .sysState = nullptr, .frame = nullptr, .clockHand = nullptr, .screensFrame = nullptr,
    .levelEffect = nullptr,
#ifdef H6SND
    .audioStart = nullptr, .audioNotify = nullptr, .audioSys = nullptr, .audioFrame = nullptr, .audioStatus = nullptr,
#endif
    .doorEnt = nullptr,
};
#endif
