#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# gtglsl.sh <label> [VAR=valeur...] — scènes gltest GLSL (glsl glslvs glslfs glsldp
# glslsmp) jouées dans l'invité avec le plugin installé : verdicts, empreinte md5 de
# l'image, lignes VERDICT / STATE des notes (30/09, glUniform neutre). Binaire
# ~/pomppc-build/guest/gltest/gltest-w (gltest.c du dépôt, gcc-4.0). Sortie ~/gt-<label>/.
L=$1; shift; O=~/gt-$L; rm -rf $O; mkdir -p $O
cd ~/pomppc-build/guest/gltest || exit 1
B=./gltest-w
for s in glsl glslvs glslfs glsldp glslsmp; do
  rm -f gltest.ppm
  env "$@" POMPPC_GL_NOTE=$O/$s.note $B $s > $O/$s.log 2>&1; echo "rc $?" >> $O/$s.log
  [ -f gltest.ppm ] && md5 -q gltest.ppm > $O/$s.md5
done
cd $O; for f in *.log; do s=${f%.log}; n=$(grep -c '^  ok' $f); k=$(grep -c 'FAIL\|ÉCHEC\|NON TENU' $f); echo "$s: OK $n, échecs $k, $(grep '^rc ' $f) $(cat $s.md5 2>/dev/null)"; done
echo "-- échecs"; grep -h 'FAIL\|ÉCHEC\|NON TENU' *.log
echo "-- VERDICT"; grep -h 'VERDICT total\|VERDICT écart\|STATE écart\|TEXMEMO écart\|plugin 2' *.note
