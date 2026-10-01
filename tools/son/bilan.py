#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Bilan d'une partie : secondes où l'invité envoie du son (rms > 0), trames
nulles et trous >= 10 ms dans ces secondes, silence inséré, tampons hôte vides.
usage : bilan.py diag.log t0 t1   (t0/t1 : horloge murale en s, bornes de la partie)"""
import sys

path = sys.argv[1]
if sys.argv[2].startswith("run:"):
    base = float(open(sys.argv[2][4:] + "/t0").read())
    t0, t1 = base + float(sys.argv[3]), base + float(sys.argv[4])
else:
    t0, t1 = float(sys.argv[2]), float(sys.argv[3])
n = son = zero = trous = sil = vide = cbmax = 0
dma = 0
for l in open(path, errors="replace"):
    if not l.startswith("SEC"):
        continue
    f = l.split()
    t = int(f[1]) / 1000
    if not (t0 <= t <= t1):
        continue
    d = dict(x.split("=", 1) for x in f[2:] if "=" in x)
    n += 1
    if float(d.get("rms", "0")) > 0 and int(d["dma_Bps"]) > 0:
        son += 1
        zero += int(d["zero"])
        trous += int(d["trous"])
    sil += int(d["sil"].split("/")[0]) if int(d["dma_Bps"]) > 0 else 0
    vide += int(d["ca_vide"])
    cbmax = max(cbmax, float(d["cbgap_max"]))
print("%d s relevées, %d avec du son de l'invité ; trames nulles %d (%.1f %%), trous >= 10 ms %d ; "
      "silence inséré (DMA actif) %d trames ; tampons hôte vides %d ; écart max entre callbacks %.1f ms"
      % (n, son, zero, 100.0 * zero / max(1, son * 44100), trous, sil, vide, cbmax))
