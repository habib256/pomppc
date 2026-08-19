#!/usr/bin/env bash
# Boot headless du système installé, avec socket moniteur. Chronométré.
# Usage: scripts/boot.sh   (tourne en arrière-plan via setsid)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/config.env"

SCR="${POMPPC_SCRATCH:-$ROOT/.run}"
mkdir -p "$SCR"
MON="$SCR/mon.sock"; rm -f "$MON"

mkdir -p "$ROOT/bench"          # gitignoré : absent d'un clone neuf
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$ROOT/bench/run-$STAMP.log"
date +%s > "$ROOT/bench/.run-start"

# Boot par chemin explicite vers BootX (le bless \\:tbxi ne fonctionne pas ici).
BOOTDEV='hd:10,\System\Library\CoreServices\BootX'

# Mode d'affichage. DÉFAUT = fenêtre (c'est TON Mac).
# Headless = mode de test pour l'assistant (Claude), pour ne pas encombrer le bureau hôte:
#   POMPPC_DISPLAY=none scripts/boot.sh
DISP="${POMPPC_DISPLAY:-gtk}"
export DISPLAY="${DISPLAY:-:1}"

setsid "$QEMU_BIN" -M "$MACHINE" -cpu "$CPU" -m "$RAM_MB" -smp "$SMP" \
  -display "$DISP" -g "$RES" \
  -drive "file=$DISK,format=qcow2,media=disk" \
  "${NET_ARGS[@]}" \
  -prom-env 'auto-boot?=true' \
  -prom-env "boot-device=$BOOTDEV" \
  -prom-env 'boot-args=-v' \
  -serial "file:$LOG" -name "POMPPC-run" \
  -monitor "unix:$MON,server,nowait" \
  > "$LOG.stdout" 2>&1 &

echo "MON=$MON"
echo "LOG=$LOG"
echo "PID=$!"
