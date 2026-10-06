#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# dcbztest - dcbz et lmw/stmw courts : helpers d'origine <-> traduction en
# ligne (patches/tcg/0032 x-lmw-inline, 0033 x-dcbz-inline, docs/tcg-g4.md
# section 32). Empreinte FNV identique dans tous les modes, puis les bancs.
#   tools/guest/jobs/stage.sh dcbztest /tmp/j && cp tools/guest/jobs/dcbztest/dcbztest.c /tmp/j
#   devloop.py run /tmp/j
# env.sh a cote (facultatif) : BANC=N (bancs seuls, trois fois).
[ -f ./env.sh ] && . ./env.sh
OUT=$PWD/out
gcc -O2 -mdynamic-no-pic -Wall -o dcbztest dcbztest.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
if [ -n "$BANC" ]; then
  ./dcbztest banc $BANC > $OUT/dcbztest.txt 2>&1
  ./dcbztest banc $BANC >> $OUT/dcbztest.txt 2>&1
  ./dcbztest banc $BANC >> $OUT/dcbztest.txt 2>&1
  rc=$?
else
  ./dcbztest > $OUT/dcbztest.txt 2>&1
  rc=$?
fi
grep -c . $OUT/dcbztest.txt
grep 'empreinte\|banc\|PAS DE FAUTE\|faute' $OUT/dcbztest.txt | sort | uniq -c | sort -rn | head -40
exit $rc
