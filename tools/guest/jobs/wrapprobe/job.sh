#!/bin/sh
# Relevé : couleur de bordure, CLAMP_TO_BORDER, MIRRORED_REPEAT, textures DXT1
# (glCompressedTexImage2D, format générique, mipmaps génériques), vus du pilote,
# et les requêtes de compression (docs/re/bordure-et-compression.md).
. ./lib.sh
plugin_install && gltest_build || exit 1
kextstat | grep -q net.pomppc.POMPPCGPU || kext_load
cd $SRC/guest/gltest
for m in border dxt generic genmip; do
  mkdir -p $OUT/tr-$m
  echo "== $m"
  PROBE_MODE=$m GLTEST_NOWS=1 POMPPC_GL_T3DDUMP=1 POMPPC_GLTRACE=$OUT/tr-$m \
    ./gltest wrapprobe 64 64 w.ppm 2>&1 | grep -E "erreur|pixels|formats|interne|glGetCompressed"
  echo "   Apple :"
  PROBE_MODE=$m GLTEST_NOWS=1 POMPPC_GL_DISABLE=1 ./gltest wrapprobe 64 64 w.ppm 2>&1 | grep -E "erreur|pixels|formats|interne|glGetCompressed"
  ( cd $OUT/tr-$m && ls | grep bin | grep -v "swap-dt3\|swap-prm3\|swap-dt3-niv0" | xargs rm -f )
done
