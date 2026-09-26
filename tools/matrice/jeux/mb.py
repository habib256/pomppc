"""Marble Blast Gold — témoin du pipeline fixe (GarageGames Torque, Carbon/AGL).

Lancement : `-fullscreen` ou `-windowed` et `-mission
marble/data/missions/beginner/gems.mis` (common/main.cs et marble/main.cs,
parseArgs) : le niveau démarre tout de suite, la bille reste sur son socle
de départ, caméra immobile — c'est la scène fixe (le chrono tourne). Le niveau
est chargé vers l'image ~550 ; fenêtre de mesure aux images 900..1500
(~33 ms/image en plein écran 1024×768 au 26/09).

La démo jouée toute seule depuis l'écran-titre a été essayée et écartée : sa
vitesse varie de 15 à 120 ms/image selon le passage (ce n'est pas une scène
fixe).

Constat du 26/09/2026 : en fenêtre, Marble Blast ouvre toujours une fenêtre de
1024×768 (la taille du bureau, quelles que soient $pref::Video::resolution et
windowedRes) ; recouverte par la barre de menus, elle n'a pas la présentation
directe (« coupée ») : Swap60 et Swap58 repliés à chaque image, et le vidage
n'a pas de SURF_PRESENT (rien à rejouer en image).
"""
from jeu import Jeu, fenetre_fixe

APP = "/Users/tiger/Desktop/MarbleBlast Gold.app"
MISSION = "marble/data/missions/beginner/gems.mis"


class MarbleBlast(Jeu):
    cle = "mb"
    titre = "Marble Blast Gold"
    famille = "pipeline fixe"
    processus = "MarbleBlast Gold"
    plancher_ms = 16            # 12,3 ms/image au 26/09 (plein écran, sans déclencheur)
    delai_scene = 300
    dump_images = 60

    def commande(self, mode):
        return 'cd "%s/Contents/MacOS" && "./MarbleBlast Gold" %s -mission %s' % (
            APP, "-fullscreen" if mode == "pe" else "-windowed", MISSION)

    def fenetre(self, rows):
        return fenetre_fixe(rows, 900, 1500)


JEU = MarbleBlast()
