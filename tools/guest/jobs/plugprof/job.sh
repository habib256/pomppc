#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# plugprof — profil d'INSTRUCTIONS du plugin sur une scène gltest (21/10/2026,
# docs/d3-plugin-x86.md §10). À jouer sous le greffon TCG hotblocks
# (QEMU_EXTRA="-plugin tools/tcg/libhotblocks.so,out=…,snap=…") : l'hôte prend
# un instantané avant et après `devloop.py run`, tools/re/hotplug.py nomme les
# blocs du plugin et fait la différence.
#
#   BUILD=1 : kext chargé, plugin compilé et installé, gltest compilé (copié en
#             /tmp/gltest-prof), table des symboles du plugin (out/nm.txt) ;
#             puis la scène comme ci-dessous (note gardée : adresse de chargement).
#   BUILD=0 (défaut) : seulement la scène, REPS fois, dans le mode MODE —
#             rien d'autre ne touche au plugin pendant la mesure.
#   SCENE=d3inter REPS=1 MODE="POMPPC_GL_X=1,POMPPC_GL_Y=1" SIZE="256 256"
set -e
. ./lib.sh
[ -f ./env.sh ] && . ./env.sh
SCENE=${SCENE:-d3inter}; REPS=${REPS:-1}; SIZE=${SIZE:-"256 256"}
vars=$(echo "${MODE:-}" | tr ',' ' ')
if [ "${BUILD:-0}" = 1 ]; then
  echo "== kext"; kext_load
  echo "== plugin"; plugin_install
  gltest_build
  cp $SRC/guest/gltest/gltest /tmp/gltest-prof
  nm -n $EXT/GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC > $OUT/nm.txt
  ( cd $EXT && tar cf $OUT/GLDriver-POMPPC.bundle.tar GLDriver-POMPPC.bundle ) || true
fi
cd /tmp
k=0
while [ $k -lt $REPS ]; do
  k=$((k + 1))
  rm -f /tmp/prof.ppm
  # shellcheck disable=SC2086
  if env GLTEST_NOWS=1 GLTEST_REQUIRE=POMPPC $vars POMPPC_GL_NOTE=$OUT/note-$k.txt \
       /tmp/gltest-prof $SCENE $SIZE /tmp/prof.ppm > $OUT/out-$k.txt 2>&1
  then rc=0; else rc=$?; fi
  echo "passe $k rc=$rc md5 $(md5 -q /tmp/prof.ppm 2>/dev/null || echo absent)"
done
exit 0
