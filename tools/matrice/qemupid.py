#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""qemupid.py — PID du QEMU de la VM, jamais du shell qui l'a lancé.

`pgrep -f "qemu-system.*disks/tiger.qcow2"` retenait aussi le shell parent quand la ligne
du bras citait QEMU_BIN et DISK (04/11, bench/tcg/ab/m4-d3-terminal-0411 : profils de bash).
On lit donc le nom de l'exécutable. Les copies renommées de QEMU restent reconnues : on
écarte les interpréteurs et on exige une machine mac99, comme `Hote.charge_hote`.

    tools/matrice/qemupid.py          # affiche le PID, code 1 s'il n'y en a pas
"""
import os
import subprocess
import sys

DISQUE = "disks/tiger.qcow2"
INTERPRETES = ("zsh", "bash", "sh", "dash", "env", "python3", "Python", "python",
               "pgrep", "ps", "sample", "perf", "caffeinate", "nohup", "timeout")


def choisit(ps_sortie, disque=DISQUE):
    """PIDs retenus dans la sortie de `ps -Ao pid=,comm=,args=`. Sous macOS, comm porte le
    chemin entier : un chemin avec espaces décale args, sans effet sur les tests ci-dessous."""
    pids = []
    for ligne in ps_sortie.splitlines():
        p = ligne.split(None, 2)
        if len(p) < 3 or not p[0].isdigit():
            continue
        if os.path.basename(p[1]) in INTERPRETES:
            continue
        if "mac99" in p[2] and disque in p[2]:
            pids.append(int(p[0]))
    return pids


def pids(disque=DISQUE):
    return choisit(subprocess.run(["ps", "-Ao", "pid=,comm=,args="], capture_output=True,
                                  text=True).stdout, disque)


def profil_de_qemu(chemin):
    """Faux si un profil `sample` désigne un interpréteur (ligne « Process: nom [pid] ») ;
    vrai sinon, et pour un autre format : perf n'a pas cette ligne."""
    try:
        with open(chemin, errors="replace") as f:
            for _, ligne in zip(range(40), f):
                if ligne.startswith("Process:"):
                    mots = ligne.split()
                    return len(mots) < 2 or mots[1] not in INTERPRETES
    except OSError:
        return False
    return True


if __name__ == "__main__":
    q = pids()
    if not q:
        sys.exit(1)
    print(q[0])
