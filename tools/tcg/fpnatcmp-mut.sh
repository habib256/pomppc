#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# fpnatcmp-mut.sh [ARBRE-USER] [NRAND] [OUT] — contre-épreuve de
# patches/tcg/0033 (x-fp-native-cmp, docs/tcg-g4.md §33) : pendant x86_64 de
# fpnat-mut.sh. Une mutation de l'émetteur RÉEL (tcg/x86_64/tcg-target.c.inc)
# ou de la traduction (fsel) à la fois, qemu-ppc linux-user reconstruit dans
# ARBRE-USER (copie de l'arbre patché, voir fpnatcmp-user.sh), puis le job
# fptest en mode c sous x-fp-native-cmp, comparé à la référence (helpers) et
# sous x-fp-native-cmp-verify : chaque mutant doit être DÉTECTÉ (sortie
# différente ou divergences du vérificateur). L'arbre est rendu intact ensuite.
set -uo pipefail
SRC="${1:-$HOME/src/qemu-fpu}"
N="${2:-16384}"
OUT="${3:-${TMPDIR:-/tmp}/fpnatcmp-mut}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
B="$SRC/build-user"
mkdir -p "$OUT"
powerpc-linux-gnu-gcc -O2 -Wall -static -mcpu=7400 -Wa,-mregnames \
    -o "$OUT/fptest.ppc" "$HERE/../guest/jobs/fptest/fptest.c" || exit 2
BASE="g4,x-fast-fp=on,x-fp-inline=on,x-fp-native=on,x-fp-native64=on"
( cd "$B" && nice ninja qemu-ppc > /dev/null ) || exit 2
"$B/qemu-ppc" -cpu "$BASE" "$OUT/fptest.ppc" c "$N" > "$OUT/ref.txt" || exit 2
n=0; det=0
while IFS='|' read -r f m; do
  [ -z "$f" ] && continue
  case "$f" in \#*) echo "$f"; continue ;; esac
  n=$((n + 1))
  cp "$SRC/$f" "$OUT/orig"
  sed -i -e "$m" "$SRC/$f"
  if cmp -s "$SRC/$f" "$OUT/orig"; then
    echo "mut$n : SANS EFFET ($m)"; cp "$OUT/orig" "$SRC/$f"; continue
  fi
  if ! ( cd "$B" && nice ninja qemu-ppc > "$OUT/mut$n.build" 2>&1 ); then
    echo "mut$n : construction ratée"; cp "$OUT/orig" "$SRC/$f"; continue
  fi
  "$B/qemu-ppc" -cpu "$BASE,x-fp-native-cmp=on" "$OUT/fptest.ppc" c "$N" \
      > "$OUT/mut$n.txt" 2> "$OUT/mut$n.err"
  "$B/qemu-ppc" -cpu "$BASE,x-fp-native-cmp=on,x-fp-native-cmp-verify=on" \
      "$OUT/fptest.ppc" c "$N" > "$OUT/mut$n-v.txt" 2> "$OUT/mut$n-v.err"
  nd=$(grep -c DIVERGENCE "$OUT/mut$n-v.err")
  ops=$(diff "$OUT/ref.txt" "$OUT/mut$n.txt" | grep '^>' | awk '{print $3}' | sort -u | tr '\n' ' ')
  if ! cmp -s "$OUT/ref.txt" "$OUT/mut$n.txt" || [ "$nd" -gt 0 ]; then
    det=$((det + 1)); echo "mut$n : DÉTECTÉ — sortie différente pour : ${ops:-aucune} ; $nd divergences ($m)"
  else
    echo "mut$n : NON DÉTECTÉ ($m)"
  fi
  cp "$OUT/orig" "$SRC/$f"
done <<'MUT'
# 1. frsp : borne basse retirée (|frB| < FLT_MIN accepté : dénormal, sous-dépassement)
tcg/x86_64/tcg-target.c.inc|s/        tcg_out_movi(s, TCG_TYPE_I64, u, 0x381ull << 53);/        tcg_out_movi(s, TCG_TYPE_I64, u, 0x001ull << 53);/
# 2. frsp : borne haute à 2^128 (FLT_MAX + 1/2 ulp et au-delà acceptés : OX manqué)
tcg/x86_64/tcg-target.c.inc|s/0x47effffff0000000ull << 1);/0x47f0000000000000ull << 1);/
# 3. fctiw tronque au lieu d'arrondir
tcg/x86_64/tcg-target.c.inc|s/op == PFP_FCTIWZ ? PFP_VCVTTSD2SI : PFP_VCVTSD2SI/PFP_VCVTTSD2SI/
# 4. fctiw(z) : pas de test de plage int32 (VXCVI manqué)
tcg/x86_64/tcg-target.c.inc|s/        pfp_cmpr(s, t, xr);/        pfp_cmpr(s, t, t);/
# 5. fctiw(z) : FI non posé
tcg/x86_64/tcg-target.c.inc|/FI = 1, FPRF untouched/{n;n;s/PFP_FI, 0);/0, 0);/}
# 6. fdiv(s) : diviseur nul accepté (0/0 : NaN, VXZDZ manqué)
tcg/x86_64/tcg-target.c.inc|/a zero divisor: ZX or VXZDZ/{n;n;d}
# 7. fdiv(s) : quotient non testé (minuscule, infini acceptés)
tcg/x86_64/tcg-target.c.inc|s/            if (base != PFP_B_DIV) {/            if (true) {/
# 8. fcmpo/fcmpu : opérande NaN de frB accepté
tcg/x86_64/tcg-target.c.inc|/without a NaN is fcmpu/,/pfp_cmpr(s, xnf, u);/{s/pfp_cmpr(s, xnf, u);/pfp_cmpr(s, u, u);/}
# 9. fsel : NaN non testé (traduction, ops TCG)
target/ppc/translate/fp-impl.c.inc|s/tcg_constant_i64(0x7ff0000000000000ull), t1, r);/tcg_constant_i64(0xfff0000000000000ull), t1, r);/
MUT
( cd "$B" && nice ninja qemu-ppc > /dev/null 2>&1 ) && echo "arbre rendu intact, qemu-ppc reconstruit"
echo "$det mutants détectés sur $n"
