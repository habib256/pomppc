#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# tlbbnu.sh QEMU-SYSTEM-PPC [PROPS] — tools/tcg/tlbbnu.S (x-tlb-base en mode superviseur,
# sans système, docs/tcg-g4.md §41.3) : assemblé à 0x10000, chargé par -device loader (le
# CPU 0 y démarre, OpenBIOS ne tourne pas), options de CPU de production (+ PROPS) ; les
# résultats (0x400000) lus par le moniteur quand le programme a posé 0x600dcafe.
# Imprime les 35 mots de résultat et leur verdict contre l'attendu.
set -uo pipefail
Q="${1:?qemu-system-ppc}"; PROPS="${2:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
powerpc-linux-gnu-as -mregnames -many -o "$T/t.o" "$HERE/tlbbnu.S" || exit 2
powerpc-linux-gnu-ld -Ttext=0x10000 -o "$T/t.elf" "$T/t.o" || exit 2
powerpc-linux-gnu-objcopy -O binary "$T/t.elf" "$T/t.bin" || exit 2
CPU="g4,x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-vmx-inline=on"
CPU="$CPU,x-vfp-native=on,x-lmw-vector=on,x-fp-inline=on,x-fp-native=on,x-fp-native64=on"
CPU="$CPU,x-fp-native-cmp=on,x-fp-known=on,x-fprf-lazy=on,x-stfs-known=on,x-vfp-native-cmp=on"
CPU="$CPU,x-tb-fast=on,x-ret-inline=on,x-jc-idx=on,x-icbi-sync=on,x-msr-nobql=on,x-tlb-precise=on"
CPU="$CPU,x-lmw-inline=on,x-dcbz-inline=on,x-dirty-skip=on,x-pteg-map=on,x-lfs-known=on"
[ -n "$PROPS" ] && CPU="$CPU,$PROPS"
"$Q" -M mac99,via=pmu -m 512 -smp 1 -accel tcg,thread=multi,x-jit-near=on,x-jit-rel32=on \
    -cpu "$CPU" -bios "$ROOT/patches/smp-mac99/openbios-smp-screamer.elf" \
    -device loader,file="$T/t.bin",addr=0x10000,cpu-num=0 \
    -display none -nodefaults -audio none -monitor unix:"$T/mon",server=on,wait=off ${QEMU_D:+-d $QEMU_D -D $QEMU_DLOG} \
    2> "$T/err" &
QP=$!
ok=""
for i in $(seq 1 60); do
  sleep 1
  [ -S "$T/mon" ] || continue
  if python3 "$HERE/hmp.py" "$T/mon" "xp /1wx 0x400ffc" 2>/dev/null | grep -q 600dcafe; then
    ok=1; break
  fi
done
R="$(python3 "$HERE/hmp.py" "$T/mon" "xp /35wx 0x400000" 2>/dev/null)"
python3 "$HERE/hmp.py" "$T/mon" quit > /dev/null 2>&1
wait $QP 2>/dev/null
grep -a 'tlb-base' "$T/err" | tail -1
[ -n "$ok" ] || { echo "tlbbnu : le programme n'a pas fini"; cat "$T/err"; exit 1; }
python3 - "$R" <<'PY'
import re, sys
w = [int(x, 16) for x in re.findall(r'0x([0-9a-f]{8})', sys.argv[1].split(':', 1)[1].replace('\n', ' '))] if ':' in sys.argv[1] else []
w = [int(x, 16) for l in sys.argv[1].splitlines() for x in re.findall(r'\b0x([0-9a-f]{8})\b', l.split(':', 1)[1])]
A = lambda i: 0xa0000000 + i
B = lambda i: 0xb0000000 + i
exp = {0: ('T1 A[0] (DBAT0 -> A)', A(0)), 1: ('T1 B[1] (après mtspr DBAT0L/U)', B(1)),
       2: ('T1 B[2]', B(2)), 3: ('T2 A[0] (PTE -> A)', A(0)),
       4: ('T2 B[1] (après stw PTE, tlbie, sync)', B(1)), 5: ('T2 B[2]', B(2)),
       10: ('T4 A[0x7ffe] (0x3001fff8)', A(0x7ffe)), 11: ('T4 A[0x7fff] (couvert)', A(0x7fff)),
       13: ('T4 A[0x7bfe] (-4096)', A(0x7bfe)),
       14: ('T5 lwz 0xff8 (premier)', B(0x3fe)), 15: ('T5 lwz 0xffc (couvert)', B(0x3ff)),
       16: ('T5 lwz 0xffd (à cheval)', 0x0003ffa0), 17: ('T5 lhz 0xfff (à cheval)', 0xffa0),
       18: ('T5 lbz 0xfff (couvert)', 0xff), 19: ('T5 lwz 0x1000 (page suivante)', A(0xc00)),
       20: ('T6 tour 1 A[0]', A(0)), 21: ('T6 tour 1 A[1]', A(1)),
       22: ('T6 tour 2 B[0]', B(0)), 23: ('T6 tour 2 B[1]', B(1)),
       24: ('T6 tour 3 B[0]', B(0)), 25: ('T6 tour 3 B[1]', B(1)),
       26: ('T6 tour 4 B[0]', B(0)), 27: ('T6 tour 4 B[1]', B(1)),
       28: ('T7 paire 1 A[0]', A(0)), 29: ('T7 paire 1 A[1]', A(1)),
       30: ('T7 paire 2 B[0]', B(0)), 31: ('T7 paire 2 B[1] (après vidage)', B(1)),
       32: ('T8 B[0]', B(0)), 33: ('T8 B[1]', B(1)), 34: ('T8 B[2] (après stwcx.)', B(2))}
bad = 0
for i, v in enumerate(w):
    if i in exp:
        nom, e = exp[i]
        st = 'OK' if v == e else 'ÉCART (attendu %08x)' % e
        bad += v != e
    else:
        nom, st = {6: 'T3 lwz (MMIO)', 7: 'T3 lwz (MMIO, même base)', 8: 'T3 lhz (MMIO)',
                   9: 'T3 lbz (MMIO)', 12: 'T4 lwz 8 (page MMIO voisine)'}[i], ''
    print('%2d %08x  %-40s %s' % (i, v, nom, st))
if w[6] != w[7]:
    print('T3 : deux lectures du registre différentes'); bad += 1
if w[12] != w[6]:
    print('T4 : la page MMIO voisine ne rend pas le registre'); bad += 1
print('tlbbnu : %d mots, %d écarts' % (len(w), bad))
PY
