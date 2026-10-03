# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Unreal Tournament 2004 Demo — tableaux, VBO, S3TC, cartes de cube (SDL, Mach-O).

Scène fixe DÉTERMINISTE, reprise du job `utflyby` (tools/guest/jobs/utflyby) :
la caméra d'introduction d'AS-Convoy, sans bots, avec seed.c préchargé
(DYLD_INSERT_LIBRARIES) qui fixe le pas de simulation à 0,2 s et la graine de
srand : l'image n d'un tour est la même que celle du tour précédent.
Fenêtre de mesure : images 13..73 (60 intervalles = 12 s simulées), vidage
déclenché juste après.

Réglages (UT2004.ini, sauvegardé puis rendu) : comme utflyby,
MinDesiredFrameRate=0, UseVSync=False, MaxTextureUnits=8 ; fenêtre =
StartupFullscreen=False (640×480, WindowedViewport) ; plein écran =
StartupFullscreen=True en 1024×768. Le défaut d'arme noire corrigé en v22
(ATI_texture_env_combine3) exige un contrôle séparé en partie : cette scène
d'introduction ne montre pas l'arme du joueur.
"""
import os

from jeu import Jeu, fenetre_fixe

UT = "/Users/tiger/Desktop/Unreal Tournament 2004 Demo.app"
INI = "/Users/tiger/Library/Application Support/Unreal Tournament 2004 Demo/System/UT2004.ini"
SEED_C = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                      "guest", "jobs", "utflyby", "seed.c")
SEED = "/Users/tiger/matrice/ut-seed.dylib"


class UT2004(Jeu):
    cle = "ut"
    titre = "UT2004 Demo"
    famille = "tableaux, VBO, S3TC"
    processus = "ut2004-bin"
    plancher_ms = 36          # 26,9 / 29,3 ms/image au 29/09 (tour 20260929-2344)
    plancher_ms_linux = 104   # PC : 58,5-66 fenêtre sans vidage, 60,5-83,4 avec, au 03/10 ;
                              # les deux vCPU à 96 % (émulation), le rendu GL à 15 %
    delai_scene = 900
    dump_images = 60

    def fichiers_reglages(self, mode):
        return [INI]

    def preparer(self, h, mode):
        h.depose(open(SEED_C).read(), "/Users/tiger/matrice/ut-seed.c")
        h.ssh("cd /Users/tiger/matrice && ( [ -f ut-seed.dylib ] && [ ut-seed.dylib -nt ut-seed.c ] || "
              "MACOSX_DEPLOYMENT_TARGET=10.4 /usr/bin/gcc-4.0 -arch ppc -dynamiclib -undefined "
              "dynamic_lookup -O2 ut-seed.c -o ut-seed.dylib )", delai=300)
        fs = "True" if mode == "pe" else "False"
        h.ssh("f='%s'; sed -e 's/^FullscreenViewportX=.*/FullscreenViewportX=1024/' "
              "-e 's/^FullscreenViewportY=.*/FullscreenViewportY=768/' "
              "-e 's/^StartupFullscreen=.*/StartupFullscreen=%s/' "
              "-e 's/^MinDesiredFrameRate=.*/MinDesiredFrameRate=0.000000/' "
              "-e 's/^UseVSync=.*/UseVSync=False/' "
              "-e 's/^MaxTextureUnits=.*/MaxTextureUnits=8/' \"$f.matrice-sauve\" > \"$f\"" % (INI, fs))

    def commande(self, mode):
        return ('cd "%s/System" && DYLD_INSERT_LIBRARIES=%s UT_FLYBY_CLOCK="$D/clock.csv" '
                './ut2004-bin \'AS-Convoy?game=UT2k4Assault.ASGameInfo?NumBots=0\'%s'
                % (UT, SEED, " -fullscreen" if mode == "pe" else ""))

    def fenetre(self, rows):
        return fenetre_fixe(rows, 13, 73)


JEU = UT2004()
