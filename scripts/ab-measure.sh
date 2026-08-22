#!/usr/bin/env bash
# ab-measure.sh — A/B interleavé entre deux binaires QEMU, protocole du README.
#
#   ./scripts/ab-measure.sh <binA> <binB> [n_paires]     # n_paires: 4 par défaut
#
# Alterne A/B/A/B… (interleave) pour annuler tout biais d'ordre ou de dérive
# thermique, force `-snapshot` (sinon un fsck sur volume HFS+ sale ajoute ~35 s
# de façon intermittente), et affiche la MÉDIANE de CPU_qemu par binaire.
#
# Chaque run passe par scripts/measure-boot.sh, donc par son pré-vol : la
# campagne s'arrête d'elle-même si l'hôte n'est pas au repos. C'est voulu — une
# mesure sous charge est pire qu'une absence de mesure, parce qu'elle a l'air
# d'un résultat.
#
# Exemple (le cas qui a motivé ce script) : vérifier qu'ajouter le device
# Screamer au macio ne coûte rien au boot.
#   ./scripts/ab-measure.sh /tmp/qemu-no-screamer /tmp/qemu-with-screamer 4
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

A="${1:-}"; B="${2:-}"; N="${3:-4}"
[ -x "$A" ] && [ -x "$B" ] || {
  echo "usage: $0 <binA> <binB> [n_paires]" >&2; exit 2; }

median() { # médiane d'une liste de flottants sur stdin
  sort -g | awk '{v[NR]=$1} END {
    if (NR==0) {print "?"; exit}
    print (NR%2) ? v[(NR+1)/2] : (v[NR/2]+v[NR/2+1])/2 }'
}

declare -a RA=() RB=()
for i in $(seq 1 "$N"); do
  for side in A B; do
    bin=$([ "$side" = A ] && echo "$A" || echo "$B")
    out=$(QEMU_BIN="$bin" EXTRA_ARGS="-snapshot" \
          "$ROOT/scripts/measure-boot.sh" 300 2>&1)
    rc=$?
    cpu=$(printf '%s\n' "$out" | sed -n 's/.*CPU_qemu=\([0-9.]*\)s.*/\1/p' | head -1)
    if [ "$rc" -ne 0 ] || [ -z "$cpu" ]; then
      echo "paire $i / $side : ÉCHEC" >&2
      printf '%s\n' "$out" | tail -3 | sed 's/^/    /' >&2
      exit 1
    fi
    echo "  paire $i  $side  CPU_qemu=${cpu}s  ($(basename "$bin"))"
    [ "$side" = A ] && RA+=("$cpu") || RB+=("$cpu")
  done
done

MA=$(printf '%s\n' "${RA[@]}" | median)
MB=$(printf '%s\n' "${RB[@]}" | median)
echo
echo "=== médianes sur $N paires ==="
printf "  A  %-40s %ss\n" "$(basename "$A")" "$MA"
printf "  B  %-40s %ss\n" "$(basename "$B")" "$MB"
echo "  delta B-A : $(echo "$MB - $MA" | bc -l)s  ($(echo "scale=1; 100*($MB-$MA)/$MA" | bc -l) %)"
