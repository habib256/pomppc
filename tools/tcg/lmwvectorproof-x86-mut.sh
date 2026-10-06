#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# lmwvectorproof-x86-mut.sh <arbre-QEMU> — contre-épreuve de patches/tcg/0029 :
# chaque mutation du bloc SSE2 de target/ppc/lmw-vector.h doit faire échouer
# lmwvectorproof.sh (écart, octet voisin touché, ou faute sur une page de garde).
# Un mutant qui ne compile pas n'est PAS compté comme détecté. docs/tcg-g4.md §30.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:?arbre QEMU avec tcg/0029 requis}"
H="$SRC/target/ppc/lmw-vector.h"
grep -q 'ppc_lmw_bswap32x4' "$H" || { echo "arbre sans tcg/0029 ($SRC)" >&2; exit 2; }
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/target/ppc"
fail=0
mut() {   # nom, expression sed (appliquée au seul bloc x86_64)
  sed "/^#elif defined(__x86_64__)/,\$ { $2 }" "$H" > "$W/target/ppc/lmw-vector.h"
  if cmp -s "$H" "$W/target/ppc/lmw-vector.h"; then
    echo "✘ mutation « $1 » sans effet sur le source"; fail=1; return
  fi
  for bits in 32 64; do
    cc -O2 -DTARGET_LONG_BITS="$bits" -I"$W/target/ppc" "$HERE/lmwvectorproof.c" \
       -o "$W/p$bits" 2> "$W/cc.log" || {
      echo "✘ mutation « $1 » : ne compile pas ($bits bits)"; fail=1; return; }
  done
  local det=""
  for bits in 32 64; do
    { ( ulimit -c 0; exec "$W/p$bits" ) > /dev/null 2>&1; } 2> /dev/null || det="$det $bits"
  done
  if [ -n "$det" ]; then
    echo "✔ « $1 » détectée (GPR de$det bits)"
  else
    echo "✘ mutation « $1 » NON détectée"; fail=1
  fi
}
mut "demi-mots non échangés"              's/_mm_shufflelo_epi16(w, 0xb1)/w/'
mut "octets des demi-mots non échangés"   's/return _mm_or_si128(_mm_slli_epi16(w, 8), _mm_srli_epi16(w, 8));/return w;/'
mut "lmw : un octet de trop par demi-mot" 's/_mm_srli_epi16(w, 8)/_mm_srli_epi16(w, 7)/'
mut "lmw 64 : pas d'extension par zéro"   's/_mm_unpacklo_epi32(w, z)/_mm_unpacklo_epi32(w, w)/'
mut "lmw 64 : mauvaise moitié haute"      's/_mm_unpackhi_epi32(w, z)/_mm_unpacklo_epi32(w, z)/'
mut "stmw 64 : moitiés hautes des GPR"    's/0x08);/0x0d);/'
mut "stmw 64 : second couple oublié"      's/_mm_unpacklo_epi64(lo, hi)/_mm_unpacklo_epi64(lo, lo)/'
mut "lmw : vecteur au-delà de la plage"   '/ppc_lmw_vector(target_ulong/,/^}/ s/reg + 4 <= 32/reg + 3 <= 32/'
mut "stmw : vecteur au-delà de la plage"  '/ppc_stmw_vector(const/,/^}/ s/reg + 4 <= 32/reg + 3 <= 32/'
mut "lmw : queue oubliée"                 '/ppc_lmw_vector(target_ulong/,/^}/ s/reg < 32; reg++, p += 4/reg < 31; reg++, p += 4/'
mut "stmw : queue oubliée"                '/ppc_stmw_vector(const/,/^}/ s/reg < 32; reg++, p += 4/reg < 31; reg++, p += 4/'
mut "stmw : pas de 12 octets"             '/ppc_stmw_vector(const/,/^}/ s/reg += 4, p += 16/reg += 4, p += 12/'
exit $fail
