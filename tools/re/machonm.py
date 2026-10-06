#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""machonm.py [-n] BINAIRE… — table de symboles d'un Mach-O PowerPC 32 bits
(binaire universel compris : la tranche ppc), sans `nm`.

    machonm.py BINAIRE…      noms, un par ligne, comme `nm -j`, sans les
                             symboles importés (sinon memcpy ou gle* importés
                             par le plugin lui seraient comptés)
    machonm.py -n BINAIRE    « adresse type nom » triés par adresse, comme
                             `nm -n` d'Apple (T/t : __TEXT,__text ; D/d :
                             __DATA,__data ; B/b : bss/common ; S/s : autre
                             section ; A : absolu)

Pour les listes de tools/re/partfil.py quand ni l'invité (VM quotidienne, sans
outils de développement) ni l'hôte Linux (pas de llvm-nm) n'ont `nm` ; et,
depuis le 06/10/2026, pour tools/re/kpanic.py et tools/endurance/symbolise.py :
le `nm` de binutils du PC ne lit pas le Mach-O (« file format not
recognized ») et rendait une table VIDE sans erreur — un gel sur le PC
n'aurait été ni symbolisé ni même lu (panicstr introuvable)."""
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


def _commandes(b):
    ncmds = struct.unpack(">I", b[16:20])[0]
    p = 28
    for _ in range(ncmds):
        cmd, size = struct.unpack(">II", b[p:p + 8])
        yield cmd, p
        p += size


def noms(b):
    b = tranche(b)
    if b is None:
        return []
    for cmd, p in _commandes(b):
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
    return []


class MachO:
    """Un Mach-O ppc 32 bits lu en entier : sections, segments, symboles,
    octets à une adresse virtuelle."""

    def __init__(self, chemin):
        b = tranche(open(chemin, "rb").read())
        if b is None:
            raise ValueError("%s : pas un Mach-O ppc 32 bits" % chemin)
        self.b = b
        self.sections = [None]                    # n_sect commence à 1
        self.segments = []                        # (nom, vmaddr, vmsize, fileoff, filesize)
        self.symtab = None
        for cmd, p in _commandes(b):
            if cmd == 1:                          # LC_SEGMENT
                seg = b[p + 8:p + 24].split(b"\0", 1)[0].decode()
                vma, vms, fo, fs = struct.unpack(">IIII", b[p + 24:p + 40])
                nsects = struct.unpack(">I", b[p + 48:p + 52])[0]
                self.segments.append((seg, vma, vms, fo, fs))
                q = p + 56
                for _ in range(nsects):
                    sn = b[q:q + 16].split(b"\0", 1)[0].decode()
                    sg = b[q + 16:q + 32].split(b"\0", 1)[0].decode()
                    self.sections.append((sg, sn))
                    q += 68
            elif cmd == 2:
                self.symtab = struct.unpack(">IIII", b[p + 8:p + 24])

    def symboles(self):
        """[(adresse, lettre, nom)] des symboles définis, triés par adresse."""
        if not self.symtab:
            return []
        symoff, nsyms, stroff, _ = self.symtab
        b, out = self.b, []
        for i in range(nsyms):
            strx, typ, sect, _, val = struct.unpack(">IBBhI", b[symoff + 12 * i:symoff + 12 * i + 12])
            if typ & 0xe0:                        # stabs
                continue
            t = typ & 0x0e
            if t == 0x0e:                         # N_SECT
                sg, sn = self.sections[sect] if 0 < sect < len(self.sections) else ("", "")
                l = {"__text": "t", "__data": "d", "__bss": "b", "__common": "b"}.get(sn, "s")
                if sn == "__text" and sg != "__TEXT":
                    l = "s"
            elif t == 0x02:                       # N_ABS
                l = "a"
            else:                                 # N_UNDF, N_PBUD, N_INDR
                continue
            if typ & 0x01:                        # N_EXT
                l = l.upper()
            e = b.index(b"\0", stroff + strx)
            s = b[stroff + strx:e].decode("latin-1")
            if s:
                out.append((val, l, s))
        out.sort()
        return out

    def octets(self, adresse, n):
        """n octets du fichier à l'adresse virtuelle `adresse` (b"" hors segment)."""
        for _, vma, _, fo, fs in self.segments:
            if vma <= adresse < vma + fs:
                d = fo + adresse - vma
                return self.b[d:d + min(n, vma + fs - adresse)]
        return b""

    def fin_texte(self):
        """Fin du segment __TEXT (adresse virtuelle), ou None."""
        for nom, vma, vms, _, _ in self.segments:
            if nom == "__TEXT":
                return vma + vms
        return None


def nm(chemin):
    """Comme `nm -n` : [(adresse, lettre, nom)] ; [] si le fichier n'est pas lisible."""
    try:
        return MachO(chemin).symboles()
    except (OSError, ValueError, struct.error, IndexError):
        return []


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "-n":
        for f in a[1:]:
            for v, l, s in nm(f):
                print("%08x %s %s" % (v, l, s))
    else:
        for f in a:
            for s in noms(open(f, "rb").read()):
                print(s)
