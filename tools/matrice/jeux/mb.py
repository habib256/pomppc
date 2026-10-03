# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
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

Fenêtre : 800×600, en réglant les DEUX exports de préférences Torque
(common/client et marble/client sous ~/Library/MarbleBlast). Ne modifier
qu'un des deux laisse l'autre rétablir 1024×768 : la fenêtre dépasse alors
du bureau et perd la présentation directe. Les préférences et leurs caches
DSO sont sauvegardés puis rendus après chaque cellule.
"""
import re

from jeu import Jeu, fenetre_fixe

APP = "/Users/tiger/Desktop/MarbleBlast Gold.app"
MISSION = "marble/data/missions/beginner/gems.mis"
PREFS = ["/Users/tiger/Library/MarbleBlast/%s/client/prefs.cs" % part
         for part in ("common", "marble")]


def preferences_video(txt, mode):
    """Les deux exports Torque portent les mêmes clés ; régler les deux."""
    values = {"resolution": "800 600 32" if mode == "fen" else "1024 768 32",
              "windowedRes": "800 600", "fullScreen": "0" if mode == "fen" else "1"}
    for key, value in values.items():
        pattern = r'(?im)^\$pref::Video::%s\s*=.*?;\s*$' % key
        txt = re.sub(pattern, "", txt)
        txt += '\n$pref::Video::%s = "%s";\n' % (key, value)
    return txt


class MarbleBlast(Jeu):
    cle = "mb"
    titre = "Marble Blast Gold"
    famille = "pipeline fixe"
    processus = "MarbleBlast Gold"
    plancher_ms = 13          # 9,3 fenêtre / 10,2 plein écran au 29/09 (tour 20260929-2344, hôte au repos)
    plancher_ms_linux = 26    # PC (i7-10700F, RTX 4060 Ti) : 20,4-21,0 fenêtre / 19,8-20,0 plein écran au 03/10
    delai_scene = 300
    dump_images = 60

    def fichiers_reglages(self, mode):
        return PREFS + [p + ".dso" for p in PREFS]

    def preparer(self, h, mode):
        for path in PREFS:
            code, txt = h.ssh("cat '%s'" % path)
            if code or not h.depose(preferences_video(txt, mode), path):
                raise RuntimeError("préférences Marble Blast : " + path)

    def commande(self, mode):
        return 'cd "%s/Contents/MacOS" && "./MarbleBlast Gold" %s -mission %s' % (
            APP, "-fullscreen" if mode == "pe" else "-windowed", MISSION)

    def fenetre(self, rows):
        return fenetre_fixe(rows, 900, 1500)


JEU = MarbleBlast()
