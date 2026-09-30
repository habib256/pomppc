#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""attente.py DOSSIER... — attente de l'invité sur l'hôte, par image AFFICHÉE, sur
la fenêtre de mesure de la matrice (30/09/2026, docs/backend-gl-attente.md).

DOSSIER est un tour de matrice (resultats.csv + <cellule>/invite|mesure/frames.csv)
ou une campagne d'A/B de tools/tcg/matab.sh (<mode>-<k>/resultats.csv) ; dans ce
second cas, un bilan par mode suit (médianes de l'attente et des ms/image).

Pourquoi pas tools/re/a4attente.py : il prend le contexte qui échange le plus et
les 600 échanges avant le premier trou d'une seconde. Chez Colin McRae (trois
échanges par image, dont deux d'une fenêtre cachée), ces 600 échanges tombaient
dans un MENU à 2 ms par échange : « 10 % d'attente, ~7 ms par image » venait de
là. Sur la fenêtre de mesure de la course, l'attente est de 0,12 ms par image.

Ici : la fenêtre est celle de resultats.csv (colonne `fenetre`, numéros d'échange
du plugin) ; le contexte affiché est, parmi ceux qui échangent au moins 20 % aussi
souvent que le plus bavard, celui qui échange le MOINS (les cibles cachées
échangent plus souvent) ; les compteurs du plugin (wait_ms, submit_ms…) sont
globaux, donc leur différence entre deux échanges du contexte affiché couvre
tous les contextes. Colonnes : ms/image, attente ms/image et part, échanges par
image affichée, lots bruts par image."""
import csv
import os
import statistics
import sys


def cellule(tour, row):
    d = row.get("dossier", "").rstrip("/")
    cel = os.path.basename(d) if d else ""
    for sous in ("invite", "mesure"):
        p = os.path.join(tour, cel, sous, "frames.csv")
        if os.path.isfile(p):
            return cel, p
    return cel, None


def mesure(frames, win):
    a, b = map(int, win.split(".."))
    rows = [r for r in csv.DictReader(open(frames)) if a <= int(r["frame"]) <= b]
    cnt = {}
    for r in rows:
        cnt[r["context"]] = cnt.get(r["context"], 0) + 1
    if not cnt:
        return None
    big = [c for c in cnt if cnt[c] >= 0.2 * max(cnt.values())]
    ctx = min(big, key=cnt.get)
    m = [r for r in rows if r["context"] == ctx]
    n = len(m) - 1
    if n <= 0:
        return None
    d = lambda k: (float(m[-1][k]) - float(m[0][k])) / n
    ms = d("elapsed_ms")
    return {"ms": ms, "attente": d("wait_ms"), "part": 100 * d("wait_ms") / ms,
            "echanges": len(rows) / (n + 1), "lots": d("raw_draws"), "soumission": d("submit_ms")}


def tour(t, etiquette=""):
    res = os.path.join(t, "resultats.csv")
    out = []
    if not os.path.isfile(res):
        return out
    for r in csv.DictReader(open(res)):
        win = r.get("fenetre", "")
        if not r.get("dossier"):
            continue                    # cellule non jouée (non automatisée)
        cel, fr = cellule(t, r)
        if ".." not in win or not fr:
            print("%s%-9s pas de fenêtre ou de frames.csv" % (etiquette, cel))
            continue
        x = mesure(fr, win)
        if not x:
            print("%s%-9s fenêtre %s hors de frames.csv" % (etiquette, cel, win))
            continue
        print("%s%-9s %-13s %6.1f ms/image  attente %6.3f ms/image (%4.1f %%)  "
              "%.1f échange(s)/image  %4.0f lots/image  verdict %s" % (
                  etiquette, cel, win, x["ms"], x["attente"], x["part"], x["echanges"],
                  x["lots"], r.get("verdict", "?")))
        out.append((cel, x))
    return out


for arg in sys.argv[1:]:
    if os.path.isfile(os.path.join(arg, "resultats.csv")):
        tour(arg)
        continue
    # campagne matab.sh : <mode>-<k>/
    par = {}
    for d in sorted(os.listdir(arg)):
        p = os.path.join(arg, d)
        if "-" not in d or not os.path.isfile(os.path.join(p, "resultats.csv")):
            continue
        mode = d.rsplit("-", 1)[0]
        for cel, x in tour(p, "%-12s " % d):
            par.setdefault((cel, mode), []).append(x)
    print()
    for (cel, mode), v in sorted(par.items()):
        att = sorted(x["attente"] for x in v)
        ms = sorted(x["ms"] for x in v)
        print("%-9s %-10s n=%d  attente médiane %.3f ms/image (%.3f..%.3f)  "
              "ms/image médiane %.1f (%.1f..%.1f)" % (
                  cel, mode, len(v), statistics.median(att), att[0], att[-1],
                  statistics.median(ms), ms[0], ms[-1]))
