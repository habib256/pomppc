#!/bin/sh
# Tâche 4.2 — l'accélérateur IOKit publié par le kext (docs/re/accelerateur-iokit.md).
#
# Deux modes, selon qu'il y a un framebuffer :
#
#   single-user (devloop.py start) : pas de framebuffer (IONDRVSupport n'est
#     chargé que par kextd). On vérifie le nub POMPPCAccelerator, l'absence
#     d'IOAGPDevice, et le déchargement / rechargement propre du kext.
#   bureau (devloop.py start --gui) : en plus, le framebuffer désigne
#     l'accélérateur, le plugin n'est QUE dans Extensions, et accelprobe, gltest
#     et glwin, lancés dans la session, rendent par POMPPC : GLEngine l'a donc
#     chargé par IOGLBundleName. Quartz Extreme doit rester inactif.
#
#   SCENES="tri tex" : scènes gltest jouées dans la session (mode bureau)
#   NOLOAD=1 : garder le kext et le plugin INSTALLÉS (job install, puis
#              redémarrage) — c'est alors l'état vu par le WindowServer au boot
. ./lib.sh
. ./guilib.sh
SCENES=${SCENES:-"tri tex lit caps"}
FB=$(fb_count)
# repère sur la console (et disque synchronisé) : si une étape bloque, la
# capture d'écran de devloop dit laquelle, et arrêter la VM n'abîme rien
step() { sync; echo "accel: $*" > /dev/console; echo "== $*"; }
# (ioreg -l échoue en entier sur cette VM, « can't obtain properties » : on
# interroge classe par classe)
acc_ioreg() {
  for c in POMPPCAccelerator IONDRVFramebuffer IOFramebuffer; do
    ioreg -c $c -w 0 2>/dev/null
  done | grep -E '"IOGLBundleName"|"IOAccelTypes"|"IOAccelIndex"' | sed 's/^[ |]*//' | sort -u
  echo "classes : $(ioreg -c POMPPCAccelerator 2>/dev/null | grep -c 'class POMPPCAccelerator')" \
       "POMPPCAccelerator, $(ioreg -c IOAGPDevice 2>/dev/null | grep -c 'class IOAGPDevice') IOAGPDevice"
}

step "kext (framebuffers : ${FB:-?})"
[ -n "$NOLOAD" ] || kext_load || exit 1
kextstat | grep -i pomppc | awk '{print "chargé :", $6, $7}'
dmesg | grep POMPPCGPU | tail -3
acc_ioreg

step "plugin"
[ -n "$NOLOAD" ] || plugin_install || exit 1
ls -d $EXT/GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC $RES/GLDriver-POMPPC.bundle 2>&1

step "accelprobe"
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o accelprobe accelprobe.c -framework IOKit \
  -framework CoreFoundation -framework OpenGL -framework ApplicationServices \
  > $OUT/accelprobe-build.txt 2>&1 || { tail -15 $OUT/accelprobe-build.txt; exit 1; }
gltest_build || exit 1
if [ "$FB" = 0 ]; then
  GLTEST_NOWS=1 ./accelprobe; echo "accelprobe rc=$?"
  step "gltest (single-user : plugin chargé depuis la copie de Resources)"
  for s in $SCENES; do
    step "gltest $s"
    env GLTEST_NOWS=1 ./gltest $s 128 128 p-$s.ppm > $OUT/$s.txt 2>&1; rc=$?
    printf "%-6s rc=%d [%s]\n" $s $rc "$(grep -m1 GL_RENDERER $OUT/$s.txt | sed 's/GL_RENDERER = //')"
  done
else
  gui_run "./accelprobe; echo accelprobe rc=\$?" 120
  step "gltest dans la session (plugin chargé par IOGLBundleName seulement)"
  for s in $SCENES; do
    gui_run "./gltest $s 128 128 p-$s.ppm > $s.txt 2>&1; echo rc=\$?" 120 |
      sed "s/^/$s /"
    grep -m1 GL_RENDERER $s.txt | sed "s/^/$s /"
    cp $s.txt $OUT/ 2>/dev/null
  done
  /usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o glwin glwin.c -framework GLUT \
    -framework OpenGL || exit 1
  step "glwin"
  gui_run "./glwin 60 320 240 glwin.ppm 3" 240
fi

[ -n "$NOLOAD" ] && exit 0
step "déchargement du kext"
kextunload -b net.pomppc.POMPPCGPU 2>&1 | tail -1
echo "instances du kext : $(kextstat | grep -c net.pomppc.POMPPCGPU)"
acc_ioreg
step "rechargement"
kextload -t /tmp/POMPPCGPU.kext 2>&1 | tail -1
acc_ioreg
if [ "$FB" = 0 ]; then
  env GLTEST_NOWS=1 ./gltest tri 64 64 r-tri.ppm 2>&1 | grep -m1 GL_RENDERER
else
  gui_run "./gltest tri 64 64 r-tri.ppm 2>&1 | grep -m1 GL_RENDERER" 120
fi
