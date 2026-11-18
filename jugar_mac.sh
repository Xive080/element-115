#!/bin/bash
# jugar_mac.sh (OLA P1): JUGAR A MANO en el Mac. Ventana Cocoa + Vulkan/MoltenVK, teclado y raton (h10_macin.mm, H10_MANUAL=1),
# sonido real y la configuracion por defecto del selftest (topes de memoria incluidos), SIN modo dios y SIN piloto. Juega hasta que
# se cierre la ventana, Cmd+Q o "Quit" en el menu de pausa (Esc).
#
#   jugar_mac.sh [zona]        sin zona (o "menu"): menu principal en la ventana (OLA U1, h21_menu.cpp); "terminal": el menu por terminal
#   OSUF=p1 (binario out_vk_<OSUF>_h5/p3), BUILD=1 lo compila antes (WIN=1, -O2), SENS=0.11 sensibilidad del raton (grados por punto),
#   INVERT=1 vista vertical invertida, AUDIO=0 sin sonido, P4_W/P4_H tamano de la ventana (1280x720), LOG=fichero (por defecto
#   jugar_out/juego.log), VAR=valor extra al final van al juego. BO1_IMAGENES=carpeta con bo1maps_m*.sparsebundle si no se encuentra sola.
cd "$(dirname "$0")"; HERE=$PWD
ZONAS=(zombie_pentagon zombie_theater zombie_cod5_prototype zombie_cod5_asylum zombie_cod5_factory zombie_cod5_sumpf zombie_temple zombie_coast zombie_cosmodrome zombie_moon)
NOMBRES=("Five" "Kino der Toten" "Nacht der Untoten" "Verrückt" "Der Riese" "Shi No Numa" "Shangri-La" "Call of the Dead" "Ascension" "Moon")
ZONA=$1; [ $# -ge 1 ] && shift
MENU=; if [ -z "$ZONA" ] || [ "$ZONA" = menu ]; then MENU=1; ZONA=zombie_pentagon; fi   # OLA U1: sin zona = menu principal en la ventana (GAME SETUP / MISSION SELECT)
if [ "$ZONA" = terminal ]; then   # menu por terminal de antes
  echo "Mapas:"; for i in "${!ZONAS[@]}"; do printf "  %2d) %-18s %s\n" $((i + 1)) "${NOMBRES[$i]}" "${ZONAS[$i]}"; done
  read -r -p "Elige mapa [1-10, Intro = 1]: " n; n=${n:-1}
  case "$n" in ''|*[!0-9]*) ZONA=$n;; *) [ "$n" -ge 1 ] && [ "$n" -le ${#ZONAS[@]} ] && ZONA=${ZONAS[$((n - 1))]};; esac
fi
ok=0; for z in "${ZONAS[@]}"; do [ "$z" = "$ZONA" ] && ok=1; done
[ $ok = 1 ] || { echo "mapa desconocido: '$ZONA' (validos: ${ZONAS[*]})"; exit 1; }

OSUF=${OSUF:-p1}; BIN=$HERE/out_vk_${OSUF}_h5/p3
export P3_GAMEDATA=${P3_GAMEDATA:-$(cd ../../gamedata && pwd)}
MAPS=${BO1_MAPS:-$(cd "$P3_GAMEDATA/../gamedata_maps" 2>/dev/null && pwd)}
if [ -n "$BUILD" ]; then
  echo "compilando $BIN ..."
  OSUF=$OSUF OPT=${OPT:--O2} BACKEND=vk WIN=1 H5G=1 H5FX=1 JOBS=${JOBS:-4} nice -n 10 ./build.sh > "$HERE/jugar_build_$OSUF.log" 2>&1 || { echo "FALLA build (ver jugar_build_$OSUF.log)"; exit 1; }
fi
[ -x "$BIN" ] || { echo "no existe $BIN (BUILD=1 para compilarlo)"; exit 1; }

# datos del mapa: Five va en la base; el resto en <raiz>/<zona>. Ascension y Moon son enlaces a imagenes sparsebundle (bo1maps_m9/m10)
# que se montan aqui si hace falta (hdiutil attach -nobrowse). La imagen se busca en BO1_IMAGENES y en <volumen>/bo1vk de cada disco montado.
montar() {   # montar <zona>: monta la imagen del mapa si su carpeta es un enlace roto; 1 si no se pudo
  local DIR=$MAPS/$1 DEST VOL IMG
  [ ! -d "$DIR/zone" ] && [ -L "$DIR" ] || return 0
  DEST=$(readlink "$DIR"); VOL=$(basename "$(dirname "$DEST")")
  IMG=$( { [ -n "$BO1_IMAGENES" ] && echo "$BO1_IMAGENES"; mount | sed -n 's/^.* on \(.*\) (.*$/\1/p'; } | while IFS= read -r c; do
    for f in "$c/$VOL.sparsebundle" "$c/bo1vk/$VOL.sparsebundle"; do [ -d "$f" ] && { echo "$f"; exit 0; }; done
  done )
  [ -n "$IMG" ] || { echo "no encuentro $VOL.sparsebundle (conecta el disco o pon BO1_IMAGENES=carpeta)"; return 1; }
  echo "montando $IMG ..."; hdiutil attach -nobrowse "$IMG" > /dev/null || { echo "FALLA hdiutil attach $IMG"; return 1; }
}
if [ -n "$MENU" ]; then
  # menu: se montan las imagenes que se puedan (los mapas sin datos salen en gris en MISSION SELECT)
  [ -n "$MAPS" ] && for z in "${ZONAS[@]}"; do [ "$z" = zombie_pentagon ] || montar "$z" > /dev/null 2>&1; done
elif [ "$ZONA" != zombie_pentagon ]; then
  montar "$ZONA" || exit 1
  [ -d "$MAPS/$ZONA/zone" ] || { echo "faltan los datos de $ZONA en $MAPS/$ZONA (maps/bo1maps.py fetch $ZONA)"; exit 1; }
fi

D=$HERE/jugar_out; mkdir -p "$D/v9cache"; LOGF=${LOG:-$D/juego.log}
DEV=; [ "${AUDIO:-1}" = 0 ] && DEV=null
# topes del selftest: V9_MAXTEX 512, V9_RESMAX_MB 64 y V9_MAX_MB 1200 (sin tex.pkg el Mac sube DXT); el memguard del motor vigila los 3 GB
if [ -n "$MENU" ]; then echo "menu principal (log en $LOGF): elige modo, graficos y mapa; cerrar la ventana o Cmd+Q = salir."
else echo "jugando $ZONA (log en $LOGF). Esc = menu de pausa, cerrar la ventana o Cmd+Q = salir."; fi
env P3_TRACE=off V9_CACHE_DIR=${V9_CACHE_DIR:-$D/v9cache} V9_SHADERCACHE=${V9_SHADERCACHE:-$D/v9_shaders.spvc} V9_MAXTEX=512 V9_MAX_MB=${V9_MAX_MB:-1200} \
    V9_RESMAX_MB=64 V9_VALIDATE=0 P4_WINDOW=1 H10_MANUAL=1 ${SENS:+H10_SENS=$SENS} ${INVERT:+H10_INVERT=$INVERT} \
    ${MENU:+H21_MENU=1} H5_MAP=$ZONA H5_GAME=1 H5_PLAY=1 H5_VIEW=1 H5_PLAY_SHOT=0 H5_PLAY_SECONDS=${H5_PLAY_SECONDS:-1000000} ${DEV:+H6A_DEVICE=$DEV} "$@" \
    ../vk/run_vk.sh "$BIN" > /dev/null 2> "$LOGF"
rc=$?
grep -aE "\[SIGNAL|FATAL|TOPE .*SUPERADO|memguard.*(aborta|ABORTA)" "$LOGF" | head -5
echo "fin (codigo $rc)"; exit $rc
