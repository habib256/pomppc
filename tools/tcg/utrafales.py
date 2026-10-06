#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""utrafales.py CAMPAGNE… — relit les frames.csv d'une campagne matab (UT2004,
`ut-fen`) et sépare la vitesse du jeu des RAFALES d'images lentes que la
matrice provoque elle-même dans l'invité (docs/tcg-g4.md §31).

Pour chaque partie <campagne>/<bras>-<k>/ut-fen/mesure/frames.csv :
  fen    ms/image sur la fenêtre de la matrice (images 13..73, ~4 s de jeu) ;
  rapport  médiane, sur la fenêtre puis sur les images 74..560 (ou jusqu'à la fin de la
           plus courte), du rapport du
           temps de chaque image à la médiane de la même image entre toutes
           les parties lues : la vitesse à contenu égal, insensible aux rafales ;
  excès fen.  temps cumulé, sur les images 13..73, au-delà de la médiane de la
           même image entre toutes les parties lues (pas de simulation fixe :
           l'image n est la même d'une partie à l'autre) ;
  rafales  instants (s après l'image 1) et images où cet excès dépasse 200 ms
           en 1,5 s : relevés ssh de la matrice, `osascript` de premier_plan() ;
puis la médiane par bras de chaque colonne.

    tools/tcg/utrafales.py bench/tcg/ab/x86-ut-tcg bench/tcg/ab/x86-ut-def
"""
import csv
import os
import re
import statistics as st
import sys
from collections import defaultdict


def rafales(e, dt, base):
    """Excès de chaque image sur la médiane, entre toutes les parties, de la même
    image (pas de simulation fixe : l'image n est la même d'une partie à l'autre)."""
    ex = [x - base[i] if i < len(base) else 0.0 for i, x in enumerate(dt)]
    out, i = [], 0
    while i < len(dt):
        j, s = i, 0.0
        while j < len(dt) and e[j + 1] - e[i + 1] < 1500:
            s += ex[j]
            j += 1
        if s > 200:
            out.append((e[i + 1] / 1000, i + 2))
            i = j
        else:
            i += 1
    return out


def lit(path):
    e = [float(r["elapsed_ms"]) for r in csv.DictReader(open(path))]
    return e, [e[i] - e[i - 1] for i in range(1, len(e))]


def main(camps):
    par_bras = defaultdict(list)
    parties = []
    for c in camps:
        for d in sorted(os.listdir(c)):
            f = os.path.join(c, d, "ut-fen", "mesure", "frames.csv")
            m = re.match(r"(.+)-(\d+)$", d)
            if m and os.path.exists(f):
                e, dt = lit(f)
                # 06/10 : depuis images_fenetre (§31), la matrice arrête l'enregistrement
                # plus tôt (~340-460 images) : la fin de l'intervalle suit la plus courte
                if len(dt) >= 80:
                    parties.append((c, d, m[1], e, dt))
    if not parties:
        sys.exit("aucune partie ut-fen d'au moins 80 images dans %s" % " ".join(camps))
    fin = min(559, min(len(p[4]) for p in parties))
    base = [st.median(p[4][i] for p in parties if i < len(p[4]))
            for i in range(max(len(p[4]) for p in parties))]
    for c, d, bras, e, dt in parties:
        fen = (e[72] - e[12]) / 60
        # vitesse à contenu égal : médiane des rapports à la référence image par image
        rf = st.median(dt[i] / base[i] for i in range(12, 72))
        rs = st.median(dt[i] / base[i] for i in range(73, fin))
        raf = rafales(e, dt, base)
        # excès sur la référence image par image, cumulé dans la fenêtre 13..73
        exf = sum(max(0.0, dt[i] - base[i]) for i in range(12, 72))
        print("%-24s fen %5.1f  excès fen. %4.0f ms  rapport fen. %.3f  rapport 74..%d %.3f  rafales %s" % (
            os.path.join(os.path.basename(c), d), fen, exf, rf, fin + 1, rs,
            " ".join("%.1fs/i%d" % (t, n) for t, n in raf[:4])))
        par_bras[os.path.basename(c) + "/" + bras].append((fen, rf, rs))
    print()
    for b, v in par_bras.items():
        print("%-24s n=%d  fen méd. %5.1f  rapport fen. méd. %.3f  rapport 74..%d méd. %.3f" % (
            b, len(v), st.median(x[0] for x in v), st.median(x[1] for x in v),
            fin + 1, st.median(x[2] for x in v)))

if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
