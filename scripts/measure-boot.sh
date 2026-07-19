#!/usr/bin/env bash
# Mesure le temps de boot (headless) jusqu'à l'écran bleu (login/bureau).
# Détection par couleur moyenne: boot=gris neutre / kernel -v=noir / login=bleu.
# Usage: scripts/measure-boot.sh [timeout_s]
set -uo pipefail   # pas de -e: les comparaisons arithmétiques fausses ne doivent pas tuer le script
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/config.env"
TIMEOUT="${1:-360}"

SCR="${POMPPC_SCRATCH:-$ROOT/.run}"; mkdir -p "$SCR"
MON="$SCR/mon.sock"; rm -f "$MON"
FRAMES="$ROOT/bench/frames"; mkdir -p "$FRAMES"; rm -f "$FRAMES"/*.png 2>/dev/null || true
BOOTDEV='hd:10,\System\Library\CoreServices\BootX'

# Overrides pour expérimenter (SMP, MTTCG, tb-size…). Ex:
#   SMP_N=2 EXTRA_ARGS="-accel tcg,thread=multi,tb-size=256" scripts/measure-boot.sh
SMP_N="${SMP_N:-$SMP}"
read -r -a EXTRA <<< "${EXTRA_ARGS:-}"
echo "### config: SMP=$SMP_N  extra='${EXTRA_ARGS:-}'"

qmon(){ python3 "$SCR/moncmd.py" "$MON" "$1" 2>/dev/null; }
meancolor(){ # -> "R G B" moyen d'un ppm
  convert "$1" -resize 1x1 -format "%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]" info:
}

START=$(date +%s.%N)
setsid "$QEMU_BIN" -M "$MACHINE" -cpu "$CPU" -m "$RAM_MB" -smp "$SMP_N" \
  -display none -g "$RES" \
  -drive "file=$DISK,format=qcow2,media=disk" \
  "${NET_ARGS[@]}" \
  -prom-env 'auto-boot?=true' -prom-env "boot-device=$BOOTDEV" -prom-env 'boot-args=-v' \
  -serial "file:$ROOT/bench/measure.log" -name "POMPPC-measure" \
  -monitor "unix:$MON,server,nowait" "${EXTRA[@]}" >/dev/null 2>&1 &

# attendre le socket
for _ in $(seq 1 30); do [ -S "$MON" ] && break; sleep 0.5; done
QPID=$(pgrep -f "qemu-system-ppc.*POMPPC-measure" | head -1)
HZ=$(getconf CLK_TCK)
cpu_time(){ # temps CPU (s) de QEMU: (utime+stime)/HZ — immunisé à la contention
  [ -n "$QPID" ] && [ -r "/proc/$QPID/stat" ] || { echo "?"; return; }
  awk -v hz="$HZ" '{print ($14+$15)/hz}' "/proc/$QPID/stat"
}

echo "t(s)  R   G   B   état"
BOOT_T=""
i=0
while :; do
  NOW=$(date +%s.%N); EL=$(echo "$NOW - $START" | bc)
  ELI=${EL%.*}; ELI=${ELI:-0}; [ -z "$ELI" ] && ELI=0
  if (( ELI > TIMEOUT )); then echo "timeout ${TIMEOUT}s"; break; fi
  PPM="$SCR/mframe.ppm"
  qmon "screendump $PPM" >/dev/null 2>&1 || { echo "VM disparue à ${ELI}s"; break; }
  [ -f "$PPM" ] || { sleep 2; continue; }
  read R G B < <(meancolor "$PPM")
  # snapshot horodaté pour vérification visuelle
  printf -v idx "%03d" "$i"; convert "$PPM" "$FRAMES/f${idx}_${ELI}s.png" 2>/dev/null || true
  # login/bureau = bleu dominant et lumineux
  STATE="boot"
  if (( B > R + 12 )) && (( B > 110 )); then STATE="BLEU(login/bureau)"; fi
  printf "%4s %3s %3s %3s  %s\n" "$ELI" "$R" "$G" "$B" "$STATE"
  if [ "$STATE" != "boot" ]; then BOOT_T="$ELI"; break; fi
  i=$((i+1)); sleep 2
done

CPU_T=$(cpu_time)
echo
if [ -n "$BOOT_T" ]; then
  echo "=== écran bleu atteint : wall=${BOOT_T}s | CPU_qemu=${CPU_T}s (1 CPU, ${RAM_MB}Mo) ==="
  echo "wall=$BOOT_T cpu=$CPU_T" > "$ROOT/bench/last-boot.txt"
else
  echo "=== écran bleu non détecté (voir bench/frames/) | CPU_qemu=${CPU_T}s ==="
fi
# couper la VM (ne pas laisser tourner et polluer les mesures suivantes)
python3 "$SCR/moncmd.py" "$MON" "quit" >/dev/null 2>&1 || true
echo "Frames: $ROOT/bench/frames/"
