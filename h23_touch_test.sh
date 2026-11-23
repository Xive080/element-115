#!/bin/bash
# h23_touch_test.sh (OLA U2): controles tactiles en partida (h23_touch.cpp) en el Mac SIN ventana, al tamano de una pantalla del Fold,
# con toques simulados (H23_PILOT) y capturas PNG. Base aislada en <salida>/base (como h21_menu_test.sh); bo1_touch.cfg en <salida>.
#
#   h23_touch_test.sh <salida> <perfil> ["piloto"]
#   perfil: plegado (2092x1080, 420 dpi), abierto (2208x1840, 360 dpi), mac (1280x720) o WxH[@dpi]
#   OSUF=u2, MAP=<zona> (por defecto el de la base), SAFE=l,t,r,b, SECONDS=26 (tiempo de partida), CFG=<bo1_touch.cfg> (por defecto <salida>/bo1_touch.cfg)
# Salida: <salida>/touch.log y <salida>/shot_h23_<etiqueta>.png.
OUT=$1; PERF=${2:-mac}; PILOT=$3
[ -n "$OUT" ] || { echo "uso: h23_touch_test.sh <salida> <perfil> [piloto]"; exit 2; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
cd "$(dirname "$0")"; HERE=$PWD
case "$PERF" in
  plegado) W=2092; H=1080; DPI=420;;
  abierto) W=2208; H=1840; DPI=360;;
  mac) W=1280; H=720; DPI=0;;
  *x*) W=${PERF%%x*}; R=${PERF#*x}; H=${R%%@*}; DPI=0; [ "$R" != "$H" ] && DPI=${R#*@};;
  *) echo "perfil desconocido: $PERF"; exit 2;;
esac
OSUF=${OSUF:-u2}; BIN=$HERE/out_vk_${OSUF}_h5/p3; [ -x "$BIN" ] || { echo "no existe $BIN"; exit 1; }
SRC=${P3_GAMEDATA:-$(cd ../../gamedata && pwd)}
MAPS=${BO1_MAPS:-$(cd "$SRC/../gamedata_maps" 2>/dev/null && pwd)}
B=$OUT/base; mkdir -p "$B/players"
for d in main zone h6a; do [ -e "$B/$d" ] || ln -s "$SRC/$d" "$B/$d"; done
[ -f "$B/players/config.cfg" ] || cp "$SRC/players/config.cfg" "$B/players/" 2>/dev/null
[ -n "$PILOT" ] || PILOT="2 shot inicio;2.2 log;3 quit"
D=$OUT/v9; mkdir -p "$D"
echo "tactil $PERF ${W}x${H} dpi ${DPI} -> $OUT"
env P3_TRACE=off V9_CACHE_DIR=$D V9_SHADERCACHE=$D/v9_shaders.spvc V9_MAXTEX=512 V9_MAX_MB=1200 V9_RESMAX_MB=64 V9_VALIDATE=0 \
    P3_GAMEDATA="$B" ${MAPS:+BO1_MAPS=$MAPS} P4_W=$W P4_H=$H ${DPI:+H10_DPI=$DPI} ${SAFE:+H10_SAFE=$SAFE} P4_OUT="$OUT/shot.png" \
    H23_PILOT="$PILOT" H23_CFG="${CFG:-$OUT/bo1_touch.cfg}" ${MAP:+H5_MAP=$MAP} H5_GAME=1 H5_PLAY=1 H5_VIEW=1 H5_PLAY_SHOT=0 H5_FIXEDDT=1 \
    H5_PLAY_SECONDS=${SECONDS_:-${SECS:-26}} H6A_DEVICE=null \
    ../vk/run_vk.sh "$BIN" > /dev/null 2> "$OUT/touch.log"
rc=$?
grep -aE "\[H23\]|\[H10\] (pestana|abre)|\[SIGNAL|FATAL" "$OUT/touch.log" | grep -v "piloto t=" | head -60
echo "fin (codigo $rc)"; exit $rc
