#!/bin/sh
# GPL3 - Copyleft VERHILLE Arnaud
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
# DIS=1 : le code de banc() tel que gcc l'a compilé (quelles instructions)
if [ -n "$DIS" ]; then
  # banc() est statique et appelée une fois : gcc la met en ligne dans main
  gcc -O2 -Wall -fno-inline -S -o $OUT/fptest.s fptest.c
  sed -n '/^_banc:/,/^_[a-z_]*:$/p' $OUT/fptest.s > $OUT/banc-dis.txt
  echo "banc() : $(grep -c . $OUT/banc-dis.txt) lignes ;" \
       "fcmpu $(grep -c 'fcmpu' $OUT/banc-dis.txt), fcmpo $(grep -c 'fcmpo' $OUT/banc-dis.txt)," \
       "fsel $(grep -c 'fsel' $OUT/banc-dis.txt), fmadds $(grep -c 'fmadds' $OUT/banc-dis.txt)"
fi
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
