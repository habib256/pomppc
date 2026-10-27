#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""selfprof.py PREFIXE --map PERFMAP --qemu BINAIRE --log RUN_TIGER.LOG [--asm JOURNAL] [--top N]
— lit les échantillons de l'auto-profileur de QEMU (POMPPC_SELFPROF=PREFIXE, fils vCPU,
1 kHz de temps CPU, sans perf : docs/vitesse-doom3-x86.md §18) et les range :

  1. code généré (blocs de la carte -perfmap) contre fonctions de QEMU (symboles du binaire,
     base de chargement lue sur la ligne « tcg: texte 0x… » de run_tiger.log) ;
  2. les blocs JIT et les fonctions les plus lourds ;
  3. avec --asm (journal -d in_asm,out_asm -dfilter de la MÊME partie) : chaque échantillon
     est rendu à l'instruction x86 interrompue, donc à l'instruction invitée et à son opcode
     PowerPC. C'est le temps réel, latences comprises, et non un compte d'instructions.
     Le rip noté est celui de l'instruction pas encore retirée : une attente de latence se
     voit sur l'instruction qui consomme le résultat lent.
"""
import argparse
import bisect
import collections
import glob
import re
import struct
import subprocess
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])


def rips(prefix):
    out = []
    for f in sorted(glob.glob(prefix + '.*.bin')):
        b = open(f, 'rb').read()
        out += struct.unpack('<%dQ' % (len(b) // 8), b)
    return out


def perfmap(path):
    m = []
    for line in open(path):
        a, s, n = line.split(None, 2)
        m.append((int(a, 16), int(s, 16), n.strip()))
    m.sort()
    return m


def symbols(binary, base):
    out = subprocess.run(['nm', '-C', '--defined-only', '-S', binary],
                         capture_output=True, text=True).stdout
    m = []
    for line in out.splitlines():
        p = line.split(None, 3)
        if len(p) == 4 and p[2] in 'tTwW':
            m.append((base + int(p[0], 16), int(p[1], 16), p[3]))
    m.sort()
    return m


def find(m, keys, a):
    i = bisect.bisect_right(keys, a) - 1
    if i >= 0 and a < m[i][0] + max(m[i][1], 1):
        return m[i]
    return None


def asm_index(path):
    """{rip hôte: (pc invité, mot invité, mnémonique x86, texte)} pour le journal."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    idx = {}
    words, inpc, seg = [], None, None

    def flush():
        if seg and seg[2]:
            g = seg[0]
            w = None
            if g is not None and inpc is not None:
                k = (g - inpc) // 4
                if 0 <= k < len(words):
                    w = words[k]
            for ins in md.disasm(bytes(seg[2]), seg[1]):
                idx[ins.address] = (g, w, ins.mnemonic, ins.mnemonic + ' ' + ins.op_str)
    mode = None
    pending = None
    for line in open(path, errors='replace'):
        if line.startswith('IN:'):
            flush(); seg = None
            mode, words, inpc = 'in', [], None
            continue
        if line.startswith('OUT:'):
            mode = 'out'
            continue
        m = re.match(r'(0x[0-9a-f]+):\s*$', line)
        if m:
            a = int(m.group(1), 16)
            if mode == 'in' and inpc is None:
                inpc = a
            elif mode == 'out':
                flush()
                seg = [pending, a, bytearray()]
            continue
        if line.startswith('OBJD-T:') and mode == 'in':
            b = bytes.fromhex(line.split()[1])
            words += [int.from_bytes(b[i:i + 4], 'big') for i in range(0, len(b), 4)]
        elif line.startswith('OBJD-H:') and mode == 'out' and seg is not None:
            seg[2] += bytes.fromhex(line.split()[1])
        else:
            g = re.match(r'\s+-- guest addr 0x([0-9a-f]+)', line)
            if g:
                pending = int(g.group(1), 16)
            elif '-- tb slow' in line or 'tb prologue' in line and 'guest addr' not in line:
                pending = None
    flush()
    return idx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prefix')
    ap.add_argument('--map', required=True)
    ap.add_argument('--qemu', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--asm')
    ap.add_argument('--top', type=int, default=25)
    a = ap.parse_args()

    r = rips(a.prefix)
    if not r:
        sys.exit('aucun échantillon sous %s.*.bin' % a.prefix)
    base = None
    for line in open(a.log, errors='replace'):
        m = re.search(r'tcg: texte (0x[0-9a-f]+)-', line)
        if m:
            base = int(m.group(1), 16)
    if base is None:
        sys.exit('ligne « tcg: texte » absente de %s' % a.log)
    jm = perfmap(a.map); jk = [x[0] for x in jm]
    sm = symbols(a.qemu, base); sk = [x[0] for x in sm]

    n = len(r)
    jit = collections.Counter(); fn = collections.Counter(); ailleurs = 0
    jit_rips = []
    for x in r:
        e = find(jm, jk, x)
        if e:
            jit[e[2]] += 1
            jit_rips.append(x)
            continue
        e = find(sm, sk, x)
        if e:
            fn[e[2]] += 1
        else:
            ailleurs += 1
    nj = sum(jit.values())
    print('%d échantillons (≈ %.1f s de vCPU) : code généré %.1f %%, QEMU %.1f %%, ailleurs (libc, noyau…) %.1f %%'
          % (n, n / 1000, 100 * nj / n, 100 * sum(fn.values()) / n, 100 * ailleurs / n))
    print('\n## fonctions de QEMU')
    for k, v in fn.most_common(a.top):
        print('%6.2f %%  %s' % (100 * v / n, k[:100]))
    print('\n## blocs JIT (somme des traductions par adresse invitée)')
    for k, v in jit.most_common(a.top):
        print('%6.2f %%  %s' % (100 * v / n, k))

    if not a.asm:
        return
    import codeop as C   # noms des opcodes PowerPC (tools/tcg/codeop.py)
    idx = asm_index(a.asm)
    couvert = 0
    par_op = collections.Counter(); par_mn = collections.Counter()
    par_ins = collections.Counter()
    for x in jit_rips:
        e = idx.get(x)
        if not e:
            continue
        couvert += 1
        g, w, mn, txt = e
        op = C.name(w) if w is not None else ('(lent/prologue)' if g is None else '?')
        par_op[op] += 1
        par_mn[mn] += 1
        par_ins[(g, op, txt)] += 1
    print('\n## instructions x86 du journal : %d échantillons (%.1f %% du code généré)'
          % (couvert, 100 * couvert / max(nj, 1)))
    print('\n### par opcode PowerPC (part des échantillons couverts)')
    for k, v in par_op.most_common(a.top):
        print('%6.2f %%  %s' % (100 * v / couvert, k))
    print('\n### par mnémonique x86 interrompue')
    for k, v in par_mn.most_common(a.top):
        print('%6.2f %%  %s' % (100 * v / couvert, k))
    print('\n### instructions x86 les plus lourdes')
    for (g, op, txt), v in par_ins.most_common(a.top * 2):
        print('%6.2f %%  %-10s %-8s %s' % (100 * v / couvert, hex(g) if g else '-', op, txt))


if __name__ == '__main__':
    main()
