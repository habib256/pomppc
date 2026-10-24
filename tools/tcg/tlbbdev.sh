#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# tlbbdev.sh ÉTIQUETTE [--gui] PROPS JOB[:VAR=v,VAR=v…] … — une suite de jobs devloop sur
# une VM de DÉVELOPPEMENT (jamais la VM quotidienne), PC Linux, SMP=2 en MTTCG sur
# qemu-system-ppc (PPC32MT=1), toutes les options de production de run_tiger.sh (Linux
# x86-64, 24/10, x-lfs-known compris) plus PROPS (p. ex. x-tlb-base=on,x-tlb-base-verify=on ; « - » pour rien).
# patches/tcg/0052, docs/tcg-g4.md §41.
#
#   DEVDISK=~/src/pomppc/disks/tiger-dev-tlbb.raw QEMU_BIN=~/src/qemu-tlbb/build/qemu-system-ppc \
#       tools/tcg/tlbbdev.sh ref - fptest:MODE=k,NRAND=4096 fptest:BANC=10000000
#   tools/tcg/tlbbdev.sh ver --gui x-tlb-base=on,x-tlb-base-verify=on fpgames:GAMES=mb,DUR=120
#   tools/tcg/tlbbdev.sh v1 x-tlb-base=on,x-tlb-base-verify=on tlbbtest tlbtest suite
#
# Job « suite » : vfptest vmxtest lmwtest tbtest smctest jctest puis atomtest (comme
# tools/tcg/ppc32mt-bench.sh). Une seconde VM de dev en même temps : une copie du dépôt sous un
# chemin court (les sockets AF_UNIX de devloop), DEVDISK et SSH_PORT à elle.
#
# Variables : DEVDISK (défaut disks/tiger-dev-tlbb.raw du dépôt principal), QEMU_BIN (défaut
# ~/src/qemu-tlbb/build/qemu-system-ppc), SSH_PORT (défaut 2272), TIMEOUT (par job, 2400 s).
# Sorties : bench/tcg/tlbb/<ÉTIQUETTE>/ du dépôt principal : <n>-<job>/ (out/ du job),
# job-<n>.log, qemu.log (stderr de QEMU : bilans des vérificateurs), run.log.
set -u
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
R="$(git -C "$WT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's|/\.git$||')"
R="${R:-$WT}"
cd "$WT"
TAG=${1:?étiquette}; shift
GUI=""
[ "${1:-}" = --gui ] && { GUI=--gui; shift; }
EXTRA=${1:?propriétés en plus (ou -)}; shift
[ "$EXTRA" = - ] && EXTRA=""
DISK="${DEVDISK:-$R/disks/tiger-dev-tlbb.raw}"
QB="${QEMU_BIN:-$HOME/src/qemu-tlbb/build/qemu-system-ppc}"
case "$DISK" in *tiger.qcow2*) echo "refus : $DISK est le disque de la VM quotidienne"; exit 1 ;; esac
OUT="$R/bench/tcg/tlbb/$TAG"; mkdir -p "$OUT"
CPU="x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-vmx-inline=on,x-vfp-native=on,x-lmw-vector=on,x-fp-inline=on,x-fp-native=on,x-fp-native64=on,x-fp-native-cmp=on,x-fp-known=on,x-fprf-lazy=on,x-stfs-known=on,x-vfp-native-cmp=on,x-tb-fast=on,x-ret-inline=on,x-jc-idx=on,x-icbi-sync=on,x-msr-nobql=on,x-tlb-precise=on,x-lmw-inline=on,x-dcbz-inline=on,x-dirty-skip=on,x-pteg-map=on,x-lfs-known=on"
TCG="x-jit-near=on,x-jit-rel32=on,x-jc-bits=14,x-jc-word=on,x-jc-2way=on"

if fuser "$DISK" >/dev/null 2>&1; then
  echo "⚠ $DISK est tenu par un autre processus (fuser) : abandon"; fuser -v "$DISK"; exit 1
fi
# le StartupItem rejoue le dernier job de la boîte d'entrée : en-tête effacé (comme t2dev.sh)
if [ -f bench/devloop/mailbox.json ]; then
  python3 - "$DISK" <<'PY'
import json, sys
box = json.load(open("bench/devloop/mailbox.json"))
with open(sys.argv[1], "r+b") as f:
    f.seek(box["IN"] * 512); f.write(b"\0" * 512)
PY
fi
echo "=== $TAG : ${EXTRA:-(rien de plus)} $GUI $(date +%H:%M:%S) ; QEMU $QB" | tee "$OUT/run.log"
uptime >> "$OUT/run.log"
export DEVDISK="$DISK" SSH_PORT="${SSH_PORT:-2272}"
QEMU_BIN="$QB" SMP=2 PPC32MT=1 CPU_OPTS="$CPU${EXTRA:+,$EXTRA}" TCG_OPTS="$TCG" \
  EGL_PLATFORM=device python3 tools/guest/devloop.py start $GUI 2>&1 | tail -3 | tee -a "$OUT/run.log"
PID=$(cat bench/devloop/qemu.pid)
n=0
for spec in "$@"; do
  n=$((n + 1))
  job=${spec%%:*}; vars=""; [ "$job" != "$spec" ] && vars=${spec#*:}
  J="$OUT/$n-$job"; rm -rf "$J"
  if [ "$job" = suite ]; then
    # vfptest vmxtest lmwtest tbtest smctest jctest, puis atomtest (comme ppc32mt-bench.sh)
    mkdir -p "$J/out"
    cp tools/guest/jobs/jit-suite/job.sh "$J/"
    for t in vfptest vmxtest lmwtest tbtest smctest jctest; do cp "tools/guest/jobs/$t/$t.c" "$J/"; done
    cp tools/guest/jobs/mtbench/atomtest.c "$J/"
    printf '%s\n' 'gcc -O2 -Wall -o atomtest atomtest.c > "$OUT/atomtest-build.txt" 2>&1' \
      './atomtest 4000000 2 > "$OUT/atomtest.txt" 2>&1' 'cat "$OUT/atomtest.txt"' >> "$J/job.sh"
  else
    sh tools/guest/jobs/stage.sh "$job" "$J" > /dev/null
    [ -f "tools/guest/jobs/$job/$job.c" ] && cp "tools/guest/jobs/$job/"*.c "$J/"
  fi
  : > "$J/env.sh"
  IFS=, read -ra kv <<< "$vars"
  for x in "${kv[@]}"; do [ -n "$x" ] && echo "${x%%=*}=\"${x#*=}\"" >> "$J/env.sh"; done
  echo "--- job $n : $job ${vars:-} $(date +%H:%M:%S)" | tee -a "$OUT/run.log"
  python3 tools/guest/devloop.py run "$J" --timeout "${TIMEOUT:-2400}" > "$OUT/job-$n.log" 2>&1
  echo "    code $? $(date +%H:%M:%S)" | tee -a "$OUT/run.log"
  d=$(sed -n 's/^→ //p' "$OUT/job-$n.log" | tail -1)
  [ -n "$d" ] && [ -d "$d" ] && cp -rf "$d" "$J/out" 2>/dev/null
done
python3 tools/guest/devloop.py shutdown $GUI >> "$OUT/run.log" 2>&1
for i in $(seq 1 60); do kill -0 "$PID" 2>/dev/null || break; sleep 2; done
if kill -0 "$PID" 2>/dev/null; then
  echo "⚠ QEMU encore vivant après shutdown : devloop.py stop" | tee -a "$OUT/run.log"
  python3 tools/guest/devloop.py stop >> "$OUT/run.log" 2>&1
fi
cp -f bench/devloop/qemu.log "$OUT/qemu.log" 2>/dev/null
grep -a -E 'verify|x-lfs-known|DIVERGENCE|tlb-base' "$OUT/qemu.log" | tail -5 | cut -c1-400 | tee -a "$OUT/run.log"
