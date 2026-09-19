#!/bin/sh
# Mode bureau (devloop.py start --gui) : kext chargé depuis /tmp, puis glwin
# (fenêtre GLUT, double tampon) dans la session de l'utilisateur, par le relais
# POMPPCGuiRunner. Suppose le plugin installé (job gpu).
. ./guilib.sh
SRC=$PWD/src; SDK=/Developer/SDKs/MacOSX10.4u.sdk
cd $SRC/kext/POMPPCGPU && make > /dev/null 2>&1 || { echo "kext : échec de compilation"; exit 1; }
if ! kextstat | grep -q net.pomppc.POMPPCGPU; then
  rm -rf /tmp/POMPPCGPU.kext && cp -R POMPPCGPU.kext /tmp/
  chown -R root:wheel /tmp/POMPPCGPU.kext && chmod -R 755 /tmp/POMPPCGPU.kext
  kextload -t /tmp/POMPPCGPU.kext 2>&1 | tail -1
fi
kextstat | grep -i pomppc | awk '{print "kext :", $6}'
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o glwin glwin.c -framework GLUT -framework OpenGL || exit 1
echo "== plugin"
gui_run "./glwin 120 320 240 glwin.ppm 6" 240
echo "== rendu d'Apple"
gui_run "POMPPC_GL_DISABLE=1 ./glwin 120 320 240 glwin-apple.ppm 2" 240
cp glwin.ppm glwin-apple.ppm $PWD/../../../out/ 2>/dev/null
