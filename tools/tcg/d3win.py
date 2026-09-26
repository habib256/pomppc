#!/usr/bin/env python3
"""d3win.py frames.csv [T] — ms/image de DOOM 3 aux fenêtres de référence.

T = fin de la cinématique de demo_mars_city1. Sans T donné : première image
a >= 1500 dont les 200 images suivantes et les 200 d'après dépassent S en
moyenne (replis <= 8), puis affinée au saut lui-même : première image x >= a
dont les 25 suivantes dépassent S. S = 0,8 × le niveau du jeu, lu sur les 400
dernières images de la partie (joueur immobile). Jusqu'au 26/09, S valait 65
ms/image fixe (règle de meas-tcg.sh) : avec x-fp-inline le jeu tourne à ~63 et
la règle ne trouvait plus T ou le trouvait en retard ; S relatif redonne le même
T (à 15 images près) sur les 41 parties rangées dans bench/tcg/d3/.
Imprime aussi le T de la règle du lot 3 (meas3.sh : 200 images > 85 ms), pour
situer la mesure par rapport aux références du TODO.
Fenêtres : T+50..T+280 (celle des lots 2 et 3), T+280..T+450."""
import sys

rows = {}
for l in open(sys.argv[1]):
    p = l.split(",")
    if p[0].isdigit():
        rows[int(p[0])] = (float(p[2]), int(p[5]))
if not rows:
    sys.exit("frames.csv vide")
if len(sys.argv) > 2:
    T = int(sys.argv[2])
else:
    T = None
    _z = max(rows); _a = min(x for x in rows if x >= _z - 400)
    S = 0.8 * (rows[_z][0] - rows[_a][0]) / (_z - _a)
    for f in sorted(rows):
        a, b = f - 400, f - 200
        if f < 1900 or a < 1500 or a not in rows or b not in rows:
            continue
        if ((rows[b][0] - rows[a][0]) / 200 > S and (rows[f][0] - rows[b][0]) / 200 > S
                and rows[f][1] - rows[a][1] <= 8):
            T = a
            break
if T is None:
    sys.exit("T introuvable (dernière image %d)" % max(rows))
if len(sys.argv) <= 2:
    for x in range(T, T + 400):
        if x + 25 in rows and (rows[x + 25][0] - rows[x][0]) / 25 > S:
            T = x
            break
T3 = None
for f in sorted(rows):
    a = f - 200
    if f >= 1700 and a >= 1500 and a in rows and (rows[f][0] - rows[a][0]) / 200 > 85 \
            and rows[f][1] - rows[a][1] <= 4:
        T3 = a
        break
print("T (règle lot 3, 85 ms) = %s" % T3)
print("T = %d (dernière image %d)" % (T, max(rows)))
for a, b in ((50, 280), (280, 450), (50, 450)):
    if T + b in rows and T + a in rows:
        print("T+%d..T+%d : %.1f ms/image, replis %d" % (
            a, b, (rows[T + b][0] - rows[T + a][0]) / (b - a), rows[T + b][1] - rows[T + a][1]))
