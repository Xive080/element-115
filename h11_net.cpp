// OLA 11: CO-OP EN RED. Capa de plataforma de red (sockets BSD UDP) bajo la red ORIGINAL del motor (qcommon/net_chan_mp.cpp:
// NET_SendPacket -> Sys_SendPacket, NET_GetClientPacket -> Sys_GetPacket) y bomba de paquetes del servidor (Com_ServerPacketEvent ->
// SV_PacketEvent: SV_ConnectionlessPacket / Netchan_Process / SV_ExecuteClientMessage, todo original).
// Lo de win32/win_net.cpp (winsock, no se compila en el harness) se reescribe aqui con la misma semantica y IPv4.
// Activacion (anfitrion, "listen server": servidor + jugador local en el mismo proceso, dentro de H5P_Run):
//   H11_HOST=1          abre el puerto UDP y acepta jugadores remotos (sin la variable no se abre ningun socket)
//   H11_PORT=n          puerto UDP (3074 por defecto: el mismo que NET_StringToAdr pone si la direccion no lleva puerto)
//   H11_MAXPLAYERS=n    jugadores en total, incluido el local (1..4, 4 por defecto) -> sv_maxclients
//   H11_NAME=texto      nombre de la partida en la lista de cercanas (por defecto "Partida"; nada del usuario ni del equipo)
// Simulacion de red (en el proceso que envia): H11_LOSS=pct (perdida), H11_LAG=ms (retardo fijo), H11_JITTER=ms (retardo aleatorio extra).
// Seguridad: solo los comandos fuera de banda del protocolo de conexion pasan al servidor (getchallenge, connect y el sondeo de la
// lista h11info); rcon, voz, steamauth y el resto se descartan. Limite de ritmo por direccion para los fuera de banda, tamano maximo
// de paquete, tope de paquetes por frame y lista blanca de comandos de cliente remotos (g_h0ClientCmdFilter, sv_client_mp.cpp).
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <qcommon/msg_mp.h>
#include <qcommon/net_chan_mp.h>
#include <server_mp/server_mp.h>
#include <server_mp/sv_main_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_sp/g_sp_client.h>
// compat/Windows.h (prelude) declara su propio in_addr/inet_addr: las del sistema entran renombradas (misma disposicion de 4 bytes).
#define in_addr h11_sys_in_addr
#define inet_addr h11_sys_inet_addr
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#undef in_addr
#undef inet_addr
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <string>

#include "h11_net.h"

extern bool (*g_h0ClientCmdFilter)(client_t *cl, const char *cmd);   // sv_client_mp.cpp (SV_ExecuteClientCommand)
extern void (*g_h5pNetPump)();                                       // h5_play.cpp: una vez por frame de render

static int s_sock = -1;
static int s_boundPort;
H11Stats g_h11Stats;

double H11_NowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

static int EnvInt(const char *n, int def) { const char *v = getenv(n); return v && *v ? atoi(v) : def; }

int H11_DefaultPort() { int p = EnvInt("H11_PORT", 3074); return p > 0 && p < 65536 ? p : 3074; }

bool H11_SockOpen(int port)
{
    if (s_sock >= 0) return true;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { fprintf(stderr, "[H11] socket: %s\n", strerror(errno)); return false; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);   // lista de partidas cercanas (sondeo por broadcast)
    if (port > 0) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((unsigned short)port);
    if (bind(s, (sockaddr *)&a, sizeof a) < 0) { fprintf(stderr, "[H11] bind al puerto UDP %d: %s\n", port, strerror(errno)); close(s); return false; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    socklen_t al = sizeof a; getsockname(s, (sockaddr *)&a, &al); s_boundPort = ntohs(a.sin_port);
    s_sock = s;
    fprintf(stderr, "[H11] socket UDP abierto en el puerto %d\n", s_boundPort);
    return true;
}
int H11_SockPort() { return s_boundPort; }
void H11_SockClose() { if (s_sock >= 0) { close(s_sock); s_sock = -1; } }

// ---------------------------------------------------------------------------------------------------------------
// Simulacion de perdida / retardo / jitter en el envio (pruebas de estabilidad de la fase 2)
struct H11Delayed { double due; sockaddr_in to; std::string data; };
static std::deque<H11Delayed> s_delayQ;
static int s_simLoss = -1, s_simLag, s_simJitter;
static void H11_SimInit() { if (s_simLoss >= 0) return; s_simLoss = EnvInt("H11_LOSS", 0); s_simLag = EnvInt("H11_LAG", 0); s_simJitter = EnvInt("H11_JITTER", 0); if (s_simLoss || s_simLag || s_simJitter) fprintf(stderr, "[H11] simulacion de red: perdida %d%%, retardo %d ms, jitter %d ms\n", s_simLoss, s_simLag, s_simJitter); }
static void H11_RawSend(const void *data, size_t len, const sockaddr_in &to)
{
    if (s_sock < 0) return;
    if (sendto(s_sock, data, len, 0, (const sockaddr *)&to, sizeof to) >= 0) { g_h11Stats.bytesOut += len; ++g_h11Stats.pktsOut; }
}
void H11_FlushDelayed()
{
    const double now = H11_NowMs();
    while (!s_delayQ.empty() && s_delayQ.front().due <= now) { H11_RawSend(s_delayQ.front().data.data(), s_delayQ.front().data.size(), s_delayQ.front().to); s_delayQ.pop_front(); }
}

static void NetadrToSin(const netadr_t &a, sockaddr_in *s)
{
    memset(s, 0, sizeof *s); s->sin_family = AF_INET; s->sin_port = a.port;   // netadr_t.port ya va en orden de red (BigShort)
    if (a.type == NA_BROADCAST) s->sin_addr.s_addr = htonl(INADDR_BROADCAST); else memcpy(&s->sin_addr.s_addr, a.ip, 4);
}
static void SinToNetadr(const sockaddr_in &s, netadr_t *a)
{
    memset(a, 0, sizeof *a); a->type = NA_IP; memcpy(a->ip, &s.sin_addr.s_addr, 4); a->port = s.sin_port; a->addrHandleIndex = -1;
}

// win_net.cpp Sys_SendPacket (rama kcod4): solo NA_IP / NA_BROADCAST por el socket IP.
char __cdecl Sys_SendPacket(unsigned int length, unsigned __int8 *data, netadr_t to)
{
    if (to.type != NA_IP && to.type != NA_BROADCAST) { fprintf(stderr, "[H11] Sys_SendPacket: tipo de direccion %d no soportado\n", (int)to.type); return 0; }
    if (s_sock < 0) return 1;   // como el original sin socket: no es error
    sockaddr_in s; NetadrToSin(to, &s);
    H11_SimInit();
    if (s_simLoss > 0 && rand() % 100 < s_simLoss) { ++g_h11Stats.pktsDroppedSim; return 1; }
    if (s_simLag > 0 || s_simJitter > 0)
    {
        double due = H11_NowMs() + s_simLag + (s_simJitter > 0 ? rand() % (s_simJitter + 1) : 0);
        // UDP no garantiza orden: con jitter los paquetes pueden adelantarse (se inserta por tiempo)
        auto it = s_delayQ.end(); while (it != s_delayQ.begin() && (it - 1)->due > due) --it;
        s_delayQ.insert(it, H11Delayed{ due, s, std::string((const char *)data, length) });
        return 1;
    }
    H11_RawSend(data, length, s);
    return 1;
}

// Sondeo de la lista de partidas cercanas (fase 3). Se contesta aqui y no llega al motor.
static bool (*s_oobHook)(const netadr_t &from, const unsigned char *data, int len);
void H11_SetOobHook(bool (*h)(const netadr_t &, const unsigned char *, int)) { s_oobHook = h; }

// win_net.cpp Sys_GetPacket: un datagrama por llamada, con el tamano comprobado (los mayores que el buffer se descartan).
int __cdecl Sys_GetPacket(netadr_t *net_from, msg_t *net_message)
{
    H11_FlushDelayed();
    if (s_sock < 0) return 0;
    for (int guard = 0; guard < 64; ++guard)
    {
        sockaddr_in from; socklen_t fl = sizeof from;
        const int cap = net_message->maxsize < H11_MAX_DATAGRAM ? net_message->maxsize : H11_MAX_DATAGRAM;
        const ssize_t r = recvfrom(s_sock, net_message->data, (size_t)cap, 0, (sockaddr *)&from, &fl);
        if (r < 0) return 0;   // EWOULDBLOCK: no hay mas
        g_h11Stats.bytesIn += (unsigned long long)r; ++g_h11Stats.pktsIn;
        if (r >= cap || r < 4 || from.sin_family != AF_INET) { ++g_h11Stats.pktsRejected; continue; }   // demasiado grande o runt
        SinToNetadr(from, net_from);
        net_message->cursize = (int)r; net_message->readcount = 0; net_message->bit = 0; net_message->overflowed = 0;
        if (s_oobHook && *(const unsigned int *)net_message->data == 0xFFFFFFFFu && s_oobHook(*net_from, net_message->data, (int)r)) continue;
        return 1;
    }
    return 0;
}

// win_net.cpp Sys_StringToAdr: nombre o IPv4 a netadr_t (sin puerto: lo pone NET_StringToAdr).
int __cdecl Sys_StringToAdr(const char *s, netadr_t *a)
{
    addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(s, nullptr, &hints, &res) != 0 || !res) return 0;
    sockaddr_in sin = *(sockaddr_in *)res->ai_addr; freeaddrinfo(res);
    SinToNetadr(sin, a); a->port = 0;
    return 1;
}

void __cdecl NET_Sleep(unsigned int msec)
{
    if (s_sock < 0) { usleep(msec * 1000); return; }
    pollfd p = { s_sock, POLLIN, 0 }; poll(&p, 1, (int)msec);
}

// win_net.cpp Sys_IsLANAddress(_IgnoreSubnet): redes privadas como el original y ademas 100.64/10 (CGNAT: Tailscale), que es
// red privada cifrada de baja latencia. En LAN el servidor manda snapshots sin la ventana de tasa (sv_snapshot_mp.cpp).
int __cdecl Sys_IsLANAddress(netadr_t adr)
{
    if (adr.type == NA_LOOPBACK || adr.type == NA_BOT) return 1;
    if (adr.type != NA_IP) return 0;
    const unsigned char *ip = adr.ip;
    return ip[0] == 10 || ip[0] == 127 || (ip[0] == 169 && ip[1] == 254) || (ip[0] == 172 && (ip[1] & 0xF0) == 0x10) || (ip[0] == 192 && ip[1] == 168)
        || (ip[0] == 100 && (ip[1] & 0xC0) == 0x40);
}

// q_shared.cpp BigShort (TU no compilada): orden de red de los puertos (NET_StringToAdr, NET_AdrToString).
__int16 __cdecl BigShort(__int16 l) { return (__int16)(((unsigned short)l >> 8) | ((unsigned short)l << 8)); }

// Perfil de fuera de banda del cliente (cl_net_chan_mp.cpp, TU no compilada): sin perfilado de red en el harness.
void __cdecl CL_Netchan_AddOOBProfilePacket(int, int) {}

// ---------------------------------------------------------------------------------------------------------------
// SERVIDOR: bomba de paquetes con filtro. Sustituye a Com_ServerPacketEvent (common.cpp) con la misma ruta: Sys_GetPacket -> SV_PacketEvent.
struct H11Rate { netadr_t adr; double windowStart; int count; };
static H11Rate s_rate[64];
static bool H11_RateOk(const netadr_t &a)
{
    const double now = H11_NowMs(); int freeSlot = -1, oldest = 0;
    for (int i = 0; i < 64; ++i)
    {
        if (s_rate[i].adr.type == NA_IP && !memcmp(s_rate[i].adr.ip, a.ip, 4))
        {
            if (now - s_rate[i].windowStart > 1000.0) { s_rate[i].windowStart = now; s_rate[i].count = 0; }
            return ++s_rate[i].count <= H11_OOB_PER_SEC;
        }
        if (s_rate[i].adr.type != NA_IP && freeSlot < 0) freeSlot = i;
        if (s_rate[i].windowStart < s_rate[oldest].windowStart) oldest = i;
    }
    const int k = freeSlot >= 0 ? freeSlot : oldest;
    s_rate[k].adr = a; s_rate[k].windowStart = now; s_rate[k].count = 1;
    return true;
}

// Primer token del comando fuera de banda (tras los 4 bytes 0xFF), sin depender del tokenizador.
static void H11_OobVerb(const unsigned char *d, int len, char *out, int outSize)
{
    int i = 4, o = 0;
    while (i < len && o < outSize - 1 && d[i] && d[i] != ' ' && d[i] != '\n' && d[i] != '\t') out[o++] = (char)d[i++];
    out[o] = 0;
}

static bool s_hostOn;
static unsigned char *s_svBuf;
void H11_ServerPump()
{
    if (!s_hostOn || !com_sv_running || !com_sv_running->current.enabled) return;
    if (!s_svBuf) s_svBuf = (unsigned char *)malloc(H11_MAX_DATAGRAM + 16);
    msg_t m; netadr_t from;
    for (int n = 0; n < H11_MAX_PKTS_PER_PUMP; ++n)
    {
        MSG_Init(&m, s_svBuf, H11_MAX_DATAGRAM);
        if (!Sys_GetPacket(&from, &m)) break;
        if (*(unsigned int *)m.data == 0xFFFFFFFFu)
        {
            char verb[32]; H11_OobVerb(m.data, m.cursize, verb, sizeof verb);
            if (!H11_RateOk(from)) { ++g_h11Stats.pktsRejected; continue; }
            if (I_stricmp(verb, "getchallenge") && I_stricmp(verb, "connect"))
            {
                ++g_h11Stats.pktsRejected;
                if (g_h11Stats.oobRejectLog < 8) { ++g_h11Stats.oobRejectLog; fprintf(stderr, "[H11] fuera de banda rechazado: '%s' de %s\n", verb, NET_AdrToString(from)); }
                continue;
            }
            if (m.cursize > 1100) { ++g_h11Stats.pktsRejected; continue; }   // connect lleva un userinfo de <1024
        }
        else if (m.cursize < 10) { ++g_h11Stats.pktsRejected; continue; }   // secuencia + qport + serverId + 2 largos como minimo
        SV_PacketEvent(from, &m);
    }
}
// Com_ServerPacketEvent (common.cpp): SV_RunEventLoop la llama dentro de cada frame de servidor si sv.allowNetPackets.
void __cdecl Com_ServerPacketEvent() { H11_ServerPump(); }

// Comandos de cliente remotos permitidos (SV_ExecuteClientCommand: tabla ucmds + ClientCommand del juego). Fuera: download/nextdl/...
// (leerian ficheros del anfitrion), cp/vdr (pure), y los de trampa o consola del juego. Los clientes de prueba (jugador local) no pasan
// por aqui.
static const char *const s_allowCmds[] = { "userinfo", "disconnect", "mr", "score", "say", "say_team", "notify", "muteplayer", "unmuteplayer", nullptr };
static bool H11_ClientCmdFilter(client_t *cl, const char *cmd)
{
    if (!cl || cl->bIsTestClient || cl->header.netchan.remoteAddress.type == NA_BOT || cl->header.netchan.remoteAddress.type == NA_LOOPBACK) return true;
    for (int i = 0; s_allowCmds[i]; ++i) if (!I_stricmp(cmd, s_allowCmds[i])) return true;
    if (g_h11Stats.cmdRejectLog < 16) { ++g_h11Stats.cmdRejectLog; fprintf(stderr, "[H11] comando de cliente remoto rechazado: '%s' (cliente %d)\n", cmd, (int)(cl - svs.clients)); }
    return false;
}

static int s_maxPlayers = 4;
int H11_MaxPlayers() { return s_maxPlayers; }
bool H11_HostActive() { return s_hostOn; }

// Lista de partidas cercanas: "h11info" -> "h11infoResponse \proto\1044\name\..\map\..\players\n\max\n\port\n".
// Solo datos de la partida (nada del usuario ni del equipo).
static bool H11_HostOob(const netadr_t &from, const unsigned char *d, int len)
{
    char verb[32]; H11_OobVerb(d, len, verb, sizeof verb);
    if (I_stricmp(verb, "h11info")) return false;
    if (!H11_RateOk(from)) return true;
    int players = 0;
    for (int i = 0; i < com_maxclients->current.integer; ++i) if (svs.clients[i].header.state >= CS_CONNECTED) ++players;
    char name[40]; { const char *n = getenv("H11_NAME"); snprintf(name, sizeof name, "%s", n && *n ? n : "Partida"); for (char *c = name; *c; ++c) if (*c == '\\' || *c == '"' || *c == '\n' || (unsigned char)*c < 32) *c = ' '; }
    const char *map = sv_mapname ? sv_mapname->current.string : "?";
    NET_OutOfBandPrint(NS_SERVER, from, va("h11infoResponse \\proto\\1044\\name\\%s\\map\\%s\\players\\%d\\max\\%d\\port\\%d", name, map, players, s_maxPlayers, s_boundPort));
    return true;
}

// [H11] registro del anfitrion: por cliente remoto, estado/ping/posicion/salud/puntos, y ancho de banda cada segundo
static double s_lastLog;
static unsigned long long s_lastIn, s_lastOut;
static int s_lastState[32];
static void H11_HostLog()
{
    const double now = H11_NowMs();
    for (int i = 0; i < com_maxclients->current.integer && i < 32; ++i)
    {
        client_t *c = &svs.clients[i];
        const int st = (int)c->header.state;
        if (st != s_lastState[i] && !c->bIsTestClient)
        {
            fprintf(stderr, "[H11] t=%d cliente %d '%s' estado %d -> %d (%s)\n", level.time, i, c->name, s_lastState[i], st, st == CS_ACTIVE ? "ACTIVO" : st == CS_FREE ? "libre" : st == CS_ZOMBIE ? "desconectado" : "conectando");
            s_lastState[i] = st;
        }
    }
    if (now - s_lastLog < 1000.0) return;
    const double dt = s_lastLog > 0 ? (now - s_lastLog) / 1000.0 : 1.0; s_lastLog = now;
    const double kin = (g_h11Stats.bytesIn - s_lastIn) / 1024.0 / dt, kout = (g_h11Stats.bytesOut - s_lastOut) / 1024.0 / dt;
    s_lastIn = g_h11Stats.bytesIn; s_lastOut = g_h11Stats.bytesOut;
    int remotes = 0;
    for (int i = 0; i < com_maxclients->current.integer && i < 32; ++i)
    {
        client_t *c = &svs.clients[i];
        if (c->header.state < CS_CONNECTED || c->bIsTestClient) continue;
        ++remotes;
        gentity_s *e = c->gentity;
        if (c->header.state == CS_ACTIVE && e && e->client)
        {
            const playerState_s *ps = &e->client->ps;
            fprintf(stderr, "[H11] t=%d remoto %d '%s' ping %d ms | pos (%.1f,%.1f,%.1f) yaw %.1f |v|=%.0f salud %d puntos %d arma %d municion %d sesion %d conn %d | red %.1f KB/s entrada, %.1f KB/s salida\n",
                level.time, i, c->name, c->ping, ps->origin[0], ps->origin[1], ps->origin[2], ps->viewangles[1],
                sqrtf(ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1]), e->health, e->client->sess.cs.score.score, ps->weapon,
                ps->weapon ? ps->ammoInClip[0].count : 0, (int)e->client->sess.sessionState, (int)c->connectState, kin, kout);
        }
        else fprintf(stderr, "[H11] t=%d remoto %d '%s' estado %d\n", level.time, i, c->name, (int)c->header.state);
    }
    if (!remotes && (kin > 0.05 || kout > 0.05)) fprintf(stderr, "[H11] t=%d sin remotos | red %.1f KB/s entrada, %.1f KB/s salida\n", level.time, kin, kout);
}

// Ritmo de tiempo real: sin vista (pruebas en Mac con P3_BACKEND=trace) el bucle de H5P_Run corre sin vsync y el reloj del servidor
// se adelantaria al de los clientes remotos. El anfitrion espera al siguiente 1/60 s (H11_PACE=0 lo quita); con vsync no espera nada.
static void H11_HostFrame()
{
    static double next = 0;
    if (EnvInt("H11_PACE", 1))
    {
        const double now = H11_NowMs();
        if (next == 0 || now - next > 100) next = now;
        while (H11_NowMs() < next) { H11_ServerPump(); NET_Sleep((unsigned)(next - H11_NowMs()) + 1); }
        next += 1000.0 / 60.0;
    }
    H11_ServerPump(); H11_HostLog();
}

// Sala de espera (H11_WAIT=N jugadores en total, incluido el local): el GSC de zombies coloca a todos en los "initial_spawn_points" una
// sola vez, cuando getnumconnectedplayers == getnumexpectedplayers (_load_common::all_players_connected -> flag
// "all_players_connected" -> coop_player_spawn_placement). Sin sala, un jugador que entra despues aparece en (0,0,0) y cae. El
// gancho de getnumexpectedplayers (game_sp/g_scr_sp_players.cpp) devuelve N hasta que hay N jugadores activos, se acaba el tiempo
// (H11_WAIT_SECONDS, 300 por defecto) o el anfitrion pulsa "empezar" (H11_StartNow). Despues, el valor original.
extern int (*g_h0ExpectedPlayersHook)();
static int s_waitN = 1; static bool s_started; static double s_waitStart; static bool s_startNow;
static int H11_ActivePlayers()
{
    int n = 0;
    for (int i = 0; i < com_maxclients->current.integer && i < 32; ++i) if (svs.clients[i].header.state == CS_ACTIVE && svs.clients[i].connectState == 10) ++n;
    return n;
}
static int H11_ExpectedPlayers()
{
    if (s_started || s_waitN <= 1) return -1;
    const double now = H11_NowMs(); if (s_waitStart == 0) s_waitStart = now;
    const int have = H11_ActivePlayers();
    const double limit = EnvInt("H11_WAIT_SECONDS", 300) * 1000.0;
    if (have >= s_waitN || now - s_waitStart > limit || s_startNow)
    {
        s_started = true;
        fprintf(stderr, "[H11] SALA: empieza la partida con %d jugador(es) (%s)\n", have, have >= s_waitN ? "estan todos" : s_startNow ? "el anfitrion empieza" : "tiempo agotado");
        return -1;
    }
    static double lastLog; if (now - lastLog > 2000) { lastLog = now; fprintf(stderr, "[H11] SALA: esperando jugadores %d/%d\n", have, s_waitN); }
    return s_waitN;
}
void H11_StartNow() { s_startNow = true; }
int H11_WaitingFor() { return s_started || s_waitN <= 1 ? 0 : s_waitN; }
// Antes de main: el GSC empieza a contar jugadores en cuanto carga el mapa (antes de H5P_Run/H11_Install).
static struct H11EarlyInit { H11EarlyInit() {
    const char *h = getenv("H11_HOST"); if (!h || !atoi(h)) return;
    s_waitN = EnvInt("H11_WAIT", 1); if (s_waitN > 4) s_waitN = 4;
    if (s_waitN > 1) g_h0ExpectedPlayersHook = H11_ExpectedPlayers;
} } s_h11EarlyInit;

// Se llama desde H5P_Run (h5_play.cpp) antes del bucle: el servidor ya existe (SV_SpawnServer hecho).
void H11_Install()
{
    if (!getenv("H11_HOST") || !atoi(getenv("H11_HOST"))) return;
    s_maxPlayers = EnvInt("H11_MAXPLAYERS", 4); if (s_maxPlayers < 1) s_maxPlayers = 1; if (s_maxPlayers > 4) s_maxPlayers = 4;
    if (!H11_SockOpen(H11_DefaultPort())) return;
    Dvar_SetIntByName("sv_maxclients", s_maxPlayers);   // SV_DirectConnect: hueco libre en [Com_GetPrivateClients, sv_maxclients)
    sv.allowNetPackets = 1;                             // SV_RunEventLoop -> Com_ServerPacketEvent dentro de cada frame de servidor
    g_h0ClientCmdFilter = H11_ClientCmdFilter;
    g_h5pNetPump = H11_HostFrame;
    H11_SetOobHook(H11_HostOob);
    s_hostOn = true;
    fprintf(stderr, "[H11] ANFITRION: partida '%s' en %s, puerto UDP %d, hasta %d jugadores (incluido el local), bits de entidad %d\n", getenv("H11_NAME") ? getenv("H11_NAME") : "Partida",
        sv_mapname ? sv_mapname->current.string : "?", s_boundPort, s_maxPlayers, g_entNumBits);
}
