#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vfpkuser.sh QEMU-PPC MODE [ARGS…] — banc linux-user de patches/tcg/0053 (x-vfp-known,
# docs/tcg-g4.md §42) : le job tools/guest/jobs/vfptest compilé par powerpc-linux-gnu-gcc
# (statique) et exécuté par un qemu-ppc linux-user de l'arbre patché (même traducteur, même
# émetteur x86_64, même talon que le binaire système ; copie neutralisée par
# tools/tcg/fpnatcmp-userhack.py).
#
#   vfpkuser.sh Q cmp [N] [GRAINES]  mode k (N programmes, défaut 4096, graines « 0 7 »), kp (16 N),
#                                    kx (N/4, un processus par graine), modes d'origine
#                                    (NVEC 65 536) et c : bras ref, vfpk, vfpk + vérificateurs ;
#                                    sorties identiques exigées ; bilans
#   vfpkuser.sh Q un ARGS…           un passage, bras vfpk + vérificateurs (ARM=vfpk : sans
#                                    eux, ARM=ref : sans x-vfp-known) ; mutants
#   vfpkuser.sh Q kx NPROC N         NPROC processus kx (graines 0..NPROC-1), bras ref et
#                                    vfpk + vérificateurs : empreintes, sorties de bloc « porte »
# Variables : OUT (défaut $TMPDIR/vfpkuser), OPTS (propriétés en plus du bras vfpk).
set -uo pipefail
Q="${1:?qemu-ppc}"; MODE="${2:?mode}"; shift 2
OUT="${OUT:-${TMPDIR:-/tmp}/vfpkuser}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT"
F="$OUT/vfptest.ppc"
SRC="$HERE/../guest/jobs/vfptest/vfptest.c"
if [ ! -x "$F" ] || [ "$SRC" -nt "$F" ]; then
  powerpc-linux-gnu-gcc -O2 -maltivec -mabi=altivec -static -mcpu=7400 -Wa,-mregnames \
      -o "$F" "$SRC" 2> "$OUT/build.err" || { cat "$OUT/build.err"; exit 2; }
fi
# les options de production de run_tiger.sh (Linux x86-64) qui touchent AltiVec et le flottant
REF="g4,x-fast-fp=on,x-vfp-fast=on,x-vperm-fast=on,x-vmx-inline=on,x-vfp-native=on"
REF="$REF,x-vfp-native-cmp=on,x-lfs-inline=on,x-fp-inline=on,x-fp-native=on,x-fp-native64=on"
REF="$REF,x-fp-native-cmp=on,x-fp-known=on,x-fprf-lazy=on,x-stfs-known=on,x-lfs-known=on"
VFPK="$REF,x-vfp-known=on${OPTS:+,$OPTS}"
VER="$VFPK,x-vfp-native-verify=on,x-vfp-native-cmp-verify=on"
rc=0
run() {         # run TAG CPU ARGS… : sortie dans $OUT/TAG.txt, bilans dans $OUT/TAG.err
  local tag="$1" cpu="$2"; shift 2
  NVEC="${NVEC:-65536}" "$Q" -cpu "$cpu" "$F" "$@" > "$OUT/$tag.txt" 2> "$OUT/$tag.err"
}
bilan() {
  grep -h 'FINAL\|vfp-native-verify:\|x-vfp-known' "$OUT/$1.err" | grep -v replis | tail -3 | sed 's/^/    /'
}
case "$MODE" in
  cmp)
    N="${1:-4096}"; SEEDS="${2:-0 7}"
    for s in $SEEDS; do
      for arm in ref vfpk ver; do
        cpu="$REF"; [ $arm = vfpk ] && cpu="$VFPK"; [ $arm = ver ] && cpu="$VER"
        run "k$s-$arm" "$cpu" k "$N" "$s" || rc=1
        run "kx$s-$arm" "$cpu" kx "$((N / 4))" "$s" || rc=1
        run "kp$s-$arm" "$cpu" kp "$((N * 16))" "$s" || rc=1
      done
    done
    for m in o c; do
      for arm in ref vfpk ver; do
        cpu="$REF"; [ $arm = vfpk ] && cpu="$VFPK"; [ $arm = ver ] && cpu="$VER"
        if [ $m = o ]; then run "$m-$arm" "$cpu" || rc=1; else run "$m-$arm" "$cpu" c || rc=1; fi
      done
    done
    for t in $(for s in $SEEDS; do echo k$s kx$s kp$s; done) o c; do
      for arm in vfpk ver; do
        if cmp -s "$OUT/$t-ref.txt" "$OUT/$t-$arm.txt"; then
          echo "$t : $arm identique à ref ($(tail -1 "$OUT/$t-ref.txt"))"
        else
          echo "$t : $arm DIFFÉRENT de ref"; rc=1
        fi
      done
      bilan "$t-ver"
    done
    ;;
  kx)
    NP="${1:-16}"; N="${2:-256}"
    for arm in ref ver; do
      cpu="$REF"; [ $arm = ver ] && cpu="$VER"
      pids=()
      for ((i = 0; i < NP; i++)); do
        run "kxp$i-$arm" "$cpu" kx "$N" "$i" & pids+=($!)
      done
      for p in "${pids[@]}"; do wait "$p" || rc=1; done
    done
    d=0
    for ((i = 0; i < NP; i++)); do
      cmp -s "$OUT/kxp$i-ref.txt" "$OUT/kxp$i-ver.txt" || { d=$((d + 1)); rc=1; }
    done
    echo "kx : $NP processus, $d sorties différentes de ref"
    for e in "$OUT"/kxp*-ver.err; do
      grep 'FINAL vfp-native-verify:' "$e" | tail -1; grep 'x-vfp-known :' "$e" | tail -1
    done | awk '
      /vfp-native-verify:/ { n += $3; b += $(NF-1) }
      /x-vfp-known :/ { gsub(/[(),]/, ""); t += $3; s += $6; r += $11; g += $13 }
      END { printf "  vérifiées %d, divergences %d ; talons relus %d, sorties %d (vD %d, porte %d)\n", n, b, t, s, r, g }'
    ;;
  banc)
    # bancs de vfptest (N x 4 vperm dépendants, N x (2 vmaddfp + vaddfp + vsubfp)), bras
    # alternés ref, vfpk, trois fois chacun ; indicatif (hôte partagé)
    N="${1:-20000000}"
    for t in 1 2 3; do
      for arm in ref vfpk; do
        cpu="$REF"; [ $arm = vfpk ] && cpu="$VFPK"
        NVEC=1 "$Q" -cpu "$cpu" "$F" "$N" | grep '^banc' | sed "s/^/$arm $t : /"
      done
    done
    ;;
  un)
    cpu="$VER"; [ "${ARM:-ver}" = vfpk ] && cpu="$VFPK"; [ "${ARM:-ver}" = ref ] && cpu="$REF"
    run un "$cpu" "$@" || rc=1
    cat "$OUT/un.txt"; bilan un
    ;;
  *) echo "mode inconnu : $MODE" >&2; exit 2 ;;
esac
exit $rc
