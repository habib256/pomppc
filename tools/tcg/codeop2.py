#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""codeop2.py JOURNAL-A JOURNAL-B PERF.DATA PERFMAP [OPCODES…] — deux journaux du même code
invité (tools/tcg/tbreplay.c, deux réglages) comparés poste par poste, chaque bloc pondéré par
le temps perf de son adresse dans la partie d'origine (tools/tcg/codeop.py, même poids pour
les deux) : instructions x86 par instruction invité, par opcode et en tout, sur les blocs
présents dans les deux journaux. docs/tcg-g4.md §40.5."""
import sys, collections
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import codeanat as C
from codeop import name
from capstone import Cs, CS_ARCH_X86, CS_MODE_64


def load(path, wt, md):
    bypc = {b['pc']: b for b in C.parse(path)}
    out = {}
    for pc, b in bypc.items():
        w = wt.get(pc, 0)
        if not w:
            continue
        words = dict(b['insns'])
        per = []
        for gpc, code in b['parts']:
            if gpc is None:
                continue
            wd = words.get(gpc, 0)
            c = C.classify(md, code, C.is_ldst(wd))
            per.append((gpc, name(wd), sum(c.values())))
        out[pc] = per
    return out


def main():
    ja, jb, perf, mp = sys.argv[1:5]
    want = sys.argv[5:]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    wt, _ = C.time_by_pc(perf, mp)
    A, B = load(ja, wt, md), load(jb, wt, md)
    common = [pc for pc in A if pc in B and [g for g, _, _ in A[pc]] == [g for g, _, _ in B[pc]]]
    ha, hb, g = collections.Counter(), collections.Counter(), collections.Counter()
    for pc in common:
        w = wt[pc]
        for (gpc, k, na), (_, _, nb) in zip(A[pc], B[pc]):
            ha[k] += w * na
            hb[k] += w * nb
            g[k] += w
    G, HA, HB = sum(g.values()), sum(ha.values()), sum(hb.values())
    print('%d blocs pondérés communs aux deux journaux (A %d, B %d)' % (len(common), len(A), len(B)))
    print('| opcode | part instr. invité | x86/instr. A | x86/instr. B | écart |')
    print('|---|---|---|---|---|')
    keys = want or [k for k, _ in ha.most_common(16)]
    for k in keys:
        if not g[k]:
            continue
        a, b = ha[k] / g[k], hb[k] / g[k]
        print('| %s | %.1f %% | %.1f | %.1f | %+.1f %% |' % (k, 100 * g[k] / G, a, b, 100 * (b - a) / a))
    print('| **tout** | 100 %% | %.2f | %.2f | **%+.1f %%** |' % (HA / G, HB / G, 100 * (HB - HA) / HA))


if __name__ == '__main__':
    main()
