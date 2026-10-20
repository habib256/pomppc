#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""hotplug.py AVANT.csv APRÈS.csv NM.txt NOTE.txt [N] [--par K] — profil d'INSTRUCTIONS
du plugin GL entre deux instantanés du greffon TCG hotblocks (option snap=,
tools/tcg/hotblocks.c), sur une scène gltest (job plugprof, docs/d3-plugin-x86.md §10).

  NM.txt   : `nm -n` du binaire du plugin (job plugprof BUILD=1, out/nm.txt)
  NOTE.txt : une note du plugin de la même session (ligne
             « plugin : pomppc_geom_dispatch à 0x… ») — le décalage de chargement
  --par K  : divise les comptes par K (p. ex. le nombre de dessins de la scène)

Imprime le total d'instructions invitées exécutées dans le plugin, puis les N
fonctions les plus chères (propre : un bloc compte pour la fonction qui contient
son adresse de départ). Exact au bloc près, sans échantillonnage ; les autres
processus ne comptent pas, sauf s'ils exécutent du code à la même adresse
virtuelle (rien d'autre que gltest ne charge le plugin pendant la mesure)."""
import bisect
import re
import sys
from collections import defaultdict


def lit_csv(path):
    d = defaultdict(int)
    for l in open(path):
        if l.startswith("#") or l.startswith("pc,"):
            continue
        pc, ins, ex = l.strip().split(",")
        d[(int(pc, 16), int(ins))] += int(ex)
    return d


def main():
    args = [a for a in sys.argv[1:]]
    par = 1.0
    if "--par" in args:
        i = args.index("--par")
        par = float(args[i + 1])
        del args[i:i + 2]
    avant, apres, nmf, note = args[:4]
    n_top = int(args[4]) if len(args) > 4 else 40
    syms = []
    for l in open(nmf):
        m = re.match(r"^([0-9a-f]{8}) ([tT]) (\S+)", l)
        if m:
            syms.append((int(m.group(1), 16), m.group(3).lstrip("_")))
    syms.sort()
    ref = dict((n, a) for a, n in syms).get("pomppc_geom_dispatch")
    run = None
    for l in open(note, errors="replace"):
        m = re.search(r"pomppc_geom_dispatch à 0x([0-9a-f]+)", l)
        if m:
            run = int(m.group(1), 16)
    if ref is None or run is None:
        sys.exit("adresse de pomppc_geom_dispatch introuvable (nm ou note)")
    slide = run - ref
    lo = syms[0][0] + slide
    hi = syms[-1][0] + slide + 0x10000
    addrs = [a + slide for a, _ in syms]
    a0, a1 = lit_csv(avant), lit_csv(apres)
    par_fn = defaultdict(int)
    total = 0
    for (pc, ins), ex in a1.items():
        if not (lo <= pc < hi):
            continue
        n = (ex - a0.get((pc, ins), 0)) * ins
        if n <= 0:
            continue
        i = bisect.bisect_right(addrs, pc) - 1
        par_fn[syms[i][1] if i >= 0 else "?"] += n
        total += n
    print("plugin : %.0f instructions%s (décalage 0x%x)" %
          (total / par, " par unité de --par" if par != 1 else "", slide))
    for f, n in sorted(par_fn.items(), key=lambda x: -x[1])[:n_top]:
        print("%12.1f %5.1f%%  %s" % (n / par, 100.0 * n / total if total else 0, f))


if __name__ == "__main__":
    main()
