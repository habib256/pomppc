"""Prey Demo — idTech4 (Human Head), ARB2, VBO, DXT5 (cartes de normales).

Sauvegarde automatique « Fuite à toute vitesse » (`+loadGame
Auto___Fuite_____toute_vitesse`, celle des mesures des lots 2-3), sans toucher
à rien. Fin du chargement L = dernière image de plus d'une seconde, dès que
300 images la suivent ; fenêtre de mesure L+300..L+600.

Fenêtre : r_mode 3 (640×480) ; plein écran : r_mode 5 (1024×768).
preyconfig.cfg est sauvegardé puis rendu. Prey se relance sans redémarrer
l'invité après un kill (contrairement à DOOM 3).
"""
from jeu import Jeu

APP = "/Users/tiger/Desktop/Prey Demo/Prey.app"
CFG = "/Users/tiger/Library/Application Support/Prey Demo/base/preyconfig.cfg"


def fin_chargement(rows, long_ms=1000, calme=300):
    """Dernière image de plus d'une seconde (chargement), si les `calme`
    images suivantes sont là (elles durent donc toutes moins d'une seconde).
    Le 26/09, la règle « > 2 s puis 300 images < 1 s » a attendu 20 min : le
    chargement de Prey finit par des images de 1,9-2 s (images 89-91)."""
    L = None
    for f in sorted(rows):
        if f - 1 in rows and rows[f][0] - rows[f - 1][0] > long_ms:
            L = f
    return L if L is not None and L + calme in rows else None


class Prey(Jeu):
    cle = "prey"
    titre = "Prey Demo"
    famille = "ARB2, VBO, DXT5"
    processus = "Prey"
    plancher_ms = 90            # 67 ms/image au 26/09 (fenêtre, sans déclencheur)
    delai_scene = 1200
    dump_images = 20

    def fichiers_reglages(self, mode):
        return [CFG]

    def commande(self, mode):
        fs, rm = ("1", "5") if mode == "pe" else ("0", "3")
        return ('cd "%s/Contents/MacOS" && ./Prey +set r_fullscreen %s +set r_mode %s +set com_showFPS 1 '
                '+set r_useARBProgram 1 +loadGame Auto___Fuite_____toute_vitesse' % (APP, fs, rm))

    def fenetre(self, rows):
        L = fin_chargement(rows)
        if L is None:
            return None
        a, b = L + 300, L + 600
        return (a, b) if b in rows else None


JEU = Prey()
