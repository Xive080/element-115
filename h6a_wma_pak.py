#!/usr/bin/env python3
# H6a (sonido): transcodifica los sonidos cargados xWMA (formato 7) a MS-ADPCM en un paquete que lee h5_audio.cpp.
# El exe los decodifica con el xWMA de XAudio2; aqui no hay decodificador WMA enlazado (solo bibliotecas permisivas en el
# binario), asi que se preparan una vez fuera de linea con la herramienta ffmpeg (proceso externo, no se enlaza ni se vende).
#
#   1) volcado: H6A_SURVEY=1 H6A_DUMPWMA=<dir> con una partida corta del mapa (escribe <dir>/NNNNN.xwma + index.tsv)
#   2) python3 h6a_wma_pak.py <dir> [salida]   (salida por defecto: $P3_GAMEDATA/h6a/wma_adpcm.pak; P3_GAMEDATA obligatorio)
#   3) python3 h6a_wma_pak.py --verify <pak> [--merge-check]   (OLA 14: cabecera, orden, rangos y decodificacion de CADA sonido con
#      ffmpeg; sale con 1 si hay alguno corrupto).  Todo junto y la copia al Fold: h6a_pak.sh (build | verify | fold).
#   OLA 14: varios volcados (otros mapas) se funden en un paquete: python3 h6a_wma_pak.py <dir1> <dir2> ... -o salida.pak
#
# Formato del paquete (little endian): 'H6AW', u32 version=1, u32 n; n entradas de 32 bytes ordenadas por hash
# {u32 hash FNV-1a del nombre en minusculas con '/', u32 frames, u32 rate, u16 canales, u16 bloque, u64 offset, u32 bytes,
# u32 0}; despues los datos MS-ADPCM (bloque 256 * canales, 500 frames por bloque, coeficientes estandar).
import array
import os
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor


def fnv1a(name):
    h = 0x811C9DC5
    for b in name.lower().replace('\\', '/').encode('latin-1', 'replace'):
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def wav_data(blob):
    if blob[:4] != b'RIFF' or blob[8:12] != b'WAVE':
        raise ValueError('no es WAV')
    p = 12
    while p + 8 <= len(blob):
        cid, size = blob[p:p + 4], struct.unpack_from('<I', blob, p + 4)[0]
        if cid == b'data':
            return blob[p + 8:p + 8 + size]
        p += 8 + size + (size & 1)
    raise ValueError('WAV sin data')


def convert(src, ch):
    out = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', src, '-c:a', 'adpcm_ms',
                          '-block_size', str(256 * ch), '-f', 'wav', '-'], capture_output=True, check=True).stdout
    return wav_data(out)


MSADPCM_COEF = ((256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232))


def adpcm_wav(data, rate, ch):
    """Envuelve un sonido del paquete en un WAV MS-ADPCM (igual que lo decodifica h5_audio.cpp: bloque 256*ch, 500 frames)."""
    fmt = struct.pack('<HHIIHHHHH', 2, ch, rate, rate * 256 * ch // 500, 256 * ch, 4, 32, 500, 7)
    fmt += b''.join(struct.pack('<hh', a, b) for a, b in MSADPCM_COEF)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data
    return b'RIFF' + struct.pack('<I', len(body)) + body


def read_pak(path):
    blob = open(path, 'rb').read()
    if len(blob) < 12 or blob[:4] != b'H6AW':
        raise ValueError('%s: no es un paquete H6AW' % path)
    ver, n = struct.unpack_from('<II', blob, 4)
    if ver != 1 or 12 + 32 * n > len(blob):
        raise ValueError('%s: version %d o cabecera truncada (%d entradas)' % (path, ver, n))
    ents = [struct.unpack_from('<IIIHHQII', blob, 12 + 32 * i) for i in range(n)]
    return blob, ents


def check_one(blob, e):
    h, frames, rate, ch, align, off, size, pad = e
    if ch not in (1, 2) or align != 256 * ch or not (8000 <= rate <= 96000) or off + size > len(blob) or size == 0:
        return 'entrada invalida (ch %d bloque %d rate %d off %d bytes %d)' % (ch, align, rate, off, size)
    r = subprocess.run(['ffmpeg', '-hide_banner', '-v', 'error', '-xerror', '-f', 'wav', '-i', '-', '-f', 's16le', '-acodec',
                        'pcm_s16le', '-'], input=adpcm_wav(blob[off:off + size], rate, ch), capture_output=True)
    if r.returncode or r.stderr.strip():
        return 'ffmpeg: ' + (r.stderr.decode('latin-1', 'replace').strip().splitlines() or ['codigo %d' % r.returncode])[0]
    got = len(r.stdout) // (2 * ch)
    if got < frames:
        return 'decodifica %d frames de %d' % (got, frames)
    pcm = array.array('h', r.stdout[:frames * 2 * ch])
    full = sum(1 for v in pcm if v >= 32767 or v <= -32768)
    if pcm and full > len(pcm) // 50:   # >2 % a fondo de escala: AVISO, no fallo (medido: los 4 de pentagon ya saturan en el xWMA original)
        return 'AVISO %.1f %% de muestras a fondo de escala' % (100.0 * full / len(pcm))
    return None


def verify(path):
    blob, ents = read_pak(path)
    bad = []
    hashes = [e[0] for e in ents]
    if hashes != sorted(hashes) or len(set(hashes)) != len(hashes):
        bad.append(('cabecera', 'hashes sin ordenar o repetidos (PakLookup busca en binario)'))
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        res = list(ex.map(lambda e: check_one(blob, e), ents))
    warn = []
    for e, r in zip(ents, res):
        if r:
            (warn if r.startswith('AVISO') else bad).append(('%08x' % e[0], r))
    secs = sum(e[1] / float(e[2]) for e in ents if e[2])
    print('verificacion %s: %d sonidos, %.1f MB, %.0f s de audio, %d corruptos, %d avisos' % (path, len(ents), len(blob) / 1048576.0, secs, len(bad), len(warn)))
    for k, r in bad[:50]:
        print('  CORRUPTO %s: %s' % (k, r))
    for k, r in warn[:50]:
        print('  %s: %s' % (k, r))
    return 1 if bad else 0


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == '--verify':
        sys.exit(verify(sys.argv[2]))
    args = sys.argv[1:]
    dst = None
    if '-o' in args:
        i = args.index('-o')
        dst = args[i + 1]
        del args[i:i + 2]
    if not args:
        sys.exit('uso: h6a_wma_pak.py <dir del volcado> [salida.pak] | <dir1> <dir2> ... -o salida.pak | --verify <pak>')
    dirs = [a for a in args if os.path.isdir(a)]
    if dst is None and len(args) == 2 and not os.path.isdir(args[1]):
        dst = args[1]
    if dst is None:
        gd = os.environ.get('P3_GAMEDATA')
        if not gd:
            sys.exit('falta P3_GAMEDATA (o la ruta de salida)')
        dst = os.path.join(gd, 'h6a', 'wma_adpcm.pak')
    rows, seen = [], set()
    for d in dirs:   # varios volcados (mapas): el primero que trae un nombre manda; los repetidos son el mismo asset
        with open(os.path.join(d, 'index.tsv'), encoding='latin-1') as f:
            for line in f:
                idx, name, frames, rate, ch = line.rstrip('\n').split('\t')
                if name.lower() in seen:
                    continue
                seen.add(name.lower())
                rows.append((os.path.join(d, idx + '.xwma'), name, int(frames), int(rate), int(ch)))
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        datas = list(ex.map(lambda r: convert(r[0], r[4]), rows))
    ents = {}
    for (src, name, frames, rate, ch), data in zip(rows, datas):
        h = fnv1a(name)
        if h in ents and ents[h][0].lower() != name.lower():
            sys.exit('colision de hash: %s / %s' % (ents[h][0], name))
        blocks = len(data) // (256 * ch) + (1 if len(data) % (256 * ch) else 0)
        frames = min(frames, blocks * 500)
        ents[h] = (name, frames, rate, ch, data)
    keys = sorted(ents)
    head = 12 + 32 * len(keys)
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(b'H6AW' + struct.pack('<II', 1, len(keys)))
        off = head
        for h in keys:
            name, frames, rate, ch, data = ents[h]
            f.write(struct.pack('<IIIHHQII', h, frames, rate, ch, 256 * ch, off, len(data), 0))
            off += len(data)
        for h in keys:
            f.write(ents[h][4])
    os.replace(tmp, dst)
    total = sum(len(e[4]) for e in ents.values())
    print('%d sonidos, %.1f MB de MS-ADPCM -> %s' % (len(keys), total / 1048576.0, dst))


if __name__ == '__main__':
    main()
