#!/bin/sh
# smctest - code modifie / recopie / remappe dans l'invite : preuve de
# l'invalidation pour les sorties indirectes de TCG (patches/tcg/0008,
# x-ret-inline, x-jc-idx ; docs/tcg-g4.md section 16).
#   mkdir /tmp/j && cp tools/guest/jobs/smctest/* /tmp/j && devloop.py run /tmp/j
# env.sh a cote (facultatif) : N=echelle (defaut 1), BANC=N (banc d'appels).
# Sortie : out/smctest.txt ; comparer les modes par `diff`, erreurs = 0.
[ -f ./env.sh ] && . ./env.sh
OUT=$PWD/out
gcc -O2 -Wall -o smctest smctest.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
if [ -n "$BANC" ]; then
  ./smctest banc $BANC > $OUT/smctest.txt 2>&1
  ./smctest banc $BANC >> $OUT/smctest.txt 2>&1
  ./smctest banc $BANC >> $OUT/smctest.txt 2>&1
  rc=$?
else
  ./smctest ${N:-1} > $OUT/smctest.txt 2>&1
  rc=$?
fi
cat $OUT/smctest.txt
exit $rc
