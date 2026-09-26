"""jeu.py — classe de base d'un jeu de la matrice (un module par jeu dans
tools/matrice/jeux/). Un jeu dit comment il se lance dans chaque mode, quand
sa scène fixe est atteinte (fenêtre de mesure dans frames.csv), comment il
s'arrête et ce qu'il faut sauvegarder puis rendre (fichiers de réglages).
"""

import time

MODES = ("fen", "pe")          # fenêtre, plein écran
NOM_MODE = {"fen": "fenêtre", "pe": "plein écran"}


class Jeu:
    cle = "?"                   # nom court (ligne de commande, dossiers)
    titre = "?"
    famille = ""
    processus = None            # nom du processus (ps -c) : arrêt
    processus_ui = None         # nom pour System Events (premier plan), défaut = processus
    non_automatise = {}         # mode -> raison (cellule « non automatisé »)
    plancher_ms = None          # ms/image au-delà : vitesse rouge
    redemarrer_apres = False    # DOOM 3 : kCGLBadDisplay après un kill
    delai_scene = 900           # s pour atteindre la fin de la fenêtre de mesure
    dump_images = 30            # POMPPC_GL_DUMP_FRAMES
    dump_attente = 2            # images vidées (en-têtes du vidage) avant la capture figée
    defaut_connu = ""           # défaut d'image connu (TODO §6), rappelé dans le tableau
    env = {}                    # POMPPC_GL_* en plus

    def modes(self):
        return [m for m in MODES]

    # --- réglages du jeu : sauvegarde et remise en état (configs intactes)
    def fichiers_reglages(self, mode):
        """Chemins (invité) des fichiers que le lancement peut modifier."""
        return []

    def preparer(self, h, mode):
        """Écrit les réglages du mode (après la sauvegarde faite par le pilote)."""

    def nettoyer(self, h, mode):
        """Après l'arrêt et la remise des réglages (démonter un CD…)."""

    def nom_ui(self):
        return self.processus_ui or self.processus

    # --- lancement : corps de la fonction shell `jeu` de ~/matrice/cellule.sh
    def commande(self, mode):
        raise NotImplementedError

    # --- scène fixe
    def fenetre(self, rows):
        """rows : {image: (elapsed_ms, fallbacks, readbacks)}. Rend (a, b), la
        fenêtre de mesure, quand l'image b est passée ; None sinon."""
        return None

    def pendant(self, h, rows, t):
        """Appelé à chaque relevé (toutes les ~5 s) : touches, remise au premier plan."""

    # --- arrêt
    def arreter(self, h):
        for p in h.processus(self.processus):
            h.ssh("kill %d" % p)
        for _ in range(10):
            time.sleep(1)
            if not h.processus(self.processus):
                return
        for p in h.processus(self.processus):
            h.ssh("kill -9 %d" % p)


def fenetre_fixe(rows, a, b):
    """Fenêtre d'images [a, b] fixe (jeux sans cinématique)."""
    return (a, b) if b in rows and a in rows else None
