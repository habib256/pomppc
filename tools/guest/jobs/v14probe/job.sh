#!/bin/sh
# Relevé d'OpenGL 1.4 : GL_COLOR_SUM, biais de LOD d'unité, ce que GLEngine
# accepte (stencil wrap, source croisée, textures de profondeur et ombre), et
# les champs de l'objet texture (docs/re/opengl-1.4.md).
. ./lib.sh
plugin_install && gltest_build || exit 1
kextstat | grep -q net.pomppc.POMPPCGPU || kext_load
cd $SRC/guest/gltest
for m in ${MODES:-annonce allext apple}; do
  case $m in
    annonce) e="" ;; allext) e="POMPPC_GL_ALLEXT=1" ;; apple) e="POMPPC_GL_DISABLE=1" ;;
  esac
  mkdir -p $OUT/tr-$m
  echo "== $m"
  env GLTEST_NOWS=1 POMPPC_GLTRACE_STATE=1 POMPPC_GL_T3DDUMP=1 POMPPC_GLTRACE=$OUT/tr-$m $e \
    ./gltest v14probe 64 64 w.ppm 2>&1 | grep -v "^  \[vidage\|^renderers\|^  \[[0-9]\]\|^CGL"
  ( cd $OUT/tr-$m && ls | grep bin | grep -v "clear-glstate\|clear-gctxlow\|swap-dt3\|swap-prm3" | xargs rm -f )
done
