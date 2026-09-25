#!/bin/sh
# vfptest - equivalence vaddfp/vsubfp/vmaddfp/vnmsubfp : helpers d'origine <->
# chemin rapide a 4 voies (patches/tcg/0003-ppc-vfp-fast, docs/tcg-g4.md section 9).
#   mkdir /tmp/j && cp tools/guest/jobs/vfptest/* /tmp/j && devloop.py run /tmp/j
# env.sh a cote (facultatif) : NVEC=n vecteurs aleatoires par valeur de NJ
# (defaut 2^22), BANC=N (banc de N x 4 instructions dependantes).
# Sortie : out/vfptest.txt ; comparer les deux modes par `diff` (hors ligne « banc »).
[ -f ./env.sh ] && . ./env.sh
export NVEC
OUT=$PWD/out
gcc -O2 -faltivec -Wall -o vfptest vfptest.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
./vfptest $BANC > $OUT/vfptest.txt 2> $OUT/vfptest.err
rc=$?
cat $OUT/vfptest.err $OUT/vfptest.txt
exit $rc
