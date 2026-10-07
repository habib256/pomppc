#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# Mode bureau (devloop.py start --gui) : épreuve de l'état de glPixelStorei dans
# la session de l'utilisateur, par le relais POMPPCGuiRunner (le contexte où
# try_draw_pixels plantait le 07/10, docs/re/pixelstore.md). `mixte` et `v15`
# en 256×256, N fois (défaut 8), sous le plugin compilé ici, puis `pixstore`
# sous le plugin et sous Apple, images comparées (écart 0 exigé).
# Témoin « avant » facultatif : un dossier src/guest/gldriver-avant joint au job
# (plugin d'une autre révision) est joué d'abord, sans compter dans le verdict.
# Rend le nombre d'échecs.
. ./lib.sh
. ./guilib.sh
N=${N:-8}
kextstat | grep -q net.pomppc.POMPPCGPU || kext_load || exit 1
kextstat | grep -i pomppc | awk '{print "kext :", $6}'
echo "framebuffers : $(fb_count)"
gltest_build || exit 1
cp $SRC/guest/gltest/gltest . && chmod 755 gltest
CR=/Users/tiger/Library/Logs/CrashReporter/gltest.crash.log
crn() { grep -c "^Date/Time" $CR 2>/dev/null || echo 0; }
ko=0
for v in ${VARIANTS:-avant apres}; do
  d=$SRC/guest/gldriver; [ $v = avant ] && d=$SRC/guest/gldriver-avant
  [ -d $d ] || continue
  ( cd $d && make > $OUT/plugin-$v-build.txt 2>&1 ) || { tail $OUT/plugin-$v-build.txt; exit 1; }
  rm -rf $EXT/GLDriver-POMPPC.bundle && cp -R $d/GLDriver-POMPPC.bundle $EXT/
  chown -R root:wheel $EXT/GLDriver-POMPPC.bundle && chmod -R 755 $EXT/GLDriver-POMPPC.bundle
  plugin_layout
  c0=$(crn); segv=0; nok=0
  i=1
  while [ $i -le $N ]; do
    for s in mixte v15; do
      rm -f note-$v-$s-$i.txt
      gui_run "env GLTEST_REQUIRE=POMPPC POMPPC_GL_NOTE=$PWD/note-$v-$s-$i.txt ./gltest $s 256 256 o-$v-$s.ppm" 120 > o-$v-$s-$i.txt 2>&1
      rc=$?
      echo "$v $s #$i rc=$rc $(grep -c '^AVANT' note-$v-$s-$i.txt 2>/dev/null) notes AVANT, $(grep -c 'incohérent' note-$v-$s-$i.txt 2>/dev/null) incohérences"
      [ $rc = 0 ] && nok=$((nok+1))
      [ $rc = 139 ] && segv=$((segv+1))
      [ $rc = 0 ] || tail -3 o-$v-$s-$i.txt
      cp note-$v-$s-$i.txt o-$v-$s-$i.txt $OUT/ 2>/dev/null
    done
    i=$((i+1))
  done
  echo "== $v : $nok OK, $segv Segmentation fault, entrées CrashReporter +$(( $(crn) - c0 ))"
  [ $v = apres ] && [ $nok != $((2*N)) ] && ko=$((ko+1))
done
# valeurs lues par le plugin « avant » : distinctes, sur toutes les exécutions
cat $OUT/note-avant-* 2>/dev/null | grep '^AVANT' | sed 's/ctx=0x[0-9a-f]* //' | sort | uniq -c | sort -rn | head -20
echo "== pixstore (plugin courant) dans la session"
gui_run "env GLTEST_REQUIRE=POMPPC POMPPC_GL_STATS=1 POMPPC_GL_NOTE=$PWD/note-pixstore.txt ./gltest pixstore 256 256 p-pixstore.ppm" 120 > $OUT/gui-pixstore.txt 2>&1
rp=$?
gui_run "env POMPPC_GL_DISABLE=1 ./gltest pixstore 256 256 a-pixstore.ppm" 120 > $OUT/gui-pixstore-apple.txt 2>&1
ra=$?
env GLTEST_DIFF_MAX=0 ./gltest diff p-pixstore.ppm a-pixstore.ppm > $OUT/gui-pixstore-diff.txt 2>&1
rd=$?
echo "pixstore plugin rc=$rp apple rc=$ra diff rc=$rd"
grep -E "FAIL|faux|incoherent|rect ReadPixels" $OUT/gui-pixstore.txt | head
tail -2 $OUT/gui-pixstore-diff.txt
[ $rp = 0 ] || ko=$((ko+1)); [ $rd = 0 ] || ko=$((ko+1))
tail -60 $CR > $OUT/crash-tail.txt 2>/dev/null
echo "VERDICT : $ko échec(s)"
exit $ko
