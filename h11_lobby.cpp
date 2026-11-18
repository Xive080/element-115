// OLA C1: PARTIDA LOCAL (cooperativo SOLO en la red local: misma Wi-Fi/LAN, punto de acceso del movil, Wi-Fi Direct o PAN Bluetooth;
// nada de internet, servidores, cuentas ni emparejamiento). Dos piezas:
//  (1) Arranque: players/bo1_net.txt (lo escribe el menu y se consume al leerlo) decide si este proceso es anfitrion, se une a una
//      partida o juega solo. "host <jugadores> <mapa>" | "join <ip:puerto> <mapa>" | "solo <mapa>". El motor no es reentrante: cambiar
//      de modo es relanzar el proceso (como el cambio de mapa de h18_maps.cpp). Pone las mismas variables que las pruebas (H11_HOST,
//      H11_WAIT, H11_CONNECT, H5_MAP), asi menu y pruebas recorren el mismo camino.
//  (2) Menu (H5G): fila LOCAL CO-OP en el menu de pausa ORIGINAL y menu h11_coop con el mismo aspecto (filas clonadas de pausedmenu,
//      tactil/mando/raton por h10_settings.cpp, zona segura en tiempo de ejecucion): crear partida, partidas encontradas en la red local
//      (sondeo h11info por broadcast de cada interfaz, h11_net.cpp), empezar ya (sala de espera del anfitrion) y volver a jugar solo.
// En partida con anfitrion el menu NO congela el servidor (los demas siguen jugando): H11_HostFrame corre el frame aunque haya pausa.
#include <universal/q_shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <unistd.h>
// compat/Windows.h (prelude) declara su propio in_addr/inet_addr: las del sistema entran renombradas (como en h11_net.cpp)
#define in_addr h11_sys_in_addr
#define inet_addr h11_sys_inet_addr
#include <sys/socket.h>
#include <netinet/in.h>
#undef in_addr
#undef inet_addr
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

void H11_ApplyHostEnv();                                                   // h11_net.cpp
int H11_SendDiscovery(int fd, const void *data, size_t len, int port);     // h11_net.cpp
bool H11_IsLocalIp(const unsigned char *ip);                               // h11_net.cpp
int H11_DefaultPort();
double H11_NowMs();

static std::string s_base;          // P3_GAMEDATA de serie (players/ vive ahi)
static std::string s_mode = "solo"; // solo | host | join (lo que decidio el arranque)
static int s_argc; static char **s_argv;
static std::string NetFile() { return s_base + "/players/bo1_net.txt"; }

static bool ValidZone(const char *z) { if (!z || !*z || strlen(z) > 48) return false; for (const char *c = z; *c; ++c) if (!(isalnum((unsigned char)*c) || *c == '_')) return false; return true; }

// h18_maps.cpp H18_ResolveBoot la llama al principio (con la base ya resuelta), antes de elegir mapa.
static void H11_ResolveBoot(const char *base, int argc, char **argv)
{
    s_base = base ? base : "."; s_argc = argc; s_argv = argv;
    FILE *f = fopen(NetFile().c_str(), "r");
    if (!f) { if (getenv("H11_HOST") && atoi(getenv("H11_HOST"))) s_mode = "host"; else if (getenv("H11_CONNECT")) s_mode = "join"; return; }
    char mode[16] = "", a[64] = "", b[64] = ""; const int n = fscanf(f, "%15s %63s %63s", mode, a, b); fclose(f);
    unlink(NetFile().c_str());   // una sola vez: el siguiente arranque normal es en solitario
    if (n < 1) return;
    unsetenv("H11_HOST"); unsetenv("H11_CONNECT"); unsetenv("H11_WAIT"); unsetenv("H11_LOBBYUI");
    if (!strcmp(mode, "host"))
    {
        int players = atoi(a); if (players < 2 || players > 4) players = 4;
        char w[8]; snprintf(w, sizeof w, "%d", players);
        setenv("H11_HOST", "1", 1); setenv("H11_WAIT", w, 1); setenv("H11_MAXPLAYERS", w, 1);
        setenv("H11_WAIT_SECONDS", "1800", 0);   // la sala espera hasta 30 min o hasta "empezar ya"
        setenv("H11_LOBBYUI", "1", 1);           // abre el menu de la sala al entrar
        if (ValidZone(b)) setenv("H5_MAP", b, 1);
        s_mode = "host";
    }
    else if (!strcmp(mode, "join"))
    {
        setenv("H11_CONNECT", a, 1);
        if (ValidZone(b)) setenv("H5_MAP", b, 1);
        s_mode = "join";
    }
    else { if (ValidZone(a)) setenv("H5_MAP", a, 1); s_mode = "solo"; }
    H11_ApplyHostEnv();
    fprintf(stderr, "[H11L] arranque: modo %s (%s %s)\n", s_mode.c_str(), a, b);
}
void (*g_h11ResolveBoot)(const char *base, int argc, char **argv) = H11_ResolveBoot;

#ifdef H5G
// ================================================================ menu (H5G: hay UI de partida)
#include <universal/ptr32.h>
#include <universal/dvar.h>
#include <universal/com_expressions.h>
#include <database/db_registry.h>
#include <qcommon/cmd.h>
#include <ui/ui_shared.h>
#include <ui/ui_utils.h>

extern void (*g_h10RestartApp)();
extern void (*g_h18PatchMenus)(UiContext *);
extern void (*g_h5pNetPump)();
void H10_SaveNow();
bool H10_Open(const char *menu);
bool H10_IsOpen();
const char *H18_Map();
const char *H18_MapName(const char *zone);   // h18_maps.cpp: nombre del catalogo en mayusculas del juego (o la zona)
bool H11_HostActive();
int H11_MaxPlayers();
int H11_WaitingFor();
int H11_ConnectedPlayers();
void H11_StartNow();

namespace {

// --- relanzar el proceso en otro modo (mismo camino que el cambio de mapa: la app se reinicia; en el Mac, execv)
void Relaunch(const char *line)
{
    if (FILE *f = fopen(NetFile().c_str(), "w")) { fprintf(f, "%s\n", line); fclose(f); }
    else { fprintf(stderr, "[H11L] no se pudo escribir %s\n", NetFile().c_str()); return; }
    fprintf(stderr, "[H11L] %s -> %s\n", line, g_h10RestartApp ? "reinicio la app" : "relanzo el proceso"); fflush(stderr);
    H10_SaveNow();
    if (g_h10RestartApp) { g_h10RestartApp(); return; }
#ifndef BO1_APP
    if (!s_argv) return;
    setenv("P3_GAMEDATA", s_base.c_str(), 1); unsetenv("H10_CFG");
    // pruebas: el guion de menu (H10_PILOT) es de la instancia que lo recibio; H11L_PILOT_NEXT es el de la relanzada
    if (const char *nx = getenv("H11L_PILOT_NEXT")) { setenv("H10_PILOT", nx, 1); unsetenv("H11L_PILOT_NEXT"); } else unsetenv("H10_PILOT");
    char exe[4096] = ""; uint32_t n = sizeof exe;
#ifdef __APPLE__
    if (_NSGetExecutablePath(exe, &n) != 0) strcpy(exe, s_argv[0]);
#else
    { ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1); if (r > 0) exe[r] = 0; else strcpy(exe, s_argv[0]); }
#endif
    execv(exe, s_argv);
    fprintf(stderr, "[H11L] execv fallo\n");
#endif
}

// --- partidas de la red local (sondeo propio: socket UDP efimero, solo mientras el menu esta abierto)
struct Game { unsigned char ip[4]; int port; std::string name, map; int players, max; bool lobby; double seen; };
std::vector<Game> s_games;
int s_scanFd = -1; bool s_scanning; double s_lastProbe; bool s_dirty = true;

void ScanOpen()
{
    if (s_scanFd >= 0) return;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (fd < 0) { fprintf(stderr, "[H11L] socket: no hay red\n"); return; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = 0;
    if (bind(fd, (sockaddr *)&a, sizeof a) < 0) { close(fd); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    s_scanFd = fd;
}
void ScanClose() { if (s_scanFd >= 0) { close(s_scanFd); s_scanFd = -1; } s_scanning = false; }

std::string InfoValue(const char *info, const char *key)
{
    // "\k\v\k\v" (Info_ValueForKey sin depender del buffer estatico del motor)
    std::string s = info, k = std::string("\\") + key + "\\";
    size_t p = 0;
    while ((p = s.find(k, p)) != std::string::npos)
    {
        // la clave debe estar en posicion de clave (numero par de separadores antes)
        int seps = 0; for (size_t i = 0; i < p; ++i) seps += s[i] == '\\';
        if ((seps & 1) == 0) { size_t v = p + k.size(), e = s.find('\\', v); return s.substr(v, e == std::string::npos ? std::string::npos : e - v); }
        p += 1;
    }
    return "";
}

void ScanPoll()
{
    if (s_scanFd < 0) return;
    const double now = H11_NowMs();
    if (s_scanning && now - s_lastProbe > 1000)
    {
        static const char probe[] = "\xff\xff\xff\xffh11info";
        H11_SendDiscovery(s_scanFd, probe, sizeof probe - 1, H11_DefaultPort());
        s_lastProbe = now;
    }
    unsigned char buf[1400];
    for (int guard = 0; guard < 64; ++guard)
    {
        sockaddr_in from; socklen_t fl = sizeof from;
        const ssize_t r = recvfrom(s_scanFd, buf, sizeof buf - 1, 0, (sockaddr *)&from, &fl);
        if (r <= 0) break;
        buf[r] = 0;
        const unsigned char *ip = (const unsigned char *)&from.sin_addr.s_addr;
        if (r < 20 || memcmp(buf, "\xff\xff\xff\xff", 4) || !H11_IsLocalIp(ip)) continue;
        const char *l = (const char *)buf + 4;
        if (strncmp(l, "h11infoResponse ", 16)) continue;
        const char *info = l + 16;
        if (InfoValue(info, "proto") != "1044") continue;
        Game g; memcpy(g.ip, ip, 4); g.port = atoi(InfoValue(info, "port").c_str());
        g.name = InfoValue(info, "name").substr(0, 24); g.map = InfoValue(info, "map").substr(0, 40);
        g.players = atoi(InfoValue(info, "players").c_str()); g.max = atoi(InfoValue(info, "max").c_str());
        g.lobby = atoi(InfoValue(info, "lobby").c_str()) != 0; g.seen = now;
        if (g.port <= 0 || g.port > 65535 || !ValidZone(g.map.c_str())) continue;
        // un mismo anfitrion contesta por varias interfaces (127.0.0.1 y su IP de la red): se queda la de la red
        bool merged = false;
        for (Game &o : s_games)
            if (o.port == g.port && o.name == g.name && o.map == g.map && (!memcmp(o.ip, g.ip, 4) || g.ip[0] == 127 || o.ip[0] == 127))
            {
                if (o.ip[0] == 127 && g.ip[0] != 127) memcpy(o.ip, g.ip, 4);
                s_dirty |= o.players != g.players || o.lobby != g.lobby || o.max != g.max;
                o.players = g.players; o.max = g.max; o.lobby = g.lobby; o.seen = now; merged = true; break;
            }
        if (!merged && s_games.size() < 8) { s_games.push_back(g); s_dirty = true;
            fprintf(stderr, "[H11L] partida '%s' en %u.%u.%u.%u:%d (%s, %d/%d, %s)\n", g.name.c_str(), g.ip[0], g.ip[1], g.ip[2], g.ip[3], g.port, g.map.c_str(), g.players, g.max, g.lobby ? "en sala" : "empezada"); }
    }
    for (size_t i = 0; i < s_games.size();) if (now - s_games[i].seen > 4000) { s_games.erase(s_games.begin() + i); s_dirty = true; } else ++i;
}

// --- filas clonadas del menu de pausa (mismo metodo que h18_maps.cpp; copia local en esta TU para no tocar la del selector de mapas)
const char *A(const char *s) { return Ptr32_ArenaStrdup(s); }
itemDef_s *CloneItem(itemDef_s *src, menuDef_t *parent, float y)
{
    itemDef_s *it = (itemDef_s *)Ptr32_ArenaAlloc(sizeof(itemDef_s), 8); memcpy(it, src, sizeof(itemDef_s));
    if (src->typeData.textDef && Item_GetTextDef(src) == src->typeData.textDef.get()) {
        textDef_s *t = (textDef_s *)Ptr32_ArenaAlloc(sizeof(textDef_s), 4); memcpy(t, src->typeData.textDef.get(), sizeof(textDef_s)); it->typeData.textDef = t;
        if (t->textExpData) { ExpressionStatement *e = (ExpressionStatement *)Ptr32_ArenaAlloc(sizeof(ExpressionStatement), 4); memcpy(e, t->textExpData.get(), sizeof(ExpressionStatement)); t->textExpData = (textExp_s *)e; }
    }
    if (src->animInfo) { UIAnimInfo *a = (UIAnimInfo *)Ptr32_ArenaAlloc(sizeof(UIAnimInfo), 4); memcpy(a, src->animInfo.get(), sizeof(UIAnimInfo)); it->animInfo = a; }
    it->parent = parent;
    it->window.rectClient.y += y - it->window.rect.y; it->window.rect.y = y;
    return it;
}
ExpressionStatement ExpReplaceInt(const ExpressionStatement &src, int from, int to)
{
    ExpressionStatement e = src; if (!src.numRpn || !src.rpn) return e;
    expressionRpn *r = (expressionRpn *)Ptr32_ArenaAlloc(sizeof(expressionRpn) * e.numRpn, 4); memcpy(r, src.rpn.get(), sizeof(expressionRpn) * e.numRpn);
    for (int i = 0; i < e.numRpn; ++i) if (r[i].type == 0 && r[i].data.constant.dataType == VAL_INT && r[i].data.constant.internals.intVal == from) r[i].data.constant.internals.intVal = to;
    e.rpn = r; return e;
}
void SetText(itemDef_s *it, const char *txt)
{
    textDef_s *t = it->typeData.textDef.get(); if (!t) return;
    if (t->textExpData) {
        ExpressionStatement *e = (ExpressionStatement *)t->textExpData.get(); expressionRpn *r = (expressionRpn *)Ptr32_ArenaAlloc(sizeof(expressionRpn) * e->numRpn, 4); memcpy(r, e->rpn.get(), sizeof(expressionRpn) * e->numRpn);
        for (int i = 0; i < e->numRpn; ++i) if (r[i].type == 0 && r[i].data.constant.dataType == VAL_STRING) r[i].data.constant.internals.string = (char *)A(txt);
        e->rpn = r;
    } else t->text = A(txt);
}
GenericEventScript *Script(const char *action) { GenericEventScript *s = (GenericEventScript *)Ptr32_ArenaAlloc(sizeof(GenericEventScript), 4); s->action = A(action); s->fireOnTrue = true; return s; }
GenericEventHandler *Handler(const char *name, const char *action, GenericEventHandler *next)
{
    GenericEventHandler *h = (GenericEventHandler *)Ptr32_ArenaAlloc(sizeof(GenericEventHandler), 4); h->name = A(name); h->eventScript = Script(action); h->next = next; return h;
}
bool HasAction(itemDef_s *it, const char *what)
{
    for (GenericEventHandler *h = it->onEvent.get(); h; h = h->next.get()) if (h->name && !I_stricmp(h->name.get(), "action"))
        for (GenericEventScript *s = h->eventScript.get(); s; s = s->next.get()) if (s->action && strstr(s->action.get(), what)) return true;
    return false;
}
struct Row { itemDef_s *bar, *hl, *btn, *lab; int hlNum; };
bool RowAt(menuDef_t *m, int b, Row *r)
{
    if (b < 2 || b + 1 >= m->itemCount) return false;
    itemDef_s *x[4] = { m->items[b - 2].get(), m->items[b - 1].get(), m->items[b].get(), m->items[b + 1].get() };
    for (itemDef_s *i : x) if (i->window.rect.y != x[2]->window.rect.y) return false;
    r->bar = x[0]; r->hl = x[1]; r->btn = x[2]; r->lab = x[3]; r->hlNum = -1;
    for (GenericEventHandler *h = r->btn->onEvent.get(); h; h = h->next.get()) if (h->name && !I_stricmp(h->name.get(), "onFocus"))
        for (GenericEventScript *s = h->eventScript.get(); s; s = s->next.get()) if (s->action) { const char *p = strstr(s->action.get(), "ui_highlight "); if (p) r->hlNum = atoi(p + 13); }
    return r->hlNum > 0;
}
bool HasText(itemDef_s *it, const char *str)
{
    textDef_s *t = Item_GetTextDef(it); if (!t) return false;
    if (t->textExpData) {
        ExpressionStatement *e = (ExpressionStatement *)t->textExpData.get(); expressionRpn *r = e->rpn.get();
        for (int i = 0; r && i < e->numRpn; ++i) if (r[i].type == 0 && r[i].data.constant.dataType == VAL_STRING && r[i].data.constant.internals.string && !strcmp(r[i].data.constant.internals.string.get(), str)) return true;
        return false;
    }
    return t->text && !strcmp(t->text.get(), str);
}
int FindRowBtn(menuDef_t *m, int ctx, const char *label, bool anyVis = false)
{
    int first = -1;
    for (int i = 0; i < m->itemCount; ++i) { itemDef_s *it = m->items[i].get(); if (!HasAction(it, "") || !HasText(it, label)) continue;
        if (Item_IsVisible(0, ctx, it)) return i; if (first < 0) first = i; }
    return anyVis ? first : -1;
}
Row MakeRow(const Row &t, menuDef_t *m, std::vector<itemDef_s *> &out, float y, int num, const char *label, const char *action, itemDef_s *visFrom)
{
    Row r; r.bar = CloneItem(t.bar, m, y); r.hl = CloneItem(t.hl, m, y); r.btn = CloneItem(t.btn, m, y); r.lab = CloneItem(t.lab, m, y); r.hlNum = num;
    r.hl->visibleExp = ExpReplaceInt(t.hl->visibleExp, t.hlNum, num); r.lab->visibleExp = ExpReplaceInt(t.lab->visibleExp, t.hlNum, num);
    if (visFrom) { r.bar->visibleExp = visFrom->visibleExp; r.btn->visibleExp = visFrom->visibleExp; }
    SetText(r.btn, label); SetText(r.lab, label);
    char foc[160]; snprintf(foc, sizeof foc, "play \"uin_navigation_over\" ; setLocalVarInt ui_highlight %d ; setLocalVarString ui_choicegroup \"pausemenu\" ;", num);
    r.btn->onEvent = Handler("onFocus", foc, Handler("leaveFocus", "setLocalVarInt ui_highlight 0 ;", Handler("action", action, nullptr)));
    out.insert(out.end(), { r.bar, r.hl, r.btn, r.lab });
    return r;
}
void AppendItems(menuDef_t *m, const std::vector<itemDef_s *> &add)
{
    const int n = m->itemCount + (int)add.size();
    PTR32(itemDef_s) *arr = (PTR32(itemDef_s) *)Ptr32_ArenaAlloc(sizeof(PTR32(itemDef_s)) * n, 4);
    for (int i = 0; i < m->itemCount; ++i) arr[i] = m->items[i];
    for (size_t i = 0; i < add.size(); ++i) arr[m->itemCount + i] = add[i];
    m->items = arr; m->itemCount = n;
}
void Enable(const Row &r, bool on)
{
    float *c = r.btn->window.foreColor; static float keep[4]; static bool saved;
    if (!saved) { memcpy(keep, c, sizeof keep); saved = true; }
    if (on) memcpy(c, keep, sizeof keep); else { c[0] = c[1] = c[2] = 0.4f; c[3] = 0.5f; }   // como SETTINGS deshabilitado (I94, h18_maps.cpp)
}
std::string Upper(const std::string &s) { std::string o; for (char c : s) o += (char)toupper((unsigned char)c); return o; }

// --- menu h11_coop
enum { kSlots = 4 };
menuDef_t *s_menu;
Row s_rowMain, s_rowSlot[kSlots], s_rowLeave;
std::string s_txtMain, s_txtSlot[kSlots], s_txtLeave;
int s_slotGame[kSlots];
bool s_patched, s_menuOpen, s_autoOpened;
void (*s_prevPump)();
cmd_function_s s_cmd[6];

void SetRowText(Row &r, std::string &cache, const std::string &txt, bool enabled)
{
    if (cache != txt) { cache = txt; SetText(r.btn, txt.c_str()); SetText(r.lab, txt.c_str()); }
    Enable(r, enabled);
}

// Unirse lleva a este proceso a ser CLIENTE REMOTO (h11_net_cl.cpp). Hoy ese cliente habla el protocolo completo y juega (se mueve,
// dispara, suma puntos) pero todavia no dibuja nada: su vista (fase 2: mundo, jugadores y zombis del snapshot, HUD, tactil) es lo
// siguiente. Hasta entonces la fila se ve pero solo se activa con H11L_JOIN=1 (pruebas), para no dejar a nadie en una pantalla quieta.
bool JoinReady() { static const bool on = getenv("H11L_JOIN") && atoi(getenv("H11L_JOIN")); return on; }

void Refresh()
{
    if (!s_patched) return;
    char b[96];
    if (s_mode == "host" && H11_HostActive())
    {
        if (H11_WaitingFor()) { snprintf(b, sizeof b, "START NOW (%d/%d PLAYERS)", H11_ConnectedPlayers(), H11_MaxPlayers()); SetRowText(s_rowMain, s_txtMain, b, true); }
        else { snprintf(b, sizeof b, "HOSTING: %d/%d PLAYERS", H11_ConnectedPlayers(), H11_MaxPlayers()); SetRowText(s_rowMain, s_txtMain, b, false); }
    }
    else SetRowText(s_rowMain, s_txtMain, "HOST LOCAL GAME (UP TO 4)", true);
    const bool canJoin = s_mode != "host";
    for (int i = 0; i < kSlots; ++i)
    {
        s_slotGame[i] = -1;
        if (i < (int)s_games.size())
        {
            const Game &g = s_games[i];
            const bool full = g.players >= g.max;
            const bool ok = canJoin && g.lobby && !full && JoinReady();
            snprintf(b, sizeof b, "%s%s - %s (%d/%d)%s", JoinReady() ? "JOIN " : "", Upper(g.name).c_str(), H18_MapName(g.map.c_str()), g.players, g.max,
                !g.lobby ? " STARTED" : full ? " FULL" : !JoinReady() ? " - CLIENT VIEW NOT READY" : "");
            if (ok) s_slotGame[i] = i;
            SetRowText(s_rowSlot[i], s_txtSlot[i], b, ok);
        }
        else SetRowText(s_rowSlot[i], s_txtSlot[i], i > 0 ? " " : s_mode == "host" ? "OTHERS JOIN FROM THEIR LOCAL CO-OP MENU" : s_scanning ? "SEARCHING THIS NETWORK..." : "NO LOCAL GAMES", false);
    }
    SetRowText(s_rowLeave, s_txtLeave, s_mode == "solo" ? "SEARCH AGAIN" : "LEAVE AND PLAY SOLO", true);
    s_dirty = false;
}

void Frame()
{
    if (s_prevPump) s_prevPump();
    if (!s_patched) return;
    if (!s_autoOpened && getenv("H11_LOBBYUI") && H11_HostActive() && H11_WaitingFor()) { s_autoOpened = true; H10_Open("h11_coop"); }
    ScanPoll();
    static int lastN = -1, lastW = -1;
    const int n = H11_HostActive() ? H11_ConnectedPlayers() : 0, w = H11_WaitingFor();
    if (n != lastN || w != lastW) { lastN = n; lastW = w; s_dirty = true; }
    if (s_dirty) Refresh();
}

// el anfitrion no busca partidas (se veria a si mismo y no puede unirse a otra sin dejar la suya)
void Cmd_Scan() { s_menuOpen = true; s_dirty = true; if (s_mode == "host") return; ScanOpen(); s_scanning = s_scanFd >= 0; s_lastProbe = 0; }
void Cmd_ScanStop() { s_menuOpen = false; ScanClose(); s_games.clear(); s_dirty = true; }
void Cmd_Main()
{
    if (s_mode == "host" && H11_HostActive()) { if (H11_WaitingFor()) { fprintf(stderr, "[H11L] el anfitrion empieza la partida\n"); H11_StartNow(); } return; }
    char l[96]; snprintf(l, sizeof l, "host 4 %s", H18_Map()); Relaunch(l);
}
void Cmd_Join()
{
    const int i = Cmd_Argc() > 1 ? atoi(Cmd_Argv(1)) : -1;
    if (i < 0 || i >= kSlots || s_slotGame[i] < 0 || s_slotGame[i] >= (int)s_games.size()) return;
    const Game &g = s_games[s_slotGame[i]];
    char l[128]; snprintf(l, sizeof l, "join %u.%u.%u.%u:%d %s", g.ip[0], g.ip[1], g.ip[2], g.ip[3], g.port, g.map.c_str());
    Relaunch(l);
}
void Cmd_Leave()
{
    if (s_mode == "solo") { Cmd_Scan(); s_games.clear(); return; }
    char l[96]; snprintf(l, sizeof l, "solo %s", H18_Map()); Relaunch(l);
}

void Patch(UiContext *dc)
{
    menuDef_t *pause = Menus_FindByName(dc, "pausedmenu");
    if (!pause || dc->menuCount >= 599) { fprintf(stderr, "[H11L] sin pausedmenu: no hay menu de partida local\n"); return; }
    const int ctx = dc->contextIndex;
    const int ib = FindRowBtn(pause, ctx, "@MENU_ZOMBIE_RESUMEGAME_CAPS"), iq = FindRowBtn(pause, ctx, "@PLATFORM_QUIT_CAPS"), ir = FindRowBtn(pause, ctx, "@MENU_RESTART_LEVEL_CAPS", true);
    Row tr, tq;
    if (ib < 0 || iq < 0 || !RowAt(pause, ib, &tr) || !RowAt(pause, iq, &tq)) { fprintf(stderr, "[H11L] pausedmenu con otra disposicion (%d/%d): sin menu de partida local\n", ib, iq); return; }
    // menu h11_coop: copia del menu de pausa (fondo y titulo), filas propias
    menuDef_t *nm = (menuDef_t *)Ptr32_ArenaAlloc(sizeof(menuDef_t), 8); memcpy(nm, pause, sizeof(menuDef_t));
    nm->window.name = A("h11_coop"); nm->onKey = nullptr; nm->itemCount = 0; nm->items = nullptr;
    nm->onEvent = Handler("onOpen", "showMenu \"pausedbkgnd\" ; focusfirst ; exec h11_scan ;", Handler("onClose", "hideMenu \"pausedbkgnd\" ; exec h11_scanstop ;", Handler("onESC", "close h11_coop ; open pausedmenu ;", nullptr)));
    std::vector<itemDef_s *> items;
    for (int i = 0; i < pause->itemCount; ++i) { itemDef_s *it = pause->items[i].get(); textDef_s *t = it->type == 1 ? it->typeData.textDef.get() : nullptr;
        if (t && t->text && !strcmp(t->text.get(), "@MENU_PAUSED_CAPS")) { itemDef_s *ti = CloneItem(it, nm, it->window.rect.y); SetText(ti, "LOCAL CO-OP"); items.push_back(ti); } }
    const float y0 = tr.btn->window.rect.y, dy = 19.0f;
    int row = 0;
    s_rowMain = MakeRow(tr, nm, items, y0 + dy * row, 1 + row, "HOST LOCAL GAME", "play \"uin_navigation_click\" ; exec h11_main ;", nullptr); ++row;
    for (int i = 0; i < kSlots; ++i) { char act[96]; snprintf(act, sizeof act, "play \"uin_navigation_click\" ; exec \"h11_join %d\" ;", i);
        s_rowSlot[i] = MakeRow(tr, nm, items, y0 + dy * row, 1 + row, " ", act, nullptr); ++row; }
    s_rowLeave = MakeRow(tr, nm, items, y0 + dy * row, 1 + row, "SEARCH AGAIN", "play \"uin_navigation_click\" ; exec h11_leave ;", nullptr); ++row;
    MakeRow(tr, nm, items, y0 + dy * row, 1 + row, "BACK", "play \"uin_navigation_back\" ; close h11_coop ; open pausedmenu ;", nullptr);
    AppendItems(nm, items);
    dc->Menus[dc->menuCount++] = nm; s_menu = nm;
    // fila LOCAL CO-OP en la pausa, en el sitio de QUIT (QUIT baja una fila), visible cuando lo es RESTART LEVEL (partida propia)
    const float qy = tq.btn->window.rect.y;
    for (itemDef_s *it : { tq.bar, tq.hl, tq.btn, tq.lab }) { it->window.rectClient.y += dy; it->window.rect.y += dy; }
    std::vector<itemDef_s *> add;
    MakeRow(tr, pause, add, qy, 31, "LOCAL CO-OP", "play \"uin_navigation_click\" ; close pausedmenu ; open h11_coop ;", ir >= 0 ? pause->items[ir].get() : nullptr);
    AppendItems(pause, add);
    Cmd_AddCommandInternal("h11_scan", Cmd_Scan, &s_cmd[0]); Cmd_AddCommandInternal("h11_scanstop", Cmd_ScanStop, &s_cmd[1]);
    Cmd_AddCommandInternal("h11_main", Cmd_Main, &s_cmd[2]); Cmd_AddCommandInternal("h11_join", Cmd_Join, &s_cmd[3]);
    Cmd_AddCommandInternal("h11_leave", Cmd_Leave, &s_cmd[4]);
    s_patched = true;
    s_prevPump = g_h5pNetPump; g_h5pNetPump = Frame;   // una vez por frame de render (encadena la del anfitrion, h11_net.cpp)
    Refresh();
    fprintf(stderr, "[H11L] menu de partida local: fila LOCAL CO-OP en pausedmenu y menu h11_coop (modo %s)\n", s_mode.c_str());
}

void (*s_prevPatch)(UiContext *);
void PatchChain(UiContext *dc) { if (s_prevPatch) s_prevPatch(dc); if (!getenv("H11L") || atoi(getenv("H11L"))) Patch(dc); }

// Encadena el parche de menus de h18_maps.cpp (g_h18PatchMenus esta inicializado de forma constante: se puede leer aqui).
struct Install { Install() { s_prevPatch = g_h18PatchMenus; g_h18PatchMenus = PatchChain; } } s_install;

} // namespace
#endif
