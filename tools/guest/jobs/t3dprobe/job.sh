#!/bin/sh
# Relevé : où GLEngine range une texture 3D (docs/re/textures-3d.md). gltest
# t3dprobe avec POMPPC_GL_TRY3D et la sonde de cible du plugin ; les vidages
# reviennent dans out/trace.
. ./lib.sh
kext_load && plugin_install && gltest_build || exit 1
cd $SRC/guest/gltest && mkdir -p $OUT/trace
GLTEST_NOWS=1 T3D_LOD=1 POMPPC_GL_T3DDUMP=1 POMPPC_GLTRACE=$OUT/trace \
  ./gltest t3dprobe 64 64 t.ppm 2>&1 | grep -E "Image3D|texel|RENDERER|FAIL|LOD"
echo "== rendu d'Apple seul"
GLTEST_NOWS=1 POMPPC_GL_TRY3D=256 POMPPC_GL_DISABLE=1 ./gltest t3dprobe 64 64 a.ppm 2>&1 |
  grep -E "Image3D|texel|RENDERER"
grep -E "SONDE|emplacement|vidage .*cible|NO_TEX|refus" $OUT/trace/trace.txt | head -30
