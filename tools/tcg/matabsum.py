#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""matabsum.py CAMPAGNE — bilan d'un A/B entrelacé de tools/tcg/matab.sh.

Une ligne par partie (ms/image de la cellule, fenêtre, charge de l'hôte avant
et après la mesure, autres QEMU), puis par mode : n, médiane, min..max, écart
relatif min..max ; et le rapport des médianes. Plusieurs jeux par partie
(`matab.sh … nx,prey fen`) : un bilan par cellule (jeu-mode). Les parties dont la cellule
n'est pas verte ou dont la charge dit « autre(s) QEMU » > 0 sont signalées."""
import csv, os, re, statistics, sys

camp = sys.argv[1]
par_mode = {}
lignes = []
for d in sorted(os.listdir(camp)):
    f = os.path.join(camp, d, "resultats.csv")
    if not os.path.isfile(f) or "-" not in d:
        continue
    mode0, k = d.rsplit("-", 1)
    for r in csv.DictReader(open(f)):
        cel = os.path.basename(r.get("dossier", "")) or "?"
        mode = (mode0, cel)
        try:
            ms = float(r["ms_image"])
        except (KeyError, ValueError):
            lignes.append("%-10s %-3s %-9s pas de mesure : %s" % (mode0, k, cel, r.get("motifs", "")))
            continue
        ch = r.get("charge_hote", "")
        charges = [float(x.replace(",", ".")) for x in re.findall(r"charge ([0-9]+,[0-9]+)", ch)]
        autres = [int(x) for x in re.findall(r"([0-9]+) autre\(s\) QEMU", ch)]
        drap = []
        if r.get("verdict") != "vert":
            drap.append(r.get("verdict", "?"))
        if any(autres):
            drap.append("AUTRE QEMU")
        lignes.append("%-10s %-3s %-9s %6.1f ms/image  fenêtre %-11s charge %s %s" % (
            mode0, k, cel, ms, r.get("fenetre", ""), "/".join("%.2f" % c for c in charges),
            " ".join(drap)))
        par_mode.setdefault(mode, []).append((ms, max(charges) if charges else float("nan")))
print("\n".join(lignes))
print()
med = {}
for mode, v in sorted(par_mode.items(), key=lambda kv: (kv[0][1], kv[0][0])):
    x = sorted(m for m, _ in v)
    med[mode] = statistics.median(x)
    print("%-10s %-9s n=%d  médiane %.1f  min..max %.1f..%.1f  (%.1f %% de dispersion)  charge max %.2f" % (
        mode[0], mode[1], len(x), med[mode], x[0], x[-1], 100 * (x[-1] - x[0]) / med[mode],
        max(c for _, c in v)))
for cel in sorted({c for _, c in med}):
    modes = [m for m in med if m[1] == cel]
    if len(modes) == 2:
        a, b = sorted(modes, key=lambda m: list(par_mode).index(m))
        print("%-9s %s → %s : %+.1f %% (médianes)" % (cel, a[0], b[0], 100 * (med[b] - med[a]) / med[a]))
