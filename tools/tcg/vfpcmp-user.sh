#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vfpcmp-user.sh QEMU-PPC [NVEC] [OUT] — preuve hôte de l'émetteur RÉEL de
# patches/tcg/0034-ppc-vfp-native-cmp.patch (x-vfp-native-cmp, docs/tcg-g4.md §34).
#
# Le job invité tools/guest/jobs/vfptest (mode c : vcmpeqfp vcmpgefp vcmpgtfp
# vcmpbfp et formes Rc, vcfux vcfsx vctuxs vctsxs pour chaque uim, VSCR[NJ] 0
# et 1, VSCR[SAT] et CR6 hachés) compilé par powerpc-linux-gnu-gcc (statique)
# et exécuté par un qemu-ppc linux-user construit depuis une copie de l'arbre
# patché (même traducteur, même émetteur x86_64 que le binaire système ;
# tools/tcg/fpnatcmp-userhack.py, docs/tcg-g4.md §33.3) :
#   ref  x-fast-fp + x-vfp-fast + x-vfp-native (les helpers)
#   cmp  + x-vfp-native-cmp ;  ver  + x-vfp-native-cmp-verify
# ref, cmp, ver : sorties identiques à l'octet ; ver sans divergence. Le mode
# d'origine de vfptest est rejoué aussi (0034 ne doit pas le changer).
set -uo pipefail
Q="${1:?qemu-ppc}"
N="${2:-65536}"
OUT="${3:-${TMPDIR:-/tmp}/vfpcmp-user}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT"
powerpc-linux-gnu-gcc -O2 -maltivec -mabi=altivec -static -mcpu=7400 \
    -Wa,-mregnames -o "$OUT/vfptest.ppc" "$HERE/../guest/jobs/vfptest/vfptest.c" || exit 2
BASE="g4,x-fast-fp=on,x-vfp-fast=on,x-vfp-native=on"
rc=0
for mode in c ""; do
  tag="${mode:-o}"
  for v in ref cmp ver; do
    case $v in
      ref) opt=$BASE ;;
      cmp) opt="$BASE,x-vfp-native-cmp=on" ;;
      ver) opt="$BASE,x-vfp-native-cmp=on,x-vfp-native-cmp-verify=on,x-vfp-native-verify=on" ;;
    esac
    NVEC=$N "$Q" -cpu "$opt" "$OUT/vfptest.ppc" $mode > "$OUT/$tag-$v.txt" 2> "$OUT/$tag-$v.err"
  done
  for v in cmp ver; do
    if cmp -s "$OUT/$tag-ref.txt" "$OUT/$tag-$v.txt"; then
      echo "mode $tag : $v identique à ref ($(tail -1 "$OUT/$tag-ref.txt"))"
    else
      echo "mode $tag : $v DIFFÉRENT de ref"; rc=1
      diff "$OUT/$tag-ref.txt" "$OUT/$tag-$v.txt" | head -20
    fi
  done
  grep -h 'vfp-native-cmp-verify:' "$OUT/$tag-ver.err" | tail -1
  if grep -q 'DIVERGENCE' "$OUT/$tag-ver.err"; then
    echo "mode $tag : $(grep -c DIVERGENCE "$OUT/$tag-ver.err") DIVERGENCES"; rc=1
    grep -m 10 DIVERGENCE "$OUT/$tag-ver.err"
  fi
done
exit $rc
