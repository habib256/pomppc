"""Return to Castle Wolfenstein — idTech3, pipeline fixe + multitexture (CFM).

Non automatisé : le jeu n'est plus sur le disque quotidien au 26/09/2026
(~/rtcw.command vise ~/Desktop/Wolfenstein, absent ; seules ses préférences
restent) ; et il quittait après 8 s sans rien dessiner (TODO §6).
"""
from jeu import Jeu

RAISON = ("absent du disque quotidien (~/Desktop/Wolfenstein manque, ~/rtcw.command le vise) ; "
          "quittait après 8 s sans dessiner (TODO §6)")


class RTCW(Jeu):
    cle = "rtcw"
    titre = "RTCW"
    famille = "idTech3, pipeline fixe"
    processus = "Wolfenstein SP"
    non_automatise = {"fen": RAISON, "pe": RAISON}


JEU = RTCW()
