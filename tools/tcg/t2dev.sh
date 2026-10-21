#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# t2dev.sh ÉTIQUETTE [PROPS_CPU_EN_PLUS] — le banc t2bench (scène gltest d3inter : plugin,
# GLEngine, kext) sur la VM de DÉVELOPPEMENT en single-user, PC Linux, toutes les options de production de run_tiger.sh (Linux x86-64) plus
# PROPS_CPU_EN_PLUS (p. ex. x-tier2=4096), pour le traducteur de second niveau
# (docs/plan-traducteur-rapide.md). Jamais la VM quotidienne.
#
#   DEVDISK=~/src/pomppc/disks/tiger-dev-plugin.raw QEMU_BIN=~/src/qemu-t2/build/qemu-system-ppc \
#       tools/tcg/t2dev.sh t2a x-tier2=4096
#
# Variables : PERF=S (perf record de S s du QEMU ; -perfmap posé), PERFAT=S (secondes après le
# début du job, défaut 240), PLUGIN=… (greffon TCG), QEXTRA=… (arguments QEMU), FR (images par
# passe, défaut 150), REPS (passes, défaut 3).
# Sorties : bench/tcg/t2/<ÉTIQUETTE>/ (dépôt principal) : qemu.log (stderr : bilans x-tier2),
# job.log (lignes « passe k … s md5 … »), perf.data + perf-<pid>.map, run.log.
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
  echo "⚠ $DISK est tenu par un autre processus (fuser) : abandon"; fuser -v "$DISK"; exit 1
fi
# le StartupItem rejoue le dernier job de la boîte d'entrée : en-tête effacé (comme mbab.sh)
python3 - "$DISK" <<'PY'
import json, sys
box = json.load(open("bench/devloop/mailbox.json"))
with open(sys.argv[1], "r+b") as f:
    f.seek(box["IN"] * 512); f.write(b"\0" * 512)
PY
QX="${QEXTRA:-}"
[ -n "${PERF:-}" ] && QX="-perfmap $QX"
[ -n "${PLUGIN:-}" ] && QX="$QX -plugin $PLUGIN"
echo "=== $TAG : ${EXTRA:-(rien de plus)} $(date +%H:%M:%S) ; QEMU $QB" | tee "$OUT/run.log"
uptime >> "$OUT/run.log"
DEVDISK="$DISK" QEMU_BIN="$QB" SMP=2 CPU_OPTS="$CPU${EXTRA:+,$EXTRA}" TCG_OPTS="$TCG" \
  EGL_PLATFORM=device QEMU_EXTRA="$QX" \
  python3 tools/guest/devloop.py start 2>&1 | tail -3 | tee -a "$OUT/run.log"
PID=$(cat bench/devloop/qemu.pid)
J="$OUT/job"; rm -rf "$J"
sh tools/guest/jobs/stage.sh t2bench "$J" > /dev/null
printf 'FR=%s\nREPS=%s\n' "${FR:-150}" "${REPS:-3}" > "$J/env.sh"
if [ -n "${PERF:-}" ]; then
  ( sleep "${PERFAT:-240}"; perf record -F 999 -p "$PID" -o "$OUT/perf.data" -- sleep "$PERF" \
      > "$OUT/perf.log" 2>&1 ) &
fi
DEVDISK="$DISK" python3 tools/guest/devloop.py run "$J" --timeout "${TIMEOUT:-2400}" \
  > "$OUT/job.log" 2>&1
echo "job : code $?" | tee -a "$OUT/run.log"
wait
cp -f "/tmp/perf-$PID.map" "$OUT/" 2>/dev/null
d=$(sed -n 's/^→ //p' "$OUT/job.log" | tail -1)
[ -n "$d" ] && [ -d "$d" ] && cp -rf "$d" "$OUT/out" 2>/dev/null
DEVDISK="$DISK" python3 tools/guest/devloop.py shutdown >> "$OUT/run.log" 2>&1
for i in $(seq 1 60); do kill -0 "$PID" 2>/dev/null || break; sleep 2; done
if kill -0 "$PID" 2>/dev/null; then
  echo "⚠ QEMU encore vivant après shutdown : devloop.py stop" | tee -a "$OUT/run.log"
  DEVDISK="$DISK" python3 tools/guest/devloop.py stop >> "$OUT/run.log" 2>&1
fi
cp -f bench/devloop/qemu.log "$OUT/qemu.log" 2>/dev/null
grep -a 'x-tier2' "$OUT/qemu.log" | tee -a "$OUT/run.log"
grep -a '^passe' "$OUT/job.log" | tee -a "$OUT/run.log"
