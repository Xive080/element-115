// OLA E3a: cliente COMUN de la bomba de agujero negro (Gersh Device), clientscripts/_zombiemode_weap_black_hole_bomb.csc. No es de un
// mapa: la incluyen Ascension (zombie_cosmodrome) y Moon (zombie_moon). Vive en maps/ porque build.sh compila maps/h19_*.cpp con un
// comodin; no tiene entrada en kTable. Lo llama H19_FootFxFrame (h19_mapcl.cpp) en cada frame de render.
// Reproduce, con los numeros del .csc:
//  - bandera 10 de scriptmover (black_hole_deployed, que el .csc ejecuta al poner Y al quitar la bandera: aqui solo al poner, el
//    servidor nunca la quita): en el origen del modelo, black_hole_bomb_portal + black_hole_bomb_marker_flare; al desaparecer la
//    entidad (entityshutdown), black_hole_bomb_event_horizon en ese punto (el punto se borra a los 0,2 s: sus particulas siguen).
//  - bandera 10 de actor (black_hole_zombie_being_pulled): black_hole_bomb_zombie_pull pegado al tag_origin del zombi mientras la
//    tenga; se quita al bajarla o al morir/desaparecer.
//  - vision (black_hole_visionset_think cada 0,1 s): agujero activo a menos de 512 u y bullettrace del ojo al agujero con fraccion
//    > 0,85 -> "inside_bh" (zombie_black_hole, prioridad 10, 2 s); si no -> "outside_bh". Como el original (add_to_array sobre
//    level._cosmodrome_black_hole_bombs, que no existe), la lista solo guarda el ULTIMO agujero lanzado.
//    La pila de visiones es de cada mapa: el mapa la consulta con H19_BhbInside() (Ascension lo hace; Moon puede hacer lo mismo).
// H19_BHB=0 lo apaga.
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <bgame/bg_local.h>
#include <clientscript/cscr_stringlist.h>
#include <xanim/dobj.h>
#include <EffectsCore/fx_system.h>
#include <EffectsCore/fx_unique_handle.h>
#include <qcommon/cm_trace.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include "h19_mapcl.h"

namespace {
// rutas de LoadFX de init() del .csc
const char *kPortal = "maps/zombie/fx_zmb_blackhole_looping", *kHorizon = "maps/zombie/fx_zmb_blackhole_implode",
           *kFlare = "maps/zombie/fx_zmb_blackhole_flare_marker", *kPull = "maps/zombie/fx_blackhole_zombie_breakup";
struct Bomb { int use; float org[3]; unsigned portal, flare; };
std::map<int, Bomb> s_bombs;                    // por entnum (scriptmover con la bandera 10)
struct Pull { int use; unsigned h; const FxEffectDef *def; };
std::map<int, Pull> s_pull;                     // por entnum (actor con la bandera 10)
bool s_have; float s_last[3]; bool s_lastActive;   // level._current_black_hole_bombs (solo el ultimo)
int s_visT, s_inside = -1, s_nDeployed, s_nPulled;

bool Inside()   // black_hole_vision_set del jugador local
{
    if (!s_have || !s_lastActive) return false;
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return false;
    float eye[3] = { pl->r.currentOrigin[0], pl->r.currentOrigin[1], pl->r.currentOrigin[2] + pl->client->ps.viewHeightCurrent };
    float d2 = 0; for (int k = 0; k < 3; ++k) d2 += (s_last[k] - pl->r.currentOrigin[k]) * (s_last[k] - pl->r.currentOrigin[k]);
    if (d2 >= 512.0f * 512.0f) return false;
    trace_t tr; memset(&tr, 0, sizeof tr); const float z[3] = { 0, 0, 0 };
    col_context_t ctx(1); CM_BoxTrace(&tr, eye, s_last, z, z, 1, &ctx);   // bullettrace(eye, origin, false, self): mundo
    return tr.fraction > 0.85f;
}
void PullFollow(Pull &p, gentity_s *e)   // el FX sigue al tag_origin del zombi (LinkTo del script_model de cliente)
{
#ifdef H5FX
    extern FxSystemContainer *fx_systemPool; if (!fx_systemPool || !p.h) return;
    FxEffectContainer *c = UniqueHandleToEffect(&FX_GetSystem(0)->system, p.h);
    if (!c || c->effect.def != p.def) { p.h = 0; return; }
    float m[4][3]; const unsigned t = SL_FindLowercaseString("tag_origin", SCRIPTINSTANCE_SERVER);
    if (!t || !G_DObjGetWorldTagMatrix(e, t, m)) { AnglesToAxis(e->r.currentAngles, m); memcpy(m[3], e->r.currentOrigin, 12); }
    float ax[3][3]; memcpy(ax, m, sizeof ax);
    memcpy(c->effect.frameNow.origin, m[3], 12); AxisToQuat(ax, c->effect.frameNow.quat);
#else
    (void)p; (void)e;
#endif
}
} // namespace

const FxEffectDef *__cdecl FX_Register(char *name);   // h5_fx.cpp

// 1 dentro / 0 fuera del agujero (para la pila de visiones del mapa: zombie_black_hole, prioridad 10, 2 s)
int H19_BhbInside() { return s_inside < 0 ? 0 : s_inside; }

void H19_BhbFrame()
{
    static const bool on = H19_EnvOn("H19_BHB"); if (!on) return;
    const int now = level.time;
    for (int n = 0; n < level.num_entities; ++n) {
        gentity_s *e = &g_entities[n];
        const bool live = e->r.inuse;
        // ---- scriptmover: black_hole_deployed / black_hole_fx_start / black_hole_activated
        auto b = s_bombs.find(n);
        if (b != s_bombs.end() && (!live || e->s.lerp.useCount != b->second.use || e->s.eType != ET_SCRIPTMOVER)) {   // entityshutdown
            Bomb &B = b->second;
            H19_KillFx(B.portal); H19_KillFx(B.flare);
            static const float up[3] = { 0, 0, 1 }, fw[3] = { 1, 0, 0 };
            unsigned h = H19_PlayFx(kHorizon, B.org, fw, up);   // event_horizon_spot: se borra a los 0,2 s, el efecto sigue
            (void)h;
            if (s_have && !memcmp(s_last, B.org, 12)) s_lastActive = false;   // _black_hole_active = 0
            fprintf(stderr, "[H19] agujero negro (ent %d): se cierra, event horizon en (%.0f %.0f %.0f) t=%d\n", n, B.org[0], B.org[1], B.org[2], now);
            s_bombs.erase(b); b = s_bombs.end();
        }
        if (live && e->s.eType == ET_SCRIPTMOVER && (e->s.lerp.eFlags2 & (1u << 10)) && b == s_bombs.end()) {
            Bomb B; B.use = e->s.lerp.useCount; memcpy(B.org, e->r.currentOrigin, 12);
            static const float up[3] = { 0, 0, 1 }, fw[3] = { 1, 0, 0 };   // Spawn script_model tag_origin sin angulos + PlayFXOnTag
            B.portal = H19_PlayFx(kPortal, B.org, fw, up); B.flare = H19_PlayFx(kFlare, B.org, fw, up);
            s_bombs[n] = B; memcpy(s_last, B.org, 12); s_have = true; s_lastActive = true; ++s_nDeployed;
            fprintf(stderr, "[H19] agujero negro (ent %d) abierto en (%.0f %.0f %.0f): portal %s, destello %s t=%d\n", n, B.org[0], B.org[1], B.org[2],
                B.portal ? "si" : "NO", B.flare ? "si" : "NO", now);
        }
        // ---- actor: black_hole_zombie_being_pulled
        auto p = s_pull.find(n);
        const bool pulled = live && e->s.eType == ET_ACTOR && (e->s.lerp.eFlags2 & (1u << 10));
        if (p != s_pull.end() && (!pulled || e->s.lerp.useCount != p->second.use)) { H19_KillFx(p->second.h); s_pull.erase(p); p = s_pull.end(); }
        if (pulled && p == s_pull.end()) {
            Pull P; P.use = e->s.lerp.useCount; P.def = nullptr;
#ifdef H5FX
            P.def = FX_Register((char *)kPull);
#endif
            float ax[3][3]; AnglesToAxis(e->r.currentAngles, ax);
            P.h = H19_PlayFx(kPull, e->r.currentOrigin, ax[0], ax[2]);
            s_pull[n] = P; p = s_pull.find(n);
            if (++s_nPulled == 1 || s_nPulled % 20 == 0) fprintf(stderr, "[H19] agujero negro: %d zombis atraidos (FX %s) t=%d\n", s_nPulled, P.h ? "si" : "NO", now);
        }
        if (p != s_pull.end()) PullFollow(p->second, e);
    }
    // ---- black_hole_visionset_think: cada 0,1 s
    if (now - s_visT >= 100 || now < s_visT) {
        s_visT = now;
        const int in = Inside() ? 1 : 0;
        if (in != s_inside) { if (s_inside >= 0 || in) fprintf(stderr, "[H19] agujero negro: vision %s t=%d\n", in ? "inside_bh (zombie_black_hole)" : "outside_bh", now); s_inside = in; }
    }
}
#endif
