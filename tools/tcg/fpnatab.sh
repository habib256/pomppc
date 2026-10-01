#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# fpnatab.sh SORTIE "A:VAR=v …" "B:VAR=v …" … — bancs invités ENTRELACÉS sur le
# disque de dev (docs/tcg-g4.md §22) : pour chaque mode, dans l'ordre donné,
# démarrage single-user par devloop.py, job JOB (défaut fptest, BANC=10000000),
# arrêt sûr. Les variables d'un mode passent dans l'environnement de devloop
# (QEMU_BIN, CPU_OPTS, TCG_OPTS…) ; ${BASECPU} et ${BASETCG} donnent les
# propriétés par défaut de run_tiger.sh (configuration retenue).
#
#   DEVDISK=bench/tcg/fpnat/dev.raw tools/tcg/fpnatab.sh res/b1 \
#       "ref:QEMU_BIN=… CPU_OPTS=\$BASECPU" "m2:QEMU_BIN=… CPU_OPTS=\$BASECPU,x-fpx-mode=2"
#
# Politesse de mesure : attend l'absence de .run/a4-mesure.lock avant chaque
# démarrage, tient .run/a4-vm-tcg.lock pendant que la VM tourne, relève la
# charge et les autres QEMU (charge.txt).
set -u
R=/Users/mercure/src/pomppc
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT=$1; shift
mkdir -p "$OUT"
JOB=${JOB:-fptest}
BANC=${BANC-10000000}
export SMP=${SMP:-2}
export BASECPU="x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-fp-inline=on,x-ret-inline=on,x-jc-idx=on,x-icbi-sync=on,x-msr-nobql=on"
export BASETCG="x-jit-near=on,x-jc-bits=14"
MYLOCK=$R/.run/a4-vm-tcg.lock
charge() {
  echo "$(date +%H:%M:%S) $1 : charge $(sysctl -n vm.loadavg) ; autres QEMU :" \
       "$(pgrep -fl qemu-system | grep -v "$DEVDISK" | awk '{print $2}' | sed 's|.*/src/||' | tr '\n' ' ')" \
       >> "$OUT/charge.txt"
}
for m in "$@"; do
  nom=${m%%:*}; vars=${m#*:}
  while :; do
    while [ -e "$R/.run/a4-mesure.lock" ]; do sleep 30; done
    echo "fpnat $$ $(date)" > "$MYLOCK"
    sleep 10          # une mesure A4 qui partait en même temps : on lui cède
    [ -e "$R/.run/a4-mesure.lock" ] || break
    rm -f "$MYLOCK"
  done
  D="$OUT/$nom"; k=1
  while [ -e "$D-$k" ]; do k=$((k + 1)); done
  D="$D-$k"; mkdir -p "$D"
  echo "$(date +%H:%M:%S) $nom-$k : $vars" | tee -a "$OUT/journal.txt"
  echo "fpnat $$ $(date)" > "$MYLOCK"
  J=$(mktemp -d)
  cp "$WT/tools/guest/jobs/$JOB/"* "$J/"
  echo "BANC=$BANC" > "$J/env.sh"
  [ -n "${NRAND:-}" ] && echo "NRAND=$NRAND" >> "$J/env.sh"
  [ -n "${DIS:-}" ] && echo "DIS=$DIS" >> "$J/env.sh"
  # shellcheck disable=SC2086
  eval "env $vars python3 $WT/tools/guest/devloop.py start" > "$D/start.txt" 2>&1
  # Hôte chargé : devloop peut taper `mount -uw /` avant l'invite, la racine
  # reste en lecture seule et l'agent échoue. Sonde : un job vide ; s'il ne
  # revient pas, Ctrl-C à l'agent, `mount -uw /`, agent relancé.
  if ! kill -0 "$(cat "$WT/bench/devloop/qemu.pid" 2>/dev/null)" 2>/dev/null; then
    echo "  $nom-$k : QEMU n'a pas démarré (voir $D/qemu.log)" | tee -a "$OUT/journal.txt"
    cp "$WT/bench/devloop/qemu.log" "$D/qemu.log" 2>/dev/null
    rm -f "$MYLOCK" "$WT/bench/devloop/qemu.pid"
    continue
  fi
  P=$(mktemp -d); printf '#!/bin/sh\necho sonde\n' > "$P/job.sh"
  for essai in 1 2 3; do
    python3 "$WT/tools/guest/devloop.py" run "$P" --timeout 60 >> "$D/start.txt" 2>&1 && break
    echo "sonde $essai ratée : agent relancé" >> "$D/start.txt"
    python3 - "$WT/tools/guest" <<'PY' >> "$D/start.txt" 2>&1
import sys, time
sys.path.insert(0, sys.argv[1])
import devloop as d
q = d.Qmp()
q("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                    {"type": "qcode", "data": "c"}], **{"hold-time": 100})
time.sleep(3); d.type_text(q, "\n"); time.sleep(2)
d.type_text(q, "mount -uw /\n"); time.sleep(6)
d.type_text(q, "sh /pomppc/agent.sh %d %d\n" % (d.find_mailbox(b"IN"), d.find_mailbox(b"OU")))
time.sleep(5)
PY
  done
  rm -rf "$P"
  charge "$nom-$k avant"
  python3 "$WT/tools/guest/devloop.py" run "$J" --timeout "${TIMEOUT:-1800}" > "$D/run.txt" 2>&1
  charge "$nom-$k après"
  cp -R "$J/out" "$D/" 2>/dev/null
  python3 "$WT/tools/guest/devloop.py" shutdown > "$D/stop.txt" 2>&1
  cp "$WT/bench/devloop/qemu.log" "$D/qemu.log" 2>/dev/null
  rm -f "$MYLOCK"
  rm -rf "$J"
  grep -h "^banc" "$D/run.txt" 2>/dev/null | sed "s/^/  $nom-$k /" | tee -a "$OUT/journal.txt"
done
