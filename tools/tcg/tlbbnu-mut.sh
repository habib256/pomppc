#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# tlbbnu-mut.sh DOSSIER-DES-MUTANTS [NOM…] — tools/tcg/tlbbnu.sh (x-tlb-base en mode
# superviseur, docs/tcg-g4.md §41.3) sur chaque binaire mut-<NOM> laissé par
# tools/tcg/tlbb-mut.py (défaut : tous) ; une ligne par mutant : bilan et mots en écart.
set -uo pipefail
D="${1:?dossier des binaires mut-*}"; shift
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ $# -gt 0 ]; then noms=("$@"); else
  noms=(); for b in "$D"/mut-*; do noms+=("${b##*/mut-}"); done
fi
for m in "${noms[@]}"; do
  r="$(timeout 150 "$HERE/tlbbnu.sh" "$D/mut-$m" x-tlb-base=on 2>&1 | grep -E 'tlbbnu :|ÉCART' |
       sed -e 's/  */ /g' -e 's/ ÉCART (attendu \([0-9a-f]*\))/≠\1/' | tr '\n' ';')"
  printf '%-20s %s\n' "$m" "${r:-tlbbnu : pas de bilan (QEMU tombé ou bloqué)}"
done
