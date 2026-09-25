#!/bin/bash
# regab.sh ÉTIQUETTE… — campagne « deux régimes » (docs/tcg-g4.md §14) sur le
# disque de DÉVELOPPEMENT (jamais la VM quotidienne). Chaque étiquette = un
# processus QEMU neuf : démarrage du bureau, relevé de la carte mémoire de QEMU
# (base du tampon du JIT, base du binaire : `vmmap`), job `regime` (micro-banc
# regbench + Marble Blast), redémarrage PROPRE de l'invité dans le même
# processus, second job `regime`, arrêt propre. Répond à : le régime suit-il le
# processus hôte (mêmes indices avant/après le redémarrage de l'invité) ou le
# démarrage de l'invité ?
#
#   DEVDISK=bench/reg/tiger-reg.raw tools/tcg/regab.sh p01 p02 …
#
#   QEMU_BIN   (défaut ~/src/qemu-tcg19/build/qsr15 ; SMP>1 : …64)
#   SMP=2  NGUEST=2 (démarrages d'invité par processus)  DUR=150  NTOUR=3
#   CPU_OPTS   (défaut : les propriétés allumées par run_tiger.sh)
#   TCG_OPTS   propriétés de l'accélérateur (x-jit-near=on, x-jit-addr=0x300000000)
#   QEXTRA     arguments QEMU en plus
#   SAMPLE_AT=S  `sample` hôte 10 s, S s après le début du 1er job (défaut 230,
#              pendant la passe de mesure ; 0 = aucun)
# Résultats : bench/reg/res/<étiquette>-g<n>-{mb,bench,log}.txt, <étiquette>-vmmap.txt,
# <étiquette>-sample.txt, <étiquette>-info.txt ; bilan : tools/tcg/regreport.py.
set -u
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$WT"
DISK="${DEVDISK:?DEVDISK=<image raw de dev> requis}"
export DEVDISK="$DISK"
QB="${QEMU_BIN:-$HOME/src/qemu-tcg19/build/qsr15}"
SMP="${SMP:-2}"; NGUEST="${NGUEST:-2}"
OPTS="${CPU_OPTS:-x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on}"
RES="$WT/bench/reg/res"; mkdir -p "$RES"
MON="$WT/bench/devloop/mon.sock"
BIN="$QB"; [ "$SMP" -gt 1 ] && BIN="${QB}64"

clearbox() {
  python3 - "$DISK" <<'PY'
import json, sys
box = json.load(open("bench/devloop/mailbox.json"))
with open(sys.argv[1], "r+b") as f:
    f.seek(box["IN"] * 512); f.write(b"\0" * 512)
PY
}

job() { # job ÉTIQUETTE REBOOT
  local tag=$1 rb=$2 J="$WT/bench/reg/job-$1"
  rm -rf "$J"; mkdir -p "$J"
  cp tools/guest/jobs/regime/* tools/guest/jobs/lib.sh "$J/"
  printf 'TAG=%s\nNTOUR=%s\nDUR=%s\nREBOOT=%s\n' "$tag" "${NTOUR:-3}" "${DUR:-150}" "$rb" > "$J/env.sh"
  python3 tools/guest/devloop.py run "$J" --timeout 1500 > "$RES/$tag-run.log" 2>&1
  local out; out=$(sed -n 's/^→ //p' "$RES/$tag-run.log" | tail -1)
  [ -n "$out" ] || { echo "   $tag : pas de résultat"; return 1; }
  cp "$out/log.txt" "$RES/$tag-log.txt"
  cp "$out/bench-$tag.txt" "$RES/$tag-bench.txt" 2>/dev/null
  cp "$out/mb-$tag-1.txt" "$RES/$tag-mb.txt" 2>/dev/null
}

for P in "$@"; do
  echo "=== $P : $(date +%H:%M:%S) SMP=$SMP bin=$(basename "$BIN") tcg='${TCG_OPTS:-}' qextra='${QEXTRA:-}'"
  clearbox
  QEMU_BIN="$QB" SMP="$SMP" CPU_OPTS="$OPTS" \
    QEMU_EXTRA="-monitor unix:$MON,server=on,wait=off ${QEXTRA:-}" \
    python3 tools/guest/devloop.py start --gui | tail -1
  pid=$(cat bench/devloop/qemu.pid)
  vmmap -interleaved "$pid" > "$RES/$P-vmmap.txt" 2>&1
  jit=$(awk '/rwx\/rwx SM=ZER/ {split($2,a,"-"); print a[1]; exit}' "$RES/$P-vmmap.txt")
  txt=$(awk -v b="$(basename "$BIN")" '$1=="__TEXT" && $0 ~ b"$" {split($2,a,"-"); print a[1]; exit}' "$RES/$P-vmmap.txt")
  { echo "pid $pid bin $BIN smp $SMP opts $OPTS tcg '${TCG_OPTS:-}' qextra '${QEXTRA:-}'"
    grep -h 'tampon JIT' bench/devloop/qemu.log
    echo "jit 0x$jit text 0x$txt"
    top -l 2 -s 3 -n 6 -o cpu -stats pid,command,cpu | tail -7; } > "$RES/$P-info.txt"
  echo "   jit 0x$jit text 0x$txt"
  if [ "${SAMPLE_AT:-230}" != 0 ]; then
    ( sleep "${SAMPLE_AT:-230}"; sample "$pid" 10 -file "$RES/$P-sample.txt" > /dev/null 2>&1 ) &
  fi
  g=1
  while [ "$g" -le "$NGUEST" ]; do
    rb=1; [ "$g" -eq "$NGUEST" ] && rb=0
    job "$P-g$g" "$rb"
    python3 tools/tcg/regidx.py "$(cat bench/reg/reffast.txt)" "$RES/$P-g$g-mb.txt" | sed 's/^/   /'
    if [ "$rb" = 1 ]; then clearbox; sleep 30; fi
    g=$((g + 1))
  done
  python3 tools/guest/devloop.py shutdown --gui | tail -1
  sleep 5
done
echo "=== fini $(date +%H:%M:%S)"
