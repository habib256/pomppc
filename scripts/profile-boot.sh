#!/usr/bin/env bash
# Profile un boot headless avec perf pour voir où l'hôte brûle ses cycles.
# Usage: scripts/profile-boot.sh [duree_s]   (défaut 55s, ~ le temps d'un boot)
# Nécessite peut-être: sudo sysctl kernel.perf_event_paranoid=1
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/config.env"
DUR="${1:-55}"

SCR="${POMPPC_SCRATCH:-$ROOT/.run}"; mkdir -p "$SCR"
MON="$SCR/mon.sock"; rm -f "$MON"
mkdir -p "$ROOT/bench"          # gitignoré : absent d'un clone neuf
DATA="$ROOT/bench/perf.data"
BOOTDEV='hd:10,\System\Library\CoreServices\BootX'

echo "Binaire profilé: $QEMU_BIN"
setsid "$QEMU_BIN" -M "$MACHINE" -cpu "$CPU" -m "$RAM_MB" -smp "$SMP" \
  -display none -g "$RES" \
  -drive "file=$DISK,format=qcow2,media=disk" \
  "${NET_ARGS[@]}" \
  -prom-env 'auto-boot?=true' -prom-env "boot-device=$BOOTDEV" -prom-env 'boot-args=-v' \
  -serial "file:$ROOT/bench/measure.log" -name "POMPPC-profile" \
  -monitor "unix:$MON,server,nowait" >/dev/null 2>&1 &

for _ in $(seq 1 30); do [ -S "$MON" ] && break; sleep 0.5; done
QPID=$(pgrep -f "qemu-system-ppc.*POMPPC-profile" | head -1)
[ -n "$QPID" ] || { echo "qemu introuvable"; exit 1; }
echo "PID qemu=$QPID — perf record ${DUR}s (tout le boot)…"

perf record -g --call-graph dwarf -F 400 -o "$DATA" -p "$QPID" -- sleep "$DUR" 2>&1 | tail -3

echo "--- arrêt VM ---"
python3 "$ROOT/scripts/moncmd.py" "$MON" "quit" >/dev/null 2>&1 || true

echo
echo "=== TOP fonctions hôte (self) ==="
perf report -i "$DATA" --stdio --no-children 2>/dev/null | grep -vE '^#|^$' | head -25
echo
echo "Rapport complet: perf report -i $DATA"
