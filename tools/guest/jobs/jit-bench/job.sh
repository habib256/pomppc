#!/bin/sh
set -e
OUT=$PWD/out
export NVEC=1
gcc -O2 -faltivec -Wall -o vfptest vfptest.c > "$OUT/vfp-build.txt" 2>&1
gcc -O1 -mdynamic-no-pic -Wall -o lmwtest lmwtest.c > "$OUT/lmw-build.txt" 2>&1
gcc -O2 -Wall -o jctest jctest.c > "$OUT/jc-build.txt" 2>&1
nm vfptest lmwtest jctest > "$OUT/symbols.txt"
./vfptest 1000000 > /dev/null
./lmwtest 1000000 > /dev/null
for trial in 1 2 3; do
    ./vfptest 50000000 > "$OUT/vfp-$trial.txt" 2>&1
    ./lmwtest 20000000 > "$OUT/lmw-$trial.txt" 2>&1
    grep '^banc' "$OUT/vfp-$trial.txt" "$OUT/lmw-$trial.txt"
done
./jctest > "$OUT/jc.txt" 2>&1
cat "$OUT/jc.txt"
