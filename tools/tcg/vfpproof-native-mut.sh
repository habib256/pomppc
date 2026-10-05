#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Mutate the native model. A compilation failure is not a detected mutant.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:?arbre QEMU construit requis}"
N="${2:-200000}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
fail=0
for mut in 1 2 3 4 5 6 7 8; do
    rc=0
    VFPPROOF_NATIVE=1 VFPPROOF_MUT="$mut" "$HERE/vfpproof.sh" "$SRC" "$N" \
        > "$OUT/$mut.log" 2>&1 || rc=$?
    if [ "$rc" = 1 ] && grep -Eq 'total : .* [1-9][0-9]* divergences — ÉCHEC' "$OUT/$mut.log"; then
        echo "mutant $mut détecté"
    else
        echo "mutant $mut NON détecté (rc=$rc)"
        cat "$OUT/$mut.log"
        fail=1
    fi
done
exit "$fail"
