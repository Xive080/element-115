// H5a: servidor ORIGINAL (server_mp/game_mp/game/game_sp) en modo dedicado local dentro del harness (H5G=1, H5_GAME=1).
// Arranque como Com_Init (common.cpp) en el orden real pero solo con lo que el servidor necesita: Cbuf/Cmd, Com_InitDvars
// (extraido tal cual de qcommon/common.cpp en el build: gen/h5_cominitdvars.inc), VM de scripts, XAnim/DObj, SV_Init y luego
// SV_SpawnServer(mapa, mapIsPreloaded=1: las zonas ya las cargo main2 con las banderas reales) y el bucle de SV_Frame.
//   H5_GAME=1           -> carga zombie_pentagon (H5_MAP), corre H5_FRAMES (600) frames de servidor de 50 ms
//   H5_GAME_VERBOSE=1   -> Com_Printf de todos los canales
// Al acabar escribe el recuento de builtins llamados / sin portar y los errores de script (registro de h5_builtins).
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <universal/com_memory.h>
#include <universal/mem_userhunk.h>
#include <universal/ptr32.h>
#include <qcommon/common.h>
#include <qcommon/cmd.h>
#include <clientscript/cscr_main.h>
#include <clientscript/cscr_vm.h>
#include <clientscript/cscr_variable.h>
#include <clientscript/cscr_instance.h>
#include <clientscript/cscr_compiler.h>
#include <clientscript/cscr_parser.h>
#include <server_mp/server_mp.h>
#include <server_mp/sv_init_mp.h>
#include <server_mp/sv_main_mp.h>
#include <game_mp/g_main_mp.h>
#include <xanim/xanim.h>
#include <xanim/dobj.h>
#include <qcommon/dobj_management.h>
#include <universal/reliablemsg.h>
#include <database/database.h>
#include <database/db_registry.h>
#include <win32/win_net.h>
#include <qcommon/net_chan_mp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// aligned_alloc es API 28 en Android (la app apunta a 26): posix_memalign con la misma semantica (se libera con free)
static inline void *H5_AlignedAlloc(size_t a, size_t n) { void *p = nullptr; return posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n ? n : a) ? nullptr : p; }

#include <map>
#include <vector>
#include <zlib.h>
#include <sys/stat.h>
#include <time.h>
#include <string>

// ---------------------------------------------------------------------------------------------------------------
// Com_InitDvars ORIGINAL (common.cpp no se compila entero: trae el cliente, el FS y el bucle de Com_Frame)
#include "h5_cominitdvars.inc"
#include "h5_comfiles.inc"
#include <qcommon/cm_trace.h>
#include <DynEntity/DynEntity_client.h>
#include "h5_dynentcl.inc"
#include <physics/phys_local.h>
#include <physics/phys_gjk.h>
#include <physics/phys_collision.h>
// El fuente decompilado solo declara este asignador (extern en g_scr_sp_entity.cpp); en el original vive en phys_collision.
phys_simple_allocator<generic_avl_map_node_t> g_generic_avl_map_node_allocator;
#include "h5_physavl.inc"
#include <cgame/cg_event.h>
#include <xanim/xmodel.h>
#include <sound/snd_utils.h>
#include "h5_misc1.inc"
#include "h5_misc2.inc"
#include "h5_misc3.inc"
#include "h5_misc4.inc"
#include <physics/phys_main.h>
#include "h5_misc5.inc"
#include "h5_misc6.inc"
#include "h5_misc7.inc"
#include "h5_misc8.inc"
#include "h5_misc9.inc"
#include <EffectsCore/fx_marks.h>
#include <gfx_d3d/r_model_skin.h>
#ifndef H5FX   // con H5FX se compila fx_marks.cpp entero (OLA 6c)
#include "h5_misc10.inc"
#endif
#include <gfx_d3d/r_dobj_skin.h>
#include "h5_misc11.inc"
#include <xanim/xmodel_utils.h>
#include <xanim/dobj_utils.h>
#include <game_mp/g_utils_mp.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include "world_h4.h"

// ---------------------------------------------------------------------------------------------------------------
// H5b/H5c: puente minimo servidor -> render de H4 (H5_VIEW=1, BACKEND=vk). No es el front-end del cliente (CG_/R_AddDObjToScene
// no estan portados): por cada actor del servidor se calcula su esqueleto con la pose ORIGINAL del servidor (G_DObjCalcPose ->
// DObjCalcSkel con el XAnim que corre G_RunFrame), se arman las DObjSkelMat como R_SkinXModelCmd (base pose inversa * hueso) y se
// deforma en CPU con R_SkinXSurfaceSkinned (r_model_skin.cpp, C portable). Los vertices salen en coordenadas de mundo y se dibujan
// con colocacion identidad (H4_DrawSkinnedSurf).
static DObjSkelMat g_h5vSkel[256] __attribute__((aligned(16)));
static GfxPackedVertex *g_h5vVerts; static unsigned g_h5vVertCap;
int g_h5vActors, g_h5vSurfs, g_h5vCulled; static int g_h5vLogged;
double g_h5vSkinMs, g_h5vDrawMs; bool g_h5vDrawPlayer;   // H5d: ms de skinning CPU / envio por frame; dibujar tambien al jugador (camara chase)
static double H5V_NowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }
static int H5V_DObjIsWorld = -1;   // -1: sin decidir; 1: las DObjAnimMat del servidor ya estan en mundo; 0: locales a la entidad
static const float *s_h5vShadowOrg; static float s_h5vShadowRad;   // OLA 9: filtro del pase de sombra (h9_shadow.cpp)
static void H5V_DrawActors(unsigned char techType)
{
    const bool shadow = s_h5vShadowOrg != nullptr;
    if (!shadow) g_h5vActors = g_h5vSurfs = 0;
    // Tope de skinning (movil): H5_SKIN_MAXDIST = distancia maxima de dibujo de actores (0 = sin tope) y H5_SKIN_CULL=1 descarta los que
    // quedan fuera del cono de vision (detras o muy de lado). Solo afecta al dibujo: la pose del servidor la calcula el propio juego.
    static const float maxDist = getenv("H5_SKIN_MAXDIST") ? (float)atof(getenv("H5_SKIN_MAXDIST")) : 0.0f;
    static const bool cull = getenv("H5_SKIN_CULL") && atoi(getenv("H5_SKIN_CULL"));
    float camOrg[3], camAng[3], camFwd[3]; H4_GetCam(camOrg, camAng); AngleVectors(camAng, camFwd, nullptr, nullptr);
    for (int e = 0; e < level.num_entities; ++e)
    {
        gentity_s *ent = &g_entities[e];
        if (!ent->r.inuse || (ent->s.eType != ET_ACTOR && ent->s.eType != ET_ACTOR_CORPSE && !(g_h5vDrawPlayer && ent->s.eType == ET_PLAYER))) continue;
        if (shadow) {   // OLA 9: solo los actores dentro del radio del foco (esfera de 48 alrededor del origen + 36 de altura)
            float d2 = 0; for (int j = 0; j < 3; ++j) { const float t = ent->r.currentOrigin[j] + (j == 2 ? 36.0f : 0.0f) - s_h5vShadowOrg[j]; d2 += t * t; }
            if (d2 > (s_h5vShadowRad + 48.0f) * (s_h5vShadowRad + 48.0f)) continue;
        }
        else if (maxDist > 0 || cull)
        {
            float d[3] = { ent->r.currentOrigin[0] - camOrg[0], ent->r.currentOrigin[1] - camOrg[1], ent->r.currentOrigin[2] + 36.0f - camOrg[2] };
            const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (maxDist > 0 && len > maxDist) { ++g_h5vCulled; continue; }
            // cono amplio (cos 0,3 ~ 72 grados del eje): cubre el campo de vision de la mitad DS (1,67:1) y el horizontal; cerca siempre se dibuja
            if (cull && len > 150.0f && (d[0] * camFwd[0] + d[1] * camFwd[1] + d[2] * camFwd[2]) < 0.3f * len) { ++g_h5vCulled; continue; }
        }
        DObj *obj = Com_GetServerDObj(ent->s.number);
        if (!obj) continue;
        int partBits[5] = { -1, -1, -1, -1, -1 };
        { bgs_t *const prev = bgs; bgs = &level_bgs; G_DObjCalcPose(ent, partBits); bgs = prev; }   // H5d: el jugador (G_PlayerController) usa bgs, nulo fuera de G_RunFrame
        const DObjAnimMat *mats = DObjGetRotTransArray(obj);
        if (!mats) continue;
        // entidad -> mundo (fila: v' = v * axis + origin)
        DObjSkelMat entMat; memset(&entMat, 0, sizeof entMat);
        { float axis[3][3]; AnglesToAxis(ent->r.currentAngles, axis);
          for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) entMat.axis[i][j] = axis[i][j];
          for (int j = 0; j < 3; ++j) entMat.origin[j] = ent->r.currentOrigin[j]; entMat.origin[3] = 1.0f; }
        if (H5V_DObjIsWorld < 0)
        {
            float d = 0; for (int j = 0; j < 3; ++j) { float t = mats[0].trans[j] - ent->r.currentOrigin[j]; d += t * t; }
            H5V_DObjIsWorld = sqrtf(d) < 8.0f;
            fprintf(stderr, "[H5V] ent %d origen (%.1f,%.1f,%.1f) hueso raiz (%.1f,%.1f,%.1f): DObjAnimMat %s\n", e, ent->r.currentOrigin[0],
                ent->r.currentOrigin[1], ent->r.currentOrigin[2], mats[0].trans[0], mats[0].trans[1], mats[0].trans[2], H5V_DObjIsWorld ? "en mundo" : "locales a la entidad");
        }
        if (!shadow) { void H5L_Bind(int); H5L_Bind(e); }   // OLA 6b: bloque del atlas de luz y tecnica de esta entidad
        extern bool H19_ActorShaderPush(int) __attribute__((weak)); extern void H19_ActorShaderPop() __attribute__((weak));   // OLA F1: setshaderconstant
        const bool scsOn = !shadow && H19_ActorShaderPush && H19_ActorShaderPop && H19_ActorShaderPush(e);                     // del CSC (Wave Gun)
        const int numModels = DObjGetNumModels(obj);
        int boneIndex = 0, drewAny = 0;
        if (!shadow && getenv("H5_SKIN_DIAG")) { static int s_mN; static unsigned char s_last[2048]; if (e < 2048 && s_last[e] != numModels && s_mN < 80) { ++s_mN; s_last[e] = (unsigned char)numModels;
            fprintf(stderr, "[H5V-DIAG] t=%d ent %d: %d modelos:", level.time, e, numModels); for (int m = 0; m < numModels; ++m) fprintf(stderr, " '%s'(%d huesos)", DObjGetModel(obj, m)->name.get(), XModelNumBones(DObjGetModel(obj, m))); fprintf(stderr, "\n"); } }
        for (int m = 0; m < numModels; ++m)
        {
            XModel *model = DObjGetModel(obj, m);
            const int nb = XModelNumBones(model);
            if (boneIndex + nb > 256) break;
            const DObjAnimMat *base = XModelGetBasePose(model);
            for (int j = 0; j < nb; ++j)
            {
                DObjSkelMat inv, bone, boneW;
                ConvertQuatToInverseSkelMat(&base[j], &inv);
                ConvertQuatToSkelMat(&mats[boneIndex + j], &bone);
                if (H5V_DObjIsWorld) R_MultiplySkelMat(&inv, &bone, &g_h5vSkel[boneIndex + j]);
                else { R_MultiplySkelMat(&inv, &bone, &boneW); R_MultiplySkelMat(&boneW, &entMat, &g_h5vSkel[boneIndex + j]); }
                DObjSkelMat &o = g_h5vSkel[boneIndex + j];
                o.axis[0][3] = o.axis[1][3] = o.axis[2][3] = 0.0f; o.origin[3] = 1.0f;
            }
            XSurface *surfs; const int ns = XModelGetSurfaces(model, &surfs, 0);
            PTR32(Material) *skins = XModelGetSkins(model, 0);
            for (int si = 0; si < ns; ++si)
            {
                XSurface *xs = &surfs[si];
                Material *mtl = skins ? skins[si].get() : nullptr;
                if (!mtl || !xs->vertCount) continue;
                { static const char *skipMat = getenv("H5_SKIN_SKIPMAT"); if (skipMat && strstr(mtl->info.name.get(), skipMat)) continue; }   // OLA M1: diagnostico por material
                { extern bool H7C_SurfHidden(const DObj *, const XSurface *, int) __attribute__((weak)); if (H7C_SurfHidden && H7C_SurfHidden(obj, xs, boneIndex)) continue; }   // OLA 7c: partes ocultas (R_SkinXModel)
                if (xs->vertCount > g_h5vVertCap) { free(g_h5vVerts); g_h5vVertCap = xs->vertCount + 1024; g_h5vVerts = (GfxPackedVertex *)H5_AlignedAlloc(16, ((g_h5vVertCap * sizeof(GfxPackedVertex)) + 15) & ~15u); }
                const double sk0 = H5V_NowMs();
                R_SkinXSurfaceSkinned(xs, &g_h5vSkel[boneIndex], g_h5vVerts);
                const double sk1 = H5V_NowMs(); g_h5vSkinMs += sk1 - sk0;
                static const bool skinDiag = getenv("H5_SKIN_DIAG") && atoi(getenv("H5_SKIN_DIAG"));   // OLA M1: vertices lejos del actor (triangulos estirados)
                if (skinDiag && !shadow)
                {
                    float far2 = 0; unsigned fv = 0;
                    for (unsigned v = 0; v < xs->vertCount; ++v) { float d2 = 0; for (int j = 0; j < 3; ++j) { const float t = g_h5vVerts[v].xyz[j] - ent->r.currentOrigin[j]; d2 += t * t; } if (d2 > far2) { far2 = d2; fv = v; } }
                    unsigned maxIdx = 0; const unsigned short *ti = xs->triIndices.get(); for (unsigned k = 0; ti && k < 3u * xs->triCount; ++k) if (ti[k] > maxIdx) maxIdx = ti[k];
                    static int s_idxN; if (maxIdx >= xs->vertCount && s_idxN < 40) { ++s_idxN; fprintf(stderr, "[H5V-DIAG] t=%d ent %d modelo %d '%s' sup %d: indice maximo %u >= verts %u (tris %u)\n", level.time, e, m, model->name.get(), si, maxIdx, xs->vertCount, xs->triCount); }
                    static int s_diagN; if (far2 > 150.0f * 150.0f && s_diagN < 60) { ++s_diagN; fprintf(stderr, "[H5V-DIAG] t=%d ent %d modelo %d '%s' sup %d verts %u: vertice %u a %.0f u (%.1f,%.1f,%.1f) | huesos del modelo %d desde %d, vertListCount %d, partBits %08x %08x %08x %08x\n", level.time, e, m, model->name.get(), si, xs->vertCount, fv, sqrtf(far2), g_h5vVerts[fv].xyz[0], g_h5vVerts[fv].xyz[1], g_h5vVerts[fv].xyz[2], nb, boneIndex, (int)xs->vertListCount, xs->partBits[0], xs->partBits[1], xs->partBits[2], xs->partBits[3]); }
                }
                if (!shadow && g_h5vLogged < 12) { ++g_h5vLogged; fprintf(stderr, "[H5V] ent %d modelo %d '%s' superficie %d verts=%u tris=%u material '%s' v0=(%.1f,%.1f,%.1f)\n", e, m, model->name.get(), si, xs->vertCount, xs->triCount, mtl->info.name.get(), g_h5vVerts[0].xyz[0], g_h5vVerts[0].xyz[1], g_h5vVerts[0].xyz[2]); }
                H4_DrawSkinnedSurf(mtl, xs, g_h5vVerts, techType);
                g_h5vDrawMs += H5V_NowMs() - sk1;
                ++g_h5vSurfs; drewAny = 1;
            }
            boneIndex += nb;
        }
        if (scsOn) H19_ActorShaderPop();
        if (!shadow) g_h5vActors += drewAny;
    }
}
// OLA 9: actores como proyectores de sombra de un foco (tecnica 2, BUILD_SHADOWMAP_DEPTH). Se vuelven a deformar en CPU (coste por foco).
void H5V_DrawActorsShadow(const float *org, float radius, unsigned char techType)
{
    s_h5vShadowOrg = org; s_h5vShadowRad = radius;
    const int a = g_h5vActors, s = g_h5vSurfs;
    H5V_DrawActors(techType);
    g_h5vActors = a; g_h5vSurfs = s; s_h5vShadowOrg = nullptr;
    { void H20_DrawShadow(const float *, float, unsigned char); H20_DrawShadow(org, radius, techType); }   // OLA S1: script_model del servidor (h20_svmodels.cpp)
}
// Camara del render: H5_VIEW_CAM=player (ojo y angulos del jugador del servidor), look (ojo del jugador mirando al actor mas
// cercano: por defecto), near (a H5_VIEW_DIST unidades del actor mas cercano, delante de el, mirandolo)
static bool H5V_Camera(float *org, float *ang)
{
    gentity_s *pl = &g_entities[0];
    if (!pl->r.inuse || !pl->client) return false;
    const playerState_s *ps = &pl->client->ps;
    float eye[3] = { ps->origin[0], ps->origin[1], ps->origin[2] + ps->viewHeightCurrent };
    const char *mode = getenv("H5_VIEW_CAM") ? getenv("H5_VIEW_CAM") : "look";
    memcpy(org, eye, 12); memcpy(ang, ps->viewangles, 12);
    if (!strcmp(mode, "player")) return true;
    gentity_s *best = nullptr; float bd = 1e30f;
    for (int e = 0; e < level.num_entities; ++e)
    {
        gentity_s *a = &g_entities[e];
        if (!a->r.inuse || a->s.eType != ET_ACTOR) continue;
        float d = 0; for (int j = 0; j < 3; ++j) { float t = a->r.currentOrigin[j] - ps->origin[j]; d += t * t; }
        if (d < bd) { bd = d; best = a; }
    }
    if (!best) return true;
    float tgt[3] = { best->r.currentOrigin[0], best->r.currentOrigin[1], best->r.currentOrigin[2] + 40.0f };
    if (!strcmp(mode, "near"))
    {
        const float dist = getenv("H5_VIEW_DIST") ? (float)atof(getenv("H5_VIEW_DIST")) : 110.0f;
        float fwd[3]; AngleVectors(best->r.currentAngles, fwd, nullptr, nullptr);
        org[0] = tgt[0] + fwd[0] * dist; org[1] = tgt[1] + fwd[1] * dist; org[2] = tgt[2] + 10.0f;
    }
    float dir[3] = { tgt[0] - org[0], tgt[1] - org[1], tgt[2] - org[2] };
    vectoangles(dir, ang);
    return true;
}
// SND_FindAliasId (snd_bank.cpp, TU de sonido no compilada): el id es SND_HashName(name); el resto es traza del mod
// (SND_BO1TraceName) y SND_LogRegisterString, que solo actua con SND_Active() (sin sonido en el servidor: falso).
#ifndef H6SND   // OLA 6a: con sonido se compila snd_bank.cpp entero
int __cdecl SND_FindAliasId(char *name) { return SND_HashName(name); }
#endif

void H5G_ReportBuiltins();

// Dvars que registran TUs que el harness no compila (FS real, UI de playlists, fisica). Mismos nombres, valores y banderas
// que el original (com_files.cpp FS_RegisterDvars, ui_playlists.cpp Playlist_RegisterDvars sin rango, phys_main.cpp Phys_Init).
extern const dvar_s *fs_debug, *fs_gameDirVar, *fs_usermapDir, *fs_ignoreLocalized, *fs_homepath, *fs_basepath;
extern const dvar_s *enable_moving_paths, *playlist_enabled, *party_maxplayers, *playlist_entry;
extern const dvar_s *xblive_wagermatch, *xblive_basictraining;
const dvar_t *dynEnt_bulletForce, *dynEnt_explodeForce, *dynEnt_explodeMaxEnts;   // de DynEntity_client.cpp (ver abajo)
static void H5G_RegisterMissingDvars()
{
    // Live_Init (live/live_win.cpp, no compilado): SV_UploadStats (builtin uploadstats() del fin de ronda) lee xblive_wagermatch. H5d.
    xblive_wagermatch = _Dvar_RegisterBool("xblive_wagermatch", 0, 4u, "Current game is a wager match");
    xblive_basictraining = _Dvar_RegisterBool("xblive_basictraining", 0, 4u, "Current game is basic training");
    fs_debug = _Dvar_RegisterInt("fs_debug", 0, 0, 2, 0, "Enable file system debugging information");
    fs_basepath = _Dvar_RegisterString("fs_b", ".", 0x210u, "Base game path");
    fs_gameDirVar = _Dvar_RegisterString("fs_game", "", 0x114u, "Game data directory. Must be \"\" or a sub directory of 'mods/'.");
    fs_usermapDir = _Dvar_RegisterString("fs_usermapdir", "", 0x144u, "Usermap data directory.");
    fs_ignoreLocalized = _Dvar_RegisterBool("fs_ignoreLocalized", 0, 0xA0u, "Ignore localized files");
    fs_homepath = _Dvar_RegisterString("fs_h", ".", 0x210u, "Game home path");
    party_maxplayers = _Dvar_RegisterInt("party_maxplayers", 18, 1, 32, 0, "Maximum number of players");
    playlist_enabled = _Dvar_RegisterBool("playlist_enabled", 0, 4u, "The server is using playlist");   // servidor sin rango
    playlist_entry = _Dvar_RegisterInt("playlist_entry", 0, 0, 0x2000, 4u, "The current server playlist entry.");
    enable_moving_paths = _Dvar_RegisterInt("enable_moving_paths", 1, 0, 1, 0, "enable moving paths");
    // DynEntCl_RegisterDvars (DynEntity_client.cpp, no compilado): DynEntSv_RadiusDamage -> DynEntSv_GetClosestEntities lee
    // dynEnt_explodeMaxEnts en cada explosion; sin registrar (dato a cero de genstubs) caia con SIGSEGV en cuanto una explosion
    // tocaba un mapa con DynEnts (Call of the Dead con la M1911 mejorada, Kino con la corriente temprana). Mismos valores.
    dynEnt_bulletForce = _Dvar_RegisterFloat("dynEnt_bulletForce", 800.0f, 0.0f, 1000000.0f, 0x1080u, "Force applied from bullet hit");
    dynEnt_explodeForce = _Dvar_RegisterFloat("dynEnt_explodeForce", 12500.0f, 0.0f, 1000000.0f, 0x80u, "Force applied from explosion hit");
    dynEnt_explodeMaxEnts = _Dvar_RegisterInt("dynEnt_explodeMaxEnts", 20, 0, 4096, 0x80u, "The maximum number of dynents that can be awakened by one explosion");
    // CG_RegisterDvars (cg_main_mp.cpp): _zombiemode_load.gsc hace level.clientscripts = (GetDvar("cg_usingClientScripts") != "").
    // Sin el, _createfx.gsc crea en el servidor todos los efectos createfx (175 SpawnFx en pentagon) y G_Spawn se queda sin
    // entidades; en el original el cliente local siempre lo registra a 1 y esos efectos van por clientscripts.
    _Dvar_RegisterBool("cg_usingClientScripts", 1, 0x80u, "True, if client scripts are enabled.");
    // Con_Init (cl_console.cpp): en el original SP siempre hay cliente local y _load_common.gsc (SetObjectiveTextColors)
    // hace SetSavedDvar sobre ellos; sin cliente lanzaria "dvar does not exist". Mismos valores, rangos y banderas.
    _Dvar_RegisterBool("con_typewriterEnabledSounds", 0, 1u, "Enable the typewriter sounds and text effects.    Disabled still uses the typewrite glow settings.");
    _Dvar_RegisterInt("con_typewriterPrintSpeed", 40, 0, 0x7FFFFFFF, 1u, "Time (in milliseconds) to print each letter in the line.");
    _Dvar_RegisterInt("con_typewriterDecayStartTime", 4000, 0, 0x7FFFFFFF, 1u, "Time (in milliseconds) to spend between the build and disolve phases.");
    _Dvar_RegisterInt("con_typewriterDecayDuration", 1000, 0, 0x7FFFFFFF, 1u, "Time (in milliseconds) to spend disolving the line away.");
    _Dvar_RegisterVec3("con_typewriterColorBase", 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0x1000u, "Base color of typewritten objective text.");
    _Dvar_RegisterVec4("con_typewriterColorGlowUpdated", 0.0f, 0.60000002f, 0.18000001f, 1.0f, 0.0f, 1.0f, 1u, "Color of typewritten objective text.");
    _Dvar_RegisterVec4("con_typewriterColorGlowCompleted", 0.0f, 0.30000001f, 0.80000001f, 1.0f, 0.0f, 1.0f, 1u, "Color of typewritten objective text.");
    _Dvar_RegisterVec4("con_typewriterColorGlowFailed", 0.80000001f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1u, "Color of typewritten objective text.");
    _Dvar_RegisterVec4("con_typewriterColorGlowCheckpoint", 0.60000002f, 0.5f, 0.60000002f, 1.0f, 0.0f, 1.0f, 1u, "Color of typewritten objective text.");
}
// Phys_Init (physics/phys_main.cpp; la fisica Nitrous no se compila): las dvars que lee el juego del servidor, con nombres,
// valores, rangos y banderas del original. Decision del port (igual que H4, ADR 2026-11-16): la colision GJK (modo 1) no esta
// portada, asi que jugador y actores usan la ruta CM_BoxTrace del propio motor (modo 0, seleccionable por estas mismas dvars).
extern const dvar_s *phys_player_collision_mode, *render_player_collision, *phys_debugExpensivePushout, *enable_new_prone_check;
extern const dvar_s *phys_ai_collision_mode, *render_actor_collision, *phys_piecesSpawnDistanceCutoff;
static void H5G_PhysInitDvars()
{
    phys_piecesSpawnDistanceCutoff = _Dvar_RegisterFloat("phys_piecesSpawnDistanceCutoff", 1000.0f, 0.0f, 3.4028235e38f, 0x1000u, "Determine how far we can spawn destructible pieces.");
    phys_debugExpensivePushout = _Dvar_RegisterBool("phys_debugExpensivePushout", 1, 0, "Draw debug info when the engine resort to exensive Player/AI pushouts");
    phys_player_collision_mode = _Dvar_RegisterInt("phys_player_collision_mode", 1, 0, 1, 0, "phys player collision mode");
    phys_ai_collision_mode = _Dvar_RegisterInt("phys_ai_collision_mode", 1, 0, 1, 0, "phys ai collision mode");
    render_player_collision = _Dvar_RegisterInt("render_player_collision", 0, 0, 1, 0, "render player collision");
    render_actor_collision = _Dvar_RegisterInt("render_actor_collision", 0, 0, 1, 0, "render actor collision");
    enable_new_prone_check = _Dvar_RegisterInt("enable_new_prone_check", 0, 0, 1, 0x80u, "enable new prone check");
    Dvar_SetInt((dvar_s *)phys_player_collision_mode, 0);
    Dvar_SetInt((dvar_s *)phys_ai_collision_mode, 0);
}
static int g_h5gFrame;

extern "C" void H15_MemPhase(const char *phase);   // OLA 15A: memguard.cpp (H15_MEMPROFILE)
int H5G_Run()
{
    extern const char *H18_Map(); const char *map = getenv("H5_MAP") ? getenv("H5_MAP") : H18_Map();   // OLA M0: lo resuelve h18_maps.cpp
    const int frames = getenv("H5_FRAMES") ? atoi(getenv("H5_FRAMES")) : 600;
    fprintf(stderr, "[H5G] arranque del servidor dedicado: %s, %d frames\n", map, frames);
    { extern int g_h0PrintAll; g_h0PrintAll = getenv("H5_GAME_VERBOSE") != 0; }
    atexit(H5G_ReportBuiltins);   // tambien si Com_Error termina el proceso
    Com_InitThreadData(0);      // Sys_Main/Com_Init: TLS del hilo principal (va, com_error, trazas de CM, args)
    com_codeTimeScale = 1.0f;   // Com_Init
    // El contador de depuracion de cadenas (SL_InitCheckLeaks) se activo en main2 antes de cargar las zonas; las cadenas que
    // los cargadores del harness meten sin pasar por el se ven con refCount de depuracion 0 y el assert de SL_FindStringOfSize
    // salta. Solo es contabilidad de fugas de depuracion: se apaga (como Scr_ClearScrStringDebugGlob tras SL_CheckLeaks).
    Scr_ClearScrStringDebugGlob(SCRIPTINSTANCE_SERVER);
    { extern HunkUser *g_DebugHunkUser; if (!g_DebugHunkUser) g_DebugHunkUser = Hunk_UserCreate(0x1700000, HU_SCHEME_DEBUG, 0, 0, "Hunk_InitDebugMemory", 0); }   // Hunk_InitDebugMemory (com_memory.cpp)
    Cbuf_Init();
    Cmd_Init();
    Dvar_Init();      // Com_Init: sv_cheats (dvar_cheats), sv_restoreDvars y los comandos de dvars
    Com_InitDvars();
    Dvar_SetIntByName("dedicated", 2);   // servidor dedicado (sin cliente local) como headless.ps1 -Zombies
    Dvar_SetBoolByName("zombiemode", true);
    // Com_Init: bo1_mod_scriptvars 0 (pools retail) y bo1_mod_scriptperf 0 (scripts tal cual, lo mas fiel)
    _Dvar_RegisterInt("bo1_mod_scriptvars", 0, 0, 2, 0x10u, "");
    _Dvar_RegisterInt("bo1_mod_scriptperf", 0, 0, 1, 0, "");
    Netchan_Init((__int16)(Sys_Milliseconds() & 0xFFFF));   // Com_Init: Netchan_Init(QueryPerformanceCounter)
    H5G_RegisterMissingDvars();
    H5G_PhysInitDvars();
    Scr_InitVariables(SCRIPTINSTANCE_SERVER);
    Scr_Init(SCRIPTINSTANCE_SERVER);
    Com_SetScriptSettings();
    // H5_DEV=1 (OLA M1): compila los /# #/ del GSC (devgui original: zombie_devgui money/power_on/...) sin abort_on_error
    if (getenv("H5_DEV") && atoi(getenv("H5_DEV"))) Scr_Settings(1, 1, 0, SCRIPTINSTANCE_SERVER);
    XAnimInit();
    DObjInit();
    Com_InitDObj();   // common.cpp (fin de Com_Init/Com_Restart): XAnimInit, DObjInit, Com_InitDObj
    SV_Init();
    NET_Init();
    RMsg_Init();
    fprintf(stderr, "[H5G] SV_Init hecho; SV_SpawnServer(%s)\n", map);
    { void H5F_InstallHooks(); H5F_InstallHooks(); }   // H5f (h5_play.cpp): claves canonicas de level.round_number / zombie_total al final de la carga
    H15_MemPhase("carga_servidor");
    { void H18_LoadVideoRemote(bool); H18_LoadVideoRemote(true); }   // OLA F3b: sin render en el hilo principal: el video lo presenta su hilo (R_BeginRemoteScreenUpdate)
    SV_SpawnServer(0, (char *)map, 1, 0);
    { void H18_LoadVideoRemote(bool); H18_LoadVideoRemote(false); }
    fprintf(stderr, "[H5G] SV_SpawnServer hecho\n");
    if (getenv("H5_PLAYER") || getenv("H5_PLAY"))   // H5_PLAY (H5d, h5_play.cpp) tambien necesita el jugador
    {
        // H5b: un jugador. El mod trae el arnes headless original (game_sp/g_sp_client.cpp, bo1_testclient): en el primer frame de
        // servidor SV_AddTestClient -> SV_DirectConnect (NA_BOT) -> SV_SendClientGameState, y un frame despues SV_ClientEnterWorld
        // (CS_CLIENTLOADING -> CS_ACTIVE, ClientBegin), lo mismo que tools/headless.ps1 -Zombies +set bo1_testclient 1.
        Dvar_SetBoolByName("bo1_testclient", true);
        fprintf(stderr, "[H5G] bo1_testclient=1 (jugador de prueba del arnes headless)\n");
    }
    { void H5E_ApplyDvars(); H5E_ApplyDvars(); }   // H5e (h5_weapon.cpp): H5_SET="dvar valor;..." p.ej. bo1_testclient_fight 1
    if (const char *dl = getenv("H5_DUMP_GSC"))   // diagnostico: vuelca rawfiles .gsc/.csc (zlib) a H5_DUMP_DIR, fuera del repo
    {
        std::string all = dl; size_t p0 = 0;
        while (p0 < all.size())
        {
            size_t p1 = all.find(',', p0); if (p1 == std::string::npos) p1 = all.size();
            std::string nm = all.substr(p0, p1 - p0); p0 = p1 + 1;
            RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)nm.c_str(), 1, -1).rawfile;
            if (!rf) { fprintf(stderr, "[H5G] dump: %s no esta en las zonas\n", nm.c_str()); continue; }
            const unsigned *hd = (const unsigned *)rf->buffer.get(); uLongf outlen = hd[0];
            std::vector<unsigned char> out(outlen + 1);
            uncompress(out.data(), &outlen, (const unsigned char *)(hd + 2), hd[1]);
            std::string fn = std::string(getenv("H5_DUMP_DIR") ? getenv("H5_DUMP_DIR") : ".") + "/" + nm; for (auto &c : fn) if (c == '\\') c = '/';
            for (size_t k = fn.find('/', 1); k != std::string::npos; k = fn.find('/', k + 1)) mkdir(fn.substr(0, k).c_str(), 0755);
            if (FILE *f = fopen(fn.c_str(), "wb")) { fwrite(out.data(), 1, outlen, f); fclose(f); fprintf(stderr, "[H5G] dump: %s (%lu bytes)\n", fn.c_str(), (unsigned long)outlen); }
        }
    }
    // H5_VIEW=1 (build BACKEND=vk, sin P3_BACKEND=trace): mundo de H4 + actores del servidor; render y PNG cada H5_VIEW_EVERY frames
    const bool view = getenv("H5_VIEW") != 0;
    const int viewEvery = getenv("H5_VIEW_EVERY") ? atoi(getenv("H5_VIEW_EVERY")) : 50;
    if (view)
    {
        const int W = getenv("P4_W") ? atoi(getenv("P4_W")) : 1280, H = getenv("P4_H") ? atoi(getenv("P4_H")) : 720;
        H15_MemPhase("carga_render");
        if (!H4_Setup(W, H)) { fprintf(stderr, "[H5V] sin GfxWorld\n"); return 1; }
        g_h4DrawExtra = H5V_DrawActors;
        { void H20_Install(); H20_Install(); }   // OLA S1: script_model del servidor (CG_ScriptMover/CG_General) tras los actores, antes de cristales/FX/viewmodel
        { void H8_InstallDraw(); H8_InstallDraw(); }   // OLA 8: cristales con su colocacion (h8_doors.cpp), antes de FX y viewmodel
#ifdef H5FX
        { void H5FX_InstallDraw(); H5FX_InstallDraw(); }   // OLA 6c: FX entre actores y viewmodel (h5_fx.cpp)
#endif
        { void H5W_InstallViewModel(); H5W_InstallViewModel(); }   // H5e: viewmodel en primera persona
        { void H5L_Install(); H5L_Install(); }   // OLA 6b: luz de modelos con el lightgrid (h5_light.cpp)
#ifdef H5FX
        { void H5FX_InstallPre(); H5FX_InstallPre(); }     // OLA 6c: actualizacion de FX en g_h4PreFrame (encadena la de H5L)
#endif
        fprintf(stderr, "[H5V] render de H4 %dx%d, captura cada %d frames de servidor\n", W, H, viewEvery);
    }
    { void H18_EndLoadVideo(); H18_EndLoadVideo(); }   // OLA F3b: carga hecha; el video de carga sigue hasta su final o hasta saltarlo
    if (getenv("H5_PLAY")) { extern int H5P_Run(int, bool); const int rc = H5P_Run(frames, view); H5G_ReportBuiltins(); return rc; }   // H5d: jugador unificado + render cada frame
    for (g_h5gFrame = 0; g_h5gFrame < frames; ++g_h5gFrame)
    {
        SV_Frame(0, 50);
        { void H5E_Frame(); H5E_Frame(); } { void H8_Frame(); H8_Frame(); }   // OLA 8: puertas/cristales (h8_doors.cpp)   // H5e: registro [H5E] del arma del jugador
        static const int viewFrom = getenv("H5_VIEW_FROM") ? atoi(getenv("H5_VIEW_FROM")) : 0;   // primer frame con render
        if (view && g_h5gFrame >= viewFrom && (g_h5gFrame % viewEvery) == viewEvery - 1)
        {
            float org[3], ang[3];
            if (H5V_Camera(org, ang))
            {
                H4_SetCam(org, ang); H4_Frame();
                char tag[24]; snprintf(tag, sizeof tag, "_t%05d", level.time); H4_Shot(tag);
                { char tl[128] = ""; for (int t = 0; t < 256; ++t) if (g_h4SkinnedTech[t]) snprintf(tl + strlen(tl), sizeof tl - strlen(tl), " tec%d=%d", t, g_h4SkinnedTech[t]);
                  memset(g_h4SkinnedTech, 0, sizeof g_h4SkinnedTech); if (*tl) fprintf(stderr, "[H5V] tecnicas de las superficies de actor:%s\n", tl); }
                fprintf(stderr, "[H5V] t=%d cam (%.1f,%.1f,%.1f) ang (%.1f,%.1f) actores dibujados=%d superficies=%d (sin tecnica %d) lotes mundo=%d\n", level.time,
                    org[0], org[1], org[2], ang[0], ang[1], g_h5vActors, g_h5vSurfs, g_h4SkinnedSkipped, H4_LastDrawn());
            }
        }
        if ((g_h5gFrame % 100) == 0) { extern int g_h5gServerFrames; fprintf(stderr, "[H5G] frame %d level.time=%d G_RunFrame=%d\n", g_h5gFrame, level.time, g_h5gServerFrames); }
    }
    fprintf(stderr, "[H5G] %d frames de servidor sin abortar (level.time=%d)\n", g_h5gFrame, level.time);
    { void H5E_Report(); H5E_Report(); }
    H5G_ReportBuiltins();
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Informe de builtins y errores de script (H5a). Ganchos #ifdef H5G en el motor:
//   cscr_compiler.cpp (tras AddFunction): nombre GSC de cada indice de func_table;
//   cscr_vm.cpp (OP_CallBuiltin / OP_CallBuiltinMethod, junto a SvProfBuiltin): cuenta de llamadas por indice;
//   cscr_parser.cpp (RuntimeErrorInternal): mensaje de cada error de script en tiempo de ejecucion.
// "No portado" = el puntero del builtin es un GScr_*NotPorted_* (g_scr_sp_notported.cpp, lanza Scr_Error) o cae en un stub
// generado (genstubs: primera BL a h0_stub_abort). Se imprime al terminar o en cualquier salida (atexit, p.ej. Com_Error).
#include <dlfcn.h>
#include <cxxabi.h>
#include <vector>
#include <algorithm>
extern "C" void h0_stub_abort(const char *);
static struct H5GBuiltin { std::string name; bool method; unsigned calls; } g_h5gBuiltins[2][1024];
static std::map<std::string, unsigned> g_h5gScriptErrors;
static const char *g_h5gLastPos[2];
static bool g_h5gReported;
void H5G_NoteBuiltinName(int inst, int idx, const char *name, bool method)
{
    if (idx < 0 || idx >= 1024) return;
    if (g_h5gBuiltins[inst][idx].name.empty()) { g_h5gBuiltins[inst][idx].name = name ? name : "?"; g_h5gBuiltins[inst][idx].method = method; }
}
void H5G_NoteBuiltinCall(int inst, int idx, const char *pos) { if (idx >= 0 && idx < 1024) ++g_h5gBuiltins[inst][idx].calls; g_h5gLastPos[inst] = pos; }
void H5G_NoteScriptError(int inst, const char *msg) { ++g_h5gScriptErrors[std::string(inst ? "[CSC] " : "") + (msg ? msg : "?")]; }
static std::string H5G_Sym(const void *f)
{
    Dl_info d; if (!f || !dladdr(f, &d) || !d.dli_sname) return "?";
    const char *m = d.dli_sname; if (*m == '_' && m[1] == '_') ++m;   // Mach-O antepone '_'
    int st = 0; char *dm = abi::__cxa_demangle(m, 0, 0, &st);
    std::string r = st == 0 && dm ? dm : d.dli_sname; free(dm); return r;
}
static bool H5G_IsGeneratedStub(const void *f)
{
    const unsigned *w = (const unsigned *)f;   // stubs.s: stp; adrp; add; bl _h0_stub_abort
    for (int i = 0; i < 4; ++i)
        if ((w[i] & 0xFC000000u) == 0x94000000u)
        {
            int imm = (int)(w[i] << 6) >> 6;
            return (const char *)&w[i] + (long)imm * 4 == (const char *)&h0_stub_abort;
        }
    return false;
}
void H5G_ReportBuiltins()
{
    if (g_h5gReported) return;
    g_h5gReported = true;
    const int inst = SCRIPTINSTANCE_SERVER;
    int n = gScrCompilePub[inst].func_table_size, called = 0;
    std::vector<int> order;
    for (int i = 0; i < n; ++i) { order.push_back(i); if (g_h5gBuiltins[inst][i].calls) ++called; }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return g_h5gBuiltins[inst][a].calls > g_h5gBuiltins[inst][b].calls; });
    fprintf(stderr, "[H5G-BUILTINS] referenciados por los scripts compilados=%d llamados=%d\n", n, called);
    std::vector<std::string> missing;
    for (int i : order)
    {
        const H5GBuiltin &b = g_h5gBuiltins[inst][i];
        const void *f = (const void *)gScrCompilePub[inst].func_table[i];
        std::string sym = H5G_Sym(f);
        const char *st = sym.find("NotPorted") != std::string::npos ? "NO_PORTADO" : H5G_IsGeneratedStub(f) ? "STUB" : "ok";
        if (strcmp(st, "ok")) missing.push_back(std::string(st) + " " + (b.method ? "metodo " : "funcion ") + b.name + " llamadas=" + std::to_string(b.calls));
        fprintf(stderr, "[H5G-BUILTIN] %6u %-7s %-40s %-10s %s\n", b.calls, b.method ? "metodo" : "funcion", b.name.c_str(), st, sym.c_str());
    }
    fprintf(stderr, "[H5G-FALTAN] %zu builtins referenciados sin portar (NO_PORTADO lanza Scr_Error al llamarse; STUB devuelve 0)\n", missing.size());
    for (auto &m : missing) fprintf(stderr, "[H5G-FALTA] %s\n", m.c_str());
    unsigned tot = 0; for (auto &e : g_h5gScriptErrors) tot += e.second;
    fprintf(stderr, "[H5G-ERRORES] %u errores de script en tiempo de ejecucion (%zu distintos)\n", tot, g_h5gScriptErrors.size());
    for (auto &e : g_h5gScriptErrors) fprintf(stderr, "[H5G-ERROR] x%u %s\n", e.second, e.first.c_str());
    fflush(stderr);
}
// Posicion GSC de la ultima llamada a builtin (diagnostico: Scr_PrintPrevCodePos va por Com_Printf).
void H5G_DumpLastScriptPos()
{
    extern int g_h0PrintAll; int o = g_h0PrintAll; g_h0PrintAll = 1;
    if (g_h5gLastPos[0]) Scr_PrintPrevCodePos(SCRIPTINSTANCE_SERVER, 0, (char *)g_h5gLastPos[0], 0);
    for (int i = gScrVmPub[0].function_count - 1; i >= 0; --i) { fprintf(stderr, "called from:\n"); Scr_PrintPrevCodePos(SCRIPTINSTANCE_SERVER, 0, (char *)gScrVmPub[0].function_frame_start[i].fs.pos, i == 0 || gScrVmPub[0].function_frame_start[i].fs.localId == 0); }
    g_h0PrintAll = o;
}


// ---------------------------------------------------------------------------------------------------------------
// Hilo del servidor COOPERATIVO. En el original SV_FrameInternal (sin sv.smp) hace svs.time += frameMsec; SV_WakeServer();
// SV_WaitServer(): despierta a SV_ServerThread (sv_main_mp.cpp) y espera a que acabe su vuelta. El harness tiene un solo
// hilo y un TLS global (rt2.cpp Sys_GetValue), asi que la vuelta del hilo del servidor se ejecuta aqui mismo, en el mismo
// orden que la rama "start" de SV_ServerThread. G_ClearVehicleInputs no se llama (sin vehiculos Nitrous en pentagon, su
// lista esta vacia) ni G_SP_ProcessSaveQueue (g_sp_savegame no se compila; sin guardados en H5a).
void SV_CalcPings(); void SV_PreFrame(); void SV_RunFrame(); void SV_PostFrame(); void SV_RunEventLoop();
void SV_ClearServerThreadOwnsGame(); void SV_IncServerThreadOwnsGame(); void SV_DecServerThreadOwnsGame();
void TaskManager2_PickUpDeferredTasks();
int g_h5gServerFrames;
static bool g_h5gServerThreadInit;
void Sys_WakeServer()
{
    if (!g_h5gServerThreadInit) { g_h5gServerThreadInit = true; SV_ClearServerThreadOwnsGame(); }   // cabecera de SV_ServerThread
    SV_RunEventLoop();
    TaskManager2_PickUpDeferredTasks();
    SV_IncServerThreadOwnsGame();
    SV_CalcPings();
    SV_PreFrame();
    SV_RunFrame();
    SV_PostFrame();
    SV_RunEventLoop();
    SV_DecServerThreadOwnsGame();
    { void SV_H5G_MarkFrameRun(); SV_H5G_MarkFrameRun(); }   // InterlockedExchange(&s_svFramesRun, serving) de SV_ServerThread
    ++g_h5gServerFrames;
}
bool Sys_WaitServer(unsigned int) { return true; }   // la vuelta ya termino dentro de Sys_WakeServer
bool Sys_WaitStartServer(unsigned int) { return false; }
void Sys_SleepServer() {}
void Sys_ServerCompleted() {}
bool Sys_AssistNeeded() { return false; }
void Sys_AssistSingle() {}
// eventos entre el hilo principal y la red del servidor: con un hilo no hay nada que senalar ni esperar
void Sys_SetServerAllowNetworkEvent() {}
void Sys_ResetServerAllowNetworkEvent() {}
void Sys_SetServerNetworkCompletedEvent() {}
void Sys_ResetServerNetworkCompletedEvent() {}
void Sys_WaitAllowServerNetworkLoop() {}
void Sys_WaitServerNetworkCompleted() {}

// ---------------------------------------------------------------------------------------------------------------
// Plataforma minima que pide el servidor (win32/ no se compila en el harness)
#include <unistd.h>
unsigned int Sys_GetCpuCount() { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (unsigned int)n : 1u; }
// Sin red: el servidor local no abre sockets (win_net.cpp no se compila). Los NET_* que lleguen se veran como stubs.
void NET_Init() {}

// ---------------------------------------------------------------------------------------------------------------
// Hunk bajo, memoria temporal y datos por fichero (universal/com_memory.cpp guarda punteros en structs de 32 bits:
// fileData_s con alloc(strlen + 10)). Version del harness con la misma semantica, dentro de la arena de 32 bits.
#include <vector>
static char *g_lowBase, *g_lowPos, *g_lowEnd;
static const size_t H5G_HUNK_LOW = 256u << 20;
static void H5G_LowInit() { if (!g_lowBase) { g_lowBase = g_lowPos = (char *)Ptr32_ArenaAlloc(H5G_HUNK_LOW, 4096); g_lowEnd = g_lowBase + H5G_HUNK_LOW; } }
unsigned char *Hunk_AllocLowAlign(unsigned int size, int alignment, const char *name, int)
{
    H5G_LowInit();
    if (alignment < 1) alignment = 1;
    char *p = (char *)(((uintptr_t)g_lowPos + alignment - 1) & ~(uintptr_t)(alignment - 1));
    if (p + size > g_lowEnd) Com_Error(ERR_FATAL, "Hunk_AllocLowAlign failed on %u bytes (%s)", size, name ? name : "");
    memset(p, 0, size);   // el hunk original entrega memoria a cero
    g_lowPos = p + size;
    return (unsigned char *)p;
}
unsigned char *Hunk_AllocLow(unsigned int size, const char *name, int type) { return Hunk_AllocLowAlign(size, 32, name, type); }
void Hunk_ClearToMarkLow(int mark) { H5G_LowInit(); g_lowPos = g_lowBase + mark; }
int Hunk_Used() { H5G_LowInit(); return (int)(g_lowPos - g_lowBase); }
bool Hunk_DataOnHunk(unsigned char *data) { H5G_LowInit(); return (char *)data >= g_lowBase && (char *)data < g_lowEnd; }
unsigned int *Hunk_AllocateTempMemory(int size, const char *) { return (unsigned int *)Ptr32_ArenaAlloc(size > 0 ? size : 1, 16); }
void Hunk_FreeTempMemory(char *buf) { if (buf) Ptr32_ArenaFree(buf); }
char *Z_MallocGarbage(int size, const char *, int) { return (char *)malloc(size > 0 ? size : 1); }
struct H5FileData { int type; std::string name; void *data; };
static std::vector<H5FileData *> g_fileData;
static std::vector<std::pair<int, void *>> g_hunkData;
static bool H5_NameEq(const std::string &a, const char *b) { return !I_stricmp(a.c_str(), b); }
void *Hunk_FindDataForFile(int type, const char *name)
{
    for (H5FileData *f : g_fileData) if (f->type == type && H5_NameEq(f->name, name)) return f->data;
    return 0;
}
char *Hunk_SetDataForFile(int type, const char *name, void *data, void *(*)(int))
{
    H5FileData *f = new H5FileData{ type, name, data }; g_fileData.push_back(f);
    return (char *)f->name.c_str();
}
void Hunk_OverrideDataForFile(int type, const char *name, void *data)
{
    for (H5FileData *f : g_fileData) if (f->type == type && H5_NameEq(f->name, name)) { f->data = data; return; }
}
void Hunk_AddData(int type, void *data, void *(*)(int)) { g_hunkData.push_back({ type, data }); }
// com_memory.cpp (no se compila entero): useFastFile siempre activo en el harness
int DB_GetAllXAssetOfType(XAssetType type, XAssetHeader *assets, int maxCount) { return DB_GetAllXAssetOfType_FastFile(type, assets, maxCount); }
// files.cpp (FS real no compilado): sin comprobaciones pure de IWD y fs_game "" -> el original devuelve dos cadenas vacias
void FS_ReferencedIwds(const char **checksums, const char **names) { *checksums = ""; *names = ""; }
// FS_ReadFile/FS_FreeFile (universal/com_files.cpp, FS real no compilado) sobre el FS del harness (iwd.cpp): misma semantica,
// -1 si no existe (p.ej. "ban.txt" de SV_DirectConnect/SV_IsBanned), longitud y buffer terminado en 0 si existe.
int FS_FOpenFileRead(const char *filename, int *file); unsigned int FS_Read(unsigned char *buffer, unsigned int len, int h); void FS_FCloseFile(int h);
int FS_ReadFile(const char *qpath, void **buffer)
{
    int h = 0; int len = FS_FOpenFileRead(qpath, &h);
    if (len < 0 || !h) { if (buffer) *buffer = 0; return -1; }
    if (!buffer) { FS_FCloseFile(h); return len; }
    unsigned char *b = (unsigned char *)Ptr32_ArenaAlloc(len + 1, 16);
    FS_Read(b, len, h); b[len] = 0; FS_FCloseFile(h); *buffer = b; return len;
}
void FS_FreeFile(void *buffer) { if (buffer) Ptr32_ArenaFree(buffer); }
// cl_console.cpp:4302 (el cliente no se compila; Scr_ScriptList::Init del depurador la usa para ordenar)
int ConDrawInput_CompareStrings(const char **e0, const char **e1) { return I_stricmp(*e0, *e1); }

// PMM_*: el gestor de memoria de fisica original (phys_mem_new.cpp) es un pool lock-free que empaqueta punteros de 32 bits
// en palabras de 64 (__SPAIR64__); no es portable a LP64 tal cual. Para el servidor de H5 basta con memoria alineada:
// mismo contrato (bloque alineado de 'size' bytes; el llamador hace el memset/placement-new).
char *__cdecl PMM_ALLOC(unsigned int size, unsigned int alignment)
{
    iassert(size > 0);
    size_t a = alignment < 16 ? 16 : alignment;
    return (char *)H5_AlignedAlloc(a, (size + a - 1) / a * a);
}
void __cdecl PMM_FREE(unsigned __int8 *ptr, unsigned int, unsigned int) { free(ptr); }
void __cdecl PMM_VALIDATE(char *, unsigned int, unsigned int) {}

// Con_OpenChannel (con_channels.cpp, TU de consola sin portar): solo lo llama createprintchannel() del GSC en los /# #/
// (H5_DEV=1). Los canales de impresion no existen en el arnes: se acepta el canal para que el script siga, como si abriera.
char __cdecl Con_OpenChannel(const char *, bool) { return 1; }
// DevGui_IsActive (devgui.cpp, sin portar): el menu devgui en pantalla nunca esta abierto en el arnes.
bool __cdecl DevGui_IsActive() { return false; }
// CL_IsKeyPressed (cl_keys.cpp, cliente): ButtonPressed() del GSC de desarrollo; el jugador de prueba no tiene teclado.
int __cdecl CL_IsKeyPressed(int, const char *) { return 0; }
// CL_AddDebug* (cl_debugdata.cpp, dibujo de depuracion del cliente): line()/print3d()/... de los /# #/ del GSC; el arnes no los dibuja.
void __cdecl CL_AddDebugString(const float *, const float *, float, char *, int) {}
void __cdecl CL_AddDebugSphere(const float *, float, const float *, int, int, int) {}
void __cdecl CL_AddDebugLine(const float *, const float *, const float *, int, int) {}
void __cdecl CL_AddDebugAxis(const float *, const float (*)[3], float, int, int) {}
void __cdecl CL_AddDebugStarWithText(const float *, const float *, const float *, char *, float, int) {}
void __cdecl CL_AddDebugStar(const float *, const float *, int) {}
void __cdecl CL_AddDebugBox(const float *, const float *, const float *, float, const float *, int, int) {}
void __cdecl CL_AddDebugBox(const float (*)[3], const float *, const float *, const float *, const float *, int, int) {}

#ifndef __ANDROID__
#include <execinfo.h>
void h0_bt_pub() { void *b[24]; int n = backtrace(b, 24); char **s = backtrace_symbols(b, n); for (int i = 1; i < n; ++i) { const char *p = strstr(s[i], " _"); fprintf(stderr, "    #%d %s\n", i, p ? p + 1 : s[i]); } }   // TMPDIAG3
#else
void h0_bt_pub() {}   // bionic: backtrace() es API 33; el manejador de senales de rt2.cpp ya recorre la pila
#endif

// Live_GetNecessaryBandwidth (console_mp.cpp, TU de cliente): band_dedicated lo registra Com_InitDvars (h5_cominitdvars.inc).
int __cdecl Live_GetNecessaryBandwidth() { return band_dedicated->current.integer; }
