#!/usr/bin/env bash
# Boote le système déjà installé, et chronomètre le temps mur du run (baseline).
source "$(dirname "$0")/lib.sh"
need "$QEMU_BIN"

[[ -f "$DISK" ]] || die "Disque absent. Installe d'abord OS X (00-create-disk.sh puis 10-install.sh)."

qemu_common c   # boot depuis le disque
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$ROOT/bench/run-$STAMP.log"

echo ">>> $QEMU_BIN ${QEMU_ARGS[*]}"
echo ">>> chrono + log kernel: $LOG"
echo ">>> ferme la fenêtre QEMU quand le bureau est chargé pour figer le temps mur."
echo

# -serial: capture la sortie série (utile si on ajoute un port debug plus tard).
# 'time' donne le temps mur total => première métrique de baseline brute.
{ time "$QEMU_BIN" "${QEMU_ARGS[@]}" -serial "file:$LOG"; } 2>&1 | tee -a "$LOG"
