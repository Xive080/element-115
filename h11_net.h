// OLA 11: co-op en red (h11_net.cpp = sockets + bomba del servidor; h11_net_cl.cpp = cliente remoto)
#pragma once
struct netadr_t;

// Limites de seguridad (anfitrion y cliente)
enum
{
    H11_MAX_DATAGRAM = 1400 * 2,      // mayor datagrama aceptado: fragmentos de netchan (MAX_PACKETLEN 1264) y usercmds (<= 2048+cabecera)
    H11_MAX_PKTS_PER_PUMP = 256,      // paquetes por vuelta de la bomba (no deja al servidor sin frame por una inundacion)
    H11_OOB_PER_SEC = 12,             // fuera de banda por direccion y segundo (getchallenge/connect/h11info)
};

struct H11Stats
{
    unsigned long long bytesIn, bytesOut, pktsIn, pktsOut, pktsRejected, pktsDroppedSim;
    int oobRejectLog, cmdRejectLog;
};
extern H11Stats g_h11Stats;

double H11_NowMs();
int H11_DefaultPort();
bool H11_SockOpen(int port);       // port 0 = efimero (cliente)
int H11_SockPort();
void H11_SockClose();
void H11_FlushDelayed();
void H11_SetOobHook(bool (*h)(const netadr_t &from, const unsigned char *data, int len));
void H11_ServerPump();
void H11_Install();                // anfitrion (H11_HOST=1), desde H5P_Run
bool H11_HostActive();
int H11_MaxPlayers();
