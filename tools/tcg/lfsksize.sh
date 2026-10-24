#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# lfsksize.sh QEMU-PPC JOURNAL [OUT] — taille du code de patches/tcg/0051 (x-lfs-known,
# docs/tcg-g4.md §40) sur les blocs mêmes d'un journal du jeu (-d in_asm,out_asm du binaire
# système) : tools/tcg/tbreplay.py les extrait, tools/tcg/tbreplay.c les fait retraduire par
# le qemu-ppc linux-user (même traducteur, même émetteur ; lfs/stfs sans TLB logiciel), bras
# ref (options de production du flottant) et lfsk (+ x-lfs-known) ; tools/tcg/tcgsize.py
# --starts compare les octets par instruction (§37.5, §39.3).
# Les deux journaux restent dans OUT (ref.log, lfsk.log) pour tools/tcg/codeop.py.
set -uo pipefail
Q="${1:?qemu-ppc}"; J="${2:?journal}"
OUT="${3:-${TMPDIR:-/tmp}/lfsksize}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT"
python3 "$HERE/tbreplay.py" "$J" "$OUT/blocs.bin"
powerpc-linux-gnu-gcc -O1 -static -fno-stack-protector -o "$OUT/tbreplay.ppc" "$HERE/tbreplay.c" || exit 2
REF="g4,x-fast-fp=on,x-lfs-inline=on,x-fp-inline=on,x-fp-native=on,x-fp-native64=on"
REF="$REF,x-fp-native-cmp=on,x-fp-known=on,x-fprf-lazy=on,x-stfs-known=on"
NB=$(python3 -c "import struct,sys
d=open(sys.argv[1],'rb').read(); i=n=0
while i < len(d):
    a, k = struct.unpack('>II', d[i:i + 8]); i += 8 + 4 * k; n += 1
print(n)" "$OUT/blocs.bin")
# Un bloc peut faire tomber tbreplay (il écrit dans ses données ou dans son gestionnaire de
# signaux, et boucle) : paquets de 25 blocs sous `timeout 20` ; un paquet qui tombe ou
# n'aboutit pas est refait bloc par bloc, et le bloc fautif seul est perdu (compté).
run1() { # run1 <cpu> <début> <nombre> : 0 si tbreplay a fini
  rm -f "$OUT/part.log"
  timeout 20 "$Q" -cpu "$1" -d in_asm,out_asm -D "$OUT/part.log" "$OUT/tbreplay.ppc" \
      "$OUT/blocs.bin" "$2" "$3" > "$OUT/part.out" 2> /dev/null
  grep -q '^tbreplay :' "$OUT/part.out"
}
replay() { # replay <log> <cpu>
  local log=$1 cpu=$2 s i lost=0
  : > "$log"
  for ((s = 0; s < NB; s += 25)); do
    if run1 "$cpu" "$s" 25; then
      cat "$OUT/part.log" >> "$log"
      continue
    fi
    for ((i = s; i < s + 25 && i < NB; i++)); do
      if run1 "$cpu" "$i" 1; then
        cat "$OUT/part.log" >> "$log"
      else
        lost=$((lost + 1))
      fi
    done
  done
  echo "  $(basename "$log") : $NB blocs, $lost perdus (tbreplay tombé ou bloqué)"
}
replay "$OUT/ref.log" "$REF"
replay "$OUT/lfsk.log" "$REF,x-lfs-known=on"
python3 "$HERE/tcgsize.py" "$OUT/ref.log" "$OUT/lfsk.log" --starts "$J" --top 40
