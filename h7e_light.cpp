// OLA 7e: iluminacion de la sala del Pentagono frente a shot14 (sala casi negra, solo brillan pantallas/atril).
// Diagnosticos y arreglos activables por variables H7E_*. Se engancha por simbolos debiles desde world_h4.cpp.
#include <database/db_registry.h>
#include <database/db_assetnames.h>
#include <universal/ptr32.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_primarylights.h>
#include <xanim/xmodel.h>
#include <qcommon/cm_load_obj.h>
#include <qcommon/cm_load.h>
#include <DynEntity/DynEntity_client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <map>
#include <set>
#include <string>

const ComPrimaryLight *__cdecl Com_GetPrimaryLight(unsigned int primaryLightIndex);
unsigned int __cdecl Com_GetPrimaryLightCount();

// H7E_DIAG="x,y,z,r": materiales del mundo a menos de r de (x,y,z), sus tecnicas LIT/LIT_SPOT/LIT_OMNI con argumentos, y el
// bytecode de su pixel shader en H7E_DUMP_DIR (fuera del repo). Ademas las luces primarias cercanas.
static void DumpTech(const char *mat, MaterialTechniqueSet *ts, int t)
{
    MaterialTechnique *te = ts->techniques[t].get(); if (!te) return;
    for (int p = 0; p < te->passCount; ++p) {
        const MaterialPass &ps = te->passArray[p]; const int n = ps.perPrimArgCount + ps.perObjArgCount + ps.stableArgCount;
        MaterialPixelShader *px = ps.pixelShader.get();
        fprintf(stderr, "[H7E]   tech %d '%s' pass %d ps '%s' args:", t, te->name.get(), p, px ? px->name.get() : "-");
        for (int a = 0; a < n; ++a) { const MaterialShaderArgument &ar = ps.args.get()[a];
            if (ar.type == 4 || ar.type == 2) fprintf(stderr, " [t%d d%d s%u]", ar.type, ar.dest, ar.u.codeSampler);
            else if (ar.type == 1 || ar.type == 7) { const float *l = ar.u.literalConst.get(); fprintf(stderr, " [t%d d%d lit %.4g %.4g %.4g %.4g]", ar.type, ar.dest, l[0], l[1], l[2], l[3]); }
            else fprintf(stderr, " [t%d d%d c%u r%u+%u]", ar.type, ar.dest, ar.u.codeConst.index, ar.u.codeConst.firstRow, ar.u.codeConst.rowCount); }
        fprintf(stderr, "\n");
        const char *dir = getenv("H7E_DUMP_DIR");
        if (dir && px && px->prog.loadDef.program.get()) {
            char fn[512]; snprintf(fn, sizeof fn, "%s/%s.ps", dir, px->name.get());
            if (FILE *f = fopen(fn, "wb")) { fwrite(px->prog.loadDef.program.get(), 4, px->prog.loadDef.programSize, f); fclose(f); }
        }
        MaterialVertexShader *vx = ps.vertexShader.get();   // OLA 15B: tambien el vertex shader (entradas de la capa de mezcla)
        if (dir && vx && vx->prog.loadDef.program.get()) {
            char fn[512]; snprintf(fn, sizeof fn, "%s/%s.vs", dir, vx->name.get());
            if (FILE *f = fopen(fn, "wb")) { fwrite(vx->prog.loadDef.program.get(), 4, vx->prog.loadDef.programSize, f); fclose(f); }
            fprintf(stderr, "[H7E]   tech %d vs '%s' decl %p\n", t, vx->name.get(), (void *)ps.vertexDecl.get());
        }
    }
}
// H7E_LMTEST=k (diagnostico): multiplica la luminancia HDR del lightmap (secondaryB G16R16, los dos canales) por k (0 = sin lightmap)
static void LightmapLumTest(GfxWorld *w)
{
    const char *e = getenv("H7E_LMTEST"); if (!e) return; const float k = (float)atof(e);
    for (int i = 0; i < w->draw.lightmapCount; ++i) {
        GfxImage *im = w->draw.lightmaps[i].secondaryB.get(); if (!im || !im->texture.map) continue;
        D3DLOCKED_RECT lr; if (im->texture.map->LockRect(0, &lr, nullptr, 0) != 0) { fprintf(stderr, "[H7E] LockRect fallo\n"); continue; }
        double sum = 0; unsigned n = 0;
        for (unsigned y = 0; y < im->height; ++y) { unsigned *row = (unsigned *)((char *)lr.pBits + y * lr.Pitch);
            for (unsigned x = 0; x < im->width; ++x) { unsigned v = row[x]; float a = (v & 0xFFFF) * k, b = (v >> 16) * k; sum += (v & 0xFFFF) / 65535.0 * 31.875; ++n;
                row[x] = (unsigned)fminf(a, 65535.f) | ((unsigned)fminf(b, 65535.f) << 16); } }
        im->texture.map->UnlockRect(0);
        fprintf(stderr, "[H7E] lightmap %d: luminancia media %.3f, escalada x%.3f\n", i, n ? sum / n : 0, k);
    }
}
// H7E_ENTS=fichero (diagnostico): vuelca la cadena de entidades del mapa (structs "light" con script_noteworthy por0-4)
static MapEnts *s_ents;
static void EnumEnts(XAssetHeader h, void *) { if (!s_ents) s_ents = h.mapEnts; }
// H7E_RAWLIST=texto (diagnostico): nombres de los rawfiles que contienen el texto (p.ej. clientscripts/)
static void EnumRaw(XAssetHeader h, void *c) { if (h.rawfile && strstr(h.rawfile->name.get(), (const char *)c)) fprintf(stderr, "[H7E] rawfile %s (%d)\n", h.rawfile->name.get(), h.rawfile->len); }
// OLA 15B (diagnostico, H15_PIXEL en world_h4.cpp): triangulos del mundo que corta el rayo org + t*dir, ordenados por distancia
static GfxWorld *s_w15;
void H15_PickRay(const float *o, const float *d)
{
    GfxWorld *w = s_w15; if (!w) return;
    const GfxWorldVertex *V = w->draw.vd.vertices.get(); const unsigned short *I = w->draw.indices.get();
    std::multimap<float, std::string> hits;
    for (int si = 0; si < w->surfaceCount; ++si) { const GfxSurface *s = &w->dpvs.surfaces[si]; const srfTriangles_t &t = s->tris;
        for (int k = 0; k < t.triCount; ++k) {
            const float *p0 = V[t.firstVertex + I[t.baseIndex + 3 * k]].xyz, *p1 = V[t.firstVertex + I[t.baseIndex + 3 * k + 1]].xyz, *p2 = V[t.firstVertex + I[t.baseIndex + 3 * k + 2]].xyz;
            float e1[3], e2[3], pv[3], tv[3], qv[3]; for (int j = 0; j < 3; ++j) { e1[j] = p1[j] - p0[j]; e2[j] = p2[j] - p0[j]; tv[j] = o[j] - p0[j]; }
            pv[0] = d[1] * e2[2] - d[2] * e2[1]; pv[1] = d[2] * e2[0] - d[0] * e2[2]; pv[2] = d[0] * e2[1] - d[1] * e2[0];
            const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2]; if (fabsf(det) < 1e-9f) continue;
            const float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) / det; if (u < 0 || u > 1) continue;
            qv[0] = tv[1] * e1[2] - tv[2] * e1[1]; qv[1] = tv[2] * e1[0] - tv[0] * e1[2]; qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
            const float v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) / det; if (v < 0 || u + v > 1) continue;
            const float tt = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det; if (tt <= 0) continue;
            const Material *m = s->material.get(); char buf[512];
            snprintf(buf, sizeof buf, "surf %d tri %d mat '%s' sort %u prim %u en (%.1f,%.1f,%.1f) det %s", si, k, m ? m->info.name.get() : "-", m ? (unsigned)m->info.drawSurf.fields.primarySortKey : 0,
                s->primaryLightIndex, o[0] + tt * d[0], o[1] + tt * d[1], o[2] + tt * d[2], det > 0 ? "+" : "-");
            hits.insert({ tt, buf }); } }
    int n = 0; for (auto &h : hits) { if (n++ >= 6) break; fprintf(stderr, "[H15]   t=%.1f %s\n", h.first, h.second.c_str()); }
}
void H7E_Setup(GfxWorld *w)
{
    s_w15 = w;
    if (const char *e = getenv("H7E_RAWLIST")) DB_EnumXAssets_FastFile(ASSET_TYPE_RAWFILE, EnumRaw, (void *)e, false);
    if (const char *e = getenv("H7E_ENTS")) {
        DB_EnumXAssets_FastFile(ASSET_TYPE_MAP_ENTS, EnumEnts, 0, false);
        if (s_ents) if (FILE *f = fopen(e, "wb")) { fwrite(s_ents->entityString.get(), 1, s_ents->numEntityChars, f); fclose(f); }
    }
    if (w) LightmapLumTest(w);
    if (const char *e = getenv("H15_SURF"); e && w) {   // OLA 15B (diagnostico): vertices de una superficie (color y datos de capa)
        const GfxSurface *s = &w->dpvs.surfaces[atoi(e)]; const srfTriangles_t &t = s->tris; Material *m = s->material.get();
        fprintf(stderr, "[H15] surf %s mat '%s' firstVertex %d n %u tris %u baseIndex %d layer %d stream2 %d layerSize %u sortKey/flags %u\n", e, m ? m->info.name.get() : "-",
            t.firstVertex, t.vertexCount, t.triCount, t.baseIndex, t.vertexLayerData, t.stream2ByteOffset, w->draw.vertexLayerDataSize, s->flags);
        if (m && m->techniqueSet) if (MaterialTechnique *te = m->techniqueSet->techniques[10].get()) if (MaterialVertexDeclaration *vd = te->passArray[0].vertexDecl.get()) {
            fprintf(stderr, "[H15] decl streams %u opt %d:", vd->streamCount, vd->hasOptionalSource);
            for (int k = 0; k < vd->streamCount && k < 16; ++k) fprintf(stderr, " %u->%u", vd->routing.data[k].source, vd->routing.data[k].dest);
            fprintf(stderr, "\n"); }
        if (m) for (int k = 0; k < m->textureCount; ++k) { const MaterialTextureDef &td = m->textureTable.get()[k]; GfxImage *im = td.semantic == 11 ? nullptr : td.u.image.get();
            fprintf(stderr, "[H15] tex %d hash %u sem %u sampler 0x%02X img '%s' fmt %d %ux%u\n", k, td.nameHash, td.semantic, td.samplerState, im ? im->name.get() : "-",
                im ? (int)R_ImagePixelFormat(im) : -1, im ? im->width : 0, im ? im->height : 0);
            D3DLOCKED_RECT lr;   // DXT1: bloques en modo 3 colores (alfa de 1 bit) y texeles transparentes del nivel 0
            if (im && im->texture.map && R_ImagePixelFormat(im) == D3DFMT_DXT1 && im->texture.map->LockRect(0, &lr, nullptr, 0) == 0) {
                unsigned b3 = 0, tr = 0, nb = 0;
                for (unsigned by = 0; by < im->height / 4; ++by) for (unsigned bx = 0; bx < im->width / 4; ++bx) {
                    const unsigned char *b = (const unsigned char *)lr.pBits + by * lr.Pitch + bx * 8; ++nb;
                    unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8; if (c0 > c1) continue; ++b3;
                    unsigned idx = b[4] | b[5] << 8 | b[6] << 16 | (unsigned)b[7] << 24; for (int q = 0; q < 16; ++q) tr += ((idx >> (2 * q)) & 3) == 3; }
                im->texture.map->UnlockRect(0);
                fprintf(stderr, "[H15]   dxt1 bloques %u, en modo 3 colores %u, texeles transparentes %u de %u\n", nb, b3, tr, nb * 16); } }
        for (unsigned v = 0; v < t.vertexCount; ++v) {
            const GfxWorldVertex &x = w->draw.vd.vertices.get()[t.firstVertex + v]; const unsigned char *c = (const unsigned char *)&x.color;
            const unsigned char *L = w->draw.vld.data.get() + t.vertexLayerData + v * 8;
            fprintf(stderr, "[H15] v%u %.1f %.1f %.1f col %02X %02X %02X %02X layer %02X %02X %02X %02X %02X %02X %02X %02X\n", v, x.xyz[0], x.xyz[1], x.xyz[2], c[0], c[1], c[2], c[3],
                L[0], L[1], L[2], L[3], L[4], L[5], L[6], L[7]);
        }
    }
    if (const char *e = getenv("H15_PICK"); e && w) {   // OLA 15B (diagnostico): triangulos del mundo que cruzan la vertical (x,y) entre z0 y z1
        float x = 0, y = 0, z0 = -1e9f, z1 = 1e9f; sscanf(e, "%f,%f,%f,%f", &x, &y, &z0, &z1);
        const GfxWorldVertex *V = w->draw.vd.vertices.get(); const unsigned short *I = w->draw.indices.get();
        for (int si = 0; si < w->surfaceCount; ++si) { const GfxSurface *s = &w->dpvs.surfaces[si]; const srfTriangles_t &t = s->tris;
            if (x < s->bounds[0][0] - 1 || x > s->bounds[1][0] + 1 || y < s->bounds[0][1] - 1 || y > s->bounds[1][1] + 1) continue;
            for (int k = 0; k < t.triCount; ++k) {
                const float *p[3]; for (int q = 0; q < 3; ++q) p[q] = V[t.firstVertex + I[t.baseIndex + 3 * k + q]].xyz;
                float d0 = (p[1][0] - p[0][0]) * (y - p[0][1]) - (p[1][1] - p[0][1]) * (x - p[0][0]), d1 = (p[2][0] - p[1][0]) * (y - p[1][1]) - (p[2][1] - p[1][1]) * (x - p[1][0]),
                      d2 = (p[0][0] - p[2][0]) * (y - p[2][1]) - (p[0][1] - p[2][1]) * (x - p[2][0]);
                if (!((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0))) continue;
                const float zz = (p[0][2] + p[1][2] + p[2][2]) / 3; if (zz < z0 || zz > z1) continue;
                const Material *m = s->material.get(); unsigned char c[3][4]; for (int q = 0; q < 3; ++q) memcpy(c[q], &V[t.firstVertex + I[t.baseIndex + 3 * k + q]].color, 4);
                fprintf(stderr, "[H15] pick surf %d tri %d z~%.1f mat '%s' sort %u flags %u col.g %02X %02X %02X (%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n", si, k, zz, m ? m->info.name.get() : "-",
                    m ? (unsigned)m->info.drawSurf.fields.primarySortKey : 0, s->flags, c[0][1], c[1][1], c[2][1], p[0][0], p[0][1], p[1][0], p[1][1], p[2][0], p[2][1]);
                for (int q = 0; q < 3; ++q) { const GfxWorldVertex &vv = V[t.firstVertex + I[t.baseIndex + 3 * k + q]];
                    fprintf(stderr, "[H15]   v (%.1f,%.1f,%.1f) lmap %.5f %.5f tex %.3f %.3f\n", vv.xyz[0], vv.xyz[1], vv.xyz[2], vv.lmapCoord[0], vv.lmapCoord[1], vv.texCoord[0], vv.texCoord[1]); } } }
    }
    for (int which = 0; which < 2 && w; ++which) if (const char *e = getenv(which ? "H15_LMSECB" : "H15_LMSEC")) {   // OLA 15B (diagnostico): rellena el lightmap con un valor fijo
        const unsigned val = (unsigned)strtoul(e, 0, 16);
        for (int i = 0; i < w->draw.lightmapCount; ++i) { GfxImage *im = which ? w->draw.lightmaps[i].secondaryB.get() : w->draw.lightmaps[i].secondary.get();
            D3DLOCKED_RECT lr; if (!im || !im->texture.map || im->texture.map->LockRect(0, &lr, nullptr, 0) != 0) continue;
            for (unsigned y = 0; y < im->height; ++y) for (unsigned x = 0; x < im->width; ++x) {
                if (which) ((unsigned *)((char *)lr.pBits + y * lr.Pitch))[x] = val; else ((unsigned short *)((char *)lr.pBits + y * lr.Pitch))[x] = (unsigned short)val; }
            im->texture.map->UnlockRect(0); fprintf(stderr, "[H15] lightmap %s = 0x%X\n", which ? "secondaryB" : "secondary", val); }
    }
    if (const char *dir = getenv("H15_LMDUMP"); dir && w) {   // OLA 15B (diagnostico): lightmaps secondary (R5G6B5) y secondaryB (G16R16) a PPM
        for (int i = 0; i < w->draw.lightmapCount; ++i) for (int which = 0; which < 2; ++which) {
            GfxImage *im = which ? w->draw.lightmaps[i].secondaryB.get() : w->draw.lightmaps[i].secondary.get(); if (!im || !im->texture.map) continue;
            D3DLOCKED_RECT lr; if (im->texture.map->LockRect(0, &lr, nullptr, 0) != 0) continue;
            char fn[512]; snprintf(fn, sizeof fn, "%s/lm%d_%s.ppm", dir, i, which ? "secB" : "sec");
            if (FILE *f = fopen(fn, "wb")) { fprintf(f, "P6 %u %u 255\n", im->width, im->height);
                for (unsigned y = 0; y < im->height; ++y) for (unsigned x = 0; x < im->width; ++x) { unsigned char px[3];
                    if (which) { const unsigned v = ((const unsigned *)((const char *)lr.pBits + y * lr.Pitch))[x]; px[0] = (v & 0xFFFF) >> 8; px[1] = v >> 24; px[2] = 0; }
                    else { const unsigned short v = ((const unsigned short *)((const char *)lr.pBits + y * lr.Pitch))[x]; px[0] = ((v >> 11) & 31) * 255 / 31; px[1] = ((v >> 5) & 63) * 255 / 63; px[2] = (v & 31) * 255 / 31; }
                    fwrite(px, 1, 3, f); }
                fclose(f); }
            im->texture.map->UnlockRect(0);
        }
    }
    if (const char *e = getenv("H15_SETG"); e && w && w->draw.vd.worldVb.get()) {   // OLA 15B (diagnostico): fija el peso de capa (color.g) de las superficies con capa
        const int g = atoi(e); void *p = nullptr; int n = 0;
        if (w->draw.vd.worldVb->Lock(0, 0, &p, 0) == 0 && p) {
            for (int si = 0; si < w->surfaceCount; ++si) { const GfxSurface *s = &w->dpvs.surfaces[si]; const Material *m = s->material.get();
                if (!m || !strstr(m->info.name.get(), "_blend")) continue;
                for (unsigned v = 0; v < s->tris.vertexCount; ++v) { ((unsigned char *)p)[44 * (s->tris.firstVertex + v) + 17] = (unsigned char)g; ++n; } }
            w->draw.vd.worldVb->Unlock(); }
        fprintf(stderr, "[H15] SETG %d en %d vertices\n", g, n);
    }
    if (const char *e = getenv("H15_FILL"); e && w) {   // OLA 15B (diagnostico): "subcadena=AARRGGBB;..." rellena todas las mips de las imagenes cuyo nombre contiene la subcadena
        std::string all = e; size_t p0 = 0;
        while (p0 < all.size()) { size_t p1 = all.find(';', p0); if (p1 == std::string::npos) p1 = all.size(); std::string it = all.substr(p0, p1 - p0); p0 = p1 + 1;
            const size_t eq = it.find('='); if (eq == std::string::npos) continue; const std::string key = it.substr(0, eq); const unsigned val = (unsigned)strtoul(it.c_str() + eq + 1, 0, 16);
            const unsigned a = val >> 24, r = (val >> 16) & 255, g = (val >> 8) & 255, b = val & 255; const unsigned short c565 = (unsigned short)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
            std::set<GfxImage *> seen;
            for (int si = 0; si < w->surfaceCount; ++si) { Material *m = w->dpvs.surfaces[si].material.get(); if (!m) continue;
                for (int k = 0; k < m->textureCount; ++k) { const MaterialTextureDef &td = m->textureTable.get()[k]; GfxImage *im = td.semantic == 11 ? nullptr : td.u.image.get();
                    if (!im || !im->texture.map || !strstr(im->name.get(), key.c_str()) || !seen.insert(im).second) continue;
                    const D3DFORMAT f = R_ImagePixelFormat(im); const unsigned lv = im->texture.map->GetLevelCount();
                    for (unsigned l = 0; l < lv; ++l) { D3DLOCKED_RECT lr; if (im->texture.map->LockRect(l, &lr, nullptr, 0) != 0) continue;
                        const unsigned W = std::max(1u, (unsigned)im->width >> l), H = std::max(1u, (unsigned)im->height >> l);
                        if (f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5) { const unsigned bs = f == D3DFMT_DXT1 ? 8 : 16;
                            unsigned char blk[16] = {}; unsigned char *cb = blk + (bs - 8);
                            if (f == D3DFMT_DXT5) { blk[0] = blk[1] = (unsigned char)a; } else if (f == D3DFMT_DXT3) memset(blk, (a >> 4) * 0x11, 8);
                            cb[0] = cb[2] = c565 & 255; cb[1] = cb[3] = c565 >> 8; if (f == D3DFMT_DXT1) { cb[0] = (c565 + 0) & 255; }   // c0 == c1: modo 3 colores, indice 0 = c0
                            for (unsigned by = 0; by < (H + 3) / 4; ++by) for (unsigned bx = 0; bx < (W + 3) / 4; ++bx) memcpy((char *)lr.pBits + by * lr.Pitch + bx * bs, blk, bs); }
                        else for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) ((unsigned *)((char *)lr.pBits + y * lr.Pitch))[x] = val;
                        im->texture.map->UnlockRect(l); }
                    fprintf(stderr, "[H15] fill '%s' fmt %d %u mips = %08X\n", im->name.get(), (int)f, lv, val); } } }
    }
    if (const char *e = getenv("H15_DYN")) {   // OLA 15B (diagnostico): entidades dinamicas del mapa (DynEntityDef) a menos de r de (x,y,z)
        float c[3] = { 0, 0, 0 }, r = 1e9f; sscanf(e, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &r);
        for (int k = 0; k < 2; ++k) { const DynEntityDef *L = cm.dynEntDefList[k].get(); int n = 0;
            for (unsigned i = 0; L && i < cm.dynEntCount[k]; ++i) { const DynEntityDef &de = L[i]; float dd = 0;
                for (int q = 0; q < 3; ++q) dd += (de.pose.origin[q] - c[q]) * (de.pose.origin[q] - c[q]);
                if (dd > r * r) continue; ++n;
                fprintf(stderr, "[H15] dynent %d/%u tipo %d '%s' brush %u en %.0f %.0f %.0f\n", k, i, (int)de.type, de.xModel.get() ? de.xModel->name.get() : "-", de.brushModel,
                    de.pose.origin[0], de.pose.origin[1], de.pose.origin[2]); }
            fprintf(stderr, "[H15] dynent lista %d: %u en total, %d cerca\n", k, cm.dynEntCount[k], n); }
    }
    const char *d = getenv("H7E_DIAG"); if (!d || !w) return;
    float c[3] = { -498.1f, 2582.6f, 76.1f }, r = 500;
    sscanf(d, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &r);
    std::map<std::string, int> cnt; std::set<int> lights;
    for (int i = 0; i < w->surfaceCount; ++i) {
        GfxSurface *s = &w->dpvs.surfaces[i]; Material *m = s->material.get(); if (!m) continue;
        float dd = 0; for (int k = 0; k < 3; ++k) { float v = c[k] < s->bounds[0][k] ? s->bounds[0][k] - c[k] : (c[k] > s->bounds[1][k] ? c[k] - s->bounds[1][k] : 0); dd += v * v; }
        if (dd > r * r) continue;
        lights.insert(s->primaryLightIndex);
        if (cnt[m->info.name.get()]++) continue;
        fprintf(stderr, "[H7E] mat '%s' techset '%s' prim=%d lm=%d surf %d (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)\n", m->info.name.get(), m->techniqueSet ? m->techniqueSet->name.get() : "-",
            s->primaryLightIndex, s->lightmapIndex, i, s->bounds[0][0], s->bounds[0][1], s->bounds[0][2], s->bounds[1][0], s->bounds[1][1], s->bounds[1][2]);
        if (m->techniqueSet) for (int t : { 10, 11, 13, 15 }) DumpTech(m->info.name.get(), m->techniqueSet.get(), t);
    }
    for (int li : lights) {
        if (!li) continue; const ComPrimaryLight *l = Com_GetPrimaryLight(li);
        fprintf(stderr, "[H7E] luz %d tipo %d color %.3f %.3f %.3f origen %.0f %.0f %.0f radio %.0f cos %.3f/%.3f exp %d def '%s'\n", li, l->type, l->color[0], l->color[1], l->color[2],
            l->origin[0], l->origin[1], l->origin[2], l->radius, l->cosHalfFovOuter, l->cosHalfFovInner, l->exponent, l->defName.get() ? l->defName.get() : "-");
    }
    fprintf(stderr, "[H7E] %zu materiales, %zu luces primarias (de %u)\n", cnt.size(), lights.size(), Com_GetPrimaryLightCount());
    const GfxWorldSunColor &sc = w->sunParse.sunSettings[0];
    fprintf(stderr, "[H7E] sol '%s' exposure %.4f ambient %.3f %.3f %.3f diffuse %.3f %.3f %.3f sky %.3f ground %.3f skyboxHDR %.3f\n", w->sunParse.name, sc.exposure,
        sc.ambientColor[0], sc.ambientColor[1], sc.ambientColor[2], sc.sunDiffuseColor[0], sc.sunDiffuseColor[1], sc.sunDiffuseColor[2], sc.skyColor[0], sc.groundColor[0], sc.skyboxHDRScale);
    for (unsigned i = 0; i < w->exposureVolumeCount; ++i) { const GfxExposureVolume &v = w->exposureVolumes[i];
        fprintf(stderr, "[H7E] exposureVolume %u control 0x%08X exposure %.4f inc %.3f dec %.3f feather %.3f/%.3f\n", i, v.control, v.exposure, v.luminanceIncreaseScale, v.luminanceDecreaseScale, v.featherRange, v.featherAdjust); }
    {   // smodels cercanos: luz de suelo horneada (groundLighting) frente a lightgrid
        int nGround = 0, nNear = 0;
        for (unsigned i = 0; i < w->dpvs.smodelCount; ++i) {
            const GfxStaticModelInst &si = w->dpvs.smodelInsts[i]; const GfxStaticModelDrawInst &di = w->dpvs.smodelDrawInsts[i];
            nGround += si.groundLighting.packed != 0;
            float dd = 0; for (int k = 0; k < 3; ++k) { float v = si.lightingOrigin[k] - c[k]; dd += v * v; }
            if (dd > r * r || nNear++ >= 40) continue;
            fprintf(stderr, "[H7E] smodel %u '%s' ground 0x%08X prim %d origen luz %.0f %.0f %.0f flags 0x%X\n", i, di.model ? di.model->name.get() : "-", si.groundLighting.packed,
                di.primaryLightIndex, si.lightingOrigin[0], si.lightingOrigin[1], si.lightingOrigin[2], di.flags);
        }
        fprintf(stderr, "[H7E] smodels con groundLighting: %d de %u\n", nGround, w->dpvs.smodelCount);
    }
    fprintf(stderr, "[H7E] volumenes de exposicion: %u (sin volumen: exposicion del sol, R_UpdateExposureValue)\n", w->exposureVolumeCount);
}

// =====================================================================================================================================
// OLA 7e: estado de CLIENTE que en el original llevan los scripts de cliente (no hay VM de CSC en el port). Se reproduce lo que hacen
// clientscripts/_lights.csc y zombie_pentagon.csc con las mismas reglas y numeros, sobre el refdef del port (h5_light.cpp g_lights):
//  - init_lights / create_lights / mixer_thread: cada struct "light" del mapa (pl# = luz primaria) es una entidad ET_PRIMARY_LIGHT cuyo
//    color en el refdef es Byte4Pack(color) * intensidad (CScr_SetLightColor / CScr_SetLightIntensity -> CG_PrimaryLight).
//      light_off      -> color (0,0,0) e intensidad 0 desde el principio (las 5 luces giratorias de los portales del laboratorio).
//      fire_flicker   -> intensidad * RandomFloatRange(0.5, 1) cada RandomFloatRange(0.1, 0.2) s en pasos de 0.05 s, solo a < 1500 u.
//      light_solid    -> sin cambios.
//      mezclador (script_mixer_event "por0".."por4"): al llegar el evento pasa al lado 2 (light_solid, script_light2_intensity/_color)
//      interpolando en script_mixer_rtl_ramp segundos (mixer_get_ramp), y avisa "on" al modelo (light_model_switch).
//      script_light_rotate_yaw: RotateYaw(360, t) en bucle (la direccion del foco gira si rotationLimit < 1, CG_PrimaryLight).
//  - zombie_pentagon.csc: levelNotify "ZPO" -> _power_on, "TLO" -> por0..4; pentagon_office_light_model_swap (ceiling_on -> ceiling,
//    tinhat_on -> tinhat_off); set_visionset_*: "vis1" -> zombie_pentagon / zombie_pentagon_offices_poweroff (con corriente),
//    "vis2" warroom (2 s), "vis3" lab (2 s), "vis4" electrician (1 s) con VisionSetNaked (h7_gfx.cpp H7_VisionSetNaked).
//  - script_model de cliente (spawnflags & 2: G_SpawnScriptModelClientSide los deja fuera del servidor y los crea el cliente): relojes,
//    pantallas de la caja, lamparas de oficina, luces de portal. Se dibujan rigidos como los smodels con la luz del lightgrid en su origen.
// Los levelNotify llegan por el '9' de GScr_ClientSysSetState (g_h0ServerCmdHook, encadenado con el del sonido).
//   H7E_CLIENT=0 desactiva todo; H7E_CLLIGHTS=0 las luces; H7E_VISION=0 los vision sets; H7E_CLMODELS=0 los modelos.
//   H7E_POWER=ms (prueba): a ese level.time se dispara el trigger use_elec_switch con el jugador, como si pulsara el interruptor.
//   H7E_NOTIFY="ms:evento,..." (prueba): levelNotify inyectados (vis1-4, ZPO...).
#ifdef H5G
#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_spawn_mp.h>
#include <server_mp/server_mp.h>
#include <server_mp/sv_init_mp.h>
#include <clientscript/scr_const.h>
#include <clientscript/cscr_stringlist.h>
#include <xanim/xmodel_utils.h>
#include <gfx_d3d/r_model_skin.h>
#include <EffectsCore/fx_system.h>
#include <time.h>
#include <ctype.h>
#include <vector>
#include "world_h4.h"
const FxEffectDef *__cdecl FX_Register(char *name);   // h5_fx.cpp (fx_load_obj.cpp extraida): OLA 15F, FX de las luces giratorias

GfxLight *H5L_Lights() __attribute__((weak));
void H5L_Prepare(int key, const float *lightingOrigin, const float *half, bool hero) __attribute__((weak));
void H5L_Bind(int key) __attribute__((weak));
int H5L_MaxKey() __attribute__((weak));
bool H7_VisionSetNaked(const char *name, float seconds) __attribute__((weak));
void H7_VisionTime(int nowMs) __attribute__((weak));
const char *H7_VisionNakedName() __attribute__((weak));                      // OLA M1 (h7_gfx.cpp)
void H7C_ClientVolFog(const float *v, int transitionMs) __attribute__((weak));   // OLA M1 (h7c_gfx.cpp): setVolFog del CSC
void H9V_SysState(const char *sys, const char *state) __attribute__((weak));   // OLA 9 (h9_video.cpp)
extern void (*g_h0ServerCmdHook)(const client_t *client, const char *cmd);

namespace {
bool EnvOn(const char *n) { const char *e = getenv(n); return !(e && !atoi(e)); }
typedef std::map<std::string, std::string> Kv;
std::vector<Kv> ParseEnts(const char *s, int n)
{
    std::vector<Kv> out; Kv cur; bool in = false; int i = 0;
    auto str = [&](std::string &o) { o.clear(); ++i; while (i < n && s[i] != '"') o += s[i++]; ++i; };
    while (i < n) {
        const char c = s[i];
        if (c == '{') { cur.clear(); in = true; ++i; }
        else if (c == '}') { if (in) out.push_back(cur); in = false; ++i; }
        else if (c == '"' && in) { std::string k, v; str(k); while (i < n && s[i] != '"') ++i; if (i >= n) break; str(v); cur[k] = v; }
        else ++i;
    }
    return out;
}
bool Vec(const Kv &k, const char *key, float *v) { auto it = k.find(key); return it != k.end() && sscanf(it->second.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3; }
const char *Str(const Kv &k, const char *key) { auto it = k.find(key); return it == k.end() ? nullptr : it->second.c_str(); }

struct ClLight {
    int pl = 0; std::string type, mixerEvent; bool mixer = false;
    float color0[3] = { 0, 0, 0 }, int0 = 0;     // lado 1 (light_struct 0): GetLightColor / GetLightIntensity
    float color1[3] = { 0, 0, 0 }, int1 = 0;     // lado 2 (script_light2_*)
    float cur[3] = { 0, 0, 0 }, curInt = 0;      // estado de la entidad de luz (Byte4 color, intensidad)
    float ramp = 0.25f; int mixT0 = -1; bool side1 = false;
    float yawRate = 0, dir0[3] = { 0, 0, 0 }, org[3] = { 0, 0, 0 };
    // fire_flicker
    float dmin = 0.1f, dmax = 0.2f, imin = 0.5f, imax = 1.0f, fTarget = 0, fDiv = 0; int fSteps = 0, fStep = 0, fNext = 0; bool rangeCheck = true;
    std::string modelTn, onModel; int model = -1;
    std::string spinModel, spinFx, spinTag; int spin = -1; float yaw0 = 0;   // OLA 15F: script_light_on_spin_model / spin_fx / spin_tag (run_spin_model)
};
struct ClModel { std::string tn, name; XModel *m = nullptr; float org[3], axis[3][3]; int key = -2; bool vis = false; float ang[3] = { 0, 0, 0 }; std::string note; float pitchAdd = 0; };   // OLA 15F: angles, script_noteworthy y giro de reloj
std::vector<ClLight> s_lights; std::vector<ClModel> s_models;
bool s_on, s_power, s_init; int s_powerAt = -1; bool s_powerDone;
void (*s_prevHook)(const client_t *, const char *);
unsigned s_rng = 0x2545F491u;
float Rand01() { s_rng = s_rng * 1664525u + 1013904223u; return (s_rng >> 8) * (1.0f / 16777216.0f); }
float RandRange(float a, float b) { return a + (b - a) * Rand01(); }
void Byte4(const float *in, float *out) { for (int k = 0; k < 3; ++k) { int v = (int)(255.0f * in[k] + 9.313225746154785e-10f); v = v > 255 ? 255 : v < 0 ? 0 : v; out[k] = v / 255.0f; } }

void Apply(ClLight &l)
{
    GfxLight *gl = H5L_Lights ? H5L_Lights() : nullptr; if (!gl || l.pl <= 0 || (unsigned)l.pl >= Com_GetPrimaryLightCount()) return;
    float c[3]; Byte4(l.cur, c);
    GfxLight &g = gl[l.pl];   // CG_PrimaryLight: color = Byte4(color) * intensidad, y diffuse/specular = color (R_SetLightProperties lee estos)
    for (int k = 0; k < 3; ++k) g.color[k] = g.diffuseColor[k] = g.specularColor[k] = c[k] * l.curInt;
    g.diffuseColor[3] = g.specularColor[3] = 1.0f;
}
void SetModel(int mi, const char *name)
{
    if (mi < 0 || mi >= (int)s_models.size() || !name || s_models[mi].name == name) return;
    XModel *m = DB_FindXAssetHeader(ASSET_TYPE_XMODEL, (char *)name, 1, -1).model; if (!m) return;
    s_models[mi].name = name; s_models[mi].m = m;
}
bool s_kino, s_packClock;   // OLA M1: zombie_theater (Kino der Toten): sistemas de su .csc (abajo, KinoInit/KinoNotify/KinoSys/KinoFrame)
void KinoInit(const std::vector<Kv> &ents); void KinoNotify(const std::string &ev, int now); bool KinoSys(const char *sys, const std::string &st, int now);
void KinoFrame(int now);
void LevelNotify(const std::string &ev, int now)
{
    if (s_kino) { KinoNotify(ev, now); return; }
    if (ev == "ZPO") {   // pentagon_ZPO_listener + pentagon_office_light_model_swap
        s_power = true;
        for (auto &m : s_models) if (m.tn == "model_interior_office_lights") {
            if (m.name == "p_pent_light_ceiling_on") SetModel(&m - &s_models[0], "p_pent_light_ceiling");
            else if (m.name == "p_pent_light_tinhat_on") SetModel(&m - &s_models[0], "p_pent_light_tinhat_off"); }
        LevelNotify("TLO", now); return;
    }
    if (ev == "TLO") { for (int i = 0; i < 5; ++i) LevelNotify("por" + std::to_string(i), now); return; }   // pentagon_TLO_listener
    if (ev.size() == 4 && !ev.compare(0, 3, "vis") && EnvOn("H7E_VISION") && H7_VisionSetNaked) {   // set_visionset_*
        const int f = ev[3] - '0';
        if (f == 1) H7_VisionSetNaked(s_power ? "zombie_pentagon_offices_poweroff" : "zombie_pentagon", 0.0f);
        else if (f == 2) H7_VisionSetNaked("zombie_pentagon_warroom", 2.0f);
        else if (f == 3) H7_VisionSetNaked("zombie_pentagon_lab", 2.0f);
        else if (f == 4) H7_VisionSetNaked("zombie_pentagon_electrician", 1.0f);
        return;
    }
    for (auto &l : s_lights) if (l.mixer && l.mixerEvent == ev && l.mixT0 < 0 && !l.side1) l.mixT0 = now;   // mixer_level_activate
}
void ServerCmd(const client_t *client, const char *cmd)
{
    if (s_prevHook) s_prevHook(client, cmd);
    if (!client || !cmd || cmd[0] != '9' || cmd[1] != ' ') return;
    const int idx = atoi(cmd + 2);
    char cs[1024]; SV_GetConfigstring(idx + 1538, cs, sizeof cs);
    const char *p = strchr(cmd + 2, ' '); if (!p) return;
    char ss[1024]; I_strncpyz(ss, p + 1, sizeof ss);
    std::string st = Info_ValueForKey(ss, "s");
    for (auto &c : st) if (c == '!') c = ' ';
    if (s_kino && KinoSys(Info_ValueForKey(cs, "n"), st, level.time)) return;   // OLA M1: box_indicator de Kino (luces del tablero)
    if (H9V_SysState) H9V_SysState(Info_ValueForKey(cs, "n"), st.c_str());   // OLA 9: box_indicator (h9_video.cpp)
    if (I_stricmp(Info_ValueForKey(cs, "n"), "levelNotify")) return;
    fprintf(stderr, "[H7E] levelNotify '%s' t=%d\n", st.c_str(), level.time);
    LevelNotify(st, level.time);
}

void ClientInit()
{
    s_init = true; s_on = EnvOn("H7E_CLIENT"); if (!s_on) return;
    if (const char *e = getenv("H7E_POWER")) s_powerAt = atoi(e);
    MapEnts *me = nullptr; DB_EnumXAssets_FastFile(ASSET_TYPE_MAP_ENTS, [](XAssetHeader h, void *c) { if (!*(MapEnts **)c) *(MapEnts **)c = h.mapEnts; }, &me, false);
    if (!me) { fprintf(stderr, "[H7E] cliente: sin MapEnts\n"); return; }
    const std::vector<Kv> ents = ParseEnts(me->entityString.get(), me->numEntityChars);
    const bool models = EnvOn("H7E_CLMODELS"), lights = EnvOn("H7E_CLLIGHTS");
    std::map<std::string, int> types;
    for (const Kv &k : ents) {
        const char *cn = Str(k, "classname"); if (!cn) continue;
        if (models && !strcmp(cn, "script_model") && Str(k, "model") && *Str(k, "model") && (atoi(Str(k, "spawnflags") ? Str(k, "spawnflags") : "0") & 2)) {
            ClModel m; m.name = Str(k, "model"); m.tn = Str(k, "targetname") ? Str(k, "targetname") : "";
            m.m = DB_FindXAssetHeader(ASSET_TYPE_XMODEL, (char *)m.name.c_str(), 1, -1).model; if (!m.m) continue;
            float ang[3] = { 0, 0, 0 }; Vec(k, "origin", m.org); Vec(k, "angles", ang); AnglesToAxis(ang, m.axis);
            memcpy(m.ang, ang, 12); if (Str(k, "script_noteworthy")) m.note = Str(k, "script_noteworthy");   // OLA 15F (relojes)
            s_models.push_back(m);
        }
        if (lights && !strcmp(cn, "light") && Str(k, "pl#")) {
            ClLight l; l.pl = atoi(Str(k, "pl#")); if (l.pl <= 0 || (unsigned)l.pl >= Com_GetPrimaryLightCount()) continue;
            const ComPrimaryLight *cl = Com_GetPrimaryLight(l.pl);
            float nc[3]; l.int0 = (float)ColorNormalize(cl->color, nc); Byte4(nc, l.color0);   // CG_CreateLight: intensidad + color normalizado
            memcpy(l.cur, l.color0, 12); l.curInt = l.int0; memcpy(l.org, cl->origin, 12);
            l.type = Str(k, "targetname") ? Str(k, "targetname") : "light_off";
            if (Str(k, "script_light_type")) l.type = Str(k, "script_light_type");
            ++types[l.type];
            if (Str(k, "script_mixer_event")) {
                l.mixer = true; l.mixerEvent = Str(k, "script_mixer_event");
                l.ramp = Str(k, "script_mixer_rtl_ramp") ? (float)atof(Str(k, "script_mixer_rtl_ramp")) : 0.25f;   // mixer_get_ramp desde mix_pos 0
                if (!Vec(k, "script_light2_color", l.color1)) memcpy(l.color1, l.color0, 12);
                l.int1 = Str(k, "script_light2_intensity") ? (float)atof(Str(k, "script_light2_intensity")) : l.int0;
            }
            if (Str(k, "script_light_rotate_yaw") && cl->rotationLimit < 1.0f) { l.yawRate = 360.0f / (float)atof(Str(k, "script_light_rotate_yaw")); }
            if (H5L_Lights && H5L_Lights()) memcpy(l.dir0, H5L_Lights()[l.pl].dir, 12);
            if (Str(k, "script_delay_min")) l.dmin = (float)atof(Str(k, "script_delay_min"));
            if (Str(k, "script_delay_max")) l.dmax = (float)atof(Str(k, "script_delay_max"));
            if (Str(k, "script_intensity_min")) l.imin = (float)atof(Str(k, "script_intensity_min"));
            if (Str(k, "script_intensity_max")) l.imax = (float)atof(Str(k, "script_intensity_max"));
            l.rangeCheck = !Str(k, "script_light_no_range_check");
            if (Str(k, "script_light_model") && Str(k, "script_light_on_model") && Str(k, "script_light_off_model")) { l.modelTn = Str(k, "script_light_model"); l.onModel = Str(k, "script_light_on_model"); }
            if (Str(k, "script_light_on_spin_model")) { l.spinModel = Str(k, "script_light_on_spin_model"); l.spinFx = Str(k, "script_light_spin_fx") ? Str(k, "script_light_spin_fx") : ""; l.spinTag = Str(k, "script_light_spin_tag") ? Str(k, "script_light_spin_tag") : ""; }   // OLA 15F
            l.yaw0 = atan2f(l.dir0[1], l.dir0[0]) * (float)(180.0 / M_PI);   // OLA 15F: yaw de partida de la luz (light.angles[1] de run_spin_model)
            if (l.type == "light_off") { memset(l.cur, 0, 12); l.curInt = 0; }   // light_off: set_light_color((0,0,0)) + set_light_intensity(0)
            s_lights.push_back(l);
        }
    }
    { const char *mn = Dvar_GetString("mapname"); s_kino = mn && !strcmp(mn, "zombie_theater") && EnvOn("H19_KINO"); if (s_kino) KinoInit(ents); }
    for (auto &l : s_lights) if (!l.modelTn.empty()) for (auto &m : s_models) if (m.tn == l.modelTn) l.model = (int)(&m - &s_models[0]);
    // claves de luz de modelo: detras de las de los smodels (1024 + smodelCount + j)
    const int base = 1024 + (int)(rgp.world ? rgp.world->dpvs.smodelCount : 0), maxKey = H5L_MaxKey ? H5L_MaxKey() : 0;
    int nk = 0; for (size_t j = 0; j < s_models.size(); ++j) if (base + (int)j <= maxKey) { s_models[j].key = base + (int)j; ++nk; }
    std::string ts; for (auto &t : types) ts += " " + t.first + "=" + std::to_string(t.second);
    fprintf(stderr, "[H7E] cliente: %zu luces de script (%s ), %zu script_model de cliente (%d con luz)\n", s_lights.size(), ts.c_str(), s_models.size(), nk);
    for (auto &l : s_lights) Apply(l);
}

void H15F_SpinOn(ClLight &l); void H15F_SpinFrame(ClLight &l, int now);   // OLA 15F (abajo)
void LightsFrame(int now, int dt, const float *cam)
{
    GfxLight *gl = H5L_Lights ? H5L_Lights() : nullptr; if (!gl) return;
    for (auto &l : s_lights) {
        bool dirty = false;
        if (l.mixer && l.mixT0 >= 0 && !l.side1) {   // mixer_thread: LerpVector / LerpFloat del lado 1 al 2
            const float t = l.ramp > 0 ? (float)(now - l.mixT0) / (l.ramp * 1000.0f) : 1.0f;
            const float tc = t > 1 ? 1 : t, *c0 = l.type == "light_off" ? nullptr : l.color0;
            const float i0 = l.type == "light_off" ? 0.0f : l.int0;
            for (int k = 0; k < 3; ++k) l.cur[k] = (c0 ? c0[k] : 0.0f) + (l.color1[k] - (c0 ? c0[k] : 0.0f)) * tc;
            l.curInt = i0 + (l.int1 - i0) * tc; dirty = true;
            if (t >= 1) { l.side1 = true; if (l.model >= 0) SetModel(l.model, l.onModel.c_str()); if (l.model >= 0 && !l.spinModel.empty()) H15F_SpinOn(l); }   // light_model_switch "on" (+ run_spin_model, OLA 15F)
        }
        if (l.type == "fire_flicker" && !(l.mixer && (l.side1 || l.mixT0 >= 0))) {
            const float dx = l.org[0] - cam[0], dy = l.org[1] - cam[1], dz = l.org[2] - cam[2];
            if (!l.rangeCheck || dx * dx + dy * dy + dz * dz <= 2250000.0f) {
                if (now >= l.fNext) {
                    if (l.fStep >= l.fSteps) {   // nuevo objetivo
                        l.fTarget = l.int0 * RandRange(l.imin, l.imax);
                        float tm = RandRange(l.dmin, l.dmax); tm = tm < 0.016f ? 0.016f : tm;
                        const float steps = tm * 20.0f; l.fSteps = (int)ceilf(steps); l.fStep = 0; l.fDiv = (l.curInt - l.fTarget) / steps;
                    }
                    l.curInt -= l.fDiv; if (l.curInt < 0) l.curInt = 0; ++l.fStep; dirty = true;
                    if (l.fStep >= l.fSteps) l.curInt = l.fTarget;
                    l.fNext = now + 50;
                }
            }
        }
        if (l.yawRate != 0 && (l.dir0[0] != 0 || l.dir0[1] != 0)) {   // RotateYaw en bucle: la direccion gira sobre z
            const float a = fmodf(l.yawRate * now / 1000.0f, 360.0f) * (float)M_PI / 180.0f, cs = cosf(a), sn = sinf(a);
            gl[l.pl].dir[0] = l.dir0[0] * cs - l.dir0[1] * sn; gl[l.pl].dir[1] = l.dir0[0] * sn + l.dir0[1] * cs;
            SpotLightViewMatrix(gl[l.pl].dir, gl[l.pl].angles[2], gl[l.pl].viewMatrix.m);   // como CG_PrimaryLight con rotationLimit < 1
        }
        if (dirty) Apply(l);
        if (l.spin >= 0) H15F_SpinFrame(l, now);   // OLA 15F: el modelo giratorio sigue el yaw de la luz (run_spin_model, cada 0,01 s)
    }
    (void)dt;
}
std::vector<std::vector<GfxPackedVertex>> s_cache;   // vertices en mundo por modelo y superficie (LOD 0..3)
std::vector<XModel *> s_cacheModel; std::vector<int> s_cacheLod;

// ---- OLA 15F: pose de los script_model de cliente que se mueven (relojes de _ambient.csc, luces giratorias de _lights.csc)
void SetPose(int j, const float *ang)
{
    ClModel &m = s_models[j]; memcpy(m.ang, ang, 12); AnglesToAxis(ang, m.axis);
    if (j < (int)s_cacheModel.size()) s_cacheModel[j] = nullptr;   // fuerza a recalcular sus vertices en mundo
}
int AddModel(const char *name, const float *org, const float *ang, const char *tn)
{
    XModel *xm = DB_FindXAssetHeader(ASSET_TYPE_XMODEL, (char *)name, 1, -1).model; if (!xm) return -1;
    ClModel m; m.name = name; m.tn = tn ? tn : ""; m.m = xm; memcpy(m.org, org, 12); memcpy(m.ang, ang, 12); AnglesToAxis(ang, m.axis);
    const int j = (int)s_models.size(), key = 1024 + (int)(rgp.world ? rgp.world->dpvs.smodelCount : 0) + j;
    m.key = H5L_MaxKey && key <= H5L_MaxKey() ? key : -2;
    s_models.push_back(m); return j;
}
// run_spin_model("on"): spawn(script_model) en el origen del modelo de la luz con sus angulos, SetModel(spin_model) y
// PlayFXOnTag(level._effect[spin_fx], spin_tag); despues, cada 0,01 s, angles = (x del modelo, light.angles[1], z del modelo).
// level._effect["test_spin_fx"] = env/light/fx_light_warning (zombie_pentagon_fx.csc). El FX se lanza orientado en la etiqueta y no
// sigue el giro (el port no tiene DObj de cliente para atornillarlo): queda fijo donde nace.
void H15F_SpinOn(ClLight &l)
{
    static const int on = getenv("H15F_SPIN") ? atoi(getenv("H15F_SPIN")) : 1; if (!on || l.spin >= 0 || l.model < 0) return;
    const ClModel src = s_models[l.model];
    l.spin = AddModel(l.spinModel.c_str(), src.org, src.ang, "spin_model"); if (l.spin < 0) return;
    const char *fxName = l.spinFx == "test_spin_fx" ? "env/light/fx_light_warning" : nullptr;
#ifdef H5FX
    const FxEffectDef *fx = fxName ? FX_Register((char *)fxName) : nullptr;
#else
    const FxEffectDef *fx = nullptr;   // sin efectos compilados (H5FX=0)
#endif
    float tag[4][3]; memcpy(tag, s_models[l.spin].axis, 36); memcpy(tag[3], src.org, 12);
    if (!l.spinTag.empty()) {
        unsigned char bi = 0xFF; const unsigned nm = SL_FindString(l.spinTag.c_str(), SCRIPTINSTANCE_SERVER);
        const XModel *xm = s_models[l.spin].m;
        if (nm && XModelGetBoneIndex(xm, nm, 0, &bi) && bi != 0xFF) {
            const DObjAnimMat *bp = XModelGetBasePose(xm);
            if (bp) for (int k = 0; k < 3; ++k) tag[3][k] = src.org[k] + bp[bi].trans[0] * s_models[l.spin].axis[0][k] + bp[bi].trans[1] * s_models[l.spin].axis[1][k] + bp[bi].trans[2] * s_models[l.spin].axis[2][k];
        }
    }
#ifdef H5FX
    if (fx) FX_PlayOrientedEffect(0, fx, FX_GetSystem(0)->system.msecNow, tag[3], tag);
#endif
    fprintf(stderr, "[H15F] luz %d: modelo giratorio '%s' (%d) y FX %s en %.0f %.0f %.0f\n", l.pl, l.spinModel.c_str(), l.spin, fx ? fxName : "(ninguno)", tag[3][0], tag[3][1], tag[3][2]);
}
void H15F_SpinFrame(ClLight &l, int now)
{
    if (l.spin < 0 || l.model < 0) return;
    const float yaw = l.yaw0 + (l.yawRate != 0 ? fmodf(l.yawRate * now / 1000.0f, 360.0f) : 0.0f);
    const float ang[3] = { s_models[l.model].ang[0], yaw, s_models[l.model].ang[2] };
    SetPose(l.spin, ang);
}
// _ambient.csc clocks_init / clock_run: hour_hand, minute_hand, second_hand (targetname) giran con la hora del sistema
// (GetSystemTime local; las horarias con script_noteworthy = zona usan GetSystemTime(1) = GMT + desfase, como el script, incluido
// que solo resta 12 una vez). RotatePitch suma al pitch: primero hand_time * rotate + first_rotate, luego rotate_bit por segundo.
// H15F_CLOCK="hh:mm:ss" fija la hora (pruebas reproducibles; la GMT se toma igual). H15F_CLOCKS=0 los deja quietos.
void ClocksFrame()
{
    static int on = -1; static long t0; static int lh, lm, ls, gh; if (on < 0) {
        on = getenv("H15F_CLOCKS") ? atoi(getenv("H15F_CLOCKS")) : 1; t0 = (long)time(nullptr);
        time_t tt = (time_t)t0; struct tm lt, gt; localtime_r(&tt, &lt); gmtime_r(&tt, &gt); lh = lt.tm_hour; lm = lt.tm_min; ls = lt.tm_sec; gh = gt.tm_hour;
        if (const char *e = getenv("H15F_CLOCK")) { sscanf(e, "%d:%d:%d", &lh, &lm, &ls); gh = lh; } }
    if (!on) return;
    const float el = (float)((long)time(nullptr) - t0);   // segundos enteros transcurridos (un RotatePitch por cambio de GetSystemTime)
    int h12 = lh > 12 ? lh - 12 : lh; if (h12 == 0) h12 = 12;
    for (size_t j = 0; j < s_models.size(); ++j) { ClModel &m = s_models[j]; float add;
        if (m.tn == "hour_hand") {
            int hour = h12;
            if (!m.note.empty()) { std::string z = m.note; for (auto &c : z) c = (char)tolower(c);
                static const struct { const char *n; int o; } tz[] = { { "honolulu", -10 }, { "alaska", -9 }, { "los angeles", -8 }, { "denver", -7 }, { "chicago", -6 },
                    { "new york", -5 }, { "halifax", -4 }, { "greenland", -3 }, { "london", 0 }, { "paris", 1 }, { "helsinki", 2 }, { "moscow", 3 }, { "china", 8 } };
                for (auto &t : tz) if (z == t.n) { hour = gh + t.o; break; }
                if (hour < 1) hour += 12; if (hour > 12) hour -= 12; }
            add = hour * 30.0f + (lm * 60 + ls) * (30.0f / 3600.0f) + el * (30.0f / 3600.0f);
        } else if (m.tn == "minute_hand") add = lm * 6.0f + ls * 0.1f + el * 0.1f;
        else if (m.tn == "second_hand") add = ls * 6.0f + el * 6.0f;
        // zombie_theater_teleporter.csc pack_clock_init / pack_clock_run (OLA M1): reloj del Pack-a-Punch de Kino, desde "pack_clock_start",
        // RotatePitch NEGATIVO de hand_time * rotate + first_rotate y luego rotate_bit por segundo (sin segundero: no tiene spawnflags 2)
        else if (m.tn == "zom_clock_hour_hand" && s_packClock) add = -(h12 * 30.0f + (lm * 60 + ls) * (30.0f / 3600.0f) + el * (30.0f / 3600.0f));
        else if (m.tn == "zom_clock_minute_hand" && s_packClock) add = -(lm * 6.0f + ls * 0.1f + el * 0.1f);
        else continue;
        if (add == m.pitchAdd) continue;
        float ang[3] = { m.ang[0] - m.pitchAdd + add, m.ang[1], m.ang[2] }; m.pitchAdd = add; SetPose((int)j, ang);
    }
}

// ---------------------------------------------------------------- OLA M1: sistemas de cliente de zombie_theater (Kino der Toten)
// Sin VM de CSC: lo mismo que hacen sus clientscripts (rawfiles del ff, recuperados como en la ola 15F), con sus numeros:
//  - zombie_theater.csc: theatre_ZPO_listener (level._power_on, luces del tablero de la caja, notify local "pl1"),
//    theater_light_model_swap (model_lights_on: lights_hang_single -> lights_hang_single_on_nonflkr, zombie_zapper_cagelight ->
//    zombie_zapper_cagelight_on), init_board_lights / theater_box_monitor (sistema "box_indicator": 9 tableros magic_box_loc_light,
//    -1 apagadas, 99 parpadeo 0,25 s al moverse, 98 parpadeo 0,3 s en fire sale, n = verde + FX boxlight_light_ready si hay corriente).
//  - zombie_theater_fx.csc: trap_fx_monitor (e1..e4 electricas = misc/fx_zombie_electric_trap, f1 fuego = fire_trap_med, en los
//    script_struct sin modelo de cada trampa; "<loc>1" enciende, "<loc>0" apaga), projector_screen_fx ("sip": haz del proyector y
//    pantalla; "ps1".."ps3" cambian la bobina), level_fog_init + dog_start_monitor / dog_stop_monitor (setVolFog, h7c_gfx.cpp).
//  - zombie_theater_teleporter.csc: wait_for_black_box (vision "default" mientras dura), wait_for_teleport_aftereffect (uno al azar de
//    fov / shellshock / shellshock_electric / bw / red / flashy / flare), pack_clock_init (reloj del PaP, ClocksFrame).
// No se hace: la extracam (isExtraCam: pantalla del proyector en vivo) ni los vision sets de las salas del huevo de pascua.
//   H19_KINO=0 lo desactiva (vuelve el comportamiento generico).
const FxEffectDef *KFx(const char *name)
{
#ifdef H5FX
    static std::map<std::string, const FxEffectDef *> c; auto it = c.find(name); if (it != c.end()) return it->second;
    const FxEffectDef *fx = FX_Register((char *)name); c[name] = fx;
    fprintf(stderr, "[H19] fx '%s' %s\n", name, fx ? "ok" : "NO esta");
    return fx;
#else
    (void)name; return nullptr;
#endif
}
// SpawnFx(clientNum, fx, origin, 0, forward, up) + triggerfx / PlayFXOnTag sobre un tag_origin con esos angulos
unsigned KPlay(const char *name, const float *org, const float *fwd, const float *up)
{
#ifdef H5FX
    const FxEffectDef *fx = KFx(name); if (!fx) return 0;
    float ax[3][3]; memcpy(ax[0], fwd, 12); memcpy(ax[2], up, 12);
    ax[1][0] = up[1] * fwd[2] - up[2] * fwd[1]; ax[1][1] = up[2] * fwd[0] - up[0] * fwd[2]; ax[1][2] = up[0] * fwd[1] - up[1] * fwd[0];
    return FX_PlayOrientedEffect(0, fx, FX_GetSystem(0)->system.msecNow, org, ax);
#else
    (void)name; (void)org; (void)fwd; (void)up; return 0;
#endif
}
unsigned KPlayAng(const char *name, const float *org, const float *ang)
{
    float f[3], r[3], u[3]; AngleVectors(ang, f, r, u); return KPlay(name, org, f, u);
}
void KKill(unsigned &h)
{
#ifdef H5FX
    if (h) FX_ThroughWithEffect(0, h, false);   // Delete() del ent de FX: deja morir las particulas ya nacidas
#endif
    h = 0;
}
struct KPt { float org[3], ang[3]; unsigned h = 0; };
struct KTrap { std::string loc, fx; std::vector<KPt> pts; };
std::vector<KTrap> s_ktraps;
struct KBox { int model; int loc; float org[3], ang[3]; unsigned fx = 0; };
std::vector<KBox> s_kbox;
const char *kBoxLocs[9] = { "start_chest_loc", "foyer_chest_loc", "crematorium_chest_loc", "alleyway_chest_loc", "control_chest_loc",
                            "stage_chest_loc", "dressing_chest_loc", "dining_chest_loc", "theater_chest_loc" };
int s_kboxInd = -1, s_kflashMs = 0, s_kflashNext = 0; bool s_kflashGreen;
KPt s_kproj, s_kscreen; bool s_kprojOk, s_kscreenOk, s_kprojOn; unsigned s_kprojFx, s_kscreenFx;
struct KStep { int at; std::string vis; float sec; float fov; };   // pasos de los efectos tras teletransporte
std::vector<KStep> s_ksteps; std::string s_ksavedVis;
const char *H7Naked();

void KinoFog(int which, float sec)   // 0 = level_fog_init / dog_stop_monitor, 1 = dog_start_monitor
{
    if (!H7C_ClientVolFog) return;
    // setVolFog(start, halfwayDist, halfwayHeight, baseHeight, r, g, b, colorScale, sunR, sunG, sunB, dirX, dirY, dirZ, startAng, endAng, time, maxOpacity)
    static const float lv[18] = { 155.625f, 2332.79f, 677.241f, 23.2733f, 0.639216f, 0.639216f, 0.639216f, 1, 0.243137f, 0.270588f, 0.270588f, 0.291692f, -0.720765f, 0.628819f, 0, 0, 0, 1 };
    static const float dg[18] = { 0, 302.622f, 306.395f, 344.622f, 0.458824f, 0.556863f, 0.619608f, 1, 0.243137f, 0.270588f, 0.270588f, 0.291692f, -0.720765f, 0.628819f, 0, 0, 0, 0.65f };
    H7C_ClientVolFog(which ? dg : lv, (int)(sec * 1000.0f));
}
void KBoxSet(int i, int color /*0 off 1 green 2 red*/, bool fx)
{
    KBox &b = s_kbox[i]; KKill(b.fx);
    SetModel(b.model, color == 1 ? "zombie_zapper_cagelight_green" : color == 2 ? "zombie_zapper_cagelight_red" : "zombie_zapper_cagelight");
    if (fx && color) { float o[3] = { b.org[0], b.org[1], b.org[2] - 10.0f }; b.fx = KPlayAng("maps/zombie/fx_zombie_theater_lightboard_green", o, b.ang); }
}
void KBoxLoc(int loc, int color, bool fx) { if (loc < 0 || loc >= 9) return; for (size_t i = 0; i < s_kbox.size(); ++i) if (s_kbox[i].loc == loc) KBoxSet((int)i, color, fx); }
void KBoxAllOff() { s_kflashMs = 0; for (size_t i = 0; i < s_kbox.size(); ++i) KBoxSet((int)i, 0, false); }
void KBoxMonitor(int st, int now)   // theater_box_monitor
{
    if (st == -1) KBoxAllOff();
    else if (st == 99 || st == 98) { s_kflashMs = st == 99 ? 250 : 300; s_kflashNext = now + s_kflashMs; s_kflashGreen = true; }   // flash_lights: wait, verdes, wait, apagadas
    else { if (st < 0 || st > 9) return; KBoxAllOff(); s_kboxInd = st; if (s_power) KBoxLoc(st, 1, true); }
}
void KinoInit(const std::vector<Kv> &ents)
{
    std::map<std::string, std::string> trapFx = { { "dressing_room_trap", "e4" }, { "vip_room_trap", "e2" }, { "foyer_room_trap", "e1" }, { "control_room_trap", "e3" }, { "crematorium_room_trap", "f1" } };
    for (auto &t : trapFx) { KTrap k; k.loc = t.second; k.fx = t.second[0] == 'f' ? "maps/zombie/fx_zombie_fire_trap_med" : "misc/fx_zombie_electric_trap"; s_ktraps.push_back(k); }
    for (const Kv &k : ents) {
        const char *cn = Str(k, "classname"), *tn = Str(k, "targetname"); if (!cn || !tn) continue;
        if (!strcmp(cn, "script_struct")) {
            KPt p; Vec(k, "origin", p.org); memset(p.ang, 0, 12); Vec(k, "angles", p.ang);
            auto it = trapFx.find(tn);
            if (it != trapFx.end() && !Str(k, "model")) for (auto &t : s_ktraps) if (t.loc == it->second) t.pts.push_back(p);
            if (!strcmp(tn, "magic_box_loc_light") && Str(k, "script_noteworthy")) {   // init_board_lights: spawn script_model zombie_zapper_cagelight
                int loc = -1; for (int i = 0; i < 9; ++i) if (!strcmp(kBoxLocs[i], Str(k, "script_noteworthy"))) loc = i;
                const int m = AddModel("zombie_zapper_cagelight", p.org, p.ang, "magic_box_loc_light");
                if (m >= 0) { KBox b; b.model = m; b.loc = loc; memcpy(b.org, p.org, 12); memcpy(b.ang, p.ang, 12); s_kbox.push_back(b); }
            }
            if (!strcmp(tn, "struct_theater_projector_beam")) { s_kproj = p; s_kprojOk = true; }
            if (!strcmp(tn, "struct_theater_screen")) { s_kscreen = p; s_kscreenOk = true; }
        }
    }
    size_t np = 0; for (auto &t : s_ktraps) np += t.pts.size();
    KinoFog(0, 0.1f);   // level_fog_init
    fprintf(stderr, "[H19] Kino: %zu trampas (%zu puntos de FX), %zu luces de tablero de la caja, proyector %s, pantalla %s, niebla de cliente %s\n",
        s_ktraps.size(), np, s_kbox.size(), s_kprojOk ? "si" : "NO", s_kscreenOk ? "si" : "NO", H7C_ClientVolFog ? "si" : "NO");
}
void KinoNotify(const std::string &ev, int now)
{
    if (ev == "ZPO") {   // theatre_ZPO_listener + theater_light_model_swap
        s_power = true;
        for (auto &m : s_models) if (m.tn == "model_lights_on") {
            if (m.name == "lights_hang_single") SetModel(&m - &s_models[0], "lights_hang_single_on_nonflkr");
            else if (m.name == "zombie_zapper_cagelight") SetModel(&m - &s_models[0], "zombie_zapper_cagelight_on"); }
        if (s_kboxInd != -1) KBoxMonitor(s_kboxInd, now);
        for (auto &l : s_lights) if (l.mixer && l.mixerEvent == ev && l.mixT0 < 0 && !l.side1) l.mixT0 = now;
        KinoNotify("pl1", now);   // level notify("pl1"): luces de corriente (mezcladores pl1)
        return;
    }
    if (ev.size() == 3 && (ev[0] == 'e' || ev[0] == 'f') && isdigit((unsigned char)ev[1]) && (ev[2] == '0' || ev[2] == '1')) {   // trap_fx_monitor
        const std::string loc = ev.substr(0, 2); const bool on = ev[2] == '1';
        for (auto &t : s_ktraps) if (t.loc == loc) for (auto &p : t.pts) {
            KKill(p.h);
            if (on) { float f[3], r[3], u[3]; AngleVectors(p.ang, f, r, u); p.h = KPlay(t.fx.c_str(), p.org, f, u); }
        }
        fprintf(stderr, "[H19] trampa %s %s\n", loc.c_str(), on ? "encendida" : "apagada");
        return;
    }
    if (ev == "sip" && !s_kprojOn) {   // projector_screen_fx
        s_kprojOn = true;
        if (s_kprojOk) s_kprojFx = KPlayAng("maps/zombie/fx_zombie_theater_projector_beam", s_kproj.org, s_kproj.ang);
        if (s_kscreenOk) s_kscreenFx = KPlayAng("maps/zombie/fx_zombie_theater_screen_0", s_kscreen.org, s_kscreen.ang);
    }
    if ((ev == "ps1" || ev == "ps2" || ev == "ps3") && s_kprojOn && s_kscreenOk)   // projector_reel_swap: PlayFXOnTag encima del anterior
        KPlayAng(("maps/zombie/fx_zombie_theater_screen_" + ev.substr(2)).c_str(), s_kscreen.org, s_kscreen.ang);
    if (ev == "dog_start") KinoFog(1, 7.0f);
    if (ev == "dog_stop") KinoFog(0, 7.0f);
    if (ev == "pack_clock_start") s_packClock = true;
    if (H7_VisionSetNaked && EnvOn("H7E_VISION")) {
        if (ev == "black_box_start") { s_ksavedVis = H7Naked(); H7_VisionSetNaked("default", 0); }
        else if (ev == "black_box_end" && !s_ksavedVis.empty()) { H7_VisionSetNaked(s_ksavedVis.c_str(), 0); s_ksavedVis.clear(); }
        else if (ev == "teleport_ae_start") {   // wait_for_teleport_aftereffect: uno al azar (theaterAftereffectOverride = -1)
            const std::string sv = H7Naked(); s_ksteps.clear();
            int pick = (int)(Rand01() * 7.0f); if (pick > 6) pick = 6;
            if (const char *e = getenv("H19_AE")) pick = atoi(e);
            static const char *names[7] = { "fov", "shellshock", "shellshock_electric", "bw", "red", "flashy", "flare" };
            fprintf(stderr, "[H19] efecto tras teletransporte: %s\n", names[pick]);
            if (pick == 0) for (int i = 0; i <= 30; ++i) s_ksteps.push_back({ now + i * 17, "", 0, 30.0f + 35.0f * (i * 0.017f / 0.5f > 1 ? 1 : i * 0.017f / 0.5f) });
            else if (pick == 3) { s_ksteps.push_back({ now, "cheat_bw_invert_contrast", 0.4f, 0 }); s_ksteps.push_back({ now + 1250, sv, 1, 0 }); }
            else if (pick == 4) { s_ksteps.push_back({ now, "zombie_turned", 0.4f, 0 }); s_ksteps.push_back({ now + 1250, sv, 1, 0 }); }
            else if (pick == 5) { const char *v[4] = { "cheat_bw_invert_contrast", "cheat_bw_contrast", "cheat_invert_contrast", "cheat_contrast" };
                for (int i = 0; i < 4; ++i) s_ksteps.push_back({ now + 400 * i, v[i], 0.1f, 0 }); s_ksteps.push_back({ now + 1600, sv, 5, 0 }); }
            else if (pick == 6) { s_ksteps.push_back({ now, "flare", 0.4f, 0 }); s_ksteps.push_back({ now + 1250, sv, 1, 0 }); }
        }
    }
    for (auto &l : s_lights) if (l.mixer && l.mixerEvent == ev && l.mixT0 < 0 && !l.side1) l.mixT0 = now;   // mixer_level_activate (tl*, sip, ft1...)
}
bool KinoSys(const char *sys, const std::string &st, int now)
{
    if (I_stricmp(sys, "box_indicator")) return false;
    KBoxMonitor(atoi(st.c_str()), now); return true;
}
void KinoFrame(int now)
{
    if (s_kflashMs && now >= s_kflashNext) {
        for (int i = 0; i < 9; ++i) KBoxLoc(i, s_kflashGreen ? 1 : 0, false);
        s_kflashGreen = !s_kflashGreen; s_kflashNext = now + s_kflashMs;
    }
    for (auto &k : s_ksteps) if (k.at >= 0 && now >= k.at) {
        if (!k.vis.empty() && H7_VisionSetNaked) H7_VisionSetNaked(k.vis.c_str(), k.sec);
        if (k.fov > 0) { char b[32]; snprintf(b, sizeof b, "%.2f", k.fov); Dvar_SetFromStringByName("cg_fov", b); }
        k.at = -1;
    }
}
const char *H7Naked() { const char *n = H7_VisionNakedName ? H7_VisionNakedName() : nullptr; return n && *n ? n : "zombie_theater"; }
} // namespace

void H7E_ClientFrame()
{
    if (!s_init) ClientInit();
    if (!s_on) return;
    // una sola vez, encadenado al anterior (OLA M1: reinstalarlo cada frame hacia ciclos con h13_hud y perdia el del sonido)
    static bool inst; if (!inst) { inst = true; s_prevHook = g_h0ServerCmdHook; g_h0ServerCmdHook = ServerCmd; }
    static int last = -1; const int now = level.time; const int dt = last < 0 ? 0 : now - last; last = now;
    if (H7_VisionTime) H7_VisionTime(now);
    float cam[3], ang[3]; H4_GetCam(cam, ang);
    LightsFrame(now, dt, cam);
    ClocksFrame();   // OLA 15F: agujas de los relojes
    if (s_kino) KinoFrame(now);   // OLA M1
    {   // H7E_NOTIFY="ms:evento,..." (prueba): inyecta levelNotify de cliente (p.ej. "100:vis3") como si llegaran del servidor
        static std::vector<std::pair<int, std::string>> q; static bool parsed;
        if (!parsed) { parsed = true; if (const char *e = getenv("H7E_NOTIFY")) { std::string a = e; size_t p = 0;
            while (p < a.size()) { size_t c = a.find(',', p); if (c == std::string::npos) c = a.size(); std::string it = a.substr(p, c - p); p = c + 1;
                size_t k = it.find(':'); if (k != std::string::npos) q.push_back({ atoi(it.c_str()), it.substr(k + 1) }); } } }
        for (auto &n : q) if (n.first >= 0 && now >= n.first) { fprintf(stderr, "[H7E] H7E_NOTIFY '%s' t=%d\n", n.second.c_str(), now); LevelNotify(n.second, now); n.first = -1; }
    }
    if (s_powerAt >= 0 && !s_powerDone && now >= s_powerAt) {   // prueba: el jugador pulsa el interruptor (electric_switch: trig waittill("trigger", user))
        s_powerDone = true; int found = 0;
        for (int e = 0; e < level.num_entities; ++e) { gentity_s *ent = &g_entities[e];
            if (!ent->r.inuse || !ent->targetname || strcmp(SL_ConvertToString(ent->targetname, SCRIPTINSTANCE_SERVER), "use_elec_switch")) continue;
            Scr_AddEntity(&g_entities[0], SCRIPTINSTANCE_SERVER); Scr_Notify(ent, scr_const.trigger, 1); ++found; break; }
        fprintf(stderr, "[H7E] H7E_POWER: interruptor %s en t=%d\n", found ? "pulsado" : "NO encontrado", now);
    }
}
void H7E_PrepareClientModels()
{
    if (!s_on || s_models.empty() || !H5L_Prepare) return;
    float cam[3], ang[3], fwd[3]; H4_GetCam(cam, ang); AngleVectors(ang, fwd, nullptr, nullptr);
    static const float half[3] = { 16, 16, 16 };
    for (auto &m : s_models) {
        const float dx = m.org[0] - cam[0], dy = m.org[1] - cam[1], dz = m.org[2] - cam[2], d = sqrtf(dx * dx + dy * dy + dz * dz), r = m.m->radius + 8.0f;
        m.vis = d - r <= 1200.0f && (d <= r || dx * fwd[0] + dy * fwd[1] + dz * fwd[2] >= 0.3f * d - r);   // mismo tope de distancia que los smodels
        if (m.vis && m.key >= 0) H5L_Prepare(m.key, m.org, half, false);   // CG_General: R_AddDObjToScene con lightingOrigin = origen
    }
}
void H7E_DrawClientModels(unsigned char techType)
{
    if (!s_on || s_models.empty()) return;
    static DObjSkelMat *skel; if (!skel && posix_memalign((void **)&skel, 16, 128 * sizeof(DObjSkelMat))) return;
    static GfxPackedVertex *tmp; static int tmpCap;
    float cam[3], ang[3]; H4_GetCam(cam, ang);
    if (s_cache.size() < s_models.size() * 64) { s_cache.resize(s_models.size() * 64); s_cacheModel.assign(s_models.size(), nullptr); s_cacheLod.assign(s_models.size(), -1); }
    for (size_t j = 0; j < s_models.size(); ++j) {
        ClModel &m = s_models[j]; if (!m.vis) continue;
        const int lod = XModelGetLodForDist(m.m, Vec3Distance(m.org, cam), Vec3Distance(m.org, cam), false); if (lod < 0) continue;
        const int nb = XModelNumBones(m.m); if (nb > 128) continue;
        XSurface *xs; const int ns = XModelGetSurfaces(m.m, &xs, lod); PTR32(Material) *skins = XModelGetSkins(m.m, lod);
        if (s_cacheModel[j] != m.m || s_cacheLod[j] != lod) { for (int s = 0; s < 64; ++s) s_cache[j * 64 + s].clear(); s_cacheModel[j] = m.m; s_cacheLod[j] = lod; }
        if (H5L_Bind) H5L_Bind(m.key);
        bool built = false;
        for (int s = 0; s < ns && s < 64; ++s) {
            Material *mtl = skins ? skins[s].get() : nullptr; if (!mtl || !xs[s].vertCount) continue;
            std::vector<GfxPackedVertex> &v = s_cache[j * 64 + s];
            if (v.empty()) {
                if (!built) { built = true; for (int b = 0; b < nb; ++b) { DObjSkelMat &o = skel[b];
                    for (int a = 0; a < 3; ++a) { for (int c = 0; c < 3; ++c) o.axis[a][c] = m.axis[a][c]; o.axis[a][3] = 0; }
                    memcpy(o.origin, m.org, 12); o.origin[3] = 1.0f; } }
                if ((int)xs[s].vertCount > tmpCap) { free(tmp); tmpCap = xs[s].vertCount + 256; if (posix_memalign((void **)&tmp, 16, tmpCap * sizeof(GfxPackedVertex))) { tmp = nullptr; tmpCap = 0; return; } }
                R_SkinXSurfaceSkinned(&xs[s], skel, tmp); v.assign(tmp, tmp + xs[s].vertCount);
            }
            H4_DrawSkinnedSurf(mtl, &xs[s], v.data(), techType);
        }
    }
}
// OLA 9 (h9_video.cpp): SetModel del CSC sobre los script_model de cliente con ese targetname (pantallas de la caja). ordinal -1 = todos;
// name nulo = solo contar. Devuelve cuantos hay con ese targetname.
int H7E_ClientModelByTn(const char *tn, int ordinal, const char *name)
{
    int k = 0; for (size_t j = 0; j < s_models.size(); ++j) if (s_models[j].tn == tn) { if (name && (ordinal < 0 || ordinal == k)) SetModel((int)j, name); ++k; }
    return k;
}
#else
void H7E_ClientFrame() {}
#endif
