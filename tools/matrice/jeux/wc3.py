"""Warcraft III: Reign of Chaos (Blizzard, CFM/PEF, lancé par LaunchCFMApp) — tableaux.

Le jeu réclame son CD : l'image « Warcraft III.toast » du dossier du jeu est
montée (hdiutil) avant le lancement et démontée après. Il s'ouvre en plein
écran 800×600 (changement de mode de l'écran, contexte plein écran kind 54)
et arrive seul au menu principal (fond 3D animé, pluie) : c'est la scène
fixe, fenêtre de mesure aux images 1200..1700 (~48 ms/image au 26/09).

Fenêtre non automatisée : pas de réglage connu (préférences vides,
LaunchCFMApp ne passe pas d'arguments au jeu).
Le 26/09, le texte des menus paraît juste (TODO §6 le disait défectueux).
"""
from jeu import Jeu, fenetre_fixe

W3 = "/Users/tiger/Desktop/Warcraft III Folder"
LCFM = "/System/Library/Frameworks/Carbon.framework/Versions/A/Support/LaunchCFMApp"


class Warcraft3(Jeu):
    cle = "wc3"
    titre = "Warcraft III"
    famille = "tableaux"
    processus = "LaunchCFMApp"
    processus_ui = "Warcraft III"
    plancher_ms = 65
    delai_scene = 400
    dump_images = 20
    non_automatise = {"fen": "s'ouvre en plein écran ; aucun réglage de fenêtre connu "
                             "(préférences vides, LaunchCFMApp ne passe pas d'arguments)"}

    def fichiers_reglages(self, mode):
        return ["/Users/tiger/Library/Preferences/com.blizzard.WarcraftIII.plist",
                "/Users/tiger/Library/Preferences/Warcraft III Preferences"]

    def preparer(self, h, mode):
        h.ssh("[ -d '/Volumes/Warcraft III' ] || hdiutil attach -readonly -noverify "
              "'%s/Warcraft III.toast' >/dev/null" % W3, delai=120)

    def nettoyer(self, h, mode):
        h.ssh("hdiutil detach '/Volumes/Warcraft III' >/dev/null 2>&1; true", delai=60)

    def commande(self, mode):
        return 'cd "%s" && %s "%s/Warcraft III"' % (W3, LCFM, W3)

    def fenetre(self, rows):
        return fenetre_fixe(rows, 1200, 1700)


JEU = Warcraft3()
