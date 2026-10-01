# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Nexuiz 2.5.2 — moteur DarkPlaces (GPL), AGL (Carbon), chemin ARB / fixe.

Scène fixe DÉTERMINISTE : `-benchmark demos/demo1` (démo de
data20091001.pk3) rejoue la démo image par image (timedemo), quelle que soit
la vitesse, puis quitte (~2017 échanges, ~190 s). L'image n du plugin est la
même démo image d'un tour à l'autre et d'un mode à l'autre (compteur de
sommets par image identique hors quelques images de texte : 27/09) ; le
chargement occupe les ~107 premiers échanges. Fenêtre de mesure : images
120..600 (~50 s ; 107,1 / 107,4 ms/image en fenêtre / plein écran, tour 20260927-1018).
Vidage déclenché sur une image FIXE (POMPPC_GL_DUMP_TRIGGER=@870, plugin
20260927-dumpat) : la preuve porte sur la même image à chaque tour ; 870 laisse
~18 s après la fin de la fenêtre pour que la matrice attende le vidage.

Fenêtre : +vid_fullscreen 0, 800×600 (fenêtre Carbon en 100,100, sous rien) ;
plein écran : +vid_fullscreen 1, 1024×768 (la taille du bureau :
CGDisplaySwitchToMode sans changement de mode). -nosound.

PREMIER PLAN : lancé par son exécutable (`nexuiz-osx-agl` → exec
`nexuiz-osx-agl-bin`, nom qui n'est pas le CFBundleExecutable du paquet) et non
par LaunchServices, le jeu reste un processus « background only » (vid_agl.c
n'appelle pas TransformProcessType) : osascript ne peut pas le mettre devant et
le plugin replie chaque échange (« not frontmost » : Swap60 + le glFinish
Swap5c, deux replis par image, 8,9 img/s au premier essai).
tools/guest/launchers/premierplan.c, préchargé, le transforme en application
de premier plan (0 repli hors rafraîchissements, 10,1 img/s).

PROFILS : `arb` (+r_glsl 0) mesure le chemin ARB / fixe ; `glsl` (+r_glsl 1,
cellules nxg-*), automatisé depuis le protocole v21 (27/09 : programmes GLSL
exécutés par l'hôte, docs/protocole-v21-glsl.md), est aussi le chemin que le jeu
prend par défaut quand GL_ARB_fragment_shader est annoncée : ~40 ms/image
contre ~107 en ARB (DarkPlaces fait en une passe GLSL ce qu'il fait en
plusieurs passes de multitexture).
"""
import os

from jeu import Jeu, fenetre_fixe

APP = "/Users/tiger/Nexuiz/Nexuiz.app"
BASE = "/Users/tiger/Nexuiz"
CFG = "/Users/tiger/.nexuiz/data/config.cfg"
PP_C = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                    "guest", "launchers", "premierplan.c")
PP = "/Users/tiger/matrice/premierplan.dylib"

PROFILS = {
    # r_glsl vaut 1 par défaut : depuis que le plugin annonce GL_ARB_fragment_shader
    # (v21), le profil ARB doit le couper lui-même pour rester le chemin ARB.
    "arb": ("+r_glsl 0", "DarkPlaces, ARB / fixe (GLSL coupé)"),
    "glsl": ("+r_glsl 1", "DarkPlaces, GLSL"),
}


class Nexuiz(Jeu):
    processus = "nexuiz-osx-agl-bin"
    plancher_ms = 137           # 109,5 / 109,2 ms/image au 29/09 (tour 20260929-2344), ×1,25
                                # GLSL : ~40
    delai_scene = 600           # image 600 vers 80 s ; la démo finit vers 200 s
    dump_image = 870            # vidage déclenché à l'image fixe 870 (@870)
    dump_images = 24            # ~1,2 s de démo ; la capture tombe à 872 (17 Mio par image vidée)

    def __init__(self, profil="arb"):
        self.profil = profil
        self.cle = "nx" if profil == "arb" else "nxg"
        self.titre = "Nexuiz 2.5.2" + ("" if profil == "arb" else " (GLSL)")
        self.args_profil, self.famille = PROFILS[profil]
        # glsl : automatisé depuis le protocole v21 (27/09, plugin 20260927-glsl,
        # docs/protocole-v21-glsl.md) — GL_ARB_fragment_shader annoncée, programmes
        # GLSL exécutés par l'hôte. Plancher propre : ~40 ms/image au 27/09.
        if profil == "glsl":
            self.plancher_ms = 52        # GLSL : 41,1 / 41,8 au 29/09

    def fichiers_reglages(self, mode):
        return [CFG]            # -benchmark n'enregistre rien, par précaution

    def preparer(self, h, mode):
        h.depose(open(PP_C).read(), "/Users/tiger/matrice/premierplan.c")
        h.ssh("cd /Users/tiger/matrice && ( [ -f premierplan.dylib ] && [ premierplan.dylib -nt premierplan.c ] || "
              "MACOSX_DEPLOYMENT_TARGET=10.4 /usr/bin/gcc-4.0 -arch ppc -dynamiclib -O2 "
              "-framework ApplicationServices premierplan.c -o premierplan.dylib )", delai=300)

    def commande(self, mode):
        fs, w, hh = ("1", 1024, 768) if mode == "pe" else ("0", 800, 600)
        return ('cd "%s/Contents/MacOS" && DYLD_INSERT_LIBRARIES=%s ./nexuiz-osx-agl -basedir %s '
                '-nosound +vid_fullscreen %s +vid_width %d +vid_height %d %s -benchmark demos/demo1'
                % (APP, PP, BASE, fs, w, hh, self.args_profil))

    def fenetre(self, rows):
        return fenetre_fixe(rows, 120, 600)


JEUX = [Nexuiz("arb"), Nexuiz("glsl")]
JEU = JEUX[0]
