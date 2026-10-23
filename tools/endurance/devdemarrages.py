#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""devdemarrages.py — démarrages répétés du BUREAU de Tiger sur un disque de DEV
(raw, tools/guest/devloop.py), sans ssh : l'invité est interrogé par l'agent de
devloop (StartupItem POMPPCAgent, boîtes aux lettres sur le disque). Pour un disque
de dev où le banc tools/endurance/endurance.py ne peut pas entrer (pas de clé ssh
du banc dans l'invité) — docs/ppc32-mttcg.md §10.

    DEVDISK=disks/tiger-dev-ppc32.raw QEMU_BIN=…/qemu-system-ppc SMP=2 PPC32MT=1 \\
        python3 tools/endurance/devdemarrages.py --nom p32-a -n 40 --froid-tous 5

Un cycle :
  - départ « froid » (QEMU neuf : `devloop.py start --gui`) tous les --froid-tous
    cycles, « reboot » (`shutdown -r now` dans l'invité, même QEMU) sinon ;
  - sonde : job devloop (nouvel identifiant à chaque essai, l'agent fraîchement
    démarré ignore le job resté dans la boîte) jusqu'à ce que le bureau soit là
    (WindowServer, Finder, Dock, SystemUIServer), `hw.ncpu` = SMP et
    `hostinfo` « Processors active: 0 1 » ; puis une seconde sonde --repos s plus tard ;
  - ok / gel (rien au bout de --delai s) / panique (panicstr ou vCPU dans panic(),
    lus au moniteur HMP) / cpu (nombre de processeurs faux) / bureau (agent
    vivant, bureau absent) / qemu-mort.
Incident : collecte de tools/endurance/endurance.py (registres, mémoire, écran,
kpanic.py…) dans <banc>/incidents/<n°>-<type>/, puis arrêt brutal (devloop stop) et
départ à froid au cycle suivant ; le disque est alors réparé par fsck au démarrage.
Moniteur HMP : ajouté par QEMU_EXTRA (-monitor unix:…). Résultats :
bench/endurance-dev/<nom>/cycles.jsonl et rapport (endurance.py rapport le lit).
Variables : DEVDISK, QEMU_BIN, SMP, PPC32MT, CPU_OPTS, TCG_OPTS (comme devloop.py),
ENDURANCE_INVITE (symboles, défaut bench/endurance/invite-10.4.11 du dépôt principal).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

ICI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ICI)
from hmp import hmp, mots                                 # noqa: E402
from symbolise import MAIN, WT, Symboliseur               # noqa: E402

DL = os.path.join(WT, "tools", "guest", "devloop.py")
STATE = os.path.join(WT, "bench", "devloop")


def journal(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def dl(*args, timeout=None, env=None):
    """devloop.py ARGS… → (code, sortie)."""
    try:
        p = subprocess.run([sys.executable, DL, *args], capture_output=True, text=True,
                           timeout=timeout, env=env)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return 124, ""


SONDE = r"""#!/bin/sh
echo "ncpu $(sysctl -n hw.ncpu)"
hostinfo | grep -i 'processors'
uptime
for p in WindowServer Finder Dock SystemUIServer loginwindow; do
  if ps axc | awk '{print $NF}' | grep -qx "$p"; then echo "proc $p oui"; else echo "proc $p non"; fi
done
ls /Library/Logs/panic.log 2>/dev/null && tail -5 /Library/Logs/panic.log
dmesg | grep -i -E 'panic|USBOHCI|LockTimeOut' | tail -5
"""

REDEMARRE = "#!/bin/sh\nsync; sync\n( sleep 3; sync; /sbin/shutdown -r now ) &\necho reboot-demande\n"


def job(texte):
    d = tempfile.mkdtemp(prefix="devend-")
    with open(os.path.join(d, "job.sh"), "w") as f:
        f.write(texte)
    return d


def sonde(timeout):
    d = job(SONDE)
    code, out = dl("run", d, "--timeout", str(timeout), timeout=timeout + 30)
    subprocess.run(["rm", "-rf", d])
    return code, out


def lit_sonde(out):
    r = {"ncpu": None, "actifs": None, "procs": {}}
    m = re.search(r"^ncpu (\d+)", out, re.M)
    r["ncpu"] = int(m.group(1)) if m else None
    m = re.search(r"Processors active:\s*(.*)$", out, re.M)
    r["actifs"] = m.group(1).split() if m else None
    for p, v in re.findall(r"^proc (\S+) (oui|non)", out, re.M):
        r["procs"][p] = v == "oui"
    m = re.search(r"load averages?:.*$", out, re.M)
    r["charge"] = m.group(0) if m else None
    # « up 1 min » : prouve que le redémarrage a bien eu lieu
    m = re.search(r"\bup\s+(\d[^,]*)", out)
    r["up"] = m.group(1).strip() if m else None
    r["panic_log"] = "panic.log" in out
    return r


class Banc:
    def __init__(self, a):
        self.a = a
        self.dir = os.path.join(a.banc, a.nom)
        os.makedirs(os.path.join(self.dir, "incidents"), exist_ok=True)
        self.jsonl = os.path.join(self.dir, "cycles.jsonl")
        self.n0 = 0
        if os.path.exists(self.jsonl):
            for l in open(self.jsonl):
                try:
                    self.n0 = max(self.n0, json.loads(l)["cycle"])
                except (ValueError, KeyError):
                    pass
        self.mon = os.path.join(a.rundir, "mon.sock")
        os.makedirs(a.rundir, exist_ok=True)
        self.sym = Symboliseur(a.invite)
        self.qemu = os.environ.get("QEMU_BIN", "?")
        self.env = dict(os.environ)
        self.env["QEMU_EXTRA"] = (os.environ.get("QEMU_EXTRA", "") +
                                  " -monitor unix:%s,server=on,wait=off" % self.mon).strip()
        self.env.setdefault("SND", "none")      # pas de son (Screamer sans sortie)
        self.env["SSH_PORT"] = "0"
        info = {"debut": time.strftime("%F %T"), "qemu": self.qemu,
                "smp": os.environ.get("SMP"), "ppc32mt": os.environ.get("PPC32MT"),
                "cpu_opts": os.environ.get("CPU_OPTS"), "tcg_opts": os.environ.get("TCG_OPTS"),
                "disque": os.environ.get("DEVDISK"), "args": vars(a),
                "commit": subprocess.run(["git", "-C", WT, "rev-parse", "--short", "HEAD"],
                                         capture_output=True, text=True).stdout.strip()}
        with open(os.path.join(self.dir, "lancements.jsonl"), "a") as f:
            f.write(json.dumps(info, ensure_ascii=False) + "\n")

    def pid(self):
        try:
            p = int(open(os.path.join(STATE, "qemu.pid")).read().strip())
            os.kill(p, 0)
            return p
        except (OSError, ValueError):
            return None

    def panique(self):
        try:
            a = self.sym.symbole_noyau("panicstr")
            w = mots(self.mon, a, 1) if a else []
            if w and 0x1000 <= w[0] < 0x10000000:
                return True
            lo, hi = self.sym.panic or (0, 0)
            regs = hmp(self.mon, "info registers -a", delai=10)
            for m in re.finditer(r"NIP\s+([0-9a-fA-F]+)", regs):
                if lo <= int(m.group(1), 16) < hi:
                    return True
        except OSError:
            pass
        return False

    def depart_froid(self):
        if self.pid():
            dl("stop", env=self.env)
        code, out = dl("start", "--gui", timeout=900, env=self.env)
        return code == 0 and self.pid() is not None, out

    def redemarre(self):
        d = job(REDEMARRE)
        dl("run", d, "--timeout", "60", timeout=120)
        subprocess.run(["rm", "-rf", d])
        time.sleep(20)          # laisse l'ancien système s'éteindre (l'agent meurt)

    def attend(self, t0):
        """(état, détail) : sondes de 45 s jusqu'au bureau complet ou --delai."""
        der = None
        while time.time() - t0 < self.a.delai:
            if not self.pid():
                return "qemu-mort", {}
            if self.panique():
                time.sleep(5)
                return "panique", {}
            code, out = sonde(45)
            if code == 0 and "ncpu" in out:
                r = lit_sonde(out)
                der = r
                bureau = all(r["procs"].get(p) for p in ("WindowServer", "Finder", "Dock",
                                                          "SystemUIServer"))
                if r["ncpu"] != self.a.smp or (r["actifs"] is not None and
                                               len(r["actifs"]) != self.a.smp):
                    return "cpu", {"sonde": r, "sortie": out[-1500:]}
                if bureau:
                    return "ok", {"sonde": r, "t_bureau": round(time.time() - t0)}
            time.sleep(5)
        if der is not None:
            return "bureau", {"sonde": der}
        return "gel", {}

    def collecte(self, n, etat, notes):
        import endurance
        d = os.path.join(self.dir, "incidents", "%04d-%s" % (n, etat))
        try:
            info = endurance.collecte(self.mon, d, self.sym, etat, notes)
        except Exception as e:      # la collecte ne doit jamais arrêter le banc
            info = {"erreur": repr(e)}
        return d, info

    def note(self, res):
        with open(self.jsonl, "a") as f:
            f.write(json.dumps(res, ensure_ascii=False) + "\n")

    def tourne(self):
        a = self.a
        vivante = False
        for k in range(1, a.n + 1):
            if os.path.exists(os.path.join(self.dir, "ARRET")):
                journal("ARRET demandé")
                break
            n = self.n0 + k
            froid = not vivante or (a.froid_tous and (k - 1) % a.froid_tous == 0)
            t0 = time.time()
            if froid:
                if vivante:
                    dl("shutdown", "--gui", timeout=300, env=self.env)
                ok, out = self.depart_froid()
                if not ok:
                    etat, det = ("qemu-mort" if not self.pid() else "gel"), {"start": out[-1500:]}
                else:
                    etat, det = self.attend(t0)
            else:
                self.redemarre()
                etat, det = self.attend(t0)
            if etat == "ok":
                time.sleep(a.repos)
                code, out = sonde(60)
                if code != 0 or "ncpu" not in out:
                    code, out = sonde(90)
                    if code != 0 or "ncpu" not in out:
                        etat, det = "gel", dict(det, apres_bureau=True)
            res = {"type": "demarrage", "cycle": n, "depart": "froid" if froid else "reboot",
                   "etat": etat, "duree": round(time.time() - t0), "date": time.strftime("%F %T"),
                   "qemu": self.qemu, **det}
            if etat != "ok":
                shot = os.path.join(self.dir, "incidents", "%04d-ecran.png" % n)
                dl("shot", shot, timeout=60, env=self.env)
                d, info = self.collecte(n, etat, {"cycle": n})
                res["incident"] = d
                journal("#%d %s : INCIDENT %s → %s" % (n, res["depart"], etat, d))
                dl("stop", env=self.env)
                vivante = False
            else:
                vivante = True
                journal("#%d %s : ok en %d s (bureau à %s s, up %s, %s)"
                        % (n, res["depart"], res["duree"], det.get("t_bureau"),
                           (det.get("sonde") or {}).get("up"), (det.get("sonde") or {}).get("charge")))
            self.note(res)
        if vivante:
            dl("shutdown", "--gui", timeout=300, env=self.env)
        self.rapport()

    def rapport(self):
        cyc = [json.loads(l) for l in open(self.jsonl)]
        par = {}
        for c in cyc:
            par[c["etat"]] = par.get(c["etat"], 0) + 1
        dep = {}
        for c in cyc:
            dep[c["depart"]] = dep.get(c["depart"], 0) + 1
        txt = ("# %s\n\n%d démarrages (%s) ; états : %s\nQEMU : %s\n"
               % (self.a.nom, len(cyc), ", ".join("%s %d" % kv for kv in sorted(dep.items())),
                  ", ".join("%s %d" % kv for kv in sorted(par.items())), self.qemu))
        ok = [c["duree"] for c in cyc if c["etat"] == "ok"]
        if ok:
            ok.sort()
            txt += "durée d'un démarrage réussi : médiane %d s, max %d s\n" % (ok[len(ok) // 2], ok[-1])
        open(os.path.join(self.dir, "rapport.md"), "w").write(txt)
        print(txt)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--nom", required=True)
    ap.add_argument("-n", type=int, default=30)
    ap.add_argument("--froid-tous", type=int, default=5,
                    help="un départ à froid tous les N cycles (0 : seulement le premier)")
    ap.add_argument("--delai", type=int, default=600, help="s sans bureau avant de déclarer un gel")
    ap.add_argument("--repos", type=int, default=20)
    ap.add_argument("--banc", default=os.path.join(WT, "bench", "endurance-dev"))
    ap.add_argument("--rundir", default=os.path.join(WT, ".e", "dev"))
    ap.add_argument("--invite", default=os.environ.get("ENDURANCE_INVITE") or
                    os.path.join(MAIN, "bench", "endurance", "invite-10.4.11"))
    a = ap.parse_args()
    a.smp = int(os.environ.get("SMP", "1"))
    if not os.environ.get("DEVDISK"):
        sys.exit("DEVDISK requis")
    Banc(a).tourne()


if __name__ == "__main__":
    main()
