#!/usr/bin/env python3
"""psmsum.py FICHIER-psM [DÉBUT FIN] — CPU hôte par fil de QEMU (docs/smp-coeurs.md).

FICHIER : relevés `ps -M -p <pid>` successifs (« == HH:MM:SS » puis la sortie),
écrits par tools/tcg/smpab.sh toutes les 20 s. Le temps cumulé (système +
utilisateur) de chaque fil est différencié entre le premier relevé ≥ DÉBUT et le
dernier ≤ FIN (HH:MM:SS ; défaut : toute la vie du processus) ; imprime la part
d'un cœur hôte de chaque fil occupé (> 1 %), triée, et la somme."""
import re, sys

def secs(t):
    p = [float(x.replace(",", ".")) for x in t.split(":")]
    return sum(v * 60 ** i for i, v in enumerate(reversed(p)))

snaps = []  # (heure s, [temps cumulé par fil])
cur = None
for l in open(sys.argv[1], errors="replace"):
    if l.startswith("== "):
        cur = (secs(l[3:].strip()), [])
        snaps.append(cur)
        continue
    if cur is None or l.startswith("USER"):
        continue
    f = l.split()
    # ligne du processus : USER PID TT %CPU STAT PRI STIME UTIME COMMAND… ;
    # lignes de fil : PID %CPU STAT PRI STIME UTIME
    if len(f) >= 9 and not f[0].isdigit():
        st, ut = f[6], f[7]
    elif len(f) >= 6 and f[0].isdigit():
        st, ut = f[4], f[5]
    else:
        continue
    cur[1].append(secs(st) + secs(ut))

snaps = [s for s in snaps if s[1]]
if len(sys.argv) > 3:
    a, b = secs(sys.argv[2]), secs(sys.argv[3])
    snaps = [s for s in snaps if a <= s[0] <= b]
if len(snaps) < 2:
    sys.exit("moins de deux relevés dans la fenêtre")
s0, s1 = snaps[0], snaps[-1]
dt = s1[0] - s0[0]
n = min(len(s0[1]), len(s1[1]))
rows = sorted(((s1[1][i] - s0[1][i]) / dt, i) for i in range(n))[::-1]
print("fenêtre %.0f s, %d fils" % (dt, n))
tot = 0.0
for v, i in rows:
    tot += v
    if v > 0.01:
        print("  fil %2d : %5.1f %% d'un cœur" % (i, 100 * v))
print("  somme : %.2f cœur" % tot)
