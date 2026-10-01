#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vmwith.sh JOURNAL [VAR=v …] — arrête la VM quotidienne proprement et la relance
# par ./run_tiger.sh avec ces variables (QEMU_BIN, CPU_OPTS, TCG_OPTS…), bureau
# atteint par ssh. Même arrêt et même relance que tools/tcg/matab.sh.
#   tools/tcg/vmwith.sh .run/tb.log QEMU_BIN=~/src/qemu-tbfp/btf/qemu-system-ppc64 CPU_OPTS=x-tb-fast=on
#   tools/tcg/vmwith.sh .run/ref.log          # binaire de référence
# JAMAIS de suppression de .run/tiger.lock.
set -u
R=/Users/mercure/src/pomppc
TS=$R/tools/guest/tssh.sh
LOCK=$R/.run/tiger.lock
. "$R/scripts/hostcompat.sh"
mon() { local m; m="$(cat "$R/.run/tiger.mon" 2>/dev/null)"; echo "${m:-$R/.run/mon.sock}"; }
log=$1; shift
if host_locked "$LOCK"; then
  $TS "echo tiger974 | sudo -S shutdown -h now" >/dev/null 2>&1
  host_wait_unlocked "$LOCK" 150 2>/dev/null || {
    python3 "$R/tools/tcg/hmp.py" "$(mon)" quit >/dev/null 2>&1
    host_wait_unlocked "$LOCK" 60 || { echo "verrou toujours tenu : abandon"; exit 1; }; }
fi
( cd "$R" && env POMPPC_FRONTEND=native "$@" nohup ./run_tiger.sh > "$log" 2>&1 & )
sleep 20
for i in $(seq 1 60); do $TS true >/dev/null 2>&1 && { echo "VM prête ($*)"; exit 0; }; sleep 5; done
echo "pas de ssh après 5 min"; exit 1
