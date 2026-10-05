#!/bin/sh
set -e
OUT=$PWD/out
export NVEC=4194304
for name in vfptest vmxtest; do
    gcc -O2 -faltivec -Wall -o "$name" "$name.c" > "$OUT/$name-build.txt" 2>&1
    ./$name > "$OUT/$name.txt" 2> "$OUT/$name.err"
    tail -2 "$OUT/$name.txt"
done
gcc -O1 -mdynamic-no-pic -Wall -o lmwtest lmwtest.c > "$OUT/lmwtest-build.txt" 2>&1
./lmwtest > "$OUT/lmwtest.txt" 2>&1
tail -3 "$OUT/lmwtest.txt"
gcc -O2 -Wall -o tbtest tbtest.c > "$OUT/tbtest-build.txt" 2>&1
./tbtest > "$OUT/tbtest.txt" 2>&1
cat "$OUT/tbtest.txt"
gcc -O2 -pthread -Wall -o smctest smctest.c > "$OUT/smctest-build.txt" 2>&1
./smctest > "$OUT/smctest.txt" 2>&1
tail -2 "$OUT/smctest.txt"
gcc -O2 -Wall -o jctest jctest.c > "$OUT/jctest-build.txt" 2>&1
./jctest 200000 > "$OUT/jctest.txt" 2>&1
tail -1 "$OUT/jctest.txt"
