#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Operates only on the explicit devloop disk and this checkout's bench state.
# Écrit sur le Mac M4 (05/10) ; portable depuis le 06/10 (PC Linux x86-64,
# docs/tcg-g4.md §30) : greffon en .so hors macOS, JIT_BENCH=0 saute les bancs
# et le profil (hôte pas au repos : les temps n'y prouvent rien).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${DEVDISK:?copie raw dédiée requise}"
: "${QEMU_BIN:?QEMU construit requis (qemu-system-ppc ; SMP>1 prend le voisin ppc64)}"
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
case "$(uname -s)" in Darwin) SO=dylib ;; *) SO=so ;; esac
JIT_BENCH="${JIT_BENCH:-1}"
if [ "$JIT_BENCH" != 0 ] && [ ! -f "$ROOT/tools/tcg/libhotblocks.$SO" ]; then
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
BASE="x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-fp-inline=on,x-ret-inline=on,x-jc-idx=on,x-icbi-sync=on,x-msr-nobql=on,x-fp-native=on,x-fp-native64=on,x-tb-fast=on,x-vmx-inline=on"
# Sonde de l'agent : `devloop start` perd parfois sa frappe (PC Linux, 06/10) ;
# l'agent ignore le job déjà présent à son démarrage, d'où une seconde sonde
# après la frappe relancée.
PROBE="$OUT/probe-job"
mkdir -p "$PROBE"
printf '#!/bin/sh\necho agent-ok\n' > "$PROBE/job.sh"
ensure_agent() {
    # Sous les vérificateurs, le démarrage est lent : `start` peut juger l'écran
    # gris d'Apple « stable » et taper dans le vide. On sonde, et tant que
    # l'agent ne répond pas, on retape (une frappe en trop dans le tty de
    # l'agent est jetée par le ^C de `shutdown`).
    local boxes k
    boxes="$(python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); print(c["IN"], c["OU"])' \
             "$ROOT/bench/devloop/mailbox.json")"
    for k in 1 2 3 4 5; do
        python3 "$DL" run "$PROBE" --timeout 150 > /dev/null 2>&1 && return 0
        echo "agent muet (essai $k) : frappe relancée"
        python3 "$DL" type '\n'; sleep 2
        python3 "$DL" type 'mount -uw /\n'; sleep 4
        python3 "$DL" type "sh /pomppc/agent.sh $boxes\n"; sleep 15
    done
    echo "agent toujours muet" >&2
    return 1
}
boot() {
    export CPU_OPTS="$BASE,$1" TCG_OPTS="x-jit-near=on,x-jc-bits=14,x-jc-word=$2"
    active=1
    python3 "$DL" start
    unset DEVFSCK
    ensure_agent
}
finish() {
    python3 "$DL" shutdown
    active=0
    cp "$ROOT/bench/devloop/qemu.log" "$OUT/$1-qemu.log"
}
echo "=== helper reference ==="
boot "x-vfp-native=off,x-lmw-vector=off" off
python3 "$DL" run "$SUITE" --timeout "${JIT_TIMEOUT:-900}"
cp -R "$ROOT/bench/devloop/last/out" "$OUT/reference"
finish reference
echo "=== verified native/vector/word ==="
boot "x-vfp-native=on,x-vfp-native-verify=on,x-vmx-verify=on,x-ret-verify=on,x-tb-verify=on,x-lmw-vector=on" on
python3 "$DL" run "$SUITE" --timeout "${JIT_TIMEOUT:-900}"
cp -R "$ROOT/bench/devloop/last/out" "$OUT/verified"
finish verified
for name in vfptest vmxtest lmwtest; do
    diff <(grep -v '^banc' "$OUT/reference/$name.txt") <(grep -v '^banc' "$OUT/verified/$name.txt")
done
if grep -Eq 'DIVERGENCE|[1-9][0-9]* divergences|vfp-native-verify: op |vmx-verify: (op |stve )' "$OUT/verified-qemu.log"; then
    echo "divergence du vérificateur"; exit 1
fi
if [ "$JIT_BENCH" = 0 ]; then
    echo "proof finished (JIT_BENCH=0, no benchmark): $OUT"
    exit 0
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
    python3 "$DL" run "$BENCH" --timeout "${JIT_TIMEOUT:-900}"
    cp -R "$ROOT/bench/devloop/last/out" "$OUT/$mode"
    finish "$mode"
done
echo "=== emitted code profile ==="
export QEMU_EXTRA="-plugin $ROOT/tools/tcg/libhotblocks.$SO,out=$OUT/hotblocks.csv -d out_asm -D $OUT/out-asm.log"
boot "x-vfp-native=on,x-lmw-vector=on" on
python3 "$DL" run "$BENCH" --timeout "${JIT_TIMEOUT:-900}"
finish profile
python3 "$ROOT/tools/tcg/jitblocks.py" "$OUT/hotblocks.csv" "$OUT/out-asm.log" > "$OUT/hotblocks.txt"
echo "proof and diagnostic benchmarks finished: $OUT"
