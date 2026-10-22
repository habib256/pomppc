#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# swapwin — l'ÉCHANGE dans la session graphique (devloop.py start --gui), sous
# plusieurs modes du plugin (22/10/2026, levier B : docs/plugin-allegement.md §3).
# swapbench (guest/gltest/swapbench.c) : fenêtre (fen), fenêtre déplacée
# (move), fenêtres qui se recouvrent (cover), plein écran CGL (fs). Chaque
# passe sous POMPPC_GL_NOTE et POMPPC_GL_FRAMES (l'usage réel : la trace est
# allumée par les lanceurs « trace » et la matrice).
#
#   MODES="ref: win:POMPPC_GL_SWAPWIN=1"  (« nom:VAR=v,VAR=v »)
#   TESTS="fen move cover fs" ; N=600 ; SIZE="320 240" ; FSSIZE="800 600" ; REPS=1
#   WORK=k : travail simulé par image (swapbench, SWB_WORK) ; TEXDEL=1 : une texture
#   détruite par image (SWB_TEXDEL) ; QUIET=1 : sans la ligne ECHANGE
#   BUILD=0 : garder le kext et le plugin déjà en place
#
# Rend le nombre d'échecs (relevés de swapbench, renderer qui n'est pas POMPPC).
set -e
. ./lib.sh
. ./guilib.sh
[ -f ./env.sh ] && . ./env.sh
MODES=${MODES:-"ref:"}
TESTS=${TESTS:-"fen move cover fs"}
N=${N:-600}; SIZE=${SIZE:-"320 240"}; FSSIZE=${FSSIZE:-"800 600"}; REPS=${REPS:-1}
ko=0
if [ "${BUILD:-1}" = 1 ]; then
  echo "== kext"; kext_load
  echo "== plugin"; plugin_install
  ( cd $EXT && tar cf $OUT/GLDriver-POMPPC.bundle.tar GLDriver-POMPPC.bundle ) || true
  nm -n $EXT/GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC > $OUT/nm.txt || true
fi
cd $SRC/guest/gltest
# PREBUILT=1 : swapbench déjà compilé (/tmp/swapbench-prof) — sous le greffon
# hotblocks, rien d'autre que la passe ne doit tourner entre deux instantanés
if [ "${PREBUILT:-0}" = 1 ] && [ -x /tmp/swapbench-prof ]; then
  cp /tmp/swapbench-prof swapbench
else
  /usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o swapbench swapbench.c \
    -framework GLUT -framework OpenGL -framework Carbon > $OUT/swapbench-build.txt 2>&1 ||
    { tail -15 $OUT/swapbench-build.txt; exit 1; }
  cp swapbench /tmp/swapbench-prof
fi
chmod 777 $OUT   # les passes tournent sous l’utilisateur de la session
r=0
while [ $r -lt $REPS ]; do
  r=$((r + 1))
  for t in $TESTS; do
    for m in $MODES; do
      nom=${m%%:*}; vars=$(echo "${m#*:}" | tr ',' ' ')
      sz=$SIZE; [ $t = fs ] && sz=$FSSIZE
      tag=$t-$nom-$r
      rm -f $OUT/note-$tag.txt $OUT/frames-$tag.csv
      if gui_run "env SWB_WORK=${WORK:-0} SWB_TEXDEL=${TEXDEL:-0} $vars POMPPC_GL_NOTE=$OUT/note-$tag.txt POMPPC_GL_FRAMES=$OUT/frames-$tag.csv ./swapbench $t $N $sz" 600 \
           > $OUT/out-$tag.txt 2>&1; then rc=0; else rc=$?; fi
      ms=$(sed -n 's/.*: \([0-9.]*\) ms\/image$/\1/p' $OUT/out-$tag.txt | tail -1)
      rend=$(grep -c 'GL_RENDERER = POMPPC' $OUT/out-$tag.txt || true)
      echo "$tag rc=$rc ${ms:-?} ms/image renderer-pomppc=$rend $(grep -h 'relevés\|centre' $OUT/out-$tag.txt | tr '\n' ' ')"
      [ $rc = 0 ] || ko=$((ko + 1))
      [ "$rend" != 0 ] || { ko=$((ko + 1)); echo "  PAS-POMPPC"; }
      [ -n "$QUIET" ] || grep -h 'ECHANGE image' $OUT/note-$tag.txt 2>/dev/null | tail -1 | cut -c1-700 || true
    done
  done
done
grep -h 'présentation directe' /var/log/system.log 2>/dev/null | tail -20 > $OUT/syslog-direct.txt || true
[ $ko -lt 250 ] || ko=250
echo "VERDICT : $ko échec(s)"
exit $ko
