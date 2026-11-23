#!/bin/bash
# map_pipeline.sh (OLA M0): datos derivados POR MAPA, cada uno dentro de la carpeta del mapa (nunca en git ni en el APK).
#
#   map_pipeline.sh check  <zona>   bo1maps.py check --deep de la carpeta (todo el catalogo presente y con sha/CRC correctos)
#   map_pipeline.sh pak    <zona>   sonido: h6a_pak.sh build <zona> con P3_GAMEDATA=<carpeta>  -> <carpeta>/h6a/wma_adpcm.pak
#   map_pipeline.sh texpkg <zona>   texturas ASTC del movil: partida corta grabando niveles DXT (V9_TEXREC) y texcache_build
#                                   -> <carpeta>/tex.pkg (+ tex_report.txt). h18_maps lo usa solo para ese mapa (V9_TEXPKG).
#   map_pipeline.sh verify <zona>   (OLA T1) dos partidas con la config del Fold (Alto: maxTex 2048, picmip 0; V9_MAX_MB 1024): con tex.pkg grabando
#                                   (cobertura: niveles/texturas que faltan, maps/texpkg_cover.py) y sin tex.pkg; deja <carpeta>/tex_verify.txt
#   map_pipeline.sh all    <zona>   check + pak + texpkg
#
# Carpeta del mapa: $BO1_MAPS/<zona> (por defecto native/gamedata_maps/<zona>, la de bo1maps.py fetch). Five en la base
# (native/gamedata) tambien vale: MAPDIR=<carpeta> fuerza otra. Variables: P3 (binario H5G con sonido, por defecto
# out_vk_m0_h5/p3 o out_vk_h5/p3), SEG (segundos de la partida de grabacion, 100), TEXCACHE (binario texcache_build; se compila si falta).
# OLA T1: se graba con h10_maxTex=TEXMAX (2048, el perfil Alto del Fold) y picmip 0 en un config.cfg propio (H10_CFG; si no, manda el
# players/config.cfg de la carpeta, con 1024: faltaban los mips de 2048 y en Alto esas texturas caian enteras a RGBA8) y presupuesto
# TEXREC_MB (8000: con menos, la degradacion por presupuesto quita mips ANTES de grabarlos). Durante la partida,
# TOUR (H5_TIMED, devgui con H5_DEV=1) da corriente, abre puertas y lleva al jugador por maquinas, cajas, puertas y escombros
# (TOUR_NOSTOPS=1 sin paradas). Si una partida de verify cae, su log queda en <carpeta>/tex_verify_<etiqueta>.log.
# Al Fold: nativeapp/deploy_data.sh --map <zona> --push (Five, la base: deploy_data.sh --texpkg native/gamedata/tex.pkg --push).
set -e
HERE=$(cd "$(dirname "$0")" && pwd); H2=$(cd "$HERE/.." && pwd); NAT=$(cd "$H2/../.." && pwd)
cmd=${1:-}; Z=${2:-}
[ -n "$cmd" ] && [ -n "$Z" ] || { sed -n 2,20p "$0"; exit 1; }
ROOT=${BO1_MAPS:-$NAT/gamedata_maps}; D=${MAPDIR:-$ROOT/$Z}
[ -d "$D" ] || { echo "map_pipeline: no existe $D (bo1maps.py fetch $Z)"; exit 1; }
D=$(cd "$D" && pwd)
P3=${P3:-}; [ -z "$P3" ] && for c in out_vk_m0_h5/p3 out_vk_h5/p3; do [ -x "$H2/$c" ] && { P3=$H2/$c; break; }; done
run_check() { python3 "$HERE/bo1maps.py" check --root "$(dirname "$D")" --map "$Z" --deep; }
run_pak() { P3_GAMEDATA=$D P3=$P3 PAK=$D/h6a/wma_adpcm.pak "$H2/h6a_pak.sh" build "$Z"; }
mkcfg() { printf 'seta h10_maxTex "%s"\nseta r_picmip_manual "1"\nseta r_picmip "0"\nseta r_picmip_bump "0"\nseta r_picmip_spec "0"\nseta r_picmip_water "0"\n' "${TEXMAX:-2048}" > "$1/config.cfg"; }
run_texpkg() {
  [ -x "$P3" ] || { echo "map_pipeline: falta el binario (P3=...)"; exit 1; }
  TC=${TEXCACHE:-$NAT/h0/vk/tools/texcache_build/out/texcache_build}
  [ -x "$TC" ] || "$NAT/h0/vk/tools/texcache_build/build.sh" >/dev/null
  T=$(mktemp -d "${TMPDIR:-/tmp}/texrec.XXXXXX"); trap 'rm -rf "$T"' EXIT
  mkcfg "$T"; mkdir -p "$T/rec"   # RecordLevel (vk_texcache_fmt.h) no crea la carpeta: sin ella no se graba nada
  echo "map_pipeline: grabando texturas de $Z (${SEG:-100} s de partida, maxTex ${TEXMAX:-2048}) ..."
  ( cd "$H2" && env P3_GAMEDATA="$D" H5_MAP="$Z" P3_TRACE=off V9_TEXREC="$T/rec" V9_TEXPKG= V9_CACHE_DIR="$T/v9cache" V9_SHADERCACHE="$T/v9.spvc" \
      H10_CFG="$T/config.cfg" V9_MAX_MB=${TEXREC_MB:-8000} V9_RESMAX_MB=64 V9_VALIDATE=0 H5_GAME=1 H5_PLAY=1 H5_PLAY_SCRIPT=2 H5_VIEW=1 H6A_DEVICE=null \
      H18_NOVIDEO=1 H5_DEV=1 H5_TIMED="$TOUR" H5_SET="bo1_testclient_god 1" H5_PLAY_SHOT=0 H5_PLAY_SECONDS=${SEG:-100} P4_OUT="$T/m.png" \
      ../vk/run_vk.sh "$P3" >/dev/null 2>"$T/rec.log" ) \
    || { echo "map_pipeline: la partida de grabacion fallo:"; tail -5 "$T/rec.log"; exit 1; }
  grep -a '\[H10\]\|\[H5E\]\|\[v9\] paquete\|ERROR\|ASSERT' "$T/rec.log" | cut -c1-300 > "$D/tex_rec_guion.txt" || true
  echo "map_pipeline: guion: $(grep -c 'NO se mueve\|no existe' "$T/rec.log") paradas sin sitio de $(grep -c 'guion: tpdmg' "$T/rec.log") (ver $D/tex_rec_guion.txt)"
  "$TC" build "$T/rec" "$D/tex.pkg.part" --report "$D/tex_report.txt" && mv "$D/tex.pkg.part" "$D/tex.pkg"
  echo "map_pipeline: $D/tex.pkg ($(du -h "$D/tex.pkg" | cut -f1))"
}
# Fold (android_main): V9_MAX_MB 1024 y perfil Alto (maxTex 2048). Una partida con <carpeta>/tex.pkg y otra sin paquete.
run_verify() {
  [ -x "$P3" ] && [ -f "$D/tex.pkg" ] || { echo "map_pipeline: falta el binario o $D/tex.pkg"; exit 1; }
  T=$(mktemp -d "${TMPDIR:-/tmp}/texver.XXXXXX"); trap 'rm -rf "$T"' EXIT
  mkcfg "$T"; mkdir -p "$T/rec"; O=$D/tex_verify.txt
  # V9_TEXPKG explicito: con P3_GAMEDATA = carpeta del mapa, h18_maps la toma por base y no fija el paquete (en la app si: base game/ + maps/)
  vrun() {   # $1 = etiqueta, $2 = V9_TEXREC (vacio = no grabar), $3 = V9_TEXPKG (vacio = sin paquete)
    ( cd "$H2" && env P3_GAMEDATA="$D" H5_MAP="$Z" P3_TRACE=off V9_TEXREC="$2" V9_TEXPKG="$3" V9_CACHE_DIR="$T/v9cache" V9_SHADERCACHE="$T/v9.spvc" \
        H10_CFG="$T/config.cfg" V9_MAX_MB=${VMAX_MB:-1024} V9_RESMAX_MB=64 V9_VALIDATE=0 H5_GAME=1 H5_PLAY=1 H5_PLAY_SCRIPT=2 H5_VIEW=1 \
        H6A_DEVICE=null H18_NOVIDEO=1 H5_DEV=1 H5_TIMED="$VTOUR" H5_SET="bo1_testclient_god 1" H5_PLAY_SHOT=0 H5_PLAY_SECONDS=${VSEG:-120} \
        P4_OUT="$T/m.png" H15_MEMPROFILE="$T/$1.mem.csv" V9_CSV="$T/$1.v9.csv" ../vk/run_vk.sh "$P3" >/dev/null 2>"$T/$1.log" ) \
      || { echo "map_pipeline: la partida $1 fallo:"; cp "$T/$1.log" "$D/tex_verify_$1.log"; tail -5 "$T/$1.log"; exit 1; }
    local rss tex fb; rss=$(awk -F, 'NR>1&&$4>m{m=$4}END{printf "%d",m/1024}' "$T/$1.mem.csv")
    tex=$(awk -F, 'NR>1&&$19>m{m=$19}END{printf "%d",m/1048576}' "$T/$1.v9.csv"); fb=$(awk -F, 'NR>1{s+=$9}END{printf "%d",s/1048576}' "$T/$1.v9.csv")
    echo "$1: RSS pico $rss MB, texturas pico $tex MB, RGBA8 de respaldo subido en partida $fb MB, $(grep -c 'sin acierto de cache' "$T/$1.log") relocks sin acierto, $(grep -m1 'paquete de texturas' "$T/$1.log" || echo 'sin paquete')" >> "$O"
  }
  : > "$O"; echo "map_pipeline: verificando $Z (2 partidas de ${VSEG:-120} s) ..."
  vrun con_pkg "$T/rec" "$D/tex.pkg"
  python3 "$HERE/texpkg_cover.py" "$D/tex.pkg" "$T/rec" >> "$O"
  vrun sin_pkg "" ""
  cat "$O"
}
TOUR=${TOUR-"2:dvar zombie_devgui power_on;5:dvar zombie_devgui open_sesame;6:dvar player_sustainAmmo 1"}
VTOUR=${VTOUR-$TOUR}
if [ -z "${TOUR_NOSTOPS:-}" ]; then t=14
  for tn in zombie_vending treasure_chest_use zombie_door zombie_debris; do for i in 0 1 2 3 4 5 6 7 8; do
    TOUR="$TOUR;$t:tpdmg $tn $i 200"; VTOUR="$VTOUR;$((t+3)):tpdmg $tn $((8-i)) 200"; t=$((t+2)); done; done
fi
case "$cmd" in
  check) run_check ;;
  verify) run_verify ;;
  pak) run_pak ;;
  texpkg) run_texpkg ;;
  all) run_check; run_pak; run_texpkg ;;
  *) sed -n 2,20p "$0"; exit 1 ;;
esac
