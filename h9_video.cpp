// OLA 9: "video" de las pantallas de los mapas. Desde la OLA M2 es solo el punto de llamada comun (h9_shadow.cpp H9_PreFrame llama a
// H9V_Frame una vez por frame de render): el codigo de las pantallas de cada mapa vive en maps/h19_<zona>.cpp (gancho screensFrame
// de h19_mapcl.h). Five: pantallas de la caja magica (pentagon_screen_switch), antes aqui, ahora en maps/h19_zombie_pentagon.cpp.
// No hay Bink en los mapas: r_cinematic.cpp solo se usa para las cinematicas globales del menu.
#ifdef H5G
#include "h19_mapcl.h"
void H9V_Frame()
{
    if (const H19MapCl *mc = H19_Map()) if (mc->screensFrame) mc->screensFrame();
}
#else
void H9V_Frame() {}
#endif
