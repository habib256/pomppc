#!/bin/bash
# GPL3 - Copyleft VERHILLE Arnaud
# ab-geometrie.sh CAMPAGNE N [JEUX] [MODES] [VARIABLES_B] — A/B ENTRELACÉ du
# volet géométrie d'A4 (docs/protocole-v23-geometrie.md) sur la VM QUOTIDIENNE.
#
# Même binaire QEMU (celui qui annonce QGPU_CAP_GEOM_HOST) et même plugin dans
# les deux modes ; seul change l'environnement du jeu, passé par
# `matrice.py --env` :
#   ref : rien (voies d'avant : raw_fix_nan sur le G4, déroulage de GLEngine)
#   geo : VARIABLES_B, défaut « POMPPC_GL_RAWSANE=1 POMPPC_GL_NATSHM=1 »
# Chaque partie relance QEMU (comme tools/tcg/matab.sh), attend le bureau, joue
# les cellules JEUX × MODES sans vidage. Ordre ref geo geo ref ref geo … (N
# parties par mode). Puis UN tour de justesse par mode, AVEC vidage (image
# jugée par la matrice : rejeu = VM et référence validée).
#
#   QEMU_BIN=~/src/qemu-a4geo/build/qemu-system-ppc \
#     tools/matrice/ab-geometrie.sh geo1 3 nx,wc3,mb,zen fen,pe
#   tools/matrice/ab-geometrie.sh geo1 3 nx fen "POMPPC_GL_RAWSANE=1"   # un seul levier
#
# Préalables (intégrateur) : QEMU_BIN construit avec patches/qgpu de cette
# branche (qgpu_proto.h ⇒ QEMU + plugin, cycle.sh NORUN=1) ; plugin de cette
# branche installé dans l'invité. Sans QGPU_CAP_GEOM_HOST les deux variables
# sont sans effet (ligne « rawsane=0 natshm=0 » dans note.txt) : l'A/B ne
# mesurerait rien, le bilan le signale.
# Résultats : <dépôt principal>/bench/matrice/ab-geo-CAMPAGNE/<mode>-<k>/,
# bilan : bilan.txt (tools/matrice/ab-geometrie-bilan.py).
# JAMAIS de suppression de .run/tiger.lock : on attend qu'il se libère.
set -u
R=/Users/mercure/src/pomppc
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
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

start_vm() { # start_vm JOURNAL
  local log=$1
  ( cd "$R" && env POMPPC_FRONTEND=native ${QEMU_BIN:+QEMU_BIN=$QEMU_BIN} nohup ./run_tiger.sh > "$log" 2>&1 & )
  sleep 20
  wait_ssh || { python3 "$WT/tools/tcg/hmp.py" "$(mon)" system_reset >/dev/null 2>&1; wait_ssh; }
}

[ $# -ge 2 ] || { sed -n '3,30p' "$0"; exit 2; }
C=$1; N=$2; JEUX=${3:-nx,wc3,mb,zen}; MODES=${4:-fen,pe}
VB=${5:-POMPPC_GL_RAWSANE=1 POMPPC_GL_NATSHM=1}
OUT=$R/bench/matrice/ab-geo-$C; mkdir -p "$OUT"
envargs() { # envargs MODE → arguments --env de matrice.py
  [ "$1" = geo ] || return 0
  local v
  for v in $VB; do printf -- '--env\n%s\n' "$v"; done
}
partie() { # partie MODE K [--sans-vidage]
  local m=$1 k=$2 D="$OUT/$1-$2" args=()
  mkdir -p "$D"
  while IFS= read -r a; do args+=("$a"); done < <(envargs "$m")
  echo "$(date +%H:%M:%S) $m-$k ${args[*]:-}" | tee -a "$OUT/journal.txt"
  stop_vm
  start_vm "$D/run_tiger.log" || { echo "  pas de ssh" | tee -a "$OUT/journal.txt"; return; }
  grep -h '▶ Tiger\|GEOM\|caps' "$D/run_tiger.log" | head -3 | tee -a "$OUT/journal.txt"
  sleep 60                                  # bureau au repos (Finder, Dock, mds)
  python3 "$WT/tools/matrice/matrice.py" -j "$JEUX" -m "$MODES" ${3:-} \
      ${args[@]+"${args[@]}"} --sortie "$D" > "$D/matrice.log" 2>&1
  tail -n +2 "$D/resultats.csv" 2>/dev/null | cut -d, -f1-3,8 | tee -a "$OUT/journal.txt"
}

echo "campagne $C : ref / geo='$VB', QEMU_BIN='${QEMU_BIN:-}', $JEUX × $MODES, $N parties par mode" \
  | tee -a "$OUT/journal.txt"
for k in $(seq 1 "$N"); do
  if [ $((k % 2)) = 1 ]; then partie ref "$k" --sans-vidage; partie geo "$k" --sans-vidage
  else partie geo "$k" --sans-vidage; partie ref "$k" --sans-vidage; fi
done
# justesse : un tour AVEC vidage par mode (image jugée contre la référence)
partie ref juste
partie geo juste
stop_vm
python3 "$WT/tools/matrice/ab-geometrie-bilan.py" "$OUT" | tee "$OUT/bilan.txt"
bash "$WT/tools/tcg/matab.sh" --restore
