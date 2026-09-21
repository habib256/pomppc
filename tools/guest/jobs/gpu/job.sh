#!/bin/sh
# Chaîne GPU complète dans la VM de dev : kext compilé puis chargé depuis /tmp
# (pas installé : un kext qui panique se répare par un redémarrage), qgpu_test,
# plugin INSTALLÉ dans /System/Library/Extensions (GLEngine l'y charge par
# l'IOGLBundleName de l'accélérateur que publie le kext, tâche 4.2), gltest
# joué scène par scène sous le plugin et sous le rendu d'Apple
# (POMPPC_GL_DISABLE=1), images comparées.
#
# Pourquoi installer le plugin : GL_RESOURCES ne marche pas sur ce 10.4.6 —
# même une copie complète des bundles du système y donne kCGLBadCodeModule
# (10015) à tout choix de pixel format (vu en vrai, job diag du 19/09/2026).
#
#   SCENES="tri tex" : restreindre ; KEEP=1 : garder les PPM dans out/
#   SCENES16="tri game" : restreindre le lot 16 bits (drawable RGB1555, v15)
. ./lib.sh          # SRC, OUT, SDK, RES, EXT, plugin_layout
SCENES=${SCENES:-"tri gouraud depth fill prims tex texfmt texpack texpersp comb mix state
  stencil depthrt varray varrayvbo game lit texgen clip fogz bigstrip dlist mixte fusion blendc logicop
  polymode stipple occl caps entry v15 tex3d texlod sepspec cube tex13 tex14 tcprobe gl15 texcache"}

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
# (l'ioreg de Tiger n'a pas -r, et -l y échoue en entier : requête par classe)
ioreg -c POMPPCGPU -w 0 2>/dev/null | grep -E '"QGPU(Version|Caps|Backend)"' | sed 's/^[ |]*//'

echo "== qgpu_test"
cd $SRC/guest/qgpu-test && make > $OUT/qgpu-test-build.txt 2>&1 || { tail -10 $OUT/qgpu-test-build.txt; exit 1; }
./qgpu_test > $OUT/qgpu_test.txt 2>&1; echo "qgpu_test rc=$?"; tail -4 $OUT/qgpu_test.txt

echo "== plugin"
cd $SRC/guest/gldriver && make > $OUT/plugin-build.txt 2>&1 || { tail -15 $OUT/plugin-build.txt; exit 1; }
ls -l GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC | awk '{print "plugin", $5, "octets"}'
rm -rf $EXT/GLDriver-POMPPC.bundle && cp -R GLDriver-POMPPC.bundle $EXT/
chown -R root:wheel $EXT/GLDriver-POMPPC.bundle && chmod -R 755 $EXT/GLDriver-POMPPC.bundle
plugin_layout
ioreg -c POMPPCAccelerator -w 0 2>/dev/null | grep '"IOGLBundleName"' | sed 's/^[ |]*//'

echo "== gltest"
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o gltest gltest.c -framework OpenGL \
  > $OUT/gltest-build.txt 2>&1 || { tail -15 $OUT/gltest-build.txt; exit 1; }
# environnement propre à une scène
scene_env() { case $1 in stencil|tex14) echo GLTEST_STENCIL=1 ;; *) echo GLTEST_NOWS=1 ;; esac; }
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

# v15 : un drawable de 16 bits (GLTEST_COLOR16=1 : RGB1555, Z de 16 bits) doit
# rester dans le domaine, l'hôte convertissant les transferts. L'écart à Apple
# en 16 bits se juge au pas de quantification : 1/31 vaut 8/255, donc quelques
# unités sur les dégradés et zéro sur les couleurs pures.
for s in ${SCENES16:-tri gouraud depth fill stencil depthrt occl fogz clip game lit texpersp varray}; do
  case " $SCENES " in *" $s "*) ;; *) continue ;; esac
  env GLTEST_NOWS=1 $(scene_env $s) GLTEST_COLOR16=1 ./gltest $s 256 256 q-$s.ppm \
    > $OUT/$s-c16.txt 2>&1; rq=$?
  env GLTEST_NOWS=1 $(scene_env $s) GLTEST_COLOR16=1 POMPPC_GL_DISABLE=1 ./gltest $s 256 256 b-$s.ppm \
    > $OUT/$s-c16-apple.txt 2>&1; rb=$?
  d=$(./gltest diff q-$s.ppm b-$s.ppm 2>&1 | sed -n 's/.*écart max \([0-9]*\)\/255.*/\1/p' | tr '\n' '/')
  printf "C16 %-10s plugin rc=%d  apple rc=%d  écart hors arêtes/total %s\n" $s $rq $rb "$d"
  [ -n "$KEEP" ] && cp q-$s.ppm b-$s.ppm $OUT/
done
# et la preuve que c'est bien la v15 qui tient le 16 bits : sans elle, le même
# drawable sort du domaine et l'hôte ne dessine plus rien.
tri_host() { sed -n 's/.*GL: \([0-9]*\) triangles.*/\1/p' "$1" | tail -1; }
env GLTEST_NOWS=1 GLTEST_COLOR16=1 POMPPC_GL_STATS=1 ./gltest game 256 256 q16.ppm \
  > $OUT/c16-on.txt 2>&1
env GLTEST_NOWS=1 GLTEST_COLOR16=1 POMPPC_GL_STATS=1 POMPPC_GL_XFER16=0 ./gltest game 256 256 q16off.ppm \
  > $OUT/c16-off.txt 2>&1
printf "C16 game 16 bits : %s triangles sur l'hôte avec la v15, %s sans (XFER16=0)\n" \
  "$(tri_host $OUT/c16-on.txt)" "$(tri_host $OUT/c16-off.txt)"
grep -h 'img/s' $OUT/c16-on.txt $OUT/c16-off.txt | sed 's/^/C16 /'
