#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""chargeparties.py CAMPAGNE (relevé dans CAMPAGNE/charge-releve.txt) — charge étrangère par partie : fenêtre = du départ de la partie
(journal.txt, après la barrière) à l'annonce de la suivante (avant sa barrière) (ou à la fin du relevé).
Écartée si > 1,5 cœur étranger sur >= 6 échantillons consécutifs (30 s soutenues)."""
import os, re, sys
def sec(h, _last=[0, 0]):
    """HH:MM:SS -> secondes, déroulées au passage de minuit (lignes dans l'ordre)."""
    x = sum(int(a) * b for a, b in zip(h.split(":"), (3600, 60, 1))) + _last[1]
    if x < _last[0] - 3600:
        _last[1] += 86400; x += 86400
    _last[0] = x
    return x
d = sys.argv[1]
j = open(os.path.join(d, "journal.txt")).read().splitlines()
parts = []  # (nom, debut)
cur = None
for l in j:
    m = re.match(r"(\d\d:\d\d:\d\d) (\S+-\d+) :", l)
    if m:
        cur = [m.group(2), m.group(1), m.group(1)]; parts.append(cur)
    m = re.match(r"calme : (\d\d:\d\d:\d\d)", l)
    if m and cur:
        cur[1] = m.group(1)
rel = []
for l in open(os.path.join(d, "charge-releve.txt")):
    if l.startswith("ATTENTE"):
        continue
    f = l.split()
    rel.append((sec(f[0]), float(f[f.index("etranger") + 1]), float(f[f.index("qemu") + 1])))
def rel_t(h):
    """heure du journal -> secondes du relevé (le relevé couvre toute la campagne)."""
    x = sum(int(a) * b for a, b in zip(h.split(":"), (3600, 60, 1)))
    while rel and x < rel[0][0] - 3600:
        x += 86400
    return x
for i, (nom, t0, _) in enumerate(parts):
    t1 = parts[i + 1][2] if i + 1 < len(parts) else "fin"
    a = rel_t(t0); b = rel_t(t1) if t1 != "fin" else 10**9
    x = [r for r in rel if a <= r[0] < b]
    if not x:
        continue
    run = best = 0
    for _, e, _ in x:
        run = run + 1 if e > 1.5 else 0; best = max(best, run)
    e = [r[1] for r in x]
    print("%-8s %s..%s n=%3d étranger moy %.2f max %.2f, plus longue série >1,5 : %d (%ds) qemu moy %.2f  %s" % (
        nom, t0, t1[:8], len(x), sum(e) / len(e), max(e), best, 5 * best,
        sum(r[2] for r in x) / len(x), "ÉCARTÉE" if best >= 6 else "valide"))
