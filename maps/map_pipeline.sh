#!/bin/bash
# map_pipeline.sh (OLA M0): datos derivados POR MAPA, cada uno dentro de la carpeta del mapa (nunca en git ni en el APK).
#
#   map_pipeline.sh check  <zona>   bo1maps.py check --deep de la carpeta (todo el catalogo presente y con sha/CRC correctos)
#   map_pipeline.sh pak    <zona>   sonido: h6a_pak.sh build <zona> con P3_GAMEDATA=<carpeta>  -> <carpeta>/h6a/wma_adpcm.pak
#   map_pipeline.sh texpkg <zona>   texturas ASTC del movil: partida corta grabando niveles DXT (V9_TEXREC) y texcache_build
#                                   -> <carpeta>/tex.pkg (+ tex_report.txt). h18_maps lo usa solo para ese mapa (V9_TEXPKG).
#   map_pipeline.sh all    <zona>   check + pak + texpkg
#
# Carpeta del mapa: $BO1_MAPS/<zona> (por defecto native/gamedata_maps/<zona>, la de bo1maps.py fetch). Five en la base
# (native/gamedata) tambien vale: MAPDIR=<carpeta> fuerza otra. Variables: P3 (binario H5G con sonido, por defecto
# out_vk_m0_h5/p3 o out_vk_h5/p3), SEG (segundos de la partida de grabacion, 60), TEXCACHE (binario texcache_build; se compila si falta).
# Al Fold: deploy por mapa pendiente (deploy_data.sh copia una sola carpeta game/; ver docs/decisions, OLA M0).
set -e
HERE=$(cd "$(dirname "$0")" && pwd); H2=$(cd "$HERE/.." && pwd); NAT=$(cd "$H2/../.." && pwd)
cmd=${1:-}; Z=${2:-}
[ -n "$cmd" ] && [ -n "$Z" ] || { sed -n 2,14p "$0"; exit 1; }
ROOT=${BO1_MAPS:-$NAT/gamedata_maps}; D=${MAPDIR:-$ROOT/$Z}
[ -d "$D" ] || { echo "map_pipeline: no existe $D (bo1maps.py fetch $Z)"; exit 1; }
D=$(cd "$D" && pwd)
P3=${P3:-}; [ -z "$P3" ] && for c in out_vk_m0_h5/p3 out_vk_h5/p3; do [ -x "$H2/$c" ] && { P3=$H2/$c; break; }; done
run_check() { python3 "$HERE/bo1maps.py" check --root "$(dirname "$D")" --map "$Z" --deep; }
run_pak() { P3_GAMEDATA=$D P3=$P3 PAK=$D/h6a/wma_adpcm.pak "$H2/h6a_pak.sh" build "$Z"; }
run_texpkg() {
  [ -x "$P3" ] || { echo "map_pipeline: falta el binario (P3=...)"; exit 1; }
  TC=${TEXCACHE:-$NAT/h0/vk/tools/texcache_build/out/texcache_build}
  [ -x "$TC" ] || "$NAT/h0/vk/tools/texcache_build/build.sh" >/dev/null
  T=$(mktemp -d "${TMPDIR:-/tmp}/texrec.XXXXXX"); trap 'rm -rf "$T"' EXIT
  echo "map_pipeline: grabando texturas de $Z (${SEG:-60} s de partida) ..."
  ( cd "$H2" && env P3_GAMEDATA="$D" H5_MAP="$Z" P3_TRACE=off V9_TEXREC="$T/rec" V9_CACHE_DIR="$T/v9cache" V9_SHADERCACHE="$T/v9.spvc" \
      V9_MAXTEX=512 V9_MAX_MB=1200 V9_RESMAX_MB=64 V9_VALIDATE=0 H5_GAME=1 H5_PLAY=1 H5_PLAY_SCRIPT=2 H5_VIEW=1 H6A_DEVICE=null \
      H5_SET="bo1_testclient_god 1" H5_PLAY_SHOT=0 H5_PLAY_SECONDS=${SEG:-60} P4_OUT="$T/m.png" ../vk/run_vk.sh "$P3" >/dev/null 2>"$T/rec.log" ) \
    || { echo "map_pipeline: la partida de grabacion fallo:"; tail -5 "$T/rec.log"; exit 1; }
  "$TC" build "$T/rec" "$D/tex.pkg.part" --report "$D/tex_report.txt" && mv "$D/tex.pkg.part" "$D/tex.pkg"
  echo "map_pipeline: $D/tex.pkg ($(du -h "$D/tex.pkg" | cut -f1))"
}
case "$cmd" in
  check) run_check ;;
  pak) run_pak ;;
  texpkg) run_texpkg ;;
  all) run_check; run_pak; run_texpkg ;;
  *) sed -n 2,14p "$0"; exit 1 ;;
esac
