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
  chargehote.py attendre JOURNAL [MAX_S] : barrière avant une partie : charge 1 min < 1, aucun
                                    qemu-system, étranger <= 1 cœur sur 20 s ; sinon sonde toutes
                                    les 90 s ; 0 quand c'est calme, 1 après MAX_S (7200)."""
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
out = open(jl, "a", buffering=1)
if mode == "releve":
    while True:
        out.write(ligne(*echantillon(5)) + "\n")
else:
    mx = float(sys.argv[3]) if len(sys.argv) > 3 else 7200
    deb = time.time()
    while True:
        r = echantillon(20)
        l = ligne(*r)
        ok = os.getloadavg()[0] < 1 and r[4] == 0 and r[2] <= 1.0
        out.write("ATTENTE %s %s\n" % ("CALME" if ok else "occupé", l))
        print(("calme : " if ok else "occupé : ") + l, flush=True)
        if ok:
            sys.exit(0)
        if time.time() - deb > mx:
            sys.exit(1)
        time.sleep(70)
