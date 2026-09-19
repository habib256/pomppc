#!/bin/sh
# Une ou plusieurs scènes gltest ($SCENES) sous le plugin, chemin brut et chemin
# hérité (POMPPC_GL_GEOM=0), et sous le rendu d'Apple seul. Suppose le kext
# chargé ou le recharge.
. ./lib.sh
plugin_install && gltest_build || exit 1
kextstat | grep -q net.pomppc.POMPPCGPU || kext_load
cd $SRC/guest/gltest
for s in ${SCENES:-sepspec}; do
  for mode in brut herite apple; do
    case $mode in
      brut) e="" ;; herite) e="POMPPC_GL_GEOM=0" ;; apple) e="POMPPC_GL_DISABLE=1" ;;
    esac
    echo "== $s / $mode"
    env GLTEST_NOWS=1 POMPPC_GL_STATS=/dev/null $e ./gltest $s 256 256 o.ppm 2>&1 |
      grep -E "ok  |FAIL|OK \(|ÉCHEC|refus"
  done
done
