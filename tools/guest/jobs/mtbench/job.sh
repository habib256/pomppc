#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# mtbench — banc de calcul pur du levier « G4 sur la cible 32 bits en MTTCG »
# (docs/ppc32-mttcg.md), VM de dev en single-user. Le pilote
# (tools/tcg/ppc32mt-bench.sh) joint regbench.c, vfptest.c, lmwtest.c et atomtest.c.
#   REPS=3 (passes) ATOMN=2000000 (incréments par fil d'atomtest)
# Lignes « mt <noyau> <ms> » : une par noyau et par passe.
OUT=$PWD/out
[ -f ./env.sh ] && . ./env.sh
REPS=${REPS:-3}; ATOMN=${ATOMN:-2000000}
t() { perl -MTime::HiRes=time -e 'printf "%.0f", time * 1000'; }
echo "ncpu $(sysctl -n hw.ncpu) ; $(uptime)"
gcc -O2 -mdynamic-no-pic -o regbench regbench.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
gcc -O2 -faltivec -o vfptest vfptest.c >> $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
gcc -O1 -mdynamic-no-pic -o lmwtest lmwtest.c >> $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
gcc -O2 -o atomtest atomtest.c >> $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
# chauffe
./regbench 1 > /dev/null 2>&1
k=0
while [ $k -lt $REPS ]; do
  k=$((k + 1))
  ./regbench 1 > $OUT/reg-$k.txt 2>&1
  sed 's/^/mt /' $OUT/reg-$k.txt
  t0=$(t); i=0
  while [ $i -lt 4 ]; do gcc -O2 -faltivec -c -o /tmp/mtb.o vfptest.c; i=$((i + 1)); done
  echo "mt gcc $(( $(t) - t0 ))"
  t0=$(t); ./vfptest 10000000 > $OUT/vfp-$k.txt 2>&1; echo "mt vfp $(( $(t) - t0 ))"
  t0=$(t); ./lmwtest 5000000 > $OUT/lmw-$k.txt 2>&1; echo "mt lmw $(( $(t) - t0 ))"
  # deux copies simultanées du noyau « call » : le débit parallèle
  t0=$(t); ./regbench 1 > /dev/null 2>&1 & ./regbench 1 > /dev/null 2>&1; wait
  echo "mt par2 $(( $(t) - t0 ))"
done
./atomtest $ATOMN 2 > $OUT/atom.txt 2>&1
cat $OUT/atom.txt
exit 0
