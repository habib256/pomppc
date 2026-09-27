#!/bin/bash
# ab-copie-gpu.sh — A/B en VM des copies surface → texture (27/09/2026,
# docs/protocole-v20-surface-texture.md §« Copie GPU ») : la matrice des jeux
# qui copient (Colin McRae : SURF_TEX ; DOOM 3, Prey : COPY_TEX) jouée deux
# fois sur le même binaire QEMU, relecture CPU (QGPU_GPU_COPY=0) puis copie
# GPU, chaque passe sur un QEMU relancé. La VM quotidienne doit être libre.
#
#   tools/matrice/ab-copie-gpu.sh [QEMU_BIN] [jeux] [modes] [passes]
#     QEMU_BIN : défaut ~/src/qemu-gpucopy/build/qemu-system-ppc (« 64 » ajouté
#                par run_tiger.sh en SMP)
#     jeux     : défaut cmr,d3,prey ; modes : défaut fen,pe
#     passes   : défaut « 0 1 » (valeurs de QGPU_GPU_COPY, dans l'ordre)
#
# Sorties : bench/matrice/ab-copie-<horodatage>-gpu<0|1>/ du dépôt principal ;
# la ligne « qgpu: copies surface → texture : … » du journal de QEMU
# (run_tiger.log dans ce dossier) dit quel chemin a tourné. À la fin, la VM
# est rendue sur le QEMU de référence (tools/tcg/d3run.sh --restore).
set -u
R=/Users/mercure/src/pomppc
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QB="${1:-/Users/mercure/src/qemu-gpucopy/build/qemu-system-ppc}"
JEUX="${2:-cmr,d3,prey}"
MODES="${3:-fen,pe}"
PASSES="${4:-0 1}"
TS=$R/.run/cmr/tssh.sh
MON=$R/.run/mon.sock
H=$(date +%Y%m%d-%H%M)

stop_vm() {
  if pgrep -f "tiger.qcow2" >/dev/null; then
    $TS "echo tiger974 | sudo -S shutdown -h now" >/dev/null 2>&1
    for i in $(seq 1 40); do pgrep -f "tiger.qcow2" >/dev/null || break; sleep 3; done
    if pgrep -f "tiger.qcow2" >/dev/null; then
      python3 "$WT/tools/tcg/hmp.py" "$MON" quit >/dev/null 2>&1; sleep 5
    fi
  fi
  rm -f $R/.run/tiger.lock
}

wait_ssh() {
  for i in $(seq 1 60); do
    $TS true >/dev/null 2>&1 && return 0
    sleep 5
  done
  return 1
}

for P in $PASSES; do
  OUT=$R/bench/matrice/ab-copie-$H-gpu$P
  mkdir -p "$OUT"
  echo "▶ passe QGPU_GPU_COPY=$P ($QB) → $OUT"
  stop_vm
  for essai in 1 2; do
    ( cd "$R" && QEMU_BIN="$QB" QGPU_GPU_COPY=$P nohup ./run_tiger.sh > "$OUT/run_tiger.log" 2>&1 & )
    sleep 20
    wait_ssh && break
    python3 "$WT/tools/tcg/hmp.py" "$MON" system_reset >/dev/null 2>&1
    wait_ssh && break
    stop_vm
  done
  sleep 40                                      # bureau au repos
  grep -h "copies surface" "$OUT/run_tiger.log" | head -1
  # QEMU_BIN et QGPU_GPU_COPY restent dans l'environnement : un QEMU relancé
  # par la matrice (hote.relance_qemu) garde la passe
  QEMU_BIN="$QB" QGPU_GPU_COPY=$P python3 "$WT/tools/matrice/matrice.py" \
      -j "$JEUX" -m "$MODES" --sortie "$OUT" 2>&1 | tee "$OUT/matrice.log"
done
bash "$WT/tools/tcg/d3run.sh" --restore
