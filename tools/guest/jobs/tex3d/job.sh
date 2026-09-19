#!/bin/sh
# Textures 3D de bout en bout (OpenGL 1.2, protocole v10) : scène tex3d par le
# chemin brut (défaut) et par le chemin hérité (POMPPC_GL_GEOM=0), puis avec la
# 3D coupée (POMPPC_GL_TEX3D=0) : elle ne doit plus être annoncée.
. ./lib.sh
kext_load && plugin_install && gltest_build || exit 1
cd $SRC/guest/gltest
for mode in ${MODES:-brut herite coupe}; do
  case $mode in
    brut)   e="" ;;
    herite) e="POMPPC_GL_GEOM=0" ;;
    coupe)  e="POMPPC_GL_TEX3D=0" ;;
  esac
  echo "== $mode"
  env GLTEST_NOWS=1 POMPPC_GL_STATS=$OUT/stats-$mode.txt $e ./gltest tex3d 256 256 t3-$mode.ppm 2>&1 |
    grep -E "MAX_3D|ok  |FAIL|OK \(|ÉCHEC|RENDERER|POMPPC GL"
done
tail -12 $OUT/stats-brut.txt 2>/dev/null
