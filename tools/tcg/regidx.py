#!/usr/bin/env python3
"""regidx.py REF[,REF…] FICHIER… — indice de régime d'une passe de Marble Blast
(docs/tcg-g4.md §14).

Chaque FICHIER est un bilan POMPPC_GL_STATS (« NN.N fps | tri N/frame | … »).
Chaque fenêtre de jeu (tri >= 1000) est rapportée aux fenêtres du jeu de
référence REF (bilans mis en commun) dont les triangles/image diffèrent de moins
de 2 % (médiane de leurs img/s) : l'indice d'une passe est la médiane de ces
rapports. 1,00 = la référence ; les deux régimes de §8.4 donnent ~0,91 et ~1,00
contre une référence rapide. Imprime aussi img/s moyen des fenêtres de jeu."""
import re, statistics, sys

rx = re.compile(r"^([\d.]+) fps \| tri (\d+)/frame")


def load(path):
    out = []
    for l in open(path):
        m = rx.match(l)
        if m and int(m.group(2)) >= 1000:
            out.append((float(m.group(1)), int(m.group(2))))
    return out


def index(ref, wins, tol=0.02):
    r = []
    for f, t in wins:
        near = [rf for rf, rt in ref if abs(rt - t) <= tol * t]
        if near:
            r.append(f / statistics.median(near))
    return (statistics.median(r) if r else float("nan")), len(r)


def main():
    ref = [w for p in sys.argv[1].split(",") for w in load(p)]
    for p in sys.argv[2:]:
        w = load(p)
        if not w:
            print("%s : aucune fenêtre de jeu" % p)
            continue
        ix, n = index(ref, w)
        print("%s  fenêtres %d  img/s %.1f  indice %.3f (%d appariées)" % (
            p, len(w), statistics.mean(f for f, t in w), ix, n))


if __name__ == "__main__":
    main()
