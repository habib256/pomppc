#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Operates only on the explicit devloop disk and this checkout's bench state.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${DEVDISK:?copie raw dédiée requise}"
: "${QEMU_BIN:?QEMU arm64 construit requis}"
export DEVDISK QEMU_BIN
export SMP="${SMP:-1}"
OUT="${JIT_OUT:-$ROOT/bench/jit-m4/$(date +%Y%m%d-%H%M%S)}"
[ ! -e "$OUT" ] || { echo "Dossier de résultats déjà présent : $OUT" >&2; exit 1; }
mkdir -p "$OUT"
DL="$ROOT/tools/guest/devloop.py"
# Refuse to take over a VM already managed by this checkout.
if [ -f "$ROOT/bench/devloop/qemu.pid" ] && kill -0 "$(cat "$ROOT/bench/devloop/qemu.pid")" 2>/dev/null; then
    echo "VM déjà active dans ce checkout ; utiliser une copie isolée." >&2
    exit 1
fi
SUITE="$OUT/suite-job"
BENCH="$OUT/bench-job"
mkdir -p "$SUITE" "$BENCH"
cp "$ROOT/tools/guest/jobs/jit-suite/job.sh" "$SUITE/"
cp "$ROOT/tools/guest/jobs/jit-bench/job.sh" "$BENCH/"
for name in vfptest vmxtest lmwtest tbtest smctest jctest; do
    cp "$ROOT/tools/guest/jobs/$name/$name.c" "$SUITE/"
done
for name in vfptest lmwtest jctest; do
    cp "$ROOT/tools/guest/jobs/$name/$name.c" "$BENCH/"
done
if [ ! -f "$ROOT/tools/tcg/libhotblocks.dylib" ]; then
    QEMU_SRC="${QEMU_SRC:-$(cd "$(dirname "$QEMU_BIN")/.." && pwd)}" \
        bash "$ROOT/tools/tcg/build.sh" hotblocks
fi
active=0
cleanup() {
    if [ "$active" = 1 ]; then
        python3 "$DL" shutdown > "$OUT/shutdown.log" 2>&1 || true
    fi
}
trap cleanup EXIT
BASE="x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-ret-inline=on,x-jc-idx=on,x-icbi-sync=on,x-msr-nobql=on,x-fp-native=on,x-fp-native64=on,x-tb-fast=on,x-vmx-inline=on"
boot() {
    export CPU_OPTS="$BASE,$1" TCG_OPTS="x-jit-near=on,x-jc-bits=14,x-jc-word=$2"
    active=1
    python3 "$DL" start
    unset DEVFSCK
}
finish() {
    python3 "$DL" shutdown
    active=0
    cp "$ROOT/bench/devloop/qemu.log" "$OUT/$1-qemu.log"
}
echo "=== helper reference ==="
boot "x-vfp-native=off,x-lmw-vector=off" off
python3 "$DL" run "$SUITE" --timeout 900
cp -R "$ROOT/bench/devloop/last/out" "$OUT/reference"
finish reference
echo "=== verified native/vector/word ==="
boot "x-vfp-native=on,x-vfp-native-verify=on,x-vmx-verify=on,x-ret-verify=on,x-tb-verify=on,x-lmw-vector=on" on
python3 "$DL" run "$SUITE" --timeout 900
cp -R "$ROOT/bench/devloop/last/out" "$OUT/verified"
finish verified
for name in vfptest vmxtest lmwtest; do
    diff <(grep -v '^banc' "$OUT/reference/$name.txt") <(grep -v '^banc' "$OUT/verified/$name.txt")
done
if grep -Eq 'DIVERGENCE|[1-9][0-9]* divergences|vfp-native-verify: op |vmx-verify: (op |stve )' "$OUT/verified-qemu.log"; then
    echo "divergence du vérificateur"; exit 1
fi
for mode in base native vector word all; do
    native=off; vector=off; word=off
    case "$mode" in
        native) native=on ;;
        vector) vector=on ;;
        word) word=on ;;
        all) native=on; vector=on; word=on ;;
    esac
    echo "=== benchmark $mode ==="
    boot "x-vfp-native=$native,x-lmw-vector=$vector" "$word"
    python3 "$DL" run "$BENCH" --timeout 900
    cp -R "$ROOT/bench/devloop/last/out" "$OUT/$mode"
    finish "$mode"
done
echo "=== emitted code profile ==="
export QEMU_EXTRA="-plugin $ROOT/tools/tcg/libhotblocks.dylib,out=$OUT/hotblocks.csv -d out_asm -D $OUT/out-asm.log"
boot "x-vfp-native=on,x-lmw-vector=on" on
python3 "$DL" run "$BENCH" --timeout 900
finish profile
python3 "$ROOT/tools/tcg/jitblocks.py" "$OUT/hotblocks.csv" "$OUT/out-asm.log" > "$OUT/hotblocks.txt"
echo "proof and diagnostic benchmarks finished: $OUT"
