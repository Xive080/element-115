#pragma once
// H4: jugador movido por el Pmove ORIGINAL del motor (bgame/bg_pmove.cpp + bg_slidemove.cpp + bg_jump.cpp...).
// pmove_h4.cpp solo pone el pegamento: dvars reales (BG_RegisterDvars), traza = CM_BoxTrace (G_TraceCapsule sin entidades),
// arma = WeaponDef real de la zona (m1911_zm) y no-ops para lo que no toca al movimiento (animacion, armas, eventos).
struct H4PmCmd
{
    float fwd, strafe;          // stick [-1,1] (strafe + = derecha) -> forwardmove/rightmove -127..127
    float yaw, pitch;           // angulos de vista deseados (grados)
    bool jump, crouch, prone, sprint;   // botones del usercmd (bits 10, 9, 8, 1)
    bool dive;                          // bit 0x2C (CL_STANCE_DIVE_TO_PRONE: CL_AddCurrentStanceToCmd pone 0x2C + 8)
};
struct H4PmState { float origin[3], velocity[3], viewangles[3]; float viewHeight; int pmFlags; bool onGround, sprinting, crouched, prone;
                   int forcedStance; };   // ultimo EV_STANCE_FORCE_* del Pmove (0 de pie, 1 agachado, 2 tumbado; -1 ninguno) -> CL_SetStance en cg_event
bool H4Pm_Spawn(const float *feet, float yaw);   // inicializa dvars/arma/playerState (una vez) y coloca al jugador
void H4Pm_Move(const H4PmCmd &c, int msec);       // un usercmd de msec ms -> Pmove_1 (subpasos <=66 ms del original)
void H4Pm_Get(H4PmState *s);
void H4Pm_Teleport(const float *origin);          // solo pruebas: mueve al jugador y le quita la velocidad (conserva postura)
void H4Pm_PrintDvars();                           // vuelca los dvars/constantes de movimiento (prueba H4_PHYS)
