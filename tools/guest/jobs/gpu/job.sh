#!/bin/sh
# Chaîne GPU complète dans la VM de dev : kext compilé puis chargé depuis /tmp
# (pas installé : un kext qui panique se répare par un redémarrage), qgpu_test,
# plugin INSTALLÉ dans le Resources d'OpenGL.framework, gltest joué scène par
# scène sous le plugin et sous le rendu d'Apple (POMPPC_GL_DISABLE=1), images
# comparées.
#
# Pourquoi installer le plugin : GL_RESOURCES ne marche pas sur ce 10.4.6 —
# même une copie complète des bundles du système y donne kCGLBadCodeModule
# (10015) à tout choix de pixel format (vu en vrai, job diag du 19/09/2026).
#
#   SCENES="tri tex" : restreindre ; KEEP=1 : garder les PPM dans out/
SRC=$PWD/src; OUT=$PWD/out
SDK=/Developer/SDKs/MacOSX10.4u.sdk
SCENES=${SCENES:-"tri gouraud depth fill prims tex texfmt texpack texpersp comb mix state
  stencil depthrt varray game lit texgen clip fogz bigstrip dlist mixte fusion blendc logicop
  polymode stipple occl caps entry v15 tex3d texlod"}

echo "== kext"
cd $SRC/kext/POMPPCGPU && make clean >/dev/null 2>&1
make > $OUT/kext-build.txt 2>&1 || { tail -15 $OUT/kext-build.txt; exit 1; }
if kextstat | grep -q net.pomppc.POMPPCGPU; then
  kextunload -b net.pomppc.POMPPCGPU 2>&1
fi
rm -rf /tmp/POMPPCGPU.kext && cp -R POMPPCGPU.kext /tmp/
chown -R root:wheel /tmp/POMPPCGPU.kext && chmod -R 755 /tmp/POMPPCGPU.kext
kextload -t /tmp/POMPPCGPU.kext 2>&1 | tail -2
kextstat | grep -i pomppc
ioreg -c POMPPCGPU -r 2>/dev/null | grep -E "POMPPCGPU|Version|Caps" | head -5

echo "== qgpu_test"
cd $SRC/guest/qgpu-test && make > $OUT/qgpu-test-build.txt 2>&1 || { tail -10 $OUT/qgpu-test-build.txt; exit 1; }
./qgpu_test > $OUT/qgpu_test.txt 2>&1; echo "qgpu_test rc=$?"; tail -4 $OUT/qgpu_test.txt

echo "== plugin"
cd $SRC/guest/gldriver && make > $OUT/plugin-build.txt 2>&1 || { tail -15 $OUT/plugin-build.txt; exit 1; }
ls -l GLDriver-POMPPC.bundle/GLDriver-POMPPC | awk '{print "plugin", $5, "octets"}'
RES=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources
rm -rf $RES/GLDriver-POMPPC.bundle && cp -R GLDriver-POMPPC.bundle $RES/
chown -R root:wheel $RES/GLDriver-POMPPC.bundle && chmod -R 755 $RES/GLDriver-POMPPC.bundle

echo "== gltest"
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o gltest gltest.c -framework OpenGL \
  > $OUT/gltest-build.txt 2>&1 || { tail -15 $OUT/gltest-build.txt; exit 1; }
# environnement propre à une scène
scene_env() { case $1 in stencil) echo GLTEST_STENCIL=1 ;; *) echo GLTEST_NOWS=1 ;; esac; }
ok=0; ko=0; bad=""
for s in $SCENES; do
  env GLTEST_NOWS=1 $(scene_env $s) ./gltest $s 256 256 p-$s.ppm > $OUT/$s.txt 2>&1; rp=$?
  env GLTEST_NOWS=1 $(scene_env $s) POMPPC_GL_DISABLE=1 ./gltest $s 256 256 a-$s.ppm \
    > $OUT/$s-apple.txt 2>&1; ra=$?
  # écart à Apple : « hors arêtes » (là où se juge la tolérance) puis total
  d=$(./gltest diff p-$s.ppm a-$s.ppm 2>&1 | sed -n 's/.*écart max \([0-9]*\)\/255.*/\1/p' | tr '\n' '/')
  r=$(grep -m1 "GL_RENDERER" $OUT/$s.txt | sed 's/GL_RENDERER = //')
  printf "%-10s plugin rc=%d  apple rc=%d  écart hors arêtes/total %s  [%s]\n" $s $rp $ra "$d" "$r"
  if [ $rp = 0 ]; then ok=$((ok+1)); else ko=$((ko+1)); bad="$bad $s"; fi
  [ -n "$KEEP" ] && cp p-$s.ppm a-$s.ppm $OUT/
done
echo "gltest : $ok OK, $ko en échec :$bad"

# v10 : les textures converties par l'HÔTE (TEX_IMAGE3) doivent donner l'image
# exacte de la conversion par l'invité (POMPPC_GL_TEX3=0, TEX_IMAGE v3).
for s in ${TEXSCENES:-tex texfmt texpack texpersp comb mix game texgen}; do
  case " $SCENES " in *" $s "*) ;; *) continue ;; esac
  env GLTEST_NOWS=1 POMPPC_GL_TEX3=0 ./gltest $s 256 256 g-$s.ppm > $OUT/$s-tex3off.txt 2>&1; rg=$?
  d=$(./gltest diff p-$s.ppm g-$s.ppm 2>&1 | sed -n 's/.*écart max \([0-9]*\)\/255.*/\1/p' | tr '\n' '/')
  printf "TEX3 %-9s conversion invité rc=%d  écart hôte/invité %s\n" $s $rg "$d"
done
