#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""fpnatd3.py — une partie de DOOM 3 (demo_mars_city1, fenêtre 640×480) sur un
RECOUVREMENT neuf de disks/tiger-endurance.qcow2, pour mesurer un QEMU d'essai
sans toucher à la VM quotidienne (docs/tcg-g4.md §22).

    tools/tcg/fpnatd3.py NOM [--qemu BIN] [--cpu OPTS] [--extra ARGS]
                             [--sample S] [--apres S]

  --qemu    qemu-system-ppc64 à lancer (défaut : celui de config.env) ;
  --cpu     propriétés de CPU EN PLUS de celles de run_tiger.sh (CPU_OPTS),
            p. ex. x-fpx-mode=1 ;
  --extra   EXTRA_ARGS de run_tiger.sh (p. ex. un greffon TCG) ;
  --sample  `sample` du processus QEMU pendant S s dans la scène fixe ;
  --apres   secondes de jeu gardées après la fenêtre (défaut 60).

Même règle que la matrice (tools/matrice/jeux/d3.py) : T = fin de la
cinématique, fenêtre T+50..T+280 ; ms/image = durée de la fenêtre / images.
Sorties : <dépôt principal>/bench/tcg/fpnat/d3/NOM/ (frames.csv, info.txt,
sample.txt, qemu.log). La VM est coupée (quit) à la fin : le recouvrement est
jeté. Politesse de mesure : attend l'absence de .run/a4-mesure.lock, tient
.run/a4-vm-tcg.lock tant que la VM tourne.
"""
import argparse
import os
import subprocess
import sys
import time

ICI = os.path.dirname(os.path.abspath(__file__))
WT = os.path.dirname(os.path.dirname(ICI))
sys.path.insert(0, os.path.join(WT, "tools", "endurance"))
sys.path.insert(0, os.path.join(WT, "tools", "matrice"))
import endurance as E                    # noqa: E402
from jeux.d3 import JEU as D3            # noqa: E402

MAIN = E.MAIN
G = "/Users/tiger/matrice"
GD = G + "/fpnat"


def charge():
    autres = subprocess.run("pgrep -fl qemu-system", shell=True, capture_output=True,
                            text=True).stdout.strip().splitlines()
    return "charge %s ; %d QEMU : %s" % (
        subprocess.run(["sysctl", "-n", "vm.loadavg"], capture_output=True, text=True).stdout.strip(),
        len(autres), " ".join(l.split()[1].replace(os.path.expanduser("~/src/"), "")
                              for l in autres if len(l.split()) > 1))


def lit_frames(txt):
    rows = {}
    for l in txt.splitlines():
        p = l.split(",")
        if len(p) >= 7 and p[0].isdigit():
            try:
                rows[int(p[0])] = (float(p[2]), int(p[5]))
            except ValueError:
                pass
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nom")
    ap.add_argument("--qemu")
    ap.add_argument("--cpu", default="")
    ap.add_argument("--extra")
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--apres", type=int, default=60)
    ap.add_argument("--slot", type=int, default=7)
    a = ap.parse_args()
    out = os.path.join(MAIN, "bench", "tcg", "fpnat", "d3", a.nom)
    os.makedirs(out, exist_ok=True)
    info = open(os.path.join(out, "info.txt"), "a")

    def note(*x):
        s = time.strftime("%H:%M:%S ") + " ".join(str(y) for y in x)
        print(s, flush=True)
        info.write(s + "\n")
        info.flush()

    mylock = os.path.join(MAIN, ".run", "a4-vm-tcg.lock")
    mesure = os.path.join(MAIN, ".run", "a4-mesure.lock")
    while True:
        while os.path.exists(mesure):
            time.sleep(30)
        open(mylock, "w").write("fpnatd3 %d %s\n" % (os.getpid(), a.nom))
        time.sleep(10)          # une mesure A4 qui partait en même temps : on lui cède
        if not os.path.exists(mesure):
            break
        os.remove(mylock)
    if a.cpu:
        os.environ["CPU_OPTS"] = a.cpu
    args = argparse.Namespace(smp=2, qemu=a.qemu, extra=a.extra)
    E.SYM = E.Symboliseur()
    vm = E.VM("fpnat-" + a.nom, a.slot, args)
    note("binaire", E.binaire_qemu(args), "CPU_OPTS+=%s" % a.cpu, "EXTRA=%s" % a.extra)
    try:
        if not vm.demarre(out):
            raise SystemExit("QEMU ne démarre pas (%s)" % vm.log)
        etat, det = E.attend_demarrage(vm, time.time(), 420, 20)
        note("démarrage", etat, det.get("t_ssh"))
        if etat != "ok":
            raise SystemExit("démarrage : %s" % etat)
        pid = int(subprocess.run(["pgrep", "-f", vm.disque], capture_output=True,
                                 text=True).stdout.split()[0])
        lance = open(os.path.join(WT, "tools", "matrice", "guest", "lance.command")).read()
        vm.ssh("mkdir -p %s; rm -rf %s; killall ScreenSaverEngine 2>/dev/null; true" % (G, GD))
        vm.ssh("cat > %s/lance.command; chmod +x %s/lance.command" % (G, G), entree=lance)
        cel = ("D='%s'\nPOMPPC_GL_STATS='1'; export POMPPC_GL_STATS\n"
               "POMPPC_GL_FRAMES='%s/frames.csv'; export POMPPC_GL_FRAMES\n"
               "jeu() {\n%s\n}\n" % (GD, GD, D3.commande("fen")))
        vm.ssh("cat > %s/cellule.sh" % G, entree=cel)
        vm.ssh("mkdir -p %s && open %s/lance.command" % (GD, G))
        t0 = time.time()
        note("jeu lancé ;", charge())
        brut, decal, rows, premier, w = "", 0, {}, 0, None
        front = ("osascript -e 'tell application \"System Events\" to set frontmost of "
                 "process \"%s\" to true'" % D3.nom_ui())
        while time.time() - t0 < 1500:
            time.sleep(10)
            code, o = vm.ssh("tail -c +%d %s/frames.csv 2>/dev/null; echo @@LOG; tail -3 %s/log.txt"
                             % (decal + 1, GD, GD), delai=60)
            if "@@LOG" not in o:
                continue
            fr, _, log = o.partition("@@LOG")
            fin = fr.rfind("\n") + 1
            brut += fr[:fin]
            decal += len(fr[:fin].encode())
            rows = lit_frames(brut)
            if "\nexit " in "\n" + log:
                raise SystemExit("le jeu a quitté : %s" % log.strip())
            if rows and premier < 2 and time.time() - t0 > 10:
                vm.ssh(front, delai=30)
                premier += 1
            elif rows:
                n = max(rows)
                if n > 40 and n - 20 in rows and rows[n][1] - rows[n - 20][1] > 5:
                    vm.ssh(front, delai=30)
            w = D3.fenetre({k: (v[0], v[1]) for k, v in rows.items()})
            if w:
                break
        if not w:
            raise SystemExit("scène non atteinte (image %s)" % (max(rows) if rows else "aucune"))
        x, y = w
        ms = (rows[y][0] - rows[x][0]) / (y - x)
        note("fenêtre %d..%d : %.1f ms/image (replis %d) ; image %d à %.0f s ; %s"
             % (x, y, ms, rows[y][1] - rows[x][1], max(rows), time.time() - t0, charge()))
        if a.sample:
            note("sample %d s du pid %d" % (a.sample, pid))
            subprocess.run(["sample", str(pid), str(a.sample), "-file",
                            os.path.join(out, "sample.txt")], capture_output=True)
            note("sample fini ;", charge())
        time.sleep(max(0, a.apres - a.sample))
        code, o = vm.ssh("cat %s/frames.csv" % GD, delai=120)
        open(os.path.join(out, "frames.csv"), "w").write(o)
        rows = lit_frames(o)
        z = max(rows)
        note("après : images %d..%d : %.1f ms/image" % (
            y, z, (rows[z][0] - rows[y][0]) / max(1, z - y)))
    finally:
        vm.quitte()
        try:
            os.remove(mylock)
        except OSError:
            pass
        note("VM coupée")


if __name__ == "__main__":
    main()
