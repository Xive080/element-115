// Runtime minimo del harness H0: lo imprescindible que db_*.cpp espera del resto del motor.
#ifdef H5G
#define H5W __attribute__((weak)) // H5G: el servidor ORIGINAL se enlaza entero y su TU real gana a este pegamento
#else
#define H5W
#endif

#include <universal/q_shared.h>
#include <universal/dvar.h>
#include <universal/com_memory.h>
#include <universal/physicalmemory.h>
#include <universal/ptr32.h>
#include <qcommon/common.h>
#include <qcommon/threads.h>
#include <win32/win_shared.h>
#include <win32/win_common.h>
#include <clientscript/cscr_stringlist.h>
#include <Windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <mutex>
#ifndef __ANDROID__
#include <execinfo.h>
static void h0_bt() { void *b[24]; int n = backtrace(b, 24); char **s = backtrace_symbols(b, n); for (int i = 1; i < n; ++i) { const char *l = s[i]; const char *p = strstr(l, " _"); fprintf(stderr, "    #%d %s\n", i, p ? p + 1 : l); } fflush(stderr); }
#else
#include <unwind.h>
#include <dlfcn.h>
struct BtSt { int n; };
static _Unwind_Reason_Code BtCb(_Unwind_Context *c, void *a) { BtSt *s = (BtSt *)a; uintptr_t pc = _Unwind_GetIP(c); if (!pc) return _URC_END_OF_STACK; Dl_info d; if (dladdr((void *)pc, &d)) fprintf(stderr, "    #%d %s+0x%lx (%s+0x%lx)\n", s->n, d.dli_sname ? d.dli_sname : "?", d.dli_sname ? (long)(pc - (uintptr_t)d.dli_saddr) : 0L, d.dli_fname ? strrchr(d.dli_fname, '/') + 1 : "?", (long)(pc - (uintptr_t)d.dli_fbase)); else fprintf(stderr, "    #%d %p\n", s->n, (void *)pc); return ++s->n >= 24 ? _URC_END_OF_STACK : _URC_NO_REASON; }
static void h0_bt() { BtSt s = { 0 }; _Unwind_Backtrace(BtCb, &s); fflush(stderr); }
#endif

// ---- hooks de los stubs autogenerados
// H0_STUB_SOFT=1 (solo para descubrir de una pasada que stubs pide un camino): registra el primero de cada nombre con traza y devuelve 0.
extern "C" void h0_stub_abort(const char *name)
{
    static const bool soft = getenv("H0_STUB_SOFT") != nullptr;
    if (soft) { static std::set<std::string> seen; if (seen.insert(name).second) { fprintf(stderr, "[STUBSOFT] %s (devuelve 0)\n", name); h0_bt(); } return; }
    fprintf(stderr, "[STUB] %s llamado -> abort\n", name); h0_bt(); abort();
}
extern "C" void h0_stub_log(const char *name)
{
    static std::set<std::string> seen;
    if (seen.insert(name).second) fprintf(stderr, "[stub0] %s (devuelve 0)\n", name);
}

// ---- E/S de fichero (ReadFileEx overlapped simulado, sincrono)
static DWORD g_lastErr = 0;
DWORD GetLastError() { return g_lastErr; }
BOOL CloseHandle(HANDLE h) { if (h && h != INVALID_HANDLE_VALUE) close((int)(intptr_t)h - 1000); return 1; }
DWORD GetFileSize(HANDLE h, DWORD *) { struct stat st; fstat((int)(intptr_t)h - 1000, &st); return (DWORD)st.st_size; }
void SleepEx(DWORD, BOOL) {}
BOOL ReadFileEx(HANDLE h, void *buf, DWORD n, OVERLAPPED *ov, void (*)(DWORD, DWORD, OVERLAPPED *))
{
    off_t off = ((off_t)ov->OffsetHigh << 32) | ov->Offset;
    ssize_t r = pread((int)(intptr_t)h - 1000, buf, n, off);
    if (r <= 0) { g_lastErr = 38; return 0; }      // ERROR_HANDLE_EOF
    if ((DWORD)r < n) memset((char *)buf + r, 0, n - r);
    return 1;
}
HANDLE h0_open(const char *path) { int fd = open(path, O_RDONLY); return fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE)(intptr_t)(fd + 1000); }

// ---- mensajes / errores
static void vout(const char *tag, const char *fmt, va_list ap) { fputs(tag, stderr); vfprintf(stderr, fmt, ap); }
int g_h0PrintAll;   // H5G: H5_GAME_VERBOSE activa todos los canales una vez cargadas las zonas
void Com_Printf(int ch, const char *fmt, ...) { va_list ap; va_start(ap, fmt); if (g_h0PrintAll || getenv("H0_VERBOSE") || ch == 10) vout("", fmt, ap); va_end(ap); }
void Com_PrintError(int, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vout("[ERROR] ", fmt, ap); va_end(ap); }
void Com_PrintWarning(int, const char *fmt, ...) { va_list ap; va_start(ap, fmt); if (getenv("H0_VERBOSE")) vout("[WARN] ", fmt, ap); va_end(ap); }
void Com_Error(errorParm_t code, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vout("[Com_Error] ", fmt, ap); va_end(ap); fputc('\n', stderr); exit(3); }
void Sys_Error(char *fmt, ...) { va_list ap; va_start(ap, fmt); vout("[Sys_Error] ", fmt, ap); va_end(ap); fputc('\n', stderr); exit(4); }
void tlPrintf(const char *, ...) {}
bool Assert_MyHandler(const char *file, int line, int, const char *fmt, ...)
{
    fprintf(stderr, "[ASSERT] %s:%d: ", file, line); va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
    h0_bt(); abort();
}
unsigned int Sys_MillisecondsRaw() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
unsigned int Sys_Milliseconds() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }

// ---- memoria: Z_Malloc / Com_Live* / PMem dentro de la arena (algunos destinos se guardan en Ptr32)
unsigned int *Z_Malloc(int size, const char *, int) { return (unsigned int *)Ptr32_ArenaAlloc(size, 64); }
void Z_Free(char *p, int) { Ptr32_ArenaFree(p); }
void *Com_LiveAllocate(unsigned int n) { return Ptr32_ArenaAlloc(n, 64); }
void Com_LiveDeallocate(void *p) { Ptr32_ArenaFree(p); }
unsigned char *Com_LiveCalloc(unsigned int n, unsigned int s) { return (unsigned char *)Ptr32_ArenaAlloc((size_t)n * s, 64); }
unsigned char *Com_LiveRealloc(unsigned char *p, unsigned int n) { unsigned char *q = (unsigned char *)Ptr32_ArenaAlloc(n, 64); if (p) { memcpy(q, p, n); Ptr32_ArenaFree(p); } return q; }
void PMem_BeginAlloc(const char *, unsigned int, EMemTrack) {}
void PMem_EndAlloc(const char *, unsigned int) {}
void PMem_Free(const char *) {}
int PMem_GetOverAllocatedSize() { return 0; }
unsigned char *_PMem_AllocNamed(unsigned int size, unsigned int align, int, unsigned int, const char *, EMemTrack) { return (unsigned char *)Ptr32_ArenaAlloc(size, align); }

// ---- hilos / sincronizacion: un solo hilo
bool Sys_IsMainThread() { return true; }
bool Sys_IsDatabaseThread() { return true; }
bool g_h0InDbLoad = false;      // true mientras corre DB_LoadXFile: el "hilo DB" no es el de render (RB_Resource_Flush lo exige)
bool Sys_IsRenderThread() { return !g_h0InDbLoad; }
// OLA 14: la unica seccion critica real es CRITSEC_SOUND_OCCLUSION, compartida entre el hilo de juego (SND_LosOcclusionUpdate) y el
// hilo de oclusion ORIGINAL (snd_occlusion.cpp, h5_audio_cl.cpp lo lanza); las demas siguen vacias (el resto es de un hilo).
// Recursiva como CRITICAL_SECTION de Win32 (con H6A_OCCTHREAD=0 la traza corre dentro de Sys_SetOcclusion con la seccion tomada).
static std::recursive_mutex &OccMutex() { static std::recursive_mutex *m = new std::recursive_mutex; return *m; }
void Sys_EnterCriticalSection(CriticalSection c) { if (c == CRITSEC_SOUND_OCCLUSION) OccMutex().lock(); }
void Sys_LeaveCriticalSection(CriticalSection c) { if (c == CRITSEC_SOUND_OCCLUSION) OccMutex().unlock(); }
bool __cdecl Sys_TryEnterCriticalSection(CriticalSection c) { return c == CRITSEC_SOUND_OCCLUSION ? OccMutex().try_lock() : true; }
void Sys_WaitDiscReads() {}
void Sys_SuspendDiscReads(ThreadOwner) {}
void Sys_ResumeDiscReads(ThreadOwner) {}
bool Sys_HaveSuspendedDiscReads(ThreadOwner) { return false; }
void Sys_SyncDatabase() {}
void Sys_NotifyDatabase() {}
void Sys_WakeDatabase() {}
void Sys_WakeDatabase2() {}
void Sys_DatabaseCompleted() {}
void Sys_DatabaseCompleted2() {}
void Sys_WaitStartDatabase() {}
bool Sys_IsDatabaseReady() { return true; }
bool Sys_IsDatabaseReady2() { return true; }
int Sys_WaitRenderer() { return 0; }
bool Sys_IsHeadless() { return true; }
void Com_SyncThreads() {}

// ---- cadenas de script (SL_*): las REALES de clientscript/cscr_stringlist.cpp (main2 llama a SL_Init antes de cargar zonas)

// ---- DB_EnumXAssets generico (vive en otro modulo del motor): en modo fastfile = el de la tabla hash
#include <database/db_registry.h>
void DB_EnumXAssets(XAssetType type, void (*func)(XAssetHeader, void *), void *in, bool ov) { DB_EnumXAssets_FastFile(type, func, in, ov); }
H5W int XAnimTreeSize() { return 8; }  // xanim.cpp:23 (sizeof XAnimTree_s == 8, valor del ejecutable; solo contabilidad de tamaños)

// ---- singletons de assets (en el motor viven en cm_load/com_bsp/g_bsp/r_bsp.cpp; aqui se definen igual, dentro de la arena)
#include <qcommon/cm_load.h>
#include <qcommon/com_bsp.h>
#include <game/g_bsp.h>
#include <gfx_d3d/r_bsp.h>
// H5G compila cm_load.cpp y g_bsp.cpp, que definen cm y gameWorldMp: un weak aqui NO basta (su inicializador dinamico
// tambien corre y reengancha la referencia despues de que el pool de la DB guardase la direccion real).
#ifndef H5G
clipMap_t &cm = *new (Ptr32_ArenaAlloc(sizeof(clipMap_t), 64)) clipMap_t;
GameWorldMp &gameWorldMp = *new (Ptr32_ArenaAlloc(sizeof(GameWorldMp), 64)) GameWorldMp;
#endif
ComWorld &comWorld = *new (Ptr32_ArenaAlloc(sizeof(ComWorld), 64)) ComWorld;
GfxWorld &s_world = *new (Ptr32_ArenaAlloc(sizeof(GfxWorld), 64)) GfxWorld;


// ---- TLS del motor (Sys_GetValue/Sys_SetValue: 1=va_info_t, 2=jmp/db, 3=TraceThreadInfo, ...): bloques lazy de 64 KB.
// OLA 14: por hilo (thread_local), como el TlsGetValue del original: el hilo de oclusion trae su propio TraceThreadInfo.
static thread_local void *g_tls[16];
void *Sys_GetValue(int i) { if (!g_tls[i]) g_tls[i] = calloc(1, 1 << 16); return g_tls[i]; }
void Sys_SetValue(int i, void *p) { g_tls[i] = p; }

// ---- com_memory (NO compilado: usa VirtualAlloc/punteros de 32 bits): version minima del harness
char *CopyString(char *in, const char *, int, scriptInstance_t) { return strdup(in); }
void FreeString(const char *, int, scriptInstance_t) {}
unsigned char *Hunk_Alloc(unsigned int size, const char *, int) { return (unsigned char *)Ptr32_ArenaAlloc(size, 64); }
unsigned char *Hunk_AllocAlign(unsigned int size, int al, const char *, int) { return (unsigned char *)Ptr32_ArenaAlloc(size, al); }
// copia literal de server_mp/sv_bot_mp.cpp (ese fichero no se compila aqui)
H5W int Com_HashString(const char *fname, int len) { if (!fname) return 0; int hash = 5381; for (int i = 0; fname[i] && (!len || i < len); ++i) hash = tolower(fname[i]) + 33 * hash; return hash; }
// BG_ZombieBoolDefault: ahora el real de bgame/bg_misc.cpp (compilado para H4/Pmove)
unsigned int Sys_GetDefaultWorkerThreadsCount() { return 1; }
bool Sys_IsHeadlessClient() { return false; }
#include <signal.h>
static void h0_segv(int sig) { fprintf(stderr, "[SIGNAL %d]\n", sig); h0_bt(); _exit(5); }
#ifdef __ANDROID__
#include <ucontext.h>
static void h0_segv2(int sig, siginfo_t *si, void *uc)
{
    mcontext_t &m = ((ucontext_t *)uc)->uc_mcontext; fprintf(stderr, "[SIGNAL %d] addr=%p pc=%p lr=%p sp=%p\n", sig, si->si_addr, (void *)m.pc, (void *)m.regs[30], (void *)m.sp);
    Dl_info d; uintptr_t v[2] = { (uintptr_t)m.pc, (uintptr_t)m.regs[30] };
    for (int i = 0; i < 2; ++i) if (dladdr((void *)v[i], &d)) fprintf(stderr, "   %s %s+0x%lx\n", i ? "lr" : "pc", d.dli_sname ? d.dli_sname : "?", (long)(v[i] - (uintptr_t)d.dli_fbase));
    uintptr_t *fp = (uintptr_t *)m.regs[29]; for (int i = 0; i < 12 && fp && ((uintptr_t)fp & 7) == 0; ++i) { uintptr_t ra = fp[1]; if (dladdr((void *)ra, &d)) fprintf(stderr, "   #%d %s+0x%lx\n", i, d.dli_sname ? d.dli_sname : "?", (long)(ra - (uintptr_t)d.dli_fbase)); fp = (uintptr_t *)fp[0]; }
    _exit(5);
}
__attribute__((constructor(300))) static void H0_InstallSegv() { struct sigaction a; memset(&a, 0, sizeof a); a.sa_sigaction = h0_segv2; a.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &a, 0); sigaction(SIGBUS, &a, 0); sigaction(SIGABRT, &a, 0); }
#elif defined(__APPLE__) && defined(__aarch64__)
#include <sys/ucontext.h>
#include <dlfcn.h>
// Mac arm64: pc y direccion del fallo (la traza de _Unwind pierde la funcion hoja que falla); luego la traza normal
static void h0_segv2(int sig, siginfo_t *si, void *uc)
{
    __darwin_arm_thread_state64 &t = ((ucontext_t *)uc)->uc_mcontext->__ss;
    Dl_info d; uintptr_t v[2] = { (uintptr_t)__darwin_arm_thread_state64_get_pc(t), (uintptr_t)__darwin_arm_thread_state64_get_lr(t) };
    fprintf(stderr, "[SIGNAL %d] addr=%p pc=%p lr=%p\n", sig, si->si_addr, (void *)v[0], (void *)v[1]);
    for (int i = 0; i < 2; ++i) if (dladdr((void *)v[i], &d)) fprintf(stderr, "   %s %s+0x%lx (+%ld)\n", i ? "lr" : "pc", d.dli_sname ? d.dli_sname : "?", (long)(v[i] - (uintptr_t)d.dli_fbase), d.dli_saddr ? (long)(v[i] - (uintptr_t)d.dli_saddr) : -1L);
    h0_bt(); _exit(5);
}
__attribute__((constructor(300))) static void H0_InstallSegv() { struct sigaction a; memset(&a, 0, sizeof a); a.sa_sigaction = h0_segv2; a.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &a, 0); sigaction(SIGBUS, &a, 0); signal(SIGABRT, h0_segv); }
#else
__attribute__((constructor(300))) static void H0_InstallSegv() { signal(SIGSEGV, h0_segv); signal(SIGBUS, h0_segv); signal(SIGABRT, h0_segv); }
#endif
char *Z_VirtualAlloc(int size, const char *, int) { return (char *)Ptr32_ArenaAlloc(size, 64); }
char *Z_TryVirtualAlloc(int size, const char *, int) { return (char *)Ptr32_ArenaAlloc(size, 64); }
void *Z_VirtualReserve(int size) { return Ptr32_ArenaAlloc(size, 4096); }
void Z_VirtualCommit(char *, int, int) {}
void Z_VirtualFree(void *p, int) { Ptr32_ArenaFree(p); }
void Z_VirtualDecommit(char *, int, int) {}
bool R_FinishedFrontendWorkerCmds() { return true; }

// La cola de recursos (rb_resource.cpp) la consume el hilo de render; aqui el "hilo DB" que espera el flush la procesa sincronamente.
void RB_Resource_Update_Internal();
void Sys_WaitResourcesFlushedEvent() { bool s = g_h0InDbLoad; g_h0InDbLoad = false; RB_Resource_Update_Internal(); g_h0InDbLoad = s; }
int CL_LocalClient_GetActiveCount() { return 1; }
H5W bool CM_Mapkit_Empty() { return false; }   // mapa de zona normal: la colision del mapa original SI cuenta (true = mapkit vacio, sin colision base)
// cm_mapkit.cpp (cajas de edicion que se anaden al mundo) no se compila: un mapa de zona no tiene ninguna -> todo vacio
#include <qcommon/cm_mapkit.h>
H5W bool CM_Mapkit_IsBrush(const void *) { return false; }
H5W void CM_Mapkit_TraceThrough(const traceWork_t *, trace_t *) {}
H5W void CM_Mapkit_TestIn(const traceWork_t *, trace_t *) {}
H5W int CM_Mapkit_PointContents(const float *) { return 0; }
H5W int CM_Mapkit_SightTrace(const traceWork_t *, trace_t *) { return 0; }
// EffectsCore/fx_convert.cpp (SSE, no compilable aqui): version escalar. Color de vertice D3D = BGRA en bytes.
static unsigned char H0Q(float f) { f = f < 0 ? 0 : (f > 1 ? 1 : f); return (unsigned char)(f * 255.0f + 0.5f); }
void Byte4PackVertexColor(const float *from, unsigned char *to) { to[0] = H0Q(from[2]); to[1] = H0Q(from[1]); to[2] = H0Q(from[0]); to[3] = H0Q(from[3]); }
#ifndef H5FX   // con H5FX la define fx_sprite.cpp (OLA 6c)
void Byte4CopyRgbaToVertexColor(const unsigned char *from, unsigned char *to) { to[0] = from[2]; to[1] = from[1]; to[2] = from[0]; to[3] = from[3]; }
#endif
