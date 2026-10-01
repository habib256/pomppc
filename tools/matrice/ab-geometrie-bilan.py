#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""ab-geometrie-bilan.py CAMPAGNE — bilan de tools/matrice/ab-geometrie.sh.

Par cellule (jeu, mode d'affichage) et par mode de l'A/B (ref, geo) : ms/image
de chaque partie, médiane, rapport geo/ref des médianes ; verdicts des tours de
justesse (image, replis). Et, lus dans note.txt / frames.csv de chaque cellule :
la ligne de configuration du plugin (rawsane, natshm — un « 0 » en mode geo
veut dire que le levier n'a pas tourné : QEMU sans QGPU_CAP_GEOM_HOST), les
sommets Begin/End par image (raw_vertices de frames.csv) et les lignes
« geom-host » du bilan POMPPC_GL_STATS quand elles y sont."""
import csv, os, re, statistics, sys

camp = sys.argv[1]
ms = {}          # (jeu, mode_aff) -> {ref: [..], geo: [..]}
juste = []
conf = {}
for d in sorted(os.listdir(camp)):
    f = os.path.join(camp, d, "resultats.csv")
    if not os.path.isfile(f) or "-" not in d:
        continue
    m, k = d.rsplit("-", 1)
    for r in csv.DictReader(open(f)):
        cle = (r["jeu"], r["mode"])
        if k == "juste":
            juste.append("%-4s %-22s %-11s verdict %-6s image %-10s replis %s  %s" % (
                m, r["jeu"], r["mode"], r.get("verdict", ""), r.get("image", ""),
                r.get("replis", ""), r.get("motifs", "")))
        try:
            v = float(r["ms_image"])
        except (KeyError, ValueError):
            continue
        if k != "juste":
            ms.setdefault(cle, {}).setdefault(m, []).append(v)
        # configuration réellement vue par le plugin
        # « dossier » est relatif au dépôt principal : CAMPAGNE = <dépôt>/bench/matrice/ab-geo-X
        main = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(camp))))
        dossier = os.path.join(main, r["dossier"])
        for sous in ("mesure", "invite"):
            note = os.path.join(dossier, sous, "note.txt")
            if os.path.isfile(note):
                l0 = open(note, errors="replace").readline()
                g = re.search(r"rawsane=(\d) natshm=(\d)", l0)
                conf.setdefault((cle, m), set()).add(g.group(0) if g else "(plugin sans A4)")
                break

print("%-24s %-11s %-28s %-28s %s" % ("jeu", "affichage", "ref ms/image", "geo ms/image", "geo/ref"))
for cle in sorted(ms):
    a, b = ms[cle].get("ref", []), ms[cle].get("geo", [])
    ma = statistics.median(a) if a else float("nan")
    mb = statistics.median(b) if b else float("nan")
    print("%-24s %-11s %-28s %-28s %.3f" % (cle[0], cle[1],
          " ".join("%.1f" % x for x in a) + " (méd. %.1f)" % ma,
          " ".join("%.1f" % x for x in b) + " (méd. %.1f)" % mb, mb / ma if a and b else float("nan")))
print()
print("configuration vue par le plugin :")
for (cle, m), s in sorted(conf.items()):
    alerte = "  ← LEVIER ÉTEINT" if m == "geo" and not any("=1" in x for x in s) else ""
    print("  %-4s %-22s %-11s %s%s" % (m, cle[0], cle[1], ", ".join(sorted(s)), alerte))
print()
print("justesse (tours avec vidage) :")
print("\n".join(juste) if juste else "  (aucun)")
