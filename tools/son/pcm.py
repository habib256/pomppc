#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""Analyse du PCM enregistré par le Screamer (S16 gros-boutiste stéréo 44,1 kHz).
usage : pcm.py fichier.raw [debut_s fin_s] [--wav sortie.wav]
Sort : trous de zéros exacts >= 2 ms, sauts (|Δ| > 12000), creux d'enveloppe
(fenêtre 5 ms > 25 dB sous la médiane des 200 ms voisines, puis retour)."""
import sys, struct, array, math, wave

f = sys.argv[1]
args = [a for a in sys.argv[2:] if not a.startswith("--")]
wavout = None
if "--wav" in sys.argv:
    wavout = sys.argv[sys.argv.index("--wav") + 1]
    args = [a for a in args if a != wavout]
data = open(f, "rb").read()
a = array.array("h", data[: len(data) // 4 * 4])
if sys.byteorder == "little":
    a.byteswap()
n = len(a) // 2
R = 44100
d0 = int(float(args[0]) * R) if args else 0
d1 = min(n, int(float(args[1]) * R)) if len(args) > 1 else n
print("durée %.1f s, analyse %.1f..%.1f" % (n / R, d0 / R, d1 / R))
if wavout:
    w = wave.open(wavout, "wb")
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(R)
    b = array.array("h", a[2 * d0: 2 * d1])
    if sys.byteorder == "big":
        b.byteswap()
    w.writeframes(b.tobytes()); w.close()

# zéros exacts
zr = 0
zeros = []
for i in range(d0, d1):
    if a[2 * i] == 0 and a[2 * i + 1] == 0:
        zr += 1
    else:
        if zr >= 88:
            zeros.append((i - zr, zr))
        zr = 0
# enveloppe 5 ms
W = 220
env = []
for s in range(d0, d1 - W, W):
    e = 0
    for i in range(s, s + W):
        l = a[2 * i]; r = a[2 * i + 1]
        e += l * l + r * r
    env.append(10 * math.log10(e / (2 * W) + 1))
creux = []
for k in range(20, len(env) - 20):
    voisins = sorted(env[k - 20:k - 2] + env[k + 3:k + 21])
    med = voisins[len(voisins) // 2]
    if med > 40 and env[k] < med - 25:
        creux.append((d0 + k * W, med - env[k]))
sauts = 0
for i in range(d0 + 1, d1):
    if abs(a[2 * i] - a[2 * i - 2]) > 12000 or abs(a[2 * i + 1] - a[2 * i - 1]) > 12000:
        sauts += 1
print("trous de zéros >= 2 ms : %d" % len(zeros))
for s, l in zeros[:40]:
    print("  %8.3f s  %6.1f ms" % (s / R, l * 1000 / R))
print("creux d'enveloppe (5 ms, > 25 dB) : %d" % len(creux))
prev = -1e9
for s, db in creux:
    if s - prev > R * 0.02:
        print("  %8.3f s  -%.0f dB" % (s / R, db))
    prev = s
print("sauts > 12000 : %d" % sauts)
# enveloppe grossière (niveau médian par seconde)
print("niveau par seconde (dB) :", " ".join("%.0f" % sorted(env[i:i + 200])[100] for i in range(0, len(env) - 200, 200)))
