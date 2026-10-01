#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""ZPOS d'un journal entre deux instants relatifs à t0 d'une partie.
usage : zpos.py diag.log rundir debut fin"""
import sys, collections
log, run, a, b = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
t0 = float(open(run + "/t0").read())
fins = collections.Counter()
longs = []
for l in open(log, errors="replace"):
    if l.startswith("ZPOS"):
        f = l.split()
        t = int(f[1]) / 1000 - t0
        if a <= t <= b:
            fin = int(f[2].split("=")[1]); lg = int(f[3].split("=")[1])
            fins[fin % 4096] += 1
            longs.append(lg)
            if len(longs) <= 15:
                print("%.2f fin=%d (mod 4096 = %d) long=%d" % (t, fin, fin % 4096, lg))
print("fins mod 4096 :", fins.most_common(6))
if longs:
    longs.sort()
    print("n=%d longueur médiane %d trames, min %d, max %d" % (len(longs), longs[len(longs)//2], longs[0], longs[-1]))
