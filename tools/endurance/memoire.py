# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Vidage physique de la RAM mac99, invité déjà arrêté par le collecteur."""
import json
import os
import re
import shutil
import time

from hmp import hmp


def plage_ram(mtree):
    """Plage inclusive de la RAM principale ; mtree peut la répéter."""
    plages = set()
    for debut, fin in re.findall(
            r"^\s*([0-9a-f]+)-([0-9a-f]+) \(prio -?\d+, ram\): ppc_core99\.ram\s*$",
            mtree, re.M):
        plages.add((int(debut, 16), int(fin, 16)))
    if len(plages) != 1:
        raise ValueError("plage ppc_core99.ram absente ou ambiguë")
    debut, fin = plages.pop()
    if debut != 0 or fin < debut or fin >= 1 << 32:
        raise ValueError("plage RAM mac99 inattendue")
    return debut, fin - debut + 1


def capture(mon, dossier):
    """Avant tout cont/reset : RAM seule, sans MMIO ; échec documenté sans lever.

    Le chemin et la taille sont ceux vus par QEMU sur le même hôte. Une réponse
    HMP vide ne suffit pas : vérifier aussi la taille du fichier réellement écrit.
    """
    t0 = time.monotonic()
    info = {"fichier": "ram.bin", "complet": False}
    try:
        mtree = hmp(mon, "info mtree")
        with open(os.path.join(dossier, "mtree.txt"), "w") as f:
            f.write(mtree)
        debut, taille = plage_ram(mtree)
        info.update(adresse=debut, octets_attendus=taille)
        if shutil.disk_usage(dossier).free < taille + 64 * 1024 * 1024:
            raise OSError("espace insuffisant pour la RAM et 64 Mio de marge")
        chemin = os.path.abspath(os.path.join(dossier, "ram.bin"))
        # Ne jamais confondre un ancien vidage avec le résultat de cette lecture.
        if os.path.lexists(chemin):
            raise FileExistsError("ram.bin existe déjà ; conservé sans écrasement")
        sortie = hmp(mon, "pmemsave 0x%x 0x%x %s" %
                     (debut, taille, json.dumps(chemin, ensure_ascii=False)), delai=60)
        info["sortie_hmp"] = sortie
        info["octets_ecrits"] = os.path.getsize(chemin)
        if sortie.strip() or info["octets_ecrits"] != taille:
            raise OSError("pmemsave incomplet ou erreur du moniteur")
        info["complet"] = True
    except (OSError, ValueError) as e:
        info["erreur"] = "%s: %s" % (type(e).__name__, e)
    info["duree_s"] = round(time.monotonic() - t0, 3)
    return info
