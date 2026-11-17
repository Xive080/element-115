// OLA 11: CLIENTE REMOTO (proceso que se une a una partida). Habla el protocolo ORIGINAL del motor por UDP con las piezas
// originales de qcommon (NET_OutOfBandPrint, Netchan_Setup/Transmit/Process, MSG_*, compresion de bits/huffman,
// MSG_WriteDeltaUsercmdKey, MSG_ReadDeltaEntity, MSG_ReadDeltaMatchState / MSG_ReadDeltaPlayerstate):
//   getchallenge -> challengeResponse -> connect "<userinfo>" -> connectResponse -> netchan: gamestate, comandos del servidor, snapshots;
//   y de vuelta los usercmds con el mismo formato y la misma clave que CL_WritePacket (client_mp/cl_input_mp.cpp).
// El cliente original (cl_main_mp/cl_parse_mp + cgame) no se usa entero: ver docs/decisions/2026-11-17-coop-red.md (opcion B).
//   H11_CONNECT=ip[:puerto]   direccion del anfitrion (IP de la red local, del punto de acceso o de Tailscale; puerto 3074 por defecto)
//   H11_NAME=texto            nombre del jugador (por defecto "Jugador")
//   H11C_SECONDS=s            duracion de la prueba (60); al acabar se desconecta limpio ("disconnect" fiable, como CL_Disconnect)
//   H11C_SCRIPT=0|1|2         0 quieto | 1 (por defecto) anda, gira y dispara | 2 igual y ademas se corta 5 s a mitad y RECONECTA
//   H11C_NODELTA=1            pide snapshots completos (sin delta); por defecto delta como el original (cl_nodelta 0)
//   H11_LOSS / H11_LAG / H11_JITTER: simulacion de red al enviar (h11_net.cpp)
// Registro [H11C] cada segundo: estado, snapshots, tiempo de servidor, posicion/salud/municion del playerState recibido, RTT, KB/s.
#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <universal/com_shared.h>
#include <qcommon/msg_mp.h>
#include <qcommon/msg.h>
#include <qcommon/net_chan_mp.h>
#include <qcommon/cmd.h>
#include <qcommon/common.h>
#include <client/cl_main.h>
#include <client_mp/sv_client_mp.h>
#include <server_mp/sv_init_mp.h>
#include <bgame/bg_public.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "h11_net.h"
#include <stdarg.h>

int Sys_GetPacket(netadr_t *net_from, msg_t *net_message);   // h11_net.cpp
void NET_Sleep(unsigned int msec);

extern int g_entNumBits;   // game_mp/g_main_mp.cpp
void Com_InitDvars();
float (*CL_GetMapCenter())[3];   // client_mp/cl_main_mp.cpp

namespace {

enum H11CState { H11C_IDLE, H11C_CHALLENGING, H11C_CONNECTING, H11C_CONNECTED, H11C_LOADED, H11C_ACTIVE };

struct H11Client
{
    H11CState state = H11C_IDLE;
    netadr_t server;
    int challenge = 0, qport = 0;
    double lastOob = -1e9, connectStart = 0;
    int tries = 0;
    netchan_t chan;
    std::vector<char> outBuf, inBuf;
    // secuencias (clientConnection_t)
    int serverMessageSequence = 0, serverCommandSequence = 0, reliableSequence = 0, reliableAcknowledge = 0;
    char serverCommands[128][1024];
    char reliableCommands[128][1024];
    int checksumFeed = 0, clientNum = -1, serverId = 0;
    char mapname[32] = "", gametype[32] = "";
    std::vector<entityState_s> baselines;
    // snapshots
    int snaps = 0, lastSnapServerTime = 0, snapsTotal = 0, badSnaps = 0, deltaSnaps = 0, lastSnapMessageNum = -1;
    double lastSnapRealtime = 0, lastPacketRealtime = 0;
    bool havePs = false; playerState_s ps; MatchState ms;
    // usercmds (clientActive_t.cmds)
    usercmd_s cmds[128]; int cmdNumber = 0; int lastCmdNumberSent = 0;
    double lastSend = 0;
    int cmdServerTime = 0;
    // estadistica
    double rttMs = -1, rttSent = 0; int rttN = 0; double rttSum = 0;
};

H11Client C;
unsigned char s_rx[0x20000];   // como net_message del cliente (MAX_MSGLEN): Netchan_Process reensambla aqui el gamestate fragmentado
unsigned char s_dec[0x40000];

void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fprintf(stderr, "[H11C] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); }

void AddReliable(const char *cmd)
{
    if (C.reliableSequence - C.reliableAcknowledge >= 127) { Log("cola de comandos fiables llena: '%s' descartado", cmd); return; }
    ++C.reliableSequence;
    I_strncpyz(C.reliableCommands[C.reliableSequence & 0x7F], cmd, 1024);
}

// ---------------------------------------------------------------------------------------------------------------
// Fuera de banda: lo que contesta SV_GetChallenge / SV_DirectConnect / SV_SendDisconnect. Nada mas se acepta (CL_ConnectionlessPacket
// original ejecuta "rcon" y otros; aqui no).
void HandleOob(const netadr_t &from, msg_t *m)
{
    if (!NET_CompareAdr(from, C.server)) return;
    MSG_BeginReading(m); MSG_ReadLong(m);
    char line[1024]; const char *s = MSG_ReadStringLine(m, line, sizeof line);
    Cmd_TokenizeString((char *)s);
    const char *c = Cmd_Argv(0);
    if (!I_stricmp(c, "challengeResponse") && C.state == H11C_CHALLENGING)
    {
        C.challenge = atoi(Cmd_Argv(1)); C.state = H11C_CONNECTING; C.lastOob = -1e9; C.tries = 0;
        Log("challengeResponse %d", C.challenge);
    }
    else if (!I_stricmp(c, "connectResponse") && C.state == H11C_CONNECTING)
    {
        C.outBuf.assign(0x20000, 0); C.inBuf.assign(0x20000, 0);
        Netchan_Setup(NS_CLIENT1, &C.chan, C.server, C.qport, C.outBuf.data(), (int)C.outBuf.size(), C.inBuf.data(), (int)C.inBuf.size());
        C.state = H11C_CONNECTED; C.serverMessageSequence = 0; C.serverCommandSequence = 0; C.reliableSequence = 0; C.reliableAcknowledge = 0;
        C.lastSend = 0; C.lastPacketRealtime = H11_NowMs();
        Log("connectResponse: conectado en %.0f ms (netchan qport %d)", H11_NowMs() - C.connectStart, C.qport);
    }
    else if (!I_stricmp(c, "error") || !I_strnicmp(s, "error", 5))
    {
        Log("el servidor rechaza: %s", s + (strlen(s) > 6 ? 6 : 0));
    }
    else if (!I_stricmp(c, "disconnect"))
    {
        if (C.state >= H11C_CONNECTED) { Log("el servidor nos desconecta"); C.state = H11C_IDLE; }
    }
    else if (!I_stricmp(c, "h11infoResponse"))
    {
        if (C.rttSent > 0) { const double r = H11_NowMs() - C.rttSent; C.rttMs = r; C.rttSum += r; ++C.rttN; C.rttSent = 0; }
    }
    Cmd_EndTokenizedString();
}

// ---------------------------------------------------------------------------------------------------------------
// Gamestate (CL_ParseGamestate hasta checksumFeed): configstrings (solo hace falta la 1, systeminfo, para sv_serverid), baselines,
// clientNum y checksumFeed (clave de los usercmds). Lo que va detras (dynent/destructibles/cuerdas/cristales/UI) lo usara el render.
bool ParseGamestate(msg_t *m)
{
    C.serverCommandSequence = MSG_ReadLong(m);
    MSG_ReadString(m, C.mapname, sizeof C.mapname);
    MSG_ReadString(m, C.gametype, sizeof C.gametype);
    MSG_ReadLong(m);   // checksum de configstrings constantes
    C.baselines.assign(1024, entityState_s{});
    std::string systemInfo; int nBase = 0;
    for (;;)
    {
        const int cmd = MSG_ReadByte(m);
        if (m->overflowed) return false;
        if (cmd == svc_EOF) break;
        if (cmd == svc_configstring)
        {
            int n = MSG_ReadShort(m), last = -1, kept = 0;
            if (n < 0 || n > MAX_CONFIGSTRINGS) return false;
            while (n-- > 0 && !m->overflowed)
            {
                const unsigned idx = MSG_ReadBit(m) ? (unsigned)(last + 1) : (unsigned)MSG_ReadBits(m, 12);
                if (idx >= MAX_CONFIGSTRINGS) { Log("gamestate: configstring %u fuera de rango", idx); return false; }
                const char *s = MSG_ReadBigString(m);
                if (idx == 1) systemInfo = s;
                if (idx == 3) C.serverId = atoi(s);   // CL_ServerIdChanged: configstring 3
                if (idx == 13)   // CL_ParseMapCenter: los origenes van en delta desde el centro del mapa (MSG_ReadOriginFloat)
                {
                    float *mc = *CL_GetMapCenter();
                    if (sscanf(s, "%f %f %f", &mc[0], &mc[1], &mc[2]) != 3) mc[0] = mc[1] = mc[2] = 0;
                }
                last = (int)idx; ++kept;
            }
            Log("gamestate: %d configstrings", kept);
        }
        else if (cmd == svc_baseline)
        {
            const unsigned num = (unsigned)MSG_ReadEntityIndex(m, (unsigned)g_entNumBits);
            if (num >= 1024) { Log("gamestate: baseline %u fuera de rango", num); return false; }
            entityState_s nullstate; memset(&nullstate, 0, sizeof nullstate);
            const int rc0 = m->readcount, bit0 = m->bit;
            MSG_ReadDeltaEntity(m, 0, &nullstate, &C.baselines[num], num); ++nBase;
            if (nBase <= 3 && getenv("H11C_DEBUG")) Log("baseline %u: eType %d, origen (%.1f,%.1f,%.1f), bytes %d->%d bit %d->%d", num, C.baselines[num].eType,
                C.baselines[num].lerp.pos.trBase[0], C.baselines[num].lerp.pos.trBase[1], C.baselines[num].lerp.pos.trBase[2], rc0, m->readcount, bit0, m->bit);
        }
        else if (cmd == svc_gameinformation) MSG_ReadLong(m);
        else { Log("gamestate: orden %d inesperada (tras %d baselines, bits de entidad %d)", cmd, nBase, g_entNumBits); return false; }
    }
    C.clientNum = MSG_ReadLong(m);
    C.checksumFeed = MSG_ReadLong(m);
    if (m->overflowed || C.clientNum < 0 || C.clientNum >= 32) return false;
    Log("gamestate: mapa %s, modo %s, clientNum %d, serverId %d, checksumFeed %d, comandos hasta %d", C.mapname, C.gametype, C.clientNum, C.serverId, C.checksumFeed, C.serverCommandSequence);
    return true;
}

// Snapshot (CL_ParseSnapshot de cl_parse_mp.cpp, misma logica): cabecera, estado de partida, playerState propio, entidades y
// clientes en delta contra el snapshot base (anillo de 32 como cl->snapshots, 8192 entidades como cl->parseEntities). Lo que va
// detras (cristales, comandos de animacion y modelos de actores) es del render (fase 2) y no cambia la base de delta.
struct H11Snap
{
    bool valid; int messageNum, deltaNum, serverTime, snapFlags;
    playerState_s ps; MatchState ms;
    int parseEntitiesNum, numEntities, parseClientsNum, numClients;
};
H11Snap s_snaps[32];
H11Snap s_cur;   // ultimo snapshot valido (cl->snap)
enum { H11_PARSE_ENTITIES = 8192, H11_PARSE_CLIENTS = 2048 };
std::vector<entityState_s> s_parseEnts(H11_PARSE_ENTITIES);
std::vector<clientState_s> s_parseClients(H11_PARSE_CLIENTS);
int s_parseEntitiesNum = 0, s_parseClientsNum = 0;

void ParseEntities(msg_t *m, int time, const H11Snap *old, H11Snap *ns)
{
    ns->parseEntitiesNum = s_parseEntitiesNum; ns->numEntities = 0;
    int oldindex = 0, oldnum = 99999; const entityState_s *oldstate = nullptr;
    auto nextOld = [&]() {
        if (old && ++oldindex < old->numEntities) { oldstate = &s_parseEnts[(old->parseEntitiesNum + oldindex) & (H11_PARSE_ENTITIES - 1)]; oldnum = oldstate->number; }
        else oldnum = 99999;
    };
    if (old && old->numEntities > 0) { oldstate = &s_parseEnts[old->parseEntitiesNum & (H11_PARSE_ENTITIES - 1)]; oldnum = oldstate->number; }
    auto copyOld = [&]() { s_parseEnts[s_parseEntitiesNum++ & (H11_PARSE_ENTITIES - 1)] = *oldstate; ++ns->numEntities; };
    auto delta = [&](int num, const entityState_s *from) {
        if (!MSG_ReadDeltaEntity(m, time, (entityState_s *)from, &s_parseEnts[s_parseEntitiesNum & (H11_PARSE_ENTITIES - 1)], (unsigned)num)) { ++s_parseEntitiesNum; ++ns->numEntities; }
    };
    static entityState_s s_zero;
    while (!m->overflowed)
    {
        const int newnum = MSG_ReadEntityIndex(m, (unsigned)g_entNumBits);
        if ((unsigned)newnum >= (1u << g_entNumBits)) { m->overflowed = 1; break; }   // el original asertaba (DoS en el harness)
        if (newnum == EntNum_EndMarker()) break;
        if (m->readcount > m->cursize) { m->overflowed = 1; break; }
        while (oldnum < newnum && !m->overflowed) { copyOld(); nextOld(); }
        if (oldnum == newnum) { delta(newnum, oldstate); nextOld(); }
        else delta(newnum, newnum < (int)C.baselines.size() ? &C.baselines[newnum] : &s_zero);
    }
    while (oldnum != 99999 && !m->overflowed) { copyOld(); nextOld(); }
}

void ParseClients(msg_t *m, int time, const H11Snap *old, H11Snap *ns)
{
    ns->parseClientsNum = s_parseClientsNum; ns->numClients = 0;
    int oldindex = 0, oldnum = 99999; const clientState_s *oldstate = nullptr;
    auto nextOld = [&]() {
        if (old && ++oldindex < old->numClients) { oldstate = &s_parseClients[(old->parseClientsNum + oldindex) & (H11_PARSE_CLIENTS - 1)]; oldnum = oldstate->clientIndex; }
        else oldnum = 99999;
    };
    if (old && old->numClients > 0) { oldstate = &s_parseClients[old->parseClientsNum & (H11_PARSE_CLIENTS - 1)]; oldnum = oldstate->clientIndex; }
    auto deltaClient = [&](int num, const clientState_s *from, bool unchanged) {
        clientState_s *st = &s_parseClients[s_parseClientsNum & (H11_PARSE_CLIENTS - 1)];
        if (unchanged) *st = *from;
        else if (MSG_ReadDeltaClient(m, time, (clientState_s *)from, st, (unsigned)num)) return;
        ++s_parseClientsNum; ++ns->numClients;
    };
    while (!m->overflowed && MSG_ReadBit(m) == 1)
    {
        const int newnum = MSG_ReadEntityIndex(m, 5u);
        if (m->readcount > m->cursize || newnum < 0 || newnum >= 32) { m->overflowed = 1; break; }
        while (oldnum < newnum && !m->overflowed) { deltaClient(oldnum, oldstate, true); nextOld(); }
        if (oldnum == newnum) { deltaClient(newnum, oldstate, false); nextOld(); }
        else { clientState_s dummy; memset(&dummy, 0, sizeof dummy); deltaClient(newnum, &dummy, false); }
    }
    while (oldnum != 99999 && !m->overflowed) { deltaClient(oldnum, oldstate, true); nextOld(); }
}

void ParseSnapshot(msg_t *m)
{
    static H11Snap ns; memset(&ns, 0, sizeof ns);
    ns.serverTime = MSG_ReadLong(m);
    MSG_ReadLong(m);   // physicsTime
    ns.messageNum = C.serverMessageSequence;
    const int deltaNum = MSG_ReadByte(m);
    ns.deltaNum = deltaNum ? ns.messageNum - deltaNum : -1;
    ns.snapFlags = MSG_ReadByte(m);
    const H11Snap *old = nullptr;
    if (ns.deltaNum >= 0)
    {
        old = &s_snaps[ns.deltaNum & 0x1F];
        if (!old->valid || old->messageNum != ns.deltaNum || s_parseEntitiesNum - old->parseEntitiesNum > H11_PARSE_ENTITIES - (1 << g_entNumBits)
            || s_parseClientsNum - old->parseClientsNum > 1920)
        { ++C.badSnaps; C.lastSnapMessageNum = -1; return; }   // base perdida: el siguiente paquete pide snapshot completo
    }
    if (m->overflowed) { ++C.badSnaps; return; }
    // estado de partida y playerState: base cero explicita (MSG_ReadDeltaPlayerstate con from nulo ponia a cero 10152 bytes en un
    // buffer de pila de 9896: corregido tambien en msg_mp.cpp)
    static MatchState s_nullMs; static playerState_s s_nullPs;
    MSG_ClearLastReferencedEntity(m);
    MSG_ReadDeltaMatchState(m, ns.serverTime, old ? (MatchState *)&old->ms : &s_nullMs, &ns.ms);
    MSG_ClearLastReferencedEntity(m);
    MSG_ReadDeltaPlayerstate(0, m, ns.serverTime, old ? &old->ps : &s_nullPs, &ns.ps, true);
    MSG_ClearLastReferencedEntity(m);
    ParseEntities(m, ns.serverTime, old, &ns);
    MSG_ClearLastReferencedEntity(m);
    ParseClients(m, ns.serverTime, old, &ns);
    if (m->overflowed) { ++C.badSnaps; C.lastSnapMessageNum = -1; return; }
    ns.valid = true;
    // CL_ParseSnapshot: invalida los huecos del anillo y guarda
    int oldMessageNum = s_cur.messageNum + 1;
    if (ns.messageNum - oldMessageNum >= 32) oldMessageNum = ns.messageNum - 31;
    while (oldMessageNum < ns.messageNum) s_snaps[oldMessageNum++ & 0x1F].valid = false;
    s_snaps[ns.messageNum & 0x1F] = ns;
    s_cur = ns;
    C.ps = ns.ps; C.ms = ns.ms; C.havePs = true;
    C.lastSnapMessageNum = ns.messageNum;
    if (ns.serverTime > C.lastSnapServerTime) { C.lastSnapServerTime = ns.serverTime; C.lastSnapRealtime = H11_NowMs(); }
    ++C.snaps; ++C.snapsTotal; if (deltaNum) ++C.deltaSnaps;
    if (C.state == H11C_LOADED) { C.state = H11C_ACTIVE; Log("primer snapshot (t servidor %d, %d entidades, %d clientes): CA_ACTIVE", ns.serverTime, ns.numEntities, ns.numClients); }
}

void ParseServerMessage(msg_t *m)
{
    msg_t d; MSG_Init(&d, s_dec, sizeof s_dec);
    MSG_ClearLastReferencedEntity(&d);   // el servidor escribe los indices de entidad en delta desde -1 (MSG_Init deja 0)
    if ((unsigned)(m->cursize - m->readcount) > sizeof s_dec) return;
    d.cursize = (int)MSG_ReadBitsCompress(&m->data[m->readcount], m->cursize - m->readcount, s_dec, sizeof s_dec);
    for (;;)
    {
        const int cmd = MSG_ReadByte(&d);
        if (d.overflowed || cmd == svc_EOF) return;
        switch (cmd)
        {
        case svc_nop: continue;
        case svc_serverCommand:
        {
            const int seq = MSG_ReadLong(&d); char buf[1024]; const char *s = MSG_ReadString(&d, buf, sizeof buf);
            if (d.overflowed) return;
            if (seq > C.serverCommandSequence)
            {
                if (seq - C.serverCommandSequence > 128) Log("comandos del servidor perdidos (%d -> %d)", C.serverCommandSequence, seq);
                C.serverCommandSequence = seq; I_strncpyz(C.serverCommands[seq & 0x7F], s, 1024);
                if (getenv("H11C_SHOWCMDS")) Log("servidor: %s", s);
            }
            continue;
        }
        case svc_gamestate:
            if (ParseGamestate(&d)) { if (C.state == H11C_CONNECTED) { C.state = H11C_LOADED; Log("gamestate recibido: CA_PRIMED"); } }
            else Log("gamestate ilegible");
            return;   // lo que va detras del gamestate (dynent/cristales/UI) no lo lee esta fase
        case svc_snapshot: ParseSnapshot(&d); return;   // lo que va detras del playerState es de la fase 2
        default: Log("orden de servidor %d desconocida: mensaje descartado", cmd); return;
        }
    }
}

void HandleSequenced(const netadr_t &from, msg_t *m)
{
    if (C.state < H11C_CONNECTED || !NET_CompareAdr(from, C.chan.remoteAddress)) return;
    if (!Netchan_Process(&C.chan, m)) return;
    C.lastPacketRealtime = H11_NowMs();
    const int savedSeq = C.serverMessageSequence, savedAck = C.reliableAcknowledge;
    C.serverMessageSequence = *(int *)m->data;
    C.reliableAcknowledge = MSG_ReadLong(m);
    if (C.reliableAcknowledge < C.reliableSequence - 128 || C.reliableAcknowledge > C.reliableSequence) C.reliableAcknowledge = C.reliableSequence;
    ParseServerMessage(m);
    if (m->overflowed) { C.serverMessageSequence = savedSeq; C.reliableAcknowledge = savedAck; }
}

// ---------------------------------------------------------------------------------------------------------------
// CL_WritePacket (cl_input_mp.cpp) con el mismo formato y la misma clave: serverId, serverMessageSequence, (connstate << 28) |
// serverCommandSequence, comandos fiables, usercmds en delta con clave, origen predicho y bit de vehiculo; compresion desde el byte 9.
void WritePacket()
{
    unsigned char data[2052]; msg_t buf; memset(&buf, 0, sizeof buf); MSG_Init(&buf, data, 2048);
    const connstate_t cs = C.state == H11C_ACTIVE ? CA_ACTIVE : C.state == H11C_LOADED ? CA_PRIMED : CA_CONNECTED;
    MSG_WriteByte(&buf, C.state >= H11C_LOADED ? C.serverId : 0);
    MSG_WriteLong(&buf, C.serverMessageSequence);
    MSG_WriteLong(&buf, (int)(((unsigned)cs << 28) | (unsigned)C.serverCommandSequence));
    for (int i = C.reliableAcknowledge + 1; i <= C.reliableSequence; ++i) { MSG_WriteBits(&buf, 2, 3u); MSG_WriteLong(&buf, i); MSG_WriteString(&buf, C.reliableCommands[i & 0x7F]); }
    int count = C.cmdNumber - C.lastCmdNumberSent; if (count > 32) count = 32;
    if (count >= 1 && C.state >= H11C_LOADED)
    {
        const bool nodelta = getenv("H11C_NODELTA") && atoi(getenv("H11C_NODELTA"));
        MSG_WriteBits(&buf, (!nodelta && C.lastSnapMessageNum >= 0 && C.lastSnapMessageNum == C.serverMessageSequence) ? 0 : 1, 3u);   // CL_WritePacket: delta si el ultimo mensaje fue un snapshot valido
        MSG_WriteByte(&buf, count);
        int key = C.checksumFeed ^ C.serverMessageSequence ^ Com_HashKey(C.serverCommands[C.serverCommandSequence & 0x7F], 32);
        usercmd_s nullcmd; memset(&nullcmd, 0, sizeof nullcmd);
        // Base nula y angulos siempre enviados (forceSendAngles): el servidor parte de MSG_SetDefaultUserCmd(ps) (arma, offhand y angulos del
        // ps); con arma 0 y offhand 0 no se envian y se quedan los del servidor, y los angulos van explicitos: base coherente sin el ps.
        const usercmd_s *old = &nullcmd;
        for (int i = 0; i < count; ++i)
        {
            const usercmd_s *cmd = &C.cmds[(C.cmdNumber - count + i + 1) & 0x7F];
            MSG_WriteDeltaUsercmdKey(&buf, key, old, cmd, 1);
            old = cmd;
        }
        const float org[3] = { C.havePs ? C.ps.origin[0] : 0, C.havePs ? C.ps.origin[1] : 0, C.havePs ? C.ps.origin[2] : 0 };
        MSG_WriteLong(&buf, *(const int *)&org[0]); MSG_WriteLong(&buf, *(const int *)&org[1]); MSG_WriteLong(&buf, *(const int *)&org[2]);
        // hora de la prediccion: 0 = este cliente no predice. Asi MSG_WriteDeltaPlayerstate siempre manda origen/velocidad/angulos
        // (si coincidiera con commandTime los omitiria y habria que sacarlos de una prediccion propia, CL_GetPredictedOriginForServerTime)
        MSG_WriteLong(&buf, 0);
        MSG_WriteBit0(&buf);
        C.lastCmdNumberSent = C.cmdNumber;
    }
    MSG_WriteBits(&buf, 3, 3u);
    unsigned char comp[2048 + 16];
    memcpy(comp, buf.data, 9);
    const int size = MSG_WriteBitsCompress(0, buf.data + 9, buf.cursize - 9, comp + 9, 2039) + 9;
    Netchan_Transmit(&C.chan, size, (char *)comp, 0);
    while (C.chan.unsentFragments) Netchan_TransmitNextFragment(&C.chan);
}

// Entrada de prueba (H11C_SCRIPT): lo mismo que daran los toques/teclado por H5PlayerInput en la fase 2.
float s_yaw = 0, s_pitch = 0;
void MakeCmd(double tActive)
{
    usercmd_s &c = C.cmds[++C.cmdNumber & 0x7F]; memset(&c, 0, sizeof c);
    const double now = H11_NowMs();
    // Hora del usercmd: la del ultimo snapshot mas lo transcurrido (CL_CreateNewCommands usa cl->serverTime). Sin snapshot todavia
    // (CA_PRIMED) va casi a 0: SV_UserMove la acepta para SV_ClientEnterWorld y SV_ClientThink no la ejecuta (pasado).
    int st = C.lastSnapServerTime > 0 ? C.lastSnapServerTime + (int)(now - C.lastSnapRealtime) : 1;
    if (st <= C.cmdServerTime) st = C.cmdServerTime + 1;
    C.cmdServerTime = c.serverTime = st;
    const int mode = getenv("H11C_SCRIPT") ? atoi(getenv("H11C_SCRIPT")) : 1;
    if (mode >= 1 && C.state == H11C_ACTIVE)
    {
        const double ph = fmod(tActive, 8.0);
        if (ph < 3.0) c.forwardmove = 127;                   // anda 3 s
        else if (ph < 5.0) s_yaw += 1.5f;                     // gira (unos 90 grados/s a 60 Hz)
        else if (ph < 6.0) c.rightmove = -127;               // se aparta
        if (fmod(tActive, 0.5) < 0.1 && ph >= 3.0) c.button_bits.setBit(0u);   // dispara (bit 0 de CL_CmdButtons)
        if (ph > 7.5 && ph < 7.6) c.button_bits.setBit(5u);  // recarga
    }
    c.angles[0] = (unsigned short)(((int)(s_pitch * 65536.0f / 360.0f)) & 0xFFFF);
    c.angles[1] = (unsigned short)(((int)(s_yaw * 65536.0f / 360.0f)) & 0xFFFF);
}

void SendOob(const char *s) { NET_OutOfBandPrint(NS_CLIENT1, C.server, s); }

void StartConnect()
{
    C.state = H11C_CHALLENGING; C.lastOob = -1e9; C.tries = 0; C.connectStart = H11_NowMs();
    C.qport = 1 + (rand() & 0x7FFE);
    C.havePs = false; C.lastSnapServerTime = 0; C.cmdServerTime = 0; C.lastSnapMessageNum = -1;
}

void Disconnect(const char *why)
{
    if (C.state >= H11C_CONNECTED)
    {
        AddReliable("disconnect");
        for (int i = 0; i < 3; ++i) WritePacket();   // CL_Disconnect envia el "disconnect" fiable tres veces
        Log("desconexion limpia (%s)", why);
    }
    C.state = H11C_IDLE;
}

// Lista de partidas cercanas (fase 3): "h11info" por broadcast a la red local (y a 127.0.0.1 para pruebas en un solo Mac); cada anfitrion
// contesta "h11infoResponse \\proto\\..\\name\\..\\map\\..\\players\\n\\max\\n\\port\\n" (h11_net.cpp). La UI lista lo mismo.
// Tailscale no reenvia broadcast: ahi va la IP 100.x a mano (H11_CONNECT=100.x.y.z), sin cambiar nada del protocolo.
int H11C_List()
{
    netadr_t bc; memset(&bc, 0, sizeof bc); bc.type = NA_BROADCAST; bc.port = BigShort((__int16)H11_DefaultPort());
    netadr_t lo; NET_StringToAdr(va("127.0.0.1:%d", H11_DefaultPort()), &lo);
    const double t0 = H11_NowMs(); double last = -1e9; int found = 0;
    std::vector<std::string> seen;
    while (H11_NowMs() - t0 < 2500)
    {
        if (H11_NowMs() - last > 800) { NET_OutOfBandPrint(NS_CLIENT1, bc, "h11info"); NET_OutOfBandPrint(NS_CLIENT1, lo, "h11info"); last = H11_NowMs(); }
        msg_t m; netadr_t from;
        MSG_Init(&m, s_rx, sizeof s_rx);
        while (Sys_GetPacket(&from, &m))
        {
            if (*(unsigned int *)m.data == 0xFFFFFFFFu && m.cursize < 1100)
            {
                MSG_BeginReading(&m); MSG_ReadLong(&m);
                char line[1024]; const char *l = MSG_ReadStringLine(&m, line, sizeof line);
                if (!I_strnicmp(l, "h11infoResponse ", 16))
                {
                    char info[1024]; I_strncpyz(info, l + 16, sizeof info);
                    char port[16]; I_strncpyz(port, Info_ValueForKey(info, "port"), sizeof port);
                    const unsigned char *ip = from.ip;
                    char key[64]; snprintf(key, sizeof key, "%u.%u.%u.%u:%s", ip[0], ip[1], ip[2], ip[3], port);
                    bool dup = false; for (auto &k : seen) dup |= k == key;
                    if (!dup)
                    {
                        seen.push_back(key); ++found;
                        char name[64], map[64], pl[8], mx[8];
                        I_strncpyz(name, Info_ValueForKey(info, "name"), sizeof name); I_strncpyz(map, Info_ValueForKey(info, "map"), sizeof map);
                        I_strncpyz(pl, Info_ValueForKey(info, "players"), sizeof pl); I_strncpyz(mx, Info_ValueForKey(info, "max"), sizeof mx);
                        Log("partida '%s' en %s: mapa %s, jugadores %s/%s (%.0f ms)", name, key, map, pl, mx, H11_NowMs() - last);
                    }
                }
            }
            MSG_Init(&m, s_rx, sizeof s_rx);
        }
        NET_Sleep(20);
    }
    Log("lista: %d partida(s)", found);
    return 0;
}

} // namespace

int H11C_Run()
{
    Com_InitThreadData(0);
    Cbuf_Init(); Cmd_Init(); Dvar_Init();
    Com_InitDvars();
    // Como el anfitrion (H5G_Run): en zombies los origenes x/y van en 18 bits (MSG_ReadOriginFloat mira zombiemode). Sin esto el
    // primer origen desalinea el resto del gamestate. dedicated queda en 0: MSG_GetMapCenter usa el centro del cliente (cls).
    Dvar_SetBoolByName("zombiemode", true);
    if (getenv("H11C_SHOWNET"))   // depuracion: traza de campos de los lectores originales (cl_shownet de cl_main_mp)
    {
        extern const dvar_t *cl_shownet; extern int g_h0PrintAll; g_h0PrintAll = 1;
        cl_shownet = _Dvar_RegisterInt("cl_shownet", atoi(getenv("H11C_SHOWNET")), -2, 4, 0, "");
    }
    srand((unsigned)H11_NowMs());
    Netchan_Init((__int16)(rand() & 0xFFFF));
    if (!H11_SockOpen(0)) return 1;
    const char *target = getenv("H11_CONNECT");
    if (!I_stricmp(target, "lista")) return H11C_List();   // H11_CONNECT=lista: partidas cercanas (broadcast) y salir
    char adr[256]; snprintf(adr, sizeof adr, "%s", target);
    if (!strchr(adr, ':')) snprintf(adr + strlen(adr), sizeof adr - strlen(adr), ":%d", H11_DefaultPort());
    if (!NET_StringToAdr(adr, &C.server) || C.server.type != NA_IP) { Log("direccion no valida: %s", target); return 1; }
    Log("conectando a %s", NET_AdrToString(C.server));
    const double seconds = getenv("H11C_SECONDS") ? atof(getenv("H11C_SECONDS")) : 60.0;
    const int mode = getenv("H11C_SCRIPT") ? atoi(getenv("H11C_SCRIPT")) : 1;
    char userinfo[512];
    { const char *n = getenv("H11_NAME"); char name[32]; snprintf(name, sizeof name, "%s", n && *n ? n : "Jugador"); for (char *c = name; *c; ++c) if (*c == '\\' || *c == '"' || *c == ';' || (unsigned char)*c < 32) *c = '_';
      snprintf(userinfo, sizeof userinfo, "\\name\\%s\\rate\\25000\\snaps\\20", name); }
    StartConnect();
    const double t0 = H11_NowMs(); double tActiveStart = -1, lastLog = t0, lastRtt = 0, cutUntil = 0; bool didCut = false;
    unsigned long long lastIn = 0, lastOut = 0; int reconnects = 0;
    while (H11_NowMs() - t0 < seconds * 1000.0)
    {
        const double now = H11_NowMs();
        // recepcion
        msg_t m; netadr_t from;
        for (int n = 0; n < 512; ++n)
        {
            MSG_Init(&m, s_rx, sizeof s_rx);
            if (!Sys_GetPacket(&from, &m)) break;
            if (cutUntil > now) continue;   // corte simulado (H11C_SCRIPT=2): el cable "desenchufado"
            if (*(unsigned int *)m.data == 0xFFFFFFFFu) HandleOob(from, &m); else HandleSequenced(from, &m);
        }
        // conexion
        if (C.state == H11C_CHALLENGING && now - C.lastOob > 1000) { SendOob("getchallenge 0 \"\" \"0\""); C.lastOob = now; ++C.tries; }
        else if (C.state == H11C_CONNECTING && now - C.lastOob > 1000)
        {
            char info[700]; snprintf(info, sizeof info, "%s\\protocol\\1044\\challenge\\%d\\qport\\%d", userinfo, C.challenge, C.qport);
            SendOob(va("connect \"%s\"", info)); C.lastOob = now; ++C.tries;
        }
        if ((C.state == H11C_CHALLENGING || C.state == H11C_CONNECTING) && C.tries > (getenv("H11C_TRIES") ? atoi(getenv("H11C_TRIES")) : 15)) { Log("sin respuesta del anfitrion tras %d intentos", C.tries); break; }
        // tiempo muerto (sv_timeout del cliente: aqui 10 s sin paquetes del servidor)
        if (C.state >= H11C_CONNECTED && now - C.lastPacketRealtime > 10000 && cutUntil < now) { Log("sin paquetes del servidor en 10 s: conexion perdida, reintento"); StartConnect(); ++reconnects; }
        // usercmds a 60 Hz, paquetes a 30 Hz (cl_maxpackets 30)
        if (C.state >= H11C_LOADED)
        {
            if (C.state == H11C_ACTIVE && tActiveStart < 0) tActiveStart = now;
            MakeCmd(tActiveStart >= 0 ? (now - tActiveStart) / 1000.0 : 0);
        }
        if (C.state >= H11C_CONNECTED && cutUntil < now && (now - C.lastSend >= 1000.0 / 30.0)) { WritePacket(); C.lastSend = now; }
        // RTT de transporte con el sondeo de la lista (h11info) cada 2 s
        if (C.state >= H11C_CONNECTED && now - lastRtt > 2000 && cutUntil < now) { C.rttSent = now; SendOob("h11info"); lastRtt = now; }
        // corte + reconexion (fase 2)
        if (mode == 2 && !didCut && tActiveStart > 0 && now - tActiveStart > seconds * 400.0)
        {
            didCut = true; cutUntil = now + 5000; Log("CORTE simulado de 5 s (sin enviar ni recibir)");
        }
        if (mode == 2 && didCut && cutUntil > 0 && now >= cutUntil && C.state >= H11C_CONNECTED && now - C.lastPacketRealtime > 4000)
        {
            Log("vuelve la red: reconexion"); Disconnect("reconexion"); StartConnect(); ++reconnects; cutUntil = 0;
        }
        if (now - lastLog >= 1000)
        {
            const double dt = (now - lastLog) / 1000.0; lastLog = now;
            const double kin = (g_h11Stats.bytesIn - lastIn) / 1024.0 / dt, kout = (g_h11Stats.bytesOut - lastOut) / 1024.0 / dt;
            lastIn = g_h11Stats.bytesIn; lastOut = g_h11Stats.bytesOut;
            if (C.havePs)
                Log("t=%.0f s estado %d snaps/s %d t_servidor %d | ps pos (%.1f,%.1f,%.1f) yaw %.1f salud %d arma %d municion %d | RTT %.1f ms | %.1f KB/s entrada, %.1f KB/s salida",
                    (now - t0) / 1000.0, (int)C.state, C.snaps, C.lastSnapServerTime, C.ps.origin[0], C.ps.origin[1], C.ps.origin[2], C.ps.viewangles[1],
                    C.ps.stats[0], C.ps.weapon, C.ps.weapon ? C.ps.ammoInClip[0].count : 0, C.rttMs, kin, kout);
            else
                Log("t=%.0f s estado %d snaps/s %d t_servidor %d | RTT %.1f ms | %.1f KB/s entrada, %.1f KB/s salida", (now - t0) / 1000.0, (int)C.state, C.snaps,
                    C.lastSnapServerTime, C.rttMs, kin, kout);
            C.snaps = 0;
        }
        H11_FlushDelayed();
        NET_Sleep(4);
    }
    Disconnect("fin de la prueba");
    for (int i = 0; i < 20; ++i) { H11_FlushDelayed(); NET_Sleep(10); }
    Log("FIN: snapshots %d (en delta %d, ilegibles %d), reconexiones %d, RTT medio %.1f ms (%d medidas), enviados %llu paquetes / %llu bytes, recibidos %llu / %llu, rechazados %llu, perdidos (simulados) %llu",
        C.snapsTotal, C.deltaSnaps, C.badSnaps, reconnects, C.rttN ? C.rttSum / C.rttN : -1.0, C.rttN, g_h11Stats.pktsOut, g_h11Stats.bytesOut, g_h11Stats.pktsIn, g_h11Stats.bytesIn,
        g_h11Stats.pktsRejected, g_h11Stats.pktsDroppedSim);
    return 0;
}
