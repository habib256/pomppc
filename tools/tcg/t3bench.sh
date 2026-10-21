#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# t3bench.sh QEMU_BIN [REPS] — micro-banc des régions compilées (tcg/0049) :
# temps de tools/tcg/t3bench.S (boucles longues, docs/plan-traducteur-rapide.md
# §10) avec x-tier2=0 (TCG seul) et x-tier2=64,x-tier3=64, en alternance, REPS
# fois (défaut 3). Mesure le gain d'une région sur son propre code, hors coût
# d'entrée et de sortie ; la somme doit être la même.
#   KITER=… K2ITER=… pour changer la charge (défaut 20000 et 60000000).
set -u
export POMPPC_T3_SYNC="${POMPPC_T3_SYNC-1}"   # régions installées tout de suite
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QB="${1:?QEMU_BIN requis}"
REPS="${2:-3}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
powerpc-linux-gnu-as -mregnames -many --defsym KITER="${KITER:-20000}" \
    --defsym K2ITER="${K2ITER:-60000000}" -o "$OUT/b.o" "$HERE/t3bench.S" || exit 2
powerpc-linux-gnu-objcopy -O binary "$OUT/b.o" "$OUT/b.bin" || exit 2

run() {   # run NOM PROPS → « secondes somme »
    local name=$1 props=$2
    local sock="$OUT/$name.sock" t0 t1 sum=""
    t0=$(date +%s.%N)
    "$QB" -M mac99 -cpu "g4${props:+,$props}" -m 256 -smp 1 \
        -bios "$OUT/b.bin" -display none -serial none -parallel none \
        -nodefaults -audio none -monitor "unix:$sock,server=on,wait=off" \
        > "$OUT/$name.out" 2> "$OUT/$name.err" &
    local pid=$!
    for t in $(seq 1 6000); do
        sleep 0.05
        [ -S "$sock" ] || continue
        local r
        r=$(python3 "$HERE/hmp.py" "$sock" "xp /2wx 0x3000" 2>/dev/null | tail -1)
        if echo "$r" | grep -q "0xc0ffee00"; then
            t1=$(date +%s.%N)
            sum=$(echo "$r" | awk '{print $3}')
            break
        fi
        kill -0 $pid 2>/dev/null || break
    done
    python3 "$HERE/hmp.py" "$sock" "quit" > /dev/null 2>&1
    wait $pid 2>/dev/null
    echo "$(echo "${t1:-0} - $t0" | bc) ${sum:-AUCUNE}"
}

for k in $(seq 1 "$REPS"); do
    echo "tcg  $(run ref$k x-tier2=0)"
    echo "t3   $(run t3$k x-tier2=64,x-tier3=64)"
done
grep -a "x-tier3 : [0-9]\|x-tier3 : comp" "$OUT/t3$REPS.err"
