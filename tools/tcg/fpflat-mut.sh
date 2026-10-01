#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# fpflat-mut.sh [arbre-QEMU] [vecteurs] — contre-épreuve de x-fp-flat
# (patches/tcg/0013, docs/tcg-g4.md §22) : chaque mutation de
# helper_fp32_flat/helper_fcmpu_flat (copie de fpu_helper.c recompilée,
# FLATMUT de fpproof.sh) doit être DÉTECTÉE (divergences « plat » > 0).
# BUILD=… comme pour fpproof.sh.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$HOME/src/qemu-fpnat}"
N="${2:-200000}"
fail=0
while IFS= read -r m; do
  [ -z "$m" ] && continue
  case "$m" in \#*) echo "$m"; continue ;; esac
  line="$(FLATMUT="$m" bash "$HERE/fpproof.sh" "$SRC" "$N" 2>&1 | grep -E '^x-fp-flat|mutation|mutant')"
  printf '%s\n    %s\n' "$m" "$line"
  case "$line" in *" 0 divergences"*|*mutation*|*mutant*) echo "    ⚠ NON DÉTECTÉE"; fail=1 ;; esac
done <<'MUT'
# frT pas écrit avant float_check_status (exception différée)
s/\*cpu_fpr_ptr(env, frt) = r;/;/
# séquence d'origine sans reset_fpstatus
/^Int128 fp32_flat(/,/^}/s/^    helper_reset_fpstatus(env);$/    ;/
# fmuls lent sur b au lieu de c
s/r = float64r32_mul(a, c, \&env->fp_status);/r = float64r32_mul(a, b, \&env->fp_status);/
# chemin court sans les drapeaux « inexact »
/^Int128 helper_fp32_flat/,/^}/s/set_float_exception_flags(float_flag_inexact, \&env->fp_status);/;/
# chemin court sans la porte
s/    if (likely(fpi_gate(fpscr) \&\& !env->fp_verify)) {/    if (likely(!env->fp_verify)) {/
# fcmpu lent sans FI
/^Int128 fcmpu_flat(/,/^}/s/do_float_check_status(env, true, ra);/do_float_check_status(env, false, ra);/
# fnmsubs lent avec les drapeaux de fnmadds
s/return do_fmadds(env, a, c, b, NMSUB_FLGS, ra);/return do_fmadds(env, a, c, b, NMADD_FLGS, ra);/
# FPSCR rendu : l'ancien au lieu de celui de la séquence d'origine
s/return int128_make128(r, env->fpscr);/return int128_make128(r, fpscr);/
# fcmpu court : CR rendu faux pour l'égalité
s/return int128_make128(crf, fpi_fpscr_fcmpu(fpscr, crf));/return int128_make128(crf == 2 ? 4 : crf, fpi_fpscr_fcmpu(fpscr, crf));/
# fcmpu lent : CR lu avant la comparaison
s/return int128_make128(env->crf\[bf\], env->fpscr);/return int128_make128(fpi_fcmpu(fpscr, a, b), env->fpscr);/
MUT
exit $fail
