#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# fpnatcmp-user.sh QEMU-PPC [NRAND] [OUT] — preuve hôte de l'émetteur RÉEL de
# patches/tcg/0033-ppc-fp-native-cmp.patch (x-fp-native-cmp, docs/tcg-g4.md §33).
#
# Le job invité tools/guest/jobs/fptest (mode c : frsp fctiw fctiwz fcmpo fdivs
# fdiv fsel, onze états du FPSCR, catalogue croisé puis aléatoire) compilé par
# powerpc-linux-gnu-gcc (statique) et exécuté par un qemu-ppc linux-user construit
# depuis l'arbre patché (le code généré est celui du binaire système : même
# traducteur, même émetteur x86_64, même talon) :
#   ref  x-fast-fp + x-fp-inline + x-fp-native + x-fp-native64 (les helpers)
#   cmp  la même chose + x-fp-native-cmp
#   ver  + x-fp-native-cmp-verify (bilan du vérificateur sur stderr)
# Les sorties ref et cmp doivent être identiques à l'octet ; ver aussi, et son
# bilan sans divergence. Modes d'origine (simple, d) rejoués aussi : 0033 ne
# doit pas les changer.
# Le qemu-ppc vient d'une COPIE de l'arbre où le code « système seulement » des
# patches 0001/0008/0011 est neutralisé (le binaire système n'en est pas
# touché) : rsync de l'arbre vers ~/src/qemu-fpu, tools/tcg/fpnatcmp-userhack.py,
# configure --target-list=ppc-linux-user --static dans build-user, ninja qemu-ppc
# (docs/tcg-g4.md §33.3).
set -uo pipefail
Q="${1:?qemu-ppc}"
N="${2:-65536}"
OUT="${3:-${TMPDIR:-/tmp}/fpnatcmp-user}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT"
powerpc-linux-gnu-gcc -O2 -Wall -static -mcpu=7400 -Wa,-mregnames \
    -o "$OUT/fptest.ppc" "$HERE/../guest/jobs/fptest/fptest.c" || exit 2
BASE="g4,x-fast-fp=on,x-fp-inline=on,x-fp-native=on,x-fp-native64=on"
rc=0
for mode in c "" d; do
  tag="${mode:-s}"
  "$Q" -cpu "$BASE" "$OUT/fptest.ppc" $mode "$N" > "$OUT/$tag-ref.txt" 2> "$OUT/$tag-ref.err"
  "$Q" -cpu "$BASE,x-fp-native-cmp=on" "$OUT/fptest.ppc" $mode "$N" \
      > "$OUT/$tag-cmp.txt" 2> "$OUT/$tag-cmp.err"
  "$Q" -cpu "$BASE,x-fp-native-cmp=on,x-fp-native-cmp-verify=on" "$OUT/fptest.ppc" $mode "$N" \
      > "$OUT/$tag-ver.txt" 2> "$OUT/$tag-ver.err"
  for v in cmp ver; do
    if cmp -s "$OUT/$tag-ref.txt" "$OUT/$tag-$v.txt"; then
      echo "mode $tag : $v identique à ref ($(tail -1 "$OUT/$tag-ref.txt"))"
    else
      echo "mode $tag : $v DIFFÉRENT de ref"; rc=1
      diff "$OUT/$tag-ref.txt" "$OUT/$tag-$v.txt" | head -20
    fi
  done
  grep -h 'fp-native-verify' "$OUT/$tag-ver.err" | tail -1
  if grep -q 'DIVERGENCE' "$OUT/$tag-ver.err"; then
    echo "mode $tag : $(grep -c DIVERGENCE "$OUT/$tag-ver.err") DIVERGENCES"; rc=1
    grep -m 10 DIVERGENCE "$OUT/$tag-ver.err"
  fi
done
exit $rc
