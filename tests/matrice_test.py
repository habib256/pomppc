#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Tests hors VM de la matrice de jeux (tools/matrice/) : lecture de frames.csv,
règles de scène (fin de la cinématique de DOOM 3, fin du chargement de Prey),
chargement des modules de jeux, comparateur d'images ppmcmp."""
import os
import subprocess
import sys
import tempfile
from unittest import mock

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(R, "tools", "matrice"))
sys.path.insert(0, os.path.join(R, "tools", "matrice", "jeux"))
import matrice  # noqa: E402
import d3  # noqa: E402
import prey  # noqa: E402
import mb  # noqa: E402

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

# Le rendez-vous ne vaut que pour une image complète consignée par le plugin.
verifie(matrice.capture_frame("2\n@@FRAMES\n" + serie([10, 20])) == 2,
        "capture confirmée par frames.csv")
verifie(matrice.capture_frame("3\n@@FRAMES\n" + serie([10, 20])) is None,
        "capture sans image correspondante refusée")
verifie(matrice.capture_frame("2\n@@FRAMES\n" + serie([10, 20]).rstrip()) is None,
        "ligne de capture incomplète refusée")
verifie(matrice.capture_frame("@@FRAMES\n" + serie([10, 20])) is None,
        "marqueur de capture absent refusé")

prefs = '$pref::Video::resolution = "1024 768 32";\r\n$pref::Player::Name = "Test";\r\n'
window = mb.preferences_video(prefs, "fen")
verifie(window.count('$pref::Video::resolution') == 1 and '"800 600 32"' in window
        and '$pref::Player::Name = "Test";' in window, "préférences vidéo sans perte des autres réglages")
verifie(mb.preferences_video(window, "fen").count('$pref::Video::resolution') == 1,
        "préférences vidéo sans doublons")

# Une image voisine identique ne doit pas masquer un numéro présenté absent.
with tempfile.TemporaryDirectory() as d:
    os.makedirs(os.path.join(d, "invite", "dump"))
    for name, contents in (("invite/dump/000000.bin", "dump"),
                           ("capture.ppm", "capture"), ("capture-frame.txt", "9\n")):
        with open(os.path.join(d, name), "w") as f:
            f.write(contents)
    presentations = [{"image": 8, "ppm": os.path.join(d, "rejeu", "r.ppm"),
                      "x": 0, "y": 0, "w": 2, "h": 2}]
    with mock.patch.object(matrice, "rejoue", return_value=(0, presentations, "")), \
         mock.patch.object(matrice, "ppmcmp", return_value="match 0 0 0") as compare:
        result = matrice.analyse_image(d, "test-fen")
        verifie(not result["image_ok"] and "image capturée 9 absente du rejeu" in result["image_motif"],
                "ne pas accepter l'image voisine d'une capture synchronisée")
        verifie(not compare.called, "comparaison limitée au numéro présenté")

with tempfile.TemporaryDirectory() as d:
    path = os.path.join(d, "presents.txt")
    with open(path, "w") as f:
        f.write("0 392 86612 1600 640 480 1\n1 393 173224 3200 640 480 0\n2 394 173224 3200 640 480\n")
    with mock.patch.object(matrice.subprocess, "run", return_value=mock.Mock(stderr="3 présentations écrites, 0 en erreur")):
        nerr, pres, _ = matrice.rejoue("dump", "replay", path)
    verifie(nerr == 0 and len(pres) == 3 and all(p["x"] == 106 and p["y"] == 54 for p in pres),
            "coordonnées de capture 16/32 bits et ancien format")

# DOOM 3 : cinématique à 20 ms avec un passage lent de 400 images (la règle à
# deux tranches s'y trompait), puis le jeu à 78 ms à partir de l'image 3001 ;
# la règle attend l'image 5000 (évaluée en direct, seuil relatif au niveau)
cine = [20] * 2000 + [90] * 400 + [20] * 600
jeu = [78] * 2100
T = d3.fin_cinematique(matrice.lit_frames(serie(cine + jeu)))
verifie(T is not None and 2990 <= T <= 3001, "fin_cinematique (T=%s, attendu ~3000)" % T)
verifie(d3.fin_cinematique(matrice.lit_frames(serie(cine))) is None, "pas de T dans la cinématique seule")
verifie(d3.JEU.fenetre(matrice.lit_frames(serie(cine + [78] * 1500))) is None,
        "pas de fenêtre avant l'image 5000")
# jeu plus rapide que l'ancien seuil fixe de 65 ms (x-fp-inline, 26/09 : ~63)
T = d3.fin_cinematique(matrice.lit_frames(serie(cine + [63] * 2100)))
verifie(T is not None and 2990 <= T <= 3001, "fin_cinematique à 63 ms (T=%s, attendu ~3000)" % T)

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

# premier_plan (osascript, 1 à 3 s de processeur invité) jamais dans une fenêtre fixe
import ut  # noqa: E402
verifie(ut.JEU.images_fenetre == (13, 73) and ut.JEU.fenetre({13: 0, 73: 0}) == (13, 73)
        and ut.JEU.fenetre({13: 0}) is None, "fenêtre fixe d'UT2004 lue sur images_fenetre")
verifie(mb.JEU.fenetre({900: 0, 1500: 0}) == (900, 1500), "fenêtre fixe de Marble Blast")
verifie(d3.JEU.images_fenetre is None, "DOOM 3 : fenêtre non fixe")
P = matrice.plan_permis
verifie(P((13, 73), {}), "avant la première image : permis")
verifie(not P((13, 73), {1: (0, 0, 0), 5: (300.0, 0, 0)}), "image 5 à ~16 img/s : fenêtre trop proche")
verifie(not P((13, 73), {40: (2500.0, 0, 0)}), "dans la fenêtre : interdit")
verifie(P((13, 73), {80: (5000.0, 0, 0)}), "après la fenêtre : permis")
verifie(P((900, 1500), {100: (5000.0, 0, 0)}), "image 100 à 20 img/s, fenêtre à 900 : permis")
verifie(not P((900, 1500), {850: (40000.0, 0, 0)}), "image 850, fenêtre à 900 : interdit")
verifie(P(None, {40: (2500.0, 0, 0)}), "sans fenêtre fixe : permis")

print("matrice : %s" % ("OK" if not echecs else "%d échec(s)" % echecs))
sys.exit(1 if echecs else 0)
