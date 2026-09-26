#!/usr/bin/env python3
"""Tests hors VM de la matrice de jeux (tools/matrice/) : lecture de frames.csv,
règles de scène (fin de la cinématique de DOOM 3, fin du chargement de Prey),
chargement des modules de jeux, comparateur d'images ppmcmp."""
import os
import subprocess
import sys
import tempfile

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(R, "tools", "matrice"))
sys.path.insert(0, os.path.join(R, "tools", "matrice", "jeux"))
import matrice  # noqa: E402
import d3  # noqa: E402
import prey  # noqa: E402

echecs = 0


def verifie(cond, quoi):
    global echecs
    if not cond:
        echecs += 1
        print("ÉCHEC :", quoi)


def serie(durees, fb=0):
    """frames.csv synthétique : une ligne par image, durées en ms."""
    l, t = ["frame,context,elapsed_ms,raw_vertices,raw_draws,fallbacks,readbacks,submit_ms,wait_ms,copy_ms"], 0.0
    for i, d in enumerate(durees, 1):
        t += d
        l.append("%d,0x1,%.3f,0,%d,%d,0,0,0,0" % (i, t, i * 10, fb))
    return "\n".join(l) + "\n"


# frames.csv : lignes incomplètes ignorées, compteurs lus
rows = matrice.lit_frames(serie([10, 20]) + "3,0x1,4")
verifie(sorted(rows) == [1, 2] and rows[2][0] == 30.0 and rows[2][3] == 20, "lit_frames")

# DOOM 3 : cinématique à 20 ms avec un passage lent de 400 images (la règle à
# deux tranches s'y trompait), puis le jeu à 78 ms à partir de l'image 3001
cine = [20] * 2000 + [90] * 400 + [20] * 600
jeu = [78] * 900
T = d3.fin_cinematique(matrice.lit_frames(serie(cine + jeu)))
verifie(T is not None and 2995 <= T <= 3001, "fin_cinematique (T=%s, attendu ~3000)" % T)
verifie(d3.fin_cinematique(matrice.lit_frames(serie(cine))) is None, "pas de T dans la cinématique seule")

# Prey : chargement (images de plusieurs secondes) puis 400 images calmes
P = prey.fin_chargement(matrice.lit_frames(serie([300] * 100 + [3000, 5000, 1900, 1985] + [80] * 400)))
verifie(P == 104, "fin_chargement (L=%s, attendu 104)" % P)
verifie(prey.fin_chargement(matrice.lit_frames(serie([300] * 100 + [3000] + [80] * 200))) is None,
        "fin_chargement : attendre 300 images")

# tous les modules de jeux se chargent, clés uniques, modes connus
jeux = matrice.charge_jeux()
verifie(set(matrice.ORDRE) <= set(jeux), "jeux de la matrice : %s" % sorted(jeux))
for k, j in jeux.items():
    for m in j.non_automatise:
        verifie(m in ("fen", "pe"), "%s : mode %s" % (k, m))

# ppmcmp : identique, écart, découpe
with tempfile.TemporaryDirectory() as d:
    exe = os.path.join(d, "ppmcmp")
    r = subprocess.run(["cc", "-O2", os.path.join(R, "tools", "matrice", "ppmcmp.c"), "-lm", "-o", exe],
                       capture_output=True, text=True)
    verifie(r.returncode == 0, "compilation de ppmcmp : %s" % r.stderr[-300:])
    if r.returncode == 0:
        def ppm(nom, w, h, f):
            p = os.path.join(d, nom)
            with open(p, "wb") as o:
                o.write(b"P6\n%d %d\n255\n" % (w, h))
                o.write(bytes(v for y in range(h) for x in range(w) for v in f(x, y)))
            return p
        ecran = ppm("e.ppm", 8, 6, lambda x, y: (x * 30, y * 40, 7))
        zone = ppm("z.ppm", 3, 2, lambda x, y: ((x + 2) * 30, (y + 1) * 40, 7))
        o = subprocess.run([exe, ecran, "2", "1", zone], capture_output=True, text=True).stdout.split()
        verifie(o[1:] == ["0.000", "0", "0.000"], "ppmcmp identique : %s" % o)
        o = subprocess.run([exe, ecran, "0", "0", zone], capture_output=True, text=True).stdout.split()
        verifie(float(o[1]) > 10 and float(o[3]) > 50, "ppmcmp différent : %s" % o)
        c = os.path.join(d, "c.ppm")
        subprocess.run([exe, "-c", ecran, "2", "1", "3", "2", c])
        o = subprocess.run([exe, "-r", c, zone], capture_output=True, text=True).stdout.split()
        verifie(o == ["0.000", "0", "0.000"], "ppmcmp découpe : %s" % o)

print("matrice : %s" % ("OK" if not echecs else "%d échec(s)" % echecs))
sys.exit(1 if echecs else 0)
