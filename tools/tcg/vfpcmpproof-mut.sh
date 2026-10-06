#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vfpcmpproof-mut.sh [arbre-QEMU] [vecteurs] — contre-épreuve de vfpcmpproof.sh
# (tcg/0034) : chaque mutation du modèle extrait doit être DÉTECTÉE.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$HOME/src/qemu-fp}"
N="${2:-2000}"
fail=0
while IFS= read -r m; do
  [ -z "$m" ] && continue
  case "$m" in \#*) continue ;; esac
  line="$(MUTATE="$m" bash "$HERE/vfpcmpproof.sh" "$SRC" "$N" 2>/dev/null | grep '^total')"
  printf '%s\n    %s\n' "$m" "$line"
  case "$line" in *" 0 divergences"*|"") echo "    ⚠ NON DÉTECTÉE"; fail=1 ;; esac
done <<'MUT'
# dénormaux acceptés par les comparaisons
s/return _mm_andnot_si128(den, okn);/return okn;/
# NaN accepté par les comparaisons
s/return _mm_andnot_si128(den, okn);/return _mm_andnot_si128(den, _mm_set1_epi32(-1));/
# vcmpgefp strict
s/r = _mm_castps_si128(_mm_cmp_ps(b, a, _CMP_LE_OS));/r = _mm_castps_si128(_mm_cmp_ps(b, a, _CMP_LT_OS));/
# vcf* sans porte
s/        if (!gate) {/        if (0) {/
# vcfux : moitié haute à 2^15
s/vfnc_pow2(16)),/vfnc_pow2(15)),/
# vctsxs : saturation basse à -2^31 inclus
s/_mm_set1_ps(-2147483648.0f),/_mm_set1_ps(-2147483520.0f),/
# vctuxs : saturation basse dès -0,5
s/_mm_set1_ps(-1.0f),/_mm_set1_ps(-0.5f),/
# NaN des conversions non annulé
s/r = _mm_andnot_si128(n, _mm_xor_si128(r, hi));/r = _mm_xor_si128(r, hi);/
MUT
exit $fail
