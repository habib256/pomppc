#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""pmc5count.py LOG… — dans un journal QEMU `-d in_asm,out_asm` (hôte x86-64), compte par
offset de `env` le motif « mov OFF(%rbp),%r ; add $n,%r ; mov %r,OFF(%rbp) » : c'est le
`PMC5 += num_insns` que `pmu_count_insns` émet à chaque sortie de bloc quand
HFLAGS_INSN_CNT est posé (docs/ppc32-mttcg.md §3.1, tcg/0049 x-pmu-real).
Sortie : blocs, les offsets les plus fréquents, nombre de blocs touchés."""
import collections, re, subprocess, sys, tempfile


def tbs(fn):
    res = {}
    lines = open(fn, errors='replace').read().split('\n')
    i = 0
    while i < len(lines):
        if lines[i].startswith('IN:'):
            pc = int(lines[i + 1].split(':')[0], 16)
            i += 2
            n = 0
            while lines[i].startswith('OBJD-T:'):
                n += len(lines[i][7:].strip()) // 8
                i += 1
            hx = ''
            ins = True
            while i < len(lines) and not lines[i].startswith('IN:'):
                l = lines[i]
                if 'tb slow paths' in l or l.startswith('  data:'):
                    ins = False
                if ins and l.startswith('OBJD-H:'):
                    hx += l[7:].strip()
                i += 1
            res.setdefault(pc, (n, hx))
            continue
        i += 1
    return res


def dis(hx):
    with tempfile.NamedTemporaryFile(suffix='.bin') as f:
        f.write(bytes.fromhex(hx))
        f.flush()
        out = subprocess.run(['objdump', '-D', '-b', 'binary', '-mi386:x86-64',
                              '--no-show-raw-insn', f.name],
                             capture_output=True, text=True).stdout
    return [l.split('\t')[-1].strip() for l in out.split('\n')
            if re.match(r'\s+[0-9a-f]+:\t', l)]


for fn in sys.argv[1:]:
    t = tbs(fn)
    c = collections.Counter()
    touched = 0
    for pc, (n, hx) in t.items():
        ins = dis(hx)
        hit = 0
        for i in range(len(ins) - 2):
            m = re.match(r'mov\s+(0x[0-9a-f]+)\(%rbp\),%(\w+)', ins[i])
            if m and re.match(r'add\s+\$0x[0-9a-f]+,%' + m.group(2) + '$', ins[i + 1]) \
                    and re.match(r'mov\s+%' + m.group(2) + ',' + m.group(1) + r'\(%rbp\)',
                                 ins[i + 2]):
                c[m.group(1)] += 1
                hit = 1
        touched += hit
    print('%s : %d blocs ; motif ld/add/st env+OFF : %s ; blocs touchés %d'
          % (fn, len(t), c.most_common(3), touched))
