// OLA M2: tabla de clientes de mapa (h19_mapcl.h) y ayudantes comunes que usan los ficheros maps/h19_<zona>.cpp.
// Anadir un mapa = un fichero maps/h19_<zona>.cpp (copiado de la plantilla) + UNA linea en kTable. Los diez mapas del catalogo
// (maps/maps_catalog.inc) ya tienen su linea y su fichero (los que aun no tienen cliente propio, con todos los ganchos a nullptr).
#ifdef H5G
#include <database/db_registry.h>
#include <database/db_assetnames.h>
#include <universal/ptr32.h>
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <qcommon/cm_load_obj.h>
#include <EffectsCore/fx_system.h>
#include <string.h>
#include "h19_mapcl.h"

const FxEffectDef *__cdecl FX_Register(char *name);   // h5_fx.cpp (fx_load_obj.cpp extraida)

extern const H19MapCl kH19_zombie_pentagon, kH19_zombie_theater, kH19_zombie_cod5_prototype, kH19_zombie_cod5_asylum, kH19_zombie_cod5_sumpf,
    kH19_zombie_cod5_factory, kH19_zombie_cosmodrome, kH19_zombie_coast, kH19_zombie_temple, kH19_zombie_moon;

// ---- la tabla: una entrada por mapa (el orden no importa; el mapname decide)
static const H19MapCl *const kTable[] = {
    &kH19_zombie_pentagon,         // Five (OLA 7e/9/14)
    &kH19_zombie_theater,          // Kino der Toten (OLA M1)
    &kH19_zombie_cod5_prototype,   // Nacht der Untoten
    &kH19_zombie_cod5_asylum,      // Verruckt
    &kH19_zombie_cod5_sumpf,       // Shi No Numa
    &kH19_zombie_cod5_factory,     // Der Riese
    &kH19_zombie_cosmodrome,       // Ascension
    &kH19_zombie_coast,            // Call of the Dead
    &kH19_zombie_temple,           // Shangri-La
    &kH19_zombie_moon,             // Moon
};

const H19MapCl *H19_Map()
{
    static bool done; static const H19MapCl *cur;
    if (done) return cur;
    const char *mn = Dvar_GetString("mapname"); if (!mn || !*mn) return nullptr;   // aun sin mapa: se vuelve a mirar en la siguiente llamada
    done = true;
    for (const H19MapCl *m : kTable) if (m && m->zone && !strcmp(m->zone, mn)) cur = m;
    const bool off = !H19_EnvOn("H19_MAPCL") || (cur && cur->offEnv && !H19_EnvOn(cur->offEnv));
    fprintf(stderr, "[H19] cliente de mapa '%s': %s%s%s\n", mn, !cur ? "sin entrada (generico)" : off ? "APAGADO por variable (generico)" : "registrado",
        cur && !off && cur->estado ? " - " : "", cur && !off && cur->estado ? cur->estado : "");
    if (off) cur = nullptr;
    return cur;
}

// ---- entidades del mapa
std::vector<H19Kv> H19_ParseEnts(const char *s, int n)
{
    std::vector<H19Kv> out; H19Kv cur; bool in = false; int i = 0;
    auto str = [&](std::string &o) { o.clear(); ++i; while (i < n && s[i] != '"') o += s[i++]; ++i; };
    while (i < n) {
        const char c = s[i];
        if (c == '{') { cur.clear(); in = true; ++i; }
        else if (c == '}') { if (in) out.push_back(cur); in = false; ++i; }
        else if (c == '"' && in) { std::string k, v; str(k); while (i < n && s[i] != '"') ++i; if (i >= n) break; str(v); cur[k] = v; }
        else ++i;
    }
    return out;
}
const std::vector<H19Kv> &H19_MapEnts()
{
    static std::vector<H19Kv> ents; static bool done;
    if (done) return ents;
    MapEnts *me = nullptr; DB_EnumXAssets_FastFile(ASSET_TYPE_MAP_ENTS, [](XAssetHeader h, void *c) { if (!*(MapEnts **)c) *(MapEnts **)c = h.mapEnts; }, &me, false);
    if (!me) return ents;   // sin MapEnts todavia: vacio, se reintenta
    done = true; ents = H19_ParseEnts(me->entityString.get(), me->numEntityChars);
    return ents;
}
bool H19_Vec(const H19Kv &k, const char *key, float *v) { auto it = k.find(key); return it != k.end() && sscanf(it->second.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3; }
const char *H19_Str(const H19Kv &k, const char *key) { auto it = k.find(key); return it == k.end() ? nullptr : it->second.c_str(); }

// ---- efectos (lo que era KFx/KPlay/KKill de Kino en h7e_light.cpp, OLA M1)
static const FxEffectDef *Fx(const char *name)
{
#ifdef H5FX
    static std::map<std::string, const FxEffectDef *> c; auto it = c.find(name); if (it != c.end()) return it->second;
    const FxEffectDef *fx = FX_Register((char *)name); c[name] = fx;
    fprintf(stderr, "[H19] fx '%s' %s\n", name, fx ? "ok" : "NO esta");
    return fx;
#else
    (void)name; return nullptr;
#endif
}
unsigned H19_PlayFx(const char *name, const float *org, const float *fwd, const float *up)
{
#ifdef H5FX
    const FxEffectDef *fx = Fx(name); if (!fx) return 0;
    float ax[3][3]; memcpy(ax[0], fwd, 12); memcpy(ax[2], up, 12);
    ax[1][0] = up[1] * fwd[2] - up[2] * fwd[1]; ax[1][1] = up[2] * fwd[0] - up[0] * fwd[2]; ax[1][2] = up[0] * fwd[1] - up[1] * fwd[0];
    return FX_PlayOrientedEffect(0, fx, FX_GetSystem(0)->system.msecNow, org, ax);
#else
    (void)name; (void)org; (void)fwd; (void)up; (void)Fx; return 0;
#endif
}
unsigned H19_PlayFxAng(const char *name, const float *org, const float *ang)
{
    float f[3], r[3], u[3]; AngleVectors(ang, f, r, u); return H19_PlayFx(name, org, f, u);
}
void H19_KillFx(unsigned &h)
{
#ifdef H5FX
    if (h) FX_ThroughWithEffect(0, h, false);
#endif
    h = 0;
}

// ---- OLA E1b: efecto de los pasos de la IA (_footsteps.csc do_foot_effect), comun a los mapas que llaman setFootstepEffect
// En el original playAIFootstep (CG_SP_PlayAIFootstep -> script) acaba en do_foot_effect: si el tipo de suelo esta en
// level._optionalStepEffects juega level._effect["step_" + suelo] con playfx(cl, fx, pos, pos + (0,0,100)) (4 parametros:
// CScr_PlayFX normaliza ese "forward" y CScr_SetFxAngles(1) = vectoangles; el original pasa la POSICION + 100 como forward, se
// reproduce igual). Con on_fire el suelo pasa a "fire" (ningun mapa lo registra: no sale nada). La tabla sale de las llamadas
// setFootstepEffect("suelo", LoadFx("ruta")) del rawfile clientscripts/<mapa>_fx.csc (Nacht, Verruckt, Shi No Numa, Der Riese).
// h5_audio_cl (frame de servidor) llama H19_AIFootFx con el punto y el suelo; aqui se encola y H19_FootFxFrame (frame de render,
// h7e_light) los lanza. H19_FOOTFX=0 lo apaga.
#include <zlib.h>
namespace {
std::map<std::string, std::string> s_stepFx; bool s_stepFxRead;
struct StepQ { float pos[3]; std::string surf; };
std::vector<StepQ> s_stepQ; int s_stepPlayed;
void ReadStepFx()
{
    s_stepFxRead = true;
    const char *mn = Dvar_GetString("mapname"); if (!mn || !*mn) { s_stepFxRead = false; return; }
    char nm[160]; snprintf(nm, sizeof nm, "clientscripts/%s_fx.csc", mn);
    RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, nm, 0, -1).rawfile;
    if (!rf || !rf->buffer.get()) return;
    const unsigned *hd = (const unsigned *)rf->buffer.get(); uLongf len = hd[0];
    std::vector<unsigned char> out(len + 1);
    if (uncompress(out.data(), &len, (const unsigned char *)(hd + 2), hd[1]) != Z_OK) return;
    const std::string t((const char *)out.data(), len);
    for (size_t p = 0; (p = t.find("setFootstepEffect", p)) != std::string::npos; ) {
        const size_t nl = t.find('\n', p); p += 17;
        size_t a = t.find('"', p), b = a == std::string::npos ? a : t.find('"', a + 1);
        size_t lf = t.find("LoadFx", p); if (lf == std::string::npos) lf = t.find("loadfx", p);
        if (b == std::string::npos || lf == std::string::npos || (nl != std::string::npos && lf > nl)) continue;
        size_t c = t.find('"', lf), d = c == std::string::npos ? c : t.find('"', c + 1);
        if (d == std::string::npos) continue;
        s_stepFx[t.substr(a + 1, b - a - 1)] = t.substr(c + 1, d - c - 1);
    }
    fprintf(stderr, "[H19] pasos de la IA: %zu setFootstepEffect en %s\n", s_stepFx.size(), nm);
}
}
void H19_AIFootFx(const float *pos, const char *surf, bool onFire)
{
    static const bool on = H19_EnvOn("H19_FOOTFX"); if (!on) return;
    if (!s_stepFxRead) ReadStepFx();
    if (s_stepFx.empty()) return;
    if (s_stepQ.size() >= 64) s_stepQ.erase(s_stepQ.begin());   // sin frame de render no crece
    StepQ q; memcpy(q.pos, pos, 12); q.surf = onFire ? "fire" : (surf ? surf : ""); s_stepQ.push_back(q);
}
void H19_FootFxFrame()
{
    for (const StepQ &q : s_stepQ) {
        auto it = s_stepFx.find(q.surf); if (it == s_stepFx.end()) continue;
        float f[3] = { q.pos[0], q.pos[1], q.pos[2] + 100.0f }, ang[3];
        if (Vec3Normalize(f) == 0.0f) continue;
        vectoangles(f, ang);
        H19_PlayFxAng(it->second.c_str(), q.pos, ang);
        if (++s_stepPlayed == 1 || s_stepPlayed % 200 == 0)
            fprintf(stderr, "[H19] pasos de la IA: %d efectos (ultimo '%s' en %s)\n", s_stepPlayed, it->second.c_str(), q.surf.c_str());
    }
    s_stepQ.clear();
}
#endif
