#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""chargehote.py — charge étrangère de l'hôte (tout sauf qemu-system), pour tools/tcg/matab.sh (06/10 :
une autre session occupait l'hôte ; docs/vitesse-doom3-x86.md §13.5).

  MATAB_AVANT="python3 tools/tcg/chargehote.py attendre bench/tcg/ab/C/charge-releve.txt" \\
      tools/tcg/matab.sh C …   (avec « chargehote.py releve bench/tcg/ab/C/charge-releve.txt » à côté)


  chargehote.py releve JOURNAL        : une ligne toutes les 5 s (heure, loadavg 1 min, cœurs occupés
                                    en tout, par qemu-system, étrangers = tout - qemu, 3 plus gros
                                    processus étrangers). Le total vient de /proc/stat : les
                                    processus courts (compilations) sont comptés.
  chargehote.py attendre JOURNAL [MAX_S] : barrière avant une partie : aucun qemu-system,
                                    étranger <= 1 cœur sur 10 s (/proc/stat) ; sinon nouvelle
                                    sonde aussitôt ; 0 quand c'est calme, 1 après MAX_S (7200).
                                    Depuis le 20/10, la charge 1 min n'entre plus en compte :
                                    elle met une minute à oublier le QEMU qu'on vient d'arrêter.
  chargehote.py invite PID JOURNAL [MAX_S] : attend que le QEMU PID soit au repos (bureau de
                                    l'invité retombé : < 0,25 cœur sur 10 s), au plus MAX_S (60) ;
                                    remplace l'attente fixe de 60 s de matab.sh."""
import os, sys, time

HZ = os.sysconf("SC_CLK_TCK")

def total():
    f = open("/proc/stat").readline().split()[1:]
    v = list(map(int, f))
    return sum(v[:3]) + sum(v[5:8])   # user nice system + irq softirq steal

def procs():
    d = {}
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            s = open("/proc/%s/stat" % p).read()
            comm = s[s.index("(") + 1:s.rindex(")")]
            f = s[s.rindex(")") + 2:].split()
            d[int(p)] = (comm, int(f[11]) + int(f[12]))
        except Exception:
            pass
    return d

def echantillon(dt):
    t0, p0, w0 = total(), procs(), time.time()
    time.sleep(dt)
    t1, p1, w1 = total(), procs(), time.time()
    el = (w1 - w0) * HZ
    tot = (t1 - t0) / el
    q = 0.0; autres = []
    for pid, (c, t) in p1.items():
        if pid in p0 and p0[pid][0] == c:
            x = (t - p0[pid][1]) / el
            if c.startswith("qemu-system"):
                q += x
            elif x > 0.02:
                autres.append((x, c, pid))
    autres.sort(reverse=True)
    nq = sum(1 for c, _ in p1.values() if c.startswith("qemu-system"))
    return tot, q, max(0.0, tot - q), autres[:3], nq

def ligne(tot, q, et, autres, nq):
    return "%s charge %.2f total %.2f qemu %.2f (%d) etranger %.2f | %s" % (
        time.strftime("%H:%M:%S"), os.getloadavg()[0], tot, q, nq, et,
        " ".join("%s:%d=%.2f" % (c, p, x) for x, c, p in autres))

mode, jl = sys.argv[1], sys.argv[2]
# en mode « invite », le second argument est le PID du QEMU et le journal vient après
out = open(jl, "a", buffering=1) if mode != "invite" else None
if mode == "releve":
    while True:
        out.write(ligne(*echantillon(5)) + "\n")
elif mode == "invite":
    # invite PID JOURNAL [MAX_S] : le QEMU de la partie est-il retombé au repos ?
    pid = int(jl)
    out = open(sys.argv[3], "a", buffering=1)
    mx = float(sys.argv[4]) if len(sys.argv) > 4 else 60
    deb = time.time()

    def cpu():
        f = open("/proc/%d/stat" % pid).read()
        f = f[f.rindex(")") + 2:].split()
        return int(f[11]) + int(f[12])
    while time.time() - deb < mx:
        a = cpu()
        time.sleep(10)
        c = (cpu() - a) / HZ / 10
        out.write("INVITE %s qemu %.2f cœur\n" % (time.strftime("%H:%M:%S"), c))
        if c < 0.25:
            break
    print("invité au repos après %.0f s" % (time.time() - deb), flush=True)
else:
    mx = float(sys.argv[3]) if len(sys.argv) > 3 else 7200
    deb = time.time()
    while True:
        r = echantillon(10)
        l = ligne(*r)
        ok = r[4] == 0 and r[2] <= 1.0
        out.write("ATTENTE %s %s\n" % ("CALME" if ok else "occupé", l))
        print(("calme : " if ok else "occupé : ") + l, flush=True)
        if ok:
            sys.exit(0)
        if time.time() - deb > mx:
            sys.exit(1)
