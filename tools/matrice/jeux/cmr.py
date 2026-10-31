# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Colin McRae Rally 2005 — programmes ARB via IndirectX (Feral, Mach-O),
rendu vers texture (aglSurfaceTexture, textures rectangle).

Scène fixe : le départ de la spéciale ESP 1 Selardu en contre la montre
(temps clair, matin), voiture arrêtée sur la ligne ; la voiture tirée au sort
change (Impreza, 206, Golf), pas le décor. Chemin (27/09,
docs/re/cmr-rendu-vers-texture.md) : « Jouer » du dialogue d'options (Entrée
par osascript, jeu au premier plan), écran titre (attendu à >= 50 teintes à
l'écran), puis Entrées TENUES par le moniteur (`sendkey ret 300` : l'Entrée
d'osascript ne passe pas les menus) : titre → film → MENU → DÉFIS → CONTRE LA
MONTRE → ÉTAPES → joueurs → nom → ÉTAPES (course) → COURSE, puis ~70 s de
chargement. Une Entrée peut se perdre dans une transition : sans pause de
chargement 40 s après la dernière, une autre est envoyée (au plus 4).

Le plugin compte des ÉCHANGES : en course, trois par image du jeu (plein
écran + deux aglSwapBuffers de la cible 800×600 cachée) ; echanges_par_image
ramène ms/image à l'image du jeu (70 ms au 27/09, plugin 20260927-rtt).
Fenêtre de mesure : après la touche COURSE, la première tranche de 600
échanges régulière (deux moitiés à 10 % près, > 10 ms/échange : le survol
d'avant le départ tourne plus vite) et sans pause de chargement (> 500 ms).
Seuil abaissé de 18 à 15 le 01/10 : avec POMPPC_GL_NATSHM la course tourne à
~17 ms/échange (~22 sans), et la cellule restait rouge, course non reconnue.
Puis à 10 le 26/10 (M4, série du PC allumée, docs/parite-m4-2510.md) : la course
tourne à 12,8-12,9 ms/échange, le jeu avant la pause de chargement du départ à
5,8-6,3 — 10 sépare les deux avec de la marge des deux côtés.

Fenêtre non automatisée : le dialogue d'options n'offre que résolution,
couleurs et FSAA, le jeu s'ouvre toujours en plein écran (800×600, changement
de mode de l'écran). Arrêt : `sudo killall -9` (un kill ordinaire ne l'arrête
pas) ; avant le lancement, le dialogue de plantage est fermé
(killall UserNotificationCenter) sinon l'Entrée de « Jouer » s'y perd.
L'invité est redémarré après la cellule (jeu GL tué).
"""
import os
import subprocess
import time

from hote import MAIN
from jeu import Jeu

DMG = "/Users/tiger/Desktop/Colin_McRae_Ready_Mac-1.dmg"
VOL = "/Volumes/Colin McRae Ready Mac"
APP = VOL + "/Colin McRae Rally Mac.app"
PPMCMP = os.path.join(MAIN, "bench", "matrice", "bin", "ppmcmp")

# attente (s) après chaque Entrée tenue, depuis l'écran titre jusqu'à COURSE
TOUCHES = [25, 10, 10, 10, 10, 8, 8, 8, 0]
# sommets bruts par échange en course (HUD et voiture : 2 056-2 173 selon la voiture) ;
# la transition d'avant la course en fait au plus 1 281
SOMMETS_COURSE = 1700


class ColinMcRae(Jeu):
    cle = "cmr"
    titre = "Colin McRae Rally 2005"
    famille = "ARB via IndirectX, rendu vers texture"
    processus = "Colin McRae Rally Mac"
    plancher_ms = 88          # 70,7 ms/image du jeu au 29/09 (tour 20260929-2344, 3 échanges par image)
    plancher_ms_linux = 105   # PC, binaire rapide, plein écran, EN COURSE : 79,9-84,3 (31/10,
                              # cmr-course-0111) ×1,25 ; les 44-48 d'avant (28-31/10) mesuraient la
                              # transition d'avant la course (voir fenetre)
    echanges_par_image = 3
    delai_scene = 900
    dump_images = 30            # 10 images du jeu
    capture_delay = 12          # 4 images du jeu
    redemarrer_apres = True
    non_automatise = {"fen": "toujours en plein écran : le dialogue d'options n'offre que "
                             "résolution, couleurs et FSAA (aucun réglage de fenêtre connu)"}
    etat, suivant, n0, touche, relances = "jouer", 20, None, 0, 0

    def preparer(self, h, mode):
        self.etat, self.suivant, self.n0, self.touche, self.relances = "jouer", 20, None, 0, 0
        h.ssh("killall UserNotificationCenter crashdump 2>/dev/null; [ -d '%s' ] || "
              "hdiutil attach -readonly -noverify '%s' >/dev/null 2>&1; true" % (VOL, DMG), delai=120)

    def nettoyer(self, h, mode):
        h.ssh("hdiutil detach '%s' >/dev/null 2>&1; true" % VOL, delai=60)

    def commande(self, mode):
        return 'cd "%s/Contents/MacOS" && "./Colin McRae Rally Mac"' % APP

    def blanc(self, h):
        """Part des pixels presque blancs à l'écran (l'écran titre en a ~90 %)."""
        p = "/tmp/matrice-cmr-%d.ppm" % os.getpid()
        if not h.capture(p):
            return 0.0
        try:
            from PIL import Image
            im = Image.open(p).convert("L").resize((160, 120))
            px = list(im.getdata())
            return sum(1 for v in px if v > 230) / len(px)
        finally:
            os.remove(p)

    def teintes(self, h):
        p = "/tmp/matrice-cmr-%d.ppm" % os.getpid()
        if not h.capture(p):
            return 0
        o = subprocess.run([PPMCMP, "-s", p], capture_output=True, text=True).stdout.split()
        os.remove(p)
        return int(o[4]) if len(o) == 5 else 0

    def charge(self, rows):
        """Une pause de chargement (> 500 ms entre deux échanges) depuis la dernière
        touche : la course se charge. Les menus n'en font pas (27/09)."""
        return any(i - 1 in rows and rows[i][0] - rows[i - 1][0] > 500
                   for i in rows if i > self.n0)

    def cap(self, h, t, quoi):
        """MATRICE_CMR_CAPTURES=dossier : une capture par touche (diagnostic des menus)."""
        d = os.environ.get("MATRICE_CMR_CAPTURES")
        if d:
            os.makedirs(d, exist_ok=True)
            h.capture(os.path.join(d, "%04d-%s.ppm" % (t, quoi)))

    def pendant(self, h, rows, t):
        if os.environ.get("MATRICE_CMR_CAPTURES") and t >= getattr(self, "cap_t", 0):
            self.cap_t = t + 10
            self.cap(h, t, self.etat)
        if self.etat == "course":
            # une Entrée perdue (tombée pendant une transition : 27/09, arrêt sur
            # NATIONALITÉ) : sans chargement 40 s après la dernière touche, on en
            # renvoie une, au plus 4 fois (en trop, l'Entrée des menus reste sur COURSE)
            if (t >= self.suivant + 40 and self.relances < 4 and rows
                    and not self.charge(rows)):
                self.cap(h, t, "avant-relance")
                h.hmp("sendkey ret 300")
                self.relances += 1
                self.suivant, self.n0 = t, max(rows)
            return
        if t < self.suivant:
            return
        if self.etat == "jouer":        # « Jouer » du dialogue d'options
            # 28/10 (PC) : une alerte « Low-end machine detected » précède le dialogue
            # sur un hôte lent et prend la première Entrée ; tant que le jeu n'a fait
            # aucun échange GL, il est encore dans ces dialogues : Entrée de nouveau
            # (au plus 3 ; le script d'interface de Tiger ne voit pas ces fenêtres)
            if rows or self.touche >= 3:
                self.etat, self.touche, self.suivant = "titre", 0, t + 20
                return
            h.ssh("osascript -e 'tell application \"System Events\" to tell process \"%s\" to set "
                  "frontmost to true' -e 'delay 1' -e 'tell application \"System Events\" to "
                  "keystroke return'" % self.processus, delai=30)
            self.touche += 1
            self.suivant = t + 12
        elif self.etat == "titre":
            # 28/10 (PC) : l'écran titre (« Appuyez sur ENTRÉE pour jouer », fond blanc)
            # ne reste que quelques secondes avant le film de démonstration, et une
            # Entrée sur ce film fait quitter le jeu (exit 1) : on le guette toutes les
            # 2 s (au plus 60 s) et la première Entrée part dès qu'il est là. Le seuil
            # de 50 teintes d'avant le prenait au film ou aux logos selon l'hôte.
            for _ in range(30):
                if self.blanc(h) >= 0.6:
                    self.etat, self.touche, self.suivant = "menus", 0, t
                    break
                time.sleep(2)
            else:
                return
        if self.etat == "menus" and t >= self.suivant:
            self.cap(h, t, "avant-touche%d" % self.touche)
            h.hmp("sendkey ret 300")
            self.suivant = t + TOUCHES[self.touche]
            self.touche += 1
            if self.touche == len(TOUCHES):
                self.etat, self.n0, self.suivant = "course", max(rows) if rows else 0, t

    def fenetre(self, rows):
        if self.n0 is None or not rows:
            return None
        z = max(rows)
        # 28/10 (PC) : le menu NATIONALITÉ tourne à ~14 ms/échange, au-dessus du seuil
        # de 10 réglé sur le M4 ; la course ne vient qu'après sa pause de chargement
        # (> 500 ms entre deux échanges), on ne cherche donc qu'après la première
        charge = [i for i in rows if i > self.n0 and i - 1 in rows
                  and rows[i][0] - rows[i - 1][0] > 500]
        if not charge:
            return None

        def ms(i, j):
            return (rows[j][0] - rows[i][0]) / (j - i)

        def sommets(i, j):
            return (rows[j][4] - rows[i][4]) / (j - i)
        for x in range(min(charge), z - 600, 50):
            if any(i not in rows for i in (x, x + 300, x + 600)):
                continue
            s1, s2 = ms(x, x + 300), ms(x + 300, x + 600)
            if min(s1, s2) < 10 or abs(s1 - s2) > 0.1 * max(s1, s2):
                continue
            # 01/11 (PC) : entre le premier chargement et la course, une transition
            # (~1 281 sommets par échange, puis 2) tourne à 12-16 ms/échange sur le PC,
            # au-dessus du seuil de 10, et le PC la mesurait depuis le 28/10 au lieu de
            # la course (2 109-2 173 sommets, ~28 ms/échange). Les sommets sont comptés
            # dans l'invité : le critère ne dépend pas de la vitesse de l'hôte.
            if min(sommets(x, x + 300), sommets(x + 300, x + 600)) < SOMMETS_COURSE:
                continue
            if any(i + 1 in rows and i in rows and rows[i + 1][0] - rows[i][0] > 500
                   for i in range(x, x + 600)):
                continue
            return (x, x + 600)
        return None

    def arreter(self, h):
        h.ssh("echo tiger974 | sudo -S killall -9 '%s' 2>/dev/null; true" % self.processus)
        time.sleep(3)


JEU = ColinMcRae()
