#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vfpcmp-mut.sh [ARBRE-USER] [NVEC] [OUT] — contre-épreuve de
# patches/tcg/0034 (x-vfp-native-cmp, docs/tcg-g4.md §34) : comme fpnatcmp-mut.sh
# (tcg/0033). Une mutation de l'émetteur RÉEL (tcg/x86_64/tcg-target.c.inc)
# ou de la traduction (CR6) à la fois, qemu-ppc linux-user reconstruit dans
# ARBRE-USER (copie de l'arbre patché, voir vfpcmp-user.sh), puis le job
# vfptest en mode c sous x-vfp-native-cmp, comparé à la référence (helpers) et
# sous x-vfp-native-cmp-verify : chaque mutant doit être DÉTECTÉ (sortie
# différente ou divergences du vérificateur). L'arbre est rendu intact ensuite.
set -uo pipefail
SRC="${1:-$HOME/src/qemu-fpu}"
N="${2:-16384}"
OUT="${3:-${TMPDIR:-/tmp}/vfpcmp-mut}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
B="$SRC/build-user"
mkdir -p "$OUT"
powerpc-linux-gnu-gcc -O2 -maltivec -mabi=altivec -static -mcpu=7400 -Wa,-mregnames \
    -o "$OUT/vfptest.ppc" "$HERE/../guest/jobs/vfptest/vfptest.c" || exit 2
BASE="g4,x-fast-fp=on,x-vfp-fast=on,x-vfp-native=on"
( cd "$B" && nice ninja qemu-ppc > /dev/null ) || exit 2
NVEC=$N "$B/qemu-ppc" -cpu "$BASE" "$OUT/vfptest.ppc" c > "$OUT/ref.txt" || exit 2
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
  NVEC=$N "$B/qemu-ppc" -cpu "$BASE,x-vfp-native-cmp=on" "$OUT/vfptest.ppc" c \
      > "$OUT/mut$n.txt" 2> "$OUT/mut$n.err"
  NVEC=$N "$B/qemu-ppc" -cpu "$BASE,x-vfp-native-cmp=on,x-vfp-native-cmp-verify=on" \
      "$OUT/vfptest.ppc" c > "$OUT/mut$n-v.txt" 2> "$OUT/mut$n-v.err"
  nd=$(grep -c DIVERGENCE "$OUT/mut$n-v.err")
  ops=$(diff "$OUT/ref.txt" "$OUT/mut$n.txt" | grep '^>' | awk '{print $2 $3}' | sort -u | tr '\n' ' ')
  if ! cmp -s "$OUT/ref.txt" "$OUT/mut$n.txt" || [ "$nd" -gt 0 ]; then
    det=$((det + 1)); echo "mut$n : DÉTECTÉ — sortie différente pour : ${ops:-aucune} ; $nd divergences ($m)"
  else
    echo "mut$n : NON DÉTECTÉ ($m)"
  fi
  cp "$OUT/orig" "$SRC/$f"
done <<'MUT'
# 1. comparaisons : dénormaux acceptés (seul le NaN est testé ; NJ les annule)
tcg/x86_64/tcg-target.c.inc|s/    tcg_out_vex_modrm(s, OPC_PANDN, d, d, u);/    tcg_out_vex_modrm(s, OPC_POR, d, u, u);/
# 2. vcmpgefp en « strictement plus grand »
tcg/x86_64/tcg-target.c.inc|s|pvf_cmpps(s, r, xb, xa, PVF_LE_OS);         /\* b <= a \*/|pvf_cmpps(s, r, xb, xa, PVF_LT_OS);|
# 3. vcmpbfp : borne basse contre b au lieu de -b
tcg/x86_64/tcg-target.c.inc|s/tcg_out_vex_modrm(s, PVF_VXORPS, t, xb, c);/tcg_out_mov(s, TCG_TYPE_V128, t, xb);/
# 4. vcfux : moitié haute à 2^15
tcg/x86_64/tcg-target.c.inc|s/pvf_pow2(s, c, 16);/pvf_pow2(s, c, 15);/
# 5. vcf* : porte retirée (inexact de vec_status pas posé)
tcg/x86_64/tcg-target.c.inc|s/    if (kind == PVF_CFUX || kind == PVF_CFSX) {/    if (0) {/
# 6. vctsxs : pas de saturation haute (2^31 -> 0x80000000)
tcg/x86_64/tcg-target.c.inc|s|tcg_out_vex_modrm(s, OPC_PXOR, r, r, hi);   /\* 2^31 -> max \*/|tcg_out_mov(s, TCG_TYPE_V128, r, r);|
# 7. VSCR[SAT] jamais posé
tcg/x86_64/tcg-target.c.inc|s/            tcg_out32(s, 1);/            tcg_out32(s, 0);/
# 8. vctuxs : saturation basse dès -0,5 au lieu de -1
tcg/x86_64/tcg-target.c.inc|s/pvf_dup(s, k1, 0xbf800000);/pvf_dup(s, k1, 0xbf000000);/
# 9. vctuxs : moitié haute (>= 2^31) convertie sans retrancher 2^31
#    (le premier essai, « r & ~big » retiré, est un mutant équivalent : la
#    conversion indéfinie 0x80000000 de ces voies est absorbée par le OU)
tcg/x86_64/tcg-target.c.inc|s/tcg_out_vex_modrm(s, PVF_VSUBPS, r2, x, c);/tcg_out_vex_modrm(s, PVF_VADDPS, r2, x, c);/
# 10. CR6 : « toutes les voies » faux (traduction)
target/ppc/translate/vmx-impl.c.inc|s/tcg_gen_setcondi_i64(TCG_COND_EQ, u, u, -1);/tcg_gen_setcondi_i64(TCG_COND_NE, u, u, -1);/
MUT
( cd "$B" && nice ninja qemu-ppc > /dev/null 2>&1 ) && echo "arbre rendu intact, qemu-ppc reconstruit"
echo "$det mutants détectés sur $n"
