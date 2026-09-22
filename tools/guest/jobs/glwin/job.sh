#!/bin/sh
# Mode bureau (devloop.py start --gui) : kext chargé depuis /tmp, puis glwin
# (fenêtre GLUT, double tampon) dans la session de l'utilisateur, par le relais
# POMPPCGuiRunner. Suppose le plugin installé (job gpu).
#
# Rend le nombre d'échecs (bug hunt T6) : gui_run rend maintenant le rc de la
# commande lancée dans la session (T4), et les deux images produites — la
# nôtre et celle d'Apple — sont ENFIN COMPARÉES ; elles étaient rapatriées côte
# à côte sans que personne ne les regarde.
#
# La largeur 320 (960 octets par ligne, multiple de 4) cachait P1 : une
# relecture glReadPixels en GL_RGB avec GL_PACK_ALIGNMENT = 1 n'y déborde pas.
# D'où une seconde passe en largeur IMPAIRE (321 → 963 octets), qui est
# exactement le cas de P1 ; si le plugin fabrique son propre pas, glwin écrit
# au-delà de son tampon et le dit (plantage ou pixels faux).
set -e
. ./guilib.sh
SRC=$PWD/src; OUT=$PWD/out; SDK=/Developer/SDKs/MacOSX10.4u.sdk
ko=0
fail() { ko=$((ko + 1)); echo "  ÉCHEC : $*"; }
run_gui() {    # run_gui étiquette "commande" délai
  if gui_run "$2" "$3" > $OUT/gui-$1.txt 2>&1; then _rc=0; else _rc=$?; fi
  sed "s/^/$1 /" $OUT/gui-$1.txt
  echo "$1 : rc=$_rc (124 = timeout du relais, 125 = relais muet)"
  return $_rc
}
cd $SRC/kext/POMPPCGPU && make > /dev/null 2>&1 || { echo "kext : échec de compilation"; exit 1; }
if ! kextstat | grep -q net.pomppc.POMPPCGPU; then
  rm -rf /tmp/POMPPCGPU.kext && cp -R POMPPCGPU.kext /tmp/
  chown -R root:wheel /tmp/POMPPCGPU.kext && chmod -R 755 /tmp/POMPPCGPU.kext
  kextload -t /tmp/POMPPCGPU.kext > $OUT/kextload.txt 2>&1 ||
    { echo "kextload : ÉCHEC"; cat $OUT/kextload.txt; exit 1; }
  tail -1 $OUT/kextload.txt
fi
kextstat | grep -i pomppc | awk '{print "kext :", $6}' || true
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o glwin glwin.c -framework GLUT -framework OpenGL || exit 1
# gltest sert ici d'outil de comparaison d'images (`gltest diff`).
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o gltest gltest.c -framework OpenGL \
  > $OUT/gltest-build.txt 2>&1 || { tail -15 $OUT/gltest-build.txt; exit 1; }
echo "== plugin"
run_gui glwin "./glwin 120 320 240 glwin.ppm 6" 240 || fail "glwin (plugin)"
echo "== rendu d'Apple"
run_gui glwin-apple "POMPPC_GL_DISABLE=1 ./glwin 120 320 240 glwin-apple.ppm 2" 240 ||
  echo "  ⚠ glwin sous Apple (référence) en échec"
echo "== comparaison des deux images"
if [ -f glwin.ppm ] && [ -f glwin-apple.ppm ]; then
  if env GLTEST_DIFF_MAX=${DIFFMAX:-2} ./gltest diff glwin.ppm glwin-apple.ppm \
       > $OUT/glwin-diff.txt 2>&1
  then rd=0; else rd=$?; fi
  cat $OUT/glwin-diff.txt
  [ $rd = 0 ] || fail "glwin : écart à Apple hors arêtes au-delà du seuil (out/glwin-diff.txt)"
else
  fail "glwin : une des deux images manque"
fi
# P1 : largeur impaire, 963 octets par ligne, GL_PACK_ALIGNMENT = 1
echo "== largeur impaire (P1)"
run_gui glwin-impair "./glwin 30 321 241 glwin-321.ppm 2" 240 ||
  fail "glwin en 321x241 (relecture RGB non alignée : P1)"
cp glwin.ppm glwin-apple.ppm glwin-321.ppm $OUT/ 2>/dev/null || true
[ $ko -lt 250 ] || ko=250
echo "VERDICT : $ko échec(s)"
exit $ko
