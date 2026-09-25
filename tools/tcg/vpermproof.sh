#!/usr/bin/env bash
# vpermproof.sh [arbre-QEMU] [vecteurs] — preuve hôte de patches/tcg/0004-ppc-vperm-fast.patch.
# Extrait helper_VPERM et helper_VPERM_FAST TELS QUELS de target/ppc/int_helper.c
# (défaut ~/src/qemu-tcg19), compile tools/tcg/vpermproof.c contre eux et le lance.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$HOME/src/qemu-tcg19}"
N="${2:-50000000}"
OUT="${TMPDIR:-/tmp}/vpermproof.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
F="$SRC/target/ppc/int_helper.c"
{
  awk '/^void helper_VPERM\(/ {on=1} on {print} on && /^}/ {exit}' "$F"
  sed -n '/vperm-fast: début/,/vperm-fast: fin/p' "$F"
} > "$OUT/vpermproof-helpers.h"
grep -q "helper_VPERM_FAST" "$OUT/vpermproof-helpers.h" || { echo "arbre sans le patch 0004 ($SRC)" >&2; exit 2; }
cc -O2 -Wall -I"$OUT" -o "$OUT/vpermproof" "$HERE/vpermproof.c"
time "$OUT/vpermproof" "$N"
