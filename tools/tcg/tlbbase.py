#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""tlbbase.py — porte du « TLB par base » (docs/vitesse-doom3-x86.md §16) : part des accès en forme D
couverts, dans le bloc, par un accès antérieur de même nature (lecture/écriture) sur la même base
inchangée, pondérée par le temps perf (journal et profil de x86-asm-2410)."""
import sys, collections; sys.path.insert(0,'tools/tcg')
import codeanat as C
blocks=C.parse('bench/tcg/t2/d3-asm-2410.log'); bypc={b['pc']:b for b in blocks}
D='bench/tcg/ab/x86-asm-2410/asm-1/d3-fen/'
wt,_=C.time_by_pc(D+'perf-hote-mesure.data',D+'perf-1757820.map')
ST={36,37,38,39,44,45,52,53,54,55}
UPD={33,35,37,39,41,43,45,49,51,53,55}
GPRLD={32,33,34,35,40,41,42,43}
c=collections.Counter(); nblk=collections.Counter()
for pc,b in bypc.items():
    w=wt.get(pc,0)
    if not w: continue
    seen={}   # (ra, kind) -> nb
    bases=set()
    for gpc,wd in b['insns']:
        op=wd>>26; rt=(wd>>21)&31; ra=(wd>>16)&31
        def kill(r):
            for k in ('ld','st'): seen.pop((r,k),None)
        if 32<=op<=55 and op not in (46,47) and ra:
            k='st' if op in ST else 'ld'
            c[k+(' couvert' if (ra,k) in seen else ' premier')]+=w
            seen[(ra,k)]=1; bases.add((ra,k))
            if op in UPD: kill(ra)
            if op in GPRLD: kill(rt)
        elif op==31 and C.is_ldst(wd):
            c['indexé']+=w; kill(rt)
        elif op in (46,47): c['lmw/stmw']+=w; 
        elif op in (20,21,23,24,25,26,27,28,29): kill(ra)
        elif op==31:
            xo=(wd>>1)&0x3ff
            kill(ra if xo in (444,28,316,24,536,792,824,60,124,476,954,922,26,284,412) else rt)
        elif op in (7,8,12,13,14,15): kill(rt)
    nblk[len(bases)]+=w
t=sum(c.values())
for k,v in sorted(c.items()): print('%-12s %5.1f %%'%(k,100*v/t))
tb=sum(nblk.values()); print('bases (base,nature) distinctes par bloc, pondéré :', {k:round(100*v/tb) for k,v in sorted(nblk.items())})
