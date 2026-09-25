#!/usr/bin/env python3
"""mbpair.py A.txt B.txt [...] — A/B de Marble Blast par fenêtres de 5 s
appariées par triangles/image (protocole de docs/gpu-3d-tiger.md §4.7 et de
docs/flottant-rapide.md : la démo se joue toute seule, les mêmes images
reviennent des deux côtés ; deux fenêtres dont les triangles/image diffèrent
de moins de TOL = 2 % sont les mêmes images).

Chaque fichier est un bilan POMPPC_GL_STATS (ligne « NN.N fps | tri N/frame |
… »). Plusieurs passes : `A1.txt,A2.txt B1.txt,B2.txt` (virgules) — les
fenêtres de toutes les passes d'un côté sont mises en commun.

Imprime : fenêtres de jeu (tri >= MIN_TRI) de chaque côté, paires trouvées,
img/s moyen et médian des paires, rapport B/A (médiane des rapports par paire,
et rapport des moyennes)."""
import re, statistics, sys

TOL = 0.02
MIN_TRI = 1000
rx = re.compile(r"^([\d.]+) fps \| tri (\d+)/frame")

def load(spec):
    w = []
    for path in spec.split(","):
        for l in open(path):
            m = rx.match(l)
            if m:
                w.append((float(m.group(1)), int(m.group(2))))
    return w

def main():
    A, B = load(sys.argv[1]), load(sys.argv[2])
    ga = [x for x in A if x[1] >= MIN_TRI]
    gb = [x for x in B if x[1] >= MIN_TRI]
    print("A : %d fenêtres, %d de jeu, img/s moyen %.2f (médiane %.2f)" % (
        len(A), len(ga), statistics.mean(f for f, t in ga), statistics.median(f for f, t in ga)))
    print("B : %d fenêtres, %d de jeu, img/s moyen %.2f (médiane %.2f)" % (
        len(B), len(gb), statistics.mean(f for f, t in gb), statistics.median(f for f, t in gb)))
    used = set()
    pairs = []
    for fa, ta in sorted(ga, key=lambda x: -x[1]):
        best = None
        for j, (fb, tb) in enumerate(gb):
            if j in used:
                continue
            e = abs(tb - ta) / ta
            if e <= TOL and (best is None or e < best[0]):
                best = (e, j)
        if best:
            used.add(best[1])
            pairs.append((ta, fa, gb[best[1]][1], gb[best[1]][0]))
    if not pairs:
        print("aucune paire")
        return
    ra = [p[3] / p[1] for p in pairs]
    ma = statistics.mean(p[1] for p in pairs)
    mb = statistics.mean(p[3] for p in pairs)
    print("paires (±%d %% de triangles) : %d" % (TOL * 100, len(pairs)))
    print("  img/s moyen des paires : A %.2f  B %.2f  → B/A %.3f (%+.1f %%)" % (
        ma, mb, mb / ma, 100 * (mb / ma - 1)))
    print("  médiane des rapports par paire : %.3f (%+.1f %%) ; quartiles %.3f / %.3f" % (
        statistics.median(ra), 100 * (statistics.median(ra) - 1),
        statistics.quantiles(ra, n=4)[0] if len(ra) > 3 else min(ra),
        statistics.quantiles(ra, n=4)[2] if len(ra) > 3 else max(ra)))
    if "-v" in sys.argv:
        for ta, fa, tb, fb in pairs:
            print("   tri %5d/%5d  %6.1f → %6.1f  %.3f" % (ta, tb, fa, fb, fb / fa))

main()
