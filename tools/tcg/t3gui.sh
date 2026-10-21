#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# t3gui.sh ÉTIQUETTE PROPS_CPU_EN_PLUS — une session VÉRIFIÉE au BUREAU sur la VM de
# DÉVELOPPEMENT (jamais la quotidienne) : démarrage du bureau (devloop.py start --gui, 2 vCPU,
# MTTCG, toutes les options de production de t2dev.sh plus PROPS), jeux (job games : Zenerchi,
# Marble Blast Gold), puis le job t2bench (compilation du kext, du plugin et de gltest dans
# l'invité), extinction propre ; bilan des vérificateurs (x-tier2-verify, x-tier3) à la sortie.
#
#   DEVDISK=~/src/pomppc/disks/tiger-dev-t3.raw QEMU_BIN=~/src/qemu-t3/build/qemu-system-ppc \
#       tools/tcg/t3gui.sh t3-verif-gui x-tier2=16384,x-tier3=400,x-tier2-verify=64
#
# Sorties : bench/tcg/t2/<ÉTIQUETTE>/ (dépôt principal) : qemu.log, games.log, t2bench.log, run.log.
set -u
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
R="$(git -C "$WT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's|/\.git$||')"
R="${R:-$WT}"
cd "$WT"
TAG=${1:?étiquette}
EXTRA=${2:-}
DISK="${DEVDISK:?DEVDISK=<image raw de dev> requis}"
QB="${QEMU_BIN:?QEMU_BIN requis}"
OUT="$R/bench/tcg/t2/$TAG"; mkdir -p "$OUT"
CPU="x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-vmx-inline=on,x-vfp-native=on,x-lmw-vector=on,x-fp-inline=on,x-fp-native=on,x-fp-native64=on,x-fp-native-cmp=on,x-fp-known=on,x-fprf-lazy=on,x-stfs-known=on,x-vfp-native-cmp=on,x-tb-fast=on,x-ret-inline=on,x-jc-idx=on,x-icbi-sync=on,x-msr-nobql=on,x-tlb-precise=on,x-lmw-inline=on,x-dcbz-inline=on,x-dirty-skip=on,x-pteg-map=on"
TCG="x-jit-near=on,x-jit-rel32=on,x-jc-bits=14,x-jc-word=on,x-jc-2way=on"
if fuser "$DISK" >/dev/null 2>&1; then
  echo "⚠ $DISK est tenu par un autre processus (fuser) : abandon"; exit 1
fi
python3 - "$DISK" <<'PY'
import json, sys
box = json.load(open("bench/devloop/mailbox.json"))
with open(sys.argv[1], "r+b") as f:
    f.seek(box["IN"] * 512); f.write(b"\0" * 512)
PY
export DEVDISK="$DISK" QEMU_BIN="$QB" SMP=2 CPU_OPTS="$CPU${EXTRA:+,$EXTRA}" TCG_OPTS="$TCG" \
  EGL_PLATFORM=device
echo "=== $TAG : $EXTRA $(date +%H:%M:%S)" | tee "$OUT/run.log"
python3 tools/guest/devloop.py start --gui 2>&1 | tail -3 | tee -a "$OUT/run.log"
for job in ${JOBS:-games t2bench}; do
  J="$OUT/job-$job"; rm -rf "$J"
  sh tools/guest/jobs/stage.sh "$job" "$J" > /dev/null
  [ "$job" = t2bench ] && printf 'FR=60\nREPS=1\n' > "$J/env.sh"
  python3 tools/guest/devloop.py run "$J" --timeout "${TIMEOUT:-2400}" > "$OUT/$job.log" 2>&1
  echo "job $job : code $? $(date +%H:%M:%S)" | tee -a "$OUT/run.log"
done
python3 tools/guest/devloop.py shutdown --gui >> "$OUT/run.log" 2>&1
PID=$(cat bench/devloop/qemu.pid)
for i in $(seq 1 90); do kill -0 "$PID" 2>/dev/null || break; sleep 2; done
if kill -0 "$PID" 2>/dev/null; then
  echo "⚠ QEMU encore vivant : devloop.py stop" | tee -a "$OUT/run.log"
  python3 tools/guest/devloop.py stop >> "$OUT/run.log" 2>&1
fi
cp -f bench/devloop/qemu.log "$OUT/qemu.log" 2>/dev/null
echo "fin $(date +%H:%M:%S)" | tee -a "$OUT/run.log"
grep -a 'x-tier' "$OUT/qemu.log" | grep -av "région [0-9]* 0x" | tee -a "$OUT/run.log"
