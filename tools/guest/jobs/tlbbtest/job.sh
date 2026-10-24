#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# tlbbtest - chargements en forme D sur une meme base, vus de l'invite : preuve de
# x-tlb-base (patches/tcg/0052, docs/tcg-g4.md section 41). Couverture a tous les bords
# de page, fautes a l'instruction pres, base ecrite, six bases a la fois ; erreurs (ecarts
# a l'attendu calcule en C) et empreinte FNV, identiques avec et sans la propriete.
#   tools/guest/jobs/stage.sh tlbbtest /tmp/j && cp tools/guest/jobs/tlbbtest/tlbbtest.c /tmp/j
#   devloop.py run /tmp/j
# env.sh a cote (facultatif) : N=tours du mode 3 (defaut 20000), BANC=N (banc seul).
[ -f ./env.sh ] && . ./env.sh
OUT=$PWD/out
gcc -O2 -Wall -o tlbbtest tlbbtest.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
if [ -n "$BANC" ]; then
  ./tlbbtest banc $BANC > $OUT/tlbbtest.txt 2>&1
  ./tlbbtest banc $BANC >> $OUT/tlbbtest.txt 2>&1
  ./tlbbtest banc $BANC >> $OUT/tlbbtest.txt 2>&1
  rc=$?
else
  ./tlbbtest ${N:-20000} > $OUT/tlbbtest.txt 2>&1
  rc=$?
fi
cat $OUT/tlbbtest.txt
exit $rc
