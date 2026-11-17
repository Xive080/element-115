// OLA 6b: luz de modelos dinamicos (actores, jugador en camara chase, viewmodel) con el LIGHTGRID ORIGINAL del mapa.
//
// En el motor completo: R_AddDObjToScene(lightingOrigin) -> R_AllocModelLighting_* -> hilo de trabajo R_CalcModelLighting ->
// R_GetLightingAtPoint (rb_light.cpp: R_LightGridLookup + 8 trazas de visibilidad + mezcla de colores) -> parche en
// frontEndDataOut->modelLightingPatchList -> RB_PatchModelLighting (R_ApplyLightGridColorsPatch / luz de suelo) -> atlas de volumen
// 256 x H x 4 A8R8G8B8 (imagen de programa 12) que los sombreadores lit leen con CONST_SRC_CODE_BASE_LIGHTING_COORDS.
// Aqui se llama a R_GetLightingAtPoint ORIGINAL y a R_ApplyLightGridColorsPatch ORIGINAL; lo que cambia es la gestion del atlas:
//  - 64 entradas fijas (una fila de bloques 4x4x4): el atlas mide 256x4x4 (16 KB) en vez de 256x256x4 (1 MB); las coordenadas y la
//    escala de busqueda son las de R_SetModelLightingCoords / R_SetModelLightingLookupScale con imageHeight = 4.
//  - Copia CPU persistente del atlas: el backend Vulkan da un staging NUEVO en cada LockBox (no conserva el contenido), asi que se sube
//    el atlas entero (16 KB) solo cuando cambia, y ANTES de abrir el pase del frame (H4 g_h4PreFrame) para no partir el render pass.
//  - Cache por entrada como r_cacheModelLighting: se recalcula si cambia la entidad o el origen se mueve mas de H5L_TOL unidades.
// Luz primaria: R_GetNonSunPrimaryLightForBox ORIGINAL (regiones de luz del mapa; caja = origen de luz +-(16,16,36) para actores y
// +-8 para el viewmodel, aproximacion de los limites de la entidad) y tecnica TECHNIQUE_LIT + desplazamiento por tipo, como
// gfxDrawMethod.litTechType sin sombras: ninguna 10, sol 11, foco 13, omni 15. Para foco/omni, antes del modelo se llama a
// R_SetLightProperties ORIGINAL con el GfxLight de R_InitPrimaryLights ORIGINAL (sin mapa de sombras). H5L_OFF=1: luz neutra;
// H5L_PRIMARY=0: solo lightgrid (TECHNIQUE_LIT).
#include <database/db_registry.h>
#include <universal/ptr32.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/rb_light.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_image_load_common.h>
#include <gfx_d3d/r_bsp.h>
#include <d3d9.h>
#include <qcommon/com_bsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <game_mp/g_main_mp.h>
#include <server_mp/server_mp.h>
#include "world_h4.h"
#include <gfx_d3d/r_primarylights.h>
#include <gfx_d3d/r_draw_shadowablelight.h>
#include <gfx_d3d/rb_backend.h>
#include "h5_light_prim.inc"   // extraidas tal cual: Com_GetPrimaryLight(Count) (cg_ents_mp.cpp), Com_CanPrimaryLightAffectPoint (com_bsp.cpp),
                               // R_InitPrimaryLights (r_bsp_load_obj.cpp), R_RegisterLightDef(_FastFile) (r_light.cpp)

extern GfxImage *(*g_h4ModelLightImage)();   // world_h4.cpp
extern void (*g_h4PreFrame)();
extern float g_h4ModelLightCoords[4];
extern unsigned char g_h4ModelLitTech;

namespace {
// OLA 7e: el numero de entradas ya no es fijo (64): con H7E_SMLIGHT (por defecto) son H7E_ATLAS (1024) para que TODOS los smodels
// visibles tengan su luz (antes solo 24 y el resto la luz neutra 0x80). kH = 4 filas por cada 64 entradas, como R_SetModelLightingCoords.
const int kMaxEntries = 4096, kW = 256, kD = 4, kRow = kW * 4, kMaxH = 4 * (kMaxEntries / 64);
int kEntries = 64, kH = 4, kSlice = kRow * 4;
unsigned char *g_atlas;
const int kKeys = 1024 + 16384 + 1;   // claves: -1 viewmodel, 0..1023 entidades, 1024+i smodels (H7E) -> indice key + 1
GfxImage *g_img;
bool g_dirty, g_on, g_init, g_said;
struct Entry { int key = -2; float org[3] = { 1e30f, 1e30f, 1e30f }; unsigned char primary = 0; unsigned frame = 0; };
Entry g_ent[kMaxEntries];
int g_slotOf[kKeys];   // OLA 7e: clave -> entrada (persistente entre frames: cache como r_cacheSModelLighting)
int g_clock;
unsigned g_frame;
int g_bindKey[kKeys];   // entnum (o -1 viewmodel) -> entrada del frame (indexado por key + 1)
double g_ms, g_msMax; int g_calc, g_frames;
float g_tolSq = 16.0f;
int g_sunIndex;
GfxLight *g_lights;     // R_InitPrimaryLights (refdef->primaryLights en el cliente)
bool g_primary = true;

double NowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

GfxImage *Image()
{
    if (g_img || !g_on) return g_img;
    g_img = Image_AllocProg(12, 4u, 1u);   // como R_InitModelLightingImage (imagen de programa 12)
    Image_SetupAndLoad(g_img, kW, kH, kD, 10, D3DFMT_A8R8G8B8);
    memset(g_atlas, 0x80, (size_t)kSlice * kD);
    g_dirty = true;
    return g_img;
}

// RB_PatchModelLighting (r_model_lighting.cpp) sobre la copia CPU, con sus mismos desplazamientos de muestra
void ApplyPatches(unsigned from, unsigned to)
{
    static bool deltas;
    if (!deltas) { R_SetLightGridSampleDeltas(kRow, kSlice); deltas = true; }
    for (unsigned p = from; p < to; ++p) {
        const GfxModelLightingPatch *patch = &frontEndDataOut->modelLightingPatchList[p];
        const unsigned idx = patch->modelLightingIndex;
        unsigned char *pixels = g_atlas + 16 * (idx & 0x3F) + kRow * ((idx >> 4) & 0xFFFFFFFC);
        if (patch->colorsCount) R_ApplyLightGridColorsPatch(patch, pixels);
        else for (int dz = 0; dz < 4; ++dz) for (int dy = 0; dy < 4; ++dy) for (int i = 0; i < 4; ++i)
            memcpy(pixels + kSlice * dz + kRow * dy + 4 * i, patch->groundLighting, 4);
    }
    g_dirty = true;
}

// R_CalcModelLighting para una entrada: R_GetLightingAtPoint ORIGINAL (el parche que deja se aplica al momento y se retira de la lista)
unsigned char Calc(int entry, const float *org, const float *half, bool hero, int forcedPrimary = -1)
{
    const GfxLightGrid *lg = &rgp.world->lightGrid;
    const unsigned before = frontEndDataOut->modelLightingPatchCount;
    // forcedPrimary >= 0: smodel (R_SetStaticModelLighting: la luz primaria precalculada en GfxStaticModelDrawInst::primaryLightIndex)
    const unsigned nonSun = !g_primary ? 0 : forcedPrimary >= 0 ? (unsigned)forcedPrimary : R_GetNonSunPrimaryLightForBox(nullptr, org, half);   // R_GetPrimaryLightForBoxCallback
    const unsigned primary = R_GetLightingAtPoint(lg, org, nonSun, (unsigned short)entry, GFX_MODELLIGHT_EXTRAPOLATE, hero);
    ApplyPatches(before, frontEndDataOut->modelLightingPatchCount);
    frontEndDataOut->modelLightingPatchCount = before;
    ++g_calc;
    return (unsigned char)primary;
}

void Upload()
{
    if (!g_dirty || !g_img) return;
    IDirect3DVolumeTexture9 *vt = g_img->texture.volmap.get();
    D3DLOCKED_BOX lb;
    if (vt && vt->LockBox(0, &lb, nullptr, 0) == 0) {
        for (int z = 0; z < kD; ++z) for (int y = 0; y < kH; ++y)   // TODO rendimiento: subir solo las filas tocadas
            memcpy((char *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch, g_atlas + z * kSlice + y * kRow, kRow);
        vt->UnlockBox(0);
    }
    g_dirty = false;
}

unsigned char TechFor(unsigned char primary)
{
    if (!g_primary || !primary || primary >= Com_GetPrimaryLightCount()) return 10;
    switch (Com_GetPrimaryLight(primary)->type) {
    case 1: return 11;                                                         // TECHNIQUE_LIT_SUN
    case 2: case 3: {                                                          // TECHNIQUE_LIT_SPOT / TECHNIQUE_LIT_OMNI
        GfxLight *l = &g_lights[primary];
        if (!l->def || l->radius <= 0.0f) return 10;
        // H4 dibuja sin R_Set3D (viewMode queda en 0 y eyeOffset en el valor con el que se coloco el mundo): se marca 3D solo para esta
        // llamada, asi la posicion de la luz (origin - eyeOffset) queda en el mismo sistema que la colocacion identidad de H4
        GfxCmdBufSourceState *src = gfxCmdBufContext.source; const GfxViewMode vm = src->viewMode; src->viewMode = VIEW_MODE_3D;
        R_SetLightProperties(src, l, l->def, LIGHT_HAS_NO_SHADOWMAP, 0.0f);   // R_SetDrawSurfsShadowableLight sin sombra
        src->viewMode = vm;
        return l->type == 2 ? 13 : 15; }
    default: return 10;
    }
}
} // namespace

// Reserva (o reutiliza) la entrada de `key` (entnum, -1 = viewmodel) y calcula su luz si hace falta. Se llama antes del pase.
static void PrepareEx(int key, const float *lightingOrigin, const float *half, bool hero, int forcedPrimary)
{
    if (!g_on || !rgp.world || key < -1 || key >= kKeys - 1) return;
    // OLA 7e: la entrada de la clave se busca por tabla (antes hash + sondeo: una clave podia caer en un hueco libre antes de llegar a
    // su entrada cacheada y recalcular). Si no tiene, se toma por reloj la primera entrada no usada en este frame.
    int slot = g_slotOf[key + 1];
    if (slot < 0 || g_ent[slot].key != key) {
        slot = -1;
        for (int n = 0; n < kEntries; ++n) { const int c = g_clock; g_clock = (g_clock + 1) % kEntries;
            if (g_ent[c].frame != g_frame || g_ent[c].key == -2) { slot = c; break; } }
        if (slot < 0) return;   // atlas lleno este frame: se dibuja con la entrada que tenga (como MODEL_LIGHTING_EXHAUSTED)
        if (g_ent[slot].key >= -1 && g_slotOf[g_ent[slot].key + 1] == slot) g_slotOf[g_ent[slot].key + 1] = -1;
        g_ent[slot].key = -3;   // fuerza el calculo
        g_slotOf[key + 1] = slot;
    }
    Entry &e = g_ent[slot];
    float dx = lightingOrigin[0] - e.org[0], dy = lightingOrigin[1] - e.org[1], dz = lightingOrigin[2] - e.org[2];
    if (e.key != key || dx * dx + dy * dy + dz * dz > g_tolSq) {
        const double t0 = NowMs();
        const unsigned char got = Calc(slot, lightingOrigin, half, hero, forcedPrimary);
        e.primary = forcedPrimary >= 0 ? (unsigned char)forcedPrimary : got;   // R_SetStaticModelLighting deja la primaria precalculada
        memcpy(e.org, lightingOrigin, 12); e.key = key;
        const double dt = NowMs() - t0; g_ms += dt; if (dt > g_msMax) g_msMax = dt;
    }
    e.frame = g_frame;
    g_bindKey[key + 1] = slot;
}
void H5L_Prepare(int key, const float *lightingOrigin, const float *half, bool hero) { PrepareEx(key, lightingOrigin, half, hero, -1); }
// OLA 7e: smodel (R_SetStaticModelLighting): lightgrid en su lightingOrigin con SU luz primaria precalculada, sin luz de heroe
void H5L_PrepareStatic(int key, const float *lightingOrigin, int primaryLightIndex)
{
    static const float half[3] = { 16, 16, 16 };
    PrepareEx(key, lightingOrigin, half, false, primaryLightIndex >= 0 && primaryLightIndex < (int)Com_GetPrimaryLightCount() ? primaryLightIndex : 0);
}
int H5L_MaxKey() { return kKeys - 2; }
GfxLight *H5L_Lights() { return g_lights; }   // OLA 7e: refdef->primaryLights del port (h7e_light.cpp emula CG_PrimaryLight de las luces de cliente)
float H5L_InvImageHeight() { return 1.0f / (float)kH; }   // para R_SetModelLightingLookupScale (world_h4)

// Antes de dibujar las superficies de `key`: coordenadas de su bloque (R_SetModelLightingCoords con invImageHeight = 1/4) y tecnica
void H5L_Bind(int key)
{
    if (!g_on) return;
    const int slot = key < -1 || key >= kKeys - 1 ? -1 : g_bindKey[key + 1];
    if (slot < 0 || g_ent[slot].key != key) { g_h4ModelLightCoords[0] = g_h4ModelLightCoords[1] = g_h4ModelLightCoords[2] = 0.5f; g_h4ModelLitTech = 10; return; }
    g_h4ModelLightCoords[0] = (4.0f * (slot & 0x3F) + 2.0f) / 256.0f;
    g_h4ModelLightCoords[1] = (((slot >> 4) & ~3) + 2.0f) / (float)kH;
    g_h4ModelLightCoords[2] = 0.5f; g_h4ModelLightCoords[3] = 1.0f;
    g_h4ModelLitTech = TechFor(g_ent[slot].primary);
}

// Entradas del frame: actores y cadaveres (lightingOrigin = pose.origin + 32, como CG_Actor), el jugador en camara chase
// (g_h5vDrawPlayer) y el viewmodel (ojo del jugador, como CG_AddViewWeapon: ps->origin + viewHeightCurrent).
extern bool g_h5vDrawPlayer;
static void PrepareAll()
{
    static const float maxDist = getenv("H5_SKIN_MAXDIST") ? (float)atof(getenv("H5_SKIN_MAXDIST")) : 0.0f;
    float cam[3], ang[3]; H4_GetCam(cam, ang);
    static const float halfVM[3] = { 8, 8, 8 }, halfActor[3] = { 16, 16, 36 };
    H5L_Prepare(-1, cam, halfVM, true);
    for (int e = 0; e < level.num_entities && e < 2047; ++e) {
        gentity_s *ent = &g_entities[e];
        if (!ent->r.inuse || (ent->s.eType != ET_ACTOR && ent->s.eType != ET_ACTOR_CORPSE && !(g_h5vDrawPlayer && ent->s.eType == ET_PLAYER))) continue;
        float o[3] = { ent->r.currentOrigin[0], ent->r.currentOrigin[1], ent->r.currentOrigin[2] + 32.0f };
        if (maxDist > 0) { float d0 = o[0] - cam[0], d1 = o[1] - cam[1], d2 = o[2] - cam[2]; if (d0 * d0 + d1 * d1 + d2 * d2 > maxDist * maxDist) continue; }
        H5L_Prepare(e, o, halfActor, true);
    }
}
void (*g_h5lPrepareAll)() = PrepareAll;
static void PreFrame()
{
    ++g_frame;
    for (int i = 0; i < kKeys; ++i) g_bindKey[i] = -1;
    { extern void H7E_ClientFrame() __attribute__((weak)); if (H7E_ClientFrame) H7E_ClientFrame(); }   // OLA 7e: estado de cliente (luces, vision, modelos)
    if (g_h5lPrepareAll) g_h5lPrepareAll();
    Upload();
    if (++g_frames % 600 == 0 && getenv("H5L_STATS")) {
        fprintf(stderr, "[H5L] %d frames: %d calculos de luz, %.3f ms/frame de media (max %.3f ms por calculo)\n", 600, g_calc, g_ms / 600, g_msMax);
        g_calc = 0; g_ms = 0; g_msMax = 0;
    }
}

void H5L_Install()
{
    if (g_init) return; g_init = true;
    g_on = !(getenv("H5L_OFF") && atoi(getenv("H5L_OFF")));
    if (const char *t = getenv("H5L_TOL")) g_tolSq = (float)(atof(t) * atof(t));
    if (const char *t = getenv("H5L_PRIMARY")) g_primary = atoi(t) != 0;
    if (!g_on) return;
    {   // OLA 7e: tamano del atlas (H7E_SMLIGHT=0 vuelve a 64 entradas)
        const bool sm = !(getenv("H7E_SMLIGHT") && !atoi(getenv("H7E_SMLIGHT")));
        int n = getenv("H7E_ATLAS") ? atoi(getenv("H7E_ATLAS")) : (sm ? 1024 : 64);
        n = n < 64 ? 64 : n > kMaxEntries ? kMaxEntries : (n + 63) / 64 * 64;
        kEntries = n; kH = 4 * (n / 64); kSlice = kRow * kH;
        g_atlas = (unsigned char *)malloc((size_t)kSlice * kD);
        fprintf(stderr, "[H7E] atlas de luz de modelos: %d entradas, %dx%dx%d (%d KB)\n", kEntries, kW, kH, kD, kSlice * kD / 1024);
    }
    for (int i = 0; i < kKeys; ++i) { g_bindKey[i] = -1; g_slotOf[i] = -1; }
    g_h4ModelLightImage = Image;
    g_h4PreFrame = PreFrame;
    { extern unsigned char (*g_h4WorldPrimaryTech)(unsigned char); g_h4WorldPrimaryTech = TechFor; }   // OLA 7g: tambien el mundo (lotes por luz primaria)
    if (rgp.world) {
        const GfxLightGrid *lg = &rgp.world->lightGrid;
        fprintf(stderr, "[H5L] lightgrid: sol=%u regiones=%d mins=(%u,%u,%u) maxs=(%u,%u,%u) ejes fila=%u col=%u entradas=%u colores=%u primarias=%u (comWorld %s)\n",
            lg->sunPrimaryLightIndex, (int)lg->hasLightRegions, lg->mins[0], lg->mins[1], lg->mins[2], lg->maxs[0], lg->maxs[1], lg->maxs[2],
            lg->rowAxis, lg->colAxis, lg->entryCount, lg->colorCount, rgp.world->primaryLightCount, comWorld.isInUse ? "en uso" : "SIN CARGAR");
        g_sunIndex = lg->sunPrimaryLightIndex;
        g_lights = (GfxLight *)calloc(rgp.world->primaryLightCount + 1, sizeof(GfxLight));
        R_InitPrimaryLights(g_lights);
        int n[4] = { 0, 0, 0, 0 }, nodef = 0;
        for (unsigned i = 0; i < rgp.world->primaryLightCount; ++i) { if (g_lights[i].type < 4) ++n[g_lights[i].type]; if (g_lights[i].type >= 2 && !g_lights[i].def) ++nodef; }
        fprintf(stderr, "[H5L] luces primarias: %d ninguna, %d sol, %d focos, %d omni (%d sin GfxLightDef)\n", n[0], n[1], n[2], n[3], nodef);
    }
}
