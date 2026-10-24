#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""codeop.py JOURNAL PERF.DATA PERFMAP — code x86 émis par opcode PowerPC (part du code x86
exécuté, pondérée par le temps perf de chaque bloc), postes de codeanat.py. docs/vitesse-doom3-x86.md §15."""
import sys, collections
sys.path.insert(0, 'tools/tcg')
import codeanat as C
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
P = {32:'lwz',33:'lwzu',34:'lbz',35:'lbzu',36:'stw',37:'stwu',38:'stb',39:'stbu',40:'lhz',41:'lhzu',42:'lha',
     44:'sth',45:'sthu',46:'lmw',47:'stmw',48:'lfs',49:'lfsu',50:'lfd',51:'lfdu',52:'stfs',53:'stfsu',54:'stfd',55:'stfdu',
     14:'addi',15:'addis',10:'cmpli',11:'cmpi',16:'bc',18:'b',21:'rlwinm',20:'rlwimi',24:'ori',25:'oris',28:'andi.',
     7:'mulli',12:'addic',8:'subfic',26:'xori'}
X31 = {23:'lwzx',151:'stwx',87:'lbzx',215:'stbx',279:'lhzx',407:'sthx',535:'lfsx',663:'stfsx',599:'lfdx',727:'stfdx',
       103:'lvx',231:'stvx',7:'lvebx',39:'lvehx',71:'lvewx',135:'stvebx',167:'stvehx',199:'stvewx',6:'lvsl',38:'lvsr',
       266:'add',40:'subf',444:'or',28:'and',316:'xor',24:'slw',536:'srw',792:'sraw',824:'srawi',0:'cmp',32:'cmpl',
       235:'mullw',339:'mfspr',467:'mtspr',19:'mfcr',144:'mtcrf',8:'subfc',136:'subfe',10:'addc',138:'adde',
       104:'neg',954:'extsb',922:'extsh',26:'cntlzw',60:'andc',124:'nor',476:'nand',459:'divwu',491:'divw',75:'mulhw',11:'mulhwu',
       1014:'dcbz',86:'dcbf',534:'lwbrx',662:'stwbrx',20:'lwarx',150:'stwcx.',371:'mftb',83:'mfmsr',146:'mtmsr',598:'sync'}
X59 = {18:'fdivs',20:'fsubs',21:'fadds',25:'fmuls',28:'fmsubs',29:'fmadds',30:'fnmsubs',31:'fnmadds',24:'fres',22:'fsqrts'}
X63 = {0:'fcmpu',32:'fcmpo',12:'frsp',14:'fctiw',15:'fctiwz',18:'fdiv',20:'fsub',21:'fadd',25:'fmul',28:'fmsub',29:'fmadd',
       30:'fnmsub',31:'fnmadd',23:'fsel',26:'frsqrte',72:'fmr',40:'fneg',264:'fabs',136:'fnabs',583:'mffs',711:'mtfsf',38:'mtfsb1',70:'mtfsb0'}
X19 = {16:'bclr',528:'bcctr',449:'cror',193:'crxor',257:'crand',289:'creqv',150:'isync',33:'crnor',417:'crorc',129:'crandc',225:'crnand',0:'mcrf'}
def name(w):
    op = w >> 26
    if op == 31: return X31.get((w >> 1) & 0x3ff, '31/%d' % ((w >> 1) & 0x3ff)) + ('.' if w & 1 else '')
    if op == 59:
        x = (w >> 1) & 0x1f
        return X59.get(x, '59/%d' % x)
    if op == 63:
        x10 = (w >> 1) & 0x3ff; x5 = (w >> 1) & 0x1f
        return X63.get(x10) or X63.get(x5, '63/%d' % x10)
    if op == 19: return X19.get((w >> 1) & 0x3ff, '19/?')
    if op == 4: return 'vmx'
    return P.get(op, 'op%d' % op)
def main():
  md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
  blocks = C.parse(sys.argv[1]); bypc = {b['pc']: b for b in blocks}
  wt, vtot = C.time_by_pc(sys.argv[2], sys.argv[3])
  host = collections.Counter(); gcnt = collections.Counter(); post = collections.defaultdict(collections.Counter)
  for pc, b in bypc.items():
      w = wt.get(pc, 0)
      if not w: continue
      words = dict(b['insns'])
      for gpc, code in b['parts']:
          if gpc is None: continue
          wd = words.get(gpc, 0); k = name(wd)
          c = C.classify(md, code, C.is_ldst(wd))
          host[k] += w * sum(c.values()); gcnt[k] += w; post[k].update({p: w * v for p, v in c.items()})
  H = sum(host.values()); G = sum(gcnt.values())
  print('| opcode | part instr. invité | hôte/invité | part du code x86 | etat ld/st | neg+tlb | saut | calcul |')
  print('|---|---|---|---|---|---|---|---|')
  for k, h in host.most_common(32):
      n = gcnt[k]; p = post[k]
      print('| %s | %.1f %% | %.1f | **%.1f %%** | %.1f | %.1f | %.1f | %.1f |' % (k, 100*n/G, h/n, 100*h/H,
            (p['etat-ld']+p['etat-st'])/n, (p['neg']+p['tlb'])/n, p['saut']/n, p['calcul']/n))


if __name__ == '__main__':
    main()
