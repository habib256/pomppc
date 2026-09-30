# GPL3 - Copyleft VERHILLE Arnaud
# A4 (30/09/2026, docs/protocole-v23-textures.md) : attente de l'invité sur l'hôte, par image,
# d'après un frames.csv du plugin (POMPPC_GL_FRAMES), sur les 600 images qui précèdent le
# vidage de la matrice (premier écart de plus d'une seconde dans la seconde moitié), et pauses
# de plus de 250 ms avant lui (chargements).
# Usage : python3 tools/re/a4attente.py frames.csv...
# Un gain côté hôte (backend : G7, G8) ne se voit dans le temps d'image que par cette attente :
# le fil de rendu du device tourne en parallèle du vCPU.
import csv
import sys

for path in sys.argv[1:]:
    rows = list(csv.DictReader(open(path)))
    count = {}
    for r in rows:
        count[r['context']] = count.get(r['context'], 0) + 1
    ctx = max(count, key=count.get)          # le contexte qui échange le plus
    rr = [r for r in rows if r['context'] == ctx]
    t = [float(r['elapsed_ms']) for r in rr]
    cut = len(rr) - 1
    for i in range(len(rr) // 2, len(rr)):
        if t[i] - t[i - 1] > 1000:
            cut = i - 1
            break
    a = max(1, cut - 600)
    n = int(rr[cut]['frame']) - int(rr[a]['frame'])
    if n <= 0:
        continue
    d = lambda k: (float(rr[cut][k]) - float(rr[a][k])) / n
    pauses = [t[i] - t[i - 1] for i in range(1, cut + 1) if t[i] - t[i - 1] > 250]
    print('%-50s images %5s..%5s  %6.1f ms/image  attente %.3f ms/image (%.1f %%)  '
          'pauses > 250 ms : %d, %.1f s' % (
              path, rr[a]['frame'], rr[cut]['frame'], (t[cut] - t[a]) / n, d('wait_ms'),
              100 * d('wait_ms') * n / (t[cut] - t[a]), len(pauses), sum(pauses) / 1000))
