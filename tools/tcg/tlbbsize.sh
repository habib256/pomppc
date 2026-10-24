#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# tlbbsize.sh QEMU-SYSTEM-PPC JOURNAL [OUT] — taille du code de patches/tcg/0052 (x-tlb-base,
# docs/tcg-g4.md §41) sur les blocs mêmes d'un journal du jeu (-d in_asm,out_asm du binaire
# système). Le TLB logiciel n'existe pas en linux-user (lfsksize.sh, §40.4, ne le voit pas) :
# les blocs sont retraduits par le binaire SYSTÈME, à leur adresse, par l'essai « tbreplay-sys »
# (POMPPC_TBREPLAY_SYS, patches/tcg/essais/0052-tbreplay-sys.patch : au premier cpu_exec,
# chaque bloc est posé en RAM, une IBAT le met à son adresse, MSR du jeu, tb_gen_code, jamais
# exécuté ; puis QEMU sort), toutes les options de production (CPU et TCG), cible 32 bits en
# MTTCG ; bras ref, puis tlbb (+ x-tlb-base). tools/tcg/tcgsize.py --starts compare les
# octets par instruction ; les deux journaux restent dans OUT pour tools/tcg/codeop2.py.
set -uo pipefail
Q="${1:?qemu-system-ppc (avec l essai tbreplay-sys)}"; J="${2:?journal}"
OUT="${3:-${TMPDIR:-/tmp}/tlbbsize}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
mkdir -p "$OUT"
python3 "$HERE/tbreplay.py" "$J" "$OUT/blocs.bin" --tous
CPU="g4,x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-vmx-inline=on"
CPU="$CPU,x-vfp-native=on,x-lmw-vector=on,x-fp-inline=on,x-fp-native=on,x-fp-native64=on"
CPU="$CPU,x-fp-native-cmp=on,x-fp-known=on,x-fprf-lazy=on,x-stfs-known=on,x-vfp-native-cmp=on"
CPU="$CPU,x-tb-fast=on,x-ret-inline=on,x-jc-idx=on,x-icbi-sync=on,x-msr-nobql=on,x-tlb-precise=on"
CPU="$CPU,x-lmw-inline=on,x-dcbz-inline=on,x-dirty-skip=on,x-pteg-map=on,x-lfs-known=on"
TCG="thread=multi,x-jit-near=on,x-jit-rel32=on,x-jc-bits=14,x-jc-word=on,x-jc-2way=on"
replay() { # replay <log> <cpu>
  rm -f "$1"
  POMPPC_TBREPLAY_SYS="$OUT/blocs.bin" timeout 600 "$Q" -M mac99,via=pmu -m 512 -smp 2 \
      -accel "tcg,$TCG" -cpu "$2" -bios "$ROOT/patches/smp-mac99/openbios-smp-screamer.elf" \
      -display none -nodefaults -audio none -d in_asm,out_asm -D "$1" 2>&1 | tail -1
}
replay "$OUT/ref.log" "$CPU"
replay "$OUT/tlbb.log" "$CPU,x-tlb-base=on"
python3 "$HERE/tcgsize.py" "$OUT/ref.log" "$OUT/tlbb.log" --starts "$J" --top 40
