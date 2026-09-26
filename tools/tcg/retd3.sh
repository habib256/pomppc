#!/bin/bash
# retd3.sh [N] — A/B DOOM 3 de tcg/0008 (docs/tcg-g4.md §16) sur la VM
# QUOTIDIENNE : N parties par mode (défaut 3), entrelacées ref / on, SMP=2,
# x-sr-tlb, `sample` hôte à PROFIL (après la fenêtre de mesure), puis la VM
# rendue au binaire de référence (d3run.sh --restore).
#   QEMU_BIN=~/src/qemu-ret/build/qret19 tools/tcg/retd3.sh 3
# ref = propriétés par défaut de run_tiger.sh ; on = + RETINLINE=1 JCIDX=1.
set -u
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
N="${1:-3}"
QB="${QEMU_BIN:?QEMU_BIN requis (binaire qgpu v19 portant tcg/0008)}"
for i in $(seq 1 "$N"); do
  QEMU_BIN="$QB" bash "$WT/tools/tcg/d3run.sh" "ret-ref-$i" 2 1 1
  QEMU_BIN="$QB" RETINLINE=1 JCIDX=1 bash "$WT/tools/tcg/d3run.sh" "ret-on-$i" 2 1 1
done
bash "$WT/tools/tcg/d3run.sh" --restore
