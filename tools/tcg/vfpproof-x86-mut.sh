#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vfpproof-x86-mut.sh [arbre-QEMU] [vecteurs] — contre-épreuve de patches/tcg/0020 :
# chaque mutation des voies AVX/FMA3 de vfp_add4/vfp_fma4 doit faire échouer
# vfpproof.sh (docs/tcg-g4.md §26). Seules les mutations qui RELÂCHENT une
# condition comptent : une condition durcie n'envoie que plus de voies au
# chemin lent, résultat inchangé (mutant équivalent, indétectable par principe).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$HOME/src/qemu11}"
N="${2:-200000}"
W="${TMPDIR:-/tmp}/vfpmut.$$"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/target/ppc"
ln -s "$SRC/build" "$W/build"
fail=0
mut() {   # nom, expression sed
  sed "$2" "$SRC/target/ppc/int_helper.c" > "$W/target/ppc/int_helper.c"
  if cmp -s "$SRC/target/ppc/int_helper.c" "$W/target/ppc/int_helper.c"; then
    echo "✘ mutation « $1 » sans effet sur le source"; fail=1; return
  fi
  if out="$("$HERE/vfpproof.sh" "$W" "$N" 2>/dev/null)"; then
    echo "✘ mutation « $1 » NON détectée"; fail=1
  else
    echo "✔ « $1 » détectée : $(echo "$out" | grep -o '[0-9]* divergences —' )"
  fi
}
mut "tiny < FLT_MIN au lieu de ≤"     's/_mm_set1_epi32(0x00800001)/_mm_set1_epi32(0x00800000)/'
mut "add : |r| ≤ FLT_MIN admis"         's/_mm_andnot_si128(both_zero, vfp_x86_tiny(ar))/_mm_setzero_si128()/'
mut "add : un seul zéro exempte"        's/_mm_or_si128(aa, ab),$/_mm_and_si128(aa, ab),/'
mut "fma : addende nul exempte aussi"   's/_mm_cmpeq_epi32(ac, _mm_setzero_si128()));/_mm_cmpeq_epi32(_mm_and_si128(ac, vfp_x86_abs(ib)), _mm_setzero_si128()));/'
mut "fma : overflow oublié"            '/vfp_fma4_fma3/,/^}/s/float_raise(float_flag_overflow, s);/;/'
mut "dénormaux admis"                  's/return _mm_or_si128(den, /return (void)den, (/'
mut "infinis admis"                    's/_mm_set1_epi32(0x7f7fffff)/_mm_set1_epi32(0x7fffffff)/'
mut "vnmsubfp : résultat non nié"      's/_mm_xor_si128(_mm_castps_si128(vr), nres)/_mm_castps_si128(vr)/'
mut "vnmsubfp : addende non nié"       's/_mm_xor_si128(ib, nc)/ib/'
exit $fail
