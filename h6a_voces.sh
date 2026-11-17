#!/bin/bash
# OLA 15C: voces de personajes y del anunciador (streams english/sound/vox/scripted/...) que NO estan en los gamedata del usuario.
# El juego las lee de los localized_English_iwNN.iwd que faltan (el usuario tiene iw00, iw01 e iw05). Sin ellas el port no falla:
# la voz no suena y h5_audio.cpp avisa una vez por fichero.
#
#   h6a_voces.sh faltan [salida.tsv]   lista los alias de stream sin fichero (partida trace de 2 s con H6A_STREAMAUDIT) y el recuento
#   h6a_voces.sh check <iwd|carpeta>   cuantos de los ficheros que faltan trae ese .iwd (o los .iwd de esa carpeta)
#   h6a_voces.sh install <iwd>         lo copia a $P3_GAMEDATA/main tras comprobar que es un zip sin compresion (lo que lee iwd.cpp)
#                                      y vuelve a contar. En el Fold: adb push del .iwd a files/game/main/ de app.bo1native.
#
# Variables: P3 (binario con sonido; por defecto el out_vk_*_h5/p3 mas reciente junto a este script), P3_GAMEDATA.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
export P3_GAMEDATA=${P3_GAMEDATA:-$(cd "$HERE/../../gamedata" && pwd)}
P3=${P3:-$(ls -t "$HERE"/out_vk_*_h5/p3 2>/dev/null | head -1)}
cmd=${1:-}; shift || true
faltan() {   # $1 = tsv de salida
  [ -x "$P3" ] || { echo "h6a_voces: no hay binario con sonido (P3=<ruta>)"; exit 1; }
  ( cd "$HERE" && H6A_SURVEY=1 H6A_STREAMAUDIT="$1" H6A_DEVICE=null H5_GAME=1 H5_PLAY=1 H5_PLAY_SCRIPT=0 H5_PLAY_SECONDS=2 \
      P3_BACKEND=trace "$P3" >/dev/null 2>"$1.log" ) || { echo "h6a_voces: la partida fallo:"; tail -5 "$1.log"; exit 1; }
  grep -a "\[H6A\] streams:" "$1.log" || { echo "h6a_voces: el binario no hace el censo (anterior a la OLA 15C)"; exit 1; }
  rm -f "$1.log"
}
case "$cmd" in
faltan)
  out=${1:-faltan_voces.tsv}; faltan "$out"
  echo "lista completa en $out (columna en_iwd = 0: falta)" ;;
check)
  src=${1:?uso: h6a_voces.sh check <iwd|carpeta>}
  T=$(mktemp -d "${TMPDIR:-/tmp}/h6a_voces.XXXXXX"); trap 'rm -rf "$T"' EXIT
  faltan "$T/s.tsv" >/dev/null
  awk -F'\t' 'NR>1 && $4==0 {gsub(/\\/,"/",$2); print tolower($2)}' "$T/s.tsv" | sort -u > "$T/faltan.txt"
  if [ -d "$src" ]; then set -- "$src"/*.iwd; else set -- "$src"; fi
  for f in "$@"; do unzip -Z1 "$f" 2>/dev/null; done | tr 'A-Z' 'a-z' | sort -u > "$T/trae.txt"
  echo "faltan $(wc -l < "$T/faltan.txt" | tr -d ' ') ficheros de voz; $src trae $(comm -12 "$T/faltan.txt" "$T/trae.txt" | wc -l | tr -d ' ') de ellos" ;;
install)
  f=${1:?uso: h6a_voces.sh install <iwd>}
  [ -f "$f" ] || { echo "h6a_voces: no existe $f"; exit 1; }
  # iwd.cpp solo lee entradas guardadas (metodo 0, stored): comprobarlo antes de copiar
  if unzip -Zv "$f" 2>/dev/null | grep -q "compression method:.*deflated"; then echo "h6a_voces: $f tiene entradas comprimidas (iwd.cpp solo lee 'stored'); no se copia"; exit 1; fi
  cp -n "$f" "$P3_GAMEDATA/main/" && echo "copiado a $P3_GAMEDATA/main/$(basename "$f")"
  faltan "$(mktemp "${TMPDIR:-/tmp}/h6a_voces.XXXXXX")" ;;
*)
  sed -n '2,13p' "$0"; exit 1 ;;
esac
