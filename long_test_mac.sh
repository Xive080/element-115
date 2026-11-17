#!/bin/bash
# OLA 7d: prueba larga del juego en el Mac (mismo motor y mismas texturas que el Fold), pilotada (H5_PLAY_SCRIPT), con una linea
# de muestra cada PASO s en un log pequeno filtrado: frames y fps del intervalo, RSS del proceso, ronda/zombis, superficies FX,
# marcas, modelos FX, prepase floatz y tiempos por frame ([H5D-PERF]). Al final, tabla resumen (min/media/max).
# Uso: long_test_mac.sh SALIDA MINUTOS_DE_RELOJ [VAR=valor ...]   (las VAR extra van al juego, p.ej. H7D_FLOATZ=0)
# El log completo del juego se filtra al vuelo (solo etiquetas utiles) y se corta a 30 MB: disco justo.
cd "$(dirname "$0")"; D=${1:?salida}; MIN=${2:-15}; shift 2; mkdir -p "$D"; PASO=${PASO:-10}
export P3_GAMEDATA=$(cd ../../gamedata && pwd)
W=$((MIN * 60))
env V9_MAXTEX=${V9_MAXTEX:-512} V9_MAX_MB=${V9_MAX_MB:-1200} V9_RESMAX_MB=64 V9_VALIDATE=0 H5_GAME=1 H5_PLAY=1 H5_PLAY_SCRIPT=${SCRIPT:-2} H5_VIEW=1 \
    P4_OUT="$D/m.png" H5_PLAY_SHOT=0 H5_PLAY_SECONDS=$((W * 4)) H5FX_STATS=1 "$@" \
    perl -e "alarm $((W + 120)); exec @ARGV" ../vk/run_vk.sh ./out_vk_${OSUF:-7d}_h5/p3 2>&1 >/dev/null \
  | grep -aE --line-buffered "^\[(v9\] f=|H5F\]|H5FX\] 600|H7D\]|H5D-PERF|H5D\] t=.*zombis|STUB|SIGNAL|ASSERT)|memguard|TOPE|  #" \
  | head -c 30000000 > "$D/juego.log" &
sleep 3; pid=$(pgrep -n -f "out_vk_${OSUF:-7d}_h5/p3"); echo "pid juego: ${pid:-?}"
echo "t_s,frames,fps,rss_mb,ronda,vivos,fx_sup,fx_sup_max,marcas,decal_sup,mod_fx_sup,floatz_ms,h4frame_ms,skin_ms" > "$D/muestras.csv"
t0=$SECONDS; fprev=0
while [ $((SECONDS - t0)) -lt $W ]; do
  sleep $PASO
  kill -0 $pid 2>/dev/null || { echo "el juego termino a los $((SECONDS - t0)) s"; break; }
  L="$D/juego.log"
  f=$(grep -a "^\[v9\] f=" "$L" | tail -1 | sed -n 's/.*f=\([0-9]*\).*/\1/p'); f=${f:-0}
  fps=$(awk -v a=$f -v b=$fprev -v p=$PASO 'BEGIN{printf "%.1f", (a-b)/p}'); fprev=$f
  rss=$(ps -o rss= -p $pid 2>/dev/null | awk '{print int($1/1024)}')
  r=$(grep -a "^\[H5F\] t=" "$L" | tail -1); ronda=$(echo "$r" | sed -n 's/.*ronda \([0-9]*\).*/\1/p'); vivos=$(echo "$r" | sed -n 's/.*vivos \([0-9]*\).*/\1/p')
  x=$(grep -a "^\[H5FX\] 600" "$L" | tail -1); fs=$(echo "$x" | sed -n 's/.*superficies \([0-9]*\)\/frame (max \([0-9]*\)).*/\1,\2/p')
  m=$(grep -a "^\[H7D\] 600" "$L" | tail -1); mk=$(echo "$m" | sed -n 's/.*mundo \([0-9]*\),.*con luz \([0-9]*\) sup.*modelos FX \([0-9]*\) sup.*/\1,\2,\3/p')
  fz=$(grep -a "^\[H7D\] floatz: 600" "$L" | tail -1 | sed -n 's/.*prepase \([0-9.]*\) ms.*/\1/p')
  pf=$(grep -a "^\[H5D-PERF\]" "$L" | tail -1); h4=$(echo "$pf" | sed -n 's/.*H4_Frame \([0-9.]*\) ms.*/\1/p'); sk=$(echo "$pf" | sed -n 's/.*skinning CPU \([0-9.]*\) ms.*/\1/p')
  echo "$((SECONDS - t0)),$f,$fps,${rss:-0},${ronda:-},${vivos:-},${fs:-,},${mk:-,,},${fz:-},${h4:-},${sk:-}" >> "$D/muestras.csv"
done
pkill -f "out_vk_${OSUF:-7d}_h5/p3" 2>/dev/null; sleep 1
echo "== fallos"; grep -aE "STUB|SIGNAL|ASSERT|TOPE" "$D/juego.log" | head -5
echo "== resumen (min/media/max por columna)"
awk -F, 'NR==1{for(i=1;i<=NF;i++)h[i]=$i; next} {n++; for(i=3;i<=NF;i++) if($i!=""){v=$i+0; s[i]+=v; c[i]++; if(!(i in mn)||v<mn[i])mn[i]=v; if(!(i in mx)||v>mx[i])mx[i]=v}}
  END{printf "muestras %d\n", n; for(i=3;i<=NF;i++) if(c[i]) printf "%-12s min %8.2f  media %8.2f  max %8.2f\n", h[i], mn[i], s[i]/c[i], mx[i]}' "$D/muestras.csv"
