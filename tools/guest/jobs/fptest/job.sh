#!/bin/sh
# fptest - equivalence du flottant scalaire simple precision : helpers d'origine
# <-> chemin court de x-fp-inline (patches/tcg/0007-ppc-fp-inline, docs/tcg-g4.md
# section 15).
#   mkdir /tmp/j && cp tools/guest/jobs/fptest/* /tmp/j && devloop.py run /tmp/j
# env.sh a cote (facultatif) : NRAND=n vecteurs aleatoires par (etat, op)
# (defaut 2^18), BANC=N (bancs de N iterations au lieu de l'equivalence).
# Sortie : out/fptest.txt ; comparer les deux modes par `diff`.
[ -f ./env.sh ] && . ./env.sh
OUT=$PWD/out
gcc -O2 -Wall -o fptest fptest.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
if [ -n "$BANC" ]; then
  ./fptest banc $BANC > $OUT/fptest.txt 2> $OUT/fptest.err
  ./fptest banc $BANC >> $OUT/fptest.txt 2>> $OUT/fptest.err
  ./fptest banc $BANC >> $OUT/fptest.txt 2>> $OUT/fptest.err
else
  ./fptest $NRAND > $OUT/fptest.txt 2> $OUT/fptest.err
fi
rc=$?
cat $OUT/fptest.err $OUT/fptest.txt
exit $rc
