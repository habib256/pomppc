"""Colin McRae Rally 2005 — programmes ARB via IndirectX (Feral, Mach-O).

Non automatisé au 26/09/2026. Ce qu'il faudrait (tout existe à la main,
.run/cmr/cycle.sh) : monter ~/Desktop/Colin_McRae_Ready_Mac-1.dmg, lancer,
deux Entrée (« Jouer », nom du pilote) pour arriver en course ; le jeu ne
s'arrête pas par un signal ordinaire, cycle.sh le tue par le stub GDB de
QEMU (.run/cmr/killgame.py : PC mis à 0 sur un vCPU en mode utilisateur dans
le jeu). La géométrie éclate en course (TODO §6) : la cellule serait rouge.
"""
from jeu import Jeu

RAISON = ("course atteinte par touches + arrêt par le stub GDB de QEMU (.run/cmr/cycle.sh, "
          "killgame.py) : pas encore porté dans la matrice ; géométrie éclatée en course (TODO §6)")


class ColinMcRae(Jeu):
    cle = "cmr"
    titre = "Colin McRae Rally 2005"
    famille = "ARB via IndirectX"
    processus = "Colin McRae Rally Mac"
    non_automatise = {"fen": RAISON, "pe": RAISON}


JEU = ColinMcRae()
