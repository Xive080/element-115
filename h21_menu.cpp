// OLA U1: MENU PRINCIPAL al estilo del original (dos pantallas, como el front-end de zombies de vel.gg): GAME SETUP (nombre, modo,
// graficos, CHOOSE MAP) y MISSION SELECT (los 10 mapas con su imagen y descripcion, HELL MODE y BACK). Mismo codigo en el Mac (raton
// y teclado) y en el Fold (tactil: H21_Touch); se dibuja con la lista 2D del renderer original (R_AddCmdDrawStretchPic/DrawText,
// fuentes del juego) y las imagenes se leen EN EJECUCION de los .iwd del usuario con el cargador de imagenes del motor
// (Image_LoadToBuffer + Image_LoadFromData, como h7_gfx.cpp): nada de datos del juego en git ni en el APK.
//   Fondos: menu_background_press_start (soldado de la portada) y menu_zombie_lobby_frame_outer_ingame (marco rojo "M 132").
//   Mapa: loadscreen_<zona> (iw_04/iw_25.iwd: las diez estan en la base). Si falta, se pinta un fondo con el nombre del mapa.
// Arranque: main2.cpp llama a g_h21Frontend despues de cargar las zonas comunes (code_* y common_zombie: fuentes y materiales) y ANTES
// del mapa. Con H21_MENU=1 (jugar_mac.sh sin argumento) corre el menu; al elegir mapa se relanza el proceso (el motor no es reentrante,
// como h18_maps.cpp y la OLA C1) con H5_MAP=<zona>; MULTIPLAYER escribe players/bo1_net.txt "host 4 <zona>" (sala de la OLA C1).
// Ficheros (en players/ de la base, junto a config.cfg): bo1_menu.txt (nombre, modo, ultimo mapa), bo1_map.txt (mapa elegido, OLA M0).
// GRAPHICS HIGH/LOW = perfiles Alto/Bajo de h10_settings.cpp (H10_CfgSetQuality: mismas claves que el menu de ajustes; LOW nunca sube
// los topes). HELL MODE: es un mod de vel.gg (mods/horde, no esta en los datos del juego): visible y marcado NOT AVAILABLE.
// Variables: H21_MENU=1 abre el menu; H21_PILOT="t accion;..." (pruebas: move/down/up/tap x y en fraccion, key n, type texto, shot tag,
// quit); H21_NOLAUNCH=1 no relanza (pruebas); H21_TRACE=1; H21_LOC=texto vuelca entradas de localizacion que lo contienen.
// OLA U2 (APK): la app Android arranca aqui (g_h21PollEvents bombea sus eventos en cada frame; H21_Touch/H21_Key/H21_Char/H21_Back),
// MISSION SELECT en dos columnas si la lista de una no cabe con filas tactiles (Fold plegado), boton START grande, textos que encogen
// para caber, descripciones MENU_ZOMBIE_*_DESC de las zonas (las 7 que faltan en las comunes viven en el en_patch.ff nuevo que traen
// las carpetas de los mapas: se carga en el menu) y, en la app, players/bo1_launch.txt para que el reinicio entre directo al mapa.
// MULTIPLAYER (co-op local) queda como hueco en la app: visible y NOT AVAILABLE (H21_COOP=1 lo abre para pruebas).
#include <database/db_registry.h>
#include <database/db_assetnames.h>
#include <universal/ptr32.h>
#include <universal/dvar.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_image_load_obj.h>
#include <gfx_d3d/r_font.h>
#include <stringed/stringed_hooks.h>
#include <ui/keycodes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <crt_externs.h>
#endif

int FS_FOpenFileRead(const char *filename, int *file);   // iwd.cpp
void FS_FCloseFile(int h);
void Z_VirtualFree(void *ptr, int type);
void H4_Shot(const char *tag);                            // world_h4.cpp (P4_OUT con sufijo)
const char *H18_BaseDir();                                // h18_maps.cpp
bool H18_MapPlayable(const char *zone);
const char *H18_MapDirFor(const char *zone);              // OLA U2: carpeta de datos jugable del mapa ("" si no hay)
void H0_LoadZoneFile(const char *path);                   // OLA U2: main2.cpp (LoadFF)
int H10_CfgQuality();                                     // h10_settings.cpp
void H10_CfgSetQuality(int q);
extern bool (*g_h10GetSafeInsets)(int W, int H, float out[4]);
extern float g_h10Dpi;
extern void (*g_h10RestartApp)();

void (*g_h21PollEvents)() = nullptr;   // OLA U2: la app Android (eventos del looper en cada frame del menu)
bool g_h21Running = false;             // OLA U2: el menu esta en pantalla (la app le manda los toques y teclas)
namespace {
bool CoopAvailable()
{
    if (const char *e = getenv("H21_COOP")) return atoi(e) != 0;
#ifdef BO1_APP
    return false;   // OLA U2: co-op local fuera de esta version del APK (sin permiso INTERNET ni vista de cliente): hueco visible
#else
    return true;
#endif
}
bool Trace() { static const bool t = getenv("H21_TRACE") && atoi(getenv("H21_TRACE")); return t; }
double NowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

// ---------------------------------------------------------------- mapas (orden y nombres del menu de vel.gg; zona = la del catalogo M0)
struct MapEntry { const char *zone, *label, *token, *fallback; };
const MapEntry kMaps[] = {
    { "zombie_theater", "KINO DER TOTEN", "THEATER", "A theater in Germany. The power is out and the undead are inside." },
    { "zombie_pentagon", "\"FIVE\"", "PENTAGON", "The Pentagon is under attack. Four world leaders hold the line." },
    { "zombie_cod5_factory", "DER RIESE", "FACTORY", "A Group 935 factory with teleporters, a Pack-a-Punch and the giant." },
    { "zombie_cod5_prototype", "NACHT DER UNTOTEN", "PROTOTYPE", "An abandoned bunker. Where it all began." },
    { "zombie_cod5_asylum", "VERR\xDC" "CKT", "ASYLUM", "A German asylum. Turn on the power and survive." },
    { "zombie_cod5_sumpf", "SHI NO NUMA", "SUMPF", "A Japanese swamp outpost with hellhounds and traps." },
    { "zombie_cosmodrome", "ASCENSION", "COSMODROME", "A Soviet launch facility. Monkeys, landers and the Gersh device." },
    { "zombie_coast", "CALL OF THE DEAD", "COAST", "A frozen coast, a film crew and George A. Romero." },
    { "zombie_temple", "SHANGRI-LA", "TEMPLE", "A lost temple in the jungle. Napalm, shriekers and geysers." },
    { "zombie_moon", "MOON", "MOON", "Griffin Station on the Moon. Low gravity and the Hacker." },
};
const int kNumMaps = (int)(sizeof kMaps / sizeof kMaps[0]);

// ---------------------------------------------------------------- recursos del motor
Material *s_white, *s_tmpl;
Font_s *s_fBig, *s_fNormal, *s_fSmall;
Material *s_bg, *s_frame, *s_mapMat[16];
std::string s_desc[16];
bool s_mapOk[16];

// imagen de los .iwd -> GfxImage (cargador del motor) -> material 2D (copia de "white" con la imagen cambiada)
Material *LoadMat(const char *name)
{
    char path[96]; snprintf(path, sizeof path, "images/%s.iwi", name);
    int fh = 0; const int sz = FS_FOpenFileRead(path, &fh); if (sz < 0) { fprintf(stderr, "[H21] imagen %s: no esta en los .iwd del usuario\n", name); return nullptr; }
    FS_FCloseFile(fh);
    if (!s_tmpl || !s_tmpl->textureTable || !s_tmpl->textureCount) return nullptr;
    const GfxImage *tim = s_tmpl->textureTable.get()[0].u.image.get();
    GfxImage *img = (GfxImage *)Ptr32_ArenaAlloc(sizeof(GfxImage), 8);
    if (tim) { img->semantic = tim->semantic; img->track = tim->track; }   // el resto a cero (Ptr32_ArenaAlloc): sin memoria de tarjeta previa
    img->name = Ptr32_ArenaStrdup(name); img->category = 3; img->mapType = 3; img->noPicmip = 1; memset(&img->picmip, 0, sizeof img->picmip); img->delayLoadPixels = 0;
    unsigned char *buf = nullptr; int size = 0;
    if (!Image_LoadToBuffer(img, true, &buf, &size) || !buf) { fprintf(stderr, "[H21] imagen %s: no se pudo leer\n", name); return nullptr; }
    Image_LoadFromData(img, (GfxImageFileHeader *)buf, buf + sizeof(GfxImageFileHeader));
    Z_VirtualFree(buf, 20);
    if (!img->texture.basemap) { fprintf(stderr, "[H21] imagen %s: el motor no la creo\n", name); return nullptr; }
    Material *m = (Material *)Ptr32_ArenaAlloc(sizeof(Material), 8); memcpy(m, s_tmpl, sizeof(Material));
    MaterialTextureDef *t = (MaterialTextureDef *)Ptr32_ArenaAlloc(sizeof(MaterialTextureDef) * s_tmpl->textureCount, 4);
    memcpy(t, s_tmpl->textureTable.get(), sizeof(MaterialTextureDef) * s_tmpl->textureCount);
    t[0].u.image = img; m->textureTable = t;
    char mn[96]; snprintf(mn, sizeof mn, "h21_%s", name); m->info.name = Ptr32_ArenaStrdup(mn);
    if (Trace()) fprintf(stderr, "[H21] imagen %s: %ux%u (%d bytes de .iwi)\n", name, img->width, img->height, size);
    return m;
}

// descripcion del mapa: entrada de localizacion del juego si la hay (zonas cargadas), si no un texto propio
void LocDump(const char *pat)
{
    struct Q { const char *p; int n; } q = { pat, 0 };
    DB_EnumXAssets_FastFile(ASSET_TYPE_LOCALIZE_ENTRY, [](XAssetHeader h, void *u) { Q *q = (Q *)u; LocalizeEntry *e = h.localize.get();
        if (!e || !e->value || !e->name) return; std::string v = e->value.get(), n = e->name.get(), lv = v, ln = n;
        for (char &c : lv) c = (char)tolower((unsigned char)c); for (char &c : ln) c = (char)tolower((unsigned char)c);
        std::string p = q->p; for (char &c : p) c = (char)tolower((unsigned char)c);
        if (lv.find(p) != std::string::npos || ln.find(p) != std::string::npos) { fprintf(stderr, "[H21] loc %s = \"%s\"\n", n.c_str(), v.c_str()); ++q->n; } }, &q, false);
    fprintf(stderr, "[H21] loc '%s': %d entradas\n", pat, q.n);
}
std::string LocFind(const char *token)
{
    struct Q { std::string tok; std::string best; } q; q.tok = token;
    DB_EnumXAssets_FastFile(ASSET_TYPE_LOCALIZE_ENTRY, [](XAssetHeader h, void *u) { Q *q = (Q *)u; LocalizeEntry *e = h.localize.get();
        if (!e || !e->value || !e->name) return; const std::string n = e->name.get();
        if (n == q->tok && q->best.empty()) q->best = e->value.get(); }, &q, false);
    return q.best;
}

// OLA U2: las descripciones de Der Riese, Nacht, Verrueckt, Shi No Numa, Call of the Dead, Shangri-La y Moon no estan en las zonas
// comunes de la base (en_code_post_gfx_mp solo trae THEATER, PENTAGON y COSMODROME): vienen en el en_patch.ff (solo localizacion). El de
// la base (8,5 KB) trae COAST, TEMPLE y MOON; el de las actualizaciones que traen las carpetas de los mapas (25 KB) ademas las cuatro de
// World at War con el prefijo WAW_. Se cargan en el menu (el
// proceso se relanza al elegir mapa, asi que no queda nada en la partida) hasta tener todas o agotar las carpetas.
long FileSize(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1; }
void LoadDescriptions()
{
    auto missing = [] { int n = 0; for (int i = 0; i < kNumMaps; ++i) n += s_desc[i].empty(); return n; };
    // claves del juego: MENU_ZOMBIE_<CLAVE>_DESC (BO1) y WAW_MENU_ZOMBIE_<CLAVE>_DESC (los cuatro mapas de World at War)
    auto lookup = [] { for (int i = 0; i < kNumMaps; ++i) for (const char *pre : { "", "WAW_" }) if (s_desc[i].empty()) { char k[64]; snprintf(k, sizeof k, "%sMENU_ZOMBIE_%s_DESC", pre, kMaps[i].token); s_desc[i] = LocFind(k); } };
    lookup();
    if (!missing()) return;
    std::vector<std::string> dirs;
    auto add = [&](const char *d) { if (d && *d) { const std::string s = d; for (const std::string &o : dirs) if (o == s) return; dirs.push_back(s); } };
    add(getenv("P3_GAMEDATA")); add(H18_BaseDir());
    for (int i = 0; i < kNumMaps; ++i) add(H18_MapDirFor(kMaps[i].zone));
    // un fichero por version (tamano distinto), el mas grande primero: el en_patch de las actualizaciones lleva las 7 descripciones y una
    // version vieja cargada antes reserva los mismos nombres (las entradas repetidas de la segunda zona no se enlazan)
    std::vector<std::pair<long, std::string>> files;
    for (const std::string &d : dirs) { const std::string f = d + "/zone/english/en_patch.ff"; const long sz = FileSize(f); if (sz <= 0) continue;
        bool seen = false; for (auto &o : files) seen |= o.first == sz; if (!seen) files.push_back({ sz, f }); }
    std::sort(files.begin(), files.end(), [](const std::pair<long, std::string> &x, const std::pair<long, std::string> &y) { return x.first > y.first; });
    int loads = 0;
    for (const auto &fz : files) {
        if (!missing() || loads >= 3) break;
        const int before = missing();
        H0_LoadZoneFile(fz.second.c_str()); ++loads; lookup();
        fprintf(stderr, "[H21] descripciones: %s (%ld bytes) -> %d de %d encontradas\n", fz.second.c_str(), fz.first, before - missing(), before);
    }
}

// ---------------------------------------------------------------- estado del menu
enum Screen { SETUP, MISSION };
enum Item { IT_NONE = -1, IT_NAME = 0, IT_SOLO, IT_COOP, IT_HIGH, IT_LOW, IT_CHOOSE, IT_MAP0 = 100, IT_HELL = 200, IT_BACK, IT_START };
Screen s_scr = SETUP;
std::string s_name = "Player";
bool s_coop = false; int s_quality = 2, s_quality0 = 2; int s_map = 0;
int s_focus = IT_SOLO; bool s_editName = false, s_nameFresh = false;   // fresh: al empezar a editar, lo primero que se escribe sustituye el nombre (como un campo con el texto seleccionado)
int s_W, s_H; float s_safe[4];
double s_t0;
bool s_launch; int s_launchMap = -1;
std::string s_notice; double s_noticeT;

std::string PlayersDir() { const char *b = H18_BaseDir(); return std::string(b && *b ? b : (getenv("P3_GAMEDATA") ? getenv("P3_GAMEDATA") : "../../gamedata")) + "/players"; }
std::string MenuFile() { return PlayersDir() + "/bo1_menu.txt"; }
bool NameChar(int c) { return isalnum(c) || c == ' ' || c == '.' || c == '_' || c == '\'' || c == '!' || c == '-'; }
void LoadPrefs()
{
    FILE *f = fopen(MenuFile().c_str(), "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        if (!strncmp(line, "name=", 5)) { std::string n; for (const char *c = line + 5; *c && n.size() < 16; ++c) if (NameChar((unsigned char)*c)) n += *c; if (!n.empty()) s_name = n; }
        else if (!strncmp(line, "mode=", 5)) s_coop = !strcmp(line + 5, "coop") && CoopAvailable();
        else if (!strncmp(line, "map=", 4)) { for (int i = 0; i < kNumMaps; ++i) if (!strcmp(kMaps[i].zone, line + 4)) s_map = i; }
    }
    fclose(f);
}
void SavePrefs()
{
    mkdir(PlayersDir().c_str(), 0755);
    const std::string p = MenuFile(), t = p + ".tmp";
    if (FILE *f = fopen(t.c_str(), "w")) { fprintf(f, "name=%s\nmode=%s\nmap=%s\n", s_name.c_str(), s_coop ? "coop" : "solo", kMaps[s_map].zone); fclose(f); rename(t.c_str(), p.c_str()); }
}

// ---------------------------------------------------------------- dibujo
float U() { const float sh = s_H - s_safe[1] - s_safe[3], sw = s_W - s_safe[0] - s_safe[2]; return fminf(sh / 720.0f, sw / 600.0f); }
// alto minimo de una fila tactil: 0.30 pulgadas (48 dp / 44 pt) con el dpi real; sin dpi, 44 px (ventana del Mac: 1 px = 1 pt)
float RowMin() { float dpi = g_h10Dpi; if (const char *e = getenv("H10_DPI")) dpi = (float)atof(e); return dpi > 0 ? 0.30f * dpi : 44.0f; }
void Rect(float x, float y, float w, float h, const float *c) { if (w > 0 && h > 0) R_AddCmdDrawStretchPic(x, y, w, h, 0, 0, 1, 1, c, s_white); }
void Frame(float x, float y, float w, float h, float t, const float *c) { Rect(x, y, w, t, c); Rect(x, y + h - t, w, t, c); Rect(x, y, t, h, c); Rect(x + w - t, y, t, h, c); }
float TextW(Font_s *f, const char *s, float px) { return f ? R_TextWidth(s, 0x7fffffff, f) * px / f->pixelHeight : 0; }
// texto con la linea base en y; px = alto de la fuente en pixeles
void Text(Font_s *f, const char *s, float x, float y, float px, const float *c) { if (f && s && *s) R_AddCmdDrawText((char *)s, 0x7fffffff, f, x, y, px / f->pixelHeight, px / f->pixelHeight, 0, c, 0); }
void TextC(Font_s *f, const char *s, float cx, float y, float px, const float *c) { Text(f, s, cx - TextW(f, s, px) * 0.5f, y, px, c); }
// OLA U2: alto de letra que cabe en maxW (en pantallas estrechas o columnas del Fold plegado los nombres largos encogen, no se salen)
float FitPx(Font_s *f, const char *s, float px, float maxW) { const float w = TextW(f, s, px); return w > maxW && w > 0 ? px * maxW / w : px; }
std::vector<std::string> Wrap(Font_s *f, const std::string &s, float px, float maxW)
{
    std::vector<std::string> out; std::string line, word;
    auto flush = [&]() { if (!word.empty()) { std::string t = line.empty() ? word : line + " " + word; if (!line.empty() && TextW(f, t.c_str(), px) > maxW) { out.push_back(line); line = word; } else line = t; word.clear(); } };
    for (char c : s) { if (c == ' ' || c == '\n') { flush(); if (c == '\n') { out.push_back(line); line.clear(); } } else word += c; }
    flush(); if (!line.empty()) out.push_back(line);
    return out;
}
// imagen que cubre el rectangulo manteniendo la proporcion (recorta lo que sobra), como un fondo de pantalla completa
void PicCover(Material *m, float x, float y, float w, float h, float iw, float ih, const float *c)
{
    float s0 = 0, t0 = 0, s1 = 1, t1 = 1; const float ra = w / h, ri = iw / ih;
    if (ra > ri) { const float v = ri / ra; t0 = (1 - v) * 0.5f; t1 = t0 + v; } else { const float v = ra / ri; s0 = (1 - v) * 0.5f; s1 = s0 + v; }
    R_AddCmdDrawStretchPic(x, y, w, h, s0, t0, s1, t1, c, m);
}

const float kWhite[4] = { 1, 1, 1, 1 }, kBlack[4] = { 0, 0, 0, 1 }, kLabel[4] = { 0.80f, 0.80f, 0.78f, 1 }, kBar[4] = { 0, 0, 0, 0.62f },
            kRed[4] = { 0.62f, 0.09f, 0.09f, 1 }, kRedHi[4] = { 0.80f, 0.14f, 0.12f, 1 }, kGrey[4] = { 0.55f, 0.55f, 0.55f, 1 }, kHint[4] = { 0.85f, 0.85f, 0.85f, 1 };

struct Hit { int id; float x, y, w, h; };
std::vector<Hit> s_hits;   // zonas tactiles del frame actual (se rehacen al dibujar)
void AddHit(int id, float x, float y, float w, float h) { s_hits.push_back({ id, x, y, w, h }); }

// fila de opcion: elegida = barra blanca con texto negro; foco = marco blanco con borde interior negro (como la captura de referencia)
void OptionRow(int id, const char *label, float x, float y, float w, float h, bool selected, bool enabled = true)
{
    const bool focus = s_focus == id; const float u = U();
    if (selected) Rect(x, y + 1, w, h - 2, kWhite); else Rect(x, y + 1, w, h - 2, kBar);
    if (focus) { Frame(x - 2, y - 1, w + 4, h + 2, 2, kWhite); if (selected) Frame(x + 1, y + 2, w - 2, h - 4, 2, kBlack); }
    float px = fminf(h * 0.70f, 31 * u);
    if (!enabled) { const char *na = "NOT AVAILABLE"; const float np = fminf(h * 0.40f, 15 * u); Text(s_fSmall, na, x + w - TextW(s_fSmall, na, np) - 10 * u, y + h * 0.5f + np * 0.36f, np, kGrey);
        px = FitPx(s_fNormal, label, px, w - 38 * u - TextW(s_fSmall, na, np) - 24 * u); }
    else px = FitPx(s_fNormal, label, px, w - 46 * u);
    Text(s_fNormal, label, x + 38 * u, y + h * 0.5f + px * 0.36f, px, selected ? kBlack : enabled ? kWhite : kGrey);
    AddHit(id, x, y, w, h);
}

void DrawSetup()
{
    const float u = U(), L = s_safe[0], T = s_safe[1], SW = s_W - s_safe[0] - s_safe[2], SH = s_H - s_safe[1] - s_safe[3];
    // fila >= objetivo tactil; si con eso no cabe la columna en la zona segura (pantalla baja), se ajusta para que quepa entera
    const float rowH = fminf(fmaxf(36 * u, RowMin()), (SH - 224 * u) / 6.0f), gap = 6 * u;
    const float colX = L + fminf(90 * u, SW * 0.06f), colW = fminf(380 * u, SW - 2 * (colX - L));
    // altura total para centrar en vertical si sobra sitio
    const float total = 40 * u + 26 * u + rowH + 34 * u + 30 * u + 2 * rowH + 36 * u + 2 * rowH + 24 * u + rowH;
    float y = T + fmaxf(30 * u, (SH - total) * 0.35f);
    Text(s_fBig, "GAME SETUP", colX, y + 30 * u, 30 * u, kWhite); y += 40 * u;
    Text(s_fSmall, "YOUR NAME", colX + 4 * u, y + 18 * u, 18 * u, kLabel); y += 26 * u;
    {   // campo de nombre
        const bool focus = s_focus == IT_NAME;
        Rect(colX, y, colW, rowH, kBar); Frame(colX, y, colW, rowH, focus ? 2 : 1, focus ? kWhite : kGrey);
        const float px = fminf(rowH * 0.62f, 27 * u); const float tx = colX + 38 * u, ty = y + rowH * 0.5f + px * 0.36f;
        Text(s_fNormal, s_name.c_str(), tx, ty, px, kWhite);
        if (focus && s_editName && fmod(NowMs() - s_t0, 1000.0) < 600) { const float cw = TextW(s_fNormal, s_name.c_str(), px); Rect(tx + cw + 2, y + rowH * 0.2f, 2 * u + 1, rowH * 0.6f, kWhite); }
        AddHit(IT_NAME, colX, y, colW, rowH); y += rowH;
    }
    Text(s_fSmall, "Up to 16 letters, numbers, spaces or . _ ' ! -", colX + 4 * u, y + 24 * u, 16 * u, kHint); y += 34 * u;
    Text(s_fSmall, "MODE", colX + 4 * u, y + 20 * u, 18 * u, kLabel); y += 30 * u;
    OptionRow(IT_SOLO, "SINGLE PLAYER", colX, y, colW, rowH, !s_coop); y += rowH;
    OptionRow(IT_COOP, "MULTIPLAYER", colX, y, colW, rowH, s_coop, CoopAvailable()); y += rowH + gap;
    Text(s_fSmall, "GRAPHICS", colX + 4 * u, y + 24 * u, 18 * u, kLabel); y += 30 * u;
    OptionRow(IT_HIGH, "HIGH", colX, y, colW, rowH, s_quality == 2); y += rowH;
    OptionRow(IT_LOW, "LOW", colX, y, colW, rowH, s_quality == 0); y += rowH + 18 * u;
    {   // boton rojo
        const bool focus = s_focus == IT_CHOOSE;
        Rect(colX, y, colW, rowH, focus ? kRedHi : kRed); if (focus) Frame(colX - 2, y - 1, colW + 4, rowH + 2, 2, kWhite);
        const float px = fminf(rowH * 0.70f, 31 * u);
        Text(s_fNormal, "CHOOSE MAP", colX + 38 * u, y + rowH * 0.5f + px * 0.36f, px, kWhite);
        AddHit(IT_CHOOSE, colX, y, colW, rowH); y += rowH;
    }
    // descripcion del elemento con foco (a la derecha si cabe; si no, debajo del boton)
    const char *title = "", *body = "";
    switch (s_focus) {
    case IT_NAME: title = "YOUR NAME"; body = s_editName ? "Type your name. Enter or click outside to finish." : "Shown to the other players in local co-op."; break;
    case IT_SOLO: title = "SINGLE PLAYER"; body = "Play alone"; break;
    case IT_COOP: title = "MULTIPLAYER"; body = CoopAvailable() ? "Local co-op on your own network: you host the game and others join it. Joining a game from this menu is not available yet (it opens with the client view)."
                                                                 : "Local co-op is not available in this version yet. Play alone for now."; break;
    case IT_HIGH: title = "HIGH"; body = "Full resolution, shadows and post effects (the platform's High profile)."; break;
    case IT_LOW: title = "LOW"; body = "Lower resolution and texture detail, no shadows or post effects. Uses less memory and battery."; break;
    case IT_CHOOSE: title = "CHOOSE MAP"; body = s_coop ? "Pick the mission to host for local co-op." : "Pick a mission."; break;
    default: title = s_coop ? "MULTIPLAYER" : "SINGLE PLAYER"; body = s_coop ? "Local co-op" : "Play alone"; break;
    }
    if (s_quality != 0 && s_quality != 2 && (s_focus == IT_HIGH || s_focus == IT_LOW)) body = "Your settings are custom (changed in Options). Pick HIGH or LOW to use a preset.";
    const float rx0 = colX + colW + 40 * u, rx1 = L + SW - 30 * u;
    float cx, ty, maxW;
    if (rx1 - rx0 >= 260 * u) { cx = (rx0 + rx1) * 0.5f; ty = T + fmaxf(30 * u, (SH - total) * 0.35f) + 70 * u; maxW = rx1 - rx0; }
    else { cx = colX + colW * 0.5f; ty = y + 40 * u; maxW = colW; }
    TextC(s_fBig, title, cx, ty, 28 * u, kWhite);
    float by = ty + 28 * u; for (const std::string &l : Wrap(s_fNormal, body, 22 * u, maxW)) { TextC(s_fNormal, l.c_str(), cx, by, 22 * u, kHint); by += 25 * u; }
}

void DrawMission()
{
    const float u = U(), L = s_safe[0], T = s_safe[1], SW = s_W - s_safe[0] - s_safe[2], SH = s_H - s_safe[1] - s_safe[3];
    // una columna (como el original) si las 10 filas + HELL + BACK caben con el alto tactil minimo; si no (pantalla baja: Fold plegado en
    // horizontal, mitad de la interior) dos columnas de 5, mismas filas y mismo orden (OLA U2)
    const float rmin = RowMin();
    float rowH = fminf(fmaxf(40 * u, rmin), (SH - 160 * u) / 12.6f);
    const bool twoCol = rowH < rmin * 0.92f || getenv("H21_TWOCOL");
    const int rowsPerCol = twoCol ? (kNumMaps + 1) / 2 : kNumMaps;
    if (twoCol) rowH = fminf(fmaxf(40 * u, rmin), (SH - 160 * u) / (rowsPerCol + 2.6f));
    // panel: titulo + filas + HELL MODE; debajo BACK / START
    const float listTop = 98 * u, ph = listTop + rowsPerCol * rowH + 14 * u + rowH + 20 * u;
    const float bottom = rowH + 14 * u;
    const float pw = twoCol ? SW - 24 * u : fminf(SW - 24 * u, fmaxf(ph * 1.55f, 880 * u));
    const float px0 = L + (SW - pw) * 0.5f, py0 = T + fmaxf(8 * u, (SH - ph - bottom) * 0.5f);
    const float shade[4] = { 1, 1, 1, 1 };
    if (s_frame) R_AddCmdDrawStretchPic(px0 - 18 * u, py0 - 12 * u, pw + 36 * u, ph + 24 * u, 0, 0, 1, 1, shade, s_frame);
    else { const float c[4] = { 0.30f, 0.05f, 0.05f, 0.85f }; Rect(px0, py0, pw, ph, c); }
    Text(s_fBig, "MISSION SELECT", px0 + 28 * u, py0 + 66 * u, 26 * u, kWhite);
    // lista
    const float lx = px0 + 26 * u, colGap = 12 * u;
    const float lw = twoCol ? fminf(2 * 300 * u + colGap, pw * 0.56f) : fminf(270 * u, pw * 0.36f);   // ancho total de la lista
    const float cw = twoCol ? (lw - colGap) * 0.5f : lw;                                             // ancho de una columna
    for (int i = 0; i < kNumMaps; ++i) {
        const int id = IT_MAP0 + i; const bool sel = s_map == i, focus = s_focus == id;
        const float x = lx + (twoCol ? (i / rowsPerCol) * (cw + colGap) : 0), y = py0 + listTop + (i % rowsPerCol) * rowH;
        if (sel) Rect(x, y + 1, cw, rowH - 2, kWhite); else Rect(x + 18 * u, y + 1, cw - 18 * u, rowH - 2, kBar);
        if (focus && !sel) Frame(x + 16 * u, y, cw - 16 * u, rowH, 2, kWhite);
        const float tp = FitPx(s_fNormal, kMaps[i].label, fminf(rowH * 0.62f, 25 * u), cw - 34 * u);
        Text(s_fNormal, kMaps[i].label, x + 26 * u, y + rowH * 0.5f + tp * 0.36f, tp, sel ? kBlack : s_mapOk[i] ? kWhite : kGrey);
        AddHit(id, x, y, cw, rowH);
    }
    float y = py0 + listTop + rowsPerCol * rowH + 14 * u;
    {   // HELL MODE: mod de vel.gg, no esta en los datos del juego
        const bool focus = s_focus == IT_HELL; Rect(lx + 18 * u, y + 1, cw - 18 * u, rowH - 2, kBar); if (focus) Frame(lx + 16 * u, y, cw - 16 * u, rowH, 2, kWhite);
        const char *na = "NOT AVAILABLE"; const float np = fminf(rowH * 0.40f, 13 * u);
        const float tp = FitPx(s_fNormal, "HELL MODE: OFF", fminf(rowH * 0.62f, 25 * u), cw - 50 * u - TextW(s_fSmall, na, np));
        Text(s_fNormal, "HELL MODE: OFF", lx + 26 * u, y + rowH * 0.5f + tp * 0.36f, tp, kGrey);
        Text(s_fSmall, na, lx + cw - TextW(s_fSmall, na, np) - 8 * u, y + rowH * 0.5f + np * 0.36f, np, kGrey);
        AddHit(IT_HELL, lx, y, cw, rowH);
    }
    // derecha: nombre, imagen y descripcion del mapa elegido (o la explicacion de HELL MODE con foco)
    const float rx0 = lx + lw + 30 * u, rx1 = px0 + pw - 30 * u, cx = (rx0 + rx1) * 0.5f;
    if (s_focus == IT_HELL) {
        TextC(s_fBig, "HELL MODE", cx, py0 + listTop + 20 * u, 24 * u, kWhite);
        float by = py0 + listTop + 60 * u;
        for (const std::string &l : Wrap(s_fNormal, "Not available. Hell mode is a vel.gg mod (more zombies at once, a later starting round and no perk machines). It is not part of the game's own data, so this port cannot turn it on.", 19 * u, rx1 - rx0)) { TextC(s_fNormal, l.c_str(), cx, by, 19 * u, kHint); by += 22 * u; }
    } else {
        const MapEntry &m = kMaps[s_map];
        TextC(s_fBig, m.label, cx, py0 + listTop + 20 * u, FitPx(s_fBig, m.label, 24 * u, rx1 - rx0), kWhite);
        std::string d = s_mapOk[s_map] ? s_desc[s_map] : std::string("The data for this map is not installed. Add it to the game folder (docs/NUEVO_MAPA.md) to play it.");
        const std::vector<std::string> lines = Wrap(s_fNormal, d, 18 * u, rx1 - rx0);
        const float descH = fmaxf(80 * u, 24 * u + 21 * u * lines.size()), imgTop = py0 + listTop + 34 * u, imgMax = py0 + ph - 24 * u - descH - imgTop;
        const float side = fmaxf(0, fminf(rx1 - rx0, imgMax)), ix = cx - side * 0.5f;
        if (s_mapMat[s_map]) PicCover(s_mapMat[s_map], ix, imgTop, side, side, 1, 1, kWhite);
        else { const float c[4] = { 0.08f, 0.08f, 0.08f, 0.92f }; Rect(ix, imgTop, side, side, c); Frame(ix, imgTop, side, side, 1, kGrey); TextC(s_fBig, m.label, cx, imgTop + side * 0.5f + 10 * u, FitPx(s_fBig, m.label, 26 * u, side * 0.9f), kLabel); }
        AddHit(IT_MAP0 + s_map, ix, imgTop, side, side);   // OLA U2: la imagen tambien es una zona tactil del mapa elegido (toque = empezar)
        float by = imgTop + side + 24 * u;
        for (const std::string &l : lines) { TextC(s_fNormal, l.c_str(), cx, by, 18 * u, kHint); by += 21 * u; }
    }
    {   // BACK, resumen de lo elegido y START (OLA U2: boton grande para el dedo; el doble toque y Enter siguen valiendo)
        const float by = py0 + ph + 14 * u, bw = fmaxf(110 * u, rowH * 1.6f); bool focus = s_focus == IT_BACK;
        Rect(lx, by, bw, rowH, focus ? kWhite : kBar);
        const float tp = fminf(rowH * 0.66f, 27 * u); TextC(s_fNormal, "BACK", lx + bw * 0.5f, by + rowH * 0.5f + tp * 0.36f, tp, focus ? kBlack : kWhite);
        AddHit(IT_BACK, lx, by, bw, rowH);
        const float sw = fmaxf(200 * u, rowH * 2.6f), sx = rx1 - sw; focus = s_focus == IT_START;
        const bool ok = s_mapOk[s_map];
        const float dis[4] = { 0.30f, 0.30f, 0.30f, 0.9f };
        Rect(sx, by, sw, rowH, !ok ? dis : focus ? kRedHi : kRed); if (focus) Frame(sx - 2, by - 1, sw + 4, rowH + 2, 2, kWhite);
        TextC(s_fNormal, "START", sx + sw * 0.5f, by + rowH * 0.5f + tp * 0.36f, tp, ok ? kWhite : kGrey);
        AddHit(IT_START, sx, by, sw, rowH);
        char sum[96]; snprintf(sum, sizeof sum, "%s / %s GRAPHICS", s_coop ? "MULTIPLAYER (LOCAL CO-OP)" : "SINGLE PLAYER", s_quality == 0 ? "LOW" : s_quality == 2 ? "HIGH" : "CUSTOM");
        const float sp = FitPx(s_fSmall, sum, 17 * u, sx - (lx + bw + 16 * u) - 12 * u);
        Text(s_fSmall, sum, lx + bw + 16 * u, by + rowH * 0.5f + sp * 0.36f, sp, kWhite);
    }
}

void Draw()
{
    s_hits.clear();
    float o[4] = {}; if (!(g_h10GetSafeInsets && g_h10GetSafeInsets(s_W, s_H, o))) memset(o, 0, sizeof o);
    memcpy(s_safe, o, sizeof o);
    Rect(0, 0, (float)s_W, (float)s_H, kBlack);
    const float dim[4] = { 0.80f, 0.80f, 0.80f, 1 }, dim2[4] = { 0.55f, 0.55f, 0.55f, 1 };
    if (s_bg) PicCover(s_bg, 0, 0, (float)s_W, (float)s_H, 1, 1, s_scr == SETUP ? dim : dim2);   // fondo a pantalla completa (tambien bajo el recorte)
    if (s_scr == SETUP) DrawSetup(); else DrawMission();
    if (!s_notice.empty() && NowMs() - s_noticeT < 4000) {
        const float u = U(); const float w = TextW(s_fNormal, s_notice.c_str(), 20 * u) + 40 * u, x = (s_W - w) * 0.5f, y = s_safe[1] + 8 * u;   // arriba: no tapa BACK ni la lista
        const float c[4] = { 0, 0, 0, 0.8f }; Rect(x, y, w, 36 * u, c); TextC(s_fNormal, s_notice.c_str(), s_W * 0.5f, y + 25 * u, 20 * u, kWhite);
    }
}

// ---------------------------------------------------------------- acciones
void Notice(const char *s) { s_notice = s; s_noticeT = NowMs(); fprintf(stderr, "[H21] aviso: %s\n", s); }
void SetQuality(int q) { if (q != s_quality) { s_quality = q; fprintf(stderr, "[H21] graficos -> %s\n", q == 2 ? "HIGH" : "LOW"); } }
void Activate(int id)
{
    if (Trace()) fprintf(stderr, "[H21] activa %d\n", id);
    if (id != IT_NAME) s_editName = false;
    switch (id) {
    case IT_NAME: if (!s_editName) s_nameFresh = true; s_editName = true; break;
    case IT_SOLO: s_coop = false; break;
    case IT_COOP: if (!CoopAvailable()) { Notice("Local co-op is not available in this version yet."); break; }
        s_coop = true; Notice("Local co-op: you host, others join. Joining from here opens with the client view."); break;
    case IT_HIGH: SetQuality(2); break;
    case IT_LOW: SetQuality(0); break;
    case IT_CHOOSE: s_scr = MISSION; s_focus = IT_MAP0 + s_map; fprintf(stderr, "[H21] pantalla MISSION SELECT\n"); break;
    case IT_HELL: Notice("Hell mode is not available (vel.gg mod, not in the game data)."); break;
    case IT_BACK: s_scr = SETUP; s_focus = IT_CHOOSE; fprintf(stderr, "[H21] pantalla GAME SETUP\n"); break;
    case IT_START: if (!s_mapOk[s_map]) { Notice("This map's data is not installed."); break; } s_launch = true; s_launchMap = s_map; break;
    default:
        if (id >= IT_MAP0 && id < IT_MAP0 + kNumMaps) {
            const int i = id - IT_MAP0; s_map = i;
            if (!s_mapOk[i]) { Notice("This map's data is not installed."); break; }
            s_launch = true; s_launchMap = i;
        }
        break;
    }
}
void Focus(int id)
{
    if (id == s_focus) return;
    s_focus = id; if (id != IT_NAME) s_editName = false;
    if (id >= IT_MAP0 && id < IT_MAP0 + kNumMaps) s_map = id - IT_MAP0;   // en la lista el foco elige (y muestra la imagen), como el original
}
const int kSetupOrder[] = { IT_NAME, IT_SOLO, IT_COOP, IT_HIGH, IT_LOW, IT_CHOOSE };
void Move(int d)
{
    std::vector<int> v;
    if (s_scr == SETUP) v.assign(kSetupOrder, kSetupOrder + 6); else { for (int i = 0; i < kNumMaps; ++i) v.push_back(IT_MAP0 + i); v.push_back(IT_HELL); v.push_back(IT_BACK); v.push_back(IT_START); }
    int k = 0; for (size_t i = 0; i < v.size(); ++i) if (v[i] == s_focus) k = (int)i;
    k = (k + d + (int)v.size()) % (int)v.size(); Focus(v[k]);
}
void Key(int key)
{
    if (s_editName) {
        if (key == K_BACKSPACE || key == K_DEL) { s_nameFresh = false; if (!s_name.empty()) s_name.pop_back(); return; }
        if (key == K_ENTER || key == K_ESCAPE || key == K_TAB) { s_editName = false; if (s_name.empty()) s_name = "Player"; if (key != K_TAB) return; }
        else if (key >= 32 && key < 127) return;   // los caracteres llegan por TypeChar (con mayusculas)
    }
    switch (key) {
    case K_UPARROW: Move(-1); break;
    case K_DOWNARROW: case K_TAB: Move(1); break;
    case K_LEFTARROW: case K_RIGHTARROW:
        if (s_scr == SETUP) { if (s_focus == IT_SOLO || s_focus == IT_COOP) { Focus(s_focus == IT_SOLO ? IT_COOP : IT_SOLO); Activate(s_focus); }
            else if (s_focus == IT_HIGH || s_focus == IT_LOW) { Focus(s_focus == IT_HIGH ? IT_LOW : IT_HIGH); Activate(s_focus); } }
        break;
    case K_ENTER: Activate(s_focus); break;
    case K_ESCAPE: if (s_scr == MISSION) Activate(IT_BACK); break;
    default: break;
    }
}
void TypeChar(int c) { if (!s_editName || !NameChar(c)) return; if (s_nameFresh) { s_name.clear(); s_nameFresh = false; } if (s_name.size() < 16) s_name += (char)c; }
int HitAt(float px, float py) { for (const Hit &h : s_hits) if (px >= h.x && px < h.x + h.w && py >= h.y && py < h.y + h.h) return h.id; return IT_NONE; }
// tolerancia del dedo (~3 mm): si no cae en ninguna zona, la mas cercana a menos de esa distancia
int HitNear(float px, float py)
{
    const int id = HitAt(px, py); if (id != IT_NONE) return id;
    const float slop = g_h10Dpi > 0 ? 0.12f * g_h10Dpi : 0.0f; int best = IT_NONE; float bd = slop;
    for (const Hit &h : s_hits) { const float dx = px < h.x ? h.x - px : px > h.x + h.w ? px - h.x - h.w : 0, dy = py < h.y ? h.y - py : py > h.y + h.h ? py - h.y - h.h : 0;
        const float d = hypotf(dx, dy); if (d < bd) { bd = d; best = h.id; } }
    return best;
}
// puntero: raton (hover + clic) y dedo (down/up). Un toque en un mapa que no estaba elegido solo lo elige (muestra la imagen);
// el segundo toque (o el clic con el raton, que ya lo eligio al pasar por encima) lo arranca.
int s_downId = IT_NONE; bool s_downWasFocus;
void PointerMove(float px, float py) { const int id = HitAt(px, py); if (id != IT_NONE) Focus(id); }
void PointerDown(float px, float py) { s_downId = HitNear(px, py); s_downWasFocus = s_downId == s_focus; if (s_downId != IT_NONE) Focus(s_downId); else s_editName = false; }
void PointerUp(float px, float py)
{
    const int id = HitNear(px, py);
    if (id != IT_NONE && id == s_downId) { if (id >= IT_MAP0 && id < IT_MAP0 + kNumMaps && !s_downWasFocus) { /* primer toque: solo elige */ } else Activate(id); }
    s_downId = IT_NONE;
}

// ---------------------------------------------------------------- entrada de la plataforma
#ifdef H0_MACWIN
extern "C" void H10_MacInputInstall(void (*ev)(int, float, float, int));
// h10_macin.mm: kind 0 mover 1 boton abajo 2 boton arriba 3 rueda 4 tecla abajo 5 tecla arriba; fx,fy en fraccion de la ventana
void MacEv(int kind, float fx, float fy, int key)
{
    const float px = fx * s_W, py = fy * s_H;
    if (Trace() && kind != 0 && kind != 5) fprintf(stderr, "[H21] evento %d (%.3f, %.3f) tecla %d\n", kind, fx, fy, key);
    switch (kind) {
    case 0: PointerMove(px, py); break;
    case 1: PointerDown(px, py); break;
    case 2: PointerUp(px, py); break;
    case 3: Move(key > 0 ? -1 : 1); break;
    case 4: Key(key); break;
    case 6: TypeChar(key); break;
    default: break;
    }
}
#endif

// ---------------------------------------------------------------- Atras (Android / mando)
// OLA U2: Atras del sistema o B: termina de editar el nombre, vuelve de MISSION SELECT; en GAME SETUP devuelve false (la app sale)
bool Back()
{
    if (s_editName) { Key(K_ENTER); return true; }
    if (s_scr == MISSION) { Activate(IT_BACK); return true; }
    return false;
}

// ---------------------------------------------------------------- piloto (pruebas): "t accion;..." con t en segundos de frames/60
struct PilotEv { double t; std::string a; };
std::vector<PilotEv> s_pilot; size_t s_pilotI;
bool s_quit;
void PilotStep(int frame)
{
    const double t = frame / 60.0;
    // H21_SHOTREQ=fichero: pruebas con eventos reales (raton/teclado del sistema); si el fichero existe, captura con su contenido como etiqueta
    if (const char *rq = getenv("H21_SHOTREQ")) if (frame % 10 == 0) if (FILE *f = fopen(rq, "r")) {
        char w[64] = ""; if (fscanf(f, "%63s", w) != 1) strcpy(w, "req"); fclose(f); unlink(rq);
        fprintf(stderr, "[H21] captura pedida: %s (foco %d, pantalla %s)\n", w, s_focus, s_scr == SETUP ? "GAME SETUP" : "MISSION SELECT");
        H4_Shot((std::string("_h21_") + w).c_str());
    }
    while (s_pilotI < s_pilot.size() && s_pilot[s_pilotI].t <= t) {
        const std::string &a = s_pilot[s_pilotI++].a; float x = 0, y = 0, dx2 = 0, dy2 = 0; int k = 0, k2 = 0; char w[64] = "";
        fprintf(stderr, "[H21] piloto t=%.2f: %s\n", t, a.c_str());
        if (sscanf(a.c_str(), "move %f %f", &x, &y) == 2) PointerMove(x * s_W, y * s_H);
        else if (sscanf(a.c_str(), "down %f %f", &x, &y) == 2) PointerDown(x * s_W, y * s_H);
        else if (sscanf(a.c_str(), "up %f %f", &x, &y) == 2) PointerUp(x * s_W, y * s_H);
        else if (sscanf(a.c_str(), "tap %f %f", &x, &y) == 2) { PointerDown(x * s_W, y * s_H); PointerUp(x * s_W, y * s_H); }
        else if (sscanf(a.c_str(), "key %d", &k) == 1) Key(k);
        else if (!strncmp(a.c_str(), "type ", 5)) { for (const char *c = a.c_str() + 5; *c; ++c) TypeChar((unsigned char)*c); }
        else if (sscanf(a.c_str(), "shot %63s", w) == 1) { std::string tag = std::string("_h21_") + w; H4_Shot(tag.c_str()); }
        else if (sscanf(a.c_str(), "tapid %d", &k) == 1) { for (const Hit &h : s_hits) if (h.id == k) { const float cx = h.x + h.w * 0.5f, cy = h.y + h.h * 0.5f; fprintf(stderr, "[H21] toque en %d (%.0f,%.0f) fila %.0f px\n", k, cx, cy, h.h); PointerDown(cx, cy); PointerUp(cx, cy); break; } }   // toque (dedo) en el centro de una zona
        else if (a == "back") { const bool r = Back(); fprintf(stderr, "[H21] atras -> %s\n", r ? "consumido" : "salir de la app"); }
        else if (sscanf(a.c_str(), "drag %f %f %f %f", &x, &y, &dx2, &dy2) == 4) {   // arrastre del dedo en 8 pasos (abajo, mover, arriba)
            PointerDown(x * s_W, y * s_H); for (int k = 1; k <= 8; ++k) PointerMove((x + (dx2 - x) * k / 8) * s_W, (y + (dy2 - y) * k / 8) * s_H); PointerUp(dx2 * s_W, dy2 * s_H); }
        else if (sscanf(a.c_str(), "dragid %d %d", &k, &k2) == 2) {   // arrastre del dedo del centro de una zona al de otra (ids de Item)
            const Hit *ha = nullptr, *hb = nullptr; for (const Hit &h : s_hits) { if (h.id == k && !ha) ha = &h; if (h.id == k2 && !hb) hb = &h; }
            if (ha && hb) { const Hit A = *ha, B = *hb; const float ax = A.x + A.w * 0.5f, ay = A.y + A.h * 0.5f, bx = B.x + B.w * 0.5f, by = B.y + B.h * 0.5f;
                fprintf(stderr, "[H21] arrastre %d (%.0f,%.0f) -> %d (%.0f,%.0f)\n", k, ax, ay, k2, bx, by);
                PointerDown(ax, ay); for (int j = 1; j <= 8; ++j) { PointerMove(ax + (bx - ax) * j / 8, ay + (by - ay) * j / 8); fprintf(stderr, "[H21]   paso %d foco %d\n", j, s_focus); } PointerUp(bx, by); } }
        else if (a == "quit") s_quit = true;
    }
}

// ---------------------------------------------------------------- lanzar el mapa elegido (relanza el proceso como h18/h11)
void Launch(int i)
{
    const MapEntry &m = kMaps[i];
    SavePrefs();
    const std::string pd = PlayersDir();
    if (FILE *f = fopen((pd + "/bo1_map.txt").c_str(), "w")) { fprintf(f, "%s\n", m.zone); fclose(f); }   // OLA M0: el siguiente arranque/Restart usa este mapa
    if (s_coop) { if (FILE *f = fopen((pd + "/bo1_net.txt").c_str(), "w")) { fprintf(f, "host 4 %s\n", m.zone); fclose(f); } }   // OLA C1: sala de anfitrion
    if (s_quality != s_quality0 && (s_quality == 0 || s_quality == 2)) H10_CfgSetQuality(s_quality);
    fprintf(stderr, "[H21] EMPIEZA %s (%s), modo %s, nombre '%s', graficos %s\n", m.zone, m.label, s_coop ? "co-op local (anfitrion)" : "solo", s_name.c_str(), s_quality == 0 ? "LOW" : s_quality == 2 ? "HIGH" : "CUSTOM");
    fflush(stderr);
    if (getenv("H21_NOLAUNCH") && atoi(getenv("H21_NOLAUNCH"))) { s_quit = true; return; }
    unsetenv("H21_MENU"); setenv("H11_NAME", s_name.c_str(), 1);
    setenv("H5_MAP", m.zone, 1);   // co-op: h11_lobby.cpp pone el mismo mapa desde bo1_net.txt
    const char *b = H18_BaseDir(); if (b && *b) setenv("P3_GAMEDATA", b, 1);
    unsetenv("H10_CFG"); unsetenv("V9_TEXPKG");
    if (const char *nx = getenv("H21_PILOT_NEXT")) { setenv("H10_PILOT", nx, 1); unsetenv("H21_PILOT_NEXT"); }
    if (g_h10RestartApp) {   // Android: la app se reinicia y lee bo1_map.txt / bo1_net.txt
        // OLA U2: un solo uso; la app lo borra al arrancar y entra directa al mapa (sin el, arranca en este menu)
        if (FILE *f = fopen((pd + "/bo1_launch.txt").c_str(), "w")) { fprintf(f, "%s\n", m.zone); fclose(f); }
        else fprintf(stderr, "[H21] no se pudo escribir %s/bo1_launch.txt\n", pd.c_str());
        g_h10RestartApp(); return;
    }
#ifndef BO1_APP
    char exe[4096] = ""; uint32_t n = sizeof exe;
#ifdef __APPLE__
    if (_NSGetExecutablePath(exe, &n) != 0) exe[0] = 0;
    char **argv = *_NSGetArgv();
#else
    { ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1); if (r > 0) exe[r] = 0; }
    char *argv[] = { exe, nullptr };
#endif
    if (!exe[0]) { fprintf(stderr, "[H21] no encuentro el ejecutable\n"); s_quit = true; return; }
    execv(exe, argv);
    fprintf(stderr, "[H21] execv fallo\n"); s_quit = true;
#endif
}

void Init(int W, int H)
{
    s_W = W; s_H = H; s_t0 = NowMs();
    s_white = Material_RegisterHandle((char *)"white", 3); s_tmpl = s_white;
    s_fBig = R_RegisterFont("fonts/bigFont", 3); s_fNormal = R_RegisterFont("fonts/normalFont", 3); s_fSmall = R_RegisterFont("fonts/smallFont", 3);
    if (const char *f = getenv("H21_FONT_BIG")) s_fBig = R_RegisterFont(f, 3);
    if (const char *f = getenv("H21_FONT_NORMAL")) s_fNormal = R_RegisterFont(f, 3);
    if (const char *f = getenv("H21_FONT_SMALL")) s_fSmall = R_RegisterFont(f, 3);
    if (Trace()) DB_EnumXAssets_FastFile(ASSET_TYPE_FONT, [](XAssetHeader h, void *) { Font_s *f = h.font.get(); if (f && f->fontName) fprintf(stderr, "[H21] fuente %s (%d px)\n", f->fontName.get(), f->pixelHeight); }, nullptr, false);
    if (const char *p = getenv("H21_LOC")) LocDump(p);
    const double t0 = NowMs();
    s_bg = LoadMat("menu_background_press_start");
    s_frame = LoadMat("menu_zombie_lobby_frame_outer_ingame");
    int imgs = 0;
    for (int i = 0; i < kNumMaps; ++i) {
        // imagen de carga original: loadscreen_zombie_<clave> (los de WaW van sin "cod5_": loadscreen_zombie_factory, ..._sumpf)
        char n[64]; snprintf(n, sizeof n, "loadscreen_zombie_%s", kMaps[i].token); for (char *c = n; *c; ++c) *c = (char)tolower((unsigned char)*c); s_mapMat[i] = LoadMat(n); imgs += s_mapMat[i] != nullptr;
        s_mapOk[i] = H18_MapPlayable(kMaps[i].zone);
    }
    LoadDescriptions();
    for (int i = 0; i < kNumMaps; ++i) {
        const bool own = !s_desc[i].empty(); if (!own) s_desc[i] = kMaps[i].fallback;
        char n[64]; snprintf(n, sizeof n, "loadscreen_zombie_%s", kMaps[i].token); for (char *c = n; *c; ++c) *c = (char)tolower((unsigned char)*c);
        fprintf(stderr, "[H21] %-22s imagen %s, datos %s, descripcion %s\n", kMaps[i].zone, s_mapMat[i] ? n : "NO (fondo con el nombre)", s_mapOk[i] ? "jugables" : "NO instalados",
            own ? "de las zonas" : "propia (MENU_ZOMBIE_*_DESC no cargada)");
        if (Trace() && own) fprintf(stderr, "[H21]   \"%s\"\n", s_desc[i].c_str());
    }
    fprintf(stderr, "[H21] menu listo: fondo %s, marco %s, %d/%d imagenes de mapa, %.0f ms; fuentes %s/%s/%s\n", s_bg ? "si" : "no", s_frame ? "si" : "no", imgs, kNumMaps, NowMs() - t0,
        s_fBig && s_fBig->fontName ? s_fBig->fontName.get() : "-", s_fNormal && s_fNormal->fontName ? s_fNormal->fontName.get() : "-", s_fSmall && s_fSmall->fontName ? s_fSmall->fontName.get() : "-");
    LoadPrefs();
    s_quality0 = s_quality = H10_CfgQuality();
    if (const char *p = getenv("H21_PILOT")) {
        while (p && *p) { const char *e = strchr(p, ';'); std::string s(p, e ? e - p : strlen(p)); double t; int n = 0; if (sscanf(s.c_str(), "%lf %n", &t, &n) == 1) s_pilot.push_back({ t, s.substr(n) }); p = e ? e + 1 : nullptr; }
    }
#ifdef H0_MACWIN
    if (getenv("P4_WINDOW")) H10_MacInputInstall(MacEv);
#endif
}

int H21_Run(int W, int H)
{
    if (!getenv("H21_MENU") || !atoi(getenv("H21_MENU"))) return -1;
    fprintf(stderr, "[H21] OLA U1: menu principal (%dx%d)\n", W, H);
    Init(W, H);
    g_h21Running = true;
    for (int frame = 1; !s_quit; ++frame) {
        if (g_h21PollEvents) g_h21PollEvents();   // OLA U2: la app Android bombea sus eventos (toques, teclas, pausa) aqui
        PilotStep(frame);
        rgp.needSortMaterials = 0;   // como world_h4 (H4_Frame): la lista 2D no usa el orden de materiales
        R_BeginFrame(); R_BeginSharedCmdList(); R_AddCmdProjectionSet2D();
        Draw();
        R_EndFrame(); R_IssueRenderCommands(3u);   // presenta y atiende la ventana (WinPump en el Present del dispositivo)
        if (s_launch) { s_launch = false; Launch(s_launchMap); }
    }
    g_h21Running = false;
    fprintf(stderr, "[H21] fin del menu\n");
    return 0;
}
}  // namespace

// Android (la app lo enlazaria como g_h10TouchFn): toque en fraccion de la pantalla; phase 0 abajo 1 mover 2 arriba
void H21_Touch(int phase, float fx, float fy) { const float px = fx * s_W, py = fy * s_H; if (phase == 0) PointerDown(px, py); else if (phase == 1) PointerMove(px, py); else PointerUp(px, py); }
void H21_Key(int key) { Key(key); }
void H21_Char(int c) { TypeChar(c); }   // Android: teclado en pantalla para el nombre
bool H21_Back() { return Back(); }      // OLA U2: Atras del sistema; false = salir de la app
bool H21_EditingName() { return s_editName; }   // OLA U2: la app muestra/oculta el teclado en pantalla
// OLA U2b: el teclado del sistema escribe en un campo de texto de Android; la app le da el nombre al empezar (H21_Name) y le devuelve el
// contenido entero del campo cada vez que cambia (H21_SetName: mismos caracteres validos y tope de 16 que al teclear; vacio se queda vacio
// hasta terminar, que vuelve a "Player" como con la tecla).
const char *H21_Name() { return s_name.c_str(); }
int H21_NameMax() { return 16; }
void H21_SetName(const char *t) { if (!s_editName || !t) return; std::string n; for (; *t && n.size() < 16; ++t) if (NameChar((unsigned char)*t)) n += *t; s_name = n; s_nameFresh = false; }
int (*g_h21Frontend)(int W, int H) = H21_Run;   // main2.cpp (puntero de datos: a cero si esta TU no esta)
