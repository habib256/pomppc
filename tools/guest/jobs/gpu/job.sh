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
# CE JOB REND UN CODE DE SORTIE (bug hunt T6) : `set -e` pour les étapes de
# préparation, un compteur `ko` pour les verdicts, `exit $ko` à la fin. Ce qui
# compte comme échec :
#   - une scène gltest dont le rc n'est pas 0 sous le PLUGIN (code 6 =
#     GL_RENDERER n'est pas POMPPC, c'est-à-dire plugin non chargé : tout
#     aurait été rendu par Apple et tout aurait été vert) ;
#   - un écart à Apple supérieur au seuil HORS ARÊTES (gltest diff rend 2) ;
#   - « NON TENU » dans la sortie d'une scène (v15 et les relevés) ;
#   - qgpu_test en échec.
# Ce qui ne compte PAS : une scène en échec sous le rendu d'Apple seul (c'est
# la référence, pas notre chaîne), signalée mais pas comptée.
#
#   SCENES="tri tex" : restreindre ; KEEP=1 : garder les PPM dans out/
#   SCENES16="tri game" : restreindre le lot 16 bits (drawable RGB1555, v15)
#   DIFFMAX=3 : seuil de l'écart hors arêtes (défaut 3 : `mix` et `game` sont à
#              3/255 hors arêtes depuis le 19/09 — arrondi de deux rasteriseurs —
#              et ce n'est pas une régression ; en dessous on chasse du bruit)
#   DIFFMAX16=17 : idem pour le lot 16 bits (deux quantifications à 5 bits
#              peuvent différer de 2 pas, 2 × 8/255 ; 8 rougissait gouraud, lit,
#              clip, fogz, texpersp sans qu'aucune image soit fausse)
set -e
. ./lib.sh          # SRC, OUT, SDK, RES, EXT, plugin_layout
SCENES=${SCENES:-"tri gouraud depth fill prims tex texfmt texpack texpersp comb mix state
  stencil depthrt varray varrayvbo game lit texgen clip fogz bigstrip dlist mixte fusion blendc logicop
  polymode stipple occl caps entry v15 tex3d texlod sepspec cube tex13 tex14 tcprobe gl15 texcache
  alpharep texcross readpack drawpack texdelmid vbocolor rawprim offset forkdraw"}
ko=0
bad=""
ko_apple=0
# Un échec de verdict : compté, et dit assez fort pour se voir dans le journal.
fail() { ko=$((ko + 1)); bad="$bad $1"; shift; echo "  ÉCHEC : $*"; }
# GL_RENDERER exigé des exécutions « plugin » : sans cette assertion, un plugin
# non chargé fait rendre TOUT par Apple, et tout est vert (bug hunt T5).
REQ=GLTEST_REQUIRE=POMPPC
DIFFENV="GLTEST_DIFF_MAX=${DIFFMAX:-3}"

echo "== kext"
cd $SRC/kext/POMPPCGPU
make clean >/dev/null 2>&1 || true
make > $OUT/kext-build.txt 2>&1 || { tail -15 $OUT/kext-build.txt; exit 1; }
if kextstat | grep -q net.pomppc.POMPPCGPU; then
  kextunload -b net.pomppc.POMPPCGPU 2>&1
fi
rm -rf /tmp/POMPPCGPU.kext && cp -R POMPPCGPU.kext /tmp/
chown -R root:wheel /tmp/POMPPCGPU.kext && chmod -R 755 /tmp/POMPPCGPU.kext
# `| tail` masquait le code de sortie de kextload : un kext non chargé passait
# pour chargé, et tout le reste du job mentait (bug hunt T5/T6).
kextload -t /tmp/POMPPCGPU.kext > $OUT/kextload.txt 2>&1 || {
  echo "kextload : ÉCHEC"; cat $OUT/kextload.txt; exit 1; }
tail -2 $OUT/kextload.txt
kextstat | grep -i pomppc || true
# (l'ioreg de Tiger n'a pas -r, et -l y échoue en entier : requête par classe)
ioreg -c POMPPCGPU -w 0 2>/dev/null | grep -E '"QGPU(Version|Caps|Backend)"' | sed 's/^[ |]*//' || true

echo "== qgpu_test"
cd $SRC/guest/qgpu-test && make > $OUT/qgpu-test-build.txt 2>&1 || { tail -10 $OUT/qgpu-test-build.txt; exit 1; }
if ./qgpu_test > $OUT/qgpu_test.txt 2>&1; then rq=0; else rq=$?; fi
echo "qgpu_test rc=$rq"; tail -4 $OUT/qgpu_test.txt
[ $rq = 0 ] || fail qgpu_test "qgpu_test rc=$rq"

echo "== plugin"
cd $SRC/guest/gldriver && make > $OUT/plugin-build.txt 2>&1 || { tail -15 $OUT/plugin-build.txt; exit 1; }
ls -l GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC | awk '{print "plugin", $5, "octets"}'
rm -rf $EXT/GLDriver-POMPPC.bundle && cp -R GLDriver-POMPPC.bundle $EXT/
chown -R root:wheel $EXT/GLDriver-POMPPC.bundle && chmod -R 755 $EXT/GLDriver-POMPPC.bundle
plugin_layout
ioreg -c POMPPCAccelerator -w 0 2>/dev/null | grep '"IOGLBundleName"' | sed 's/^[ |]*//' || true

echo "== gltest"
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o gltest gltest.c -framework OpenGL \
  > $OUT/gltest-build.txt 2>&1 || { tail -15 $OUT/gltest-build.txt; exit 1; }
# environnement propre à une scène (dans SENV : une substitution de commande
# non quotée déclencherait le découpage en mots que shellcheck signale)
scene_env() { case $1 in stencil|tex14) SENV=GLTEST_STENCIL=1 ;; *) SENV=GLTEST_NOWS=1 ;; esac; }
# Un relevé qui dit « NON TENU » est un échec : c'est une capacité annoncée et
# pas tenue (bug hunt T19 — personne ne greppait cette chaîne).
# Seules les lignes de relevé (« … : NON TENU ») comptent : la ligne de bilan
# « 0 capacité(s) annoncée(s) NON TENUE(S) » contenait la sous-chaîne et faisait
# un faux rouge sur un v15 pourtant 17/17 (vu le 22/09).
non_tenu() { n=$(grep -c ': NON TENU$' "$1" 2>/dev/null) || n=0; [ -n "$n" ] || n=0; echo $n; }
ok=0
for s in $SCENES; do
  scene_env $s
  if env GLTEST_NOWS=1 $SENV $REQ ./gltest $s 256 256 p-$s.ppm > $OUT/$s.txt 2>&1
  then rp=0; else rp=$?; fi
  if env GLTEST_NOWS=1 $SENV POMPPC_GL_DISABLE=1 ./gltest $s 256 256 a-$s.ppm \
       > $OUT/$s-apple.txt 2>&1
  then ra=0; else ra=$?; fi
  # écart à Apple : « hors arêtes » (là où se juge la tolérance) puis total ;
  # `gltest diff` rend 2 au-delà du seuil (GLTEST_DIFF_MAX), 0 sinon.
  if env $DIFFENV ./gltest diff p-$s.ppm a-$s.ppm > $OUT/$s-diff.txt 2>&1
  then rd=0; else rd=$?; fi
  d=$(sed -n 's/.*écart max \([0-9]*\)\/255.*/\1/p' $OUT/$s-diff.txt | tr '\n' '/')
  r=$(grep -m1 "GL_RENDERER" $OUT/$s.txt | sed 's/GL_RENDERER = //')
  printf "%-10s plugin rc=%d  apple rc=%d  diff rc=%d  écart hors arêtes/total %s  [%s]\n" \
    $s $rp $ra $rd "$d" "$r"
  nt=$(non_tenu $OUT/$s.txt)
  if [ $rp = 0 ]; then ok=$((ok + 1)); else fail $s "$s : gltest rc=$rp (1 témoin faux, 5 scène inconnue, 6 pas POMPPC)"; fi
  # L'écart n'est jugé que si la RÉFÉRENCE a produit son image : sous le rendu
  # d'Apple, entry/texlod/sepspec/cube/tex13/tex14/gl15 échouent (extensions
  # absentes, ou Bus error), et comparer à une image manquante donnait 255/255
  # — sept faux rouges qui ne disaient rien de notre chaîne.
  if [ $ra = 0 ]; then
    [ $rd = 0 ] || fail $s-diff "$s : écart à Apple hors arêtes au-delà du seuil (voir out/$s-diff.txt)"
  elif [ $rd != 0 ]; then
    echo "  ⚠ $s : écart non jugé, la référence Apple a échoué (rc=$ra)"
  fi
  [ "$nt" = 0 ] || fail $s-nontenu "$s : $nt « NON TENU »"
  if [ $ra != 0 ]; then
    ko_apple=$((ko_apple + 1))
    echo "  ⚠ $s : rc=$ra sous le rendu d'Apple seul (référence, pas compté)"
  fi
  if [ -n "$KEEP" ]; then cp p-$s.ppm a-$s.ppm $OUT/ 2>/dev/null || true; fi
done
echo "gltest : $ok OK, $((ko)) en échec :$bad"
[ $ko_apple = 0 ] || echo "⚠ $ko_apple scène(s) en échec sous le rendu d'Apple seul"

# v10 : les textures converties par l'HÔTE (TEX_IMAGE3) doivent donner l'image
# exacte de la conversion par l'invité (POMPPC_GL_TEX3=0, TEX_IMAGE v3).
for s in ${TEXSCENES:-tex texfmt texpack texpersp comb mix game texgen}; do
  case " $SCENES " in *" $s "*) ;; *) continue ;; esac
  if env GLTEST_NOWS=1 $REQ POMPPC_GL_TEX3=0 ./gltest $s 256 256 g-$s.ppm > $OUT/$s-tex3off.txt 2>&1
  then rg=0; else rg=$?; fi
  if env $DIFFENV ./gltest diff p-$s.ppm g-$s.ppm > $OUT/$s-tex3diff.txt 2>&1
  then rd=0; else rd=$?; fi
  d=$(sed -n 's/.*écart max \([0-9]*\)\/255.*/\1/p' $OUT/$s-tex3diff.txt | tr '\n' '/')
  printf "TEX3 %-9s conversion invité rc=%d  diff rc=%d  écart hôte/invité %s\n" $s $rg $rd "$d"
  [ $rg = 0 ] || fail tex3-$s "TEX3 $s : gltest rc=$rg"
  [ $rd = 0 ] || fail tex3-$s-diff "TEX3 $s : la conversion hôte et la conversion invité diffèrent"
done

# v15 : un drawable de 16 bits (GLTEST_COLOR16=1 : RGB1555, Z de 16 bits) doit
# rester dans le domaine, l'hôte convertissant les transferts. L'écart à Apple
# en 16 bits se juge au pas de quantification : 1/31 vaut 8/255, donc quelques
# unités sur les dégradés et zéro sur les couleurs pures — d'où un seuil de 8.
for s in ${SCENES16:-tri gouraud depth fill stencil depthrt occl fogz clip game lit texpersp varray}; do
  case " $SCENES " in *" $s "*) ;; *) continue ;; esac
  scene_env $s
  if env GLTEST_NOWS=1 $SENV $REQ GLTEST_COLOR16=1 ./gltest $s 256 256 q-$s.ppm \
       > $OUT/$s-c16.txt 2>&1
  then rq16=0; else rq16=$?; fi
  if env GLTEST_NOWS=1 $SENV GLTEST_COLOR16=1 POMPPC_GL_DISABLE=1 ./gltest $s 256 256 b-$s.ppm \
       > $OUT/$s-c16-apple.txt 2>&1
  then rb=0; else rb=$?; fi
  if env GLTEST_DIFF_MAX=${DIFFMAX16:-17} ./gltest diff q-$s.ppm b-$s.ppm > $OUT/$s-c16-diff.txt 2>&1
  then rd=0; else rd=$?; fi
  d=$(sed -n 's/.*écart max \([0-9]*\)\/255.*/\1/p' $OUT/$s-c16-diff.txt | tr '\n' '/')
  printf "C16 %-10s plugin rc=%d  apple rc=%d  diff rc=%d  écart hors arêtes/total %s\n" \
    $s $rq16 $rb $rd "$d"
  [ $rq16 = 0 ] || fail c16-$s "C16 $s : gltest rc=$rq16"
  if [ $rb = 0 ]; then
    [ $rd = 0 ] || fail c16-$s-diff "C16 $s : écart à Apple au-delà de ${DIFFMAX16:-17}/255 hors arêtes"
  elif [ $rd != 0 ]; then
    echo "  ⚠ C16 $s : écart non jugé, la référence Apple a échoué (rc=$rb)"
  fi
  if [ -n "$KEEP" ]; then cp q-$s.ppm b-$s.ppm $OUT/ 2>/dev/null || true; fi
done
# et la preuve que c'est bien la v15 qui tient le 16 bits : sans elle, le même
# drawable sort du domaine et l'hôte ne dessine plus rien.
tri_host() { sed -n 's/.*GL: \([0-9]*\) triangles.*/\1/p' "$1" | tail -1; }
env GLTEST_NOWS=1 $REQ GLTEST_COLOR16=1 POMPPC_GL_STATS=1 ./gltest game 256 256 q16.ppm \
  > $OUT/c16-on.txt 2>&1 || fail c16-game "C16 game (v15) : gltest en échec"
env GLTEST_NOWS=1 GLTEST_COLOR16=1 POMPPC_GL_STATS=1 POMPPC_GL_XFER16=0 ./gltest game 256 256 q16off.ppm \
  > $OUT/c16-off.txt 2>&1 || true
printf "C16 game 16 bits : %s triangles sur l'hôte avec la v15, %s sans (XFER16=0)\n" \
  "$(tri_host $OUT/c16-on.txt)" "$(tri_host $OUT/c16-off.txt)"
grep -h 'img/s' $OUT/c16-on.txt $OUT/c16-off.txt | sed 's/^/C16 /' || true

# `exit` ne transporte que 8 bits : un plafond, pour qu'un job très rouge ne
# retombe jamais sur 0 par repliement.
[ $ko -lt 250 ] || ko=250
echo "VERDICT : $ko échec(s) :$bad"
exit $ko
