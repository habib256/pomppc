#!/bin/sh
# Installation réelle par guest/gldriver/install.sh (kext et plugin dans
# /System/Library/Extensions), vérification de la garde contre un second
# exemplaire du plugin, puis redémarrage : au démarrage suivant, le kext est
# chargé AVANT le WindowServer, qui voit donc l'accélérateur (tâche 4.2).
#   NOREBOOT=1 : ne pas redémarrer
. ./lib.sh
. ./guilib.sh
step() { sync; echo "install: $*" > /dev/console; echo "== $*"; }
step "install.sh"
( cd $SRC/guest/gldriver && sh install.sh ) > $OUT/install.txt 2>&1; echo "install.sh rc=$?"
tail -6 $OUT/install.txt
ls -d $EXT/POMPPCGPU.kext $EXT/GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC \
      $RES/GLDriver-POMPPC.bundle 2>&1
kextstat | grep -i pomppc | awk '{print "chargé :", $6, $7}'

step "second exemplaire (copie à plat laissée dans Resources)"
mkdir -p $RES/GLDriver-POMPPC.bundle
cp $EXT/GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC $RES/GLDriver-POMPPC.bundle/
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o glwin glwin.c -framework GLUT \
  -framework OpenGL || exit 1
rm -rf /tmp/trdup && mkdir -m 777 /tmp/trdup
gui_run "POMPPC_GLTRACE=/tmp/trdup ./glwin 30 320 240 glwin-dup.ppm 2 | tail -2" 240
grep -E "gldInitializeLibrary|gldTerminateLibrary|qgpu actif|déchargé" /tmp/trdup/trace.txt | head -8
rm -rf $RES/GLDriver-POMPPC.bundle /tmp/trdup

step "bureau : Desktop de l'utilisateur"
ls /Users/tiger/Desktop 2>&1 | head -20
if [ -z "$NOREBOOT" ]; then
  step "redémarrage dans 5 s"
  ( sleep 5; sync; reboot ) > /dev/null 2>&1 &
fi
