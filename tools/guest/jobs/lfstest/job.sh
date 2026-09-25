#!/bin/sh
# lfstest - equivalence lfs/stfs helper <-> en ligne (patches/tcg/0002-ppc-lfs-inline).
#   mkdir /tmp/j && cp tools/guest/jobs/lfstest/* /tmp/j && devloop.py run /tmp/j
# env.sh a cote (facultatif) : LFSBITS=n (defaut 32 : les 2^32 motifs, quelques
# minutes par mode en SMP=2), BANC=N (banc de N paires lfs/stfs).
# Sortie : out/lfstest.txt ; comparer les deux modes par `diff` (hors ligne « banc »).
[ -f ./env.sh ] && . ./env.sh
export LFSBITS
OUT=$PWD/out
gcc -O2 -mdynamic-no-pic -Wall -o lfstest lfstest.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
./lfstest $BANC > $OUT/lfstest.txt 2> $OUT/lfstest.err
rc=$?
cat $OUT/lfstest.err
grep -v "^lfs  [0-9a-f]" $OUT/lfstest.txt
exit $rc
