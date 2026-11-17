// P3: FS shim minimo sobre los .iwd (zip method 0 sin cifrar) de gamedata/main. Indice por nombre en minusculas.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

struct IwdEntry { int fd; uint64_t off; uint32_t size; };
static std::unordered_map<std::string, IwdEntry> g_iwdIndex;
struct OpenFile { IwdEntry e; uint64_t pos; bool used; const char *mem = nullptr; };
// H5: ficheros en memoria (GSC de prueba del harness); tienen prioridad sobre los .iwd
static std::unordered_map<std::string, std::string> g_memFiles;
void FS_H0_AddMemFile(const char *name, const char *data, size_t len) { std::string k = name; for (auto &c : k) { c = tolower((unsigned char)c); if (c == '\\') c = '/'; } g_memFiles[k] = std::string(data, len); }
static std::vector<OpenFile> g_open(1);
static bool g_iwdInit = false;

static void IwdAdd(const std::string &path)
{
    int fd = open(path.c_str(), O_RDONLY); if (fd < 0) return;
    struct stat st; fstat(fd, &st); uint64_t sz = st.st_size;
    uint64_t tail = std::min<uint64_t>(sz, 70000); std::vector<uint8_t> t(tail);
    pread(fd, t.data(), tail, sz - tail);
    int64_t eocd = -1; for (int64_t i = (int64_t)tail - 22; i >= 0; --i) if (!memcmp(&t[i], "PK\5\6", 4)) { eocd = i; break; }
    if (eocd < 0) { close(fd); return; }
    uint16_t n = *(uint16_t *)&t[eocd + 10]; uint32_t cdsz = *(uint32_t *)&t[eocd + 12], cdoff = *(uint32_t *)&t[eocd + 16];
    std::vector<uint8_t> cd(cdsz); pread(fd, cd.data(), cdsz, cdoff);
    size_t p = 0; int added = 0;
    for (int i = 0; i < n && p + 46 <= cdsz; ++i) {
        uint16_t method = *(uint16_t *)&cd[p + 10]; uint32_t csz = *(uint32_t *)&cd[p + 20], usz = *(uint32_t *)&cd[p + 24];
        uint16_t nl = *(uint16_t *)&cd[p + 28], el = *(uint16_t *)&cd[p + 30], cl = *(uint16_t *)&cd[p + 32]; uint32_t lho = *(uint32_t *)&cd[p + 42];
        std::string nm((char *)&cd[p + 46], nl); for (auto &c : nm) c = tolower((unsigned char)c);
        p += 46 + nl + el + cl;
        if (method != 0 || csz != usz) continue;
        uint8_t lh[30]; pread(fd, lh, 30, lho);
        uint64_t data = lho + 30 + *(uint16_t *)&lh[26] + *(uint16_t *)&lh[28];
        g_iwdIndex[nm] = IwdEntry{fd, data, usz}; ++added;
    }
}
static void IwdInit()
{
    g_iwdInit = true;
    const char *gd = getenv("P3_GAMEDATA"); std::string dir = std::string(gd ? gd : "../../gamedata") + "/main";
    std::vector<std::string> names; DIR *d = opendir(dir.c_str());
    if (d) { while (dirent *e = readdir(d)) { std::string n = e->d_name; if (n.size() > 4 && n.substr(n.size() - 4) == ".iwd") names.push_back(n); } closedir(d); }
    std::sort(names.begin(), names.end());
    for (auto &n : names) IwdAdd(dir + "/" + n);      // los posteriores (iw_xx) pisan a los anteriores
    fprintf(stderr, "[iwd] %zu iwd, %zu ficheros indexados\n", names.size(), g_iwdIndex.size());
}
int FS_FOpenFileRead(const char *filename, int *file)
{
    if (!g_iwdInit) IwdInit();
    std::string k = filename; for (auto &c : k) { c = tolower((unsigned char)c); if (c == '\\') c = '/'; }
    auto mf = g_memFiles.find(k);
    if (mf != g_memFiles.end()) { OpenFile o{IwdEntry{-1, 0, (uint32_t)mf->second.size()}, 0, true}; o.mem = mf->second.data(); g_open.push_back(o); *file = (int)g_open.size() - 1; return (int)mf->second.size(); }
    auto it = g_iwdIndex.find(k); if (it == g_iwdIndex.end()) { *file = 0; return -1; }
    g_open.push_back(OpenFile{it->second, 0, true}); *file = (int)g_open.size() - 1; return (int)it->second.size;
}
unsigned int FS_FOpenFileReadCurrentThread(const char *filename, int *file) { return (unsigned)FS_FOpenFileRead(filename, file); }
unsigned int FS_Read(unsigned char *buffer, unsigned int len, int h)
{
    if (h <= 0 || h >= (int)g_open.size() || !g_open[h].used) return 0;
    OpenFile &f = g_open[h]; uint64_t rem = f.e.size - f.pos; unsigned n = (unsigned)std::min<uint64_t>(len, rem);
    if (f.mem) { memcpy(buffer, f.mem + f.pos, n); f.pos += n; return n; }
    ssize_t r = pread(f.e.fd, buffer, n, f.e.off + f.pos); if (r < 0) return 0; f.pos += r; return (unsigned)r;
}
void FS_FCloseFile(int h) { if (h > 0 && h < (int)g_open.size()) g_open[h].used = false; }
// H6a (h5_audio.cpp): localiza un fichero de los .iwd (fd, desplazamiento y tamano) para leerlo con pread desde el hilo de carga
// de streams. Se llama en el hilo principal; el indice ya no cambia despues de IwdInit.
bool FS_H6A_Locate(const char *filename, int *fd, unsigned long long *off, unsigned int *size)
{
    if (!g_iwdInit) IwdInit();
    std::string k = filename; for (auto &c : k) { c = tolower((unsigned char)c); if (c == '\\') c = '/'; }
    auto it = g_iwdIndex.find(k); if (it == g_iwdIndex.end()) return false;
    *fd = it->second.fd; *off = it->second.off; *size = it->second.size; return true;
}
