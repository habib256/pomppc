#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""ppc32mtsum.py — tableau des démarrages de tools/tcg/ppc32mt-bench.sh.

    ppc32mtsum.py bench/ppc32mt/g1-ppc-a bench/ppc32mt/g1-ppc64-a …
    ppc32mtsum.py --ref g1-ppc --cmp g1-ppc64 bench/ppc32mt/g1-*   (regroupe par préfixe)

Par démarrage : médiane de chaque noyau de mtbench (ms), médiane des passes de
t2bench (s), instructions hôte (mode utilisateur, milliards) par job. Avec
--ref/--cmp : médiane des démarrages de chaque groupe et écart cmp/ref."""
import os, re, statistics, sys

def load(d):
    r = {"mt": {}, "t2": [], "ins": {}, "cyc": {}, "atom": ""}
    for l in open(os.path.join(d, "run.log"), errors="replace"):
        m = re.match(r"mt (\w+) (\d+)", l)
        if m:
            r["mt"].setdefault(m.group(1), []).append(int(m.group(2)))
        m = re.match(r"passe \d+ rc=\d+ ([\d.]+) s", l)
        if m:
            r["t2"].append(float(m.group(1)))
        m = re.match(r"perf (\S+) (\d+),,(instructions|cycles):u", l)
        if m:
            r["ins" if m.group(3) == "instructions" else "cyc"][m.group(1)] = int(m.group(2))
        if l.startswith("atom bilan"):
            r["atom"] = l.split()[2]
    return r

def row(name, r):
    mt = {k: statistics.median(v) for k, v in r["mt"].items()}
    t2 = statistics.median(r["t2"]) if r["t2"] else None
    return name, mt, t2, r["ins"], r["atom"]

def main(a):
    ref = cmp_ = None
    if "--ref" in a:
        i = a.index("--ref"); ref = a[i + 1]; del a[i:i + 2]
    if "--cmp" in a:
        i = a.index("--cmp"); cmp_ = a[i + 1]; del a[i:i + 2]
    rows = [row(os.path.basename(d.rstrip("/")), load(d)) for d in a]
    keys = ["call", "mem", "fp", "sys", "ctx", "copy", "gcc", "vfp", "lmw", "par2"]
    print("| démarrage | " + " | ".join(keys) + " | somme | t2bench (s) | Ginsn mtbench | Ginsn t2bench | atom |")
    print("|---" * (len(keys) + 6) + "|")
    for n, mt, t2, ins, atom in rows:
        s = sum(mt.get(k, 0) for k in keys)
        print("| %s | %s | %d | %s | %s | %s | %s |" % (
            n, " | ".join("%d" % mt[k] if k in mt else "—" for k in keys), s,
            "%.3f" % t2 if t2 else "—",
            "%.1f" % (ins["mtbench"] / 1e9) if "mtbench" in ins else "—",
            "%.1f" % (ins["t2bench"] / 1e9) if "t2bench" in ins else "—", atom or "—"))
    if ref and cmp_:
        def grp(p):
            g = [r for r in rows if r[0].startswith(p + "-")]
            out = {}
            for k in keys:
                v = [r[1][k] for r in g if k in r[1]]
                if v:
                    out[k] = statistics.median(v)
            out["somme"] = statistics.median([sum(r[1].get(k, 0) for k in keys) for r in g])
            t = [r[2] for r in g if r[2]]
            if t:
                out["t2bench"] = statistics.median(t)
            for j in ("mtbench", "t2bench"):
                v = [r[3][j] for r in g if j in r[3]]
                if v:
                    out["ins-" + j] = statistics.median(v)
            return out, len(g)
        R, nr = grp(ref); C, nc = grp(cmp_)
        print("\n%s (%d démarrages) contre %s (%d) : écart de %s" % (cmp_, nc, ref, nr, cmp_))
        for k in keys + ["somme", "t2bench", "ins-mtbench", "ins-t2bench"]:
            if k in R and k in C:
                print("  %-12s %12.3f %12.3f  %+6.1f %%" % (k, R[k], C[k], 100 * (C[k] / R[k] - 1)))

if __name__ == "__main__":
    main(sys.argv[1:])
