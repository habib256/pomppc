#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""machonm.py BINAIRE… — noms de la table de symboles d'un Mach-O PowerPC 32 bits
(binaire universel compris : la tranche ppc), un par ligne, comme `nm -j`, sans
les symboles importés (sinon memcpy ou gle* importés par le plugin lui seraient comptés).
Pour les listes de tools/re/partfil.py quand ni l'invité (VM quotidienne, sans
outils de développement) ni l'hôte Linux (pas de llvm-nm) n'ont `nm`."""
import struct
import sys


def tranche(b):
    magic = struct.unpack(">I", b[:4])[0]
    if magic == 0xcafebabe:                       # universel : on prend ppc (18)
        n = struct.unpack(">I", b[4:8])[0]
        for i in range(n):
            cpu, _, off, size, _ = struct.unpack(">iiIII", b[8 + 20 * i:28 + 20 * i])
            if cpu == 18:
                return b[off:off + size]
        return None
    return b if magic == 0xfeedface else None


def noms(b):
    b = tranche(b)
    if b is None:
        return []
    ncmds = struct.unpack(">I", b[16:20])[0]
    p = 28
    for _ in range(ncmds):
        cmd, size = struct.unpack(">II", b[p:p + 8])
        if cmd == 2:                              # LC_SYMTAB
            symoff, nsyms, stroff, _ = struct.unpack(">IIII", b[p + 8:p + 24])
            out = []
            for i in range(nsyms):
                strx, typ = struct.unpack(">IB", b[symoff + 12 * i:symoff + 12 * i + 5])
                if typ & 0xe0 or typ & 0x0e in (0, 0x0c):  # stabs, importés (N_UNDF, N_PBUD)
                    continue
                e = b.index(b"\0", stroff + strx)
                s = b[stroff + strx:e].decode("latin-1")
                if s:
                    out.append(s)
            return out
        p += size
    return []


for f in sys.argv[1:]:
    for s in noms(open(f, "rb").read()):
        print(s)
