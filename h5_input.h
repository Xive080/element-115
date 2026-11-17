#pragma once
// H5f: ENTRADA DEL JUGADOR del bucle de juego (h5_play.cpp). Es lo unico que la app (toques DS / mando) tiene que rellenar cada
// frame de render: ejes + botones. h5_play.cpp lo convierte en el usercmd del jugador del servidor ORIGINAL como el cliente local
// (CL_CreateCmd -> CL_CmdButtons -> CL_FinishMove): movimiento y vista por la capa de H4 (sprint automatico, maquina de postura de
// cl_input_mp), y los botones con los bits ORIGINALES del usercmd (H5P_UsercmdBit). Un toque corto entre dos frames de servidor no se
// pierde: igual que kbutton_t.wasPressed, una pulsacion queda retenida hasta el siguiente usercmd.
enum : unsigned
{
    H5IN_FIRE = 1u << 0,      // +attack        -> bit 0
    H5IN_ADS = 1u << 1,       // +speed_throw   -> bit 11 (apuntar)
    H5IN_RELOAD = 1u << 2,    // +reload        -> bit 5 (tambien "usar" sobre un trigger con pista, como el original)
    H5IN_MELEE = 1u << 3,     // +melee         -> bit 2 (cuchillo)
    H5IN_USE = 1u << 4,       // +activate      -> bit 3 (comprar pared/puerta/perk/caja, reparar barricada)
    H5IN_JUMP = 1u << 5,      // +gostand       -> salto (bit 10) / levantarse un nivel de postura (IN_UpDown)
    H5IN_STANCE = 1u << 6,    // +stance        -> toque agacharse/levantarse, mantener tumbarse (cl_stanceHoldTime), corriendo plancha
    H5IN_PRONE = 1u << 7,     // +prone         -> tumbarse / levantarse (CL_ToggleStance)
    H5IN_SPRINT = 1u << 8,    // +sprint        -> bit 1 (ademas del sprint automatico con el stick a tope)
    H5IN_NEXTWEAPON = 1u << 9,// weapnext       -> usercmd.weapon = siguiente primaria que lleva (CycleWeapPrimary)
    H5IN_FRAG = 1u << 10,     // +frag          -> bit 14 (granada letal)
    H5IN_SMOKE = 1u << 11,    // +smoke         -> bit 15 (granada tactica / mono en zombies)
};
struct H5PlayerInput
{
    float moveFwd, moveRight;   // stick izquierdo [-1,1] (adelante +, derecha +)
    float lookYaw, lookPitch;   // giro de la vista en ESTE frame, grados (izquierda +, abajo +), como H4Input dyaw/dpitch
    unsigned buttons;           // H5IN_* mantenidos en este frame
    unsigned weaponRequest;     // 0 o indice de arma (seleccion directa; la app normalmente usa H5IN_NEXTWEAPON)
};
// La app llama a esto cada frame antes de H5P_Step (o pone el proveedor). Sin proveedor, el bucle usa el guion H5_PLAY_SCRIPT.
extern void (*g_h5pInputProvider)(H5PlayerInput *in, float dt);
// Bit del usercmd (button_bits) de cada H5IN_* (-1 si no es un bit: NEXTWEAPON, STANCE, PRONE, JUMP pasan por la capa de postura).
int H5P_UsercmdBit(unsigned h5inButton);
