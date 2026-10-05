#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:?arbre QEMU avec tcg/0025 requis}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
for bits in 32 64; do
    cc -O2 -Wall -Wextra -Werror -DTARGET_LONG_BITS="$bits" \
       -I"$SRC/target/ppc" "$HERE/lmwvectorproof.c" -o "$OUT/proof$bits"
    "$OUT/proof$bits"
done
