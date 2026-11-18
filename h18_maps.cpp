// OLA M0: MAPAS. (1) Resolucion del mapa de arranque y de su carpeta de datos (sin rutas fijas: todo sale de P3_GAMEDATA/BO1_MAPS en
// ejecucion). (2) Selector de mapas dentro del MENU ORIGINAL de pausa (filas clonadas de pausedmenu: mismo aspecto, tactil, mando y
// raton por el camino de h10_settings.cpp). (3) Catalogo compilado (maps/maps_catalog.inc, generado por maps/bo1maps.py desde los
// manifiestos de vel.gg: solo rutas y tamanos, ningun dato del juego).
//
// Carpetas:
//   base  = P3_GAMEDATA al arrancar (Mac: native/gamedata; Android: <files>/game). Es la instalacion "de serie" (Five en desarrollo).
//   raiz  = BO1_MAPS, o <base>/../gamedata_maps si existe (Mac, desarrollo: bo1maps.py fetch). En Android la app pone <files>/maps.
//   Un mapa vive en <raiz>/<zona>/ con la misma estructura que base (zone/, main/, localization.txt, playlists_sp.info, video/,
//   h6a/wma_adpcm.pak, tex.pkg). Los ajustes (players/config.cfg) son SIEMPRE los de base: H10_CFG apunta alli si nadie lo fijo.
// Mapa de arranque (primero que exista):
//   1. H5_MAP o P4_MAP explicitos (pruebas, scripts): carpeta = base si base tiene la zona, si no <raiz>/<zona>.
//   2. Mapa elegido en el menu: <base>/players/bo1_map.txt (Android siempre; Mac solo con ventana P4_WINDOW o H18_MAPFILE=1, asi las
//      pruebas sin ventana arrancan siempre en el mapa por defecto). Carpeta = <raiz>/<zona> si esta completa, si no base.
//   3. Por defecto: zombie_pentagon en base (como hasta ahora).
// "auto" o vacio en H5_MAP/P4_MAP = sin mapa explicito (la app Android lo usa para delegar aqui).
// Variables: BO1_MAPS (raiz), H18_MAPFILE=0|1, H18=0 (sin fila MAPS ni menu), H18_TRACE=1.
#include <universal/ptr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static const char *const kDefaultMap = "zombie_pentagon";

struct H18File { const char *path; unsigned size; };
struct H18Map { const char *zone, *slug, *name, *video; std::vector<H18File> files; };
static const std::vector<H18Map> &Catalog()
{
    static const std::vector<H18Map> v = [] {
        std::vector<H18Map> v;
#define H18_MAP_BEGIN(z, s, n, vid) v.push_back({ z, s, n, vid, {} });
#define H18_FILE(p, sz) v.back().files.push_back({ p, sz });
#define H18_MAP_END()
#include "maps/maps_catalog.inc"
#undef H18_MAP_BEGIN
#undef H18_FILE
#undef H18_MAP_END
        return v;
    }();
    return v;
}
static const H18Map *Find(const char *zone) { for (const H18Map &m : Catalog()) if (zone && !strcmp(m.zone, zone)) return &m; return nullptr; }

static std::string s_base, s_root, s_dir, s_map, s_from;
static int s_argc; static char **s_argv;
static bool IsFile(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
static bool IsDir(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static bool ZoneIn(const std::string &dir, const char *zone) { return !dir.empty() && IsFile(dir + "/zone/Common/" + zone + ".ff"); }
// ruta del catalogo -> ruta en disco ("L:" = fichero de idioma; solo hay ingles por ahora: la ruta inglesa tal cual)
static std::string CatPath(const char *p) { return !strncmp(p, "L:", 2) ? p + 2 : p; }
static int Missing(const std::string &dir, const H18Map &m)
{
    int n = 0; for (const H18File &f : m.files) n += !IsFile(dir + "/" + CatPath(f.path));
    return n;
}
static bool Complete(const std::string &dir, const H18Map &m) { return !dir.empty() && IsDir(dir) && Missing(dir, m) == 0; }
// carpeta para un mapa elegido en el menu: la completa de la raiz; si no, base si tiene la zona; "" = no se puede jugar
static std::string PickDir(const char *zone)
{
    const H18Map *m = Find(zone);
    if (m && !s_root.empty() && Complete(s_root + "/" + zone, *m)) return s_root + "/" + zone;
    if (ZoneIn(s_base, zone)) return s_base;
    return "";
}
static bool Explicit(const char *v) { return v && *v && strcmp(v, "auto"); }
static bool MapFileOn()
{
    if (const char *e = getenv("H18_MAPFILE")) return atoi(e) != 0;
#ifdef BO1_APP
    return true;
#else
    return getenv("P4_WINDOW") != nullptr;
#endif
}
static std::string MapFile() { return s_base + "/players/bo1_map.txt"; }

const char *H18_DefaultMap() { return kDefaultMap; }
const char *H18_Map() { return s_map.empty() ? kDefaultMap : s_map.c_str(); }
const char *H18_MapDir() { return s_dir.c_str(); }

// al principio de main / bo1_engine_main, antes de leer P3_GAMEDATA o el config
void H18_ResolveBoot(int argc, char **argv)
{
    s_argc = argc; s_argv = argv;
    const char *b = getenv("BO1_GAMEDATA_BASE"); if (!b) b = getenv("P3_GAMEDATA"); s_base = b ? b : "../../gamedata";
    setenv("BO1_GAMEDATA_BASE", s_base.c_str(), 1);   // un relanzamiento (cambio de mapa en el Mac) parte de la misma base
    { extern void (*g_h11ResolveBoot)(const char *, int, char **); if (g_h11ResolveBoot) g_h11ResolveBoot(s_base.c_str(), argc, argv); }   // OLA C1: partida local (players/bo1_net.txt, h11_lobby.cpp)
    if (const char *r = getenv("BO1_MAPS")) s_root = r;
    else if (IsDir(s_base + "/../gamedata_maps")) s_root = s_base + "/../gamedata_maps";
    const char *h5 = getenv("H5_MAP"), *p4 = getenv("P4_MAP");
    std::string want;
    if (Explicit(h5)) { want = h5; s_from = "H5_MAP"; }
    else if (Explicit(p4)) { want = p4; s_from = "P4_MAP"; }
    if (!want.empty()) s_dir = ZoneIn(s_base, want.c_str()) ? s_base : (!s_root.empty() && ZoneIn(s_root + "/" + want, want.c_str()) ? s_root + "/" + want : s_base);
    if (want.empty() && MapFileOn()) {
        if (FILE *f = fopen(MapFile().c_str(), "r")) {
            char z[64] = ""; if (fscanf(f, "%63s", z) == 1 && Find(z)) { std::string d = PickDir(z); if (!d.empty()) { want = z; s_dir = d; s_from = "menu (bo1_map.txt)"; }
                else fprintf(stderr, "[H18] bo1_map.txt pide %s pero no hay datos jugables: mapa por defecto\n", z); }
            fclose(f);
        }
    }
    if (want.empty()) { want = kDefaultMap; s_from = "defecto"; s_dir = ZoneIn(s_base, kDefaultMap) ? s_base : PickDir(kDefaultMap); if (s_dir.empty()) s_dir = s_base; }
    s_map = want;
    if (!getenv("H10_CFG")) setenv("H10_CFG", (s_base + "/players/config.cfg").c_str(), 1);   // ajustes comunes a todos los mapas
    if (s_dir != s_base) {
        setenv("P3_GAMEDATA", s_dir.c_str(), 1);
        // paquete de texturas ASTC: es por mapa (<carpeta del mapa>/tex.pkg); el de base no sirve para otro mapa
        if (IsFile(s_dir + "/tex.pkg")) setenv("V9_TEXPKG", (s_dir + "/tex.pkg").c_str(), 1); else unsetenv("V9_TEXPKG");
    }
    setenv("H5_MAP", s_map.c_str(), 1);
    if (p4) setenv("P4_MAP", s_map.c_str(), 1);
    const H18Map *m = Find(s_map.c_str());
    fprintf(stderr, "[H18] mapa %s (%s), origen %s, datos %s%s, raiz de mapas %s\n", s_map.c_str(), m ? m->name : "fuera del catalogo", s_from.c_str(),
        s_dir == s_base ? "base " : "", s_dir.c_str(), s_root.empty() ? "(ninguna)" : s_root.c_str());
    if (m) { const int n = Missing(s_dir, *m); if (n) fprintf(stderr, "[H18] aviso: a %s le faltan %d de %zu ficheros del catalogo (bo1maps.py check)\n", s_dir.c_str(), n, m->files.size()); }
}

// ---------------------------------------------------------------- video de carga (<zona>_load.webm, VP9 + Opus de vel.gg)
// OLA M0: diseno + gancho; sin reproductor todavia. El que lo implemente (plataforma) rellena g_h18PlayVideo:
//   Android: AMediaExtractor + AMediaCodec (VP9 y Opus nativos del sistema) sobre la superficie de la app, sin dependencias nuevas.
//   Mac: demuxer WebM propio (EBML, solo SimpleBlock) + VideoToolbox VP9 (VTRegisterSupplementalVideoDecoderIfAvailable) + AudioToolbox Opus.
// Contrato: se llama una vez al entrar en el mapa, antes de cargar las zonas; bloquea hasta el final del video o un toque/tecla
// (saltar) y devuelve false si no pudo (falta el fichero, codec no disponible, error): el juego sigue sin video y sin error.
bool (*g_h18PlayVideo)(const char *path) = nullptr;
void H18_PlayLoadVideo()
{
    const H18Map *m = Find(H18_Map()); if (!m || !m->video || !*m->video || getenv("H18_NOVIDEO")) return;
    const std::string path = s_dir + "/" + m->video;
    if (!IsFile(path)) { fprintf(stderr, "[H18] video de carga: no hay %s (se sigue sin video)\n", path.c_str()); return; }
    if (!g_h18PlayVideo) { fprintf(stderr, "[H18] video de carga: %s presente; reproductor pendiente (stub OLA M0), se salta\n", path.c_str()); return; }
    const bool ok = g_h18PlayVideo(path.c_str());
    fprintf(stderr, "[H18] video de carga %s: %s\n", path.c_str(), ok ? "reproducido" : "fallo, se salta");
}

// cambio de mapa (menu): se guarda la eleccion y se relanza (el motor no es reentrante, como "Restart level")
extern void (*g_h10RestartApp)();
void H10_SaveNow();
static void Relaunch(const char *zone)
{
    if (FILE *f = fopen(MapFile().c_str(), "w")) { fprintf(f, "%s\n", zone); fclose(f); }
    else fprintf(stderr, "[H18] no se pudo escribir %s\n", MapFile().c_str());
    fprintf(stderr, "[H18] cambio de mapa -> %s (%s)\n", zone, g_h10RestartApp ? "reinicio la app" : "relanzo el proceso");
    fflush(stderr);
    if (g_h10RestartApp) { g_h10RestartApp(); return; }
#ifndef BO1_APP
    if (!s_argv) return;
    // mismo binario y argumentos; el mapa sale de bo1_map.txt (H18_MAPFILE=1) con la base original
    unsetenv("H5_MAP"); if (getenv("P4_MAP")) setenv("P4_MAP", "auto", 1);
    setenv("P3_GAMEDATA", s_base.c_str(), 1); setenv("H18_MAPFILE", "1", 1); unsetenv("H10_CFG");
    char exe[4096] = ""; uint32_t n = sizeof exe;
#ifdef __APPLE__
    if (_NSGetExecutablePath(exe, &n) != 0) strcpy(exe, s_argv[0]);
#else
    { ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1); if (r > 0) exe[r] = 0; else strcpy(exe, s_argv[0]); }
#endif
    execv(exe, s_argv);
    fprintf(stderr, "[H18] execv fallo\n");
#endif
}

#ifdef H5G
// ================================================================ selector dentro del menu original (H5G: hay UI de partida)
#include <database/db_registry.h>
#include <universal/dvar.h>
#include <universal/com_expressions.h>
#include <qcommon/cmd.h>
#include <ui/ui_shared.h>
#include <ui/ui_utils.h>

static menuDef_t *s_pause, *s_menu;
static cmd_function_s s_cmdMap;
static bool Trace() { static const bool t = getenv("H18_TRACE") && atoi(getenv("H18_TRACE")); return t; }
static const char *A(const char *s) { return Ptr32_ArenaStrdup(s); }

static itemDef_s *CloneItem(itemDef_s *src, menuDef_t *parent, float y)
{
    itemDef_s *it = (itemDef_s *)Ptr32_ArenaAlloc(sizeof(itemDef_s), 8); memcpy(it, src, sizeof(itemDef_s));
    if (src->typeData.textDef && Item_GetTextDef(src) == src->typeData.textDef.get()) {   // textDef propio (texto y expresion cambian por fila)
        textDef_s *t = (textDef_s *)Ptr32_ArenaAlloc(sizeof(textDef_s), 4); memcpy(t, src->typeData.textDef.get(), sizeof(textDef_s)); it->typeData.textDef = t;
        if (t->textExpData) { ExpressionStatement *e = (ExpressionStatement *)Ptr32_ArenaAlloc(sizeof(ExpressionStatement), 4); memcpy(e, t->textExpData.get(), sizeof(ExpressionStatement)); t->textExpData = (textExp_s *)e; }
    }
    if (src->animInfo) { UIAnimInfo *a = (UIAnimInfo *)Ptr32_ArenaAlloc(sizeof(UIAnimInfo), 4); memcpy(a, src->animInfo.get(), sizeof(UIAnimInfo)); it->animInfo = a; }
    it->parent = parent;
    it->window.rectClient.y += y - it->window.rect.y; it->window.rect.y = y;
    return it;
}
// expresion copiada con la constante entera 'from' cambiada por 'to' (numero de fila de ui_highlight)
static ExpressionStatement ExpReplaceInt(const ExpressionStatement &src, int from, int to)
{
    ExpressionStatement e = src; if (!src.numRpn || !src.rpn) return e;
    expressionRpn *r = (expressionRpn *)Ptr32_ArenaAlloc(sizeof(expressionRpn) * e.numRpn, 4); memcpy(r, src.rpn.get(), sizeof(expressionRpn) * e.numRpn);
    for (int i = 0; i < e.numRpn; ++i) if (r[i].type == 0 && r[i].data.constant.dataType == VAL_INT && r[i].data.constant.internals.intVal == from) r[i].data.constant.internals.intVal = to;
    e.rpn = r; return e;
}
static void SetText(itemDef_s *it, const char *txt)
{
    textDef_s *t = it->typeData.textDef.get(); if (!t) return;
    if (t->textExpData) {
        ExpressionStatement *e = (ExpressionStatement *)t->textExpData.get(); expressionRpn *r = (expressionRpn *)Ptr32_ArenaAlloc(sizeof(expressionRpn) * e->numRpn, 4); memcpy(r, e->rpn.get(), sizeof(expressionRpn) * e->numRpn);
        for (int i = 0; i < e->numRpn; ++i) if (r[i].type == 0 && r[i].data.constant.dataType == VAL_STRING) r[i].data.constant.internals.string = (char *)A(txt);
        e->rpn = r;
    } else t->text = A(txt);
}
static GenericEventScript *Script(const char *action)
{
    GenericEventScript *s = (GenericEventScript *)Ptr32_ArenaAlloc(sizeof(GenericEventScript), 4); s->action = A(action); s->fireOnTrue = true; return s;
}
static GenericEventHandler *Handler(const char *name, const char *action, GenericEventHandler *next)
{
    GenericEventHandler *h = (GenericEventHandler *)Ptr32_ArenaAlloc(sizeof(GenericEventHandler), 4); h->name = A(name); h->eventScript = Script(action); h->next = next; return h;
}
static bool HasAction(itemDef_s *it, const char *what)
{
    for (GenericEventHandler *h = it->onEvent.get(); h; h = h->next.get()) if (h->name && !I_stricmp(h->name.get(), "action"))
        for (GenericEventScript *s = h->eventScript.get(); s; s = s->next.get()) if (s->action && strstr(s->action.get(), what)) return true;
    return false;
}
// fila del menu de pausa = 4 items con el mismo rect: barra, barra resaltada, boton (accion + texto) y etiqueta resaltada
struct Row { itemDef_s *bar, *hl, *btn, *lab; int hlNum; };
static bool RowAt(menuDef_t *m, int b, Row *r)
{
    if (b < 2 || b + 1 >= m->itemCount) return false;
    itemDef_s *x[4] = { m->items[b - 2].get(), m->items[b - 1].get(), m->items[b].get(), m->items[b + 1].get() };
    for (itemDef_s *i : x) if (i->window.rect.y != x[2]->window.rect.y) return false;
    r->bar = x[0]; r->hl = x[1]; r->btn = x[2]; r->lab = x[3]; r->hlNum = -1;
    for (GenericEventHandler *h = r->btn->onEvent.get(); h; h = h->next.get()) if (h->name && !I_stricmp(h->name.get(), "onFocus"))
        for (GenericEventScript *s = h->eventScript.get(); s; s = s->next.get()) if (s->action) { const char *p = strstr(s->action.get(), "ui_highlight "); if (p) r->hlNum = atoi(p + 13); }
    return r->hlNum > 0;
}
static bool HasText(itemDef_s *it, const char *str)
{
    textDef_s *t = Item_GetTextDef(it); if (!t || !t->textExpData) return false;
    ExpressionStatement *e = (ExpressionStatement *)t->textExpData.get(); expressionRpn *r = e->rpn.get();
    for (int i = 0; r && i < e->numRpn; ++i) if (r[i].type == 0 && r[i].data.constant.dataType == VAL_STRING && r[i].data.constant.internals.string && !strcmp(r[i].data.constant.internals.string.get(), str)) return true;
    return false;
}
// boton de fila (con 'action') cuyo texto es 'label', visible en el estado actual (zombies); si ninguno lo es y anyVis, el primero
static int FindRowBtn(menuDef_t *m, int ctx, const char *label, bool anyVis = false)
{
    int first = -1;
    for (int i = 0; i < m->itemCount; ++i) { itemDef_s *it = m->items[i].get(); if (!HasAction(it, "") || !HasText(it, label)) continue;
        if (Item_IsVisible(0, ctx, it)) return i; if (first < 0) first = i; }
    return anyVis ? first : -1;
}
// fila nueva a partir de la plantilla: texto, accion, numero de resaltado y visibilidad (la del boton 'visFrom' si se da)
static void MakeRow(const Row &t, menuDef_t *m, std::vector<itemDef_s *> &out, float y, int num, const char *label, const char *action, bool enabled, itemDef_s *visFrom)
{
    itemDef_s *bar = CloneItem(t.bar, m, y), *hl = CloneItem(t.hl, m, y), *btn = CloneItem(t.btn, m, y), *lab = CloneItem(t.lab, m, y);
    hl->visibleExp = ExpReplaceInt(t.hl->visibleExp, t.hlNum, num); lab->visibleExp = ExpReplaceInt(t.lab->visibleExp, t.hlNum, num);
    if (visFrom) { bar->visibleExp = visFrom->visibleExp; btn->visibleExp = visFrom->visibleExp; }
    SetText(btn, label); SetText(lab, label);
    char foc[160]; snprintf(foc, sizeof foc, "play \"uin_navigation_over\" ; setLocalVarInt ui_highlight %d ; setLocalVarString ui_choicegroup \"pausemenu\" ;", num);
    btn->onEvent = Handler("onFocus", foc, Handler("leaveFocus", "setLocalVarInt ui_highlight 0 ;", Handler("action", action, nullptr)));
    if (!enabled) { btn->window.foreColor[0] = btn->window.foreColor[1] = btn->window.foreColor[2] = 0.4f; btn->window.foreColor[3] = 0.5f; }   // como SETTINGS deshabilitado (I94)
    out.insert(out.end(), { bar, hl, btn, lab });
}
static void AppendItems(menuDef_t *m, const std::vector<itemDef_s *> &add)
{
    const int n = m->itemCount + (int)add.size();
    PTR32(itemDef_s) *arr = (PTR32(itemDef_s) *)Ptr32_ArenaAlloc(sizeof(PTR32(itemDef_s)) * n, 4);
    for (int i = 0; i < m->itemCount; ++i) arr[i] = m->items[i];
    for (size_t i = 0; i < add.size(); ++i) arr[m->itemCount + i] = add[i];
    m->items = arr; m->itemCount = n;
}
// mayusculas en el juego de caracteres de las fuentes del juego (Latin-1): el catalogo viene en UTF-8 (Verrückt)
static std::string Upper(const char *s)
{
    std::string o;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        unsigned c = *p;
        if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (p[1] & 0x3F); ++p; }
        if (c >= 0xE0 && c <= 0xFE && c != 0xF7) c -= 0x20;   // a-grave..thorn -> mayuscula
        o += (char)(c < 0x80 ? toupper((int)c) : (c < 0x100 ? (int)c : '?'));
    }
    return o;
}
const char *H18_MapName(const char *zone) { static std::string n; const H18Map *m = Find(zone); n = Upper(m ? m->name : zone); return n.c_str(); }   // OLA C1 (h11_lobby.cpp)
// estado de cada mapa para el menu y el registro
struct MapState { const H18Map *m; std::string dir; int missing; bool playable, current; };
static std::vector<MapState> States()
{
    std::vector<MapState> v;
    for (const H18Map &m : Catalog()) {
        MapState s{ &m, PickDir(m.zone), 0, false, s_map == m.zone };
        const std::string full = s_root.empty() ? "" : s_root + "/" + m.zone;
        s.missing = IsDir(full) ? Missing(full, m) : (int)m.files.size();
        s.playable = !s.dir.empty();
        v.push_back(s);
    }
    return v;
}
static void Cmd_H18Map()
{
    const char *z = Cmd_Argc() > 1 ? Cmd_Argv(1) : "";
    const H18Map *m = Find(z);
    if (!m) { fprintf(stderr, "[H18] h18_map: mapa desconocido '%s'\n", z); return; }
    if (PickDir(z).empty()) { fprintf(stderr, "[H18] h18_map: %s sin datos jugables (bo1maps.py check)\n", z); return; }
    H10_SaveNow();
    Relaunch(z);
}
void H18_PatchMenus(UiContext *dc)
{
    if (getenv("H18") && !atoi(getenv("H18"))) return;
    s_pause = Menus_FindByName(dc, "pausedmenu");
    if (!s_pause || dc->menuCount >= 599) { fprintf(stderr, "[H18] sin pausedmenu: no hay selector de mapas\n"); return; }
    const int ctx = dc->contextIndex;
    const int ib = FindRowBtn(s_pause, ctx, "@MENU_ZOMBIE_RESUMEGAME_CAPS");   // RESUME de zombies (plantilla de aspecto)
    const int iq = FindRowBtn(s_pause, ctx, "@PLATFORM_QUIT_CAPS");            // QUIT (baja una fila)
    const int ir = FindRowBtn(s_pause, ctx, "@MENU_RESTART_LEVEL_CAPS", true); // RESTART LEVEL (su visibilidad: partida propia)
    Row tr, tq;
    if (ib < 0 || iq < 0 || !RowAt(s_pause, ib, &tr) || !RowAt(s_pause, iq, &tq)) { fprintf(stderr, "[H18] pausedmenu con otra disposicion (%d/%d): sin selector\n", ib, iq); return; }
    if (Trace()) fprintf(stderr, "[H18] plantilla RESUME item %d (resaltado %d), QUIT item %d, RESTART item %d\n", ib, tr.hlNum, iq, ir);
    // 1. menu nuevo "h18_maps": copia del menu de pausa (mismo fondo, titulo y filas) con una fila por mapa + BACK
    menuDef_t *nm = (menuDef_t *)Ptr32_ArenaAlloc(sizeof(menuDef_t), 8); memcpy(nm, s_pause, sizeof(menuDef_t));
    nm->window.name = A("h18_maps"); nm->onKey = nullptr; nm->itemCount = 0; nm->items = nullptr;
    nm->onEvent = Handler("onOpen", "showMenu \"pausedbkgnd\" ; focusfirst ;", Handler("onClose", "hideMenu \"pausedbkgnd\" ;", Handler("onESC", "close h18_maps ; open pausedmenu ;", nullptr)));
    std::vector<itemDef_s *> items;
    for (int i = 0; i < s_pause->itemCount; ++i) { itemDef_s *it = s_pause->items[i].get(); textDef_s *t = it->type == 1 ? it->typeData.textDef.get() : nullptr;
        if (t && t->text && !strcmp(t->text.get(), "@MENU_PAUSED_CAPS")) { itemDef_s *ti = CloneItem(it, nm, it->window.rect.y); SetText(ti, "SELECT MAP"); items.push_back(ti); } }
    const float y0 = tr.btn->window.rect.y, dy = 19.0f;   // paso de fila de pausedmenu (75, 94, 113, 132)
    int row = 0; const std::vector<MapState> st = States();
    for (const MapState &s : st) {
        std::string lbl = Upper(s.m->name);
        if (s.current) lbl += "  (CURRENT)";
        else if (!s.playable) { char b[48]; snprintf(b, sizeof b, "  - %d FILES MISSING", s.missing); lbl += b; }
        char act[128];
        if (s.current) snprintf(act, sizeof act, "play \"uin_navigation_click\" ; close h18_maps ;");
        else if (s.playable) snprintf(act, sizeof act, "play \"uin_navigation_click\" ; exec \"h18_map %s\" ;", s.m->zone);
        else snprintf(act, sizeof act, "play \"uin_navigation_back\" ;");
        MakeRow(tr, nm, items, y0 + dy * row, 1 + row, lbl.c_str(), act, s.playable, nullptr); ++row;
        fprintf(stderr, "[H18] %-18s %-22s %s%s\n", s.m->name, s.m->zone, s.playable ? "JUGABLE en " : "", s.playable ? s.dir.c_str() : (std::to_string(s.missing) + " ficheros faltan en la raiz").c_str());
    }
    MakeRow(tr, nm, items, y0 + dy * row, 1 + row, "BACK", "play \"uin_navigation_back\" ; close h18_maps ; open pausedmenu ;", true, nullptr);
    AppendItems(nm, items);
    dc->Menus[dc->menuCount++] = nm; s_menu = nm;
    // 2. fila MAPS en el menu de pausa, en el sitio de QUIT (QUIT baja una fila); visible cuando lo es RESTART LEVEL (partida propia)
    const float qy = tq.btn->window.rect.y;
    for (itemDef_s *it : { tq.bar, tq.hl, tq.btn, tq.lab }) { it->window.rectClient.y += dy; it->window.rect.y += dy; }
    std::vector<itemDef_s *> add;
    MakeRow(tr, s_pause, add, qy, 30, "MAPS", "play \"uin_navigation_click\" ; close pausedmenu ; open h18_maps ;", true, ir >= 0 ? s_pause->items[ir].get() : nullptr);
    AppendItems(s_pause, add);
    Cmd_AddCommandInternal("h18_map", Cmd_H18Map, &s_cmdMap);
    fprintf(stderr, "[H18] selector de mapas: fila MAPS en pausedmenu y menu h18_maps (%d mapas del catalogo)\n", row);
}
void (*g_h18PatchMenus)(UiContext *) = H18_PatchMenus;   // h10_settings.cpp lo llama (puntero de datos: sin stub si falta esta TU)
#endif
