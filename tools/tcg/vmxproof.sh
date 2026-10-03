#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vmxproof.sh [arbre-QEMU] [N] [mut] — preuve hôte de patches/tcg/0021-ppc-vmx-inline.patch
# (x-vmx-inline, docs/tcg-g4.md §28). Extrait TELS QUELS helper_vsldoi, VMRG_DO
# (int_helper.c) et STVE (mem_helper.c) de l'arbre (défaut ~/src/qemu-d3tcg),
# vérifie que vmx-impl.c.inc émet bien les ops du modèle de vmxproof.c, compile
# et compare. Avec « mut » : les six mutations du modèle, chacune doit diverger.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$HOME/src/qemu-d3tcg}"
N="${2:-1000000}"
OUT="${TMPDIR:-/tmp}/vmxproof.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
I="$SRC/target/ppc/int_helper.c"
M="$SRC/target/ppc/mem_helper.c"
V="$SRC/target/ppc/translate/vmx-impl.c.inc"
{
  awk '/^void helper_vsldoi\(/ {on=1} on {print} on && /^}/ {exit}' "$I"
  sed -n '/^#define VMRG_DO/,/^#undef VMRG$/p' "$I" | grep -v '^VMRG(b\|^VMRG(h'
  sed -n '/^#define STVE(name/,/^STVE(STVEWX/p' "$M"
  echo '#undef I'
} > "$OUT/vmxproof-helpers.h"
grep -q 'helper_vsldoi' "$OUT/vmxproof-helpers.h" && grep -q 'STVE(STVEWX' "$OUT/vmxproof-helpers.h" ||
  { echo "helpers introuvables dans $SRC" >&2; exit 2; }
# les ops de la traduction en ligne, telles que le modèle les écrit
while IFS= read -r l; do
  [ -z "$l" ] && continue
  grep -qF -- "$l" "$V" || { echo "✘ op absente de vmx-impl.c.inc : $l" >&2; exit 3; }
done <<'OPS'
int vsh = VSH(ctx->opcode) & 0xf, q = vsh >> 3, s = (vsh & 7) * 8;
int n = s ? q + 3 : q + 2;
get_avr64(w[i], i < 2 ? rA(ctx->opcode) : rB(ctx->opcode),
tcg_gen_extract2_i64(rh, w[q + 1], w[q], 64 - s);
tcg_gen_extract2_i64(rl, w[q + 2], w[q + 1], 64 - s);
get_avr64(a, rA(ctx->opcode), high);
get_avr64(b, rB(ctx->opcode), high);
tcg_gen_shri_i64(rh, b, 32);
tcg_gen_deposit_i64(rh, a, rh, 0, 32);
tcg_gen_deposit_i64(rl, b, a, 32, 32);
get_avr64(d, a->rt, true);
get_avr64(lo, a->rt, false);
tcg_gen_andi_i64(t, ea, 8);
tcg_gen_movcond_i64(TCG_COND_NE, d, t, tcg_constant_i64(0), lo, d);
tcg_gen_andi_i64(t, ea, 7);
tcg_gen_shli_i64(t, t, 3);
tcg_gen_subfi_i64(t, 64 - 8 * size, t);
tcg_gen_shr_i64(d, d, t);
size == 1 ? MO_UB : size == 2 ? MO_BEUW : MO_BEUL);
OPS
cc -O2 -Wall -I"$OUT" -o "$OUT/vmxproof" "$HERE/vmxproof.c"
"$OUT/vmxproof" "$N"
if [ "${3:-}" = mut ]; then
  for k in 1 2 3 4 5 6; do
    cc -O2 -w -DMUT=$k -I"$OUT" -o "$OUT/vmxmut$k" "$HERE/vmxproof.c"
    if "$OUT/vmxmut$k" 20000 > "$OUT/m$k.txt" 2>/dev/null; then
      echo "✘ mutation $k NON détectée"; exit 4
    fi
    echo "mutation $k détectée : $(grep -v ' 0 divergences' "$OUT/m$k.txt" | tr '\n' ' ')"
  done
fi
