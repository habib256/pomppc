#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# lmwvectorproof.sh <arbre-QEMU> — preuve hôte des copies RAM vectorielles de
# lmw/stmw (target/ppc/lmw-vector.h de l'arbre, inclus tel quel) : NEON
# (tcg/0025) sur arm64, SSE2 (tcg/0029) sur x86-64. Compile
# tools/tcg/lmwvectorproof.c pour des GPR de 32 puis de 64 bits et le lance.
# Contre-épreuve x86-64 : lmwvectorproof-x86-mut.sh.
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
