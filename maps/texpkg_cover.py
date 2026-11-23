#!/usr/bin/env python3
# texpkg_cover.py (OLA T1): cobertura de un tex.pkg frente a una grabacion V9_TEXREC de otra partida.
#   texpkg_cover.py <tex.pkg> <recdir>
# Lee el indice del paquete (formato en ../../vk/vk_texcache_fmt.h: cabecera de 32 B, entradas de 32 B ordenadas) y los niveles
# grabados (<recdir>/<hash16>_<fmt8>_<w>x<h>.lvl y order.log "texId level hash fmt w h"). Una textura solo usa el paquete si TODOS sus
# niveles estan dentro (vk_texture Materialize, allHit): si falta uno, la textura entera cae a RGBA8.
# Imprime niveles y texturas que faltan (las texturas con algun nivel fuera, con su tamano) y sale con 0. Solo lectura.
import os, struct, sys

FMT = {0x31545844: 'DXT1', 0x33545844: 'DXT3', 0x35545844: 'DXT5'}

def pkg_keys(path):
    with open(path, 'rb') as f:
        h = f.read(32)
        if h[:8] != b'BO1TCPK1':
            sys.exit('texpkg_cover: %s no es un tex.pkg' % path)
        _ver, count, index_off, _data_off = struct.unpack('<IIQQ', h[8:32])
        f.seek(index_off)
        raw = f.read(count * 32)
    keys = set()
    for i in range(count):
        hv, _off, _size, src, _vk, w, hh = struct.unpack_from('<QQIIIHH', raw, i * 32)
        keys.add((hv, src, w, hh))
    return keys

def main():
    if len(sys.argv) != 3:
        sys.exit('uso: texpkg_cover.py <tex.pkg> <recdir>')
    keys = pkg_keys(sys.argv[1])
    rec = sys.argv[2]
    tex = {}   # texId -> lista de (clave, nivel)
    with open(os.path.join(rec, 'order.log')) as f:
        for line in f:
            p = line.split()
            if len(p) != 6:
                continue
            k = (int(p[2], 16), int(p[3], 16), int(p[4]), int(p[5]))
            tex.setdefault(int(p[0]), []).append((k, int(p[1])))
    levels = set(k for v in tex.values() for k, _ in v)
    miss_lv = sorted(levels - keys)
    miss_tex = [(t, v) for t, v in tex.items() if any(k not in keys for k, _ in v)]
    print('paquete: %d niveles; grabados en la partida de verificacion: %d niveles unicos, %d texturas' % (len(keys), len(levels), len(tex)))
    print('niveles que faltan: %d   texturas que caen a RGBA8: %d' % (len(miss_lv), len(miss_tex)))
    for t, v in sorted(miss_tex, key=lambda x: -max(k[2] * k[3] for k, _ in x[1]))[:40]:
        big = max(v, key=lambda x: x[0][2] * x[0][3])[0]
        out = sum(1 for k, _ in v if k not in keys)
        print('  textura %d: %s %dx%d, %d/%d niveles fuera' % (t, FMT.get(big[1], '%08x' % big[1]), big[2], big[3], out, len(v)))
    return 0

if __name__ == '__main__':
    sys.exit(main())
