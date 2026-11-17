// P3: IDirect3DDevice9 'TRACE' (sin GL): cada llamada relevante se registra en el log; Create*/Lock* con memoria CPU real.
// Las clases del shim son concretas no virtuales (d3d9_ifaces_trace.inc solo declara); aqui se definen los metodos con estado.
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include "trace.h"
// Backend grafico enlazado: -DD3D9_GLES (gles/d3d9_gles.cpp) o -DD3D9_VK (vk/libv9.a, [T12]); G9BE = hay un backend g9:: compilado.
// Con -DD3D9_VK y backend activo (OWNMEM) los recursos NO tienen copia CPU aqui: Lock/Unlock van a g9::BufLock/TexLock (memoria del backend).
#if defined(D3D9_GLES)
#include "../gles/d3d9_gles.h"
#define G9BE 1
#define GLON (g9::Active())
#define OWNMEM false
// [T12] El backend GLES no posee memoria ni refcount (OWNMEM es false): estos stubs solo dejan compilar las ramas "if (OWNMEM)" (nunca se ejecutan).
namespace g9 { struct Query;
inline void *BufLock(Buf *, UINT, UINT, DWORD) { return nullptr; } inline void BufUnlock(Buf *) {}
inline void *TexLock(Tex *, int, int, UINT *, UINT *, DWORD) { return nullptr; } inline void TexUnlock(Tex *, int, int) {}
inline void Release(Tex *) {} inline void Release(Buf *) {} inline void Release(Surf *) {} inline void Release(Shader *) {} inline void Release(Decl *) {} inline void Release(Query *) {}
inline Query *QueryCreate(int) { return nullptr; } inline void QueryIssue(Query *) {} inline bool QueryDone(Query *, bool) { return true; } }
#elif defined(D3D9_VK)
#include "../vk/g9_api.h"
namespace v9 { void PassSetDepthOnly(bool on); }   // vk_internal.h (no se incluye aqui: arrastra volk)
#define G9BE 1
#define GLON (g9::Active())
#define OWNMEM (g9::Active())
#else
#define GLON false
#define OWNMEM false
#endif

#ifdef H0_MACWIN
extern "C" void WinPump(void);
#endif
static FILE *g_tf; static int g_nid;
const char *(*g_traceName)(int kind, const void *p) = nullptr;   // main2: nombre de asset de un objeto D3D (kind 0=tex 1=vs 2=ps 3=decl)
TraceStats g_ts;
static void L(const char *fmt, ...)
{
    static int off = -1; if (off < 0) { const char *p = getenv("P3_TRACE"); off = p && !strcmp(p, "off"); } if (off) return;
    if (!g_tf) { const char *p = getenv("P3_TRACE"); g_tf = p ? fopen(p, "w") : stdout; if (!g_tf) g_tf = stdout; }
    va_list ap; va_start(ap, fmt); vfprintf(g_tf, fmt, ap); va_end(ap); fputc('\n', g_tf);
}
void d3dtrace_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); char b[1024]; vsnprintf(b, sizeof b, fmt, ap); va_end(ap); L("%s", b); }
void d3dtrace_default(const char *name) { g_ts.defaults[name]++; L("  [default] %s", name); }
// P3_TRACE=off (la app, las partidas largas): sin nombres de assets. NameOf (main2) re-enumera TODOS los assets cuando un objeto no
// esta en su mapa (texturas de codigo, render targets): con la traza apagada eso era CPU pura en cada draw/SetTexture. OLA 5.
static bool TraceOff() { static int off = -1; if (off < 0) { const char *p = getenv("P3_TRACE"); off = p && !strcmp(p, "off"); } return off != 0; }
static const char *NM(int k, const void *p) { return g_traceName && p && !TraceOff() ? g_traceName(k, p) : ""; }

// ---- objetos
struct TTex : IDirect3DTexture9 { void *g = nullptr; int id; UINT w, h, levels; DWORD usage; D3DFORMAT fmt; D3DPOOL pool; std::vector<std::vector<uint8_t>> mem; };
struct TCube : IDirect3DCubeTexture9 { void *g = nullptr; int id; UINT e, levels; DWORD usage = 0; D3DPOOL pool = D3DPOOL_MANAGED; D3DFORMAT fmt; std::vector<std::vector<uint8_t>> mem; };
struct TVol : IDirect3DVolumeTexture9 { void *g = nullptr; int id; UINT w, h, d, levels; D3DFORMAT fmt; std::vector<std::vector<uint8_t>> mem; };
struct TVB : IDirect3DVertexBuffer9 { void *g = nullptr; int id; UINT len; DWORD usage; std::vector<uint8_t> mem; };
struct TIB : IDirect3DIndexBuffer9 { bool inflight = false; void *g = nullptr; int id; UINT len; D3DFORMAT fmt; DWORD usage; std::vector<uint8_t> mem; };
static std::vector<TIB *> g_ibFlight; static double g_ghostMB = 0, g_dirtyMB = 0; static long g_ghostN = 0, g_lockN = 0;   // H1: reescrituras de IB en vuelo
struct TSurf : IDirect3DSurface9 { void *g = nullptr; int id; UINT w, h; D3DFORMAT fmt; DWORD usage; std::vector<uint8_t> mem; const char *what; IUnknown *parent = nullptr; };   // parent: textura de la que sale (GetSurfaceLevel); la superficie retiene una referencia
struct TVS : IDirect3DVertexShader9 { void *g = nullptr; int id; unsigned dwords; };
struct TPS : IDirect3DPixelShader9 { void *g = nullptr; int id; unsigned dwords; };
struct TDecl : IDirect3DVertexDeclaration9 { void *g = nullptr; int id; std::vector<D3DVERTEXELEMENT9> el; };
struct TQuery : IDirect3DQuery9 { void *g = nullptr; int id; D3DQUERYTYPE type = (D3DQUERYTYPE)0; };
struct TDev : IDirect3DDevice9 {
    TSurf *backbuf, *depth, *rt0, *ds; TTex *tex[16]; TVS *vs; TPS *ps; TDecl *decl; TVB *vb[4]; UINT vbOff[4], vbStride[4]; TIB *ib;
    D3DVIEWPORT9 vp; bool inScene; int frame;
};
static TDev *D;

// ---- [T12] refcount y liberacion hacia el backend (solo con OWNMEM): IUnknown::AddRef/Release (compat/Windows.h) llaman a estos ganchos.
// El motor hace AddRef de alias (r_image.cpp:743, r_rendertarget.cpp:251) y Release de todo (RB_Resource_Release): al llegar a 0 se desengancha del
// estado del dispositivo y del backend (como D3D9, que mantiene referencias internas) y se libera con g9::Release (destruccion diferida).
enum { K_TEX, K_CUBE, K_VOL, K_VB, K_IB, K_SURF, K_VS, K_PS, K_DECL, K_QUERY };
struct RegEnt { int kind, rc; void *obj; };
static std::unordered_map<const void *, RegEnt> g_reg;
static long g_relCount[10], g_relUnknown;
ULONG (*g_comAddRefHook)(void *) = nullptr; ULONG (*g_comReleaseHook)(void *) = nullptr;
static const void *RegKey(IUnknown *u) { return (const void *)u; }
template <class T> static T *RegAdd(T *o, int kind, int rc = 1) { if (OWNMEM) g_reg[RegKey(static_cast<IUnknown *>(o))] = { kind, rc, (void *)o }; return o; }
static void RegAddRef(IUnknown *u) { if (!OWNMEM) return; auto it = g_reg.find(RegKey(u)); if (it != g_reg.end()) ++it->second.rc; }
static ULONG ComAddRef(void *p) { auto it = g_reg.find(p); if (it == g_reg.end()) return 1; return (ULONG)++it->second.rc; }
static void DestroyObj(const RegEnt &e);
static ULONG ComRelease(void *p)
{
    auto it = g_reg.find(p); if (it == g_reg.end()) { ++g_relUnknown; return 0; }
    if (--it->second.rc > 0) return (ULONG)it->second.rc;
    RegEnt e = it->second; g_reg.erase(it); ++g_relCount[e.kind]; DestroyObj(e); return 0;
}

static void DestroyObj(const RegEnt &e)
{
#ifdef G9BE
    switch (e.kind) {
    case K_TEX: { TTex *t = (TTex *)e.obj; for (int i = 0; i < 16; ++i) if (D && D->tex[i] == t) { D->tex[i] = nullptr; g9::SetTexture(i, nullptr); } g9::Release((g9::Tex *)t->g); delete t; break; }
    case K_CUBE: { TCube *t = (TCube *)e.obj; for (int i = 0; i < 16; ++i) if (D && D->tex[i] && (void *)D->tex[i] == (void *)t) { D->tex[i] = nullptr; g9::SetTexture(i, nullptr); } g9::Release((g9::Tex *)t->g); delete t; break; }
    case K_VOL: { TVol *t = (TVol *)e.obj; for (int i = 0; i < 16; ++i) if (D && D->tex[i] && (void *)D->tex[i] == (void *)t) { D->tex[i] = nullptr; g9::SetTexture(i, nullptr); } g9::Release((g9::Tex *)t->g); delete t; break; }
    case K_VB: { TVB *b = (TVB *)e.obj; for (int i = 0; i < 4; ++i) if (D && D->vb[i] == b) { D->vb[i] = nullptr; g9::SetStream(i, nullptr, 0, 0); } g9::Release((g9::Buf *)b->g); delete b; break; }
    case K_IB: { TIB *b = (TIB *)e.obj; if (D && D->ib == b) { D->ib = nullptr; g9::SetIndices(nullptr); } for (size_t i = 0; i < g_ibFlight.size(); ++i) if (g_ibFlight[i] == b) { g_ibFlight.erase(g_ibFlight.begin() + i); break; } g9::Release((g9::Buf *)b->g); delete b; break; }
    case K_SURF: { TSurf *s = (TSurf *)e.obj; if (D && D->rt0 == s) D->rt0 = D->backbuf; if (D && D->ds == s) D->ds = D->depth; if (s->g) g9::Release((g9::Surf *)s->g); IUnknown *par = s->parent; delete s; if (par) ComRelease((void *)par); break; }
    case K_VS: { TVS *x = (TVS *)e.obj; if (D && D->vs == x) { D->vs = nullptr; g9::SetVS(nullptr); } if (x->g) g9::Release((g9::Shader *)x->g); delete x; break; }
    case K_PS: { TPS *x = (TPS *)e.obj; if (D && D->ps == x) { D->ps = nullptr; g9::SetPS(nullptr); } if (x->g) g9::Release((g9::Shader *)x->g); delete x; break; }
    case K_DECL: { TDecl *x = (TDecl *)e.obj; if (D && D->decl == x) { D->decl = nullptr; g9::SetDecl(nullptr); } if (x->g) g9::Release((g9::Decl *)x->g); delete x; break; }
    case K_QUERY: { TQuery *q = (TQuery *)e.obj; if (q->g) g9::Release((g9::Query *)q->g); delete q; break; }
    }
#endif
}

// [T12] Autoprueba del camino de liberacion (T12_RELTEST=1, solo OWNMEM): a mitad de ejecucion (2.o Present, justo antes de g9::Present) crea recursos, los engancha al
// estado, hace AddRef de alias y los libera por COM como el motor; comprueba desenganche del estado y destruccion diferida (los frames siguientes siguen renderizando).
static void T12_SelfTest()
{
    static bool done = false; static int calls = 0; if (done || !OWNMEM || !getenv("T12_RELTEST") || !D || ++calls < 2) return; done = true;
    long k0[10]; memcpy(k0, g_relCount, sizeof k0); int bad = 0;
    IDirect3DTexture9 *t = nullptr; D->CreateTexture(64, 64, 1, 0, (D3DFORMAT)22, (D3DPOOL)1, &t, nullptr);
    D->SetTexture(0, t); IDirect3DSurface9 *sf = nullptr; t->GetSurfaceLevel(0, &sf);   // sf retiene la textura (rc=2)
    t->Release(); if (!g_reg.count((void *)(IUnknown *)static_cast<TTex *>(t))) { ++bad; fprintf(stderr, "[T12] SELFTEST: la textura murio con alias vivo\n"); }
    sf->Release(); if (D->tex[0] != nullptr || g_reg.count((void *)(IUnknown *)static_cast<TTex *>(t))) { ++bad; fprintf(stderr, "[T12] SELFTEST: la textura no se destruyo/desengancho\n"); }
    IDirect3DVertexBuffer9 *vb = nullptr; D->CreateVertexBuffer(256, 8, 0, (D3DPOOL)0, &vb, nullptr); D->SetStreamSource(0, vb, 0, 16);
    IDirect3DIndexBuffer9 *ib = nullptr; D->CreateIndexBuffer(128, 8, (D3DFORMAT)101, (D3DPOOL)0, &ib, nullptr); D->SetIndices(ib);
    vb->AddRef(); vb->Release(); if (D->vb[0] == nullptr) { ++bad; fprintf(stderr, "[T12] SELFTEST: VB desenganchado con rc>0\n"); }
    vb->Release(); ib->Release(); if (D->vb[0] != nullptr || D->ib != nullptr) { ++bad; fprintf(stderr, "[T12] SELFTEST: VB/IB siguen enganchados\n"); }
    IDirect3DQuery9 *q = nullptr; D->CreateQuery((D3DQUERYTYPE)8, &q); q->Issue(1); q->Release();
    fprintf(stderr, "[T12] SELFTEST (Present n.%d): tex+%ld surf+%ld vb+%ld ib+%ld query+%ld, fallos=%d\n", calls, g_relCount[K_TEX] - k0[K_TEX], g_relCount[K_SURF] - k0[K_SURF], g_relCount[K_VB] - k0[K_VB], g_relCount[K_IB] - k0[K_IB], g_relCount[K_QUERY] - k0[K_QUERY], bad);
}
void TraceDevice_ReleaseAll()
{   // el registro se vacia ANTES de destruir (DestroyObj de una superficie libera su textura padre por ComRelease: no debe encontrarla)
    if (!OWNMEM && g_reg.empty()) return;
    std::vector<RegEnt> all; all.reserve(g_reg.size()); for (auto &kv : g_reg) all.push_back(kv.second); g_reg.clear();
    long n = 0, perm = 0;
    for (auto &e : all) { if (e.kind == K_SURF && e.rc >= (1 << 19)) { ++perm; continue; } DestroyObj(e); ++n; }   // backbuffer/depth del dispositivo: son del backend
    fprintf(stderr, "[T12] ReleaseAll: %ld recursos liberados al cerrar (+%ld permanentes); liberados por el motor: tex=%ld cubo=%ld vol=%ld vb=%ld ib=%ld surf=%ld vs=%ld ps=%ld decl=%ld query=%ld (Release desconocidos=%ld)\n",
            n, perm, g_relCount[K_TEX], g_relCount[K_CUBE], g_relCount[K_VOL], g_relCount[K_VB], g_relCount[K_IB], g_relCount[K_SURF], g_relCount[K_VS], g_relCount[K_PS], g_relCount[K_DECL], g_relCount[K_QUERY], g_relUnknown);
}

static unsigned FmtBlk(D3DFORMAT f, unsigned *bpp)   // bytes por bloque de 4x4 (DXT) o 0 + bpp por pixel
{
    *bpp = 4;
    switch ((unsigned)f) {
    case 0x31545844: return 8; case 0x32545844: case 0x33545844: case 0x34545844: case 0x35545844: return 16;
    case 28: case 50: *bpp = 1; break; case 23: case 51: case 60: case 80: case 81: case 25: case 26: case 111: *bpp = 2; break;
    case 36: case 113: *bpp = 8; break; case 116: *bpp = 16; break;
    }
    return 0;
}
static unsigned LevelBytes(D3DFORMAT f, UINT w, UINT h, UINT *pitch)
{
    unsigned bpp, blk = FmtBlk(f, &bpp); w = w ? w : 1; h = h ? h : 1;
    if (blk) { unsigned bw = (w + 3) / 4, bh = (h + 3) / 4; *pitch = bw * blk; return bw * bh * blk; }
    *pitch = w * bpp; return w * h * bpp;
}
static const char *FmtName(D3DFORMAT f)
{
    switch ((unsigned)f) { case 21: return "A8R8G8B8"; case 22: return "X8R8G8B8"; case 23: return "R5G6B5"; case 28: return "A8"; case 50: return "L8"; case 51: return "A8L8";
    case 0x31545844: return "DXT1"; case 0x32545844: return "DXT2"; case 0x33545844: return "DXT3"; case 0x34545844: return "DXT4"; case 0x35545844: return "DXT5";
    case 113: return "A16B16G16R16F"; case 116: return "A32B32G32R32F"; case 111: return "R16F"; case 112: return "G16R16F"; case 114: return "R32F"; case 75: return "D24S8"; case 77: return "D24X8"; case 80: return "D16"; case 60: return "V8U8"; case 63: return "Q8W8V8U8"; }
    static char b[8][16]; static int i; snprintf(b[i & 7], 16, "fmt%u", (unsigned)f); return b[i++ & 7];
}
static unsigned LevelCount(UINT w, UINT h, UINT lv) { if (lv) return lv; unsigned n = 1; while (w > 1 || h > 1) { w >>= 1; h >>= 1; ++n; } return n; }
static void AllocLevels(std::vector<std::vector<uint8_t>> &mem, D3DFORMAT f, UINT w, UINT h, unsigned lv)
{ mem.resize(lv); for (unsigned i = 0; i < lv; ++i) { UINT p; mem[i].assign(LevelBytes(f, w >> i, h >> i, &p), 0); } }
static const char *PrimName(D3DPRIMITIVETYPE t) { static const char *n[] = { "?", "POINTLIST", "LINELIST", "LINESTRIP", "TRILIST", "TRISTRIP", "TRIFAN" }; return (unsigned)t < 7 ? n[t] : "?"; }
static const char *TexTag(const void *p) { return p ? NM(0, p) : "(null)"; }

// ---- IDirect3D9
HRESULT IDirect3D9::GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS9 *c) { memset(c, 0, sizeof *c); c->MaxTextureWidth = c->MaxTextureHeight = 4096; c->MaxUserClipPlanes = 6; c->MaxAnisotropy = 16; c->Caps2 = 0x20000; c->TextureCaps = 0x10000;
    // OLA 7g: D3DPTFILTERCAPS_MINFANISOTROPIC|MAGFANISOTROPIC (como cualquier GPU de PC): sin esto R_SetTexFilter nunca usa filtro anisotropico
    // (gfxMetrics.hasAnisotropicMin/MagFilter) y los suelos/paredes oblicuos se emborronan. H7_ANISO=0 vuelve a lo anterior.
    { const char *e = getenv("H7_ANISO"); if (!(e && !atoi(e))) c->TextureFilterCaps = 0x400 | 0x4000000; }
    return 0; }

// ---- IDirect3DDevice9: creacion
IDirect3DDevice9 *TraceDevice_Create(unsigned bbw, unsigned bbh)
{
    D = new TDev(); memset((void *)((char *)D + sizeof(IDirect3DDevice9)), 0, sizeof(TDev) - sizeof(IDirect3DDevice9));
    D->backbuf = new TSurf(); D->backbuf->id = ++g_nid; D->backbuf->w = bbw; D->backbuf->h = bbh; D->backbuf->fmt = (D3DFORMAT)22; D->backbuf->what = "backbuffer";
    D->depth = new TSurf(); D->depth->id = ++g_nid; D->depth->w = bbw; D->depth->h = bbh; D->depth->fmt = (D3DFORMAT)75; D->depth->what = "depthstencil";
    #ifdef G9BE
    if (GLON) { D->backbuf->g = g9::SurfBackbuffer(false); D->depth->g = g9::SurfBackbuffer(true); }
#endif
    D->rt0 = D->backbuf; D->ds = D->depth;
#ifdef D3D9_VK
    // [T12] D3D9 con EnableAutoDepthStencil: el depth del dispositivo queda ligado desde el principio y el motor NUNCA llama a SetDepthStencilSurface para el
    // frame buffer (verificado en la traza). En GLES lo daba el framebuffer por defecto; en el backend Vulkan sin esto el pass no tiene depth.
    if (GLON && D->depth->g) g9::SetDepthStencil((g9::Surf *)D->depth->g);
#endif
    if (OWNMEM) { RegAdd(D->backbuf, K_SURF, 1 << 20); RegAdd(D->depth, K_SURF, 1 << 20); g_comAddRefHook = ComAddRef; g_comReleaseHook = ComRelease; }   // el backbuffer vive hasta g9::Shutdown
    L("Device TRACE creado: backbuffer %ux%u", bbw, bbh);
    return D;
}
#define DV static_cast<TDev *>(this)
HRESULT IDirect3DDevice9::TestCooperativeLevel() { return 0; }
UINT IDirect3DDevice9::GetAvailableTextureMem() { return 512u << 20; }
HRESULT IDirect3DDevice9::GetDeviceCaps(D3DCAPS9 *c) { return IDirect3D9().GetDeviceCaps(0, D3DDEVTYPE_HAL, c); }
HRESULT IDirect3DDevice9::GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **pp) { *pp = DV->backbuf; RegAddRef(DV->backbuf); return 0; }
HRESULT IDirect3DDevice9::Present(const RECT *, const RECT *, HWND, const RGNDATA *) { L("Present  (frame %d)", DV->frame++); ++g_ts.presents;
    if (g_lockN) { fprintf(stderr, "[H1] frame: draws=%d locks IB=%ld (%.1f MB marcados) locks sobre IB en vuelo=%ld => ghost potencial=%.0f MB (len del buffer por lock)\n", g_ts.draws, g_lockN, g_dirtyMB, g_ghostN, g_ghostMB); } g_lockN = g_ghostN = 0; g_ghostMB = g_dirtyMB = 0; for (TIB *b : g_ibFlight) b->inflight = false; g_ibFlight.clear();
#ifdef G9BE
    T12_SelfTest(); if (GLON) g9::Present();
#endif
#ifdef H0_MACWIN
    WinPump();   // [T12] eventos de la ventana Cocoa (P4_WINDOW=1)
#endif
    return 0; }

template <class T> static void SetPP(T **pp, T *v) { *pp = v; }
static void NewTexture(TTex *t, UINT w, UINT h, UINT lv, DWORD usage, D3DFORMAT f, D3DPOOL pool)
{
    t->id = ++g_nid; t->w = w; t->h = h; t->usage = usage; t->fmt = f; t->pool = pool; t->levels = LevelCount(w, h, lv);
    if (!OWNMEM) AllocLevels(t->mem, f, w, h, t->levels); ++g_ts.textures;
#ifdef G9BE
    if (GLON) t->g = g9::TexCreate(0, w, h, 1, t->levels, usage, f, OWNMEM ? nullptr : &t->mem);
#endif
    RegAdd(t, K_TEX);
    L("CreateTexture tex#%d %ux%u levels=%u fmt=%s usage=0x%x pool=%d", t->id, w, h, t->levels, FmtName(f), (unsigned)usage, (int)pool);
}
HRESULT IDirect3DDevice9::CreateTexture(UINT w, UINT h, UINT lv, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9 **pp, HANDLE *) { TTex *t = new TTex(); NewTexture(t, w, h, lv, u, f, p); *pp = t; return 0; }
HRESULT IDirect3DDevice9::CreateTexture(UINT w, UINT h, UINT lv, DWORD u, D3DFORMAT f, D3DPOOL p, PtrRt32<IDirect3DTexture9> *pp, HANDLE *) { TTex *t = new TTex(); NewTexture(t, w, h, lv, u, f, p); *pp = t; return 0; }
HRESULT IDirect3DDevice9::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT lv, DWORD, D3DFORMAT f, D3DPOOL, PtrRt32<IDirect3DVolumeTexture9> *pp, HANDLE *)
{ TVol *t = new TVol(); t->id = ++g_nid; t->w = w; t->h = h; t->d = d; t->fmt = f; t->levels = lv ? lv : 1; UINT p; if (!OWNMEM) { t->mem.resize(t->levels); for (unsigned i = 0; i < t->levels; ++i) t->mem[i].assign(LevelBytes(f, w >> i, h >> i, &p) * std::max(1u, d >> i), 0); } L("CreateVolumeTexture tex#%d %ux%ux%u fmt=%s", t->id, w, h, d, FmtName(f));
#ifdef G9BE
    if (GLON) t->g = g9::TexCreate(1, w, h, d, t->levels, 0, f, OWNMEM ? nullptr : &t->mem);
#endif
    RegAdd(t, K_VOL); *pp = t; return 0; }
HRESULT IDirect3DDevice9::CreateCubeTexture(UINT e, UINT lv, DWORD cu, D3DFORMAT f, D3DPOOL pool, PtrRt32<IDirect3DCubeTexture9> *pp, HANDLE *)
{ TCube *t = new TCube(); t->id = ++g_nid; t->e = e; t->fmt = f; t->usage = cu; t->pool = pool; t->levels = LevelCount(e, e, lv); if (!OWNMEM) { t->mem.resize(t->levels * 6); UINT p; for (unsigned i = 0; i < t->levels * 6; ++i) t->mem[i].assign(LevelBytes(f, e >> (i % t->levels), e >> (i % t->levels), &p), 0); } L("CreateCubeTexture tex#%d edge=%u fmt=%s", t->id, e, FmtName(f));
#ifdef G9BE
    if (GLON) t->g = g9::TexCreate(2, e, e, 1, t->levels, cu, f, OWNMEM ? nullptr : &t->mem);
#endif
    RegAdd(t, K_CUBE); *pp = t; return 0; }
static TVB *NewVB(UINT len, DWORD u) { TVB *b = new TVB(); b->id = ++g_nid; b->len = len; b->usage = u; if (!OWNMEM) b->mem.assign(len, 0); ++g_ts.vbs;
#ifdef G9BE
    if (GLON) b->g = g9::BufCreate(false, len, u, OWNMEM ? nullptr : b->mem.data(), D3DFMT_UNKNOWN);
#endif
    RegAdd(b, K_VB);
    L("CreateVertexBuffer vb#%d len=%u usage=0x%x", b->id, len, (unsigned)u); return b; }
HRESULT IDirect3DDevice9::CreateVertexBuffer(UINT len, DWORD u, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **pp, HANDLE *) { *pp = NewVB(len, u); return 0; }
HRESULT IDirect3DDevice9::CreateVertexBuffer(UINT len, DWORD u, DWORD, D3DPOOL, PtrRt32<IDirect3DVertexBuffer9> *pp, HANDLE *) { *pp = NewVB(len, u); return 0; }
static TIB *NewIB(UINT len, D3DFORMAT f, DWORD u) { TIB *b = new TIB(); b->id = ++g_nid; b->len = len; b->fmt = f; b->usage = u; if (!OWNMEM) b->mem.assign(len, 0); ++g_ts.ibs;
#ifdef G9BE
    if (GLON) b->g = g9::BufCreate(true, len, u, OWNMEM ? nullptr : b->mem.data(), f);
#endif
    RegAdd(b, K_IB);
    L("CreateIndexBuffer ib#%d len=%u fmt=%s usage=0x%x", b->id, len, (unsigned)f == 101 ? "I16" : "I32", (unsigned)u); return b; }
HRESULT IDirect3DDevice9::CreateIndexBuffer(UINT len, DWORD u, D3DFORMAT f, D3DPOOL, IDirect3DIndexBuffer9 **pp, HANDLE *) { *pp = NewIB(len, f, u); return 0; }
HRESULT IDirect3DDevice9::CreateIndexBuffer(UINT len, DWORD u, D3DFORMAT f, D3DPOOL, PtrRt32<IDirect3DIndexBuffer9> *pp, HANDLE *) { *pp = NewIB(len, f, u); return 0; }
static TSurf *NewSurf(UINT w, UINT h, D3DFORMAT f, const char *what) { TSurf *s = new TSurf(); s->id = ++g_nid; s->w = w; s->h = h; s->fmt = f; s->what = what;
#ifdef G9BE
    if (GLON && (!strcmp(what, "RenderTarget") || !strcmp(what, "DepthStencilSurface"))) s->g = g9::SurfCreate(w, h, f, !strcmp(what, "DepthStencilSurface"));
#endif
    RegAdd(s, K_SURF);
    L("Create%s surf#%d %ux%u fmt=%s", what, s->id, w, h, FmtName(f)); return s; }
HRESULT IDirect3DDevice9::CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE, DWORD, WINBOOL, IDirect3DSurface9 **pp, HANDLE *) { *pp = NewSurf(w, h, f, "RenderTarget"); return 0; }
HRESULT IDirect3DDevice9::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE, DWORD, WINBOOL, IDirect3DSurface9 **pp, HANDLE *) { *pp = NewSurf(w, h, f, "DepthStencilSurface"); return 0; }
HRESULT IDirect3DDevice9::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f, D3DPOOL, IDirect3DSurface9 **pp, HANDLE *) { *pp = NewSurf(w, h, f, "OffscreenSurface"); return 0; }
static HRESULT NewVS(const DWORD *bc, IDirect3DVertexShader9 **out) { TVS *s = new TVS(); s->id = ++g_nid; unsigned n = 0; if (bc) { while (bc[n] != 0x0000FFFF && n < 65536) ++n; ++n; } s->dwords = n; ++g_ts.vshaders;
#ifdef G9BE
    if (GLON) s->g = g9::ShaderCreate(true, bc, n);
#endif
    RegAdd(s, K_VS); L("CreateVertexShader vs#%d bytecode=%u bytes", s->id, n * 4); *out = s; return 0; }
HRESULT IDirect3DDevice9::CreateVertexShader(const DWORD *bc, IDirect3DVertexShader9 **pp) { return NewVS(bc, pp); }
HRESULT IDirect3DDevice9::CreateVertexShader(const DWORD *bc, PtrRt32<IDirect3DVertexShader9> *pp) { IDirect3DVertexShader9 *s; NewVS(bc, &s); *pp = s; return 0; }
static HRESULT NewPS(const DWORD *bc, IDirect3DPixelShader9 **out) { TPS *s = new TPS(); s->id = ++g_nid; unsigned n = 0; if (bc) { while (bc[n] != 0x0000FFFF && n < 65536) ++n; ++n; } s->dwords = n; ++g_ts.pshaders;
#ifdef G9BE
    if (GLON) s->g = g9::ShaderCreate(false, bc, n);
#endif
    RegAdd(s, K_PS); L("CreatePixelShader ps#%d bytecode=%u bytes", s->id, n * 4); *out = s; return 0; }
HRESULT IDirect3DDevice9::CreatePixelShader(const DWORD *bc, IDirect3DPixelShader9 **pp) { return NewPS(bc, pp); }
HRESULT IDirect3DDevice9::CreatePixelShader(const DWORD *bc, PtrRt32<IDirect3DPixelShader9> *pp) { IDirect3DPixelShader9 *s; NewPS(bc, &s); *pp = s; return 0; }
static TDecl *NewDecl(const D3DVERTEXELEMENT9 *e) { TDecl *d = new TDecl(); d->id = ++g_nid; for (; e->Stream != 0xFF; ++e) d->el.push_back(*e); ++g_ts.decls;
#ifdef G9BE
    if (GLON) d->g = g9::DeclCreate(d->el.data(), (unsigned)d->el.size());
#endif
    RegAdd(d, K_DECL); L("CreateVertexDeclaration decl#%d elements=%zu", d->id, d->el.size()); return d; }
HRESULT IDirect3DDevice9::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **pp) { *pp = NewDecl(e); return 0; }
HRESULT IDirect3DDevice9::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *e, PtrRt32<IDirect3DVertexDeclaration9> *pp) { *pp = NewDecl(e); return 0; }
HRESULT IDirect3DDevice9::CreateQuery(D3DQUERYTYPE t, IDirect3DQuery9 **pp) { TQuery *q = new TQuery(); q->id = ++g_nid; q->type = t;
#ifdef G9BE
    if (OWNMEM) q->g = g9::QueryCreate((int)t);
#endif
    RegAdd(q, K_QUERY); *pp = q; return 0; }
HRESULT IDirect3DQuery9::Issue(DWORD) {
#ifdef G9BE
    if (OWNMEM) { TQuery *q = static_cast<TQuery *>(this); if (q->g) g9::QueryIssue((g9::Query *)q->g); }
#endif
    return 0; }
// EVENT: S_OK cuando la GPU paso el punto marcado, S_FALSE (1) si no (D3DGETDATA_FLUSH=1 fuerza un submit parcial). OCCLUSION: el contador no se mide (se devuelve 1 pixel visible, como antes).
HRESULT IDirect3DQuery9::GetData(void *p, DWORD sz, DWORD fl) { if (p && sz) memset(p, 0, sz); if (p && sz >= 4) *(DWORD *)p = 1;
#ifdef G9BE
    if (OWNMEM) { TQuery *q = static_cast<TQuery *>(this); if (q->g && !g9::QueryDone((g9::Query *)q->g, (fl & 1) != 0)) return 1; }
#endif
    return 0; }

// ---- locks
HRESULT IDirect3DTexture9::LockRect(UINT lv, D3DLOCKED_RECT *lr, const RECT *, DWORD fl) { TTex *t = static_cast<TTex *>(this); if (lv >= t->levels) return D3DERR_INVALIDCALL;
#ifdef G9BE
    if (OWNMEM) { UINT rp = 0, sp = 0; void *m = g9::TexLock((g9::Tex *)t->g, 0, (int)lv, &rp, &sp, fl); if (!m) return D3DERR_INVALIDCALL; lr->Pitch = rp; lr->pBits = m; return 0; }
#endif
    UINT p; LevelBytes(t->fmt, t->w >> lv, t->h >> lv, &p); lr->Pitch = p; lr->pBits = t->mem[lv].data(); return 0; }
HRESULT IDirect3DTexture9::UnlockRect(UINT lv) {
#ifdef G9BE
    if (OWNMEM) g9::TexUnlock((g9::Tex *)static_cast<TTex *>(this)->g, 0, (int)lv); else
    if (GLON) g9::TexDirty((g9::Tex *)static_cast<TTex *>(this)->g, 0, lv);
#endif
    return 0; }
DWORD IDirect3DTexture9::GetLevelCount() { return static_cast<TTex *>(this)->levels; }
HRESULT IDirect3DTexture9::GetLevelDesc(UINT lv, D3DSURFACE_DESC *d) { TTex *t = static_cast<TTex *>(this); memset(d, 0, sizeof *d); d->Format = t->fmt; d->Width = t->w >> lv ? t->w >> lv : 1; d->Height = t->h >> lv ? t->h >> lv : 1; d->Usage = t->usage; d->Pool = t->pool; return 0; }
HRESULT IDirect3DTexture9::GetSurfaceLevel(UINT lv, IDirect3DSurface9 **pp) { TTex *t = static_cast<TTex *>(this); TSurf *s = new TSurf(); s->id = ++g_nid; s->w = t->w >> lv ? t->w >> lv : 1; s->h = t->h >> lv ? t->h >> lv : 1; s->fmt = t->fmt; s->what = "TexSurface";
#ifdef G9BE
    if (GLON) s->g = g9::SurfFromTex((g9::Tex *)t->g, 0, lv, s->w, s->h);
#endif
    RegAdd(s, K_SURF); if (OWNMEM) { s->parent = static_cast<IUnknown *>(t); RegAddRef(s->parent); } *pp = s; return 0; }
HRESULT IDirect3DSurface9::GetDesc(D3DSURFACE_DESC *d) { TSurf *s = static_cast<TSurf *>(this); memset(d, 0, sizeof *d); d->Format = s->fmt; d->Width = s->w; d->Height = s->h; return 0; }
HRESULT IDirect3DSurface9::LockRect(D3DLOCKED_RECT *lr, const RECT *, DWORD) { TSurf *s = static_cast<TSurf *>(this); UINT p; unsigned n = LevelBytes(s->fmt, s->w, s->h, &p); if (s->mem.size() < n) s->mem.assign(n, 0); lr->Pitch = p; lr->pBits = s->mem.data(); return 0; }
HRESULT IDirect3DSurface9::UnlockRect() { return 0; }
#ifdef G9BE
// [T12] las que eran "default" (stub) en la traza; solo con backend Vulkan activo (el camino GLES no las tenia: se queda igual)
#ifdef D3D9_VK
static g9::Surf *SG(IDirect3DSurface9 *s) { return s ? (g9::Surf *)static_cast<TSurf *>(s)->g : nullptr; }
HRESULT IDirect3DDevice9::StretchRect(IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const RECT *dr, D3DTEXTUREFILTERTYPE filter)
{ if (!GLON) { d3dtrace_default("IDirect3DDevice9::StretchRect"); return 0; } if (SG(src) && SG(dst)) g9::StretchRect(SG(src), sr, SG(dst), dr, (DWORD)filter); else d3dtrace_default("StretchRect(sin superficie g9)"); return 0; }
HRESULT IDirect3DDevice9::GetRenderTargetData(IDirect3DSurface9 *rt, IDirect3DSurface9 *dst)
{   // rt = superficie de GPU; dst = superficie de sistema (OffscreenPlainSurface): su memoria CPU la gestiona TSurf::mem
    if (!GLON) { d3dtrace_default("IDirect3DDevice9::GetRenderTargetData"); return 0; }
    TSurf *d = static_cast<TSurf *>(dst); if (!SG(rt) || !d) return D3DERR_INVALIDCALL;
    UINT p; unsigned n = LevelBytes(d->fmt, d->w, d->h, &p); if (d->mem.size() < n) d->mem.assign(n, 0);
    return g9::GetRenderTargetData(SG(rt), d->mem.data(), p) ? 0 : D3DERR_INVALIDCALL; }
HRESULT IDirect3DDevice9::UpdateSurface(IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const POINT *dp)
{   // origen de sistema (sin Surf g9): no hay camino de subida en el contrato; pendiente (1 uso en el motor, rb_backend.cpp:862)
    if (!GLON) { d3dtrace_default("IDirect3DDevice9::UpdateSurface"); return 0; }
    if (SG(src) && SG(dst)) { g9::UpdateSurface(SG(src), sr, SG(dst), dp); return 0; }
    d3dtrace_default("UpdateSurface(origen de sistema: no soportado)"); return 0; }
#endif
#endif
HRESULT IDirect3DVertexBuffer9::Lock(UINT off, UINT sz, void **pp, DWORD fl) { TVB *b = static_cast<TVB *>(this);
#ifdef G9BE
    if (OWNMEM) { *pp = g9::BufLock((g9::Buf *)b->g, off, sz, fl); return *pp ? 0 : D3DERR_INVALIDCALL; }
#endif
    *pp = b->mem.data() + off;
#ifdef G9BE
    if (GLON && !(fl & D3DLOCK_READONLY)) g9::BufDirty((g9::Buf *)b->g, off, sz, fl);
#endif
    return 0; }
HRESULT IDirect3DVertexBuffer9::Unlock() {
#ifdef G9BE
    if (OWNMEM) g9::BufUnlock((g9::Buf *)static_cast<TVB *>(this)->g);
#endif
    return 0; }
HRESULT IDirect3DVertexBuffer9::GetDesc(D3DVERTEXBUFFER_DESC *d) { TVB *b = static_cast<TVB *>(this); memset(d, 0, sizeof *d); d->Size = b->len; d->Usage = b->usage; return 0; }
HRESULT IDirect3DIndexBuffer9::Lock(UINT off, UINT sz, void **pp, DWORD fl) { TIB *b = static_cast<TIB *>(this);
#ifdef G9BE
    if (OWNMEM) { if (!(fl & D3DLOCK_READONLY) && b->inflight) { ++g_ghostN; } *pp = g9::BufLock((g9::Buf *)b->g, off, sz, fl); return *pp ? 0 : D3DERR_INVALIDCALL; }
#endif
    *pp = b->mem.data() + off;
    if (!(fl & D3DLOCK_READONLY)) { ++g_lockN; g_dirtyMB += (sz ? sz : b->len - off) / 1048576.0; if (b->inflight) { ++g_ghostN; g_ghostMB += b->len / 1048576.0; if (!(fl & (D3DLOCK_NOOVERWRITE | D3DLOCK_DISCARD))) { static int w = 0; if (w++ < 3) fprintf(stderr, "[H1] Lock de IB en vuelo SIN NOOVERWRITE/DISCARD (fl=0x%x)\n", (unsigned)fl); } } }
#ifdef G9BE
    if (GLON && !(fl & D3DLOCK_READONLY)) g9::BufDirty((g9::Buf *)b->g, off, sz, fl);
#endif
    return 0; }
HRESULT IDirect3DIndexBuffer9::Unlock() {
#ifdef G9BE
    if (OWNMEM) g9::BufUnlock((g9::Buf *)static_cast<TIB *>(this)->g);
#endif
    return 0; }
HRESULT IDirect3DIndexBuffer9::GetDesc(D3DINDEXBUFFER_DESC *d) { TIB *b = static_cast<TIB *>(this); memset(d, 0, sizeof *d); d->Size = b->len; d->Format = b->fmt; d->Usage = b->usage; return 0; }
HRESULT IDirect3DVolumeTexture9::LockBox(UINT lv, D3DLOCKED_BOX *lb, const D3DBOX *, DWORD fl) { TVol *t = static_cast<TVol *>(this);
#ifdef G9BE
    if (OWNMEM) { UINT rp = 0, sp = 0; void *m = g9::TexLock((g9::Tex *)t->g, 0, (int)lv, &rp, &sp, fl); if (!m) return D3DERR_INVALIDCALL; lb->RowPitch = rp; lb->SlicePitch = sp; lb->pBits = m; return 0; }
#endif
    UINT p; LevelBytes(t->fmt, t->w >> lv, t->h >> lv, &p); lb->RowPitch = p; lb->SlicePitch = LevelBytes(t->fmt, t->w >> lv, t->h >> lv, &p); lb->pBits = t->mem[lv].data(); return 0; }
HRESULT IDirect3DVolumeTexture9::UnlockBox(UINT lv) {
#ifdef G9BE
    if (OWNMEM) g9::TexUnlock((g9::Tex *)static_cast<TVol *>(this)->g, 0, (int)lv); else
    if (GLON) g9::TexDirty((g9::Tex *)static_cast<TVol *>(this)->g, 0, lv);
#endif
    return 0; }
HRESULT IDirect3DCubeTexture9::LockRect(D3DCUBEMAP_FACES f, UINT lv, D3DLOCKED_RECT *lr, const RECT *, DWORD fl) { TCube *t = static_cast<TCube *>(this);
#ifdef G9BE
    if (OWNMEM) { UINT rp = 0, sp = 0; void *m = g9::TexLock((g9::Tex *)t->g, (int)f, (int)lv, &rp, &sp, fl); if (!m) return D3DERR_INVALIDCALL; lr->Pitch = rp; lr->pBits = m; return 0; }
#endif
    UINT p; LevelBytes(t->fmt, t->e >> lv, t->e >> lv, &p); lr->Pitch = p; lr->pBits = t->mem[f * t->levels + lv].data(); return 0; }
// OLA 15F: consultas de cubo con el comportamiento D3D9 (antes eran los weak por defecto: 0 niveles, descripcion vacia, sin superficie).
// GetCubeMapSurface da la superficie (cara, nivel) como GetSurfaceLevel: con backend vk es render target si el cubo se creo con
// D3DUSAGE_RENDERTARGET (g9::SurfFromTex, vista 2D de una capa) -> sirve para pintar las 6 caras de un reflejo con SetRenderTarget.
DWORD IDirect3DCubeTexture9::GetLevelCount() { return static_cast<TCube *>(this)->levels; }
HRESULT IDirect3DCubeTexture9::GetLevelDesc(UINT lv, D3DSURFACE_DESC *d) { TCube *t = static_cast<TCube *>(this); if (!d || lv >= t->levels) return D3DERR_INVALIDCALL;
    memset(d, 0, sizeof *d); d->Format = t->fmt; d->Type = D3DRTYPE_SURFACE; d->Width = d->Height = t->e >> lv ? t->e >> lv : 1; d->Usage = t->usage; d->Pool = t->pool; return 0; }
HRESULT IDirect3DCubeTexture9::GetCubeMapSurface(D3DCUBEMAP_FACES f, UINT lv, IDirect3DSurface9 **pp) { TCube *t = static_cast<TCube *>(this);
    if (!pp || lv >= t->levels || (unsigned)f > 5) return D3DERR_INVALIDCALL;
    TSurf *s = new TSurf(); s->id = ++g_nid; s->w = s->h = t->e >> lv ? t->e >> lv : 1; s->fmt = t->fmt; s->usage = t->usage; s->what = "CubeSurface";
#ifdef G9BE
    if (GLON) s->g = g9::SurfFromTex((g9::Tex *)t->g, (int)f, (int)lv, s->w, s->h);
#endif
    RegAdd(s, K_SURF); if (OWNMEM) { s->parent = static_cast<IUnknown *>(t); RegAddRef(s->parent); } *pp = s; return 0; }
HRESULT IDirect3DCubeTexture9::UnlockRect(D3DCUBEMAP_FACES f, UINT lv) {
#ifdef G9BE
    if (OWNMEM) g9::TexUnlock((g9::Tex *)static_cast<TCube *>(this)->g, (int)f, (int)lv); else
    if (GLON) g9::TexDirty((g9::Tex *)static_cast<TCube *>(this)->g, (int)f, lv);
#endif
    return 0; }

// ---- estado y dibujo
HRESULT IDirect3DDevice9::SetRenderTarget(DWORD i, IDirect3DSurface9 *s) {
#ifdef G9BE
    if (GLON && s && static_cast<TSurf *>(s)->g) g9::SetRenderTarget(i, (g9::Surf *)static_cast<TSurf *>(s)->g);
#ifdef D3D9_VK
    // [T12] DECISION (cierra "quien llama a PassSetDepthOnly", DISENO O1 pto 5): SetRenderTarget(0,nullptr) es el pass solo de depth del motor (sombras
    // sin color). g9::SetRenderTarget ignora nullptr, asi que aqui se activa v9::PassSetDepthOnly(true) y el primer SetRenderTarget(0, RT) lo apaga.
    // Solo se llama cuando cambia el modo (PassSetDepthOnly hace PassFlush).
    if (GLON && i == 0) { static bool depthOnly = false; bool want = (s == nullptr); if (want != depthOnly) { depthOnly = want; v9::PassSetDepthOnly(want); } }
#endif
#endif
    if (i == 0) { DV->rt0 = static_cast<TSurf *>(s); L("SetRenderTarget[0] %s", s ? static_cast<TSurf *>(s)->what : "(null)"); } return 0; }
HRESULT IDirect3DDevice9::GetRenderTarget(DWORD, IDirect3DSurface9 **pp) { *pp = DV->rt0; RegAddRef(DV->rt0); return 0; }
HRESULT IDirect3DDevice9::SetDepthStencilSurface(IDirect3DSurface9 *s) { DV->ds = static_cast<TSurf *>(s); L("SetDepthStencilSurface %s", s ? static_cast<TSurf *>(s)->what : "(null)");
#ifdef G9BE
    if (GLON) g9::SetDepthStencil(s ? (g9::Surf *)static_cast<TSurf *>(s)->g : nullptr);
#endif
    return 0; }
HRESULT IDirect3DDevice9::GetDepthStencilSurface(IDirect3DSurface9 **pp) { *pp = DV->ds; RegAddRef(DV->ds); return 0; }
HRESULT IDirect3DDevice9::BeginScene() { L("BeginScene"); DV->inScene = true; ++g_ts.beginScenes; return 0; }
HRESULT IDirect3DDevice9::EndScene() { L("EndScene"); DV->inScene = false; return 0; }
HRESULT IDirect3DDevice9::Clear(DWORD n, const D3DRECT *rc, DWORD fl, D3DCOLOR c, float z, DWORD st) { L("Clear flags=0x%x color=0x%08x z=%g", (unsigned)fl, (unsigned)c, z);
#ifdef G9BE
    if (GLON) g9::Clear(n, rc, fl, c, z, st);
#endif
    return 0; }
HRESULT IDirect3DDevice9::SetViewport(const D3DVIEWPORT9 *v) { DV->vp = *v; L("SetViewport %u,%u %ux%u z=%g..%g", (unsigned)v->X, (unsigned)v->Y, (unsigned)v->Width, (unsigned)v->Height, v->MinZ, v->MaxZ); ++g_ts.setViewports;
#ifdef G9BE
    if (GLON) g9::SetViewport(v);
#endif
    return 0; }
HRESULT IDirect3DDevice9::GetViewport(D3DVIEWPORT9 *v) { *v = DV->vp; return 0; }
HRESULT IDirect3DDevice9::SetRenderState(D3DRENDERSTATETYPE s, DWORD v) { L("SetRenderState %u = 0x%x", (unsigned)s, (unsigned)v);
#ifdef G9BE
    if (GLON) g9::SetRS(s, v);
#endif
    return 0; }
HRESULT IDirect3DDevice9::GetRenderState(D3DRENDERSTATETYPE, DWORD *v) { *v = 0; return 0; }
HRESULT IDirect3DDevice9::SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) { L("SetSamplerState s%u type=%u = 0x%x", (unsigned)s, (unsigned)t, (unsigned)v);
#ifdef G9BE
    if (GLON) g9::SetSampler(s, t, v);
#endif
    return 0; }
HRESULT IDirect3DDevice9::SetScissorRect(const RECT *r) { L("SetScissorRect %d,%d,%d,%d", (int)r->left, (int)r->top, (int)r->right, (int)r->bottom);
#ifdef G9BE
    if (GLON) g9::SetScissor(r);
#endif
    return 0; }
HRESULT IDirect3DDevice9::SetTexture(DWORD s, IDirect3DBaseTexture9 *t)
{
    TTex *tt = static_cast<TTex *>(t); if (s < 16) DV->tex[s] = tt;
#ifdef G9BE
    if (GLON) g9::SetTexture(s, t ? (g9::Tex *)tt->g : nullptr);   // g es el primer miembro en TTex/TCube/TVol
#endif
    if (!t) L("SetTexture s%u (null)", (unsigned)s);
    else L("SetTexture s%u tex#%d %ux%u %s '%s'", (unsigned)s, tt->id, tt->w, tt->h, FmtName(tt->fmt), TexTag(t));
    g_ts.setTextures++; if (t) g_ts.texNames[TexTag(t)]++;
    return 0;
}
HRESULT IDirect3DDevice9::SetVertexShader(IDirect3DVertexShader9 *s) { DV->vs = static_cast<TVS *>(s); if (s) L("SetVertexShader vs#%d (%u bytes) '%s'", DV->vs->id, DV->vs->dwords * 4, NM(1, s)); else L("SetVertexShader (null)"); ++g_ts.setVS; if (s) g_ts.vsNames[NM(1, s)]++;
#ifdef G9BE
    if (GLON) g9::SetVS(s ? (g9::Shader *)static_cast<TVS *>(s)->g : nullptr);
#endif
    return 0; }
HRESULT IDirect3DDevice9::SetPixelShader(IDirect3DPixelShader9 *s) { DV->ps = static_cast<TPS *>(s); if (s) L("SetPixelShader ps#%d (%u bytes) '%s'", DV->ps->id, DV->ps->dwords * 4, NM(2, s)); else L("SetPixelShader (null)"); ++g_ts.setPS; if (s) g_ts.psNames[NM(2, s)]++;
#ifdef G9BE
    if (GLON) g9::SetPS(s ? (g9::Shader *)static_cast<TPS *>(s)->g : nullptr);
#endif
    return 0; }
HRESULT IDirect3DDevice9::SetVertexDeclaration(IDirect3DVertexDeclaration9 *d) { DV->decl = static_cast<TDecl *>(d); L("SetVertexDeclaration decl#%d (%zu el)", DV->decl ? DV->decl->id : 0, DV->decl ? DV->decl->el.size() : 0); ++g_ts.setDecl;
#ifdef G9BE
    if (GLON) g9::SetDecl(d ? (g9::Decl *)static_cast<TDecl *>(d)->g : nullptr);
#endif
    return 0; }
HRESULT IDirect3DDevice9::SetStreamSource(UINT n, IDirect3DVertexBuffer9 *b, UINT off, UINT stride) { if (n < 4) { DV->vb[n] = static_cast<TVB *>(b); DV->vbOff[n] = off; DV->vbStride[n] = stride; } L("SetStreamSource %u vb#%d off=%u stride=%u", n, b ? static_cast<TVB *>(b)->id : 0, off, stride);
#ifdef G9BE
    if (GLON) g9::SetStream(n, b ? (g9::Buf *)static_cast<TVB *>(b)->g : nullptr, off, stride);
#endif
    return 0; }
HRESULT IDirect3DDevice9::SetIndices(IDirect3DIndexBuffer9 *b) { DV->ib = static_cast<TIB *>(b); L("SetIndices ib#%d", b ? static_cast<TIB *>(b)->id : 0);
#ifdef G9BE
    if (GLON) g9::SetIndices(b ? (g9::Buf *)static_cast<TIB *>(b)->g : nullptr);
#endif
    return 0; }
HRESULT IDirect3DDevice9::SetVertexShaderConstantF(UINT r, const float *d, UINT n) { L("SetVertexShaderConstantF c%u x%u [%g %g %g %g]", r, n, d[0], d[1], d[2], d[3]); ++g_ts.vsConsts;
#ifdef G9BE
    if (GLON) g9::SetVSConstF(r, d, n);
#endif
    return 0; }
HRESULT IDirect3DDevice9::SetPixelShaderConstantF(UINT r, const float *d, UINT n) { L("SetPixelShaderConstantF c%u x%u [%g %g %g %g]", r, n, d[0], d[1], d[2], d[3]); ++g_ts.psConsts;
#ifdef G9BE
    if (GLON) g9::SetPSConstF(r, d, n);
#endif
    return 0; }
#ifdef G9BE
HRESULT IDirect3DDevice9::SetVertexShaderConstantI(UINT r, const int *d, UINT n) { if (GLON) g9::SetVSConstI(r, d, n); return 0; }
HRESULT IDirect3DDevice9::SetPixelShaderConstantI(UINT r, const int *d, UINT n) { if (GLON) g9::SetPSConstI(r, d, n); return 0; }
HRESULT IDirect3DDevice9::SetVertexShaderConstantB(UINT r, const WINBOOL *d, UINT n) { if (GLON) g9::SetVSConstB(r, (const int *)d, n); return 0; }
HRESULT IDirect3DDevice9::SetPixelShaderConstantB(UINT r, const WINBOOL *d, UINT n) { if (GLON) g9::SetPSConstB(r, (const int *)d, n); return 0; }
#endif
HRESULT IDirect3DDevice9::DrawIndexedPrimitive(D3DPRIMITIVETYPE t, INT bv, UINT minv, UINT nv, UINT start, UINT prims)
{
    L("DrawIndexedPrimitive %s base=%d minv=%u nverts=%u start=%u prims=%u  [vs#%d ps#%d tex0='%s' tex1='%s']", PrimName(t), bv, minv, nv, start, prims,
      DV->vs ? DV->vs->id : 0, DV->ps ? DV->ps->id : 0, TexTag(DV->tex[0]), TexTag(DV->tex[1]));
#ifdef G9BE
    if (GLON) g9::DrawIndexed(t, bv, minv, nv, start, prims);
#endif
    if (DV->ib && !static_cast<TIB *>(DV->ib)->inflight) { static_cast<TIB *>(DV->ib)->inflight = true; g_ibFlight.push_back(static_cast<TIB *>(DV->ib)); }
    ++g_ts.draws; g_ts.drawTris += prims; if (!TraceOff() && g_ts.drawList.size() < 200000) g_ts.drawList.push_back({prims, DV->vs ? DV->vs->id : 0, DV->ps ? DV->ps->id : 0, TexTag(DV->tex[0])});   // OLA 5: antes crecia sin tope (fuga de ~48 B por draw, ~4 MB/s en el Fold)
    if (DV->vb[0] && !OWNMEM && getenv("P3_TRACE_VERTS")) { std::string o; const uint8_t *vb = DV->vb[0]->mem.data() + DV->vbOff[0]; unsigned st = DV->vbStride[0]; for (unsigned v = 0; v < nv && v < 6; ++v) { const float *f = (const float *)(vb + (size_t)(minv + bv + v) * st); char b[160]; snprintf(b, sizeof b, " v%u[%g %g %g %g %g %g]", v, f[0], f[1], f[2], f[3], f[4], f[5]); o += b; } L("    verts(stride %u):%s", st, o.c_str()); }
    if (DV->ib && !OWNMEM && getenv("P3_TRACE_IDX")) { const uint16_t *ix = (const uint16_t *)DV->ib->mem.data() + start; std::string s; for (unsigned i = 0; i < prims * 3 && i < 18; ++i) { char b[16]; snprintf(b, 16, " %u", ix[i]); s += b; } L("    indices:%s", s.c_str()); }
    return 0;
}
HRESULT IDirect3DDevice9::DrawPrimitive(D3DPRIMITIVETYPE t, UINT sv, UINT prims) { L("DrawPrimitive %s start=%u prims=%u", PrimName(t), sv, prims);
#ifdef G9BE
    if (GLON) g9::Draw(t, sv, prims);
#endif
    ++g_ts.draws; g_ts.drawTris += prims; return 0; }
HRESULT IDirect3DDevice9::DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT prims, const void *vd, UINT stride) { L("DrawPrimitiveUP %s prims=%u stride=%u", PrimName(t), prims, stride);
#ifdef G9BE
    if (GLON) g9::DrawUP(t, prims, vd, stride);
#endif
    ++g_ts.draws; g_ts.drawTris += prims; return 0; }
HRESULT IDirect3DDevice9::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT mv, UINT nv, UINT prims, const void *ix, D3DFORMAT ifm, const void *vd, UINT stride) { L("DrawIndexedPrimitiveUP %s nverts=%u prims=%u stride=%u", PrimName(t), nv, prims, stride);
#ifdef G9BE
    if (GLON) g9::DrawIndexedUP(t, mv, nv, prims, ix, ifm, vd, stride);
#endif
    ++g_ts.draws; g_ts.drawTris += prims; return 0; }

// Present por swapchain (el motor usa GetSwapChain(0)->Present en RB_SwapBuffers)
HRESULT IDirect3DSwapChain9::Present(const RECT *, const RECT *, HWND, const RGNDATA *, DWORD) { L("SwapChain Present"); ++g_ts.presents;
#ifdef G9BE
    T12_SelfTest(); if (GLON) g9::Present();
#endif
#ifdef H0_MACWIN
    WinPump();
#endif
    return 0; }
