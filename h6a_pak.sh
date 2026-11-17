#!/bin/bash
# OLA 14: wma_adpcm.pak (los sonidos xWMA del juego pasados a MS-ADPCM para h5_audio.cpp), de principio a fin.
#
#   h6a_pak.sh build [mapa ...]   vuelca los xWMA cargados de cada mapa (por defecto zombie_pentagon) con una partida corta en el
#                                 backend trace, los funde en UN paquete ($P3_GAMEDATA/h6a/wma_adpcm.pak o PAK=<ruta>) y lo verifica.
#                                 Con varios mapas (p. ej. "build zombie_pentagon zombie_theater zombie_cosmodrome") los nombres
#                                 repetidos se guardan una vez. Cada mapa necesita su zone/<mapa>.ff y en_<mapa>.ff en P3_GAMEDATA.
#   h6a_pak.sh add mapa [...]     OLA 15C: anade mapas al paquete actual sin perder los que ya tiene: rehace 'build' con los mapas
#                                 de $P3_GAMEDATA/h6a/maps.txt (lo escribe 'build'; si no existe, zombie_pentagon) mas los nuevos.
#   h6a_pak.sh maps               OLA 15C: mapas con zona en P3_GAMEDATA (candidatos para 'add') y los que ya estan en el paquete.
#   h6a_pak.sh verify [pak]       cabecera, orden de hashes, rangos y decodificacion de cada sonido con ffmpeg (sale con 1 si hay corruptos).
#   h6a_pak.sh fold [pak]         copia el paquete al Fold (files/game/h6a/ de app.bo1native) y compara tamano y md5. NO fuerza
#                                 conexiones: si adb no ve el dispositivo (FOLD=<serie>, por defecto el de nativeapp/fold.sh) se para.
#
# Variables: P3 (binario con sonido, por defecto out_vk_h5/p3 junto a este script), P3_GAMEDATA (por defecto ../../gamedata), PAK.
# ffmpeg solo se usa aqui, fuera de linea: el binario no enlaza ningun decodificador WMA.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
export P3_GAMEDATA=${P3_GAMEDATA:-$(cd "$HERE/../../gamedata" && pwd)}
PAK=${PAK:-$P3_GAMEDATA/h6a/wma_adpcm.pak}
cmd=${1:-}; shift || true
command -v ffmpeg >/dev/null || { echo "h6a_pak: falta ffmpeg (brew install ffmpeg)"; exit 1; }
case "$cmd" in
build)
  P3=${P3:-$HERE/out_vk_h5/p3}
  [ -x "$P3" ] || { echo "h6a_pak: no existe $P3 (compila con H5G=1, el sonido va por defecto, o pasa P3=<binario>)"; exit 1; }
  grep -q "H6A_DUMPWMA" "$P3" || { echo "h6a_pak: $P3 no lleva el sonido (compilado con H6SND=0)"; exit 1; }
  [ $# -gt 0 ] || set -- zombie_pentagon
  T=$(mktemp -d "${TMPDIR:-/tmp}/h6a_pak.XXXXXX"); trap 'rm -rf "$T"' EXIT
  dirs=""
  for m in "$@"; do
    [ -f "$P3_GAMEDATA/zone/Common/$m.ff" ] || [ -f "$P3_GAMEDATA/zone/$m.ff" ] || { echo "h6a_pak: falta la zona de $m en $P3_GAMEDATA/zone: se salta"; continue; }
    mkdir -p "$T/$m"
    echo "h6a_pak: volcando los xWMA de $m ..."
    ( cd "$HERE" && H5_MAP=$m H6A_SURVEY=1 H6A_DUMPWMA="$T/$m" H6A_DEVICE=null H5_GAME=1 H5_PLAY=1 H5_PLAY_SCRIPT=0 H5_PLAY_SECONDS=2 \
        P3_BACKEND=trace "$P3" >/dev/null 2>"$T/$m.log" ) || { echo "h6a_pak: la partida de $m fallo (ver el final de su registro):"; tail -5 "$T/$m.log"; exit 1; }
    [ -s "$T/$m/index.tsv" ] || { echo "h6a_pak: $m no volco ningun xWMA"; tail -5 "$T/$m.log"; exit 1; }
    echo "h6a_pak: $m: $(wc -l < "$T/$m/index.tsv" | tr -d ' ') sonidos xWMA"
    dirs="$dirs $T/$m"
  done
  [ -n "$dirs" ] || { echo "h6a_pak: ningun mapa volcado"; exit 1; }
  python3 -I "$HERE/h6a_wma_pak.py" $dirs -o "$PAK"
  python3 -I "$HERE/h6a_wma_pak.py" --verify "$PAK"
  for d in $dirs; do basename "$d"; done > "$(dirname "$PAK")/maps.txt"   # OLA 15C: mapas fundidos (lo lee 'add')
  echo "h6a_pak: mapas en el paquete: $(tr '\n' ' ' < "$(dirname "$PAK")/maps.txt")" ;;
add)
  [ $# -gt 0 ] || { echo "uso: h6a_pak.sh add mapa [...]"; exit 1; }
  M="$(dirname "$PAK")/maps.txt"; old=$( [ -f "$M" ] && cat "$M" || echo zombie_pentagon )
  all=$(printf '%s\n' $old "$@" | awk 'NF && !s[$0]++' | tr '\n' ' ')
  echo "h6a_pak: rehago el paquete con: $all"
  exec "$0" build $all ;;
maps)
  echo "zonas de mapa en $P3_GAMEDATA/zone (sin parches ni comunes):"
  ls "$P3_GAMEDATA"/zone/*/ "$P3_GAMEDATA"/zone/ 2>/dev/null | grep -E '\.ff$' | grep -vE '^(en_|common|code_|ui|patch|frontend)|_patch\.ff$' | sed 's/\.ff$//' | sort -u
  M="$(dirname "$PAK")/maps.txt"; echo "en el paquete: $( [ -f "$M" ] && tr '\n' ' ' < "$M" || echo 'zombie_pentagon (sin maps.txt)')" ;;
verify)
  P=${1:-$PAK}; [ -f "$P" ] || { echo "h6a_pak: no existe $P (genera con: h6a_pak.sh build)"; exit 1; }
  python3 -I "$HERE/h6a_wma_pak.py" --verify "$P" ;;
fold)
  P=${1:-$PAK}; [ -f "$P" ] || { echo "h6a_pak: no existe $P (genera con: h6a_pak.sh build)"; exit 1; }
  python3 -I "$HERE/h6a_wma_pak.py" --verify "$P" || { echo "h6a_pak: el paquete tiene sonidos corruptos: no se copia"; exit 1; }
  FOLD=${FOLD:-$(sed -n 's/.*FOLD:-\([^}]*\)}.*/\1/p' "$HERE/../../nativeapp/fold.sh")}
  st=$(adb -s "$FOLD" get-state 2>/dev/null || true)
  [ "$st" = device ] || { echo "h6a_pak: adb no ve el Fold $FOLD (estado '${st:-ninguno}'); conectalo y repite (no se fuerza la conexion)"; exit 2; }
  D=/sdcard/Android/data/app.bo1native/files/game/h6a
  adb -s "$FOLD" shell mkdir -p $D
  adb -s "$FOLD" push "$P" $D/wma_adpcm.pak
  rs=$(adb -s "$FOLD" shell stat -c %s $D/wma_adpcm.pak | tr -d '\r'); ls=$(stat -f %z "$P" 2>/dev/null || stat -c %s "$P")
  rm5=$(adb -s "$FOLD" shell md5sum $D/wma_adpcm.pak | cut -d' ' -f1); lm5=$( (md5 -q "$P" 2>/dev/null || md5sum "$P" | cut -d' ' -f1) )
  [ "$rs" = "$ls" ] && [ "$rm5" = "$lm5" ] || { echo "h6a_pak: copia distinta en el Fold (tamano $rs/$ls, md5 $rm5/$lm5)"; exit 1; }
  echo "h6a_pak: copiado y comprobado en el Fold ($ls bytes, md5 $lm5). La app lo lee en el arranque: '[H6A] wma_adpcm.pak: N sonidos' en el logcat." ;;
*)
  sed -n '2,18p' "$0"; exit 1 ;;
esac
