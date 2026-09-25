#!/bin/bash
# mbreport.sh [dossier] — bilan des A/B Marble Blast de tools/tcg/mbab.sh :
# pour chaque couple de configurations, toutes les passes de toutes les
# manches d'un côté (fichiers <manche>-mb-<config>-<n>.txt) contre l'autre.
#   mbreport.sh bench/tcg/res "s2off s2on" "s1off s1on" "s2off s1off" "s2on s1on"
# « r2:s1off » ne prend que la manche r2 (fichiers r2-mb-s1off-<n>.txt).
D="${1:-bench/tcg/res}"; shift
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
files() {
  case "$1" in
    *:*) ls "$D/${1%%:*}"-mb-"${1#*:}"-[0-9].txt 2>/dev/null | paste -sd, - ;;
    *)   ls "$D"/*-mb-"$1"-[0-9].txt 2>/dev/null | paste -sd, - ;;
  esac
}
for couple in "$@"; do
  read -r a b <<< "$couple"
  echo "== $a → $b"
  python3 "$HERE/mbpair.py" "$(files "$a")" "$(files "$b")" | tail -5
done
