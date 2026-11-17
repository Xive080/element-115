// Guardian de memoria: hilo que lee VmRSS y MemAvailable cada 250 ms, registra cada 2 s y protege al sistema (un exceso de RAM/GPU en el
// Pixel Fold llego a tumbar el SO con kernel_panic por OOM; INCIDENTE 2026-11-16). SIEMPRE activo: no hay forma de apagarlo.
// OLA 18: guarda por TRAMOS (antes: RSS>1500 MB o avail<2500 MB -> abort):
//   - RSS (VmRSS de /proc/self/status: lo residente REAL del proceso; incluye la memoria del driver Mali mapeada, sin el doble conteo de
//     dumpsys) > MG_RSS_MAX_MB (3072) -> abort() inmediato. Es el techo duro.
//   - RSS > MG_RSS_SOFT_MB (2048, el objetivo normal) -> PRESION: la app reduce calidad en vez de morir (g_mgPressure, ver abajo).
//   - MemAvailable del sistema < MG_AVAIL_LOW_MB (1500) -> PRESION. Solo aborta si sigue cayendo: avail < MG_AVAIL_MIN_MB (1000)
//     durante MG_AVAIL_MIN_TICKS muestras seguidas (8 = 2 s), o < la mitad de MG_AVAIL_MIN_MB en una sola muestra.
//   - La presion se quita con histeresis: avail > LOW + 300 y RSS < SOFT - 128 durante 5 s seguidos.
// Las variables se vuelven a leer cada 2 s (en Android llegan DESPUES de este constructor, desde android_main / debug.bo1.env) y SOLO pueden
// APRETAR los valores compilados: un MG_RSS_MAX_MB mayor que 3072 o un MG_AVAIL_MIN_MB menor que 1000 se ignoran.
// Presion (g_mgPressure: 0 nada, 1 presion): vk_frame recorta tex.pkg cada frame y vk_texture crea las texturas NUEVAS con la mitad de lado
// (un mip menos); aqui, al entrar, mallopt(M_PURGE) devuelve al sistema lo libre del heap. MG_TEST_PRESSURE_S=s (pruebas) simula presion
// desde el segundo s (no afloja nada).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <malloc.h>
#endif
static long ReadKb(const char *file, const char *key)
{
    FILE *f = fopen(file, "r"); if (!f) return -1; char l[256]; long v = -1; size_t n = strlen(key);
    while (fgets(l, sizeof l, f)) if (!strncmp(l, key, n)) { v = atol(l + n); break; }
    fclose(f); return v;
}
extern "C" { volatile int g_mgPressure = 0; }   // lo leen vk_frame / vk_texture (referencia debil: sin memguard vale 0)
static const long kRssMaxMb = 3072, kRssSoftMb = 2048, kAvailLowMb = 1500, kAvailMinMb = 1000;
struct MgCfg { long rssMax, rssSoft, availLow, availMin; int minTicks; long testS; };
static long EnvMb(const char *n, long def, bool tighterIsLower)
{
    const char *e = getenv(n); if (!e || !*e) return def; long v = atol(e); if (v <= 0) return def;
    return tighterIsLower ? (v < def ? v : def) : (v > def ? v : def);   // solo aprieta
}
static MgCfg MgRead()
{
    MgCfg c; c.rssMax = EnvMb("MG_RSS_MAX_MB", kRssMaxMb, true); c.rssSoft = EnvMb("MG_RSS_SOFT_MB", kRssSoftMb, true); if (c.rssSoft > c.rssMax) c.rssSoft = c.rssMax;
    c.availMin = EnvMb("MG_AVAIL_MIN_MB", kAvailMinMb, false); c.availLow = EnvMb("MG_AVAIL_LOW_MB", kAvailLowMb, false); if (c.availLow < c.availMin) c.availLow = c.availMin;
    const char *t = getenv("MG_AVAIL_MIN_TICKS"); c.minTicks = t && atoi(t) > 0 && atoi(t) < 8 ? atoi(t) : 8;   // solo aprieta (menos muestras)
    const char *p = getenv("MG_TEST_PRESSURE_S"); c.testS = p && atol(p) > 0 ? atol(p) : 0;
    return c;
}
static void MgLog(int prio, const char *m)
{
    fprintf(stderr, "%s\n", m);
#ifdef __ANDROID__
    __android_log_print(prio == 2 ? ANDROID_LOG_FATAL : prio == 1 ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "bo1native", "%s", m);
#endif
}
static void *Guard(void *)
{
    MgCfg c = MgRead(); int tick = 0, lowTicks = 0, okTicks = 0; long minAvail = -1, peakRss = 0;
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); const long t0 = ts.tv_sec;
    for (;;) {
        if (tick % 8 == 0) c = MgRead();
        long rss = ReadKb("/proc/self/status", "VmRSS:"), avail = ReadKb("/proc/meminfo", "MemAvailable:");
        if (rss > peakRss) peakRss = rss; if (avail >= 0 && (minAvail < 0 || avail < minAvail)) minAvail = avail;
        char m[260];
        if (tick % 8 == 0) {
            snprintf(m, sizeof m, "[memguard] RSS=%ld MB (pico %ld) MemAvailable=%ld MB (min %ld) presion=%d (techo RSS %ld, objetivo %ld, avail baja<%ld, aborto<%ld)",
                     rss / 1024, peakRss / 1024, avail / 1024, minAvail / 1024, g_mgPressure, c.rssMax, c.rssSoft, c.availLow, c.availMin);
            MgLog(0, m);
        }
        // 1) techo duro de RSS y caida sostenida de MemAvailable -> abort ordenado
        const bool availVeryLow = avail >= 0 && avail < c.availMin * 1024L / 2;
        lowTicks = avail >= 0 && avail < c.availMin * 1024L ? lowTicks + 1 : 0;
        if (rss > c.rssMax * 1024L || availVeryLow || lowTicks >= c.minTicks) {
            snprintf(m, sizeof m, "[memguard] LIMITE SUPERADO: RSS=%ld MB (techo %ld) avail=%ld MB (min %ld, %d muestras seguidas) -> abort()", rss / 1024, c.rssMax, avail / 1024, c.availMin, lowTicks);
            MgLog(2, m); fflush(stderr); abort();
        }
        // 2) presion: reducir calidad en vez de morir
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const bool test = c.testS && ts.tv_sec - t0 >= c.testS;
        const bool hot = test || rss > c.rssSoft * 1024L || (avail >= 0 && avail < c.availLow * 1024L);
        const bool cool = !test && rss < (c.rssSoft - 128) * 1024L && (avail < 0 || avail > (c.availLow + 300) * 1024L);
        if (hot && !g_mgPressure) {
            g_mgPressure = 1; okTicks = 0;
            snprintf(m, sizeof m, "[memguard] PRESION ON: RSS=%ld MB avail=%ld MB%s -> recorte de tex.pkg por frame, texturas nuevas a mitad de lado, purga del heap",
                     rss / 1024, avail / 1024, test ? " (MG_TEST_PRESSURE_S)" : "");
            MgLog(1, m);
#if defined(__ANDROID__) && defined(M_PURGE)
            mallopt(M_PURGE, 0);
#endif
        } else if (g_mgPressure) {
            okTicks = cool ? okTicks + 1 : 0;
            if (okTicks >= 20) { g_mgPressure = 0; snprintf(m, sizeof m, "[memguard] PRESION OFF: RSS=%ld MB avail=%ld MB", rss / 1024, avail / 1024); MgLog(1, m); }
        }
        ++tick; usleep(250000);
    }
    return nullptr;
}
__attribute__((constructor(102))) static void StartGuard() { pthread_t t; pthread_create(&t, nullptr, Guard, nullptr); pthread_detach(t); }

// ---- OLA 15A: H15_MEMPROFILE=1 (o =ruta.csv) registra la memoria del proceso por FASES y cada H15_MEMPROFILE_MS (1000) ms:
// Linux/Android: VmRSS, RssAnon, RssFile, RssShmem, VmSize (VSS), VmHWM de /proc/self/status + mallinfo (uordblks/hblkhd).
// Mac: task_info (resident_size, phys_footprint, virtual_size) + mstats. Lineas "[H15MEM] fase=<f> t=<ms> rss=.. anon=.. file=.. vss=.. heap=.. pico=..".
// Las fases las marca H15_MemPhase("carga", "partida", "ronda 3", ...) desde el arnes; sin H15_MEMPROFILE no hace nada (coste cero).
#include <atomic>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
struct H15Mem { long rss, anon, file, shm, vss, hwm, heap; };   // KB
static H15Mem H15Read()
{
    H15Mem m = { -1, -1, -1, -1, -1, -1, -1 };
#ifdef __APPLE__
    task_vm_info_data_t vi; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) == KERN_SUCCESS) {
        m.rss = (long)(vi.resident_size >> 10); m.anon = (long)(vi.phys_footprint >> 10); m.vss = (long)(vi.virtual_size >> 10); m.hwm = (long)(vi.resident_size_peak >> 10);
        m.file = (long)((vi.resident_size > vi.internal ? vi.resident_size - vi.internal : 0) >> 10);   // aproximado: residente no interno
    }
    malloc_statistics_t ms; malloc_zone_statistics(nullptr, &ms); m.heap = (long)(ms.size_in_use >> 10);
#else
    m.rss = ReadKb("/proc/self/status", "VmRSS:"); m.anon = ReadKb("/proc/self/status", "RssAnon:"); m.file = ReadKb("/proc/self/status", "RssFile:");
    m.shm = ReadKb("/proc/self/status", "RssShmem:"); m.vss = ReadKb("/proc/self/status", "VmSize:"); m.hwm = ReadKb("/proc/self/status", "VmHWM:");
    struct mallinfo mi = mallinfo(); m.heap = (long)(((unsigned long)mi.uordblks + (unsigned long)mi.hblkhd) >> 10);
#endif
    return m;
}
static std::atomic<const char *> s_h15Phase{ "arranque" }; static FILE *s_h15Csv; static long s_h15Peak; static const char *s_h15PeakPhase = "arranque";
// OLA 16: en Android el constructor corre al cargar libmain.so, ANTES de que android_main ponga las variables (debug.bo1.env): mientras
// este apagado se vuelve a leer la variable, y el hilo de muestras arranca en la primera fase si no arranco en el constructor.
static bool H15On() { static int on = -1; if (on <= 0) { const char *e = getenv("H15_MEMPROFILE"); on = e && *e && strcmp(e, "0"); } return on > 0; }
static void H15Begin();
static long H15Ms() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L; }
static void H15Log(const char *why)
{
    static long t0 = H15Ms(); static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mu);
    const H15Mem m = H15Read(); const char *ph = s_h15Phase.load();
    if (m.rss > s_h15Peak) { s_h15Peak = m.rss; s_h15PeakPhase = ph; }
    const long avail = ReadKb("/proc/meminfo", "MemAvailable:");   // OLA 18: memoria libre del sistema y presion de memguard en cada muestra
    char l[360]; snprintf(l, sizeof l, "[H15MEM] %s fase=%s t=%ld rss=%ld anon=%ld file=%ld shmem=%ld vss=%ld hwm=%ld heap=%ld avail=%ld presion=%d pico=%ld(%s) KB", why, ph, H15Ms() - t0,
                          m.rss, m.anon, m.file, m.shm, m.vss, m.hwm, m.heap, avail, g_mgPressure, s_h15Peak, s_h15PeakPhase);
    fprintf(stderr, "%s\n", l);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "bo1native", "%s", l);
#endif
    if (s_h15Csv) { fprintf(s_h15Csv, "%ld,%s,%s,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%d\n", H15Ms() - t0, why, ph, m.rss, m.anon, m.file, m.shm, m.vss, m.hwm, m.heap, avail, g_mgPressure); fflush(s_h15Csv); }
    pthread_mutex_unlock(&mu);
}
extern "C" void H15_MemPhase(const char *phase)   // fases: el texto debe vivir siempre (literal o static)
{
    if (!H15On()) return;
    H15Begin(); s_h15Phase.store(phase); H15Log("FASE");
}
static void *H15Thread(void *)
{
    const long ms = getenv("H15_MEMPROFILE_MS") ? atol(getenv("H15_MEMPROFILE_MS")) : 1000;
    for (;;) { usleep((useconds_t)(ms > 50 ? ms : 50) * 1000); H15Log("muestra"); }
    return nullptr;
}
static void H15BeginOnce()
{
    const char *e = getenv("H15_MEMPROFILE");
    if (strcmp(e, "1")) { s_h15Csv = fopen(e, "w"); if (s_h15Csv) fprintf(s_h15Csv, "t_ms,motivo,fase,rss_kb,anon_kb,file_kb,shmem_kb,vss_kb,hwm_kb,heap_kb,avail_kb,presion\n"); }
    atexit([] { H15Log("SALIDA"); });
    pthread_t t; pthread_create(&t, nullptr, H15Thread, nullptr); pthread_detach(t);
}
static void H15Begin() { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, H15BeginOnce); }
__attribute__((constructor(103))) static void H15Start() { if (H15On()) H15Begin(); }
