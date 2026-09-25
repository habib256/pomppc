#!/bin/sh
# lmwtest - equivalence lmw/stmw helper <-> en ligne (patches/tcg/0002-ppc-lmw-inline).
# Copier lmwtest.c a cote de job.sh (tools/tcg/mbab.sh ne sert pas ici) :
#   mkdir /tmp/j && cp tools/guest/jobs/lmwtest/* /tmp/j && devloop.py run /tmp/j
# -mdynamic-no-pic : le code PIC de Darwin reserve r31, que les tests doivent clobber.
# BANC=N (dans un env.sh a cote) : un second banc de N paires (pour un `sample` hote).
[ -f ./env.sh ] && . ./env.sh
# Sortie : out/lmwtest.txt ; comparer les deux modes par `diff` (hors ligne « banc »).
OUT=$PWD/out
gcc -O1 -mdynamic-no-pic -Wall -o lmwtest lmwtest.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
./lmwtest > $OUT/lmwtest.txt 2>&1
rc=$?
[ -n "$BANC" ] && ./lmwtest $BANC | grep banc
tail -3 $OUT/lmwtest.txt
grep -c "^lmw\|^stmw" $OUT/lmwtest.txt
grep "^faute" $OUT/lmwtest.txt
exit $rc
