#!/usr/bin/env python3
# OLA 15C: analisis de una grabacion H6A_WAV (estereo 16 bits 48 kHz) junto a su registro [H6A] del mismo proceso.
#   python3 -I h6a_wav_analisis.py <grabacion.wav> <registro.log> <carpeta_salida>
# Escribe en la carpeta: espectrograma.png (ffmpeg showspectrumpic), nivel.png (RMS/pico por segundo, ffmpeg no hace falta),
# resumen.txt (picos, recortes, silencios, RMS por tramo, sincronia de disparos) y por_segundo.csv.
# Sincronia: cada '[H6A] play t=...' de un disparo del jugador (wpn_*_fire_plr) se busca el ataque en la grabacion (subida de la
# envolvente de 5 ms) en +-150 ms; el WAV y los t= del registro arrancan los dos en SD_Init (h5_audio.cpp).
import array
import math
import os
import re
import subprocess
import sys
import wave


def main():
    wav, log, out = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(out, exist_ok=True)
    w = wave.open(wav, 'rb')
    ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
    a = array.array('h'); a.frombytes(w.readframes(n)); w.close()
    if sys.byteorder == 'big':
        a.byteswap()
    L, R = a[0::ch], a[1::ch] if ch > 1 else a[0::ch]
    dur = n / rate
    clip = sum(1 for v in a if v >= 32767 or v <= -32768)
    peak = max(max(a), -min(a)) / 32768.0 if len(a) else 0
    rows = []
    sil = 0
    for s in range(int(dur)):
        seg_l, seg_r = L[s * rate:(s + 1) * rate], R[s * rate:(s + 1) * rate]
        if not seg_l:
            break
        rl = math.sqrt(sum(v * v for v in seg_l) / len(seg_l)) / 32768.0
        rr = math.sqrt(sum(v * v for v in seg_r) / len(seg_r)) / 32768.0
        pk = max(max(seg_l), -min(seg_l), max(seg_r), -min(seg_r)) / 32768.0
        db = 20 * math.log10(max((rl + rr) / 2, 1e-9))
        if db < -60:
            sil += 1
        rows.append((s, db, 20 * math.log10(max(pk, 1e-9)), 20 * math.log10(max(rl, 1e-9)) - 20 * math.log10(max(rr, 1e-9))))
    with open(os.path.join(out, 'por_segundo.csv'), 'w') as f:
        f.write('s,rms_db,pico_db,balance_LR_db\n')
        for r in rows:
            f.write('%d,%.1f,%.1f,%.1f\n' % r)
    # envolvente de 5 ms para la sincronia
    hop = rate // 200
    env = []
    for i in range(0, len(L) - hop, hop):
        env.append(max(max(abs(v) for v in L[i:i + hop]), max(abs(v) for v in R[i:i + hop])))
    plays = []
    alias_count = {}
    for line in open(log, errors='replace'):
        m = re.match(r"\[H6A\] play t=([0-9.]+)s voz=\d+ '([^']+)'", line)
        if m:
            t, al = float(m.group(1)), m.group(2)
            plays.append((t, al))
            alias_count[al] = alias_count.get(al, 0) + 1
    fires = [t for t, al in plays if re.match(r'wpn_.*_fire_plr$', al)]
    lags = []
    for t in fires:
        i0 = int(t * 200)
        base = max(env[max(0, i0 - 30):max(1, i0 - 2)] or [0])
        best = None
        for j in range(max(1, i0 - 30), min(len(env), i0 + 30)):
            if env[j] > max(2000, 3 * base) and env[j] > env[j - 1]:
                best = j
                break
        if best is not None:
            lags.append((best - i0) * 5)
    # capas por nombre de alias (lo que se pidio sonar)
    capas = {'musica': r'^mus_', 'armas': r'^(wpn_|fly_.*(slide|clip|shell|mag)|prj_)', 'zombis': r'^(zmb_vocals|fly_step_zombie|fly_gear_zombie)',
             'barricadas': r'^zmb_(break|rebuild|board|repair)', 'compras/caja/puertas': r'^(zmb_cha_ching|zmb_no_cha_ching|zmb_door|zmb_box|zmb_weap_wall|zmb_perks|zmb_purchase|mus_perks|zmb_lid)',
             'potenciadores/anunciador': r'^(zmb_powerup|zmb_spawn_powerup|zmb_full_ammo|zmb_double|zmb_vox_ann|zmb_insta|zmb_nuke)',
             'ambiente': r'^(amb_|evt_)', 'jugador': r'^(fly_step_.*plr|fly_.*sprint|fly_cloth|chr_)'}
    capa_n = {k: 0 for k in capas}
    for al, c in alias_count.items():
        for k, rx in capas.items():
            if re.match(rx, al):
                capa_n[k] += c
                break
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', wav, '-lavfi', 'showspectrumpic=s=1600x600:legend=1:scale=log:fscale=log',
                    os.path.join(out, 'espectrograma.png')], check=False)
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', wav, '-filter_complex', 'showwavespic=s=1600x300:split_channels=1',
                    '-frames:v', '1', os.path.join(out, 'onda.png')], check=False)
    with open(os.path.join(out, 'resumen.txt'), 'w') as f:
        p = lambda *x: (print(*x), print(*x, file=f))
        p('grabacion %s: %.1f s, %d Hz, %d canales' % (os.path.basename(wav), dur, rate, ch))
        p('pico total %.2f dBFS, muestras recortadas (+-32767) %d de %d (%.4f%%)' % (20 * math.log10(max(peak, 1e-9)), clip, len(a), 100.0 * clip / max(1, len(a))))
        dbs = sorted(r[1] for r in rows)
        if dbs:
            p('RMS por segundo: min %.1f, mediana %.1f, max %.1f dBFS; segundos por debajo de -60 dBFS (silencio): %d' % (dbs[0], dbs[len(dbs) // 2], dbs[-1], sil))
            p('balance L/R medio %.2f dB' % (sum(r[3] for r in rows) / len(rows)))
        p('reproducciones en el registro: %d (%d alias distintos)' % (len(plays), len(alias_count)))
        p('por capa: ' + ', '.join('%s %d' % kv for kv in capa_n.items()))
        if lags:
            lags.sort()
            p('sincronia de disparos del jugador: %d de %d con ataque encontrado; retardo mediana %d ms (min %d, max %d)' % (len(lags), len(fires), lags[len(lags) // 2], lags[0], lags[-1]))
        else:
            p('sincronia: %d disparos del jugador en el registro, ningun ataque localizado' % len(fires))
        top = sorted(alias_count.items(), key=lambda kv: -kv[1])
        p('alias (todos):')
        for al, c in top:
            p('  %5d %s' % (c, al))


if __name__ == '__main__':
    main()
