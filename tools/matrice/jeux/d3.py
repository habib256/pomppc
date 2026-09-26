"""DOOM 3 Demo — idTech4, programmes ARB2, VBO, 7 unités, DXT.

Partie `game/demo_mars_city1` lancée par `+map`, sans toucher à rien : la
cinématique d'arrivée (~20 ms/image) puis le joueur immobile au début du
niveau (~63 ms/image depuis x-fp-inline). T = fin de la cinématique, règle
de d3win.py durcie (fin_cinematique : trois tranches de 200 images au-dessus
du seuil au lieu de deux) ; fenêtre de mesure T+50..T+280, celle des lots du
verdict unique et des A/B TCG.

Fenêtre : r_mode 3 (640×480) ; plein écran : r_mode 5 (1024×768, la taille du
bureau, pas de changement de mode). Jamais de `+bind` (il s'enregistre dans
DoomConfig.cfg) ; DoomConfig.cfg est sauvegardé puis rendu quand même.
Après un kill de DOOM 3, tout lancement suivant échoue (kCGLBadDisplay) :
l'invité est redémarré après la cellule.
"""
from jeu import Jeu

APP = "/Users/tiger/Desktop/Doom 3 Demo/Doom 3 Demo.app"
CFG = "/Users/tiger/Library/Application Support/Doom 3 Demo/demo/DoomConfig.cfg"


def fin_cinematique(rows):
    """Première image a >= 1500 dont les TROIS tranches de 200 images suivantes
    dépassent S et sont stables (à 15 % près : un passage lent de la
    cinématique n'est pas un palier) avec au plus 8 replis (d3win.py n'en demande que deux : le
    26/09, une cinématique ralentie par le relevé ssh de la matrice l'a
    trompée), puis affinée au saut (d3win.py). S = 0,8 × le niveau du jeu lu
    sur les 400 dernières images (la partie tourne jusqu'au délai, joueur
    immobile) ; S fixe à 65 ms/image ne trouvait plus T depuis x-fp-inline
    (jeu à ~63). La matrice appelle cette règle EN DIRECT : les 400 dernières
    images ne sont sûrement dans le niveau que loin de la cinématique, d'où
    image >= 5000 et T accepté seulement 1000 images derrière la dernière
    (T ~3500-4100 selon la vitesse, 26/09)."""
    z = max(rows)
    if z < 5000:
        return None
    a0 = min(x for x in rows if x >= z - 400)
    s = 0.8 * (rows[z][0] - rows[a0][0]) / (z - a0)
    for f in sorted(rows):
        a = f - 600
        if a < 1500 or any(x not in rows for x in (a, a + 200, a + 400)):
            continue
        sl = [(rows[x + 200][0] - rows[x][0]) / 200 for x in (a, a + 200, a + 400)]
        if min(sl) > s and min(sl) > 0.85 * max(sl) and rows[f][1] - rows[a][1] <= 8:
            for x in range(a, a + 400):
                if x + 25 in rows and (rows[x + 25][0] - rows[x][0]) / 25 > s:
                    return x
            return a
    return None


class Doom3(Jeu):
    cle = "d3"
    titre = "DOOM 3 Demo"
    famille = "ARB2, VBO, DXT"
    processus = "Doom 3 Demo"
    plancher_ms = 100           # 73-79 ms/image au 26/09 (sans déclencheur), marge pour le bruit
    delai_scene = 1500
    dump_images = 20
    redemarrer_apres = True

    def fichiers_reglages(self, mode):
        return [CFG]

    def commande(self, mode):
        fs, rm = ("1", "5") if mode == "pe" else ("0", "3")
        return ('cd "%s/Contents/MacOS" && "./Doom 3 Demo" +set r_fullscreen %s +set r_mode %s '
                '+set com_showFPS 1 +set r_useARBProgram 1 +map game/demo_mars_city1' % (APP, fs, rm))

    def fenetre(self, rows):
        t = fin_cinematique(rows)
        if t is None or t + 1000 > max(rows):
            return None
        a, b = t + 50, t + 280
        return (a, b) if b in rows else None


JEU = Doom3()
