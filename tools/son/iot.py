#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""Résumé d'iotrace.csv : par tranche de 10 s, appels, durée moyenne/max de
l'IOProc, marge min, sauts de temps d'échantillon ; puis la liste des sauts."""
import sys, collections

rows = []
t_debut = None
for l in open(sys.argv[1]):
    if l.startswith("# debut"):
        t_debut = float(l.split()[2])
        continue
    if not l[0].isdigit():
        continue
    n, e, d, ns, os_, m, s, b = l.strip().split(",")
    rows.append((int(n), float(e) / 1e6, float(d) / 1000, float(ns), float(os_), float(m) / 1000, float(s), int(b)))
w = collections.defaultdict(list)
for r in rows:
    w[int(r[1] // 10)].append(r)
print("début mural %.3f" % (t_debut or 0))
print("  t(s) appels  durée_moy  durée_max  marge_min  écart_max  sauts")
prev = None
for k in sorted(w):
    v = w[k]
    ecarts = []
    for r in v:
        if prev is not None:
            ecarts.append((r[1] - prev) * 1000)
        prev = r[1]
    print("%6d %6d %9.1f %9.1f %9.1f %9.1f %6d" % (
        k * 10, len(v), sum(x[2] for x in v) / len(v), max(x[2] for x in v),
        min(x[5] for x in v), max(ecarts) if ecarts else 0, sum(1 for x in v if x[6] != 0)))
if "-s" in sys.argv:
    for r in rows:
        if r[6] != 0:
            print("saut n=%d t=%.3f s  durée=%.1f ms marge=%.1f ms  saut=%+.0f trames" % (r[0], r[1], r[2], r[5], r[6]))
