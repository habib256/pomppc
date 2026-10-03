# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Zenerchi (PlayFirst, 2007) — témoin du pipeline fixe par AGL (Carbon).

Sur le disque quotidien, Zenerchi s'ouvre EN FENÊTRE (800×600) : il attache
d'abord un contexte plein écran (kind 0) puis repasse en fenêtre selon
prefs.dat. Scène fixe : le menu d'accueil animé (15 dessins par image), atteint
sans toucher à rien après l'écran de l'éditeur (2 dessins par image) et ~4 s de
chargement (~18 ms/image).

Fenêtre de mesure REPÉRÉE sur la scène (30/09), plus aux images fixes 1500..2500 :
l'écran de l'éditeur dure un temps fixe, pas un nombre d'images, et il tourne
selon le lancement à ~3,4 ms/image (le menu arrive vers l'image 1650) ou à
~15 ms/image (vers l'image 490) — deux régimes vus avec la même pile, plugin et
kext de 8af9efc sur le QEMU d'avant les bug hunts comme avec HEAD. Dans le
premier cas, 1500..2500 contenait le chargement du menu : 7,7-7,9 ms/image au
lieu de 4,3, la « régression 4,4 → 7,9 » du 29/09 (bench/matrice/zenreg-*,
docs/matrice-jeux.md). Fenêtre : 200 images après que le menu a tenu 100 images
(fin_ecran_titre), sur 1000 images ; elle ne contient que des images à 15 dessins
dans les 35 tours rangés du 26 au 30/09.

Plein écran non automatisé : le réglage est la case « plein écran » du menu
Options, rangée dans prefs.dat (chiffré) ; ni Option+Entrée ni Cmd+F ne
basculent. Les préférences sont sauvegardées puis rendues par le pilote
(le jeu réécrit prefs.dat et metrics.txt).
"""
from jeu import Jeu

DESSINS_MENU = 15         # dessins par image au menu (l'écran de l'éditeur en fait 2)
TENUE = 100               # images au régime du menu avant de le tenir pour atteint
MARGE, LONGUEUR = 200, 1000


def fin_ecran_titre(rows):
    """Première image n dont les TENUE images précédentes font en moyenne au
    moins DESSINS_MENU - 1 dessins (compteur cumulé rows[n][3]) : le menu est
    affiché et chargé. None tant qu'il ne l'est pas."""
    for n in sorted(rows):
        if n - TENUE in rows and rows[n][3] - rows[n - TENUE][3] >= (DESSINS_MENU - 1) * TENUE:
            return n
    return None


APP = "/Users/tiger/Desktop/Zenerchi.app"
PREFS = "/Users/tiger/Library/Preferences/Zenerchi"


class Zenerchi(Jeu):
    cle = "zen"
    titre = "Zenerchi"
    famille = "pipeline fixe (AGL)"
    processus = "Zenerchi"
    plancher_ms = 6           # menu à 4,0-4,4 ms/image (fenêtre repérée sur la scène, 30/09)
    plancher_ms_linux = 10    # PC : 6,8-7,9 ms/image au 03/10
    delai_scene = 300
    dump_images = 120
    non_automatise = {
        "pe": "plein écran = case du menu Options, rangée dans prefs.dat (chiffré) ; "
              "aucune touche de bascule (Option+Entrée, Cmd+F essayés)",
    }

    def fichiers_reglages(self, mode):
        return [PREFS + "/prefs.dat", PREFS + "/hiscore.dat", PREFS + "/metrics.txt"]

    def commande(self, mode):
        return 'cd "%s/Contents/MacOS" && ./Zenerchi' % APP

    def fenetre(self, rows):
        m = fin_ecran_titre(rows)
        if m is None:
            return None
        a = m + MARGE
        b = a + LONGUEUR
        return (a, b) if a in rows and b in rows else None


JEU = Zenerchi()
