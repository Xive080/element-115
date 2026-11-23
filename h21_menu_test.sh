#!/bin/bash
# h21_menu_test.sh (OLA U2): prueba del menu principal (h21_menu.cpp) en el Mac SIN ventana, al tamano y densidad de una pantalla del Fold,
# con toques simulados (H21_PILOT: tap/tapid/drag/back/shot) y capturas PNG. No toca el config.cfg real: base aislada en <salida>/base con
# enlaces a main/, zone/ y h6a/ de la base y players/ propio (copia del config.cfg). Mapas: BO1_MAPS o <base>/../gamedata_maps.
#
#   h21_menu_test.sh <salida> <perfil> ["piloto"]
#   perfil: plegado (2092x1080, 420 dpi), abierto (2208x1840, 360 dpi), vertical (1840x2208, 360 dpi), mac (1280x720) o WxH[@dpi]
#   OSUF=u2 (binario out_vk_<OSUF>_h5/p3), SAFE=l,t,r,b (H10_SAFE), LAUNCH=1 deja que el toque final lance el mapa (relanza el proceso:
#   carga con el video de carga y juega H5_PLAY_SECONDS, 25 por defecto), TRACE=1, COOP=0 (como el APK: co-op como hueco).
# Salida: <salida>/menu.log y <salida>/shot_h21_<etiqueta>.png por cada "shot <etiqueta>" del piloto.
cd "$(dirname "$0")"; HERE=$PWD
OUT=$1; PERF=${2:-mac}; PILOT=$3
[ -n "$OUT" ] || { echo "uso: h21_menu_test.sh <salida> <perfil> [piloto]"; exit 2; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
case "$PERF" in
  plegado) W=2092; H=1080; DPI=420;;
  abierto) W=2208; H=1840; DPI=360;;
  vertical) W=1840; H=2208; DPI=360;;
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
rm -f "$B/players/bo1_menu.txt" "$B/players/bo1_map.txt"
[ -n "$PILOT" ] || PILOT="1 shot setup;2 tapid 5;3 shot mission;4 quit"
D=$OUT/v9; mkdir -p "$D"; NOL=1; [ -n "$LAUNCH" ] && NOL=0
echo "menu $PERF ${W}x${H} dpi ${DPI} -> $OUT"
env P3_TRACE=off V9_CACHE_DIR=$D V9_SHADERCACHE=$D/v9_shaders.spvc V9_MAXTEX=512 V9_MAX_MB=1200 V9_RESMAX_MB=64 V9_VALIDATE=0 \
    P3_GAMEDATA="$B" ${MAPS:+BO1_MAPS=$MAPS} P4_W=$W P4_H=$H ${DPI:+H10_DPI=$DPI} ${SAFE:+H10_SAFE=$SAFE} P4_OUT="$OUT/shot.png" \
    H21_MENU=1 H21_PILOT="$PILOT" H21_NOLAUNCH=$NOL ${TRACE:+H21_TRACE=1} ${COOP:+H21_COOP=$COOP} H5_GAME=1 H5_PLAY=1 H5_VIEW=1 H5_PLAY_SHOT=0 \
    H5_PLAY_SECONDS=${H5_PLAY_SECONDS:-25} H6A_DEVICE=null \
    ../vk/run_vk.sh "$BIN" > /dev/null 2> "$OUT/menu.log"
rc=$?
grep -aE "\[H21\] (OLA|menu listo|descripciones|EMPIEZA|atras|aviso|pantalla)|\[SIGNAL|FATAL" "$OUT/menu.log" | head -40
echo "fin (codigo $rc)"; exit $rc
