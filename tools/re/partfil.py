#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""partfil.py SYMS_DIR SAMPLE.txt [SAMPLE2.txt …] [--ms X] — partage le fil principal
d'un jeu (`sample` pris DANS Tiger) entre quatre propriétaires :

  plugin     tout échantillon dont la pile passe par une fonction du plugin POMPPC
             (memcpy, appels au kext et noyau sous lui compris) ;
  GLEngine   sinon, pile passant par GLEngine ou OpenGL.framework/libGL (le moteur
             d'Apple et ses appels, hors plugin) ;
  attente    sinon, feuille d'attente (mach_msg_trap, semaphore_*, usleep…) ;
  jeu        le reste : code du jeu, libSystem, Carbon/Cocoa, noyau hors GL.

SYMS_DIR contient plugin.txt, glengine.txt, libgl.txt : un nom par ligne (`nm -j`
des binaires de l'invité, soulignés de tête retirés ou non). Le `sample` de Tiger
ne donne pas la bibliothèque d'un cadre, d'où ces listes. Le fil principal est le
plus chargé de ceux qui passent par le plugin (comme tools/re/sampleplug.py).
Plusieurs relevés : sommes d'échantillons. --ms X convertit les parts en ms/image."""
import re
import sys
from collections import defaultdict

ATTENTE = {"mach_msg_trap", "semaphore_wait_trap", "semaphore_timedwait_trap",
           "semaphore_wait_signal_trap", "__semwait_signal", "usleep", "nanosleep",
           "select", "kevent", "mach_wait_until", "__psynch_cvwait", "pthread_cond_wait",
           "syscall_thread_switch", "swtch_pri", "thread_switch"}

LIGNE = re.compile(r"^( *)(\d+) (.*?)\s*$")


def nom(cadre):
    c = cadre.strip()
    for coupe in ("  (in ", " + ", "  "):
        if coupe in c:
            c = c.split(coupe)[0]
    return c.strip()


def charge(d, f):
    try:
        return {l.strip().lstrip("_") for l in open("%s/%s" % (d, f)) if l.strip()}
    except FileNotFoundError:
        return set()


def fils(path):
    """[(nom du fil, [(profondeur, compte, nom)])] du graphe d'appel."""
    out, cur = [], None
    dans = False
    for l in open(path, errors="replace"):
        l = l.rstrip("\n")
        if l.startswith("Call graph:"):
            dans = True
            continue
        if not dans:
            continue
        if not l.strip():
            if cur:
                pass
            continue
        if not l.startswith(" "):
            break
        m = LIGNE.match(l)
        if not m:
            continue
        prof, n, f = len(m.group(1)), int(m.group(2)), nom(m.group(3))
        if prof == 4:
            cur = (f, [])
            out.append(cur)
        elif cur:
            cur[1].append((prof, n, f))
    return out


def feuilles(noeuds):
    """[(pile, propre)] : pour chaque nœud, son compte moins celui de ses enfants."""
    res, pile = [], []
    for i, (p, n, f) in enumerate(noeuds):
        while pile and pile[-1][0] >= p:
            pile.pop()
        pile.append((p, f))
        enf = 0
        for p2, n2, _ in noeuds[i + 1:]:
            if p2 <= p:
                break
            if p2 == p + 2:
                enf += n2
        if n - enf > 0:
            res.append(([x[1] for x in pile], n - enf))
    return res


def main():
    a = sys.argv[1:]
    ms = None
    if "--ms" in a:
        i = a.index("--ms")
        ms = float(a[i + 1])
        del a[i:i + 2]
    syms, chemins = a[0], a[1:]
    plug = charge(syms, "plugin.txt")
    gle = charge(syms, "glengine.txt") | charge(syms, "libgl.txt")
    tot = defaultdict(int)
    detail = defaultdict(lambda: defaultdict(int))
    for c in chemins:
        candidats = []
        for nomfil, noeuds in fils(c):
            fs = feuilles(noeuds)
            via = sum(k for pile, k in fs if any(x.lstrip("_") in plug for x in pile))
            candidats.append((via, sum(k for _, k in fs), nomfil, fs))
        candidats.sort(reverse=True)
        if not candidats:
            continue
        _, n, nomfil, fs = candidats[0]
        print("%s : fil %s, %d échantillons" % (c, nomfil, n), file=sys.stderr)
        for pile, k in fs:
            propres = [x.lstrip("_") for x in pile]
            if any(x in plug for x in propres):
                cle = "plugin"
                if any(x.startswith(("io_connect_", "IOConnect")) for x in propres):
                    detail["plugin → kext (appels IOKit, noyau compris)"]["*"] += k
            elif any(x in gle for x in propres):
                cle = "GLEngine"
            elif propres[-1] in ATTENTE:
                cle = "attente"
            else:
                cle = "jeu"
            tot[cle] += k
            detail[cle][propres[-1]] += k
    n = sum(tot.values())
    if not n:
        sys.exit("aucun échantillon")
    print("| propriétaire | part du fil principal |%s" % (" ms/image |" if ms else ""))
    print("|---|---|%s" % ("---|" if ms else ""))
    for cle in ("jeu", "GLEngine", "plugin", "attente"):
        p = tot[cle] / n
        print("| %s | %.1f %% |%s" % (cle, 100 * p, " %.1f |" % (p * ms) if ms else ""))
    for cle in sorted(k for k in detail if k.startswith("plugin →")):
        k = detail[cle]["*"]
        print("| dont %s | %.1f %% |%s" % (cle, 100 * k / n, " %.1f |" % (k / n * ms) if ms else ""))
    print("\nPropre le plus lourd par propriétaire :")
    for cle in ("jeu", "GLEngine", "plugin", "attente"):
        top = sorted(detail[cle].items(), key=lambda x: -x[1])[:8]
        print("  %s : %s" % (cle, ", ".join("%s %.1f %%" % (f, 100 * k / n) for f, k in top)))


if __name__ == "__main__":
    main()
