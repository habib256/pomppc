#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""fprun.py FICHIER [T0 T1] — résume la sortie du greffon fprun (tools/tcg/fprun.c).

Différence des instantanés les plus proches de T0 et T1 (secondes depuis le
démarrage de QEMU ; défaut : le premier et le dernier). Imprime, pour chaque
définition de « suite » (s1 : flottant consécutif ; s2 : séparé seulement par
du calcul entier ; s3 : aussi par des lectures), le nombre de suites, la
longueur moyenne pondérée par instruction, et la part des instructions
flottantes candidates qui sont dans une suite de longueur 1, 2, 3-4, 5-8, 9+.
Puis les exécutions par instruction (fadds … fcmpu) et par seconde."""
import sys

NOMS = ["fadds", "fsubs", "fmuls", "fmadds", "fmsubs", "fnmadds", "fnmsubs", "fcmpu"]


def lit(path):
    snaps, cur = [], None
    for l in open(path):
        p = l.split()
        if not p or p[0] == "END":
            continue
        if p[0] == "T":
            cur = {"t": float(p[1]), "h": {}, "op": {}}
            snaps.append(cur)
        elif p[0] == "op":
            cur["op"][int(p[1])] = int(p[2])
        else:
            cur["h"][(p[0], int(p[1]))] = int(p[2])
    return snaps


def proche(snaps, t):
    return min(snaps, key=lambda s: abs(s["t"] - t))


def main():
    snaps = lit(sys.argv[1])
    a = proche(snaps, float(sys.argv[2])) if len(sys.argv) > 3 else snaps[0]
    b = proche(snaps, float(sys.argv[3])) if len(sys.argv) > 3 else snaps[-1]
    dt = b["t"] - a["t"]
    print("fenêtre %.0f → %.0f s (%.0f s)" % (a["t"], b["t"], dt))
    tot = sum(b["op"].get(i, 0) - a["op"].get(i, 0) for i in range(8))
    for d in ("s1", "s2", "s3"):
        h = {k: b["h"].get((d, k), 0) - a["h"].get((d, k), 0) for k in range(64)}
        n = sum(h.values())
        insn = sum(k * v for k, v in h.items())
        if not n:
            continue
        tr = [(1, 1), (2, 2), (3, 4), (5, 8), (9, 63)]
        parts = ["%d-%d %4.1f %%" % (lo, hi, 100.0 * sum(k * h[k] for k in range(lo, hi + 1)) / insn)
                 for lo, hi in tr]
        print("%s : %.1f M suites/s, %.2f instr./suite (moyenne pondérée %.2f) ; par longueur : %s"
              % (d, n / dt / 1e6, insn / n, sum(k * k * v for k, v in h.items()) / insn,
                 ", ".join(parts)))
    print("candidates : %.1f M/s" % (tot / dt / 1e6))
    for i in range(8):
        c = b["op"].get(i, 0) - a["op"].get(i, 0)
        print("  %-8s %6.2f M/s  %5.1f %%" % (NOMS[i], c / dt / 1e6, 100.0 * c / max(1, tot)))


if __name__ == "__main__":
    main()
