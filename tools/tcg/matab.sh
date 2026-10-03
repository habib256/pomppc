#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# matab.sh CAMPAGNE N "A:VAR=v …" "B:VAR=v …" [JEU MODE] — A/B ENTRELACÉ d'un
# réglage de QEMU sur la VM QUOTIDIENNE, par la matrice (docs/tcg-g4.md §18).
#
# Chaque partie relance QEMU (le dépôt principal, ./run_tiger.sh, fenêtre
# native) avec les variables du mode, attend le bureau, puis joue une cellule
# de la matrice sans vidage (`matrice.py -j JEU -m MODE --sans-vidage`, défaut
# d3 fen). Ordre A B B A A B B A … (N parties par mode) : une dérive lente de
# l'hôte pèse autant sur les deux modes.
#
#   QEMU_BIN=~/src/qemu-vit/build/qvit tools/tcg/matab.sh jc 6 \
#       "ref:" "j14:TCG_OPTS=x-jc-bits=14"
#   tools/tcg/matab.sh --restore        # binaire de référence, SMP=2, bureau
#
# Les variables d'un mode passent par l'environnement de run_tiger.sh ET de la
# matrice (qui relance QEMU elle-même si l'invité gèle) : utiliser celles que
# le run_tiger.sh du dépôt principal connaît (QEMU_BIN, TCG_OPTS, CPU_OPTS,
# ICBISYNC, RETVERIFY…) ; une variable POMPPC_GL_* (drapeau du plugin) est en
# plus transmise au jeu par `matrice.py --env` (30/09). Résultats : <dépôt principal>/bench/tcg/ab/CAMPAGNE/
# <mode>-<k>/ (tour de matrice, run_tiger.log = stderr de QEMU), bilan
# bench/tcg/ab/CAMPAGNE/bilan.txt (tools/tcg/matabsum.py).
# JAMAIS de suppression de .run/tiger.lock : on attend qu'il se libère.
set -u
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# dépôt principal (.run/, bench/), même depuis un worktree ; c'était le chemin du M4 en dur
R="$(git -C "$WT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's|/\.git$||')"
R="${R:-$WT}"
TS=$WT/tools/guest/tssh.sh
LOCK=$R/.run/tiger.lock
. "$WT/scripts/hostcompat.sh"
mon() { local m; m="$(cat "$R/.run/tiger.mon" 2>/dev/null)"; echo "${m:-$R/.run/mon.sock}"; }

stop_vm() {
  if host_locked "$LOCK"; then
    $TS "echo tiger974 | sudo -S shutdown -h now" >/dev/null 2>&1
    host_wait_unlocked "$LOCK" 150 2>/dev/null && return 0
    echo "arrêt propre raté : quit au moniteur"
    python3 "$WT/tools/tcg/hmp.py" "$(mon)" quit >/dev/null 2>&1
    host_wait_unlocked "$LOCK" 60 || { echo "verrou toujours tenu : abandon"; exit 1; }
  fi
}

wait_ssh() {
  for i in $(seq 1 60); do
    $TS true >/dev/null 2>&1 && return 0
    sleep 5
  done
  return 1
}

start_vm() { # start_vm JOURNAL [VAR=v …]
  local log=$1; shift
  ( cd "$R" && env POMPPC_FRONTEND=native "$@" nohup ./run_tiger.sh > "$log" 2>&1 & )
  sleep 20
  wait_ssh || { python3 "$WT/tools/tcg/hmp.py" "$(mon)" system_reset >/dev/null 2>&1; wait_ssh; }
}

if [ "${1:-}" = "--restore" ]; then
  stop_vm
  start_vm "$R/.run/run_tiger-zen.log" && echo "VM quotidienne relancée (binaire de référence)"
  exit 0
fi

C=$1; N=$2; MA=$3; MB=$4; JEU=${5:-d3}; MODE=${6:-fen}
OUT=$R/bench/tcg/ab/$C; mkdir -p "$OUT"
QB="${QEMU_BIN:-}"
na=${MA%%:*}; va=${MA#*:}; nb=${MB%%:*}; vb=${MB#*:}
ordre=()
for k in $(seq 1 "$N"); do
  if [ $((k % 2)) = 1 ]; then ordre+=("a:$k" "b:$k"); else ordre+=("b:$k" "a:$k"); fi
done
echo "campagne $C : $na='$va' / $nb='$vb', QEMU_BIN='$QB', $JEU $MODE, $N parties par mode" | tee -a "$OUT/journal.txt"
for p in "${ordre[@]}"; do
  m=${p%%:*}; k=${p#*:}
  if [ "$m" = a ]; then nom=$na; vars=$va; else nom=$nb; vars=$vb; fi
  D="$OUT/$nom-$k"; mkdir -p "$D"
  echo "$(date +%H:%M:%S) $nom-$k : $vars" | tee -a "$OUT/journal.txt"
  stop_vm
  # shellcheck disable=SC2086
  set -- ${QB:+QEMU_BIN=$QB} $vars
  start_vm "$D/run_tiger.log" "$@" || { echo "  pas de ssh" | tee -a "$OUT/journal.txt"; continue; }
  grep -h '▶ Tiger' "$D/run_tiger.log" | tee -a "$OUT/journal.txt"
  sleep 60                                  # bureau au repos (Finder, Dock, mds)
  # 30/09 : une variable POMPPC_GL_* du mode vise le plugin, donc le JEU dans
  # l'invité : elle passe aussi en --env à la matrice (sans effet sur QEMU)
  genv=()
  for v in "$@"; do case $v in POMPPC_GL_*=*) genv+=(--env "$v") ;; esac; done
  env "$@" python3 "$WT/tools/matrice/matrice.py" -j "$JEU" -m "$MODE" --sans-vidage \
      ${genv[@]+"${genv[@]}"} --sortie "$D" > "$D/matrice.log" 2>&1
  tail -1 "$D/resultats.csv" 2>/dev/null | tee -a "$OUT/journal.txt"
done
stop_vm
python3 "$WT/tools/tcg/matabsum.py" "$OUT" | tee "$OUT/bilan.txt"
