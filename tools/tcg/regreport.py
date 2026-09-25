#!/usr/bin/env python3
"""regreport.py REF RES ÉTIQUETTE… — bilan d'une campagne tools/tcg/regab.sh
(docs/tcg-g4.md §14) : une ligne par démarrage d'invité.

    processus  g  jit(base, fenêtre)  indice MB  img/s  call mem fp sys ctx copy (ms, médianes)

REF : bilans de référence de regidx.py (virgules) ; RES : dossier des
résultats (bench/reg/res). La fenêtre est « même » quand le tampon du JIT a les
mêmes bits 63..32 que le texte de QEMU, « AUTRE » sinon."""
import glob, os, re, statistics, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from regidx import load, index  # noqa: E402

KERN = ("call", "mem", "fp", "sys", "ctx", "copy")


def jitinfo(res, p):
    try:
        t = open(os.path.join(res, p + "-info.txt")).read()
    except OSError:
        return "?", "?"
    m = re.search(r"jit 0x([0-9a-f]+) text 0x([0-9a-f]+)", t)
    if not m:
        return "?", "?"
    j, x = int(m.group(1), 16), int(m.group(2), 16)
    return "%#x" % j, "même" if j >> 32 == x >> 32 else "AUTRE"


def bench(path):
    v = {k: [] for k in KERN}
    try:
        for l in open(path):
            a = l.split()
            if len(a) == 2 and a[0] in v:
                v[a[0]].append(float(a[1]))
    except OSError:
        pass
    return [statistics.median(v[k][1:] or v[k]) if v[k] else float("nan") for k in KERN]


def main():
    ref = [w for p in sys.argv[1].split(",") for w in load(p)]
    res = sys.argv[2]
    print("%-10s %2s %-12s %-6s %6s %6s  %s" % ("processus", "g", "jit", "fenêtre", "indice", "img/s",
                                                  " ".join("%5s" % k for k in KERN)))
    for p in sys.argv[3:]:
        base, win = jitinfo(res, p)
        for mb in sorted(glob.glob(os.path.join(res, p + "-g*-mb.txt"))):
            g = re.search(r"-g(\d+)-mb", mb).group(1)
            w = load(mb)
            ix, n = index(ref, w) if w else (float("nan"), 0)
            fps = statistics.mean(f for f, t in w) if w else float("nan")
            b = bench(mb.replace("-mb.txt", "-bench.txt"))
            print("%-10s %2s %-12s %-6s %6.3f %6.1f  %s" % (p, g, base, win, ix, fps,
                                                          " ".join("%5.0f" % x for x in b)))


if __name__ == "__main__":
    main()
