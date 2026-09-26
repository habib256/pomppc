#!/usr/bin/env bash
# fpproof-mut.sh [arbre-QEMU] [vecteurs] — contre-épreuve de fpproof.sh : chaque
# mutation du bloc « fp-inline » doit être DÉTECTÉE (divergences > 0).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$HOME/src/qemu-fp}"
N="${2:-2000000}"
fail=0
while IFS= read -r m; do
  [ -z "$m" ] && continue
  case "$m" in \#*) continue ;; esac
  line="$(MUTATE="$m" bash "$HERE/fpproof.sh" "$SRC" "$N" 2>/dev/null | grep '^total')"
  printf '%s\n    %s\n' "$m" "$line"
  case "$line" in *" 0 divergences"*) echo "    ⚠ NON DÉTECTÉE"; fail=1 ;; esac
done <<'MUT'
# débordement d'une addition accepté
s/if (isinf(r)) {/if (0) {/
# FLT_MIN exact accepté (sous-dépassement avant arrondi)
s/fabsf(r) <= FLT_MIN))/fabsf(r) < FLT_MIN))/
# fmadds en deux arrondis
s/r = fmaf(fa, fc, fb);/{ volatile float p_ = fa * fc; r = p_ + fb; }/
# porte sans RN
s/return (fpscr \& FPI_GATE_MASK) == FP_XX;/return (fpscr \& (FP_XX | FP_XE | FP_OE | FP_UE)) == FP_XX;/
# clé de fcmpu sans -0 == +0
s/-(int64_t)(x \& INT64_MAX)/(int64_t)(x ^ INT64_MAX)/
# FPRF de -0 faux
s/(neg << 4) | 0x02/(neg << 3) | 0x02/
# exposant sous-normal simple admis
s/exp >= 0x381/exp >= 0x380/
# fcmpu : NaN signalant admis
s/(a \& INT64_MAX) > 0x7ff0000000000000ull/(a \& INT64_MAX) > 0x7ff8000000000000ull/
# tout zéro de fmuls/fmadds accepté (sous-dépassement vers 0 manqué)
s/if (fa != 0 \&\& fc != 0 \&\& (isinf/if (r != 0 \&\& (isinf/
# débordement accepté
s/(isinf(r) || fabsf(r) <= FLT_MIN))/(fabsf(r) <= FLT_MIN))/
MUT
exit $fail
