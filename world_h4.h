#pragma once
// H4: mapa 3D estatico con camara libre (world_h4.cpp)
struct H4Input { float fwd, strafe, up, dyaw, dpitch; bool prone; bool sprint; };   // ejes [-1,1] y giro en grados de este frame; up>0.5 salto, up<-0.5 agachado, prone tumbado; sprint = boton +sprint (H5f; ademas del automatico)
bool H4_Setup(int W, int H);
void H4_Update(const H4Input &in, float dt);
void H4_Frame();
void H4_GetCam(float *org, float *ang);
int H4_LastDrawn(); int H4_LastPasses();
// H5b: puente servidor -> render (h5_game.cpp, H5_VIEW=1)
struct Material; struct XSurface; struct GfxPackedVertex;
extern void (*g_h4DrawExtra)(unsigned char techType);
extern int g_h4SkinnedDrawn, g_h4SkinnedSkipped, g_h4SkinnedTech[256];
void H4_DrawSkinnedSurf(Material *m, XSurface *xsurf, GfxPackedVertex *verts, unsigned char techType);
void H4_SetCam(const float *org, const float *ang);
void H4_Shot(const char *tag);
// H5f: capa 2D sobre el frame 3D (HUD). Si esta puesto, H4_Frame abre una lista de comandos 2D del renderer ORIGINAL
// (R_BeginFrame/R_AddCmdProjectionSet2D/.../R_IssueRenderCommands, que presenta) en lugar de Present directo. Recibe ancho y alto.
extern void (*g_h4Draw2D)(int W, int H);
// OLA 6c: origen, ejes y tangentes de medio FOV de la vista del frame (para la camara de los FX, h5_fx.cpp)
void H4_GetView(float *org, float (*axis)[3], float *tanHalfX, float *tanHalfY);
