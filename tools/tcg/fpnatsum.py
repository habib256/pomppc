#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""fpnatsum.py DOSSIER… — résume les bancs `fptest banc` rangés par
tools/tcg/fpnatab.sh (<dossier>/<mode>-<k>/run.txt) : par mode, médiane de
chaque banc sur tous les tours de tous les démarrages, min..max, et ns par
opération (chaîne : N×4 opérations, sommets : N×16, fcmpu : N×2)."""
import collections
import os
import re
import statistics
import sys

RX = re.compile(r"^banc : (\w+) (\d+) x (\d+) (\d+) ms")


def main():
    res = collections.defaultdict(lambda: collections.defaultdict(list))
    ops = {}
    ordre = []
    for d in sys.argv[1:]:
        for sub in sorted(os.listdir(d)):
            p = os.path.join(d, sub, "run.txt")
            if not os.path.exists(p):
                continue
            mode = sub.rsplit("-", 1)[0]
            if mode not in ordre:
                ordre.append(mode)
            for l in open(p, errors="replace"):
                m = RX.match(l)
                if m:
                    b = m.group(1)
                    res[mode][b].append(int(m.group(4)))
                    ops[b] = int(m.group(2)) * int(m.group(3))
    bancs = ["chaine", "sommets", "fcmpu"]
    print("%-8s" % "mode" + "".join("  %-34s" % b for b in bancs))
    for mode in ordre:
        cells = []
        for b in bancs:
            v = res[mode].get(b)
            if not v:
                cells.append("  %-34s" % "-")
                continue
            med = statistics.median(v)
            cells.append("  %-34s" % ("%d ms (%d..%d, n=%d) %.2f ns" % (
                med, min(v), max(v), len(v), med * 1e6 / ops[b])))
        print("%-8s" % mode + "".join(cells))


if __name__ == "__main__":
    main()
