// Guardian de memoria: hilo que lee VmRSS y MemAvailable; registra cada 2 s y hace abort() ordenado si el proceso o el sistema se pasan
// (un exceso de RAM/GPU en el Pixel Fold llego a tumbar el SO con kernel_panic por OOM). Env: MG_RSS_MAX_MB (2500), MG_AVAIL_MIN_MB (1500).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif
static long ReadKb(const char *file, const char *key)
{
    FILE *f = fopen(file, "r"); if (!f) return -1; char l[256]; long v = -1; size_t n = strlen(key);
    while (fgets(l, sizeof l, f)) if (!strncmp(l, key, n)) { v = atol(l + n); break; }
    fclose(f); return v;
}
static void *Guard(void *)
{
    long rssMax = (getenv("MG_RSS_MAX_MB") ? atol(getenv("MG_RSS_MAX_MB")) : 1500) * 1024L;
    long availMin = (getenv("MG_AVAIL_MIN_MB") ? atol(getenv("MG_AVAIL_MIN_MB")) : 2500) * 1024L;
    int tick = 0;
    for (;;) {
        long rss = ReadKb("/proc/self/status", "VmRSS:"), avail = ReadKb("/proc/meminfo", "MemAvailable:");
        if (tick % 8 == 0) {
            char m[200]; snprintf(m, sizeof m, "[memguard] RSS=%ld MB MemAvailable=%ld MB (limites RSS<%ld MB, avail>%ld MB)", rss / 1024, avail / 1024, rssMax / 1024, availMin / 1024);
            fprintf(stderr, "%s\n", m);
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_INFO, "bo1native", "%s", m);
#endif
        }
        if (rss > rssMax || (avail >= 0 && avail < availMin)) {
            char m[200]; snprintf(m, sizeof m, "[memguard] LIMITE SUPERADO: RSS=%ld MB avail=%ld MB -> abort()", rss / 1024, avail / 1024);
            fprintf(stderr, "%s\n", m);
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_FATAL, "bo1native", "%s", m);
#endif
            fflush(stderr); abort();
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
    char l[320]; snprintf(l, sizeof l, "[H15MEM] %s fase=%s t=%ld rss=%ld anon=%ld file=%ld shmem=%ld vss=%ld hwm=%ld heap=%ld pico=%ld(%s) KB", why, ph, H15Ms() - t0,
                          m.rss, m.anon, m.file, m.shm, m.vss, m.hwm, m.heap, s_h15Peak, s_h15PeakPhase);
    fprintf(stderr, "%s\n", l);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "bo1native", "%s", l);
#endif
    if (s_h15Csv) { fprintf(s_h15Csv, "%ld,%s,%s,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n", H15Ms() - t0, why, ph, m.rss, m.anon, m.file, m.shm, m.vss, m.hwm, m.heap); fflush(s_h15Csv); }
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
    if (strcmp(e, "1")) { s_h15Csv = fopen(e, "w"); if (s_h15Csv) fprintf(s_h15Csv, "t_ms,motivo,fase,rss_kb,anon_kb,file_kb,shmem_kb,vss_kb,hwm_kb,heap_kb\n"); }
    atexit([] { H15Log("SALIDA"); });
    pthread_t t; pthread_create(&t, nullptr, H15Thread, nullptr); pthread_detach(t);
}
static void H15Begin() { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, H15BeginOnce); }
__attribute__((constructor(103))) static void H15Start() { if (H15On()) H15Begin(); }
