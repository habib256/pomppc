"""Zenerchi (PlayFirst, 2007) — témoin du pipeline fixe par AGL (Carbon).

Sur le disque quotidien, Zenerchi s'ouvre EN FENÊTRE (800×600) : il attache
d'abord un contexte plein écran (kind 0) puis repasse en fenêtre selon
prefs.dat. Scène fixe : le menu d'accueil animé, atteint sans toucher à rien ;
fenêtre de mesure aux images 1500..2500 (~120 img/s au 26/09).

Plein écran non automatisé : le réglage est la case « plein écran » du menu
Options, rangée dans prefs.dat (chiffré) ; ni Option+Entrée ni Cmd+F ne
basculent. Les préférences sont sauvegardées puis rendues par le pilote
(le jeu réécrit prefs.dat et metrics.txt).
"""
from jeu import Jeu, fenetre_fixe

APP = "/Users/tiger/Desktop/Zenerchi.app"
PREFS = "/Users/tiger/Library/Preferences/Zenerchi"


class Zenerchi(Jeu):
    cle = "zen"
    titre = "Zenerchi"
    famille = "pipeline fixe (AGL)"
    processus = "Zenerchi"
    plancher_ms = 7             # 5,2 ms/image au menu (26/09, sans déclencheur)
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
        return fenetre_fixe(rows, 1500, 2500)


JEU = Zenerchi()
