// H5a0: la VM GSC ORIGINAL (src/clientscript) en arm64/LP64, probada aislada en el harness.
//   H5_VM=1          -> compila y ejecuta un GSC de prueba propio (hilos, wait/notify/endon, arrays, structs, strings)
//   H5_VM=2          -> ademas, solo-compilacion de los maps/_zombiemode*.gsc reales (RawFiles zlib de las zonas, en memoria)
//   H5_VM=3          -> igual que 2 pero con TODOS los GSC de las zonas cargadas
//   H5_FILES=a,b     -> lista explicita de GSC reales para la pasada de solo-compilacion
// Aqui vive tambien el pegamento minimo que la VM necesita del resto del motor: hunk de usuario y memoria temporal en la
// arena Ptr32 (lo que se guarda como codepos de 32 bits tiene que estar en ella) y la busqueda de builtins.
#ifdef H5G
#define H5W __attribute__((weak)) // H5G: el servidor ORIGINAL se enlaza entero y su TU real gana a este pegamento
#else
#define H5W
#endif

#include <universal/q_shared.h>
#include <universal/com_memory.h>
#include <universal/mem_userhunk.h>
#include <universal/com_files.h>
#include <universal/ptr32.h>
#include <qcommon/common.h>
#include <clientscript/cscr_main.h>
#include <clientscript/cscr_vm.h>
#include <clientscript/cscr_variable.h>
#include <clientscript/cscr_stringlist.h>
#include <clientscript/cscr_parser.h>
#include <clientscript/cscr_compiler.h>
#include <database/db_registry.h>
#include <stdio.h>
#include <zlib.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------------------------------------------
// Hunk de usuario (universal/mem_userhunk.cpp guarda pos/end como int de 32 bits: aqui una version del harness con
// la MISMA semantica: HF_FIXEDSIZE (4) = un solo bloque contiguo y Com_Error al llenarse; sin el, se encadenan bloques).
struct H5Hunk
{
    HunkUser hunkUser;      // primero: el motor solo mira scheme/flags/name/type
    char *buf;
    char *pos;
    char *end;
    H5Hunk *current;
    H5Hunk *next;
    int maxSize;
    bool extBuf;            // buffer del llamante (Hunk_UserCreateFromBuffer)
};
HunkUser *Hunk_UserCreate(int size, HU_ALLOCATION_SCHEME scheme, unsigned int flags, void *, const char *name, int type)
{
    H5Hunk *u = (H5Hunk *)Ptr32_ArenaAlloc(sizeof(H5Hunk), 64);
    memset(u, 0, sizeof *u);
    u->hunkUser.scheme = scheme; u->hunkUser.flags = flags; u->hunkUser.name = name; u->hunkUser.type = type;
    u->buf = (char *)Ptr32_ArenaAlloc(size, 4096);
    memset(u->buf, 0, size);
    u->pos = u->buf; u->end = u->buf + size; u->current = u; u->maxSize = size;
    return &u->hunkUser;
}
// mem_userhunk.cpp:483: el hunk vive dentro del buffer del llamante (CL_AllocatePerLocalClientMemory); aqui la cabecera va
// aparte en la arena y el buffer entero queda para asignaciones. Siempre bloque fijo (no se encadena memoria fuera del buffer).
HunkUser *Hunk_UserCreateFromBuffer(void *buffer, int size, HU_ALLOCATION_SCHEME scheme, unsigned int flags, void *, const char *name, int type)
{
    H5Hunk *u = (H5Hunk *)Ptr32_ArenaAlloc(sizeof(H5Hunk), 64);
    memset(u, 0, sizeof *u);
    u->hunkUser.scheme = scheme; u->hunkUser.flags = flags | 4; u->hunkUser.name = name; u->hunkUser.type = type;
    u->buf = (char *)buffer; u->pos = u->buf; u->end = u->buf + size; u->current = u; u->maxSize = size; u->extBuf = true;
    return &u->hunkUser;
}
void *Hunk_UserAlloc(HunkUser *user, int size, int alignment, const char *)
{
    H5Hunk *u = (H5Hunk *)user;
    if (alignment < 1) alignment = 1;
    for (H5Hunk *c = u->current;;)
    {
        char *p = (char *)(((uintptr_t)c->pos + alignment - 1) & ~(uintptr_t)(alignment - 1));
        if (p + size <= c->end) { c->pos = p + size; return p; }
        if (u->hunkUser.flags & 4) Com_Error(ERR_FATAL, "Hunk_UserAlloc: out of memory");
        H5Hunk *n = (H5Hunk *)Hunk_UserCreate(std::max(u->maxSize, size + alignment), u->hunkUser.scheme, u->hunkUser.flags, 0, u->hunkUser.name, u->hunkUser.type);
        c->next = n; u->current = c = n;
    }
}
void Hunk_UserFree(HunkUser *, void *) {}
void Hunk_UserReset(HunkUser *user) { H5Hunk *u = (H5Hunk *)user; u->pos = u->buf; u->current = u; u->next = 0; }
void Hunk_UserDestroy(HunkUser *user)
{
    // los de buffer ajeno (Hunk_UserCreateFromBuffer) no liberan su buffer: es del llamante
    for (H5Hunk *u = (H5Hunk *)user; u;) { H5Hunk *n = u->next; if (!u->extBuf) Ptr32_ArenaFree(u->buf); Ptr32_ArenaFree(u); u = n; }
}
void Hunk_UserSetPos(HunkUser *user, unsigned char *pos)
{
    H5Hunk *u = (H5Hunk *)user;
    if (!(u->hunkUser.flags & 4) || (char *)pos < u->buf || (char *)pos > u->pos) Com_Error(ERR_FATAL, "Hunk_UserSetPos fuera de rango");
    u->pos = (char *)pos;
}
HunkUser *g_DebugHunkUser;   // Hunk_UserStartup (mem_userhunk.cpp): tablas de depuracion de la VM en modo developer
char *Hunk_CopyString(HunkUser *user, const char *in) { size_t n = strlen(in) + 1; char *o = (char *)Hunk_UserAlloc(user, (int)n, 1, 0); memcpy(o, in, n); return o; }

// Memoria temporal del hunk (alta y baja): dos pilas dentro de la arena. El compilador guarda punteros a ella como
// sval_u de 32 bits (bloques, nodos de case/break) y el parser los buffers de fuente.
static char *g_tmpHigh, *g_tmpHighPos, *g_tmpHighEnd;
static const size_t H5_TEMP_HIGH = 64u << 20;
char *Hunk_AllocateTempMemoryHigh(int size, const char *)
{
    if (!g_tmpHigh) { g_tmpHigh = g_tmpHighPos = (char *)Ptr32_ArenaAlloc(H5_TEMP_HIGH, 4096); g_tmpHighEnd = g_tmpHigh + H5_TEMP_HIGH; }
    char *p = (char *)(((uintptr_t)g_tmpHighPos + 15) & ~(uintptr_t)15);
    if (p + size > g_tmpHighEnd) Com_Error(ERR_FATAL, "Hunk_AllocateTempMemoryHigh: sin memoria (%d)", size);
    g_tmpHighPos = p + size;
    return p;
}
void Hunk_ClearTempMemoryHigh() { g_tmpHighPos = g_tmpHigh; }
char Hunk_CheckTempMemoryHighClear() { if (g_tmpHighPos != g_tmpHigh) Com_Error(ERR_FATAL, "Hunk_CheckTempMemoryHighClear: memoria temporal alta sin liberar"); return 1; }
char Hunk_CheckTempMemoryClear() { return 1; }

// ---------------------------------------------------------------------------------------------------------------
// FS: las mods del motor (fs_mods) no existen en el harness. FS_ModsActive solo se enciende para leer el GSC de prueba
// (fichero en memoria por FS_FOpenFileRead del shim de iwd.cpp); los GSC reales salen siempre del fastfile.
static bool g_h5Local = false;
bool FS_ModsActive() { return g_h5Local; }
bool FS_ModStackActive() { return false; }
int FS_ModStackNames(char (*)[64], int) { return 0; }
unsigned int FS_FOpenFileByMode(char *qpath, int *f, fsMode_t) { return (unsigned)FS_FOpenFileRead(qpath, f); }
void FS_H0_AddMemFile(const char *name, const char *data, size_t len);

// Ganchos de juego (game_sp) que Scr_EndLoadScripts llama: indices de campos para pruebas headless / depuracion SP.
// Sin juego cargado no hay nada que medir: no-op.
H5W void Scr_SP_BuildCodePosMap(scriptInstance_t) {}
H5W void G_SP_MeasureFields(scriptInstance_t) {}
H5W void GScr_ModScriptPerfFields(scriptInstance_t) {}
H5W void G_SP_HeadlessRoundFields(scriptInstance_t) {}
// mismos ganchos en notify / errores de ejecucion (medidas y depuracion del juego SP): sin juego, nada que hacer
H5W void G_SP_MeasureNotify(scriptInstance_t, unsigned int, unsigned int, const VariableValue *) {}
H5W void G_SP_HeadlessRoundNotify(scriptInstance_t, unsigned int, unsigned int) {}
void Monkey_GrabComPrints(bool) {}
void Monkey_Error(const char *) {}
bool Sys_IsRemoteDebugServer() { return false; }   // sin depurador remoto de scripts   // monkey (automatizacion de pruebas por red) no corre en el harness
void Com_PrintMessage(int, char *msg, int error) { fprintf(stderr, "%s%s", error ? "[ERROR] " : "", msg); }
H5W bool Scr_SP_ReportRuntimeError(scriptInstance_t, const char *, const char *) { return false; }

// ---------------------------------------------------------------------------------------------------------------
// Builtins. Prueba propia: unos pocos reales del harness. Solo-compilacion: los NOMBRES de las tablas reales del motor
// (h5_builtins.inc, generado por h5_genbuiltins.py); al ejecutarse uno de esos sin portar se lanza Scr_Error.
static int g_h5Pass, g_h5Fail, g_h5Done;
static void H5_Print() { for (unsigned i = 0, n = Scr_GetNumParam(SCRIPTINSTANCE_SERVER); i < n; ++i) fprintf(stderr, "%s", Scr_GetDebugString(i, SCRIPTINSTANCE_SERVER)); }
static void H5_Println() { fprintf(stderr, "[H5 gsc] "); H5_Print(); fputc('\n', stderr); }
static void H5_Check()
{
    int ok = Scr_GetInt(0, SCRIPTINSTANCE_SERVER);
    const char *what = Scr_GetString(1, SCRIPTINSTANCE_SERVER);
    fprintf(stderr, "[H5] %-28s %s\n", what, ok ? "OK" : "FALLA");
    ++(ok ? g_h5Pass : g_h5Fail);
}
static void H5_Done() { g_h5Done = 1; }
static void H5_GetTime() { Scr_AddInt(gScrVarPub[SCRIPTINSTANCE_SERVER].time * 50, SCRIPTINSTANCE_SERVER); }
// en x86 los builtins se llaman sin argumentos y el valor de inst no importa; en arm64 x0 seria basura: envoltorio
static void H5_SpawnStruct() { Scr_AddStruct(SCRIPTINSTANCE_SERVER); }
static void H5_IsDefined() { Scr_AddInt(Scr_GetType(0, SCRIPTINSTANCE_SERVER) != 0, SCRIPTINSTANCE_SERVER); }
static void H5_Unported() { Scr_Error(SCRIPTINSTANCE_SERVER, "H5: builtin sin portar (solo-compilacion)", false); }
static void H5_UnportedM(scr_entref_t) { H5_Unported(); }

struct H5Fn { const char *name; void (*fn)(); int type; };
static const H5Fn s_h5Funcs[] = {
    { "print", H5_Print, 0 }, { "println", H5_Println, 0 }, { "h5_check", H5_Check, 0 }, { "h5_done", H5_Done, 0 },
    { "gettime", H5_GetTime, 0 }, { "spawnstruct", H5_SpawnStruct, 0 }, { "isdefined", H5_IsDefined, 0 },
};
struct H5Name { const char *name; int type; };
#define H5_F(n, t) { n, t },
#define H5_M(n, t)
static const H5Name s_engineFuncs[] = {
#include "h5_builtins.inc"
};
#undef H5_F
#undef H5_M
#define H5_F(n, t)
#define H5_M(n, t) { n, t },
static const H5Name s_engineMeths[] = {
#include "h5_builtins.inc"
};
#undef H5_F
#undef H5_M

H5W void (*Scr_GetFunction(const char **pName, int *type))()
{
    for (const H5Fn &f : s_h5Funcs) if (!strcmp(*pName, f.name)) { *pName = f.name; *type = f.type; return f.fn; }
    for (const H5Name &f : s_engineFuncs) if (!strcmp(*pName, f.name)) { *pName = f.name; *type = f.type; return H5_Unported; }
    return 0;
}
H5W void (*Scr_GetMethod(const char **pName, int *type))(scr_entref_t)
{
    for (const H5Name &f : s_engineMeths) if (!strcmp(*pName, f.name)) { *pName = f.name; *type = f.type; return H5_UnportedM; }
    return 0;
}
void (*CScr_GetFunction(const char **, int *))() { return 0; }
void (*CScr_GetMethod(const char **, int *))(scr_entref_t) { return 0; }

// ---------------------------------------------------------------------------------------------------------------
static const char s_testGsc[] =
    "main()\n"
    "{\n"
    "    println(\"hola desde GSC, tiempo \" + gettime());\n"
    "    h5_check(1 + 2 * 3 == 7, \"aritmetica int\");\n"
    "    h5_check(0.5 + 0.25 == 0.75, \"aritmetica float\");\n"
    "    s = \"ab\" + 3 + \"cd\";\n"
    "    h5_check(s == \"ab3cd\", \"concatenar strings\");\n"
    "    h5_check(\"zombi\" != \"zombie\", \"comparar strings\");\n"
    "    st = spawnstruct();\n"
    "    st.vida = 150; st.nombre = \"ray gun\"; st.sub = spawnstruct(); st.sub.x = 7;\n"
    "    h5_check(st.vida == 150 && st.nombre == \"ray gun\" && st.sub.x == 7, \"struct anidado\");\n"
    "    a = [];\n"
    "    for (i = 0; i < 10; i++) a[a.size] = i * i;\n"
    "    h5_check(a.size == 10 && a[9] == 81, \"array indexado\");\n"
    "    m = []; m[\"x\"] = 1; m[\"y\"] = 2;\n"
    "    h5_check(m.size == 2 && m[\"x\"] + m[\"y\"] == 3, \"array por clave\");\n"
    "    b = a; b[0] = 99;\n"
    "    h5_check(a[0] == 0 && b[0] == 99, \"copia de array\");\n"
    "    v = (1, 2, 3);\n"
    "    h5_check(v[1] == 2 && v + (1, 1, 1) == (2, 3, 4), \"vectores\");\n"
    "    h5_check(fact(6) == 720, \"recursion\");\n"
    "    h5_check(sw(2) == \"dos\" && sw(5) == \"otro\", \"switch\");\n"
    "    level.cuenta = 0; level.quien = \"\";\n"
    "    level thread worker(\"a\");\n"
    "    level thread worker(\"b\");\n"
    "    level thread esperaparam();\n"
    "    level thread ticker();\n"
    "    wait 0.1;\n"
    "    h5_check(level.cuenta == 0, \"waittill antes de notify\");\n"
    "    level notify(\"go\");\n"
    "    wait 0.05;\n"
    "    h5_check(level.cuenta == 2, \"notify despierta 2 hilos\");\n"
    "    level notify(\"param\", \"zombi\", 115);\n"
    "    waittillframeend;\n"
    "    h5_check(level.quien == \"zombi115\", \"notify con parametros\");\n"
    "    wait 0.2;\n"
    "    t0 = level.ticks;\n"
    "    h5_check(t0 >= 3, \"wait en bucle\");\n"
    "    level notify(\"stop\");\n"
    "    wait 0.2;\n"
    "    h5_check(level.ticks == t0, \"endon mata el hilo\");\n"
    "    st thread hijo();\n"
    "    wait 0.2;\n"
    "    st notify(\"death\");\n"
    "    t1 = st.hijo_ticks;\n"
    "    wait 0.2;\n"
    "    h5_check(t1 >= 1 && st.hijo_ticks == t1, \"endon en struct\");\n"
    "    h5_check(isdefined(st.vida) && !isdefined(st.nada), \"isdefined\");\n"
    "    level thread errhilo();\n"
    "    wait 0.05;\n"
    "    h5_check(isdefined(level.antes_error) && level.antes_error == 1, \"hilo con error arranca\");\n"
    "    h5_check(isdefined(level.tras_error) && !level.b_definida, \"error no terminal: sigue (como el juego)\");\n"
    "    println(\"fin, tiempo \" + gettime());\n"
    "    h5_done();\n"
    "}\n"
    "fact(n) { if (n <= 1) return 1; return n * fact(n - 1); }\n"
    "sw(n) { switch (n) { case 1: return \"uno\"; case 2: return \"dos\"; default: return \"otro\"; } }\n"
    "worker(nombre) { self waittill(\"go\"); self.cuenta++; }\n"
    "esperaparam() { self waittill(\"param\", a, b); self.quien = a + b; }\n"
    "ticker() { self endon(\"stop\"); self.ticks = 0; for (;;) { wait 0.05; self.ticks++; } }\n"
    "errhilo() { level.antes_error = 1; a = 5; b = a.campo; level.b_definida = isdefined(b); level.tras_error = 1; }\n"
    "hijo() { self endon(\"death\"); self.hijo_ticks = 0; for (;;) { wait 0.05; self.hijo_ticks++; } }\n";

static void H5_InitVM(scriptInstance_t inst)
{
    // sin servidor corriendo: Scr_EndLoadScripts no arranca el depurador de scripts (cscr_debugger, sin portar)
    if (!com_sv_running) com_sv_running = _Dvar_RegisterBool("sv_running", false, 0, "");
    Scr_InitVariables(inst);
    Scr_Init(inst);
    // zombiemode=1 como en el juego: Scr_Settings respeta entonces developer/developer_script. H5_DEV=1 = modo developer
    // (compila los /# #/ y da fichero:linea en los errores); por defecto 0, como el juego publicado.
    if (!g_DebugHunkUser) g_DebugHunkUser = Hunk_UserCreate(0x1700000, HU_SCHEME_DEBUG, 0, 0, "Hunk_InitDebugMemory", 0);
    if (!zombiemode) zombiemode = _Dvar_RegisterBool("zombiemode", true, 0, "");
    int dev = getenv("H5_DEV") ? atoi(getenv("H5_DEV")) : 0;
    Scr_Settings(dev, dev, 0, inst);
}

// prueba propia: compila maps/h5test.gsc (en memoria) y ejecuta main() avanzando el tiempo de servidor
static int H5_OwnTest()
{
    const scriptInstance_t inst = SCRIPTINSTANCE_SERVER;
    FS_H0_AddMemFile("maps/h5test.gsc", s_testGsc, sizeof(s_testGsc) - 1);
    H5_InitVM(inst);
    Scr_BeginLoadScripts(inst, 1);
    g_h5Local = true;
    unsigned loaded = Scr_LoadScript(inst, (char *)"maps/h5test");
    g_h5Local = false;
    int handle = Scr_GetFunctionHandle(inst, "maps/h5test", "main");
    Scr_PostCompileScripts(inst);
    Scr_EndLoadScripts(inst);
    Scr_EndLoadAnimTrees(inst);     // como el juego tras cargar los arboles de animacion: cierra la carga
    fprintf(stderr, "[H5] compilado maps/h5test: loaded=%u handle=%d programLen=%u\n", loaded, handle, gScrCompilePub[inst].programLen);
    if (!handle) return 1;
    Scr_InitSystem(inst, 1);
    unsigned th = Scr_ExecThread(inst, handle, 0);
    Scr_FreeThread(th, inst);
    int frames = 0;
    for (; !g_h5Done && frames < 400; ++frames) Scr_IncTime(inst);
    fprintf(stderr, "[H5] main() termino=%d tras %d frames: %d OK, %d FALLA\n", g_h5Done, frames, g_h5Pass, g_h5Fail);
    return g_h5Done && !g_h5Fail ? 0 : 2;
}

// solo-compilacion de un GSC real: en un proceso hijo (un CompileError del motor acaba en Com_Error -> exit) con la VM nueva
static bool H5_CompileOne(const char *name, std::string &err)
{
    int pfd[2]; if (pipe(pfd)) return false;
    pid_t pid = fork();
    if (!pid)
    {
        close(pfd[0]); dup2(pfd[1], 2);
        const scriptInstance_t inst = SCRIPTINSTANCE_SERVER;
        H5_InitVM(inst);
        Scr_BeginLoadScripts(inst, 1);
        unsigned ok = Scr_LoadScript(inst, (char *)name);
        Scr_PostCompileScripts(inst);
        fprintf(stderr, "[H5C] loaded=%u programLen=%u\n", ok, gScrCompilePub[inst].programLen);
        _exit(ok ? 0 : 7);
    }
    close(pfd[1]);
    std::string out; char b[4096]; ssize_t n;
    while ((n = read(pfd[0], b, sizeof b)) > 0) out.append(b, n);
    close(pfd[0]);
    int st = 0; waitpid(pid, &st, 0);
    bool good = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (getenv("H5_RAW")) fprintf(stderr, "%s", out.c_str());   // salida completa del hijo (diagnostico)
    err.clear();
    if (!good)
    {   // ultima linea con contenido: el mensaje del motor
        size_t e = out.find_last_not_of("\n "); size_t s = out.rfind('\n', e == std::string::npos ? 0 : e);
        err = out.substr(s == std::string::npos ? 0 : s + 1, e == std::string::npos ? 0 : e - (s == std::string::npos ? 0 : s + 1) + 1);
        // el error de compilacion del motor puede ocupar varias lineas: se guarda todo lo que no es ruido
        std::string full; size_t p = 0;
        while (p < out.size()) { size_t q = out.find('\n', p); if (q == std::string::npos) q = out.size(); std::string l = out.substr(p, q - p); if (l.find("[stub0]") == std::string::npos && !l.empty() && l.rfind("    #", 0) != 0 && l.rfind("[SIGNAL", 0) != 0) full += l + " | "; p = q + 1; }
        err = full.size() > 600 ? full.substr(full.size() - 600) : full;
        if (WIFSIGNALED(st)) err += " (senal " + std::to_string(WTERMSIG(st)) + ")"; else err += " (exit " + std::to_string(WEXITSTATUS(st)) + ")";
    }
    return good;
}

static void H5_CollectRaw(XAssetHeader h, void *data)
{
    const char *nm = h.rawfile ? h.rawfile->name.get() : 0;
    if (!nm) return;
    std::string s = nm;
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".gsc") == 0) ((std::vector<std::string> *)data)->push_back(s.substr(0, s.size() - 4));
}

static int H5_RealCompile()
{
    std::vector<std::string> all, pick;
    DB_EnumXAssets_FastFile(ASSET_TYPE_RAWFILE, H5_CollectRaw, &all, true);
    std::sort(all.begin(), all.end()); all.erase(std::unique(all.begin(), all.end()), all.end());
    if (const char *fl = getenv("H5_FILES"))
    {
        std::string l = fl; size_t p = 0;
        while (p <= l.size()) { size_t q = l.find(',', p); if (q == std::string::npos) q = l.size(); if (q > p) pick.push_back(l.substr(p, q - p)); p = q + 1; }
    }
    else if (getenv("H5_VM") && !strcmp(getenv("H5_VM"), "3"))
        pick = all;     // H5_VM=3: todos los GSC de las zonas cargadas
    else
        for (auto &s : all) if (s.rfind("maps/_zombiemode", 0) == 0) pick.push_back(s);
    fprintf(stderr, "[H5] %zu GSC en las zonas cargadas; solo-compilacion de %zu\n", all.size(), pick.size());
    int good = 0;
    std::vector<std::pair<std::string, std::string>> bad;
    for (auto &s : pick)
    {
        std::string err;
        bool ok = H5_CompileOne(s.c_str(), err);
        fprintf(stderr, "[H5C] %-44s %s %s\n", s.c_str(), ok ? "OK" : "FALLA", ok ? "" : err.c_str());
        if (ok) ++good; else bad.push_back({ s, err });
    }
    fprintf(stderr, "[H5] solo-compilacion: %d/%zu compilan\n", good, pick.size());
    return bad.empty() ? 0 : 3;
}

// H5_DUMP=maps/x: escribe por stderr la fuente de un GSC de las zonas (diagnostico; no se guarda en ningun sitio)
struct H5Ver { std::string name; int n; };
static void H5_Versions(XAssetHeader h, void *d)
{
    H5Ver *v = (H5Ver *)d;
    if (h.rawfile && h.rawfile->name.get() && v->name == h.rawfile->name.get())
        fprintf(stderr, "[H5] version %d de %s: len=%d descomprimido=%u\n", v->n++, v->name.c_str(), h.rawfile->len, *(const unsigned int *)h.rawfile->buffer.get());
}
static void H5_Dump(const char *name)
{
    { H5Ver v{ std::string(name) + ".gsc", 0 }; DB_EnumXAssets_FastFile(ASSET_TYPE_RAWFILE, H5_Versions, &v, true); }
    std::string n = std::string(name) + ".gsc";
    RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)n.c_str(), 0, -1).rawfile;
    if (!rf) { fprintf(stderr, "[H5] %s no esta en las zonas\n", n.c_str()); return; }
    uLongf len = *(const unsigned int *)rf->buffer.get(), inl = *((const unsigned int *)rf->buffer.get() + 1);
    std::vector<unsigned char> o(len + 1);
    uncompress(o.data(), &len, (const unsigned char *)rf->buffer.get() + 8, inl);
    o[len] = 0; fprintf(stderr, "[H5DUMP %s %lu]\n%s\n[/H5DUMP]\n", n.c_str(), (unsigned long)len, (char *)o.data());
}

int H5_Run()
{
    if (const char *d = getenv("H5_DUMP")) { H5_Dump(d); return 0; }
    const char *m = getenv("H5_VM");
    int r = 0;
    // la pasada real va primero: cada hijo (fork) parte de la VM sin inicializar
    if (m && strcmp(m, "1")) r = H5_RealCompile();
    int r2 = H5_OwnTest();
    return r ? r : r2;
}
