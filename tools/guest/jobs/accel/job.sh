#!/bin/sh
# Tâche 4.2 — l'accélérateur IOKit publié par le kext (docs/re/accelerateur-iokit.md).
#
# Deux modes, selon qu'il y a un framebuffer :
#
#   single-user (devloop.py start) : pas de framebuffer (IONDRVSupport n'est
#     chargé que par kextd). On vérifie le nub POMPPCAccelerator, l'absence
#     d'IOAGPDevice, et le déchargement / rechargement propre du kext.
#   bureau (devloop.py start --gui) : en plus, le framebuffer désigne
#     l'accélérateur, le plugin n'est QUE dans Extensions, et accelprobe, gltest,
#     glwin et fullscreen, lancés dans la session, rendent par POMPPC : GLEngine
#     l'a donc chargé par IOGLBundleName. Quartz Extreme doit rester inactif.
#
# Rend le nombre d'échecs (bug hunt T6). Deux choses ont changé avec la chaîne
# de verdict : gltest exige GL_RENDERER = POMPPC (T5, code 6) — c'est ici que
# l'assertion compte le plus, puisque tout l'objet du job est de prouver que
# GLEngine charge NOTRE plugin — et gui_run rend le rc de la commande lancée
# dans la session (T4), qu'il ne faut donc plus perdre dans un tube.
# fullscreen.c, que personne ne lançait (T20), tourne dans le mode bureau.
#
#   SCENES="tri tex" : scènes gltest jouées dans la session (mode bureau)
#   NOLOAD=1 : garder le kext et le plugin INSTALLÉS (job install, puis
#              redémarrage) — c'est alors l'état vu par le WindowServer au boot
#   NOFULLSCREEN=1 : sauter fullscreen ; FSRES="1024 768" : sa résolution
set -e
. ./lib.sh
. ./guilib.sh
SCENES=${SCENES:-"tri tex lit caps"}
FB=$(fb_count)
ko=0
fail() { ko=$((ko + 1)); echo "  ÉCHEC : $*"; }
# repère sur la console (et disque synchronisé) : si une étape bloque, la
# capture d'écran de devloop dit laquelle, et arrêter la VM n'abîme rien
step() { sync; echo "accel: $*" > /dev/console; echo "== $*"; }
# gui_run, sans perdre son code de sortie dans un tube (T4).
run_gui() {    # run_gui étiquette "commande" délai
  if gui_run "$2" "$3" > $OUT/gui-$1.txt 2>&1; then _rc=0; else _rc=$?; fi
  sed "s/^/$1 /" $OUT/gui-$1.txt
  echo "$1 : rc=$_rc (124 = timeout du relais, 125 = relais muet)"
  return $_rc
}
# (ioreg -l échoue en entier sur cette VM, « can't obtain properties » : on
# interroge classe par classe)
acc_ioreg() {
  for c in POMPPCAccelerator IONDRVFramebuffer IOFramebuffer; do
    ioreg -c $c -w 0 2>/dev/null
  done | grep -E '"IOGLBundleName"|"IOAccelTypes"|"IOAccelIndex"' | sed 's/^[ |]*//' | sort -u || true
  echo "classes : $(ioreg -c POMPPCAccelerator 2>/dev/null | grep -c 'class POMPPCAccelerator')" \
       "POMPPCAccelerator, $(ioreg -c IOAGPDevice 2>/dev/null | grep -c 'class IOAGPDevice') IOAGPDevice"
}

step "kext (framebuffers : ${FB:-?})"
[ -n "$NOLOAD" ] || kext_load || exit 1
kextstat | grep -i pomppc | awk '{print "chargé :", $6, $7}' || true
dmesg | grep POMPPCGPU | tail -3 || true
acc_ioreg

step "plugin"
[ -n "$NOLOAD" ] || plugin_install || exit 1
ls -d $EXT/GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC $RES/GLDriver-POMPPC.bundle 2>&1 || true

step "accelprobe"
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o accelprobe accelprobe.c -framework IOKit \
  -framework CoreFoundation -framework OpenGL -framework ApplicationServices \
  > $OUT/accelprobe-build.txt 2>&1 || { tail -15 $OUT/accelprobe-build.txt; exit 1; }
gltest_build || exit 1
if [ "$FB" = 0 ]; then
  if GLTEST_NOWS=1 ./accelprobe; then rc=0; else rc=$?; fi
  echo "accelprobe rc=$rc"
  [ $rc = 0 ] || fail "accelprobe rc=$rc"
  step "gltest (single-user : plugin chargé depuis la copie de Resources)"
  for s in $SCENES; do
    step "gltest $s"
    if env GLTEST_NOWS=1 GLTEST_REQUIRE=POMPPC ./gltest $s 128 128 p-$s.ppm > $OUT/$s.txt 2>&1
    then rc=0; else rc=$?; fi
    printf "%-6s rc=%d [%s]\n" $s $rc "$(grep -m1 GL_RENDERER $OUT/$s.txt | sed 's/GL_RENDERER = //')"
    [ $rc = 0 ] || fail "gltest $s rc=$rc (1 témoin faux, 5 scène inconnue, 6 pas POMPPC)"
  done
else
  # le `exit $r` final compte : le relais rend le rc de la DERNIÈRE commande,
  # et un `echo` de plus effaçait le verdict d'accelprobe.
  run_gui accelprobe "./accelprobe; r=\$?; echo accelprobe rc=\$r; exit \$r" 120 ||
    fail "accelprobe dans la session"
  step "gltest dans la session (plugin chargé par IOGLBundleName seulement)"
  for s in $SCENES; do
    if run_gui "gltest-$s" \
         "GLTEST_REQUIRE=POMPPC ./gltest $s 128 128 p-$s.ppm > $s.txt 2>&1" 120
    then rc=0; else rc=$?; fi
    grep -m1 GL_RENDERER $s.txt 2>/dev/null | sed "s/^/$s /" || true
    cp $s.txt $OUT/ 2>/dev/null || true
    [ $rc = 0 ] || fail "gltest $s dans la session rc=$rc (6 = GL_RENDERER pas POMPPC)"
  done
  /usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o glwin glwin.c -framework GLUT \
    -framework OpenGL || exit 1
  step "glwin"
  run_gui glwin "./glwin 60 320 240 glwin.ppm 3" 240 || fail "glwin dans la session"
  cp glwin.ppm $OUT/ 2>/dev/null || true
  # T20 : fullscreen.c (bascule plein écran, scanout relu à la main) n'était
  # lancé par AUCUN job. Il lui faut une vraie session graphique : c'est ici.
  if [ -z "$NOFULLSCREEN" ]; then
    step "fullscreen"
    /usr/bin/gcc-4.0 -arch ppc -isysroot $SDK -o fullscreen fullscreen.c \
      -framework Carbon -framework OpenGL > $OUT/fullscreen-build.txt 2>&1 ||
      { tail -15 $OUT/fullscreen-build.txt; fail "fullscreen : compilation"; }
    if [ -x ./fullscreen ]; then
      run_gui fullscreen "./fullscreen ${FSRES:-1024 768}" 240 ||
        fail "fullscreen (bascule plein écran / scanout)"
    fi
  fi
fi

if [ -n "$NOLOAD" ]; then
  echo "VERDICT : $ko échec(s)"
  exit $ko
fi
step "déchargement du kext"
kextunload -b net.pomppc.POMPPCGPU > $OUT/kextunload.txt 2>&1 ||
  fail "kextunload : $(tail -1 $OUT/kextunload.txt)"
tail -1 $OUT/kextunload.txt
echo "instances du kext : $(kextstat | grep -c net.pomppc.POMPPCGPU)"
acc_ioreg
step "rechargement"
kextload -t /tmp/POMPPCGPU.kext > $OUT/kextload2.txt 2>&1 ||
  fail "rechargement du kext : $(tail -1 $OUT/kextload2.txt)"
tail -1 $OUT/kextload2.txt
acc_ioreg
if [ "$FB" = 0 ]; then
  if env GLTEST_NOWS=1 GLTEST_REQUIRE=POMPPC ./gltest tri 64 64 r-tri.ppm > $OUT/retri.txt 2>&1
  then rc=0; else rc=$?; fi
  grep -m1 GL_RENDERER $OUT/retri.txt || true
  [ $rc = 0 ] || fail "gltest tri après rechargement rc=$rc"
else
  run_gui retri "GLTEST_REQUIRE=POMPPC ./gltest tri 64 64 r-tri.ppm" 120 ||
    fail "gltest tri après rechargement, dans la session"
fi
[ $ko -lt 250 ] || ko=250
echo "VERDICT : $ko échec(s)"
exit $ko
