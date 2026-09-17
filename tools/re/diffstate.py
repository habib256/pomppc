#!/usr/bin/env python3
"""diffstate.py — differ les vidages d'état de GLEngine produits par le traceur
du plugin POMPPC, et y chercher des valeurs flottantes big-endian.

Les vidages sont écrits par `pomppc_dump()` (guest/gldriver/pomppc_gld.c) dans
le dossier `POMPPC_GLTRACE`, sous le nom `NNNN-<étiquette>.bin` où `NNNN` est un
numéro de séquence global. Avec `POMPPC_GLTRACE_STATE=1`, chaque `glClear` vide
le bloc d'état GL (`clear-glstate`, base notée GS) et les objets qu'il pointe
(`clear-vao`, `clear-matf`, `clear-matb`, `clear-pp`, `clear-gctxlow`).

Le mode d'emploi des sondes est : un réglage GL par `glClear`, puis on diffe les
vidages successifs d'une même étiquette pour voir *quel octet* chaque appel a
bougé (docs/re/tableaux-de-sommets.md §7, docs/re/etat-tcl.md).

Usage :

    # diff des vidages successifs d'une étiquette (par défaut clear-glstate)
    tools/re/diffstate.py diff DOSSIER [--tag clear-glstate] [--base 0]
                               [--from N] [--to N] [--min OFF] [--max OFF]
                               [--labels fichier.txt]

    # chercher des flottants big-endian (et les entiers correspondants)
    tools/re/diffstate.py find DOSSIER/0007-clear-glstate.bin 0.0625 0.125 60 0.5

    # vue hexadécimale annotée d'une plage
    tools/re/diffstate.py show DOSSIER/0007-clear-glstate.bin 0x24c0 0x80

    # lister les vidages d'un dossier
    tools/re/diffstate.py list DOSSIER

Options utiles :
  --base OFF   ajoute OFF à tous les offsets affichés (0 pour un vidage
               `clear-glstate`, dont l'offset 0 est déjà GS+0)
  --labels F   fichier « offset  nom » (une ligne par étape) donnant le nom de
               l'appel GL de chaque diff : la ligne k nomme le diff k→k+1.
  --all        n'omet pas les plages « bruyantes » connues (compteurs, horloges)

Sortie d'un diff : une ligne par plage d'octets contiguë modifiée,
`+0xOFFSET  n  avant → après   [flottants]`, les flottants étant donnés quand la
plage est alignée sur 4 et fait un multiple de 4 octets.
"""

import os
import re
import struct
import sys


def dumps(folder, tag):
    """Vidages `NNNN-<tag>.bin` du dossier, triés par numéro de séquence."""
    out = []
    for name in os.listdir(folder):
        m = re.match(r"^(\d+)-(.+)\.bin$", name)
        if m and m.group(2) == tag:
            out.append((int(m.group(1)), os.path.join(folder, name)))
    out.sort()
    return out


def fmt_f(b):
    """Interprétation flottante big-endian d'une suite d'octets alignée."""
    if len(b) % 4 or not b:
        return ""
    vals = struct.unpack(">%df" % (len(b) // 4), b)
    return " ".join("%g" % v for v in vals)


def ranges(a, b, lo, hi):
    """Plages contiguës où a et b diffèrent, dans [lo, hi)."""
    n = min(len(a), len(b), hi)
    i = lo
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            # recoller deux plages séparées par moins de 4 octets identiques
            while j + 4 < n and any(a[k] != b[k] for k in range(j, min(j + 4, n))):
                j += 1
            yield i, j
            i = j
        else:
            i += 1


def hexs(b, cap=24):
    s = b[:cap].hex()
    return s + ("…" if len(b) > cap else "")


def cmd_diff(argv):
    folder = argv[0]
    tag = "clear-glstate"
    base = 0
    lo, hi = 0, 1 << 30
    first, last = 1, 1 << 30
    labels = []
    i = 1
    while i < len(argv):
        k = argv[i]
        if k == "--tag":
            tag = argv[i + 1]; i += 2
        elif k == "--base":
            base = int(argv[i + 1], 0); i += 2
        elif k == "--min":
            lo = int(argv[i + 1], 0); i += 2
        elif k == "--max":
            hi = int(argv[i + 1], 0); i += 2
        elif k == "--from":
            first = int(argv[i + 1], 0); i += 2
        elif k == "--to":
            last = int(argv[i + 1], 0); i += 2
        elif k == "--labels":
            labels = [l.rstrip("\n") for l in open(argv[i + 1])]; i += 2
        elif k == "--all":
            i += 1
        else:
            sys.exit("option inconnue : " + k)
    ds = dumps(folder, tag)
    if not ds:
        sys.exit("aucun vidage « %s » dans %s" % (tag, folder))
    print("%d vidages « %s » (%s … %s)" % (len(ds), tag, ds[0][1], ds[-1][1]))
    prev = None
    step = 0
    for seq, path in ds:
        cur = open(path, "rb").read()
        step += 1
        if prev is not None and first <= step - 1 <= last:
            name = labels[step - 2] if len(labels) >= step - 1 else ""
            print("\n--- étape %d → %d  (%04d)  %s" % (step - 1, step, seq, name))
            nb = 0
            for a, b in ranges(prev, cur, lo, hi):
                nb += 1
                if nb > 200:
                    print("  … (plus de 200 plages : état instable ?)")
                    break
                fa, fb = fmt_f(prev[a:b]), fmt_f(cur[a:b])
                fl = ("   [%s → %s]" % (fa, fb)) if fa else ""
                print("  +0x%04x  %2d  %s → %s%s"
                      % (base + a, b - a, hexs(prev[a:b]), hexs(cur[a:b]), fl))
            if nb == 0:
                print("  (rien)")
        prev = cur


def cmd_find(argv):
    path = argv[0]
    data = open(path, "rb").read()
    for arg in argv[1:]:
        pats = []
        try:
            v = float(arg)
            pats.append(("float", struct.pack(">f", v)))
            if v == int(v) and abs(v) < 2 ** 31:
                pats.append(("i32", struct.pack(">i", int(v))))
                if 0 <= v < 65536:
                    pats.append(("u16", struct.pack(">H", int(v))))
        except ValueError:
            n = int(arg, 0)
            pats.append(("i32", struct.pack(">i", n)))
            if 0 <= n < 65536:
                pats.append(("u16", struct.pack(">H", n)))
        for kind, pat in pats:
            hits = []
            i = data.find(pat)
            while i >= 0:
                hits.append(i)
                i = data.find(pat, i + 1)
            if hits:
                print("%-10s %-6s %s" % (arg, kind,
                      " ".join("+0x%04x" % h for h in hits[:40])
                      + (" …" if len(hits) > 40 else "")))
            else:
                print("%-10s %-6s (absent)" % (arg, kind))


def cmd_show(argv):
    data = open(argv[0], "rb").read()
    off = int(argv[1], 0)
    n = int(argv[2], 0) if len(argv) > 2 else 0x40
    for i in range(off, min(off + n, len(data)), 16):
        row = data[i:i + 16]
        print("+0x%04x  %-32s %-32s  %s"
              % (i, row[:8].hex(), row[8:].hex(), fmt_f(row)))


def cmd_list(argv):
    folder = argv[0]
    seen = {}
    for name in sorted(os.listdir(folder)):
        m = re.match(r"^(\d+)-(.+)\.bin$", name)
        if m:
            seen.setdefault(m.group(2), []).append(int(m.group(1)))
    for tag in sorted(seen):
        v = seen[tag]
        print("%-20s %4d vidages  (séquences %d … %d)" % (tag, len(v), v[0], v[-1]))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    rest = sys.argv[2:]
    {"diff": cmd_diff, "find": cmd_find, "show": cmd_show, "list": cmd_list}.get(
        cmd, lambda a: sys.exit("commande inconnue : " + cmd))(rest)


if __name__ == "__main__":
    main()
