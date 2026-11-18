#!/bin/bash
# selftest_mac.sh (OLA 15A): autoprueba reproducible en el Mac con la configuracion POR DEFECTO (Vulkan/MoltenVK, juego H5G, FX, sonido,
# HUD H13, cristal H12GLASS, menus h10, luz h7e). Partida pilotada de SEGUNDOS de juego (180 = 3 min, llega a la ronda 3) con modo dios,
# topes de memoria del Fold (V9_MAX_MB 640, V9_MAXTEX 512, V9_RESMAX_MB 64) y el ritmo de subida del Fold (V9_UPLOAD_MB_S 200) ACTIVO.
# Pasa si: el proceso termina solo con codigo 0, sin [SIGNAL], sin STUB/ASSERT/abort/TOPE/RITMO SUPERADO/FATAL, sin errores nuevos
# (lineas de error que no esten en selftest_conocidos.txt) y con al menos 3 capturas. Deja en SALIDA: juego.log (filtrado), errores.txt,
# mem.csv (H15_MEMPROFILE por fases), resumen.txt y las capturas (shot_*.png).
#
#   selftest_mac.sh [SALIDA] [SEGUNDOS]        (por defecto ./selftest_out y 180)
#   OSUF=i15o (binario out_vk_<OSUF>_h5/p3), BUILD=1 lo compila antes con build.sh (OPT=-O2 como el APK, JOBS=4), AUDIO=1 usa la salida de audio real
#   (por defecto el dispositivo null: el sistema de sonido entero mezcla, pero sin sacar audio por los altavoces), TEXPKG=fichero para
#   usar el paquete ASTC como en el Fold, y VAR=valor extra al final van al juego.
#   MAP=zona (OLA M1) elige el mapa con H5_MAP (h18_maps.cpp: datos en <raiz>/<zona>, raiz = BO1_MAPS o ../gamedata_maps junto a P3_GAMEDATA);
#   sin MAP no se pasa H5_MAP y el juego carga su mapa por defecto (Five), igual que antes.
cd "$(dirname "$0")"; HERE=$PWD
D=${1:-$HERE/selftest_out}; SEG=${2:-180}; [ $# -ge 2 ] && shift 2 || shift $#; mkdir -p "$D"; D=$(cd "$D" && pwd)
OSUF=${OSUF:-i15o}; BIN=$HERE/out_vk_${OSUF}_h5/p3
export P3_GAMEDATA=${P3_GAMEDATA:-$(cd ../../gamedata && pwd)}
if [ -n "$BUILD" ]; then OSUF=$OSUF OPT=${OPT:--O2} BACKEND=vk H5G=1 H5FX=1 JOBS=${JOBS:-4} nice -n 10 ./build.sh > "$D/build.log" 2>&1 || { echo "FALLA build (ver $D/build.log)"; exit 1; }; fi
[ -x "$BIN" ] || { echo "no existe $BIN (BUILD=1 para compilarlo)"; exit 1; }
rm -f "$D"/shot_*.png "$D"/m_*.png; mkdir -p "$D/v9cache"   # cache de pipelines PROPIA (la de harness2/v9cache la comparten otras pruebas y puede crecer mucho)
DEV=null; [ -n "$AUDIO" ] && DEV=
# tope del Fold (640) solo con el paquete ASTC: sin el, el Mac sube DXT (~520 MB) y el tope del Fold aborta a proposito (como en el Fold sin tex.pkg)
MAXMB=${V9_MAX_MB:-640}; [ -z "$TEXPKG" ] && MAXMB=${V9_MAX_MB:-1200}
ALARM=$((SEG * 8 + 300))   # tope de reloj: el Mac cargado puede ir lento; el juego cuenta tiempo de juego, no de reloj
t0=$SECONDS
env P3_TRACE=off V9_CACHE_DIR=${V9_CACHE_DIR:-$D/v9cache} V9_SHADERCACHE=${V9_SHADERCACHE:-$D/v9_shaders.spvc} V9_MAXTEX=512 V9_MAX_MB=$MAXMB V9_RESMAX_MB=64 V9_VALIDATE=0 V9_UPLOAD_MB_S=200 ${TEXPKG:+V9_TEXPKG=$TEXPKG} \
    ${MAP:+H5_MAP=$MAP} H5_GAME=1 H5_PLAY=1 H5_PLAY_SCRIPT=2 H5_VIEW=1 H5_SET="bo1_testclient_god 1" ${DEV:+H6A_DEVICE=$DEV} \
    P4_OUT="$D/m.png" H5_PLAY_SHOT=$((SEG / 3)) H5_PLAY_SECONDS=$SEG H15_MEMPROFILE="$D/mem.csv" "$@" \
    perl -e "alarm $ALARM; exec @ARGV" ../vk/run_vk.sh "$BIN" > /dev/null 2> "$D/juego_completo.log"
rc=$?; wall=$((SECONDS - t0))
# log filtrado (el completo puede ser grande: se queda solo si falla)
grep -aE "^\[(H5F|H15MEM\] FASE|H6A\] (wma|sin|FALTA)|v9budget|SIGNAL|P4\] backend|H13|H12)|STUB|ASSERT|abort|FATAL|memguard" "$D/juego_completo.log" | head -c 20000000 > "$D/juego.log"
PAT='\[SIGNAL|STUB|ASSERT|abort\(\)|FATAL|TOPE .*SUPERADO|RITMO DE SUBIDA SUPERADO|RECHAZADO|ERROR|[Ee]rror:|CORRUPTO|FALTA '
grep -aE "$PAT" "$D/juego_completo.log" | sed -E 's/[0-9]+/N/g' | sort | uniq -c | sort -rn > "$D/errores_todos.txt"
KN=$HERE/selftest_conocidos.txt; : > "$D/errores.txt"
while IFS= read -r l; do t=${l#*[0-9] }; m=0; [ -f "$KN" ] && while IFS= read -r k; do [ -z "$k" ] || [ "${k#\#}" != "$k" ] && continue; case "$t" in *"$k"*) m=1; break;; esac; done < "$KN"; [ $m = 0 ] && echo "$l" >> "$D/errores.txt"; done < "$D/errores_todos.txt"
# capturas: todas las que dejo el juego, renombradas por orden
n=0; for f in $(ls "$D"/m*.png 2>/dev/null); do n=$((n + 1)); mv "$f" "$D/shot_$(printf %02d $n)_$(basename "$f" .png | sed 's/^m//').png"; done
sig=$(grep -ac "\[SIGNAL" "$D/juego_completo.log"); fin=$(grep -a "^\[H5F\] FIN" "$D/juego_completo.log" | tail -1)
ronda=$(grep -a "ronda [0-9]* empieza" "$D/juego_completo.log" | tail -1 | sed -n 's/.*ronda \([0-9]*\).*/\1/p')
pico=$(awk -F, 'NR>1 && $4>m {m=$4; f=$3} NR>1 && $5>a {a=$5; g=$3} END {printf "rss %d MB (%s), footprint/anon %d MB (%s)", m/1024, f, a/1024, g}' "$D/mem.csv" 2>/dev/null)
nerr=$(wc -l < "$D/errores.txt" | tr -d ' ')
ok=1; [ $rc = 0 ] || ok=0; [ "$sig" = 0 ] || ok=0; [ "$nerr" = 0 ] || ok=0; [ $n -ge 3 ] || ok=0; [ -n "$fin" ] || ok=0
{
  echo "selftest_mac $(date '+%Y-%m-%d %H:%M') binario $(basename "$(dirname "$BIN")")/p3, mapa ${MAP:-por defecto}, $SEG s de juego, $wall s de reloj"
  echo "codigo de salida $rc | SIGNAL $sig | errores nuevos $nerr | capturas $n | ronda maxima ${ronda:-?} | pico $pico"
  echo "${fin:-sin linea [H5F] FIN (no termino la partida)}"
  echo "fases de memoria (MB):"; awk -F, '$2=="FASE" || $2=="SALIDA" {printf "  %-16s t=%6.1f s  rss %5d  footprint/anon %5d  heap %5d\n", $3, $1/1000, $4/1024, $5/1024, $10/1024}' "$D/mem.csv" 2>/dev/null
  [ "$nerr" = 0 ] || { echo "errores nuevos:"; head -20 "$D/errores.txt"; }
  [ $ok = 1 ] && echo "RESULTADO: PASA" || echo "RESULTADO: FALLA"
} | tee "$D/resumen.txt"
[ $ok = 1 ] && [ -z "$KEEPLOG" ] && rm -f "$D/juego_completo.log"   # KEEPLOG=1 lo conserva
[ $ok = 1 ]
