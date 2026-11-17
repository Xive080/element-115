#pragma once
#include <map>
#include <string>
#include <vector>
struct TraceDraw { unsigned prims; int vs, ps; std::string tex0; };
struct TraceStats {
    int presents = 0, beginScenes = 0, setViewports = 0, setTextures = 0, setVS = 0, setPS = 0, setDecl = 0, draws = 0, drawTris = 0, vsConsts = 0, psConsts = 0;
    int textures = 0, vbs = 0, ibs = 0, vshaders = 0, pshaders = 0, decls = 0;
    std::map<std::string, int> defaults, texNames, vsNames, psNames; std::vector<TraceDraw> drawList;
};
extern TraceStats g_ts;
extern const char *(*g_traceName)(int kind, const void *p);
struct TDev; struct IDirect3DDevice9;
IDirect3DDevice9 *TraceDevice_Create(unsigned bbw, unsigned bbh);
void d3dtrace_log(const char *fmt, ...);
void TraceDevice_ReleaseAll();   // [T12] libera en el backend todo recurso vivo (antes de g9::Shutdown: VMA exige 0 asignaciones vivas)
