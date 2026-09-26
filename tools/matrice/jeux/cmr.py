"""Colin McRae Rally 2005 — programmes ARB via IndirectX (Feral, Mach-O).

Non automatisé au 26/09/2026. Ce qu'il faudrait (tout existe à la main,
tools/guest/cycle.sh) : monter ~/Desktop/Colin_McRae_Ready_Mac-1.dmg, lancer,
trois Entrée (« Jouer » du dialogue d'options, écran titre, menu) pour arriver
en course ; le jeu ne s'arrête pas par un kill ordinaire : `sudo kill -9`
suffit (26/09), tools/guest/killgame.py (PC mis à 0 par le stub GDB de QEMU)
pas toujours. Depuis le 26/09 la géométrie est juste (GL_APPLE_vertex_array_range,
docs/re/cmr-var.md), mais l'image affichée est celle du rendu d'Apple (rendu
vers texture rectangle replié à chaque image, TODO §6) : la cellule serait
rouge (capture ≠ rejeu). Entre deux lancements, fermer le dialogue de plantage
(killall UserNotificationCenter), sinon la première Entrée s'y perd.
"""
from jeu import Jeu

RAISON = ("course atteinte par touches + arrêt par le stub GDB de QEMU (tools/guest/cycle.sh, "
          "killgame.py) : pas encore porté ; géométrie juste depuis le 26/09 mais image affichée "
          "repliée chez Apple (rendu vers texture rectangle, TODO §6)")


class ColinMcRae(Jeu):
    cle = "cmr"
    titre = "Colin McRae Rally 2005"
    famille = "ARB via IndirectX"
    processus = "Colin McRae Rally Mac"
    non_automatise = {"fen": RAISON, "pe": RAISON}


JEU = ColinMcRae()
