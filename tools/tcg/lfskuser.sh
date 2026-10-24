#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# lfskuser.sh QEMU-PPC MODE [ARGS…] — banc linux-user de patches/tcg/0051 (x-lfs-known,
# docs/tcg-g4.md §40) : le job tools/guest/jobs/fptest compilé par powerpc-linux-gnu-gcc
# (statique) et exécuté par un qemu-ppc linux-user de l'arbre patché (même traducteur, même
# émetteur x86_64, même talon que le binaire système ; copie neutralisée par
# tools/tcg/fpnatcmp-userhack.py, §37 et §40).
#
#   lfskuser.sh Q l LO HI [NPROC]   mode l, mots [LO, HI) coupés en NPROC tranches parallèles
#                                   (défaut 8), bras ref puis lfsk ; empreintes par tranche
#   lfskuser.sh Q cmp [N]           modes k (N programmes, défaut 4096, graines 0 et 7), s, d, c
#                                   (65 536) : ref, lfsk, lfsk + x-fp-verify ; doivent être
#                                   identiques ; bilan du vérificateur
#   lfskuser.sh Q un ARGS…          un seul passage, bras lfsk (mutants : sortie brute)
# Variables : OUT (défaut $TMPDIR/lfskuser), OPTS (propriétés en plus pour le bras lfsk).
set -uo pipefail
Q="${1:?qemu-ppc}"; MODE="${2:?mode}"; shift 2
OUT="${OUT:-${TMPDIR:-/tmp}/lfskuser}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT"
F="$OUT/fptest.ppc"
if [ ! -x "$F" ] || [ "$HERE/../guest/jobs/fptest/fptest.c" -nt "$F" ]; then
  powerpc-linux-gnu-gcc -O2 -Wall -static -mcpu=7400 -Wa,-mregnames \
      -o "$F" "$HERE/../guest/jobs/fptest/fptest.c" || exit 2
fi
# les options de production de run_tiger.sh qui touchent le flottant simple
REF="g4,x-fast-fp=on,x-lfs-inline=on,x-fp-inline=on,x-fp-native=on,x-fp-native64=on"
REF="$REF,x-fp-native-cmp=on,x-fp-known=on,x-fprf-lazy=on,x-stfs-known=on"
LFSK="$REF,x-lfs-known=on${OPTS:+,$OPTS}"
rc=0
case "$MODE" in
  l)
    LO=$((16#${1:?LO})); HI=$((16#${2:?HI})); NP="${3:-8}"
    STEP=$(( (HI - LO + NP - 1) / NP ))
    for arm in ref lfsk; do
      cpu="$REF"; [ "$arm" = lfsk ] && cpu="$LFSK"
      t0=$(date +%s)
      pids=()
      for ((i = 0; i < NP; i++)); do
        a=$((LO + i * STEP)); b=$((a + STEP)); [ $b -gt $HI ] && b=$HI
        "$Q" -cpu "$cpu" "$F" l "$(printf %x $a)" "$(printf %x $b)" \
            > "$OUT/l-$arm-$i.txt" 2> "$OUT/l-$arm-$i.err" &
        pids+=($!)
      done
      for p in "${pids[@]}"; do wait "$p" || rc=1; done
      echo "bras $arm : $(( $(date +%s) - t0 )) s"
      cat "$OUT"/l-$arm-*.txt | grep -c ECART | sed 's/^/  lignes ECART : /'
      grep -h '^L ' "$OUT"/l-$arm-*.txt | sort > "$OUT/l-$arm.txt"
      awk '{n += $4; z += $6; e += $10} END {print "  " n " mots, " z " nuls ou normaux, " e " ecarts"}' \
          FS='[ (]+' "$OUT/l-$arm.txt"
    done
    if cmp -s "$OUT/l-ref.txt" "$OUT/l-lfsk.txt"; then
      echo "empreintes ref = lfsk ($(wc -l < "$OUT/l-ref.txt") tranches)"
    else
      echo "empreintes ref != lfsk"; rc=1; diff "$OUT/l-ref.txt" "$OUT/l-lfsk.txt" | head
    fi
    ;;
  cmp)
    N="${1:-4096}"
    for m in "k $N 0" "k $N 7" "s 65536" "d 65536" "c 65536"; do
      set -- $m; tag="$1${3:-}"; args=("$@"); [ "$1" = s ] && args=("$2")
      "$Q" -cpu "$REF" "$F" "${args[@]}" > "$OUT/$tag-ref.txt" 2> "$OUT/$tag-ref.err"
      "$Q" -cpu "$LFSK" "$F" "${args[@]}" > "$OUT/$tag-lfsk.txt" 2> "$OUT/$tag-lfsk.err"
      "$Q" -cpu "$LFSK,x-fp-verify=on,x-fp-native-cmp-verify=on" "$F" "${args[@]}" \
          > "$OUT/$tag-ver.txt" 2> "$OUT/$tag-ver.err"
      for v in lfsk ver; do
        if cmp -s "$OUT/$tag-ref.txt" "$OUT/$tag-$v.txt"; then
          echo "mode $tag : $v identique à ref ($(tail -1 "$OUT/$tag-ref.txt"))"
        else
          echo "mode $tag : $v DIFFÉRENT de ref ($(diff "$OUT/$tag-ref.txt" "$OUT/$tag-$v.txt" | grep -c '^>') lignes)"; rc=1
        fi
      done
      grep -h 'fp-native-verify:' "$OUT/$tag-ver.err" | grep -v DIVERGENCE | tail -1 | \
          sed -e 's/fadds.*fsel [0-9/]* //' -e 's/^/  /'
      d=$(grep -c DIVERGENCE "$OUT/$tag-ver.err")
      [ "$d" != 0 ] && { echo "  $d DIVERGENCES"; grep -m 5 DIVERGENCE "$OUT/$tag-ver.err"; rc=1; }
    done
    ;;
  un)
    "$Q" -cpu "$LFSK" "$F" "$@" || rc=1
    ;;
esac
exit $rc
