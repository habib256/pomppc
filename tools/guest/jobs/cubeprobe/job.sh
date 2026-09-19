#!/bin/sh
# Relevé : où GLEngine range les faces d'une carte de cube (docs/re/cartes-de-cube.md).
. ./lib.sh
plugin_install && gltest_build || exit 1
kextstat | grep -q net.pomppc.POMPPCGPU || kext_load
cd $SRC/guest/gltest && mkdir -p $OUT/trace
GLTEST_NOWS=1 POMPPC_GL_TRYCUBE=256 POMPPC_GL_T3DDUMP=1 POMPPC_GLTRACE=$OUT/trace \
  ./gltest cubeprobe 64 64 c.ppm 2>&1 | grep -E "cube|face|RENDERER"
grep -E "emplacement|gldCreateTextureLevel" $OUT/trace/trace.txt | head -30
cd $OUT/trace && ls | grep -v "prm\|dt[0-9]" | grep bin | xargs rm -f
