#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""Une partie de DOOM 3 (ou Prey) pour le relevé du son.
usage : partie.py <étiquette> <durée_s> [fs] [jeu=d3|prey]
Lance ~/son/<jeu>.command dans l'invité, remet le jeu au premier plan tant
qu'il se replie, le laisse tourner <durée_s> secondes après le lancement, le
tue, rapatrie frames.csv / stdout.txt / log.txt dans .run/son/<étiquette>/."""
import os, subprocess, sys, time

T = "/Users/mercure/src/pomppc/tools/guest/tssh.sh"
et, duree = sys.argv[1], int(sys.argv[2])
fs = "fs" in sys.argv[3:]
jeu = "prey" if "prey" in sys.argv[3:] else ("mb" if "mb" in sys.argv[3:] else "d3")
proc = {"d3": "Doom 3 Demo", "prey": "Prey", "mb": "MarbleBlast Gold"}[jeu]
for a in sys.argv[3:]:
    if a.startswith("proc="):
        proc = a[5:]
out = "/Users/mercure/src/pomppc/.run/son/" + et
os.makedirs(out, exist_ok=True)


def ssh(cmd, timeout=60):
    try:
        r = subprocess.run([T, cmd], capture_output=True, text=True, timeout=timeout)
        return r.stdout
    except subprocess.TimeoutExpired:
        return ""


def front():
    ssh("osascript -e 'tell application \"System Events\" to set frontmost of process \"%s\" to true'" % proc)


ssh("echo 'FS=%d; export FS' > ~/son/env" % (1 if fs else 0))
t0 = time.time()
ssh("open ~/son/%s.command" % jeu)
open(out + "/t0", "w").write("%.3f\n" % t0)
print("lancé à %.3f" % t0, flush=True)
last_fb = None
premier = 0
while time.time() - t0 < duree:
    time.sleep(10)
    l = ssh("tail -1 ~/son/%s/frames.csv 2>/dev/null; tail -1 ~/son/%s/log.txt" % (jeu, jeu)).splitlines()
    if len(l) >= 2 and l[-1].startswith("exit"):
        print("le jeu a quitté :", l[-1], flush=True)
        break
    fr = l[0].split(",") if l and "," in l[0] else None
    if fr and fr[0].isdigit():
        fb = int(fr[5])
        if premier < 2 or (last_fb is not None and fb - last_fb > 5):
            front()
            premier += 1
        last_fb = fb
        print("%5.0f s image %s fallbacks %s" % (time.time() - t0, fr[0], fb), flush=True)
ssh("killall '%s' 2>/dev/null; sleep 3; killall -9 '%s' 2>/dev/null; true" % (proc, proc))
time.sleep(3)
for f in ("frames.csv", "stdout.txt", "log.txt", "note.txt", "iotrace.csv"):
    data = subprocess.run([T, "cat ~/son/%s/%s" % (jeu, f)], capture_output=True).stdout
    open(os.path.join(out, f), "wb").write(data)
print("fini, %s" % out, flush=True)
