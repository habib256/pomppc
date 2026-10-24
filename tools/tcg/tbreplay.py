#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""tbreplay.py JOURNAL SORTIE.bin [--tous]

Extrait les blocs de code invité (« IN: » + OBJD-T) d'un journal QEMU
`-d in_asm` pour tools/tcg/tbreplay.c : suite de (u32 adresse, u32 n, n
mots), gros-boutiste. Un bloc dont l'adresse est déjà vue n'est gardé
qu'une fois. docs/tcg-g4.md section 36. Sans --tous, seulement les adresses sous
0x10000000 (celles que tbreplay.c peut tenir en linux-user) ; --tous : toutes, pour la
retraduction en mode système (POMPPC_TBREPLAY_SYS, docs/tcg-g4.md §41.4).
"""
import re
import struct
import sys

lines = open(sys.argv[1], errors='replace').read().split('\n')
out = open(sys.argv[2], 'wb')
TOUS = '--tous' in sys.argv[3:]
seen = set()
i = 0
nb = 0
while i < len(lines):
    if lines[i].startswith('IN:'):
        i += 1
        start = None
        ws = []
        while i < len(lines) and lines[i].strip() and not lines[i].startswith('OP'):
            m = re.match(r'0x([0-9a-f]+):', lines[i])
            if m and start is None:
                start = int(m.group(1), 16)
            if lines[i].startswith('OBJD-T:'):
                h = lines[i].split(':', 1)[1].strip()
                ws += [int(h[k:k + 8], 16) for k in range(0, len(h), 8)]
            i += 1
        if start is not None and ws and start not in seen and (TOUS or start < 0x10000000):
            seen.add(start)
            out.write(struct.pack('>II', start, len(ws)))
            out.write(struct.pack('>%dI' % len(ws), *ws))
            nb += 1
    else:
        i += 1
print('%d blocs' % nb)
