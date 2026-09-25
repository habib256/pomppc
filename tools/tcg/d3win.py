#!/usr/bin/env python3
"""d3win.py frames.csv [T] — ms/image de DOOM 3 aux fenêtres de référence.

T = fin de la cinématique de demo_mars_city1. Sans T donné : règle de
tools/tcg/guest/meas-tcg.sh (première image a >= 1500 dont les 200 images
suivantes et les 200 d'après dépassent 65 ms/image en moyenne, replis <= 8),
qui ne dépend pas de la vitesse de l'émulateur à 20 % près, puis affinée au
saut lui-même : première image x >= a dont les 25 suivantes dépassent 65
ms/image (la cinématique tourne à ~37 ms/image juste avant, le jeu à ~86).
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
    for f in sorted(rows):
        a, b = f - 400, f - 200
        if f < 1900 or a < 1500 or a not in rows or b not in rows:
            continue
        if ((rows[b][0] - rows[a][0]) / 200 > 65 and (rows[f][0] - rows[b][0]) / 200 > 65
                and rows[f][1] - rows[a][1] <= 8):
            T = a
            break
if T is None:
    sys.exit("T introuvable (dernière image %d)" % max(rows))
if len(sys.argv) <= 2:
    for x in range(T, T + 400):
        if x + 25 in rows and (rows[x + 25][0] - rows[x][0]) / 25 > 65:
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
