#!/usr/bin/env bash
# Profile un boot headless avec perf pour voir où l'hôte brûle ses cycles.
# Usage: scripts/profile-boot.sh [duree_s]   (défaut 55s, ~ le temps d'un boot)
# Nécessite peut-être: sudo sysctl kernel.perf_event_paranoid=1
#
# Même discipline que measure-boot.sh : PID via -pidfile (un pgrep sur la ligne
# de commande peut profiler un QEMU orphelin d'un run précédent) et trap qui
# tue la VM en sortie (sinon un Ctrl-C laisse tourner une VM qui pollue la
# mesure suivante).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/config.env"
DUR="${1:-55}"

SCR="${POMPPC_SCRATCH:-$ROOT/.run}"; mkdir -p "$SCR"
MON="$SCR/mon.sock"; rm -f "$MON"
PIDFILE="$SCR/profile.pid"; rm -f "$PIDFILE"
mkdir -p "$ROOT/bench"          # gitignoré : absent d'un clone neuf
DATA="$ROOT/bench/perf.data"
BOOTDEV='hd:10,\System\Library\CoreServices\BootX'

QPID=""
cleanup() {
  if [ -n "$QPID" ] && kill -0 "$QPID" 2>/dev/null; then
    kill -TERM "$QPID" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$QPID" 2>/dev/null || break; sleep 0.1; done
    kill -KILL "$QPID" 2>/dev/null
  fi
  rm -f "$PIDFILE"
}
trap cleanup EXIT INT TERM

echo "Binaire profilé: $QEMU_BIN"
setsid "$QEMU_BIN" -M "$MACHINE" -cpu "$CPU" -m "$RAM_MB" -smp "$SMP" \
  -display none -g "$RES" \
  -drive "file=$DISK,format=qcow2,media=disk" \
  "${NET_ARGS[@]}" \
  -prom-env 'auto-boot?=true' -prom-env "boot-device=$BOOTDEV" -prom-env 'boot-args=-v' \
  -serial "file:$ROOT/bench/measure.log" -name "POMPPC-profile" \
  -pidfile "$PIDFILE" \
  -monitor "unix:$MON,server,nowait" >/dev/null 2>&1 &

for _ in $(seq 1 30); do [ -S "$MON" ] && [ -s "$PIDFILE" ] && break; sleep 0.5; done
QPID=$(cat "$PIDFILE" 2>/dev/null || true)
[ -n "$QPID" ] && kill -0 "$QPID" 2>/dev/null || { echo "qemu n'a pas démarré"; exit 1; }
echo "PID qemu=$QPID — perf record ${DUR}s (tout le boot)…"

perf record -g --call-graph dwarf -F 400 -o "$DATA" -p "$QPID" -- sleep "$DUR" 2>&1 | tail -3

echo "--- arrêt VM ---"
python3 "$ROOT/scripts/moncmd.py" "$MON" "quit" >/dev/null 2>&1 || true

echo
echo "=== TOP fonctions hôte (self) ==="
perf report -i "$DATA" --stdio --no-children 2>/dev/null | grep -vE '^#|^$' | head -25
echo
echo "Rapport complet: perf report -i $DATA"
