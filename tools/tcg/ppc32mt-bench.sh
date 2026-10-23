#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# ppc32mt-bench.sh ÉTIQUETTE JOB [JOB…] — un démarrage de la VM de DÉVELOPPEMENT
# (single-user, PC Linux) avec toutes les options de production de run_tiger.sh
# (Linux x86-64), puis les jobs devloop donnés, chacun sous `perf stat` (instructions
# et cycles de l'hôte en mode utilisateur, tout le processus QEMU). Pour le levier
# « G4 sur la cible 32 bits en MTTCG » (docs/ppc32-mttcg.md). Jamais la VM quotidienne.
#
#   DEVDISK=~/src/pomppc/disks/tiger-dev-ppc32.raw \
#   QEMU_BIN=~/src/qemu-ppc32mt/build-mt/qemu-system-ppc SMP=2 PPC32MT=1 \
#       tools/tcg/ppc32mt-bench.sh mt-ppc32 mtbench t2bench
#
# JOB : mtbench (calcul pur), t2bench (gltest d3inter, plugin + GLEngine + kext),
# suite (vfptest vmxtest lmwtest tbtest smctest jctest + atomtest), ou tout autre
# nom de tools/guest/jobs/ (stage.sh).
# Variables : SMP (défaut 1), PPC32MT=1 (SMP sur qemu-system-ppc ; sinon ppc64),
# CPU_EXTRA (propriétés de CPU en plus, p. ex. les vérificateurs), TCG_EXTRA,
# QEXTRA (arguments QEMU), FR/REPS (t2bench), REPS/ATOMN (mtbench), TIMEOUT.
# Sorties : bench/ppc32mt/<ÉTIQUETTE>/ du worktree : run.log, qemu.log,
# <job>.log (sortie du job), <job>.perf (perf stat), <job>-out/.
set -u
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$WT"
TAG=${1:?étiquette}; shift
[ $# -ge 1 ] || { echo "usage : $0 ÉTIQUETTE JOB [JOB…]" >&2; exit 2; }
DISK="${DEVDISK:?DEVDISK=<image raw de dev> requis}"
QB="${QEMU_BIN:?QEMU_BIN requis}"
export SMP="${SMP:-1}" PPC32MT="${PPC32MT:-}"
OUT="$WT/bench/ppc32mt/$TAG"; mkdir -p "$OUT"
DL="$WT/tools/guest/devloop.py"
CPU="x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-vmx-inline=on,x-vfp-native=on,x-lmw-vector=on,x-fp-inline=on,x-fp-native=on,x-fp-native64=on,x-fp-native-cmp=on,x-fp-known=on,x-fprf-lazy=on,x-stfs-known=on,x-vfp-native-cmp=on,x-tb-fast=on,x-ret-inline=on,x-jc-idx=on,x-icbi-sync=on,x-msr-nobql=on,x-tlb-precise=on,x-lmw-inline=on,x-dcbz-inline=on,x-dirty-skip=on,x-pteg-map=on"
TCG="x-jit-near=on,x-jit-rel32=on,x-jc-bits=14,x-jc-word=on,x-jc-2way=on"
[ -n "${CPU_EXTRA:-}" ] && CPU="$CPU,$CPU_EXTRA"
[ -n "${TCG_EXTRA:-}" ] && TCG="$TCG,$TCG_EXTRA"

if fuser "$DISK" >/dev/null 2>&1; then
  echo "⚠ $DISK est tenu par un autre processus (fuser) : abandon"; fuser -v "$DISK"; exit 1
fi
[ -f bench/devloop/mailbox.json ] || { echo "⚠ bench/devloop/mailbox.json absent (le copier du dépôt)"; exit 1; }
python3 - "$DISK" <<'PY'
import json, sys
box = json.load(open("bench/devloop/mailbox.json"))
with open(sys.argv[1], "r+b") as f:
    f.seek(box["IN"] * 512); f.write(b"\0" * 512)
PY
{
  echo "=== $TAG $(date '+%F %T') : QEMU $QB SMP=$SMP PPC32MT=${PPC32MT:-0} jobs : $*"
  echo "CPU=$CPU"; echo "TCG=$TCG"; echo "QEXTRA=${QEXTRA:-}"
  uptime
} | tee "$OUT/run.log"
DEVDISK="$DISK" QEMU_BIN="$QB" CPU_OPTS="$CPU" TCG_OPTS="$TCG" SSH_PORT=0 \
  EGL_PLATFORM=device QEMU_EXTRA="${QEXTRA:-}" \
  python3 "$DL" start 2>&1 | tail -3 | tee -a "$OUT/run.log"
PID=$(cat bench/devloop/qemu.pid)
echo "pid $PID : $(tr '\0' ' ' < /proc/$PID/cmdline | cut -c1-200)" >> "$OUT/run.log"

# Sonde de l'agent (devloop start perd parfois sa frappe : docs/tcg-g4.md §30.5)
PROBE="$OUT/probe-job"; mkdir -p "$PROBE"
printf '#!/bin/sh\necho agent-ok\n' > "$PROBE/job.sh"
boxes="$(python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); print(c["IN"], c["OU"])' \
         bench/devloop/mailbox.json)"
ok=0
for k in 1 2 3 4 5; do
  if DEVDISK="$DISK" python3 "$DL" run "$PROBE" --timeout 150 2>&1 | grep -q agent-ok; then ok=1; break; fi
  echo "agent muet (essai $k) : frappe relancée" | tee -a "$OUT/run.log"
  DEVDISK="$DISK" python3 "$DL" type '\n'; sleep 2
  DEVDISK="$DISK" python3 "$DL" type 'mount -uw /\n'; sleep 4
  DEVDISK="$DISK" python3 "$DL" type "sh /pomppc/agent.sh $boxes\n"; sleep 15
done
[ "$ok" = 1 ] || echo "⚠ agent toujours muet" | tee -a "$OUT/run.log"

for JOB in "$@"; do
  J="$OUT/$JOB-job"; rm -rf "$J"
  case "$JOB" in
    mtbench)
      sh tools/guest/jobs/stage.sh mtbench "$J" > /dev/null
      cp tools/guest/jobs/regime/regbench.c tools/guest/jobs/vfptest/vfptest.c \
         tools/guest/jobs/lmwtest/lmwtest.c tools/guest/jobs/mtbench/atomtest.c "$J/"
      printf 'REPS=%s\nATOMN=%s\n' "${REPS:-3}" "${ATOMN:-2000000}" > "$J/env.sh" ;;
    suite)
      mkdir -p "$J"
      cp tools/guest/jobs/jit-suite/job.sh "$J/"
      for n in vfptest vmxtest lmwtest tbtest smctest jctest; do
        cp "tools/guest/jobs/$n/$n.c" "$J/"
      done
      cp tools/guest/jobs/mtbench/atomtest.c "$J/"
      cat >> "$J/job.sh" <<'EOF'
gcc -O2 -Wall -o atomtest atomtest.c > "$OUT/atomtest-build.txt" 2>&1
./atomtest 4000000 2 > "$OUT/atomtest.txt" 2>&1
cat "$OUT/atomtest.txt"
EOF
      ;;
    mtcharge)
      # longue charge (docs/ppc32-mttcg.md §10) : TIMEOUT > DUREE
      sh tools/guest/jobs/stage.sh mtcharge "$J" > /dev/null
      cp tools/guest/jobs/regime/regbench.c tools/guest/jobs/vfptest/vfptest.c \
         tools/guest/jobs/mtbench/atomtest.c "$J/"
      printf 'DUREE=%s\nATOMN=%s\nGLFR=%s\nGLPAUSE=%s\n' "${DUREE:-3600}" "${ATOMN:-1000000}" \
        "${GLFR:-60}" "${GLPAUSE:-20}" > "$J/env.sh" ;;
    t2bench)
      sh tools/guest/jobs/stage.sh t2bench "$J" > /dev/null
      printf 'FR=%s\nREPS=%s\n' "${FR:-150}" "${REPS:-3}" > "$J/env.sh" ;;
    *)
      sh tools/guest/jobs/stage.sh "$JOB" "$J" > /dev/null ;;
  esac
  perf stat -x, -e instructions:u,cycles:u,task-clock -p "$PID" -o "$OUT/$JOB.perf" &
  PP=$!
  t0=$(date +%s.%N)
  DEVDISK="$DISK" python3 "$DL" run "$J" --timeout "${TIMEOUT:-2400}" > "$OUT/$JOB.log" 2>&1
  rc=$?
  t1=$(date +%s.%N)
  kill -INT "$PP" 2>/dev/null; wait "$PP" 2>/dev/null
  echo "job $JOB : code $rc, $(python3 -c "print('%.1f' % ($t1 - $t0))") s" | tee -a "$OUT/run.log"
  d=$(sed -n 's/^→ //p' "$OUT/$JOB.log" | tail -1)
  [ -n "$d" ] && [ -d "$d" ] && { rm -rf "$OUT/$JOB-out"; cp -rf "$d" "$OUT/$JOB-out"; }
  grep -a '^mt \|^passe\|^atom\|^ncpu\|^charge\|^releve' "$OUT/$JOB.log" | tee -a "$OUT/run.log" > /dev/null
  grep -v '^#' "$OUT/$JOB.perf" | sed '/^$/d' | sed "s/^/perf $JOB /" >> "$OUT/run.log"
done

DEVDISK="$DISK" python3 "$DL" shutdown >> "$OUT/run.log" 2>&1
for i in $(seq 1 60); do kill -0 "$PID" 2>/dev/null || break; sleep 2; done
if kill -0 "$PID" 2>/dev/null; then
  echo "⚠ QEMU encore vivant après shutdown : devloop.py stop" | tee -a "$OUT/run.log"
  DEVDISK="$DISK" python3 "$DL" stop >> "$OUT/run.log" 2>&1
fi
cp -f bench/devloop/qemu.log "$OUT/qemu.log" 2>/dev/null
echo "→ $OUT"
