#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vfpcmpproof-mut.sh [arbre-QEMU] [vecteurs] — contre-épreuve de vfpcmpproof.sh
# (tcg/0034) : chaque mutation du modèle extrait doit être DÉTECTÉE.
# Le modèle compilé est celui de l'hôte : mutants SSE/AVX sur x86-64, mutants
# NEON (tcg/0038, docs/parite-arm64-0031-0035.md) sur aarch64.
# MUT_LIST=1 : imprime seulement la liste de l'hôte (MUT_ARCH=x86_64|arm64
# pour en choisir une autre ; sert à tools/tcg/vfncxcheck.sh sur le PC).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$HOME/src/qemu-fp}"
N="${2:-2000}"
mutants() {
  case "${MUT_ARCH:-$(uname -m)}" in
    arm64|aarch64) cat <<'MUT'
# NEON : dénormaux acceptés par les comparaisons
s/    uint32x4_t d = vorrq_u32(vcgeq_u32(t, vdupq_n_u32(0x01000000)),/    uint32x4_t d = vorrq_u32(vdupq_n_u32(0xffffffff),/
# NEON : NaN accepté par les comparaisons
s/return vandq_u32(d, vcgeq_u32(vdupq_n_u32(0xff000000), t));/return d;/
# NEON : vcmpgefp strict
s/r = vcgeq_f32(a, b);/r = vcgtq_f32(a, b);/
# NEON : vcmpbfp contre b au lieu de -b
s/vandq_u32(vcgtq_f32(vnegq_f32(b), a),/vandq_u32(vcgtq_f32(b, a),/
# vcf* sans porte
s/        if (!gate) {/        if (0) {/
# NEON : vcfux par scvtf (entier signé)
s/: vcvtq_f32_u32(ib);/: vcvtq_f32_s32(vreinterpretq_s32_u32(ib));/
# NEON : vctsxs, saturation basse à -2^31 inclus
s/lo = vcgtq_f32(vdupq_n_f32(-2147483648.0f), x);/lo = vcgeq_f32(vdupq_n_f32(-2147483648.0f), x);/
# NEON : vctuxs, saturation basse dès -0,5
s/lo = vcgeq_f32(vdupq_n_f32(-1.0f), x);/lo = vcgeq_f32(vdupq_n_f32(-0.5f), x);/
# NEON : vctsxs, SAT haut oublié
s/hi = vcgeq_f32(x, vfnc_pow2(31));/hi = vdupq_n_u32(0);/
# NEON : vctsxs par fcvtzu
s/r = vreinterpretq_u32_s32(vcvtq_s32_f32(x));/r = vcvtq_u32_f32(x);/
MUT
      ;;
    *) cat <<'MUT'
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
      ;;
  esac
}
if [ -n "${MUT_LIST:-}" ]; then mutants; exit 0; fi
fail=0
while IFS= read -r m; do
  [ -z "$m" ] && continue
  case "$m" in \#*) continue ;; esac
  line="$(MUTATE="$m" bash "$HERE/vfpcmpproof.sh" "$SRC" "$N" 2>/dev/null | grep '^total')"
  printf '%s\n    %s\n' "$m" "$line"
  case "$line" in *" 0 divergences"*|"") echo "    ⚠ NON DÉTECTÉE"; fail=1 ;; esac
done < <(mutants)
exit $fail
