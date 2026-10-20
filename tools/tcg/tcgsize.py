#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""tcgsize.py JOURNAL [JOURNAL2] [--pc LO-HI ...] [--starts JOURNAL0] [--top N]

Taille du code x86 généré par instruction PowerPC, depuis un journal QEMU
`-d in_asm,out_asm` (formes OBJD-T / OBJD-H : sans désassembleur intégré).
Chaque bloc « OUT: » est découpé aux marqueurs « -- guest addr 0x… » ; la
part d'une instruction = les octets entre son marqueur et le suivant (le
prologue du bloc est compté avec la première, les chemins lents en fin de
bloc à part). Les mots d'instruction viennent des blocs « IN: » (décodage
minimal : flottant, chargements/rangements, sinon « int/<opcode> »).

Avec deux journaux : les deux colonnes côte à côte (même code invité, deux
binaires ou deux réglages). --pc restreint aux blocs dont l'adresse de
départ tombe dans une des plages ; --starts aux blocs qui commencent là où
commence un bloc « IN: » de JOURNAL0, chacun compté une fois (tbreplay :
pas les blocs créés par une sortie en cours de bloc). docs/tcg-g4.md
section 36.
"""
import re
import sys
from collections import defaultdict

A59 = {18: 'fdivs', 20: 'fsubs', 21: 'fadds', 25: 'fmuls', 28: 'fmsubs',
       29: 'fmadds', 30: 'fnmsubs', 31: 'fnmadds', 22: 'fsqrts', 24: 'fres'}
A63 = {18: 'fdiv', 20: 'fsub', 21: 'fadd', 25: 'fmul', 28: 'fmsub',
       29: 'fmadd', 30: 'fnmsub', 31: 'fnmadd', 23: 'fsel', 22: 'fsqrt',
       26: 'frsqrte'}
X63 = {0: 'fcmpu', 12: 'frsp', 14: 'fctiw', 15: 'fctiwz', 32: 'fcmpo',
       40: 'fneg', 72: 'fmr', 136: 'fnabs', 264: 'fabs', 583: 'mffs',
       711: 'mtfsf', 134: 'mtfsfi', 38: 'mtfsb1', 70: 'mtfsb0', 64: 'mcrfs'}
D = {48: 'lfs', 49: 'lfsu', 50: 'lfd', 51: 'lfdu', 52: 'stfs', 53: 'stfsu',
     54: 'stfd', 55: 'stfdu', 32: 'lwz', 36: 'stw', 14: 'addi', 15: 'addis'}
X31 = {535: 'lfsx', 567: 'lfsux', 599: 'lfdx', 631: 'lfdux', 663: 'stfsx',
       695: 'stfsux', 727: 'stfdx', 759: 'stfdux'}


def name(w):
    op = w >> 26
    if op == 59:
        return A59.get((w >> 1) & 31, 'fp59/%d' % ((w >> 1) & 31))
    if op == 63:
        x = (w >> 1) & 1023
        if x in X63:
            return X63[x]
        return A63.get((w >> 1) & 31, 'fp63/%d' % x)
    if op == 31 and ((w >> 1) & 1023) in X31:
        return X31[(w >> 1) & 1023]
    if op in D:
        return D[op]
    return 'int/%d' % op


def starts_of(path):
    st = set()
    lines = open(path, errors='replace').read().split('\n')
    for i, ln in enumerate(lines):
        if ln.startswith('IN:') and i + 1 < len(lines):
            m = re.match(r'0x([0-9a-f]+):', lines[i + 1])
            if m:
                st.add(int(m.group(1), 16))
    return st


def parse(path, ranges, starts=None):
    insn = {}               # guest address -> word
    sizes = []              # (name, bytes)
    slow = 0
    lines = open(path, errors='replace').read().split('\n')
    i = 0
    n = len(lines)
    while i < n:
        ln = lines[i]
        if ln.startswith('IN:'):
            i += 1
            addr = None
            hexs = ''
            while i < n and lines[i].strip() and not lines[i].startswith('OP'):
                m = re.match(r'0x([0-9a-f]+):', lines[i])
                if m:
                    if addr is not None and hexs:
                        for k in range(0, len(hexs), 8):
                            insn[addr + k // 2] = int(hexs[k:k + 8], 16)
                    addr = int(m.group(1), 16)
                    hexs = ''
                elif lines[i].startswith('OBJD-T:'):
                    hexs += lines[i].split(':', 1)[1].strip()
                i += 1
            if addr is not None and hexs:
                for k in range(0, len(hexs), 8):
                    insn[addr + k // 2] = int(hexs[k:k + 8], 16)
            continue
        if ln.startswith('OUT:'):
            i += 1
            cur = None
            first = None
            acc = 0
            in_slow = False
            while i < n and not lines[i].startswith('---') and not lines[i].startswith('IN:'):
                l2 = lines[i]
                m = re.match(r'\s+-- guest addr 0x([0-9a-f]+)', l2)
                if m:
                    if cur is not None:
                        sizes.append((cur, acc, first))
                    cur = int(m.group(1), 16)
                    if first is None:
                        first = cur
                    acc = 0
                    in_slow = False
                elif re.match(r'\s+-- tb slow paths', l2):
                    if cur is not None:
                        sizes.append((cur, acc, first))
                    cur = None
                    in_slow = True
                    acc = 0
                elif l2.startswith('OBJD-H:'):
                    b = len(l2.split(':', 1)[1].strip()) // 2
                    if in_slow:
                        slow += b
                    else:
                        acc += b
                i += 1
            if cur is not None:
                sizes.append((cur, acc, first))
            continue
        i += 1
    res = defaultdict(lambda: [0, 0])
    done = set()
    cur_first = None
    for a, b, first in sizes:
        if ranges and not any(lo <= first < hi for lo, hi in ranges):
            continue
        if starts is not None:
            if first not in starts:
                continue
            if a == first:
                if first in done:
                    cur_first = None
                    continue
                done.add(first)
                cur_first = first
            elif cur_first != first:
                continue
        w = insn.get(a)
        nm = name(w) if w is not None else '?'
        res[nm][0] += 1
        res[nm][1] += b
    return res, slow


def main():
    args = sys.argv[1:]
    ranges = []
    top = 40
    files = []
    starts = None
    while args:
        a = args.pop(0)
        if a == '--pc':
            lo, hi = args.pop(0).split('-')
            ranges.append((int(lo, 16), int(hi, 16)))
        elif a == '--starts':
            starts = starts_of(args.pop(0))
        elif a == '--top':
            top = int(args.pop(0))
        else:
            files.append(a)
    rs = [parse(f, ranges, starts) for f in files]
    names = sorted(set().union(*[r[0].keys() for r in rs]),
                   key=lambda k: -rs[0][0].get(k, [0, 0])[1])
    hdr = 'instruction'.ljust(12) + ''.join(
        ('  n   octets/insn (%d)' % (j + 1)).rjust(26) for j in range(len(rs)))
    print(hdr)
    tot = [[0, 0] for _ in rs]
    for k in names[:top]:
        row = k.ljust(12)
        for j, (r, _) in enumerate(rs):
            c, b = r.get(k, [0, 0])
            row += ('%7d %8.1f' % (c, b / c if c else 0)).rjust(26)
        print(row)
    for j, (r, s) in enumerate(rs):
        c = sum(v[0] for v in r.values())
        b = sum(v[1] for v in r.values())
        print('journal %d : %d instructions, %.1f octets/insn en ligne, %d octets de chemins lents'
              % (j + 1, c, b / c if c else 0, s))


if __name__ == '__main__':
    main()
