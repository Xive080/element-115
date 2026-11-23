// OLA F2 (visual comun): cliente COMUN de armas, para todos los mapas. No hay VM de CSC: lo que hacen estos clientscripts y el
// CG_Missile del cliente se reproduce aqui con sus numeros. Vive en maps/ porque build.sh compila maps/h19_*.cpp con un comodin; no
// tiene entrada en kTable. Lo llama H7E_ClientFrame (h7e_light.cpp) en cada frame de render, antes de preparar los modelos de cliente.
//
// 1) Caja misteriosa: _zombiemode_weapons.csc weapon_box_callback (bandera 15 de scriptmover, _ZOMBIE_SCRIPTMOVER_FLAG_BOX_RANDOM, que
//    el GSC pone en chest.chest_box durante treasure_chest_weapon_spawn). weapon_floats_up: un script_model de cliente en el origen
//    de la caja con angles + (0,180,0), modelo GetWeaponModel(rand) con useweaponhidetags, moveto(origen + (0,0,64), 3, 2, 0.9) y 39
//    cambios de modelo (wait 0,05 x20, 0,1 x10, 0,2 x5, 0,3 x3; el ultimo sin espera); el .dw (segundo modelo a -(3,3,3)) solo se ve
//    con las armas dobles (weapon_is_dual_wield). Al acabar o al quitar la bandera: cleanup_weapon_models. rand sale de
//    level._display_box_weapons: las llamadas include_weapon("x") sin false de clientscripts/<zona>.csc (si no hay ninguna, el
//    array por defecto del .csc: python, g11_lps, famas). El arma final es del servidor (script_model, h20_svmodels).
// 2) CG_Missile (cg_ents_mp.cpp): cada ET_MISSILE sin EF_NODRAW y ya nacido dibuja su projectileModel (R_AddDObjToScene con la pose
//    de la entidad) y lanza weapDef->projTrailEffect en tag_fx (CG_PlayBoltedEffect) la primera vez; el efecto sigue a la etiqueta y
//    se suelta (FX_ThroughWithEffect) al desaparecer el misil. Sin DObj de cliente: la etiqueta sale de la pose base del modelo.
//    Mas los entityspawned de misil de _callbacks.csc: _explosive_bolt.csc (ballesta: luz verde fx_trail_crossbow_blink_grn_os en
//    tag_origin cada 0,3 s / 1,2 hasta 0,1 s; la mejorada, roja una vez) y _sticky_grenade.csc (semtex: igual en tag_fx).
// 3) FX de vista de armas maravilla (PlayViewmodelFx = CG_PlayBoltedEffect en el DObj del viewmodel):
//    _zombiemode_weap_tesla.csc (tesla_fx_tube cada 0,1 s en tag_brass segun la municion; tesla_fx_rail cada 8-12 s en tag_flash) y
//    _zombiemode_weap_thundergun.csc (celdas tag_bulb1..3 segun la municion, cada 3 s; vapor en tag_steam1..3 al disparar).
//    Los notetracks (pullout/putaway/idle) se aproximan con el weaponstate: apagado mientras se guarda el arma.
// No se hace aqui: sonidos de estos scripts (wpn_crossbow_alert, wpn_semtex_alert, wpn_tesla_effects, wpn_thunder_breath: son del
// agente de sonido), ballistic angles (SetBallisticAngles) y armas recuperables (worldModel[2]).
// Variables: H19_WEAPCL=0 apaga todo; H19_BOXCL=0 la caja; H19_MISSILE=0 los misiles; H19_VMFX=0 los FX de vista.
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <universal/dvar.h>
#include <database/db_registry.h>
#include <xanim/xmodel.h>
#include <xanim/xmodel_utils.h>
#include <xanim/dobj.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <bgame/bg_local.h>
#include <bgame/bg_weapons.h>
#include <bgame/bg_weapons_def.h>
#include <bgame/bg_mantle.h>
#include <clientscript/cscr_stringlist.h>
#include <EffectsCore/fx_system.h>
#include <EffectsCore/fx_unique_handle.h>
#include <qcommon/dobj_management.h>
#include <cgame/cg_event.h>
#include <cgame/cg_weapons.h>
#include <zlib.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "h19_mapcl.h"

const FxEffectDef *__cdecl FX_Register(char *name);   // h5_fx.cpp
// h7e_light.cpp (modelos de cliente): se declaran aqui y no en h19_mapcl.h (que recompila todo)
void H19_ModelPose(int j, const float *org, const float *ang);
void H19_ModelHide(int j, bool hide);
void H19_ModelHideParts(int j, const unsigned *bits);

namespace {
int NowMs()   // tiempo del frame de render (el de los FX): los modelos de cliente se mueven suaves, no a saltos de 50 ms del servidor
{
#ifdef H5FX
    extern FxSystemContainer *fx_systemPool;
    if (fx_systemPool) { const int t = FX_GetSystem(0)->system.msecNow; if (t > 0) return t; }
#endif
    return level.time;
}

// ---- utilidades de armas
const XModel *WorldModel(int w)   // GetWeaponModel(name): worldModel[0]
{
    if (w <= 0 || (unsigned)w >= BG_GetNumWeapons()) return nullptr;
    const WeaponDef *d = BG_GetWeaponDef(w); if (!d || !d->worldModel.get()) return nullptr;
    return d->worldModel.get()[0].get();
}
void HideBits(int w, const XModel *m, unsigned *bits)   // CScr_UseWeaponHideTags: bit 0x80000000 >> (hueso & 31) en la palabra hueso >> 5
{
    memset(bits, 0, 5 * sizeof(unsigned));
    if (w <= 0 || !m) return;
    const WeaponVariantDef *v = BG_GetWeaponVariantDef(w); const unsigned short *ht = v ? v->hideTags.get() : nullptr; if (!ht) return;
    for (int i = 0; i < 32 && ht[i]; ++i) { unsigned char b = 0xFE;
        if (XModelGetBoneIndex(m, ht[i], 0, &b) && b < 160) bits[b >> 5] |= 0x80000000u >> (b & 31); }
}
const char *WeapName(int w) { const WeaponVariantDef *v = w > 0 ? BG_GetWeaponVariantDef(w) : nullptr; return v && v->szInternalName.get() ? v->szInternalName.get() : ""; }
int WeapIndex(const char *n) { return BG_GetWeaponIndexForName(n); }

std::string RawText(const char *name)
{
    RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)name, 0, -1).rawfile;
    if (!rf || !rf->buffer.get()) return {};
    const unsigned *hd = (const unsigned *)rf->buffer.get(); uLongf len = hd[0];
    std::vector<unsigned char> out(len + 1);
    if (uncompress(out.data(), &len, (const unsigned char *)(hd + 2), hd[1]) != Z_OK) return {};
    return std::string((const char *)out.data(), len);
}

// ================================================================================================ 1) caja: weapon_box_callback
std::vector<int> s_boxWeapons;   // level._display_box_weapons (indices de arma)
bool s_boxRead;
void ReadBoxWeapons()
{
    s_boxRead = true;
    const char *mn = Dvar_GetString("mapname"); if (!mn || !*mn) { s_boxRead = false; return; }
    char nm[160]; snprintf(nm, sizeof nm, "clientscripts/%s.csc", mn);
    const std::string raw = RawText(nm);
    std::string t; t.reserve(raw.size());   // fuera comentarios // y /* */ y bloques de desarrollo /# #/
    for (size_t i = 0; i < raw.size();) {
        if (raw.compare(i, 2, "//") == 0) { while (i < raw.size() && raw[i] != '\n') ++i; continue; }
        if (raw.compare(i, 2, "/*") == 0) { size_t e = raw.find("*/", i + 2); i = e == std::string::npos ? raw.size() : e + 2; continue; }
        if (raw.compare(i, 2, "/#") == 0) { size_t e = raw.find("#/", i + 2); i = e == std::string::npos ? raw.size() : e + 2; continue; }
        t += raw[i++];
    }
    int nInc = 0, nMiss = 0;
    for (size_t p = 0; (p = t.find("include_weapon", p)) != std::string::npos; ) {
        p += 14; if (p < t.size() && (isalnum((unsigned char)t[p]) || t[p] == '_')) continue;   // include_weapons() no
        const size_t par = t.find_first_not_of(" \t", p); if (par == std::string::npos || t[par] != '(') continue;
        const size_t cl = t.find(')', par); if (cl == std::string::npos) break;
        const std::string args = t.substr(par + 1, cl - par - 1);
        const size_t a = args.find('"'), b = a == std::string::npos ? a : args.find('"', a + 1); if (b == std::string::npos) continue;
        const std::string w = args.substr(a + 1, b - a - 1), rest = args.substr(b + 1);
        ++nInc;
        if (rest.find("false") != std::string::npos) continue;   // include_weapon(w, false): no sale en la caja
        const int idx = WeapIndex(w.c_str());
        if (idx > 0 && WorldModel(idx)) s_boxWeapons.push_back(idx); else ++nMiss;
    }
    if (s_boxWeapons.empty()) for (const char *w : { "python_zm", "g11_lps_zm", "famas_zm" }) { const int i = WeapIndex(w); if (i > 0 && WorldModel(i)) s_boxWeapons.push_back(i); }
    fprintf(stderr, "[H19W] caja: %zu armas para el giro (de %d include_weapon en %s; %d sin modelo)\n", s_boxWeapons.size(), nInc, nm, nMiss);
}
int BoxRandom() { return s_boxWeapons.empty() ? 0 : s_boxWeapons[(int)(H19_Rand01() * s_boxWeapons.size()) % s_boxWeapons.size()]; }
bool DualWield(const char *n)
{
    static const char *k[] = { "cz75dw_zm", "cz75dw_upgraded_zm", "m1911_upgraded_zm", "hs10_upgraded_zm", "pm63_upgraded_zm", "microwavegundw_zm", "microwavegundw_upgraded_zm" };
    for (const char *s : k) if (!strcmp(n, s)) return true;
    return false;
}
const XModel *LeftHandModel(int w)   // get_left_hand_weapon_model_name
{
    const char *n = WeapName(w);
    if (!strcmp(n, "microwavegundw_zm")) return WorldModel(WeapIndex("microwavegunlh_zm"));
    if (!strcmp(n, "microwavegundw_upgraded_zm")) return WorldModel(WeapIndex("microwavegundwlh_upgraded_zm"));
    return WorldModel(w);
}
// moveto(dest, 3, 2, 0.9) del CSC (perfil trapezoidal de G_RunMover / ScriptMover: aceleracion, crucero, frenada)
float MoveFrac(float t, float T, float ta, float td)
{
    if (t <= 0) return 0; if (t >= T) return 1;
    const float v = 1.0f / (T - 0.5f * ta - 0.5f * td);
    if (t < ta) return 0.5f * v / ta * t * t;
    if (t < T - td) return 0.5f * v * ta + v * (t - ta);
    const float r = T - t; return 1.0f - 0.5f * v / td * r * r;
}
struct Box { int use = -1; bool on = false, started = false; int t0 = 0, cyc = 0, next = 0; float org[3], ang[3]; int m = -1, dw = -1; };
std::map<int, Box> s_box;   // por entnum del chest_box
int s_boxStarts, s_boxCycles;
float s_boxLast[3]; bool s_boxHave;   // ultima posicion del arma (para el log)

void BoxSetModel(Box &b, int w)
{
    const XModel *xm = WorldModel(w); if (!xm || b.m < 0) return;
    unsigned bits[5];
    H19_SetModel(b.m, XModelGetName(xm)); HideBits(w, xm, bits); H19_ModelHideParts(b.m, bits);
    if (DualWield(WeapName(w))) { const XModel *lh = LeftHandModel(w);
        if (lh && b.dw >= 0) { H19_SetModel(b.dw, XModelGetName(lh)); HideBits(w, lh, bits); H19_ModelHideParts(b.dw, bits); H19_ModelHide(b.dw, false); } }
    else if (b.dw >= 0) H19_ModelHide(b.dw, true);
}
void BoxCleanup(Box &b) { b.on = false; if (b.m >= 0) H19_ModelHide(b.m, true); if (b.dw >= 0) H19_ModelHide(b.dw, true); }
void BoxStart(Box &b, const gentity_s *e, int now)   // weapon_floats_up
{
    BoxCleanup(b);
    if (!s_boxRead) ReadBoxWeapons();
    const int w = BoxRandom(); const XModel *xm = WorldModel(w); if (!xm) return;
    memcpy(b.org, e->r.currentOrigin, 12);
    b.ang[0] = e->r.currentAngles[0]; b.ang[1] = e->r.currentAngles[1] + 180.0f; b.ang[2] = e->r.currentAngles[2];
    if (b.m < 0) b.m = H19_AddModel(XModelGetName(xm), b.org, b.ang, "box_weapon");
    if (b.dw < 0) { const float o[3] = { b.org[0] - 3, b.org[1] - 3, b.org[2] - 3 }; b.dw = H19_AddModel(XModelGetName(xm), o, b.ang, "box_weapon_dw"); }
    if (b.m < 0) return;
    H19_ModelHide(b.m, false); if (b.dw >= 0) H19_ModelHide(b.dw, true);   // .dw Hide() al nacer
    { unsigned bits[5]; H19_SetModel(b.m, XModelGetName(xm)); HideBits(w, xm, bits); H19_ModelHideParts(b.m, bits); }
    b.on = true; b.t0 = now; b.cyc = 0; b.next = now + 50;
    ++s_boxStarts;
    fprintf(stderr, "[H19W] caja (ent %d): el arma sube y gira en (%.0f %.0f %.0f) yaw %.0f, primera %s t=%d\n", e->s.number, b.org[0], b.org[1], b.org[2], b.ang[1], WeapName(w), level.time);
}
void BoxFrame(int now)
{
    static const bool on = H19_EnvOn("H19_BOXCL"); if (!on) return;
    for (int n = 0; n < level.num_entities; ++n) {
        gentity_s *e = &g_entities[n];
        const bool flag = e->r.inuse && e->s.eType == ET_SCRIPTMOVER && (e->s.lerp.eFlags2 & (1u << 15));
        auto it = s_box.find(n);
        if (it == s_box.end()) { if (!flag) continue; it = s_box.emplace(n, Box()).first; }
        Box &b = it->second;
        if (!flag) { if (b.on) { BoxCleanup(b); fprintf(stderr, "[H19W] caja (ent %d): bandera quitada, cleanup (ciclos %d) t=%d\n", n, b.cyc, level.time); } b.started = false; continue; }
        if (!b.started || b.use != e->s.lerp.useCount) { b.started = true; b.use = e->s.lerp.useCount; BoxStart(b, e, now); }   // flanco de subida: weapon_floats_up
        if (!b.on) continue;   // ya termino sus 39 ciclos: espera a que el servidor quite la bandera
        // movimiento: moveto(origin + (0,0,64), 3, 2, 0.9); el .dw igual con -(3,3,3)
        const float f = MoveFrac((now - b.t0) / 1000.0f, 3.0f, 2.0f, 0.9f);
        float o[3] = { b.org[0], b.org[1], b.org[2] + 64.0f * f };
        H19_ModelPose(b.m, o, b.ang); memcpy(s_boxLast, o, 12); s_boxHave = true;
        if (b.dw >= 0) { const float d[3] = { o[0] - 3, o[1] - 3, o[2] - 3 }; H19_ModelPose(b.dw, d, b.ang); }
        // ciclos: i < 20 -> 0,05; < 30 -> 0,1; < 35 -> 0,2; < 38 -> 0,3; el 38 sin espera
        while (b.on && now >= b.next) {
            BoxSetModel(b, BoxRandom()); ++s_boxCycles; ++b.cyc;
            if (b.cyc >= 39) { BoxCleanup(b); fprintf(stderr, "[H19W] caja (ent %d): 39 cambios hechos, cleanup t=%d\n", n, level.time); b.cyc = 39; break; }
            const int i = b.cyc; b.next += i < 20 ? 50 : i < 30 ? 100 : i < 35 ? 200 : i < 38 ? 300 : 0;
        }
    }
}

// ================================================================================================ 2) CG_Missile
struct Missile { int use = -1, m = -1; unsigned trail = 0; const FxEffectDef *trailDef = nullptr; int w = 0; bool tagOk = false; float tagOfs[3] = { 0, 0, 0 };
                 int blinkNext = -1, blinkMs = 300; const FxEffectDef *blink = nullptr; bool blinkLoop = false; unsigned blinkH = 0; int seen = 0; };
std::map<int, Missile> s_mis;
std::vector<int> s_freeModels;   // modelos de cliente ya creados y libres (se reutilizan: el arnes no borra script_model de cliente)
int s_nMis, s_nTrail, s_nModel, s_nBlink;

void TagWorld(const Missile &M, const gentity_s *e, float *org, float ax[3][3])
{
    AnglesToAxis(e->r.currentAngles, ax);
    for (int k = 0; k < 3; ++k) org[k] = e->r.currentOrigin[k] + M.tagOfs[0] * ax[0][k] + M.tagOfs[1] * ax[1][k] + M.tagOfs[2] * ax[2][k];
}
void FollowFx(unsigned &h, const FxEffectDef *def, const float *org, float ax[3][3])   // el FX atornillado sigue a la etiqueta
{
#ifdef H5FX
    extern FxSystemContainer *fx_systemPool; if (!fx_systemPool || !h) return;
    FxEffectContainer *c = UniqueHandleToEffect(&FX_GetSystem(0)->system, h);
    if (!c || c->effect.def != def) { h = 0; return; }
    memcpy(c->effect.frameNow.origin, org, 12); AxisToQuat(ax, c->effect.frameNow.quat);
#else
    (void)h; (void)def; (void)org; (void)ax;
#endif
}
unsigned PlayDef(const FxEffectDef *def, const float *org, float ax[3][3])
{
#ifdef H5FX
    if (!def) return 0; return FX_PlayOrientedEffect(0, def, FX_GetSystem(0)->system.msecNow, org, ax);
#else
    (void)def; (void)org; (void)ax; return 0;
#endif
}
void MissileGone(Missile &M)
{
    if (M.trail) { H19_KillFx(M.trail); }
    if (M.blinkH) { H19_KillFx(M.blinkH); }
    if (M.m >= 0) { H19_ModelHide(M.m, true); s_freeModels.push_back(M.m); M.m = -1; }
}
void MissileStart(Missile &M, const gentity_s *e, int now)
{
    M.w = e->s.weapon; const WeaponDef *wd = BG_GetWeaponDef(M.w);
    const XModel *pm = wd ? wd->projectileModel.get() : nullptr;
    if (pm) {   // tag_fx en la pose base del modelo del proyectil
        const unsigned t = SL_FindLowercaseString("tag_fx", SCRIPTINSTANCE_SERVER); unsigned char b = 0xFE;
        if (t && XModelGetBoneIndex(pm, t, 0, &b) && b != 0xFE) { const DObjAnimMat *bp = XModelGetBasePose(pm); if (bp) { memcpy(M.tagOfs, bp[b].trans, 12); M.tagOk = true; } }
        if (!s_freeModels.empty()) { M.m = s_freeModels.back(); s_freeModels.pop_back(); H19_SetModel(M.m, XModelGetName(pm)); }
        else M.m = H19_AddModel(XModelGetName(pm), e->r.currentOrigin, e->r.currentAngles, "missile");
        if (M.m >= 0) { H19_ModelPose(M.m, e->r.currentOrigin, e->r.currentAngles); H19_ModelHideParts(M.m, nullptr); H19_ModelHide(M.m, false); ++s_nModel; }
    }
    float org[3], ax[3][3]; TagWorld(M, e, org, ax);
    M.trailDef = wd ? wd->projTrailEffect.get() : nullptr;
    if (M.trailDef) { M.trail = PlayDef(M.trailDef, org, ax); if (M.trail) ++s_nTrail; }
    // entityspawned de misil (_callbacks.csc): ballesta (_explosive_bolt.csc) y semtex (_sticky_grenade.csc)
    const char *n = WeapName(M.w);
    if (!strcmp(n, "crossbow_explosive_zm") || !strcmp(n, "explosive_bolt_zm") || !strcmp(n, "explosive_bolt_upgraded_zm")) {
        static const FxEffectDef *grn = FX_Register((char *)"weapon/crossbow/fx_trail_crossbow_blink_grn_os"), *red = FX_Register((char *)"weapon/crossbow/fx_trail_crossbow_blink_red_os");
        M.blink = strstr(n, "upgraded") ? red : grn; M.blinkLoop = true; M.blinkNext = now; M.tagOk = false; memset(M.tagOfs, 0, 12);   // fxTagName = tag_origin
    } else if (!strcmp(n, "crossbow_explosive_upgraded_zm")) {   // spawned(lc, false, true): PlayFXOnTag rojo una vez
        static const FxEffectDef *red = FX_Register((char *)"weapon/crossbow/fx_trail_crossbow_blink_red_os");
        M.blink = red; M.blinkLoop = false; M.blinkNext = now; M.tagOk = false; memset(M.tagOfs, 0, 12);
    } else if (!strcmp(n, "sticky_grenade_zm")) {   // jugador del mismo equipo: luz verde en tag_fx
        static const FxEffectDef *grn = FX_Register((char *)"weapon/crossbow/fx_trail_crossbow_blink_grn_os");
        M.blink = grn; M.blinkLoop = true; M.blinkNext = now;
    }
    ++s_nMis;
    if (s_nMis <= 12 || s_nMis % 50 == 0)
        fprintf(stderr, "[H19W] misil %d (ent %d, %s): modelo %s, estela %s%s%s t=%d\n", s_nMis, e->s.number, n, pm ? XModelGetName(pm) : "-",
            M.trailDef ? M.trailDef->name.get() : "-", M.trail ? "" : (M.trailDef ? " (NO)" : ""), M.blink ? ", luz intermitente" : "", level.time);
}
void MissileFrame(int now)
{
    static const bool on = H19_EnvOn("H19_MISSILE"); if (!on) return;
    static int frame; ++frame;
    for (int n = 0; n < level.num_entities; ++n) {
        gentity_s *e = &g_entities[n];
        const bool live = e->r.inuse && e->s.eType == ET_MISSILE && !(e->s.lerp.eFlags & 0x20) && e->s.lerp.u.actor.actorNum <= level.time
                          && e->s.weapon > 0 && e->s.weapon < BG_GetNumWeapons();
        auto it = s_mis.find(n);
        if (it != s_mis.end() && (!live || it->second.use != e->s.lerp.useCount || it->second.w != (int)e->s.weapon)) { MissileGone(it->second); s_mis.erase(it); it = s_mis.end(); }
        if (!live) continue;
        if (it == s_mis.end()) { it = s_mis.emplace(n, Missile()).first; it->second.use = e->s.lerp.useCount; MissileStart(it->second, e, now); }
        Missile &M = it->second; M.seen = frame;
        if (M.m >= 0) H19_ModelPose(M.m, e->r.currentOrigin, e->r.currentAngles);
        float org[3], ax[3][3]; TagWorld(M, e, org, ax);
        FollowFx(M.trail, M.trailDef, org, ax);
        if (M.blink && M.blinkNext >= 0 && now >= M.blinkNext) {   // loop_local_sound: PlayFXOnTag, wait(interval), interval /= 1.2 (min 0,1)
            M.blinkH = PlayDef(M.blink, org, ax); ++s_nBlink;
            if (!M.blinkLoop) M.blinkNext = -1;
            else { M.blinkNext = now + M.blinkMs; M.blinkMs = (int)(M.blinkMs / 1.2f); if (M.blinkMs < 100) M.blinkMs = 100; }
        }
        if (M.blinkH) FollowFx(M.blinkH, M.blink, org, ax);
    }
}

// ================================================================================================ 3) FX de vista (PlayViewmodelFx)
unsigned VmFx(const char *name, const char *tag)
{
#ifdef H5FX
    const FxEffectDef *fx = FX_Register((char *)name); if (!fx) return 0;
    const int h = CG_WeaponDObjHandle(0); if (!Com_GetClientDObj(h, 0)) return 0;
    const unsigned t = SL_FindLowercaseString(tag, SCRIPTINSTANCE_SERVER); if (!t) return 0;
    return CG_PlayBoltedEffect(0, fx, h, t);
#else
    (void)name; (void)tag; return 0;
#endif
}
bool Busy(const playerState_s *ps)   // IsThrowingGrenade || IsMeleeing (IsOnTurret: no hay torretas en zombis)
{
    const int s = ps->weaponstate;
    return (s >= WEAPON_MELEE_INIT && s <= WEAPON_MELEE_END) || (s >= WEAPON_OFFHAND_INIT && s <= WEAPON_OFFHAND_END);
}
bool PutAway(const playerState_s *ps) { const int s = ps->weaponstate; return s == WEAPON_DROPPING || s == WEAPON_DROPPING_QUICK || s == WEAPON_DROPPING_ALTSWITCH; }
int s_teslaTube = 0, s_teslaRail = -1, s_tgPoll = 0, s_tgOldAmmo = -1, s_tgOldCount = -1, s_tgCellsAt = -1, s_tgCount = 0;
unsigned s_tgCell[3];
int s_nTube, s_nRail, s_nCells, s_nSteam;
void TgKillCells() { for (unsigned &h : s_tgCell) H19_KillFx(h); }
void VmFrame(int now)
{
    static const bool on = H19_EnvOn("H19_VMFX"); if (!on) return;
    gentity_s *pl = &g_entities[0]; if (!pl->r.inuse || !pl->client) return;
    const playerState_s *ps = &pl->client->ps; const int w = ps->weapon; const char *n = WeapName(w);
    const bool tesla = !strcmp(n, "tesla_gun_zm") || !strcmp(n, "tesla_gun_upgraded_zm"), up = strstr(n, "upgraded") != nullptr;
    const int ammo = w > 0 ? BG_GetAmmoInClip(ps, w) : 0;
    // ---- tesla_fx_tube (realwait 0,1) y tesla_fx_rail (realwait 8-12 s)
    if (tesla && now >= s_teslaTube) {
        s_teslaTube = now + 100;
        if (!PutAway(ps) && !Busy(ps) && ammo > 0) {
            const char *fx = up ? (ammo == 3 || ammo == 4 ? "maps/zombie/fx_zombie_tesla_tube_view2_ug" : ammo == 1 || ammo == 2 ? "maps/zombie/fx_zombie_tesla_tube_view3_ug" : "maps/zombie/fx_zombie_tesla_tube_view_ug")
                                : (ammo == 1 ? "maps/zombie/fx_zombie_tesla_tube_view3" : ammo == 2 ? "maps/zombie/fx_zombie_tesla_tube_view2" : "maps/zombie/fx_zombie_tesla_tube_view");
            if (VmFx(fx, "tag_brass") && (++s_nTube == 1 || s_nTube % 300 == 0)) fprintf(stderr, "[H19W] tesla: tubo %s (%d) municion %d t=%d\n", fx, s_nTube, ammo, level.time);
        }
    }
    if (s_teslaRail < 0) s_teslaRail = now + 8000 + (int)(H19_Rand01() * 4000);
    if (now >= s_teslaRail) {
        s_teslaRail = now + 8000 + (int)(H19_Rand01() * 4000);
        if (tesla && !PutAway(ps) && !Busy(ps) && ps->fWeaponPosFrac <= 0.0f && ammo > 0) {
            const char *fx = up ? "maps/zombie/fx_zombie_tesla_rail_view_ug" : "maps/zombie/fx_zombie_tesla_rail_view";
            if (VmFx(fx, "tag_flash")) { ++s_nRail; fprintf(stderr, "[H19W] tesla: rail %s (%d) t=%d\n", fx, s_nRail, level.time); }
        }
    }
    // ---- thundergun_fx_power_cell (realwait 0,1): celdas segun la municion (cada 3 s) y vapor al cambiar la municion (disparo)
    const bool tg = !strcmp(n, "thundergun_zm") || !strcmp(n, "thundergun_upgraded_zm");
    if (now >= s_tgPoll) {
        s_tgPoll = now + 100;
        if (!tg || PutAway(ps) || Busy(ps)) {
            if (s_tgOldAmmo != -1) { TgKillCells(); s_tgCount = 0; s_tgCellsAt = -1; }
            s_tgOldAmmo = -1; s_tgOldCount = -1;
        } else {
            if (s_tgOldAmmo > 0 && s_tgOldAmmo != ammo) {   // thundergun_fx_fire
                for (int i = 3; i > 0; --i) { char t[16]; snprintf(t, sizeof t, "tag_steam%d", i); VmFx("weapon/thunder_gun/fx_thundergun_steam_view", t); }
                ++s_nSteam; fprintf(stderr, "[H19W] thundergun: vapor (%d) municion %d -> %d t=%d\n", s_nSteam, s_tgOldAmmo, ammo, level.time);
            }
            s_tgOldAmmo = ammo;
            const int cnt = ammo > 3 ? 3 : ammo;
            if (s_tgOldCount == -1 || s_tgOldCount != cnt) { TgKillCells(); s_tgCount = cnt; s_tgCellsAt = cnt ? now : -1; }
            s_tgOldCount = cnt;
        }
    }
    if (tg && s_tgCellsAt >= 0 && now >= s_tgCellsAt) {   // thundergun_play_power_cell_fx: cada 3 s
        s_tgCellsAt = now + 3000;
        for (int i = s_tgCount; i > 0; --i) { char t[16], f[80]; snprintf(t, sizeof t, "tag_bulb%d", i); snprintf(f, sizeof f, "weapon/thunder_gun/fx_thundergun_power_cell_view%d", i);
            s_tgCell[i - 1] = VmFx(f, t); }
        if (++s_nCells == 1 || s_nCells % 20 == 0) fprintf(stderr, "[H19W] thundergun: %d celdas encendidas (%d) t=%d\n", s_tgCount, s_nCells, level.time);
    }
}
} // namespace

void H19_WeapClFrame()
{
    static const bool on = H19_EnvOn("H19_WEAPCL"); if (!on) return;
    const int now = NowMs();
    BoxFrame(now);
    MissileFrame(now);
    VmFrame(now);
}
#endif
