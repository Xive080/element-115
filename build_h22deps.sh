#!/bin/bash
# OLA F3b: compila libvpx (solo el decodificador VP9) y libopus (solo lo que usa el decodificador, coma flotante) desde sus FUENTES en
# native/third_party/{libvpx,opus} para el video de carga (h22_codec.cpp). Igual en Mac y Android: sin binarios aparte.
# Uso: build_h22deps.sh <dir_salida> <mac|ndk> [CC del destino]
#   Deja <dir_salida>/lib/libvpx.a, <dir_salida>/lib/libopus.a y <dir_salida>/include/{vpx,opus}/. Si ya estan, no hace nada (FORCE=1 rehace).
# Licencias: BSD de 3 clausulas (libvpx: LICENSE + PATENTS; opus: COPYING); avisos en harness2/avisos_terceros/.
set -e
OUT=$1; TGT=$2; CCX=${3:-clang}
H=$(cd "$(dirname "$0")/.." && pwd); T=$H/../third_party; VPXS=$T/libvpx; OPS=$T/opus
[ -f $VPXS/configure ] && [ -f $OPS/include/opus.h ] || { echo "h22deps: faltan las fuentes en third_party/libvpx y third_party/opus"; exit 2; }
if [ -z "$FORCE" ] && [ -f $OUT/lib/libvpx.a ] && [ -f $OUT/lib/libopus.a ] && [ -f $OUT/include/vpx/vpx_decoder.h ] && [ -f $OUT/include/opus/opus.h ]; then exit 0; fi
mkdir -p $OUT/lib $OUT/include/vpx $OUT/include/opus
TB=$(dirname "$(command -v $CCX || echo $CCX)")
NPFX="-ffile-prefix-map=$(cd $H/.. && pwd)/="   # sin rutas /Users/ en los objetos (OLA 15F)
J=${JOBS:-4}

# ---- libvpx: su propio configure (genera vpx_config.h y los rtcd con NEON de arm64) en una carpeta de compilacion aparte
VB=$OUT/vpx_build; rm -rf $VB; mkdir -p $VB
VCONF="--disable-vp8 --disable-vp9-encoder --enable-vp9-decoder --disable-examples --disable-tools --disable-docs --disable-unit-tests
  --disable-webm-io --disable-libyuv --enable-static --disable-shared --disable-install-docs --disable-install-bins --disable-debug --enable-pic --as=auto"
if [ "$TGT" = ndk ]; then
  ( cd $VB && CC=$CCX CXX=${CCX}++ AR=$TB/llvm-ar AS=$CCX LD=$CCX NM=$TB/llvm-nm STRIP=$TB/llvm-strip RANLIB=$TB/llvm-ranlib \
      CFLAGS="$NPFX" CXXFLAGS="$NPFX" $VPXS/configure --target=arm64-android-gcc $VCONF >configure.log 2>&1 ) || { echo "h22deps: configure de libvpx (ndk) fallo; ver $VB/configure.log"; exit 1; }
else
  ( cd $VB && CC=$CCX CXX=${CCX}++ CFLAGS="$NPFX -mmacosx-version-min=13.0" CXXFLAGS="$NPFX -mmacosx-version-min=13.0" LDFLAGS="-mmacosx-version-min=13.0" \
      $VPXS/configure --target=arm64-darwin22-gcc $VCONF >configure.log 2>&1 ) || { echo "h22deps: configure de libvpx (mac) fallo; ver $VB/configure.log"; exit 1; }
fi
( cd $VB && make -j$J libvpx.a >make.log 2>&1 ) || { echo "h22deps: make de libvpx fallo; ver $VB/make.log"; exit 1; }
cp $VB/libvpx.a $OUT/lib/libvpx.a
cp $VPXS/vpx/*.h $OUT/include/vpx/
echo "h22deps: libvpx $(cat $VPXS/VERSION_BO1.txt 2>/dev/null | cut -d' ' -f2) compilado ($TGT)"

# ---- libopus: lista de fuentes del propio opus (celt_sources.mk, silk_sources.mk, opus_sources.mk), coma flotante, sin DNN
OB=$OUT/opus_build; rm -rf $OB; mkdir -p $OB
List() { awk -v v="$2" 'on { c = /\\$/; gsub(/\\/, ""); for (i = 1; i <= NF; i++) print $i; if (!c) on = 0; next } $1 == v && $2 == "=" { on = /\\$/ }' "$OPS/$1"; }
SRCS="$(List celt_sources.mk CELT_SOURCES) $(List silk_sources.mk SILK_SOURCES) $(List silk_sources.mk SILK_SOURCES_FLOAT) $(List opus_sources.mk OPUS_SOURCES) $(List opus_sources.mk OPUS_SOURCES_FLOAT)"
OFL="-O2 -fPIC -w $NPFX -DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINT -DHAVE_LRINTF -DPACKAGE_VERSION=\"1.6.1\" -I$OPS/include -I$OPS/celt -I$OPS/silk -I$OPS/silk/float -I$OPS/src -I$OPS/dnn"
[ "$TGT" != ndk ] && OFL="$OFL -mmacosx-version-min=13.0"
n=0; objs=""
for s in $SRCS; do
  o=$OB/$(echo $s | tr '/' '_').o; objs="$objs $o"
  $CCX $OFL -c $OPS/$s -o $o &
  n=$((n+1)); [ $((n % J)) = 0 ] && wait
done
wait
for o in $objs; do [ -f $o ] || { echo "h22deps: opus no compila $(basename $o)"; exit 1; }; done
rm -f $OUT/lib/libopus.a
if [ "$TGT" = ndk ]; then $TB/llvm-ar rcs $OUT/lib/libopus.a $objs; else ar rcs $OUT/lib/libopus.a $objs; fi
cp $OPS/include/*.h $OUT/include/opus/
echo "h22deps: libopus $(cat $OPS/VERSION_BO1.txt 2>/dev/null | cut -d' ' -f2) compilado ($TGT, $(echo $objs | wc -w | tr -d ' ') fuentes)"
