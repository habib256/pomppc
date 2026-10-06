#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# tlbtest - invalidations du TLB vues de l'invite : preuve de x-tlb-precise
# (patches/tcg/0031, docs/tcg-g4.md section 32). Protections et fautes,
# munmap/mmap, fork et copie sur ecriture, tubes, fichier ; empreinte FNV
# identique avec et sans la propriete. Puis les bancs (hors empreinte).
#   tools/guest/jobs/stage.sh tlbtest /tmp/j && cp tools/guest/jobs/tlbtest/tlbtest.c /tmp/j
#   devloop.py run /tmp/j
# env.sh a cote (facultatif) : N=echelle (defaut 1), BANC=N (bancs seuls, x N).
[ -f ./env.sh ] && . ./env.sh
OUT=$PWD/out
gcc -O2 -Wall -o tlbtest tlbtest.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
if [ -n "$BANC" ]; then
  ./tlbtest banc $BANC > $OUT/tlbtest.txt 2>&1
  ./tlbtest banc $BANC >> $OUT/tlbtest.txt 2>&1
  ./tlbtest banc $BANC >> $OUT/tlbtest.txt 2>&1
  rc=$?
else
  ./tlbtest ${N:-1} > $OUT/tlbtest.txt 2>&1
  rc=$?
fi
grep -v '^[ABCDE] ' $OUT/tlbtest.txt
exit $rc
