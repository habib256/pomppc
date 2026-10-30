#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# matab.sh CAMPAGNE N "A:VAR=v …" "B:VAR=v …" ["C:…" …] [JEU MODE] — A/B ENTRELACÉ d'un
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
#   tools/tcg/matab.sh --restore        # binaire par défaut du lanceur, SMP=2, bureau
#   (chaque bras part avec QEMU_FAST=0 : binaire de RÉFÉRENCE build/, sauf QEMU_BIN=… ou
#   QEMU_FAST=1 dans le bras ; docs/binaire-rapide-x86.md)
#
# Les variables d'un mode passent par l'environnement de run_tiger.sh ET de la
# matrice (qui relance QEMU elle-même si l'invité gèle) : utiliser celles que
# le run_tiger.sh du dépôt principal connaît (QEMU_BIN, TCG_OPTS, CPU_OPTS,
# ICBISYNC, RETVERIFY…) ; une variable POMPPC_GL_* (drapeau du plugin) est en
# plus transmise au jeu par `matrice.py --env` (30/09). Résultats : <dépôt principal>/bench/tcg/ab/CAMPAGNE/
# <mode>-<k>/ (tour de matrice, run_tiger.log = stderr de QEMU), bilan
# bench/tcg/ab/CAMPAGNE/bilan.txt (tools/tcg/matabsum.py).
# MATRICE_OPTS="--sample-hote 30" : options de plus pour matrice.py (profil hôte ;
# sous Linux, avec "mode:EXTRA_ARGS=-perfmap" pour nommer le code JIT).
# MATAB_AVANT="commande" : barrière lancée avant chaque partie, VM arrêtée (attendre l'hôte au
# repos : tools/tcg/chargehote.py attendre) ; un code non nul arrête la campagne.
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

start_vm() { # start_vm JOURNAL [VAR=v …]  (les parties : fenêtre QEMU native, comme la matrice)
  local log=$1; shift
  ( cd "$R" && env POMPPC_FRONTEND="${MATAB_FRONTEND:-native}" "$@" nohup ./run_tiger.sh > "$log" 2>&1 & )
  sleep 20
  wait_ssh || { python3 "$WT/tools/tcg/hmp.py" "$(mon)" system_reset >/dev/null 2>&1; wait_ssh; }
}

if [ "${1:-}" = "--restore" ]; then
  stop_vm
  # 31/10 : le binaire et les leviers d'un bras (QEMU_BIN, QEMU_FAST, TCGCOLD…) hérités de
  # l'appelant ne doivent pas suivre la VM quotidienne ; le binaire lancé est affiché.
  # (28/10 13 h 40 : chaine4.sh tué en pleine partie ref-3, la VM qemu-ceil/build/ de la
  # partie est restée allumée et le --restore final n'a jamais tourné.)
  unset QEMU_BIN QEMU_FAST USER_QEMU_BIN TCG_OPTS CPU_OPTS EXTRA_ARGS TCGCOLD
  # rendue comme ./run_tiger.sh la lance : ImGuiDock (demande de l'utilisateur, 26/10)
  MATAB_FRONTEND=imgui start_vm "$R/.run/run_tiger-zen.log" &&
    echo "VM quotidienne relancée dans ImGuiDock (binaire et défauts du lanceur)"
  b=$(grep -a -o '▶ Tiger (QEMU [^)]*)' "$R/.run/run_tiger-zen.log" | tail -1)
  echo "  ${b:-binaire non relevé (voir .run/run_tiger-zen.log)}"
  case "$(uname -sm):$b" in
    "Linux x86_64:"*rapide*|Darwin*) ;;
    *) echo "⚠  la VM quotidienne ne tourne pas sur le binaire rapide : voir les ⚠ de .run/run_tiger-zen.log" ;;
  esac
  exit 0
fi

# Plus de deux modes : chaque argument qui contient « : » en est un ; les deux
# suivants éventuels sont JEU et MODE. Ordre par tour : A B C… puis …C B A.
C=$1; N=$2; shift 2
MODES=()
while [ $# -gt 0 ] && [[ "$1" == *:* ]]; do MODES+=("$1"); shift; done
JEU=${1:-d3}; MODE=${2:-fen}
OUT=$R/bench/tcg/ab/$C; mkdir -p "$OUT"
QB="${QEMU_BIN:-}"
ordre=()
for k in $(seq 1 "$N"); do
  if [ $((k % 2)) = 1 ]; then
    for i in "${!MODES[@]}"; do ordre+=("$i:$k"); done
  else
    for ((i = ${#MODES[@]} - 1; i >= 0; i--)); do ordre+=("$i:$k"); done
  fi
done
echo "campagne $C : ${MODES[*]}, QEMU_BIN='$QB', $JEU $MODE, $N parties par mode" | tee -a "$OUT/journal.txt"
for p in "${ordre[@]}"; do
  m=${p%%:*}; k=${p#*:}
  nom=${MODES[$m]%%:*}; vars=${MODES[$m]#*:}
  D="$OUT/$nom-$k"; mkdir -p "$D"
  echo "$(date +%H:%M:%S) $nom-$k : $vars" | tee -a "$OUT/journal.txt"
  stop_vm
  # MATAB_AVANT="commande" : barrière avant chaque partie, VM arrêtée (p. ex. attendre l'hôte
  # au repos) ; un code non nul arrête la campagne (06/10 : une autre session occupait l'hôte)
  if [ -n "${MATAB_AVANT:-}" ]; then
    bash -c "$MATAB_AVANT" 2>&1 | tee -a "$OUT/journal.txt"
    [ "${PIPESTATUS[0]}" = 0 ] || { echo "$(date +%H:%M:%S) barrière refusée : campagne arrêtée" | tee -a "$OUT/journal.txt"; exit 1; }
  fi
  # QEMU_FAST=0 d'abord : sans QEMU_BIN, un bras tourne sur le binaire de RÉFÉRENCE
  # (build/), pas sur le binaire rapide que run_tiger.sh préfère sur le PC ;
  # un bras peut le redemander ("fast:QEMU_FAST=1" ou "fast:QEMU_BIN=…").
  # shellcheck disable=SC2086
  set -- QEMU_FAST=0 ${QB:+QEMU_BIN=$QB} $vars
  start_vm "$D/run_tiger.log" "$@" || { echo "  pas de ssh" | tee -a "$OUT/journal.txt"; continue; }
  grep -h '▶ Tiger' "$D/run_tiger.log" | tee -a "$OUT/journal.txt"
  # bureau au repos (Finder, Dock, mds) : on attend que le QEMU retombe sous 0,25 cœur
  # (au plus 60 s, l'ancienne attente fixe) ; MATAB_REPOS=fixe rend les 60 s d'avant
  qpid=$(pgrep -f "qemu-system.*disks/tiger.qcow2" | head -1)
  if [ "${MATAB_REPOS:-}" = fixe ] || [ -z "$qpid" ]; then
    sleep 60
  else
    python3 "$WT/tools/tcg/chargehote.py" invite "$qpid" "$OUT/charge-invite.txt" 60 | tee -a "$OUT/journal.txt"
  fi
  # 30/09 : une variable POMPPC_GL_* du mode vise le plugin, donc le JEU dans
  # l'invité : elle passe aussi en --env à la matrice (sans effet sur QEMU)
  genv=()
  for v in "$@"; do case $v in POMPPC_GL_*=*) genv+=(--env "$v") ;; esac; done
  # MATAB_VIDAGE=1 : tour complet avec vidage et preuve d'image (non-régression
  # d'une configuration sur plusieurs jeux, p. ex. JEU=mb,zen,ut,d3), pas un A/B de vitesse
  vid=--sans-vidage; [ "${MATAB_VIDAGE:-0}" != 0 ] && vid=
  env "$@" python3 "$WT/tools/matrice/matrice.py" -j "$JEU" -m "$MODE" $vid \
      ${genv[@]+"${genv[@]}"} ${MATRICE_OPTS:-} --sortie "$D" > "$D/matrice.log" 2>&1
  tail -1 "$D/resultats.csv" 2>/dev/null | tee -a "$OUT/journal.txt"
done
stop_vm
python3 "$WT/tools/tcg/matabsum.py" "$OUT" | tee "$OUT/bilan.txt"
