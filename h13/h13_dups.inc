// OLA 13: funciones ya definidas en otro sitio del harness (extraidas con h5_extract.py o pegamento de ui_p5.cpp); en las TUs del HUD
// se renombran para no duplicar el simbolo (el codigo es el mismo).
#pragma once
#define Hunk_AllocXAnimClient H13dup_Hunk_AllocXAnimClient            // h5_weapon.cpp (h5_weapon_cgmain.inc)
#define CG_GetPredictedPlayerState H13dup_CG_GetPredictedPlayerState  // h5_weapon.cpp (h5_weapon_cgmain.inc)
#define ConvertQuatToMat H13dup_ConvertQuatToMat                      // h5_game.cpp (h5_misc8.inc)
#define Vec2NormalizeTo H13dup_Vec2NormalizeTo                        // h5_game.cpp (h5_misc8.inc)
#define Material_FromHandle H13dup_Material_FromHandle                // main2.cpp
#define Window_GetRect H13dup_Window_GetRect                          // ui_p5.cpp
#define ScopedScrPlaceViewStack H13dup_ScopedScrPlaceViewStack        // ui_p5.cpp
#define cg_draw2D H13dup_cg_draw2D                                    // main2.cpp (mismo dvar por nombre tras CG_RegisterDvars)
// Funciones que el resto del harness tenia como stub (safe0 o nunca llamadas) y que el servidor alcanza: se mantienen fuera para no
// cambiar su camino (CG_AllocateClientMemory pediria el hunk de cliente y SV_SpawnServer se queda sin memoria; CG_IsMature es de la Ola 7d).
#define CG_AllocateClientMemory_SizeRequired H13dup_CG_AllocateClientMemory_SizeRequired
#define CG_AllocateClientMemory H13dup_CG_AllocateClientMemory
#define CG_FreeClientMemory H13dup_CG_FreeClientMemory
#define CL_GetLocalClientConnection H13dup_CL_GetLocalClientConnection
#define CG_IsMature H13dup_CG_IsMature
#define CG_GetDObjOrientation H13dup_CG_GetDObjOrientation                // h5_fx.cpp (OLA 6c) la define para los efectos
