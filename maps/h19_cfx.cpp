// OLA F1: createfx de cliente compartido (ver h19_cfx.h). Mismo metodo que los clientes de M3/M6 (Nacht, Verruckt), en un solo sitio.
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <database/db_registry.h>
#include <EffectsCore/fx_system.h>
#include <zlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h19_mapcl.h"
#include "h19_cfx.h"

const FxEffectDef *__cdecl FX_Register(char *name);   // h5_fx.cpp
extern FxSystemContainer *fx_systemPool;              // fx_system.cpp (lo reserva h5_fx)

namespace {
std::string Lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
bool Quoted(const std::string &t, size_t p, std::string &out, size_t *end = nullptr)   // primera cadena entre comillas desde p
{
    size_t a = t.find('"', p); if (a == std::string::npos) return false;
    size_t b = t.find('"', a + 1); if (b == std::string::npos) return false;
    out = t.substr(a + 1, b - a - 1); if (end) *end = b + 1; return true;
}
void Axis(const float *ang, float ax[3][3])   // set_forward_and_up_vectors + eje izquierdo = up x forward
{
    float f[3], r[3], u[3]; AngleVectors(ang, f, r, u);
    memcpy(ax[0], f, 12); memcpy(ax[2], u, 12);
    ax[1][0] = u[1] * f[2] - u[2] * f[1]; ax[1][1] = u[2] * f[0] - u[0] * f[2]; ax[1][2] = u[0] * f[1] - u[1] * f[0];
}
} // namespace

// rawfile de script: [len][zlen][zlib] (mismo formato que RawText de h5_audio_cl)
std::string H19_RawText(const char *name)
{
    RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)name, 0, -1).rawfile;
    if (!rf || !rf->buffer.get()) return {};
    const unsigned *hd = (const unsigned *)rf->buffer.get(); uLongf outlen = hd[0];
    std::vector<unsigned char> out(outlen + 1);
    if (uncompress(out.data(), &outlen, (const unsigned char *)(hd + 2), hd[1]) != Z_OK) return {};
    return std::string((const char *)out.data(), outlen);
}

void H19Cfx_Load(H19Cfx &c, const char *tag, const char *fxCsc, const char *createfxCsc)
{
    c.tag = tag ? tag : "?";
    if (fxCsc) {   // level._effect["clave"] = LoadFx("ruta")
        const std::string t = H19_RawText(fxCsc), l = Lower(t);
        size_t p = 0;
        while ((p = l.find("level._effect[", p)) != std::string::npos) {
            std::string key, path; size_t e = 0;
            if (!Quoted(t, p, key, &e)) break;
            size_t lf = l.find("loadfx", e), nl = t.find('\n', e);
            p = e;
            if (lf == std::string::npos || (nl != std::string::npos && lf > nl)) continue;
            if (Quoted(t, lf, path)) c.effect[key] = path;
        }
    }
    if (!createfxCsc) return;
    const std::string t = H19_RawText(createfxCsc);
    size_t p = 0; H19Cfx::Ent cur; bool open = false;
    auto flush = [&]() {
        if (open && !cur.fxid.empty()) {
            if (cur.exploder >= 0 && !c.exploderId.count(cur.exploder)) { const int id = (int)c.exploderId.size() + 1; c.exploderId[cur.exploder] = id; }
            c.ents.push_back(cur);
        }
        cur = H19Cfx::Ent(); open = false;
    };
    while (p < t.size()) {
        size_t q = t.find('\n', p); if (q == std::string::npos) q = t.size();
        const std::string ln = t.substr(p, q - p); p = q + 1;
        if (ln.find("createOneshotEffect") != std::string::npos) { flush(); open = true; cur.delay = -15; continue; }
        if (ln.find("createLoopEffect") != std::string::npos) { flush(); open = true; cur.delay = 0.5f; cur.loop = true; continue; }
        if (ln.find("createExploder") != std::string::npos) { flush(); open = true; cur.delay = 0; continue; }
        size_t k = ln.find("ent.v[");
        if (!open || k == std::string::npos) continue;
        std::string key, val; size_t e = 0; if (!Quoted(ln, k, key, &e)) continue;
        size_t eq = ln.find('=', e); if (eq == std::string::npos) continue;
        const char *r = ln.c_str() + eq + 1;
        if (key == "origin" || key == "angles") { float *d = key == "origin" ? cur.org : cur.ang; sscanf(r, " ( %f , %f , %f )", &d[0], &d[1], &d[2]); }
        else if (key == "delay") cur.delay = (float)atof(r);
        else if (key == "fxid") { if (Quoted(ln, eq, val)) cur.fxid = val; }
        else if (key == "exploder") { const char *s = r; while (*s == ' ' || *s == '"') ++s; cur.exploder = atoi(s); }
        else if (key == "type") { if (Quoted(ln, eq, val)) { if (val == "loopfx") cur.loop = true; else if (val != "oneshotfx" && val != "exploder") open = false; } }
    }
    flush();
    size_t nexp = 0, nloop = 0; for (const auto &o : c.ents) { nexp += o.exploder >= 0; nloop += o.loop; }
    fprintf(stderr, "[H19] %s: createfx %zu (%zu oneshot, %zu loopfx, %zu exploders en %zu numeros), %zu level._effect\n", c.tag.c_str(), c.ents.size(),
        c.ents.size() - nexp - nloop, nloop, nexp, c.exploderId.size(), c.effect.size());
}

const char *H19Cfx_Path(const H19Cfx &c, const char *key)
{
    auto it = c.effect.find(key); return it == c.effect.end() ? nullptr : it->second.c_str();
}

void H19Cfx_PerkInit(H19Cfx &c, const std::vector<H19Kv> &ents)
{
    H19Cfx_Load(c, c.tag.empty() ? "?" : c.tag.c_str(), "clientscripts/_zombiemode.csc", nullptr);   // init_perk_machines_fx: level._effect["*_light"]
    for (const H19Kv &k : ents) {
        const char *cn = H19_Str(k, "classname"), *tn = H19_Str(k, "targetname");
        if (!cn || !tn || strcmp(cn, "script_struct") || strcmp(tn, "perksacola")) continue;
        H19Cfx::Perk q; memset(q.org, 0, 12); H19_Vec(k, "origin", q.org); q.org[2] -= 50;   // play_perk_fx_on_client: origin + (0, 0, -50)
        const char *ss = H19_Str(k, "script_sound"); const std::string s = ss ? ss : "";
        q.fx = s == "mx_speed_jingle" ? "sleight_light" : s == "mx_doubletap_jingle" ? "doubletap_light" : s == "mx_divetonuke_jingle" ? "divetonuke_light" :
               s == "mx_marathon_jingle" ? "marathon_light" : s == "mx_revive_jingle" ? "revive_light" :
               s == "mx_additionalprimaryweapon_jingle" ? "additionalprimaryweapon_light" : "jugger_light";
        c.perks.push_back(q);
    }
}
void H19Cfx_PerkStart(H19Cfx &c, int now)
{
    if (c.perkStep >= 0 || c.perks.empty() || c.perkTimer > 0) return;   // perk_start_up corre una vez
    c.perkStep = 0; c.perkAt = now; c.perkDur = 0.1f; c.perkTimer = 0;
}
namespace {
void PerkFrame(H19Cfx &c, int now)   // perk_start_up: timer 0, duration 0.1; FX durante duration s; duration += 0.2; hasta timer >= 3; realwait(duration)
{
    for (size_t i = 0; i < c.perkFx.size();) { if (now >= c.perkFx[i].second) { H19_KillFx(c.perkFx[i].first); c.perkFx.erase(c.perkFx.begin() + i); } else ++i; }
    if (c.perkStep < 0 || now < c.perkAt) return;
    static const float ang[3] = { 0, 0, 0 };   // fxobj.angles comentado en el original
    for (const H19Cfx::Perk &q : c.perks) { const char *p = H19Cfx_Path(c, q.fx.c_str()); if (p) c.perkFx.push_back({ H19_PlayFxAng(p, q.org, ang), now + (int)(c.perkDur * 1000.0f) }); }
    c.perkTimer += c.perkDur; c.perkDur += 0.2f; ++c.perkStep;
    if (c.perkTimer >= 3.0f) { fprintf(stderr, "[H19] %s: parpadeo de ventajas terminado (%d pasos, %zu maquinas) t=%d\n", c.tag.c_str(), c.perkStep, c.perks.size(), now); c.perkStep = -1; return; }
    c.perkAt = now + (int)(c.perkDur * 1000.0f);
}
} // namespace

void H19Cfx_QueueExploder(H19Cfx &c, const std::string &st)
{
    std::lock_guard<std::mutex> g(c.mu); c.pending.push_back(atoi(st.c_str()));
}

void H19Cfx_Frame(H19Cfx &c, int now)
{
#ifdef H5FX
    // h5_fx inicia el FxSystem en su primer frame (FX_InitSystem): antes FX_SpawnEffect cae. Hasta que el reloj de FX corre no se lanza nada
    if (!fx_systemPool) return;
    const int fxNow = FX_GetSystem(0)->system.msecNow;
    if (c.t0 < 0) { if (fxNow <= 0) return; c.t0 = fxNow; fprintf(stderr, "[H19] %s: createfx: reloj de FX en marcha, t0=%d\n", c.tag.c_str(), fxNow); }
    for (H19Cfx::Ent &o : c.ents) {
        if (o.exploder >= 0) continue;
        if (o.loop) {   // loopfx: playFx(.., delay) y luego cada "delay" s, cuando el anterior ha terminado (isfxplaying)
            if (o.nextLoop < 0) o.nextLoop = c.t0;
            if (fxNow < o.nextLoop) continue;
            const char *path = H19Cfx_Path(c, o.fxid.c_str());
            const FxEffectDef *fx = path ? FX_Register((char *)path) : nullptr;
            if (!fx) { if (!o.done) ++c.missing; o.done = true; o.nextLoop = 0x7fffffff; continue; }
            float ax[3][3]; Axis(o.ang, ax);
            FX_PlayOrientedEffect(0, fx, fxNow, o.org, ax);
            if (!o.done) { o.done = true; ++c.loops; }
            o.nextLoop = fxNow + (int)((o.delay > 0.05f ? o.delay : 0.05f) * 1000.0f);
            continue;
        }
        if (o.done) continue;
        if (o.delay > 0 && fxNow < c.t0 + (int)(o.delay * 1000.0f)) continue;   // oneshotfxthread: realWait(delay)
        o.done = true;
        const char *path = H19Cfx_Path(c, o.fxid.c_str());
        const FxEffectDef *fx = path ? FX_Register((char *)path) : nullptr;
        if (!fx) { ++c.missing; continue; }
        float ax[3][3]; Axis(o.ang, ax);
        FX_PlayOrientedEffect(0, fx, c.t0 + (int)(o.delay * 1000.0f), o.org, ax);   // create_triggerfx: inicio = delay*1000 desde el arranque
        ++c.played;
    }
    if (!c.logged) { bool all = true; for (const H19Cfx::Ent &o : c.ents) all = all && (o.done || o.exploder >= 0);
        if (all) { c.logged = true; fprintf(stderr, "[H19] %s: createfx: %d oneshotfx y %d loopfx lanzados (%d sin FX en los datos)\n", c.tag.c_str(), c.played, c.loops, c.missing); } }
#endif
    PerkFrame(c, now);
    // exploders: activate_exploder / deactivate_exploder
    std::vector<int> ids; { std::lock_guard<std::mutex> g(c.mu); ids.swap(c.pending); }
    for (int id : ids) {
        int num = -1; for (auto &e : c.exploderId) if (e.second == (id < 0 ? -id : id)) num = e.first;
        if (num < 0) { fprintf(stderr, "[H19] %s: exploder id %d desconocido\n", c.tag.c_str(), id); continue; }
        int n = 0;
        for (size_t i = 0; i < c.ents.size(); ++i) {
            H19Cfx::Ent &o = c.ents[i];
            if (o.exploder != num) continue;
            for (unsigned &h : o.fx) H19_KillFx(h);   // cannon_effect / deactivate_exploder: borra los loopFX anteriores
            o.fx.clear();
            if (id < 0) continue;
            c.tasks.push_back({ now + (o.delay > 0 ? (int)(o.delay * 1000.0f) : 0), i });   // exploder_delay
            ++n;
        }
        c.explodersOn += id > 0;
        fprintf(stderr, "[H19] %s: exploder %d (id %d) %s: %d efectos t=%d\n", c.tag.c_str(), num, id, id > 0 ? "activado" : "desactivado", n, now);
    }
    for (size_t k = 0; k < c.tasks.size();) {
        if (now < c.tasks[k].at) { ++k; continue; }
        H19Cfx::Ent &o = c.ents[c.tasks[k].ent];
        c.tasks.erase(c.tasks.begin() + k);
        const char *path = H19Cfx_Path(c, o.fxid.c_str()); if (!path) continue;
        float f[3], r[3], u[3]; AngleVectors(o.ang, f, r, u);
        o.fx.push_back(H19_PlayFx(path, o.org, f, u));   // spawnFx + triggerfx
    }
}
#endif
