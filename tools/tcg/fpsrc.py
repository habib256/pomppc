#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""fpsrc.py JOURNAL PERF.DATA PERFMAP — d'où viennent, dans le bloc, les opérandes des opérations
flottantes (lfs, producteur x-fp-known, autre, inconnu), pondéré par le temps. docs/vitesse-doom3-x86.md §15."""
import sys, collections
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import codeanat as C
from codeop import name
blocks = C.parse(sys.argv[1]); bypc = {b['pc']: b for b in blocks}
wt, vtot = C.time_by_pc(sys.argv[2], sys.argv[3])
PROD = {'fmuls','fmadds','fmsubs','fnmadds','fnmsubs','fdivs','frsp','fadds','fsubs'}
LFS = {'lfs','lfsx','lfsu','lfsux'}
src = collections.Counter(); kill_after_lfs = collections.Counter()
for pc, b in bypc.items():
    w = wt.get(pc, 0)
    if not w: continue
    st = {}   # fpr -> origine
    for gpc, wd in b['insns']:
        n = name(wd); op = wd >> 26
        rt, ra, rb, rc = (wd >> 21) & 31, (wd >> 16) & 31, (wd >> 11) & 31, (wd >> 6) & 31
        if n in PROD or n in ('fcmpu','fcmpo','fctiwz','fmul','fadd','fsub','fmadd','fmsub','stfs','stfsx','stfsu'):
            if n.startswith('stfs'): ops = [rt]
            elif n in ('fmuls','fmul'): ops = [ra, rc]
            elif n in ('fadds','fsubs','fdivs','fadd','fsub','fcmpu','fcmpo'): ops = [ra, rb]
            elif n in ('frsp','fctiwz'): ops = [rb]
            else: ops = [ra, rc, rb]
            for r in ops: src[(n in PROD or n.startswith('f'), st.get(r, 'inconnu'))] += w
            if n in PROD: st[rt] = 'producteur'
        elif n in LFS: st[rt] = 'lfs'
        elif n in ('lfd','lfdx','lfdu','fmr','fneg','fabs','fsel','fres','frsqrte','mtfsf','mffs','fctiw'): 
            if op in (50,51) or op == 31 or op == 63: st[rt] = 'autre'
        elif n.startswith('31/') or n in ('mtfsb0','mtfsb1'): pass
tot = sum(src.values())
for (isfp, s), v in sorted(src.items(), key=lambda x: -x[1]):
    print('%-12s %-12s %5.1f %%' % ('op flottante' if isfp else 'stfs', s, 100 * v / tot))
