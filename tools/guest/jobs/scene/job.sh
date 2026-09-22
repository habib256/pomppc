#!/bin/sh
# Une ou plusieurs scènes gltest ($SCENES) sous le plugin, chemin brut et chemin
# hérité (POMPPC_GL_GEOM=0), et sous le rendu d'Apple seul. Suppose le kext
# chargé ou le recharge.
#
# Rend le nombre d'échecs (bug hunt T6) : avant, le rc du job était celui du
# dernier `grep` du dernier tube, c'est-à-dire n'importe quoi. Les modes
# « brut » et « hérité » exigent GL_RENDERER = POMPPC (T5) : sans cela, un
# plugin non chargé fait tout rendre par Apple et tout paraît vert.
set -e
. ./lib.sh
plugin_install && gltest_build || exit 1
kextstat | grep -q net.pomppc.POMPPCGPU || kext_load
cd $SRC/guest/gltest
ko=0
for s in ${SCENES:-sepspec}; do
  for mode in brut herite apple; do
    case $mode in
      brut) e="GLTEST_REQUIRE=POMPPC" ;;
      herite) e="GLTEST_REQUIRE=POMPPC POMPPC_GL_GEOM=0" ;;
      apple) e="POMPPC_GL_DISABLE=1" ;;
    esac
    case $s in stencil|tex14) e="$e GLTEST_STENCIL=1" ;; esac
    echo "== $s / $mode"
    if env GLTEST_NOWS=1 POMPPC_GL_STATS=/dev/null $e ./gltest $s 256 256 o-$s-$mode.ppm \
         > o-$s-$mode.txt 2>&1
    then rc=0; else rc=$?; fi
    grep -E "ok  |FAIL|OK \(|ÉCHEC|refus|NON TENU" o-$s-$mode.txt || true
    echo "   rc=$rc"
    # Le rendu d'Apple est la RÉFÉRENCE, pas notre chaîne : son échec se dit,
    # il ne rougit pas le job.
    if [ $rc != 0 ]; then
      if [ $mode = apple ]; then echo "   ⚠ échec sous Apple (référence)"
      else ko=$((ko + 1)); echo "   ÉCHEC : $s / $mode rc=$rc"; fi
    fi
    if grep -q ': NON TENU$' o-$s-$mode.txt && [ $mode != apple ]; then
      ko=$((ko + 1)); echo "   ÉCHEC : $s / $mode annonce une capacité NON TENUE"
    fi
    cp o-$s-$mode.txt $OUT/ 2>/dev/null || true
  done
done
[ $ko -lt 250 ] || ko=250
echo "VERDICT : $ko échec(s)"
exit $ko
