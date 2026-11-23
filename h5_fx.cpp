// OLA 6c (efectos): EffectsCore ORIGINAL sobre el servidor dedicado del harness y el render de H4. Lo que hace:
//   - reserva los pools de FX como FX_AllocateClientMemory (sin hunk de cliente) y llama a FX_InitSystem (dvars fx_* originales);
//   - cada frame, en g_h4PreFrame (antes de abrir el pase), la secuencia de CG_DrawActiveFrame + R_ProcessCmd_UpdateFx*:
//     FX_BeginUpdate -> FX_SetNextUpdateTime -> FX_SetNextUpdateCamera (camara de H4) -> eventos del frame de servidor ->
//     FX_UpdateNonDependent -> FX_UpdateRemaining -> FX_EndUpdate -> FX_FillGenerateVertsCmd + FX_GenerateVerts (sprites, beams,
//     codemesh por R_AddCodeMeshDrawSurf, r_drawsurf.cpp) y la subida del codemesh como RB_UpdateDynamicBuffers (lock 0x2000);
//   - el dibujo es el de la lista emisiva del original (r_scene.cpp R_FinishDecalAndEmissiveDrawSurfs): R_SortDrawSurfs de la region
//     13, R_MergeAndEmitDrawSurfLists(0xA, 6) y el bucle de R_DrawSurfs con R_RenderDrawSurfListMaterial (tess 11 = R_TessCodeMeshList)
//     con tecnica base gfxDrawMethod.emissiveTechType (5). Va en g_h4DrawExtra despues de mundo y actores y ANTES del viewmodel
//     (que limpia la profundidad);
//   - eventos (CG_CheckPlayerstateEvents / CG_CheckEvents de cg_event.cpp) leidos de g_entities: disparo -> WeaponFlash ORIGINAL
//     (cg_weapons.cpp) -> CG_PlayBoltedEffect -> FX_PlayBoltedEffect en tag_flash del viewmodel (handle 1536); EV_BULLET_HIT ->
//     CG_ImpactEffectForWeapon ORIGINAL (tabla de impactos del ff) + CG_RandomEffectAxis + FX_PlayOrientedEffect (lo que hace
//     CG_BulletHitEvent_Internal sin centity_s: sin marcas ni efecto atornillado al hueso); EV_PLAY_FX / EV_PLAY_FX_ON_TAG ->
//     cgs.fxs[] registrado de los configstrings 2080+i como CG_RegisterGraphics.
//   Pegamento de cliente sin cg_entities: CG_GetPose / CG_DObjGetWorldBoneMatrix / CG_GetDObjOrientation para el viewmodel (1536)
//   con el esqueleto que deja h5_weapon.cpp (espacio de modelo) por cg->viewModelAxis.
// Variables: H5FX_OFF=1 (sin FX), H5FX_STATS=1 (coste cada 600 frames), H5FX_TEST="<efecto>" (lo lanza delante de la camara cada 2 s).
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <qcommon/common.h>
#include <server_mp/server_mp.h>
#include <server_mp/sv_init_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <bgame/bg_weapons.h>
#include <bgame/bg_weapons_def.h>
#include <bgame/bg_misc.h>
#include <clientscript/scr_const.h>
#include <database/database.h>
#include <database/db_registry.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <cgame_mp/cg_local_mp.h>
#include <cgame_mp/cg_main_mp.h>
#include <cgame_mp/cg_ents_mp.h>
#include <cgame/cg_local.h>
#include <cgame/cg_weapons.h>
#include <cgame/cg_effects_load_obj.h>
#include <xanim/dobj.h>
#include <xanim/dobj_utils.h>
#include <EffectsCore/fx_system.h>
#include <EffectsCore/fx_update.h>
#include <EffectsCore/fx_draw.h>
#include <EffectsCore/fx_dvars.h>
#include <EffectsCore/fx_load_obj.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_drawsurf.h>
#include <gfx_d3d/r_pretess.h>
#include <gfx_d3d/r_buffers.h>
#include <gfx_d3d/r_draw_method.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_material_load_obj.h>
#include <gfx_d3d/rb_backend.h>
#include <gfx_d3d/r_state.h>
#include <gfx_d3d/r_rendertarget.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_dvars.h>
#include <d3d9.h>
#include <gfx_d3d/r_workercmds_common.h>
#include <clientscript/cscr_stringlist.h>
#include <EffectsCore/fx_marks.h>
#include <qcommon/dobj_management.h>
#include <universal/com_memory.h>
#include <cgame/cg_drawtools.h>
#include "world_h4.h"
#include <gfx_d3d/r_meshdata.h>
#include <gfx_d3d/rb_tess.h>
#include <gfx_d3d/rb_shade.h>
#include <xanim/xmodel_utils.h>
#include <gfx_d3d/r_model_skin.h>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <chrono>

#include <gfx_d3d/r_material_consts.h>
#include "h5_fx_reg.inc"     // FX_Register / FX_Register_FastFile (fx_load_obj.cpp, tal cual)
#include "h5_fx_msort.inc"   // Material_SortInternal y sus claves (r_material_load_obj.cpp, tal cual)
#include "h5_fx_cgev.inc"    // CG_GetBoneIndex / CG_PlayBoltedEffect (cg_event.cpp, tal cual)
#include "h5_fx_impact.inc"  // CG_RegisterImpactEffects_FastFile (cg_effects_load_obj.cpp, tal cual)

extern FxSystemContainer *fx_systemPool; extern FxSystemBuffers *fx_systemBufferPool;   // fx_system.cpp
extern FxMarksSystem *fx_marksSystemPool; extern int fx_maxLocalClients;           // fx_marks.cpp
extern GfxBackEndData s_backEndData[2];     // r_rendercmds.cpp
extern GfxMeshData g_codeMesh[2];           // r_rendercmds.cpp
extern void (*g_h4PreFrame)();              // world_h4.cpp
extern bool g_h5wViewOverride; extern int g_h5wViewTime;   // h5_weapon.cpp (tiempo del frame de render)
void __cdecl R_InitPrimaryLights(GfxLight *primaryLights);   // h5_light.cpp (r_bsp_load_obj.cpp, extraida tal cual)

// handles de 16 bits (FX_EffectToHandle = desplazamiento / 4): 1024 contenedores deben caber; en x86 eran 0xD0 bytes
static_assert(sizeof(FxEffectContainer) % 4 == 0 && 1024 * sizeof(FxEffectContainer) / 4 < 0xFFFF, "FxEffectContainer LP64");

void H12_GlassGenerateVerts();   // OLA 12 (h12_barrier.cpp debil / h12_glasscl.cpp)
void H4_GetCam(float *org, float *ang);   // OLA E2a: vista para la distancia del terremoto
namespace {
bool g_on, g_init, g_ready;
GfxImage *g_floatZFar;   // floatz "lejos" para las tecnicas zfeather (ver Init)
void (*g_prevPre)();
void (*g_prevDraw)(unsigned char);
void (*g_prevLate)(unsigned char);
GfxBackEndData *g_data;           // frontEndDataOut del frame en que se generaron los vertices
int g_lastSvTime = -1, g_fxTime;
const FxEffectDef *g_fxs[196];    // cgs_t::fxs (CS_EFFECT_NAMES = 2080)
bool g_fxsTried[196];
const FxEffectDef *g_test; int g_testNext;
// OLA E2a: CG_StartShakeCamera / CG_UpdateCameraShake / CG_ShakeCamera (cg_camerashake.cpp) para Earthquake() del GSC
struct Shake { float scale, length, src[3], radius, size, rumble; int time; } g_shakes[4];
float g_shakePhase; int g_evQuake;
bool UpdateShake(Shake &k, const float *view, int time)
{
    const int dt = time - k.time;
    if (k.length <= 0 || dt < 0 || (float)dt >= k.length) return false;
    const float d = sqrtf((k.src[0] - view[0]) * (k.src[0] - view[0]) + (k.src[1] - view[1]) * (k.src[1] - view[1]) + (k.src[2] - view[2]) * (k.src[2] - view[2]));
    const float val = 1.0f - d / k.radius, x = (1.0f - (float)dt / k.length) * k.scale;
    if (x <= 0) return false;
    k.size = val < 0 ? val / x : val * x; k.rumble = x; return true;
}
// estado de eventos (CG_CheckPlayerstateEvents / CG_CheckEvents)
int s_psPred = -1, s_psUnpred = -1;
bool s_entSeen[MAX_GENTITIES_SV]; int s_entUse[MAX_GENTITIES_SV], s_entSeq[MAX_GENTITIES_SV], s_tempTime[MAX_GENTITIES_SV];
// estadisticas
int g_frames, g_evFlash, g_evImpact, g_evFx, g_evFxTag, g_surfs, g_maxSurfs, g_distSkipped, g_projDecalSkipped;   // g_distSkipped, g_projDecalSkipped: OLA E2a
double g_msUpd, g_msDraw, g_msMax, g_msReg;
unsigned g_flashWeapon = ~0u; const void *g_flashMtl[32]; int g_flashMtlCount;   // ver CollectFlashMaterials
// H5FX_SHOTS: captura PNG unos frames despues de un fogonazo / impacto / sangre (max 3 de cada) para la evidencia
int g_shotAt[4] = { -1, -1, -1, -1 }, g_shotN[4]; const char *const g_shotName[4] = { "fogonazo", "impacto", "sangre", "prueba" };
bool g_shotWant[4];
void ArmShot(int k, int) { if (getenv("H5FX_SHOTS") && g_shotN[k] < 3 && g_shotAt[k] < 0) g_shotWant[k] = true; }
void ShotIfDrawn(int k) { if (g_shotWant[k]) { g_shotWant[k] = false; g_shotAt[k] = g_frames + 1; } }   // al frame siguiente (ya presentado)
// H5FX_STATS: en Android va encendido por defecto (la app no pasa variables; una linea cada 600 frames y como mucho 40 avisos de tiron)
bool StatsOn() {
#ifdef __ANDROID__
    static const bool on = !(getenv("H5FX_STATS") && !atoi(getenv("H5FX_STATS")));
#else
    static const bool on = getenv("H5FX_STATS") != nullptr;
#endif
    return on; }
int g_hitchLogs;
// OLA 7d: marcas y decals (agujeros de bala, sangre en paredes). H7D_MARKS=0 las quita (fx_marks a 0); H7D_MARKS_MODELS=1 deja tambien
// fx_marks_smodels/fx_marks_ents (marcas sobre modelos: sin portar el dibujo de marcas de modelo, ver Init); H7D_LITFX=0 no dibuja la
// lista de decals/FX con luz (regiones 3..9) y deja solo la emisiva como en 6c/7b.
bool g_marksOn, g_litFxOn;
int g_markSurfs, g_markMax, g_markMeshes, g_litSurfs; double g_msMarks; int g_prevNow = -1;
// OLA 7d: efectos de modelo (FX_ELEM_TYPE_MODEL: casquillos, trozos). FX_AddNonSpriteDrawSurfs (r_workercmds_common.cpp) -> FX_DrawElem_Model
// -> R_FilterXModelIntoScene, que aqui guarda el modelo y su colocacion; se dibujan rigidos como los smodels de 7c (deformado en CPU de un
// esqueleto con la colocacion y H4_DrawSkinnedSurf). Por defecto encendidos desde OLA 15B; H7D_FXMODELS=0 los apaga; H7D_BRASS=0 quita los casquillos (CG_EjectWeaponBrass).
struct FxModelInst { const XModel *model; GfxScaledPlacement pl; };
std::vector<FxModelInst> g_fxModels; bool g_fxModelsOn, g_brassOn; int g_fxModelSurfs, g_fxModelMax, g_evBrass, g_fxLights, g_modelVerts; double g_msModels, g_msModelSkin, g_msModelMax;
double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void DumpEffect(const FxEffectDef *fx);
const FxEffectDef *EffectFor(int id)
{
    if (id <= 0 || id >= 196) return nullptr;
    if (!g_fxsTried[id]) {
        g_fxsTried[id] = true;
        char name[256]; SV_GetConfigstring(2080 + id, name, sizeof name);
        if (*name) { const double r0 = NowMs(); g_fxs[id] = FX_Register(name); g_msReg += NowMs() - r0; fprintf(stderr, "[H5FX] fx %d '%s' -> %s\n", id, name, g_fxs[id] ? "ok" : "NO"); }
    }
    return g_fxs[id];
}

void PlayAt(const FxEffectDef *fx, const float *org, const float *fwd)
{
    if (!fx) return;
    float axis[3][3]; axis[0][0] = fwd[0]; axis[0][1] = fwd[1]; axis[0][2] = fwd[2];
    CG_RandomEffectAxis(axis[0], axis[1], axis[2]);
    FX_PlayOrientedEffect(0, fx, g_fxTime, org, axis);
}

// CG_EntityEvent (cg_event.cpp): solo los casos que lanzan FX
void EntityEvent(int number, const gentity_s *e, int event, int eventParm, bool isPlayerView, const playerState_s *ps)
{
    switch (event & 0xFF) {
    case EV_FIRE_WEAPON: case EV_FIRE_WEAPON_LASTSHOT: case EV_FIRE_WEAPON_LEFT: case EV_FIRE_WEAPON_LASTSHOT_LEFT: {
        if (!isPlayerView || !ps) return;   // fogonazo del mundo de otros (worldFlashEffect): no hay otros jugadores en el harness
        const unsigned weapon = ps->weapon;
        if (!weapon || weapon >= BG_GetNumWeapons()) return;
        const int ev = event & 0xFF;
        const unsigned tag = (ev == EV_FIRE_WEAPON_LEFT || ev == EV_FIRE_WEAPON_LASTSHOT_LEFT) ? scr_const.tag_flash1 : scr_const.tag_flash;
        // CG_FireWeapon marca el fogonazo y CG_AddPlayerWeapon (CG_ShowMuzzleFlash) llama a WeaponFlash con el DObj del viewmodel
        if (Com_GetClientDObj(CG_WeaponDObjHandle(0), 0)) {
            if (!g_evFlash) { const FxEffectDef *vf = BG_GetWeaponDef(weapon)->viewFlashEffect; fprintf(stderr, "[H5FX] fogonazo: %s\n", vf ? vf->name.get() : "(ninguno)"); }
            WeaponFlash(0, CG_WeaponDObjHandle(0), weapon, 1, tag); ++g_evFlash; ArmShot(0, 2);
            // OLA 7d: CG_FireWeapon -> CG_EjectWeaponBrass (cg_weapons.cpp) si el arma no es de cerrojo: efecto de casquillo de vista
            // (viewShellEjectEffect, o viewLastShotEjectEffect en el ultimo tiro) atornillado a tag_brass del viewmodel
            if (g_brassOn && cg_brass && cg_brass->current.enabled && !BG_GetWeaponDef(weapon)->bBoltAction) {
                const WeaponDef *wd = BG_GetWeaponDef(weapon);
                const FxEffectDef *bfx = (wd->viewLastShotEjectEffect && wd->worldLastShotEjectEffect && (ev == EV_FIRE_WEAPON_LASTSHOT || ev == EV_FIRE_WEAPON_LASTSHOT_LEFT))
                    ? wd->viewLastShotEjectEffect.get() : wd->viewShellEjectEffect.get();
                const unsigned btag = (ev == EV_FIRE_WEAPON_LEFT || ev == EV_FIRE_WEAPON_LASTSHOT_LEFT) ? scr_const.tag_brass1 : scr_const.tag_brass;
                if (bfx) { CG_PlayBoltedEffect(0, bfx, CG_WeaponDObjHandle(0), btag); if (!g_evBrass++) fprintf(stderr, "[H7D] casquillo: %s\n", bfx->name.get()); }
            }
            if (g_evFlash <= 3 && StatsOn()) {
                float o[3], ax[3][3], tx, ty; H4_GetView(o, ax, &tx, &ty); const float *v = cgArray ? cgArray->viewModelAxis[3] : o;
                fprintf(stderr, "[H5FX] fogonazo %d: cam %.1f %.1f %.1f  viewmodel %.1f %.1f %.1f  materiales %d  activos %d\n", g_evFlash, o[0], o[1], o[2], v[0], v[1], v[2],
                        g_flashMtlCount, FX_GetSystem(0)->shared.firstNewEffect - FX_GetSystem(0)->shared.firstActiveEffect); } }
        return; }
    case EV_BULLET_HIT: {   // CG_BulletHitEvent -> CG_BulletHitEvent_Internal
        const unsigned weapon = e->s.weapon;
        if (!weapon || weapon >= BG_GetNumWeapons()) return;
        float dir[3]; ByteToDir(eventParm, dir);
        const FxEffectDef *fx = nullptr; unsigned snd = 0;
        if (!cgMedia.fx) return;
        CG_ImpactEffectForWeapon(weapon, e->s.surfType, (char)e->s.un1.scale, &fx, &snd);
        if (fx && e->s.surfType == 7 && !cg_blood->current.enabled) fx = nullptr;
        PlayAt(fx, e->r.currentOrigin, dir); ++g_evImpact; if (fx) ArmShot(e->s.surfType == 7 ? 2 : 1, 2);
        if (fx && getenv("H5FX_DUMP")) {   // primer efecto de cada tipo de superficie: nombre y materiales de sus elementos
            static bool seen[64]; const unsigned st = e->s.surfType & 63;
            if (!seen[st]) { seen[st] = true; fprintf(stderr, "[H5FX] impacto surf %u: %s\n", st, fx->name.get()); DumpEffect(fx); } }
        return; }
    case EV_GRENADE_EXPLODE: case EV_ROCKET_EXPLODE: case EV_ROCKET_EXPLODE_NOMARKS: case EV_FLASHBANG_EXPLODE:
    case EV_CUSTOM_EXPLODE: case EV_CUSTOM_EXPLODE_NOMARKS: {
        // OLA F2: CG_EntityEvent (cg_event.cpp) de las explosiones de proyectil (granadas, semtex, ray gun, ballesta, cohetes): efecto de
        // impacto de la tabla del ff por superficie (salvo el flashbang) y el projExplosionEffect del arma, orientados con
        // ByteToDir(eventParm) + Vec3Basis_RightHanded. EV_CUSTOM_* elige projExplosionEffect2..5 por lerp.u.loopFx.period. Sin agua
        // simulada (r_watersim) ni Ragdoll/DynEnt/notify "explode" de cliente. El sonido lo pone h5_audio_cl. H5FX_EXPLODE=0 lo apaga.
        static const bool on = !(getenv("H5FX_EXPLODE") && !atoi(getenv("H5FX_EXPLODE"))); if (!on) return;
        const unsigned weapon = e->s.weapon;
        if (!weapon || weapon >= BG_GetNumWeapons() || !cgMedia.fx) return;
        const WeaponDef *wd = BG_GetWeaponDef(weapon); const int ev = event & 0xFF;
        float pos[3]; memcpy(pos, e->r.currentOrigin, 12);
        float axis[3][3]; ByteToDir(eventParm, axis[0]); Vec3Basis_RightHanded(axis[0], axis[1], axis[2]);
        const FxEffectDef *ifx = nullptr; unsigned snd = 0;
        if (ev != EV_FLASHBANG_EXPLODE && e->s.surfType < 31) CG_ImpactEffectForWeapon(weapon, e->s.surfType, 0, &ifx, &snd);
        if (ifx) FX_PlayOrientedEffect(0, ifx, g_fxTime, pos, axis);
        const FxEffectDef *xfx = wd->projExplosionEffect.get();
        if (ev == EV_CUSTOM_EXPLODE || ev == EV_CUSTOM_EXPLODE_NOMARKS) switch (e->s.lerp.u.loopFx.period) {
            case '1': xfx = wd->projExplosionEffect2.get(); break; case '2': xfx = wd->projExplosionEffect3.get(); break;
            case '3': xfx = wd->projExplosionEffect5.get(); break; case '4': xfx = wd->projExplosionEffect4.get(); break; default: break; }
        if (xfx) FX_PlayOrientedEffect(0, xfx, g_fxTime, pos, axis);
        static int nExp; if (nExp++ < 24) fprintf(stderr, "[H5FX] explosion t=%d ev %d arma %s en (%.0f %.0f %.0f) surf %d: impacto %s, efecto %s\n", level.time, ev,
            BG_GetWeaponVariantDef(weapon)->szInternalName.get(), pos[0], pos[1], pos[2], e->s.surfType, ifx ? ifx->name.get() : "-", xfx ? xfx->name.get() : "-");
        return; }
    case EV_PLAY_FX: {   // CG_PlayFx
        const FxEffectDef *fx = EffectFor(eventParm);
        if (!fx) return;
        float axis[3][3]; AnglesToAxis(e->r.currentAngles, axis);
        FX_PlayOrientedEffect(0, fx, g_fxTime, e->r.currentOrigin, axis); ++g_evFx;
        return; }
    case EV_PLAY_FX_ON_TAG: {   // CG_PlayFxOnTag: "NNNtag" en el configstring 2276+parm; sin DObj de cliente se coloca con el del servidor
        char tagAndEffect[256]; SV_GetConfigstring(eventParm + 2276, tagAndEffect, sizeof tagAndEffect);
        if (!tagAndEffect[0] || !tagAndEffect[1] || !tagAndEffect[2]) return;
        const int fxId = 100 * (tagAndEffect[0] - '0') + 10 * (tagAndEffect[1] - '0') + tagAndEffect[2] - '0';
        const FxEffectDef *fx = EffectFor(fxId);
        if (!fx) return;
        // OLA 15E: el original lo ancla a la propia entidad del evento (CG_PlayFxOnTag: dobjHandle = cent->nextState.number); con
        // otherEntityNum (0 en casi todas) salia en el jugador y G_PlayerController caia sin clientinfo (maquina de ventaja)
        const gentity_s *target = e;
        const unsigned tagName = SL_FindString(tagAndEffect + 3, SCRIPTINSTANCE_SERVER);
        float m[4][3];
        if (!tagName || target->s.eType == ET_PLAYER || !G_DObjGetWorldTagMatrix(target, tagName, m)) { AnglesToAxis(target->r.currentAngles, m); memcpy(m[3], target->r.currentOrigin, 12); }
        FX_PlayOrientedEffect(0, fx, g_fxTime, m[3], m); ++g_evFxTag;
        return; }
    case EV_EARTHQUAKE: {   // OLA E2a: CG_StartShakeCamera(scale, duracion, origen, radio); la hueco de menor tamano si las 4 estan vivas
        Shake k{}; k.scale = e->s.lerp.u.turret.gunAngles[0]; k.length = (float)e->s.lerp.u.actor.team; k.radius = e->s.lerp.u.turret.gunAngles[1];
        memcpy(k.src, e->r.currentOrigin, 12); k.time = level.time;
        if (k.scale <= 0 || k.length <= 0 || k.radius <= 0) return;
        float o[3], a[3]; H4_GetCam(o, a); UpdateShake(k, o, k.time);
        int i = 0; for (; i < 4; ++i) if (g_shakes[i].time > level.time || (float)level.time >= g_shakes[i].time + g_shakes[i].length) break;
        if (i == 4) { float mn = k.size; for (int j = 0; j < 4; ++j) if (mn > g_shakes[j].size) { mn = g_shakes[j].size; i = j; } }
        if (i != 4) g_shakes[i] = k;
        if (g_evQuake++ < 8) fprintf(stderr, "[H5FX] terremoto t=%d escala %.2f %d ms radio %.0f en (%.0f,%.0f,%.0f)\n", level.time, k.scale, (int)k.length, k.radius, k.src[0], k.src[1], k.src[2]);
        return; }
    default: return;
    }
}

void ServerFrameEvents()
{
    gentity_s *pl = &g_entities[0];
    if (pl->r.inuse && pl->client) {
        playerState_s *ps = &pl->client->ps;
        const int pred = (unsigned short)ps->predictableEventSequence, unpred = (unsigned short)ps->unpredictableEventSequence;
        if (s_psPred < 0) { s_psPred = pred; s_psUnpred = unpred; }
        int d = (pred - s_psPred) & 0xFF; if (d > 4) { s_psPred = (pred - 4) & 0xFFFF; d = 4; }
        for (int k = 0; k < d; ++k) { const int i = (s_psPred + k) & 3; EntityEvent(0, pl, ps->predictableEvents[i], ps->predictableEventParms[i], true, ps); }
        s_psPred = pred;
        d = (unpred - s_psUnpred) & 0xFF; if (d > 4) { s_psUnpred = (unpred - 4) & 0xFFFF; d = 4; }
        for (int k = 0; k < d; ++k) { const int i = (s_psUnpred + k) & 3; EntityEvent(0, pl, ps->unpredictableEvents[i], ps->unpredictableEventParms[i], true, ps); }
        s_psUnpred = unpred;
    }
    for (int n = 0; n < level.num_entities; ++n) {
        gentity_s *e = &g_entities[n];
        if (!e->r.inuse) { s_entSeen[n] = false; continue; }
        if (e->client) continue;
        const int use = e->s.lerp.useCount;
        if ((int)e->s.eType >= 21) {   // ET_EVENTS + evento: entidad temporal, un evento al aparecer
            if (!s_entSeen[n] || s_entUse[n] != use || s_tempTime[n] != e->r.eventTime) {
                s_entSeen[n] = true; s_entUse[n] = use; s_tempTime[n] = e->r.eventTime;
                EntityEvent(n, e, (int)e->s.eType - 21, e->s.eventParm, false, nullptr);
            }
            continue;
        }
        if (!s_entSeen[n] || s_entUse[n] != use) {
            s_entSeen[n] = true; s_entUse[n] = use; s_tempTime[n] = -1;
            // OLA F2: entidad recien creada con su evento en el mismo frame (G_ExplodeMissile hace G_Spawn + G_AddEvent):
            // G_Spawn la deja a cero, asi que se parte de la secuencia 0 y el evento no se pierde.
            const bool fresh = e->r.eventTime && level.time - e->r.eventTime <= 100;
            s_entSeq[n] = fresh ? 0 : (unsigned short)e->s.eventSequence;
            if (!fresh) continue;
        }
        const int seq = (unsigned short)e->s.eventSequence;
        int d = (seq - s_entSeq[n]) & 0xFF; if (d > 4) { s_entSeq[n] = (seq - 4) & 0xFFFF; d = 4; }
        for (int k = 0; k < d; ++k) { const int i = (s_entSeq[n] + k) & 3; EntityEvent(n, e, e->s.events[i], e->s.eventParms[i], false, nullptr); }
        s_entSeq[n] = seq;
    }
}

// Material_Sort (r_material_load_obj.cpp) en LP64: DB_GetAllXAssetOfType rellena XAssetHeader de 4 bytes (Ptr32), no Material*
void SortMaterials()
{
    static XAssetHeader hdr[4096];
    const int n = DB_GetAllXAssetOfType(ASSET_TYPE_MATERIAL, hdr, 4096);
    for (int i = 0; i < n; ++i) rgp.sortedMaterials[i] = hdr[i].material.get();
    rgp.materialCount = n;
    Material_SortInternal(rgp.sortedMaterials, rgp.materialCount);
    rgp.needSortMaterials = 0;
    fprintf(stderr, "[H5FX] Material_Sort: %d materiales\n", n);
}

void Init()
{
    g_init = true;
    // FX_AllocateClientMemory (fx_system.cpp) lo llama CL_InitCGame... (cl_main_mp.cpp) en el arranque del cliente; si no corrio,
    // los mismos pools sin hunk, alineados a 128 y a cero
    if (!fx_systemPool) {
    fx_maxLocalClients = 1;
    void *p;
    posix_memalign(&p, 128, sizeof(FxSystemContainer)); memset(p, 0, sizeof(FxSystemContainer)); fx_systemPool = (FxSystemContainer *)p;
    posix_memalign(&p, 128, sizeof(FxSystemBuffers)); memset(p, 0, sizeof(FxSystemBuffers)); fx_systemBufferPool = (FxSystemBuffers *)p;
    posix_memalign(&p, 128, sizeof(FxMarksSystem)); memset(p, 0, sizeof(FxMarksSystem)); fx_marksSystemPool = (FxMarksSystem *)p;
    fx_systemPool[0].system.shared = &fx_systemPool[0].shared;
    fx_marksSystemPool[0].frameCount = 1;
    }
    FX_InitSystem(0);
    // CG_AllocateClientMemory (cg_main_mp.cpp) no corre en el harness: centity_s a cero (sin snapshots). FX_GetBoltTemporalBits y
    // FX_GetBoneOrientation_IsDObjEntityValid leen eFlags y las banderas del cent de cualquier efecto (dobjHandle 2047 = ninguno)
    if (!cg_entitiesArray[0]) { cg_entitiesArray[0] = (centity_s *)calloc(MAX_CL_ENTNUM, sizeof(centity_s)); cg_entArrayCount = MAX_CL_ENTNUM; }
    if (!cg_fakeEntitiesArray) cg_fakeEntitiesArray = (fake_centity_s *)calloc(MAX_CL_ENTNUM - 1024, sizeof(fake_centity_s));   // OLA 15A (de F15): CG_GetEntity(0, 2047) con g_entNumBits 10 indexa fake[1023]; con 512 SIGSEGV en FX_GetBoltTemporalBits (Fold)
    // CG_RegisterDvars: las que leen los caminos de FX que se usan
    if (!cg_blood) cg_blood = _Dvar_RegisterBool("cg_blood", 1, 1u, "Show Blood");
    // Un solo proceso: las balas las resuelve el servidor y manda EV_BULLET_HIT tambien para el mundo (como SP, sin balas de
    // cliente: el cliente local no simula DrawBulletImpacts, que necesita snapshots y centity_s). H5FX_CLIENTBULLETS=1 lo deja.
    if (sv_clientSideBullets && !getenv("H5FX_CLIENTBULLETS")) Dvar_SetBool((dvar_s *)sv_clientSideBullets, false);
    // CG_RegisterGraphics: tabla de impactos del ff
    cgMedia.fx = CG_RegisterImpactEffects_FastFile();
    // R_InitRenderBuffers (r_rendercmds.cpp) si el arranque del harness no lo hizo: codemesh de 0xF000 indices / 0x4000 vertices de 32 bytes
    for (int i = 0; i < 2; ++i) {
        if (!g_codeMesh[i].vb.buffer) { R_InitDynamicMesh(&g_codeMesh[i], 0xF000u, 0x4000u, 0x20u); g_codeMesh[i].vb.verts = (unsigned char *)calloc(1, 0x80000); }
        if (!s_backEndData[i].codeMeshPtr) s_backEndData[i].codeMeshPtr = &g_codeMesh[i];
    }
    if (gfxDrawMethod.emissiveTechType == 0) gfxDrawMethod.emissiveTechType = 5;   // R_InitDrawMethod: TECHNIQUE_EMISSIVE
    if (gfxDrawMethod.baseTechType == 0) gfxDrawMethod.baseTechType = 10;           // R_InitDrawMethod: TECHNIQUE_LIT
    // OLA 7d: R_InitDrawMethod -> R_SetDefaultLitTechTypes. R_AddMarkMeshDrawSurf acepta la marca solo si el material tiene la tecnica
    // litTechType[12][0][0][0] (luz de la marca); con la tabla a cero pedia la tecnica 0 y desechaba marcas validas
    if (!gfxDrawMethod.litTechType[0][0][0][0]) R_SetDefaultLitTechTypes();
    // OLA 7d: marcas. R_InitRenderBuffers (r_rendercmds.cpp): markMesh de 0x2400 indices / 0x1800 vertices de 0x2C bytes por GfxBackEndData.
    // La lista de decals se dibuja con TECHNIQUE_LIT y R_SetTechnique llama a R_SetShadowableLight con la luz primaria de cada drawsurf:
    // data->shadowableLights se rellena como el cliente (R_InitPrimaryLights, refdef->primaryLights) sin mapas de sombra.
    g_marksOn = !(getenv("H7D_MARKS") && !atoi(getenv("H7D_MARKS")));
    g_litFxOn = !(getenv("H7D_LITFX") && !atoi(getenv("H7D_LITFX")));
    if (g_marksOn || g_litFxOn) {
        for (int i = 0; i < 2; ++i) {
            GfxBackEndData *d = &s_backEndData[i];
            if (!d->markMesh.vb.buffer) { R_InitDynamicMesh(&d->markMesh, 0x2400u, 0x1800u, 0x2Cu); d->markMesh.vb.verts = (unsigned char *)calloc(1, 270336); }
            if (rgp.world && !d->shadowableLightCount && rgp.world->primaryLightCount <= 254) {
                R_InitPrimaryLights(d->shadowableLights); d->shadowableLightCount = rgp.world->primaryLightCount; }
            R_UpdateDrawMethod(d);   // primaryLightTechType[surf][fade][charred][luz] desde litTechType (R_SetTechnique, rama TECHNIQUE_LIT)
        }
    }
    if (!g_marksOn && fx_marks) Dvar_SetBool((dvar_s *)fx_marks, false);
    // modelos de FX (casquillos, astillas, sangre): ENCENDIDOS por defecto desde OLA 15B (H7D_FXMODELS=0 los apaga). En OLA 7d costaban
    // 30-70 ms/frame; medido de nuevo en el Mac (long_test_mac.sh 3 min, SCRIPT=2): 0,04 ms/frame de media con 14 superficies/frame y
    // peor frame 0,1 ms, H4_Frame 17,6 ms con ellos frente a 20,8 ms sin ellos (ruido). El coste de 7d no se reproduce con el backend vk
    // actual (pipes=0 en regimen: no se crean pipelines por dibujo); no se pudo aislar que ola lo arreglo. Ver docs/evidencia/h15b.
    g_fxModelsOn = !(getenv("H7D_FXMODELS") && !atoi(getenv("H7D_FXMODELS")));
    g_brassOn = !(getenv("H7D_BRASS") && !atoi(getenv("H7D_BRASS")));
    if (!cg_brass) cg_brass = _Dvar_RegisterBool("cg_brass", 1, 1u, "Weapons eject brass");   // CG_RegisterDvars
    // marcas sobre modelos (smodels/entidades): necesitan FX_GenerateMarkVertsForStaticModels/EntModels con la visibilidad de modelos del
    // render original, que H4 no tiene: se dejan fuera salvo H7D_MARKS_MODELS=1
    if (!(getenv("H7D_MARKS_MODELS") && atoi(getenv("H7D_MARKS_MODELS")))) {
        if (fx_marks_smodels) Dvar_SetBool((dvar_s *)fx_marks_smodels, false);
        if (fx_marks_ents) Dvar_SetBool((dvar_s *)fx_marks_ents, false);
    }
    SortMaterials();
    // floatz (TEXTURE_SRC_CODE_FLOATZ) de las tecnicas zfeather de particulas: en PC el original usa la profundidad de la escena como
    // textura INTZ (R_InitIntZRenderTarget_PC, r_rendertarget.cpp) o un R32F del prepase de profundidad. El harness no tiene ninguno
    // (r_floatz apagado, H4 sin prepase): se pone una imagen R32F a 1.0 (= plano lejano), asi las particulas se ven enteras, sin el
    // fundido suave contra la geometria.
    if (!gfxRenderTargets[R_RENDERTARGET_FLOAT_Z].image) {
        IDirect3DTexture9 *t = nullptr;
        if (dx.device->CreateTexture(4, 4, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &t, 0) == 0 && t) {
            D3DLOCKED_RECT lr;
            // OLA 7b: la floatz del original guarda la profundidad de vista en unidades de mundo (R_RENDERTARGET_FLOAT_Z, prepase
            // TECHNIQUE_BUILD_FLOAT_Z), no un 0..1: con 1.0 todo lo que esta a mas de 1 u de la camara quedaba fundido a cero
            // (fogonazo invisible). H5FX_FLOATZ cambia el valor (por defecto 1e5 u = lejos).
            const float zfar = getenv("H5FX_FLOATZ") ? (float)atof(getenv("H5FX_FLOATZ")) : 100000.0f;
            if (t->LockRect(0, &lr, nullptr, 0) == 0) { for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) ((float *)((char *)lr.pBits + y * lr.Pitch))[x] = zfar; t->UnlockRect(0); }
            g_floatZFar = Image_AllocProg(2, 6u, 0);
            g_floatZFar->texture.basemap = t; g_floatZFar->width = 4; g_floatZFar->height = 4; g_floatZFar->depth = 1; g_floatZFar->mapType = 3;
        }
        fprintf(stderr, "[H5FX] floatz: sin INTZ ni prepase en el harness -> imagen R32F lejana %s\n", g_floatZFar ? "ok" : "NO");
    }
    // OLA 7b: precarga. Los efectos que el GSC precachea (configstrings 2080+i, CG_RegisterGraphics los registra todos al entrar)
    // se registran ya, no al primer EV_PLAY_FX. Los pipelines/shaders de sus materiales los guarda la cache de V9
    // (pipelines.keys + spvc) y el prewarm de la app los crea al arrancar las siguientes veces. H5FX_PRELOAD=0 lo quita.
    if (!(getenv("H5FX_PRELOAD") && !atoi(getenv("H5FX_PRELOAD")))) {
        const double p0 = NowMs(); int n = 0;
        for (int id = 1; id < 196; ++id) n += EffectFor(id) != nullptr;
        fprintf(stderr, "[H5FX] precarga: %d efectos del mapa registrados en %.1f ms\n", n, NowMs() - p0);
    }
    if (const char *t = getenv("H5FX_TEST")) { g_test = FX_Register(t); fprintf(stderr, "[H5FX] prueba '%s' -> %s\n", t, g_test ? "ok" : "NO"); }
    if (g_test && getenv("H5FX_DUMP")) DumpEffect(g_test);   // OLA E2a: elementos y materiales del efecto de prueba
    fprintf(stderr, "[H5FX] FX listos: FxSystem %zu, FxSystemBuffers %zu, FxMarksSystem %zu, FxEffect %zu, FxElem %zu; impactos %s; codemesh vb=%p\n",
        sizeof(FxSystem), sizeof(FxSystemBuffers), sizeof(FxMarksSystem), sizeof(FxEffect), sizeof(FxElem), cgMedia.fx ? cgMedia.fx->name.get() : "NO",
        (void *)g_codeMesh[0].vb.buffer);
    g_ready = true;
}

void PreFrame()
{
    if (g_prevPre) g_prevPre();
    if (!g_init) Init();
    if (!g_ready) return;
    const double t0 = NowMs();
    for (int k = 0; k < 4; ++k) if (g_shotAt[k] >= 0 && g_frames >= g_shotAt[k]) {   // lee el frame anterior (ya presentado)
        char tag[48]; snprintf(tag, sizeof tag, "_fx_%s%d_t%06d", g_shotName[k], g_shotN[k]++, level.time); H4_Shot(tag); g_shotAt[k] = -1; }
    if (!cgArray) cgArray = (cg_s *)calloc(1, sizeof(cg_s));   // h5_weapon.cpp lo reserva al primer viewmodel
    cg_s *cg = cgArray;
    int now = g_h5wViewOverride ? g_h5wViewTime : level.time;
    FxSystemContainer *sys = FX_GetSystem(0);
    if (now < sys->system.msecNow) now = sys->system.msecNow;
    g_fxTime = now;
    cg->time = now;
    // vista: la de H4 (BuildView ya hecho) en refdef, como CG_CalcViewValues
    float org[3], axis[3][3], tx, ty; H4_GetView(org, axis, &tx, &ty);
    memcpy(cg->refdef.vieworg, org, 12); memcpy(cg->refdef.viewaxis, axis, 36);
    cg->refdef.tanHalfFovX = tx; cg->refdef.tanHalfFovY = ty;
    // datos del front end (R_ToggleSmpFrame los deja en el HUD; sin HUD, el primero)
    if (!frontEndDataOut) frontEndDataOut = &s_backEndData[0];
    g_data = frontEndDataOut;
    if (!g_data->codeMeshPtr) g_data->codeMeshPtr = &g_codeMesh[0];
    g_data->codeMeshCount = 0; g_data->codeMeshArgsCount = 0; R_ResetMesh(g_data->codeMeshPtr);
    for (int r = 10; r <= 18; ++r) scene.drawSurfCount[r] = 0;
    for (int r = 6; r <= 9; ++r) scene.drawSurfCount[r] = 0;   // OLA 7d: FX con luz y marcas (R_ToggleSmpFrame/R_InitScene las vacian)
    if (g_data->markMesh.vb.buffer) { g_data->markMeshCount = 0; R_ResetMesh(&g_data->markMesh); }
    // R_RenderScene -> FX_BeginMarks; CG_DrawActiveFrame (cg_view_mp.cpp) -> FX_MarkUpdateClientTime con cg->frametime
    FX_BeginMarks(0);
    FX_MarkUpdateClientTime(0, g_prevNow < 0 ? 0 : now - g_prevNow); g_prevNow = now;
    // CG_DrawActiveFrame: FX_BeginUpdate ... FX_SetNextUpdateTime / FX_SetNextUpdateCamera
    FX_BeginUpdate(0);
    FX_SetNextUpdateTime(0, now);
    FxCameraUpdate cam; FX_GetCameraUpdateFromRefdefAndZFar(&cam, &cg->refdef, 0.0f);
    FX_SetNextUpdateCamera(0, &cam);
    // eventos del frame de servidor nuevo
    g_msReg = 0; const double tEv = NowMs();
    if (svs.time != g_lastSvTime) { g_lastSvTime = svs.time; ServerFrameEvents(); }
    const double msEv = NowMs() - tEv;
    if (g_test && now >= g_testNext) {
        g_testNext = now + 2000;
        float p[3] = { org[0] + axis[0][0] * 120.0f, org[1] + axis[0][1] * 120.0f, org[2] + axis[0][2] * 120.0f };
        float up[3] = { 0, 0, 1 }; PlayAt(g_test, p, up); ArmShot(3, 2);
    }
    // OLA 7d: H7D_MARKTEST=1 (prueba de marcas): cada 1.5 s un impacto del arma del jugador en el punto del mundo que tiene delante
    // (pares) o en el suelo delante de sus pies (impares), con la ruta de EV_BULLET_HIT: CG_ImpactEffectForWeapon (surfType del trazo,
    // (sflags & 0x3F00000) >> 20 como bullet.cpp) + PlayAt. La marca la crea el elemento de marca del efecto (FX_CreateImpactMark).
    static const bool markTest = getenv("H7D_MARKTEST") && atoi(getenv("H7D_MARKTEST"));
    static int mtNext = 0, mtN = 0;
    if (markTest && cgMedia.fx && now >= mtNext && mtN < 16 && g_entities[0].client) {
        mtNext = now + 1500;
        const unsigned weapon = g_entities[0].client->ps.weapon;
        if (weapon && weapon < BG_GetNumWeapons()) {
            // suelo: 25 grados por debajo de la vista (dentro del encuadre); pared: la mas cercana en +-30 grados a la altura de los ojos,
            // con un pequeno desplazamiento por disparo para que las marcas no caigan en el mismo punto (FX_ImpactMark las fundiria)
            const float zero[3] = { 0, 0, 0 };
            const float yaw0 = atan2f(axis[0][1], axis[0][0]), jit = 0.035f * (float)((mtN * 7) % 9 - 4);
            trace_t tr; tr.fraction = 1.0f; float end[3] = { 0, 0, 0 };
            for (int k = 0; k < 13; ++k) {
                const float yw = yaw0 + jit + (k - 6) * 0.087f;
                float dir[3] = { cosf(yw), sinf(yw), (mtN & 1) ? -0.47f : 0.0f }; Vec3Normalize(dir);
                float e[3] = { org[0] + dir[0] * 2000.0f, org[1] + dir[1] * 2000.0f, org[2] + dir[2] * 2000.0f };
                trace_t t; col_context_t ctx(1); CM_BoxTrace(&t, org, e, zero, zero, 1, &ctx);
                const float nz = ((const float *)&t.normal.vec)[2];
                const float hz = org[2] + (e[2] - org[2]) * t.fraction;   // suelo de verdad (no mesas ni sillas): normal hacia arriba y bajo los pies + 8
                const bool ok = (mtN & 1) ? (nz > 0.9f && hz < g_entities[0].client->ps.origin[2] + 8.0f) : fabsf(nz) < 0.3f;
                if (t.fraction < tr.fraction && ok) { tr = t; memcpy(end, e, 12); }
            }
            if (tr.fraction < 1.0f) {
                float hit[3]; for (int k = 0; k < 3; ++k) hit[k] = org[k] + (end[k] - org[k]) * tr.fraction;
                const float *n = (const float *)&tr.normal.vec;
                const int st = (int)(0x3F00000 & tr.sflags) >> 20;
                const FxEffectDef *fx = nullptr; unsigned snd = 0; CG_ImpactEffectForWeapon(weapon, st, 0, &fx, &snd);
                if (fx) { PlayAt(fx, hit, n); ++g_evImpact; if (mtN >= 4) ArmShot(3, 2); }
                fprintf(stderr, "[H7D] prueba de marca %d (%s): %.1f %.1f %.1f normal %.2f %.2f %.2f surf %d -> %s\n", mtN, (mtN & 1) ? "suelo" : "pared",
                        hit[0], hit[1], hit[2], n[0], n[1], n[2], st, fx ? fx->name.get() : "(sin efecto)");
            }
        }
        ++mtN;
    }
    // R_ProcessCmd_UpdateFxNonDependent / UpdateFxRemaining / R_DrawEffects (r_workercmds_common.cpp), en el acto (un hilo)
    const double tUp = NowMs();
    FxCmd cmd; FX_FillUpdateCmd(0, &cmd);
    FX_UpdateNonDependent(&cmd);
    FX_UpdateRemaining(&cmd);
    // OLA 7d: R_ProcessCmd_AddNonSpriteDrawSurfs -> FX_AddNonSpriteDrawSurfs (modelos y luces de FX), antes de FX_EndUpdate como en el original
    g_fxModels.clear();
    // FX_DrawElem_Light (luces de FX: fogonazo, explosiones) llama a R_AddOmniLightToScene, que exige rg.inFrame (R_BeginFrame) y las
    // deja en scene.addedLight (max 32). H4 no tiene R_BeginFrame: se marca el frame solo durante esta llamada y la lista se vacia
    // cada frame; se cuentan ([H7D] luces FX) pero H4 aun no las aplica a la escena.
    if (g_fxModelsOn) {
        scene.addedLightCount = 0;
        const bool inF = rg.inFrame; rg.inFrame = true;
        FX_AddNonSpriteDrawSurfs(&cmd);
        rg.inFrame = inF; g_fxLights += scene.addedLightCount;
    }
    FX_EndUpdate(0);
    const double tGen = NowMs();
    FxGenerateVertsCmd gen; FX_FillGenerateVertsCmd(0, &gen);
    gen.useReflection = cmd.useReflection; gen.reflectionHeight = cmd.reflectionHeight; gen.genVertsCameraType = cmd.updateCameraType;
    FX_GenerateVerts(&gen);
    // OLA 7d: R_ProcessCmd_GenerateMarkVerts (r_workercmds.cpp) -> FX_GenerateMarkVertsForWorld (marcas del mundo -> markMesh y regiones
    // 6..8). Sin luces dinamicas visibles (el harness no tiene dlights): visibleLights = 0.
    const double tMk = NowMs();
    if (g_marksOn && g_data->markMesh.vb.buffer) { FX_GenerateMarkVertsForWorld(0, nullptr, 0); g_markMeshes += g_data->markMeshCount; }
    g_msMarks += NowMs() - tMk;
    ::H12_GlassGenerateVerts();   // OLA 12: fragmentos de cristal (GlassCl_GenerateVerts -> codemesh, region 9)
    // RB_UpdateDynamicBuffers (rb_backend.cpp): codemesh y markMesh
    GfxMeshData *meshes[2] = { g_data->codeMeshPtr, &g_data->markMesh };
    for (GfxMeshData *mesh : meshes) if (mesh->vb.buffer && mesh->vb.used) {
        const unsigned used = mesh->vb.used >= mesh->vb.total ? mesh->vb.total : mesh->vb.used;
        void *dst = R_LockVertexBuffer(mesh->vb.buffer, 0, used, 0x2000u);
        if (dst) { memcpy(dst, mesh->vb.verts, used); R_UnlockVertexBuffer(mesh->vb.buffer); }
    }
    const double dt = NowMs() - t0; g_msUpd += dt; if (dt > g_msMax) g_msMax = dt;
    if (dt > 8.0 && StatsOn() && g_hitchLogs++ < 40)   // tirones: de donde sale el tiempo
        fprintf(stderr, "[H5FX] tiron %.1f ms: eventos %.1f (registro %.1f), update %.1f, verts %.1f\n", dt, msEv, g_msReg, tGen - tUp, NowMs() - tGen);
}

// Materiales del fogonazo de vista del arma actual: en el original el viewmodel se pinta en la escena (con su particion de
// profundidad) antes de la lista emisiva, asi que el fogonazo queda delante del canon. H4 pinta el viewmodel DESPUES de los FX
// y limpia la profundidad: esas superficies se apartan y se pintan tras el viewmodel (DrawLate), el resto con la del mundo.
void CollectFlashMaterials(unsigned weapon)
{
    if (weapon == g_flashWeapon) return;
    g_flashWeapon = weapon; g_flashMtlCount = 0;
    if (!weapon || weapon >= BG_GetNumWeapons()) return;
    const FxEffectDef *fx = BG_GetWeaponDef(weapon)->viewFlashEffect;
    if (!fx) return;
    const int n = fx->elemDefCountLooping + fx->elemDefCountOneShot + fx->elemDefCountEmission;
    for (int i = 0; i < n; ++i) {
        const FxElemDef *el = &fx->elemDefs.get()[i];
        if (el->elemType > 5u) continue;   // solo sprites/billboards (FX_ELEM_TYPE_LAST_SPRITE): materiales
        for (int k = 0; k < el->visualCount && g_flashMtlCount < 32; ++k)
            g_flashMtl[g_flashMtlCount++] = el->visualCount > 1 ? el->visuals.array.get()[k].anonymous.get() : el->visuals.instance.anonymous.get();
    }
}
void DumpMaterial(const Material *m)   // H5FX_DUMP: tecnica emisiva y argumentos del pixel shader
{
    const MaterialTechniqueSet *ts = m->techniqueSet.get();
    const MaterialTechnique *t = ts ? ts->techniques[gfxDrawMethod.emissiveTechType].get() : nullptr;
    fprintf(stderr, "[H5FX] mat %s techset %s tech %s\n", m->info.name.get(), ts ? ts->name.get() : "-", t ? t->name.get() : "-");
    if (!t) return;
    for (int p = 0; p < t->passCount; ++p) {
        const MaterialPass *ps = &t->passArray[p];
        fprintf(stderr, "[H5FX]   pase %d ps %s vs %s args", p, ps->pixelShader.get() ? ps->pixelShader.get()->name.get() : "-", ps->vertexShader.get() ? ps->vertexShader.get()->name.get() : "-");
        const int n = ps->perPrimArgCount + ps->perObjArgCount + ps->stableArgCount;
        for (int a = 0; a < n; ++a) { const MaterialShaderArgument *ar = &ps->args.get()[a]; fprintf(stderr, " [t%u d%u c%u]", ar->type, ar->dest, ar->u.codeConst.index); }
        fprintf(stderr, "\n");
    }
}
void DumpEffect(const FxEffectDef *fx)
{
    const int n = fx->elemDefCountLooping + fx->elemDefCountOneShot + fx->elemDefCountEmission;
    for (int i = 0; i < n; ++i) {
        const FxElemDef *el = &fx->elemDefs.get()[i];
        if (el->elemType > 5u) { fprintf(stderr, "[H5FX]  elem %d tipo %u flags %08x\n", i, el->elemType, (unsigned)el->flags); continue; }
        for (int k = 0; k < el->visualCount; ++k) {
            const Material *m = (const Material *)(el->visualCount > 1 ? el->visuals.array.get()[k].anonymous.get() : el->visuals.instance.anonymous.get());
            const FxElemVisStateSample *vs = el->visSamples.get();
            fprintf(stderr, "[H5FX]  elem %d tipo %u flags %08x luz %u color0 %u,%u,%u,%u: ", i, el->elemType, (unsigned)el->flags, el->lightingFrac,
                    vs ? vs->base.color[0] : 0, vs ? vs->base.color[1] : 0, vs ? vs->base.color[2] : 0, vs ? vs->base.color[3] : 0); if (m) DumpMaterial(m); else fprintf(stderr, "(sin material)\n");
        }
    }
}
bool IsFlashSurf(const GfxDrawSurf &d)
{
    const void *m = rgp.sortedMaterials[d.fields.materialSortedIndex];
    for (int i = 0; i < g_flashMtlCount; ++i) if (g_flashMtl[i] == m) {
        static const void *dumped[32]; static int nd;
        if (getenv("H5FX_DUMP") && nd < 32) { bool seen = false; for (int k = 0; k < nd; ++k) seen |= dumped[k] == m; if (!seen) { dumped[nd++] = m; DumpMaterial((const Material *)m); } }
        return true; }
    return false;
}
bool g_noLate = getenv("H5FX_NOLATE") != nullptr;   // diagnostico: fogonazo en la pasada del mundo
GfxDrawSurf g_late[512]; unsigned g_lateCount, g_lateTot;

// texturas de codigo que el harness no rellena (RB_InitCodeImages/RB_SetBspImages las ponen en el original):
// floatz -> RT o imagen lejana; outdoor -> la del BSP (RB_SetBspImages) o blanco (exterior); el resto -> blanco/negro
void FillCodeImages(GfxCmdBufSourceState *src, const GfxImage **savedCode)
{
    for (int i = 0; i < TEXTURE_SRC_CODE_COUNT; ++i) {
        savedCode[i] = src->input.codeImages[i];
        if (savedCode[i]) continue;
        const GfxImage *fb;
        switch (i) {
        case TEXTURE_SRC_CODE_FLOATZ: fb = gfxRenderTargets[R_RENDERTARGET_FLOAT_Z].image ? gfxRenderTargets[R_RENDERTARGET_FLOAT_Z].image : g_floatZFar; break;
        case TEXTURE_SRC_CODE_OUTDOOR: fb = (rgp.world && rgp.world->outdoorImage.get()) ? (const GfxImage *)rgp.world->outdoorImage.get() : rgp.whiteImage; break;
        case TEXTURE_SRC_CODE_RESOLVED_SCENE: case TEXTURE_SRC_CODE_RESOLVED_POST_SUN: case TEXTURE_SRC_CODE_FEEDBACK:
        case TEXTURE_SRC_CODE_POST_EFFECT_SRC: case TEXTURE_SRC_CODE_POST_EFFECT_0: case TEXTURE_SRC_CODE_POST_EFFECT_1:
            fb = rgp.blackImage; break;
        default: fb = rgp.whiteImage; break;
        }
        src->input.codeImages[i] = fb;
        if (!src->input.codeImageSamplerStates[i]) src->input.codeImageSamplerStates[i] = 98;
    }
}

// bucle de R_DrawSurfs sobre [first, first+count) de g_data->drawSurfs con las texturas de codigo que falten
void DrawList(unsigned first, unsigned count, unsigned char baseTechType = 0xFF)
{
    GfxCmdBufContext ctx = gfxCmdBufContext;
    GfxCmdBufSourceState *src = ctx.source;
    const GfxBackEndData *savedData = src->input.data;
    src->input.data = g_data;
    // texturas de codigo que el harness no rellena (RB_InitCodeImages/RB_SetBspImages las ponen en el original):
    // floatz -> RT o imagen lejana; outdoor -> la del BSP (RB_SetBspImages) o blanco (exterior); el resto -> blanco/negro
    const GfxImage *savedCode[TEXTURE_SRC_CODE_COUNT];
    FillCodeImages(src, savedCode);
    GfxDrawSurfListInfo info; memset(&info, 0, sizeof info); R_InitDrawSurfListInfo(&info);   // la del original vive en GfxViewInfo (a cero)
    info.drawSurfs = &g_data->drawSurfs[first];
    info.drawSurfCount = count;
    info.baseTechType = baseTechType == 0xFF ? gfxDrawMethod.emissiveTechType : baseTechType;
    float org[3], axis[3][3], tx, ty; H4_GetView(org, axis, &tx, &ty);
    info.viewOrigin[0] = org[0]; info.viewOrigin[1] = org[1]; info.viewOrigin[2] = org[2]; info.viewOrigin[3] = 1.0f;
    info.cameraView = src->cameraView;
    // sin R_Set3D (H4 coloca el mundo con su eyeOffset: viewMode 3D solo durante la lista, como h5_light.cpp)
    const GfxViewMode vm = src->viewMode; src->viewMode = VIEW_MODE_3D;
    ctx.state->origMaterial = 0;
    GfxCmdBufContext none; none.source = 0; none.state = 0;
    GfxDrawSurfListArgs la; la.context = ctx; la.firstDrawSurfIndex = 0; la.info = &info;
    while (la.firstDrawSurfIndex != info.drawSurfCount) la.firstDrawSurfIndex += R_RenderDrawSurfListMaterial(&la, none);
    R_TessEnd(ctx, none);
    ctx.state->origMaterial = 0;
    src->viewMode = vm;
    src->input.data = savedData;
    for (int i = 0; i < TEXTURE_SRC_CODE_COUNT; ++i) src->input.codeImages[i] = savedCode[i];
}

// modelos de FX rigidos: esqueleto con la colocacion (quat + origen, escala) como R_SkinXModel de un modelo sin animar
void DrawFxModels(unsigned char techType)
{
    const double t0 = NowMs();
    static DObjSkelMat *skel; static GfxPackedVertex *tmp; static int tmpCap;
    if (!skel) { void *p = nullptr; posix_memalign(&p, 16, 128 * sizeof(DObjSkelMat)); skel = (DObjSkelMat *)p; }
    float cam[3], ax[3][3], tx, ty; H4_GetView(cam, ax, &tx, &ty);
    int surfs = 0;
    GfxCmdBufSourceState *src = gfxCmdBufContext.source; const GfxImage *savedCode[TEXTURE_SRC_CODE_COUNT]; FillCodeImages(src, savedCode);   // floatz de materiales de FX
    for (const FxModelInst &fm : g_fxModels) {
        XModel *model = (XModel *)fm.model;
        { static const void *seen[16]; static int ns; bool k = false; for (int i = 0; i < ns; ++i) k |= seen[i] == model;
          if (!k && ns < 16) { seen[ns++] = model; fprintf(stderr, "[H7D] modelo FX: %s (%d huesos, escala %.2f)\n", model->name.get(), XModelNumBones(model), fm.pl.scale); } }
        const float sc = fm.pl.scale > 0 ? fm.pl.scale : 1.0f, dist = Vec3Distance(fm.pl.base.origin, cam);
        const int lod = XModelGetLodForDist(model, dist / sc, dist / sc, false); if (lod < 0) continue;
        const int nb = XModelNumBones(model); if (nb > 128) continue;
        float axis[3][3]; UnitQuatToAxis(fm.pl.base.quat, axis);
        for (int j = 0; j < nb; ++j) { DObjSkelMat &o = skel[j];
            for (int a = 0; a < 3; ++a) { for (int b = 0; b < 3; ++b) o.axis[a][b] = axis[a][b] * sc; o.axis[a][3] = 0; }
            memcpy(o.origin, fm.pl.base.origin, 12); o.origin[3] = 1.0f; }
        XSurface *xsurfs; const int ns = XModelGetSurfaces(model, &xsurfs, lod);
        PTR32(Material) *skins = XModelGetSkins(model, lod);
        for (int k = 0; k < ns; ++k) {
            XSurface *xs = &xsurfs[k]; Material *mtl = skins ? skins[k].get() : nullptr;
            if (!mtl || !xs->vertCount) continue;
            // OLA E2a: decals proyectados (techset mc_projected_decal*, sin tecnica lit): la malla es una caja que el original proyecta sobre
            // la profundidad de la escena. Sin esa proyeccion se pintaba la caja oscura (crater fx_decal_zombie_crater_dirt de la aparicion
            // del Shrieker de Shangri-La, escala 16-29). Se omiten. H7D_PROJDECAL=1 los vuelve a dibujar (diagnostico).
            { static const bool pd = getenv("H7D_PROJDECAL") && atoi(getenv("H7D_PROJDECAL")); const MaterialTechniqueSet *ts = mtl->techniqueSet.get();
              if (!pd && ts && strstr(ts->name.get(), "projected_decal")) { ++g_projDecalSkipped; continue; } }
            if ((int)xs->vertCount > tmpCap) { free(tmp); tmpCap = xs->vertCount + 256; void *p = nullptr; posix_memalign(&p, 16, tmpCap * sizeof(GfxPackedVertex)); tmp = (GfxPackedVertex *)p; }
            const double ts = NowMs();
            R_SkinXSurfaceSkinned(xs, skel, tmp);
            g_msModelSkin += NowMs() - ts;
            H4_DrawSkinnedSurf(mtl, xs, tmp, techType); ++surfs; g_modelVerts += xs->vertCount;
        }
    }
    if (tess.indexCount) RB_EndTessSurface();
    for (int i = 0; i < TEXTURE_SRC_CODE_COUNT; ++i) src->input.codeImages[i] = savedCode[i];
    g_fxModelSurfs += surfs; if (surfs > g_fxModelMax) g_fxModelMax = surfs;
    { const double dt = NowMs() - t0; g_msModels += dt; if (dt > g_msModelMax) g_msModelMax = dt; }
}

} // namespace (se reabre abajo)

// OLA 7d: prepase floatz como R_DepthPrepass/R_DepthPrepassCallback (rb_depthprepass.cpp) con renderingFloatZ: render target R32F
// FLOAT_Z (R_InitFullscreenRenderTargetImage 2, como R_InitRenderTargets_PC sin INTZ), clear blanco + quad shadowclear, y el mundo con
// la tecnica 1 (TECHNIQUE_BUILD_FLOAT_Z) y CONST_SRC_CODE_DEPTH_FROM_CLIP = (0,0,0,1): profundidad de vista en unidades de mundo.
// Solo el BSP (sin smodels/actores: no tapan humo de forma visible y costarian otra pasada). H7D_FLOATZ=0 vuelve a la imagen lejana.
void __cdecl R_SetCodeConstant(GfxCmdBufSourceState *source, CodeConstant constant, float x, float y, float z, float w);   // r_foliage.cpp
namespace { int g_fzState; double g_msFloatZ; int g_fzFrames; GfxQuadMeshData g_fzQuad; }
void H7D_FloatZPrepass(void (*drawWorld)(unsigned char, int, int), int W, int H)
{
    if (g_fzState < 0) return;
    if (!g_fzState) {
        const char *e = getenv("H7D_FLOATZ"); if (e && !atoi(e)) { g_fzState = -1; return; }
        // la imagen lejana de Init ocupa la imagen de programa 2 (la misma que usa este render target): se pasa a una copia propia
        if (g_floatZFar && g_floatZFar->texture.basemap) { GfxImage *c = (GfxImage *)malloc(sizeof(GfxImage)); memcpy(c, g_floatZFar, sizeof(GfxImage)); g_floatZFar->texture.basemap = nullptr; g_floatZFar = c; }
        if (!gfxRenderTargets[R_RENDERTARGET_FLOAT_Z].surface.color)
            R_InitFullscreenRenderTargetImage(2, FULLSCREEN_SCENE, 0, D3DFMT_R32F, RENDERTARGET_USAGE_RENDER, &gfxRenderTargets[R_RENDERTARGET_FLOAT_Z]);
        GfxRenderTarget &rt = gfxRenderTargets[R_RENDERTARGET_FLOAT_Z];
        if (!rt.surface.color || !rt.image) { fprintf(stderr, "[H7D] floatz: no se pudo crear el R32F, sigue la imagen lejana\n"); rt.image = nullptr; g_fzState = -1; return; }
        // Con R_AllocateMinimalResources el FRAME_BUFFER tiene depthStencil nulo (usa el depth automatico del dispositivo). Si este RT
        // lleva el depth del dispositivo y el FRAME_BUFFER no, al volver R_HW_SetRenderTarget hace SetDepthStencilSurface(NULL) y el
        // pase principal se queda SIN depth (paredes tapadas por lo que se dibuja despues). Se asigna el depth del dispositivo al
        // FRAME_BUFFER (y SCENE) igual que h7_gfx.cpp antes del bloom, y el mismo al FLOAT_Z (en el juego tambien comparte el de la escena).
        for (GfxRenderTargetId id : { R_RENDERTARGET_FRAME_BUFFER, R_RENDERTARGET_SCENE, R_RENDERTARGET_FLOAT_Z })
            if (!gfxRenderTargets[id].surface.depthStencil) { IDirect3DSurface9 *ds = nullptr; dx.device->GetDepthStencilSurface(&ds); gfxRenderTargets[id].surface.depthStencil = ds; }
        int with1 = 0, tot = 0; const GfxWorld *w = rgp.world;
        for (unsigned i = 0; w && i < (unsigned)w->materialMemoryCount; ++i) { Material *m = w->materialMemory[i].material.get(); if (!m) continue; ++tot; if (Material_GetTechnique(m, 1)) ++with1; }
        const MaterialTechnique *qt = rgp.shadowClearMaterial ? Material_GetTechnique(rgp.shadowClearMaterial, 4) : nullptr;
        fprintf(stderr, "[H7D] floatz: prepase R32F %ux%u = %.1f MB (+ depth compartido), materiales del mundo con build_floatz %d/%d, quad shadowclear %s\n",
            rt.width, rt.height, rt.width * rt.height * 4 / 1048576.0, with1, tot, qt ? qt->name.get() : "NO");
        R_InitDynamicMesh(&g_fzQuad.meshData, 6u, 4u, 0x20u); R_SetQuadMesh(&g_fzQuad, 0.0f, 0.0f, (float)rt.width, (float)rt.height, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF);
        g_fzState = 1;
    }
    static const int dbg = getenv("H7D_FZDBG") ? atoi(getenv("H7D_FZDBG")) : 0;   // diagnostico: 1 sin clear, 2 sin quad, 4 sin mundo, 8 nada por frame, 16 sin constante
    if (dbg & 8) return;
    const double t0 = NowMs();
    GfxRenderTarget &rt = gfxRenderTargets[R_RENDERTARGET_FLOAT_Z];
    GfxCmdBufContext ctx = gfxCmdBufContext;
    R_SetRenderTargetSize(ctx.source, R_RENDERTARGET_FLOAT_Z);
    R_SetRenderTarget(ctx, R_RENDERTARGET_FLOAT_Z);
    if (!(dbg & 1)) R_ClearScreen(ctx.state->prim.device, 7, colorWhite, 1.0f, 0, 0);
    if (rgp.shadowClearMaterial && !(dbg & 2)) {
        GfxViewport vpt = { 0, 0, (int)rt.width, (int)rt.height }; R_SetViewportStruct(ctx.source, &vpt);
        R_DrawQuadMesh(ctx, rgp.shadowClearMaterial, &g_fzQuad.meshData);
    }
    if (!(dbg & 16)) R_SetCodeConstant(ctx.source, CONST_SRC_CODE_DEPTH_FROM_CLIP, 0.0f, 0.0f, 0.0f, 1.0f);
    if (!(dbg & 4)) drawWorld(1, W, H);
    if (tess.indexCount) RB_EndTessSurface();
    g_msFloatZ += NowMs() - t0;
    if (++g_fzFrames % 600 == 0) { fprintf(stderr, "[H7D] floatz: 600 frames, prepase %.3f ms/frame (CPU)\n", g_msFloatZ / 600); g_msFloatZ = 0; }
}
namespace {

void Draw(unsigned char techType)
{
    if (g_prevDraw) g_prevDraw(techType);
    g_lateCount = 0;
    if (!g_ready || !g_data) return;
    // OLA 7d: lista de decals (R_FinishDecalAndEmissiveDrawSurfs: orden de FX_CAMERA_LIT y CODEMESH_CAMERA_LIT, fusion de las regiones
    // 3..9 = decals del mundo/modelos, FX con luz y marcas, y R_DrawDecal con gfxDrawMethod.baseTechType). Va antes de la emisiva.
    // Las regiones 3..5 (decals BSP/smodel/ent) no las llena H4: los decals del mapa los pinta world_h4 en sus lotes (orden 7g).
    if (!g_fxModels.empty()) DrawFxModels(techType);
    if (g_litFxOn || g_marksOn) {
        const double tl = NowMs();
        R_SortDrawSurfs(scene.drawSurfs[6], scene.drawSurfCount[6]);
        R_SortDrawSurfs(scene.drawSurfs[9], scene.drawSurfCount[9]);
        const unsigned firstLit = g_data->drawSurfCount;
        R_MergeAndEmitDrawSurfLists(3u, 7u, g_data);
        const unsigned nLit = g_data->drawSurfCount - firstLit;
        if (nLit) { DrawList(firstLit, nLit, gfxDrawMethod.baseTechType); g_litSurfs += nLit; if ((int)nLit > g_markMax) g_markMax = nLit; }
        g_data->drawSurfCount = firstLit;
        g_msMarks += NowMs() - tl;
    }
    if (!scene.drawSurfCount[13]) return;
    const double t0 = NowMs();
    const gentity_s *pl = &g_entities[0];
    CollectFlashMaterials(pl->r.inuse && pl->client ? pl->client->ps.weapon : 0u);
    // R_FinishDecalAndEmissiveDrawSurfs (r_scene.cpp): orden de la region FX emisiva y fusion de las listas 10..15
    R_SortDrawSurfs(scene.drawSurfs[13], scene.drawSurfCount[13]);
    const unsigned first = g_data->drawSurfCount;
    R_MergeAndEmitDrawSurfLists(0xAu, 6u, g_data);
    unsigned n = 0;   // particion estable: mundo delante, fogonazo aparte (conserva el orden de R_SortDrawSurfs)
    for (unsigned i = first; i < g_data->drawSurfCount; ++i) {
        const GfxDrawSurf d = g_data->drawSurfs[i];
        if (g_flashMtlCount && g_lateCount < 512 && !g_noLate && IsFlashSurf(d)) g_late[g_lateCount++] = d;
        else g_data->drawSurfs[first + n++] = d;
    }
    g_surfs += n + g_lateCount; if ((int)(n + g_lateCount) > g_maxSurfs) g_maxSurfs = n + g_lateCount;
    // OLA E2a: los FX con distorsion (calor, ondas: tecnica emisiva con MTL_TECHFLAG_NEEDS_RESOLVED_SCENE/POST_SUN = 2/1) muestrean la
    // escena que R_ResolveDistortion (rb_draw3d.cpp) copia antes de R_DrawEmissive. Con el negro de FillCodeImages oscurecian lo que
    // tapan (aparicion del Shrieker de Shangri-La, fx_ztem_sonic_zombie_spawn: la mano se veia negra) y resolver aqui la escena a
    // RESOLVED_POST_SUN tampoco dio el color bueno, asi que se omiten: sin la ondulacion, el resto del efecto (anillos, polvo, humo)
    // se ve igual. H5FX_DISTORT=1 los vuelve a dibujar (diagnostico).
    static const bool distOn = getenv("H5FX_DISTORT") && atoi(getenv("H5FX_DISTORT"));
    if (!distOn) { unsigned m = 0;
        for (unsigned i = first; i < first + n; ++i) {
            const GfxDrawSurf d = g_data->drawSurfs[i];
            const MaterialTechnique *t = Material_GetTechnique(rgp.sortedMaterials[d.fields.materialSortedIndex], gfxDrawMethod.emissiveTechType);
            if (t && (t->flags & 3)) { ++g_distSkipped; continue; }
            g_data->drawSurfs[first + m++] = d;
        }
        n = m; }
    if (n) { DrawList(first, n); ShotIfDrawn(1); ShotIfDrawn(2); if (g_noLate) ShotIfDrawn(3); }
    g_data->drawSurfCount = first;
    const double dd = NowMs() - t0; g_msDraw += dd;
    if (dd > 8.0 && StatsOn() && g_hitchLogs++ < 40) fprintf(stderr, "[H5FX] tiron dibujo %.1f ms (%u sup)\n", dd, n);
}

// tras el viewmodel (su profundidad): el fogonazo de vista
void DrawLate(unsigned char techType)
{
    if (g_prevLate) g_prevLate(techType);
    if (!g_ready || !g_data || !g_lateCount) return;
    const double t0 = NowMs();
    const unsigned first = g_data->drawSurfCount;
    memcpy(&g_data->drawSurfs[first], g_late, g_lateCount * sizeof(GfxDrawSurf));
    DrawList(first, g_lateCount); ShotIfDrawn(0); ShotIfDrawn(3);
    g_lateTot += g_lateCount; g_lateCount = 0;
    g_msDraw += NowMs() - t0;
}

void Stats()
{
    if (++g_frames % 600 || !StatsOn()) return;
    fprintf(stderr, "[H5FX] 600 frames: actualizacion %.3f ms/frame (max %.3f), dibujo %.3f ms/frame, superficies %d/frame (max %d); "
        "eventos fogonazo %d impacto %d fx %d fx_tag %d; codemesh %u, efectos activos %d, camara %d, fogonazo tras viewmodel %u sup, superficies de distorsion omitidas %d, decals proyectados omitidos %d\n",
        g_msUpd / 600, g_msMax, g_msDraw / 600, g_surfs / 600, g_maxSurfs, g_evFlash, g_evImpact, g_evFx, g_evFxTag, g_data ? g_data->codeMeshCount : 0u,
        (int)(FX_GetSystem(0)->shared.firstNewEffect - FX_GetSystem(0)->shared.firstActiveEffect), (int)FX_GetSystem(0)->system.camera.isValid, g_lateTot, g_distSkipped, g_projDecalSkipped);
    {   // OLA 7d: marcas (FxMarksSystem: activas = las de la lista del mundo) y lista de decals/FX con luz
        FxMarksSystem *ms = FX_GetMarksSystem(0); int nm = 0;
        for (unsigned short h = ms->firstActiveWorldMarkHandle; h != 0xFFFF && nm < 100000; h = FX_MarkFromHandle(ms, h)->nextMark) ++nm;
        fprintf(stderr, "[H7D] 600 frames: marcas del mundo %d, markMesh %d/frame, decals+FX con luz %d sup/frame (max %d), marcas+lista %.3f ms/frame; "
            "modelos FX %d sup/frame (max %d, %d verts/frame) %.3f ms/frame (deformado %.3f, peor frame %.1f), casquillos %d, luces FX %d (no aplicadas)\n",
            nm, g_markMeshes / 600, g_litSurfs / 600, g_markMax, g_msMarks / 600, g_fxModelSurfs / 600, g_fxModelMax, g_modelVerts / 600, g_msModels / 600, g_msModelSkin / 600, g_msModelMax, g_evBrass, g_fxLights);
        g_markMeshes = g_litSurfs = g_markMax = 0; g_msMarks = 0; g_fxModelSurfs = g_fxModelMax = g_fxLights = g_modelVerts = 0; g_msModels = g_msModelSkin = g_msModelMax = 0; }
    g_lateTot = 0; g_msUpd = g_msDraw = g_msMax = 0; g_surfs = g_maxSurfs = 0;
}
void PreFrameWithStats() { PreFrame(); Stats(); }
} // namespace

// CG_IsMature (cg_main_mp.cpp): contenido maduro permitido (el PC sin censura regional); sin esto FX_DrawElement y
// FX_SpawnElem descartan los elementos de sangre (elemDef->flags < 0)
bool __cdecl CG_IsMature() { return true; }

// R_FilterXModelIntoScene (r_dpvs.cpp): en el original mete el modelo en la escena (celdas, luz de modelo cacheada por
// cachedLightingHandle) y lo dibuja la lista de entidades. H4 no tiene esa escena: se guarda para DrawFxModels.
void __cdecl R_FilterXModelIntoScene(const XModel *model, const GfxScaledPlacement *placement, unsigned __int16 renderFxFlags,
                                     unsigned __int16 *cachedLightingHandle, float lightToleranceSq)
{
    (void)renderFxFlags; (void)cachedLightingHandle; (void)lightToleranceSq;
    if (model && placement && g_fxModels.size() < 256) g_fxModels.push_back({ model, *placement });
}

// ---- pegamento de cliente para el viewmodel (handle 1536): cg_ents_mp.cpp / cg_main_mp.cpp sin centity_s ----
const cpose_t *__cdecl CG_GetPose(int localClientNum, unsigned int handle)
{
    (void)handle;   // sin cg_entities: solo el viewmodel tiene DObj de cliente en el harness
    return &CG_GetLocalClientGlobals(localClientNum)->viewModelPose;
}
// El esqueleto del viewmodel (h5_weapon.cpp) esta en espacio de modelo: se pasa a mundo con cg->viewModelAxis (CG_AddPlayerWeapon)
int __cdecl CG_DObjGetWorldBoneMatrix(const cpose_t *pose, DObj *obj, int boneIndex, float (*tagMat)[3], float *origin)
{
    (void)pose;
    DObjAnimMat *mats = DObjGetRotTransArray(obj);
    if (!mats || !cgArray) return 0;
    float local[3][3]; ConvertQuatToMat(&mats[boneIndex], local);
    const float (*a)[3] = cgArray->viewModelAxis;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) tagMat[i][j] = local[i][0] * a[0][j] + local[i][1] * a[1][j] + local[i][2] * a[2][j];
    const float *t = mats[boneIndex].trans;
    for (int j = 0; j < 3; ++j) origin[j] = t[0] * a[0][j] + t[1] * a[1][j] + t[2] * a[2][j] + a[3][j];
    return 1;
}
void __cdecl CG_GetDObjOrientation(int localClientNum, int dobjHandle, float (*axis)[3], float *origin)
{
    (void)dobjHandle;
    const cg_s *cg = CG_GetLocalClientGlobals(localClientNum);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) axis[i][j] = cg->viewModelAxis[i][j];
    for (int j = 0; j < 3; ++j) origin[j] = cg->viewModelAxis[3][j];
}

// h5_game.cpp: el dibujo se engancha ANTES de instalar el viewmodel (queda entre actores y viewmodel); el PreFrame DESPUES de
// H5L_Install (que pone g_h4PreFrame sin encadenar).
void H5FX_InstallDraw()
{
    g_on = !(getenv("H5FX_OFF") && atoi(getenv("H5FX_OFF")));
    if (!g_on) return;
    g_prevDraw = g_h4DrawExtra;
    g_h4DrawExtra = Draw;
}
void H5FX_InstallPre()
{
    if (!g_on) return;
    g_prevPre = g_h4PreFrame;
    g_h4PreFrame = PreFrameWithStats;
    g_prevLate = g_h4DrawExtra;   // ya con el viewmodel instalado: el fogonazo de vista va despues
    g_h4DrawExtra = DrawLate;
    fprintf(stderr, "[H5FX] efectos activados (EffectsCore original + codemesh emisivo)\n");
}

// OLA E2a: CG_ShakeCamera sobre los angulos de la vista (h5_play.cpp la llama antes de dibujar): la sacudida de mayor tamano manda
// OLA E2b: Earthquake() de un script de cliente (CScr_Earthquake -> CG_StartShakeCamera), mismo hueco que EV_EARTHQUAKE
void H5FX_Earthquake(float scale, float seconds, const float *org, float radius)
{
    Shake k{}; k.scale = scale; k.length = seconds * 1000.0f; k.radius = radius; memcpy(k.src, org, 12); k.time = level.time;
    if (k.scale <= 0 || k.length <= 0 || k.radius <= 0) return;
    float o[3], a[3]; H4_GetCam(o, a); UpdateShake(k, o, k.time);
    int i = 0; for (; i < 4; ++i) if (g_shakes[i].time > level.time || (float)level.time >= g_shakes[i].time + g_shakes[i].length) break;
    if (i == 4) { float mn = k.size; for (int j = 0; j < 4; ++j) if (mn > g_shakes[j].size) { mn = g_shakes[j].size; i = j; } }
    if (i != 4) g_shakes[i] = k;
}
bool H5FX_ShakeCamera(const float *org, float *ang, int time)
{
    float scale = 0, rumble = 0;
    for (Shake &k : g_shakes) if (UpdateShake(k, org, time) && k.size > scale) { scale = k.size; rumble = k.rumble; }
    if (scale <= 0) { g_shakePhase = crandom() * 3.1415927f; return false; }
    if (scale > 1) scale = 1;
    const float sx = (float)time / 600.0f;
    ang[0] += sinf(25.132742f * sx + g_shakePhase) * rumble * 18.0f * scale;
    ang[1] += sinf(47.12389f * sx + g_shakePhase) * rumble * 16.0f * scale;
    ang[2] += sinf(37.699112f * sx + g_shakePhase) * rumble * 10.0f * scale;
    static int s_logT = -100000; if (time - s_logT >= 1000) { s_logT = time;   // una linea por segundo mientras tiembla
        fprintf(stderr, "[H5FX] sacudida t=%d tamano %.3f rumble %.3f: hasta %.2f/%.2f/%.2f grados\n", time, scale, rumble, 18.0f * rumble * scale, 16.0f * rumble * scale, 10.0f * rumble * scale); }
    return true;
}
