#!/bin/bash
# d3run.sh <étiquette> <SMP> <SRTLB 0|1> [sample 0|1] — une partie de DOOM 3
# (demo_mars_city1) sur la VM QUOTIDIENNE, pour les A/B du processeur émulé
# (docs/tcg-g4.md). Chaque partie redémarre QEMU (DOOM 3 ne se relance pas
# sans redémarrer l'invité, kCGLBadDisplay) :
#
#   1. arrêt propre de l'invité en cours (sudo shutdown -h now par ssh) ;
#   2. ./run_tiger.sh avec QEMU_BIN (défaut : ~/src/qemu-tcg19/build/qsr),
#      SMP et SRTLB demandés, FASTFP inchangé (défaut : allumé) ;
#   3. tools/tcg/guest/meas-tcg.sh dans l'invité (T = fin de la cinématique,
#      fenêtre T+50..T+280, PROFIL à T+300, FINI à T+450) ;
#   4. côté hôte, toutes les 10 s : remise au premier plan si le jeu se replie
#      (« not frontmost », comme frontwatch.sh du lot 3), relevé `info jit` ;
#      à PROFIL, `sample` du processus QEMU pendant 10 s si demandé ;
#   5. rapatrie frames.csv dans bench/tcg/d3/<étiquette>/.
#
# QEMU_BIN=… pour un autre binaire (le lanceur ajoute « 64 » en SMP >= 2).
# L'invité est laissé allumé avec DOOM 3 en cours : la partie suivante le
# redémarre. Pour rendre la VM : tools/tcg/d3run.sh --restore.
set -u
R=/Users/mercure/src/pomppc
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TS=$R/.run/cmr/tssh.sh
MON=$R/.run/mon.sock

stop_vm() {
  if pgrep -f "tiger.qcow2" >/dev/null; then
    $TS "echo tiger974 | sudo -S shutdown -h now" >/dev/null 2>&1
    for i in $(seq 1 40); do pgrep -f "tiger.qcow2" >/dev/null || break; sleep 3; done
    if pgrep -f "tiger.qcow2" >/dev/null; then
      echo "arrêt propre raté : quit au moniteur"
      python3 "$WT/tools/tcg/hmp.py" "$MON" quit >/dev/null 2>&1; sleep 5
    fi
  fi
  rm -f $R/.run/tiger.lock
}

wait_ssh() { # 0 si l'invité répond à ssh dans les 300 s
  for i in $(seq 1 60); do
    $TS true >/dev/null 2>&1 && return 0
    sleep 5
  done
  return 1
}

if [ "${1:-}" = "--restore" ]; then
  # la VM comme on l'a trouvée : binaire de référence, SMP=2, FASTFP défaut
  stop_vm
  cd "$R" && nohup ./run_tiger.sh > "$R/.run/run_tiger-restore.log" 2>&1 &
  wait_ssh && echo "VM quotidienne relancée (binaire de référence, SMP=2)"
  exit 0
fi

L=$1; SMPN=$2; SR=$3; SAMPLE=${4:-0}
QB="${QEMU_BIN:-/Users/mercure/src/qemu-tcg19/build/qsr}"
OUT="$WT/bench/tcg/d3/$L"; mkdir -p "$OUT"

stop_vm
for essai in 1 2; do
  ( cd "$WT" && QEMU_BIN="$QB" SMP="$SMPN" SRTLB="$SR" DISK=$R/disks/tiger.qcow2 \
      POMPPC_SCRATCH=$R/.run NOCD=1 nohup ./run_tiger.sh > "$OUT/run_tiger.log" 2>&1 & )
  sleep 20
  wait_ssh && break
  echo "pas de ssh (panique au démarrage ?) : system_reset"
  python3 "$WT/tools/tcg/hmp.py" "$MON" system_reset >/dev/null 2>&1
  wait_ssh && break
  stop_vm
done
PID=$(pgrep -f "tiger.qcow2" | head -1)
echo "QEMU $PID : $(grep '▶ Tiger' "$OUT/run_tiger.log")" | tee "$OUT/info.txt"
# placement du tampon du JIT (docs/tcg-g4.md §14 : hors de la fenêtre de 4 Gio
# du texte = régime lent) : ligne du patch tcg/0006 si le binaire l'a, et vmmap
grep -h 'tampon JIT' "$OUT/run_tiger.log" | tee -a "$OUT/info.txt"
vmmap -interleaved "$PID" 2>/dev/null | awk -v b="$(basename "$QB")" '
  /rwx\/rwx SM=ZER/ && !j {split($2,a,"-"); j=a[1]}
  $1=="__TEXT" && $NF ~ b && !t {split($2,a,"-"); t=a[1]}
  END {print "jit 0x" j " text 0x" t}' | tee -a "$OUT/info.txt"
sleep 40                                      # bureau au repos
$TS "cat > ~/meas-tcg.sh" < "$WT/tools/tcg/guest/meas-tcg.sh"
$TS "nohup sh ~/meas-tcg.sh $L > /dev/null 2>&1 &"
python3 "$WT/tools/tcg/jitpoll.py" "$MON" 10 3000 > "$OUT/jit.txt" 2>&1 &
JP=$!
T0=$(date +%s)
sampled=0
while :; do
  sleep 10
  out=$($TS "cat ~/meas-$L.txt 2>/dev/null; tail -21 ~/d3-dump/frames.csv 2>/dev/null | awk -F, '\$1 ~ /^[0-9]+\$/ {if (!a) a=\$6; b=\$6; n=\$1} END {print \"N\", n, b-a+0}'" 2>/dev/null)
  echo "$(( $(date +%s) - T0 )) $(echo "$out" | tr '\n' ' ')" >> "$OUT/watch.txt"
  # PROFIL (T+300) avant FINI : à ~65 ms/image les deux arrivent dans le même
  # relevé de 10 s ; le `sample` passe donc avant la sortie de boucle, et le jeu
  # tourne encore pendant les 10 s (la partie ne se ferme qu'après FINI).
  if [ "$SAMPLE" = 1 ] && [ $sampled = 0 ] && echo "$out" | grep -q PROFIL; then
    sample "$PID" 10 -file "$OUT/sample.txt" >/dev/null 2>&1; sampled=1
  fi
  case "$out" in *FINI*|*TIMEOUT*) break;; esac
  n=$(echo "$out" | awk '$1=="N"{print $2}'); r=$(echo "$out" | awk '$1=="N"{print $3}')
  if [ -n "$r" ] && [ "$r" -gt 10 ] && [ "${n:-0}" -gt 150 ]; then
    $TS "osascript -e 'tell application \"System Events\" to set frontmost of process \"Doom 3 Demo\" to true'" >/dev/null 2>&1
    echo "  premier plan ($n, $r replis)" >> "$OUT/watch.txt"
  fi
  [ $(( $(date +%s) - T0 )) -gt 2700 ] && { echo "délai dépassé" >> "$OUT/watch.txt"; break; }
done
kill $JP 2>/dev/null
$TS "cat ~/frames-$L.csv" > "$OUT/frames.csv" 2>/dev/null
$TS "cat ~/meas-$L.txt" > "$OUT/meas.txt" 2>/dev/null
python3 "$WT/tools/tcg/d3win.py" "$OUT/frames.csv" | tee "$OUT/resultat.txt"
