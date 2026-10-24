#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""tlbbcover.py JOURNAL PERF.DATA PERFMAP — part des chargements en forme D de DOOM 3 que
x-tlb-base (patches/tcg/0052, docs/tcg-g4.md §41.4) couvre vraiment : dans un journal de
retraduction en mode système (tools/tcg/tlbbsize.sh, bras tlbb), un chargement est
« couvert » quand son code porte le test de couverture (lea L0, [negp + ea] ; cmp L0,
0x1000 - taille), « premier » quand il porte le chemin court du TLB et pose une base
(mov adend, [L0 + 0x18] ; mov negp, L1 ; neg negp), « seul » sinon (qemu_ld ordinaire).
Chaque bloc est pondéré par son temps perf dans la partie profilée (comme codeop2.py), à
comparer à la porte de tools/tcg/tlbbase.py (docs/vitesse-doom3-x86.md §16)."""
import collections, sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import codeanat as C
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

LOADS = {32, 33, 34, 35, 40, 41, 42, 43, 48, 49, 50, 51}
STORES = {36, 37, 38, 39, 44, 45, 52, 53, 54, 55}


def kind(md, code):
    ins = list(md.disasm(code, 0))
    for i, x in enumerate(ins):
        if x.mnemonic == 'cmp' and x.op_str.startswith('rdi, 0xff') and \
                i + 1 < len(ins) and ins[i + 1].mnemonic == 'ja':
            return 'couvert'
    for x in ins:
        if x.mnemonic == 'neg' and x.op_str.startswith('r'):
            return 'premier'
    return 'seul'


def main():
    log, perf, mp = sys.argv[1:4]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    wt, _ = C.time_by_pc(perf, mp)
    c = collections.Counter()
    for b in C.parse(log):
        w = wt.get(b['pc'], 0)
        if not w:
            continue
        words = dict(b['insns'])
        for gpc, code in b['parts']:
            if gpc is None:
                continue
            op = words.get(gpc, 0) >> 26
            if op in LOADS:
                c['chargement ' + kind(md, code)] += w
            elif op in STORES:
                c['rangement'] += w
            elif op == 31 and C.is_ldst(words.get(gpc, 0)):
                c['indexé'] += w
            elif op in (46, 47):
                c['lmw/stmw'] += w
    t = sum(c.values())
    for k, v in sorted(c.items()):
        print('%-22s %5.1f %%' % (k, 100 * v / t))


if __name__ == '__main__':
    main()
