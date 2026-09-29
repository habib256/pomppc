#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""endurance.py — banc d'endurance de session : enchaîne seul N démarrages de
Tiger (et, en option, N cycles lancement/arrêt d'un jeu), détecte panique et
gel, range un dossier par incident et donne le taux par type avec son
intervalle de confiance. Mode d'emploi : docs/endurance.md.

    python3 tools/endurance/endurance.py demarrages -n 100 [--instances 2]
        [--mode froid|reboot|reset] [--qemu BIN] [--nom CAMPAGNE]
    python3 tools/endurance/endurance.py jeu --jeu mb -n 20 [--duree 60]
    python3 tools/endurance/endurance.py rapport bench/endurance/CAMPAGNE
    python3 tools/endurance/endurance.py collecte SOCKET_MONITEUR DOSSIER

Jamais sur la VM quotidienne : chaque instance démarre sur un recouvrement
qcow2 NEUF (disks/tiger-endurance.qcow2 en lecture seule dessous, clone APFS
du disque quotidien), avec son répertoire d'exécution
(.run/endurance/<campagne>-<i> : verrou, moniteur, port ssh publié), son port
ssh (2240 + 10·i) et sans son (POMPPC_AUDIO_PROFILE=muet).
"""
import argparse
import datetime
import hashlib
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

ICI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ICI)
from hmp import hmp, hmp_suite, mots          # noqa: E402
from symbolise import (MAIN, WT, Symboliseur, adresses_du_texte, court,  # noqa: E402
                       lit_kextstat)

BASE = os.environ.get("ENDURANCE_BASE", os.path.join(MAIN, "disks", "tiger-endurance.qcow2"))
QEMU_IMG = os.path.expanduser("~/src/qemu/build/qemu-img")
RUN_TIGER = os.path.join(WT, "run_tiger.sh")
TSSH = os.path.join(WT, "tools", "guest", "tssh.sh")
BANC = os.path.join(MAIN, "bench", "endurance")
RUNDIR = os.path.join(MAIN, ".run", "endurance")
QUOTIDIEN = os.path.join(MAIN, "disks", "tiger.qcow2")

_verrou_journal = threading.Lock()


def journal(*a):
    with _verrou_journal:
        print(time.strftime("%H:%M:%S"), *a, flush=True)


def wilson(k, n, z=1.96):
    """Intervalle de Wilson à 95 % d'une proportion k/n."""
    if n == 0:
        return (0.0, 1.0)
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    m = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (max(0.0, c - m), min(1.0, c + m))


def binaire_qemu(args):
    """Chemin, date et empreinte du qemu-system-ppc64 réellement lancé : le
    binaire de référence peut être reconstruit pendant une campagne."""
    if getattr(args, "qemu", None):
        p = args.qemu
    else:
        p = os.path.expanduser("~/src/qemu/build/qemu-system-ppc64")
    try:
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()[:16]
        return "%s (%s, sha256 %s)" % (p, time.strftime("%Y-%m-%d %H:%M:%S",
                                                       time.localtime(os.path.getmtime(p))), h)
    except OSError:
        return p


# ------------------------------------------------------------------ une VM
class VM:
    """Une instance de Tiger du banc : son disque, son moniteur, son port ssh."""

    def __init__(self, campagne, slot, args):
        self.args = args
        self.slot = slot
        self.nom = "%s-%d" % (campagne, slot)
        self.scr = os.path.join(RUNDIR, self.nom)
        os.makedirs(self.scr, exist_ok=True)
        self.disque = os.path.join(self.scr, "disque.qcow2")
        self.port_voulu = 2240 + 10 * slot
        self.proc = None
        self.port = None
        self.log = None
        self.sym = SYM

    @property
    def mon(self):
        return os.path.join(self.scr, "mon.sock")

    # --- vie du processus QEMU
    def demarre(self, dossier_log):
        if os.path.realpath(self.disque) == os.path.realpath(QUOTIDIEN):
            raise SystemExit("refus : disque quotidien")
        if os.path.exists(self.disque):
            os.remove(self.disque)
        subprocess.run([QEMU_IMG, "create", "-q", "-f", "qcow2", "-b", BASE, "-F", "qcow2",
                        self.disque], check=True)
        for f in ("tiger.sshport", "tiger.mon"):
            try:
                os.remove(os.path.join(self.scr, f))
            except OSError:
                pass
        env = dict(os.environ, HEADLESS="1", TABLET="1", WEBPROXY="0", GLISO="0",
                   POMPPC_AUDIO_PROFILE="muet", SSH_FWD=str(self.port_voulu),
                   POMPPC_SCRATCH=self.scr, DISK=self.disque, SMP=str(self.args.smp))
        env.pop("DBUS_DISPLAY", None)
        env.pop("QMP_SOCK", None)
        if self.args.qemu:
            env["QEMU_BIN"] = self.args.qemu.replace("qemu-system-ppc64", "qemu-system-ppc")
        if self.args.extra:
            env["EXTRA_ARGS"] = self.args.extra
        self.log = os.path.join(dossier_log, "qemu.log")
        self.proc = subprocess.Popen([RUN_TIGER], cwd=WT, env=env, start_new_session=True,
                                     stdout=open(self.log, "w"), stderr=subprocess.STDOUT)
        t0 = time.time()
        while time.time() - t0 < 60:
            if self.proc.poll() is not None:
                return False
            try:
                with open(os.path.join(self.scr, "tiger.sshport")) as f:
                    self.port = int(f.read().strip() or 0)
                if os.path.exists(self.mon):
                    hmp(self.mon, "info status", delai=5)
                    return True
            except (OSError, ValueError):
                pass
            time.sleep(1)
        return False

    def vivant(self):
        return self.proc is not None and self.proc.poll() is None

    def quitte(self):
        if not self.vivant():
            return
        try:
            hmp(self.mon, "quit", delai=5)
        except OSError:
            pass
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait(10)

    # --- invité
    def ssh(self, cmd, delai=60, entree=None):
        env = dict(os.environ, TSSH_PORT=str(self.port or self.port_voulu))
        try:
            p = subprocess.run([TSSH, cmd], env=env, capture_output=True, text=True,
                               timeout=delai, input=entree)
        except subprocess.TimeoutExpired:
            return 124, ""
        return p.returncode, p.stdout

    def repond(self):
        return self.ssh("true", delai=20)[0] == 0

    def mot_noyau(self, nom):
        a = self.sym.symbole_noyau(nom)
        if a is None:
            return None
        try:
            w = mots(self.mon, a, 1)
            return w[0] if w else None
        except OSError:
            return None

    def panique_en_cours(self):
        """panicstr != 0 : panic() a été appelé (lu dans la mémoire de l'invité)."""
        v = self.mot_noyau("panicstr")
        return bool(v)


# ------------------------------------------------------------- collecte
def chaine(mon, adresse, maxi=4096):
    """Chaîne C à l'adresse physique `adresse` (le noyau est V=R en bas)."""
    if not adresse:
        return ""
    w = mots(mon, adresse, (maxi + 3) // 4)
    b = b"".join(x.to_bytes(4, "big") for x in w)
    return b.split(b"\0", 1)[0].decode("latin-1")


def liste_kmods(mon, sym):
    """Parcours de la liste `kmod` du noyau (kmod_info_t : next, info_version,
    id, name[64], version[64], reference_count, reference_list, address, size,
    hdr_size, start, stop). Les kexts chargés au démarrage sont V=R : leur
    kmod_info se lit en physique ; la tête peut être allouée ailleurs (ndrv),
    on la saute alors en reprenant au suivant lisible."""
    tete = sym.symbole_noyau("kmod")
    if tete is None:
        return []
    p = mots(mon, tete, 1)[0]
    res, vus = [], set()
    while p and p not in vus and len(res) < 200:
        vus.add(p)
        w = mots(mon, p, 42)
        if len(w) < 42:
            break
        b = b"".join(x.to_bytes(4, "big") for x in w)
        nom = b[12:76].split(b"\0", 1)[0]
        if not nom or not all(32 <= c < 127 for c in nom):
            break
        adr, taille = w[37], w[38]
        res.append((nom.decode(), adr, taille, w[2]))
        p = w[0]
    return res


def lit_registres(texte):
    """{cpu: {'NIP':…, 'LR':…, 'MSR':…, 'R1':…, 'SRR0':…, 'SRR1':…, 'DAR':…}}"""
    cpus, cur = {}, None
    for l in texte.splitlines():
        m = re.match(r"CPU#(\d+)", l.strip())
        if m:
            cur = cpus.setdefault(int(m.group(1)), {})
        if cur is None:
            continue
        for k in ("NIP", "LR", "MSR", "SRR0", "SRR1", "DAR", "DSISR", "CTR"):
            m = re.search(r"\b%s ([0-9a-f]{8,16})" % k, l)
            if m and k not in cur:
                cur[k] = int(m.group(1), 16)
        m = re.match(r"GPR00 [0-9a-f]+ ([0-9a-f]+)", l)
        if m:
            cur["R1"] = int(m.group(1), 16)
        m = re.match(r"GPR00 ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)", l)
        if m:
            cur["R3"] = int(m.group(4), 16)
    return cpus


def pile(mon, cpu, r1, n=12):
    """Remontée de la chaîne de cadres PowerPC depuis r1 ([r1] = cadre
    précédent, [r1+8] = LR sauvé), lue dans le contexte du vCPU (x) puis en
    physique (xp) si le contexte ne traduit pas."""
    res = []
    for physique in (False, True):
        res, sp = [], r1
        try:
            for _ in range(n):
                if not sp or sp & 3:
                    break
                w = mots(mon, sp, 3, physique=physique, cpu=cpu)
                if len(w) < 3:
                    break
                suivant, lr = w[0], w[2]
                if lr:
                    res.append(lr)
                if suivant <= sp:
                    break
                sp = suivant
        except OSError:
            res = []
        if len(res) >= 2:
            return res, ("x" if not physique else "xp")
    return res, "-"


def collecte(vm_mon, dossier, sym, motif, notes=None):
    """Tout ce qui sert à comprendre un incident, VM arrêtée (`stop`)."""
    os.makedirs(dossier, exist_ok=True)
    info = {"motif": motif, "date": datetime.datetime.now().isoformat(timespec="seconds")}
    if notes:
        info.update(notes)
    ecr = lambda n, t: open(os.path.join(dossier, n), "w").write(t)  # noqa: E731
    texte = ""
    try:
        hmp(vm_mon, "stop")
    except OSError as e:
        info["erreur"] = "moniteur injoignable : %s" % e
        ecr("incident.json", json.dumps(info, indent=1, ensure_ascii=False))
        return info
    try:
        regs1 = hmp(vm_mon, "info registers -a")
        ecr("registres.txt", regs1)
        ppm = os.path.join(dossier, "ecran.ppm")
        hmp(vm_mon, "screendump %s" % ppm)
        for _ in range(30):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
                break
            time.sleep(0.1)
        subprocess.run(["sips", "-s", "format", "png", ppm, "--out",
                        os.path.join(dossier, "ecran.png")], capture_output=True)
        # panique : panicstr, debug_buf (texte complet que l'invité écrirait en NVRAM)
        pstr = mots(vm_mon, sym.symbole_noyau("panicstr"), 1)[0]
        info["panicstr"] = "0x%x" % pstr
        texte = ""
        if pstr:
            info["panicstr_texte"] = chaine(vm_mon, pstr, 256)
        dbuf = mots(vm_mon, sym.symbole_noyau("debug_buf"), 1)[0]
        dptr = mots(vm_mon, sym.symbole_noyau("debug_buf_ptr"), 1)[0]
        if dbuf and dptr > dbuf:
            texte = chaine(vm_mon, dbuf, min(dptr - dbuf, 8192))
            ecr("panique.txt", texte)
        info["debug_buf"] = "0x%x..0x%x" % (dbuf, dptr)
        kmods = liste_kmods(vm_mon, sym)
        ecr("kmods.txt", "".join("%s 0x%x 0x%x %d\n" % (n, a, t, i) for n, a, t, i in kmods))
        km = [(n, a, t) for n, a, t, _ in kmods if a]
        info["kmods"] = len(km)
        # registres de tous les vCPU, PC/LR et pile symbolisés
        cpus = lit_registres(regs1)
        lignes = []
        for c, r in sorted(cpus.items()):
            ligne = {"cpu": c}
            for k in ("NIP", "LR", "SRR0", "CTR"):
                if k in r:
                    ligne[k] = "0x%x %s" % (r[k], sym.resout(r[k], km or None))
            for k in ("MSR", "SRR1", "DAR", "DSISR", "R1"):
                if k in r:
                    ligne[k] = "0x%x" % r[k]
            if "R1" in r:
                chaine_lr, mode = pile(vm_mon, c, r["R1"])
                ligne["pile(%s)" % mode] = ["0x%x %s" % (a, sym.resout(a, km or None))
                                            for a in chaine_lr]
            lignes.append(ligne)
        info["cpus"] = lignes
        # Vue des registres du contrôleur USB : côté device (xp sur la BAR de
        # l'OHCI) et côté invité (page de DAR traduite par la table de pages
        # du vCPU, gva2gpa + x) ; une différence dit une traduction périmée.
        vues = []
        pci = hmp(vm_mon, "info pci")
        ecr("pci.txt", pci)
        m = re.search(r"USB controller.*?BAR0: 32 bit memory at (0x[0-9a-f]+)", pci, re.S)
        if m:
            bar = int(m.group(1), 16)
            vues.append("OHCI BAR0 0x%x (xp) : %s" % (bar, " ".join(
                "%08x" % w for w in mots(vm_mon, bar, 24))))
        for c, r in sorted(cpus.items()):
            if r.get("DAR"):
                page = r["DAR"] & ~0xfff
                g = hmp_suite(vm_mon, ["cpu %d" % c, "gva2gpa 0x%x" % page])
                v = hmp_suite(vm_mon, ["cpu %d" % c, "x /24wx 0x%x" % page])
                vues.append("cpu%d DAR 0x%x : gva2gpa %s\n%s" % (c, r["DAR"], g.strip(), v))
        ecr("vues.txt", "\n".join(vues) + "\n")
        if texte:
            vus = []
            for a in adresses_du_texte(texte.split("Kernel version")[0]):
                if a not in vus and 0x1000 <= a < 0x20000000:
                    vus.append(a)
            info["panique_symbolisee"] = ["0x%x %s" % (a, sym.resout(a, km or None))
                                          for a in vus]
    except Exception as e:           # un incident à moitié collecté vaut mieux que rien
        info["erreur_collecte"] = "%s: %s" % (type(e).__name__, e)
    finally:
        try:
            hmp(vm_mon, "cont")
        except OSError:
            pass
    # le vCPU tourne-t-il encore ? (gel : boucle serrée, ou arrêt complet)
    try:
        time.sleep(3)
        regs2 = hmp(vm_mon, "info registers -a")
        ecr("registres-2.txt", regs2)
        c1, c2 = lit_registres(regs1), lit_registres(regs2)
        info["nip_immobile"] = {c: c1[c].get("NIP") == c2.get(c, {}).get("NIP") for c in c1}
        # boucle serrée : où tourne-t-elle ? 20 relevés de NIP/LR/SRR0/DAR,
        # puis 0,3 s de journal des exceptions et des fautes MMU de QEMU
        # (`log int,mmu` : une faute répétée à la même adresse s'y lit)
        ech = []
        for _ in range(20):
            r = lit_registres(hmp(vm_mon, "info registers -a"))
            ech.append({c: {k: "0x%x" % v for k, v in x.items()
                            if k in ("NIP", "LR", "SRR0", "SRR1", "DAR", "DSISR", "MSR")}
                        for c, x in r.items()})
            time.sleep(0.1)
        ecr("echantillons.json", json.dumps(ech, indent=1))
        compte = {}
        for e in ech:
            for c, x in e.items():
                k = "cpu%s %s" % (c, x.get("NIP"))
                compte[k] = compte.get(k, 0) + 1
        info["nip_echantillons"] = dict(sorted(compte.items(), key=lambda kv: -kv[1])[:12])
        jl = os.path.join(dossier, "qemu-int.log")
        hmp(vm_mon, "logfile %s" % jl)
        hmp(vm_mon, "log int,mmu")
        time.sleep(0.3)
        hmp(vm_mon, "log none")
        try:
            if os.path.getsize(jl) > 20_000_000:        # on garde le début
                with open(jl, "r+b") as f:
                    f.truncate(20_000_000)
        except OSError:
            pass
        p2 = os.path.join(dossier, "ecran-2.ppm")
        hmp(vm_mon, "screendump %s" % p2)
        time.sleep(1)
        h = [hashlib.sha1(open(os.path.join(dossier, f), "rb").read()).hexdigest()
             for f in ("ecran.ppm", "ecran-2.ppm") if os.path.exists(os.path.join(dossier, f))]
        info["ecran_fige"] = len(h) == 2 and h[0] == h[1]
    except Exception as e:
        info["erreur_collecte_2"] = "%s: %s" % (type(e).__name__, e)
    ecr("incident.json", json.dumps(info, indent=1, ensure_ascii=False))
    ecr("incident.md", rapport_incident(info, texte))
    for f in ("ecran.ppm", "ecran-2.ppm"):
        p = os.path.join(dossier, f)
        if os.path.exists(p) and f == "ecran-2.ppm":
            os.remove(p)
    return info


def rapport_incident(info, texte):
    l = ["# Incident : %s" % info["motif"], "",
         "- date : %s" % info.get("date"), "- cycle : %s" % info.get("cycle", "?"),
         "- instance : %s, QEMU : %s" % (info.get("instance", "?"), info.get("qemu", "?")),
         "- panicstr : %s %s" % (info.get("panicstr", "?"), info.get("panicstr_texte", "")),
         "- écran figé : %s ; NIP immobile : %s" % (info.get("ecran_fige"), info.get("nip_immobile")),
         "- kexts lus dans la mémoire (liste kmod) : %s" % info.get("kmods"),
         "- NIP sur 20 relevés : %s" % info.get("nip_echantillons"), ""]
    if info.get("erreur_collecte"):
        l.append("- ⚠ collecte incomplète : %s" % info["erreur_collecte"])
    l.append("## vCPU")
    for c in info.get("cpus", []):
        l.append("")
        l.append("### CPU %s" % c["cpu"])
        for k, v in c.items():
            if k == "cpu":
                continue
            if isinstance(v, list):
                l.append("- %s :" % k)
                l.extend("    - %s" % x for x in v)
            else:
                l.append("- %s : %s" % (k, v))
    if texte:
        l += ["", "## Texte de panique (debug_buf)", "", "```", texte.strip(), "```"]
    if info.get("panique_symbolisee"):
        l += ["", "## Adresses du texte, symbolisées", ""]
        l.extend("- %s" % x for x in info["panique_symbolisee"])
    if info.get("panic_log"):
        l += ["", "## panic.log (démarrage suivant)", "", "```", info["panic_log"].strip(), "```"]
    l += ["", "Fichiers : ecran.png, registres.txt (arrêt), registres-2.txt (3 s après `cont`), "
          "kmods.txt, panique.txt, qemu.log.", ""]
    return "\n".join(l)


# ------------------------------------------------------------- un démarrage
class Campagne:
    def __init__(self, args):
        self.args = args
        self.nom = args.nom or time.strftime("%Y%m%d-%H%M")
        self.dir = os.path.join(BANC, self.nom)
        os.makedirs(self.dir, exist_ok=True)
        self.verrou = threading.Lock()
        self.fait = 0
        self.arret = False
        self.jsonl = os.path.join(self.dir, "cycles.jsonl")
        with open(os.path.join(self.dir, "campagne.json"), "w") as f:
            json.dump({"args": vars(args), "debut": time.strftime("%Y-%m-%d %H:%M:%S"),
                       "base": BASE, "worktree": WT, "qemu_binaire": binaire_qemu(args),
                       "commit": subprocess.run(["git", "-C", WT, "rev-parse", "--short", "HEAD"],
                                                capture_output=True, text=True).stdout.strip()},
                      f, indent=1)

    def prochain(self):
        with self.verrou:
            if self.arret or self.fait >= self.args.n:
                return None
            self.fait += 1
            return self.fait

    def note(self, res):
        with self.verrou:
            with open(self.jsonl, "a") as f:
                f.write(json.dumps(res, ensure_ascii=False) + "\n")


def attend_demarrage(vm, t0, delai, repos):
    """('ok'|'panique'|'gel'|'qemu-mort'|'cpu', détails) d'un démarrage en cours."""
    vu_ssh = None
    while True:
        t = time.time() - t0
        if not vm.vivant():
            return "qemu-mort", {"t": round(t)}
        if vm.panique_en_cours():
            time.sleep(5)               # laisse finir le texte de la panique
            return "panique", {"t": round(t)}
        if vu_ssh is None:
            if vm.repond():
                vu_ssh = time.time()
                code, out = vm.ssh("sysctl -n hw.ncpu; uptime; kextstat; "
                                   "ls /Library/Logs/panic.log 2>/dev/null", delai=60)
                det = {"t_ssh": round(t), "sortie": out}
                n = out.split("\n", 1)[0].strip()
                if n.isdigit() and int(n) != vm.args.smp:
                    return "cpu", dict(det, t=round(t), ncpu=int(n))
            elif t > delai:
                return "gel", {"t": round(t)}
            else:
                time.sleep(5)
        else:
            if time.time() - vu_ssh >= repos:
                if not vm.repond():     # l'invité est-il resté vivant après le bureau ?
                    return "gel", {"t": round(t), "apres_ssh": True}
                return "ok", det
            time.sleep(5)


def recupere_panic_log(vm, info, dossier):
    """system_reset, puis /Library/Logs/panic.log au démarrage suivant."""
    try:
        hmp(vm.mon, "system_reset")
    except OSError:
        return
    t0 = time.time()
    while time.time() - t0 < 300 and vm.vivant():
        if vm.repond():
            code, out = vm.ssh("cat /Library/Logs/panic.log 2>/dev/null", delai=60)
            if out.strip():
                open(os.path.join(dossier, "panic.log"), "w").write(out)
                info["panic_log"] = out[-6000:]
            info["panic_log_redemarrage"] = "ok en %d s" % (time.time() - t0)
            return
        time.sleep(5)
    info["panic_log_redemarrage"] = "pas de ssh en 300 s après system_reset"


def instance(camp, slot):
    a = camp.args
    vm = VM(camp.nom, slot, a)
    vivante = False
    echecs = 0
    try:
        while True:
            i = camp.prochain()
            if i is None:
                break
            cyc = os.path.join(camp.dir, "cycles", "%04d" % i)
            os.makedirs(cyc, exist_ok=True)
            t0 = time.time()
            if not vivante or a.mode == "froid":
                vm.quitte()
                if not vm.demarre(cyc):
                    res = {"cycle": i, "instance": slot, "etat": "lancement",
                           "detail": open(vm.log).read()[-2000:]}
                    camp.note(res)
                    journal("[%d] #%d : QEMU ne démarre pas (%s)" % (slot, i, vm.log))
                    echecs += 1
                    if echecs >= 3:         # binaire ou réglage cassé : inutile d'insister
                        journal("[%d] trois lancements ratés de suite : instance arrêtée" % slot)
                        break
                    time.sleep(10)
                    continue
                echecs = 0
                vivante = True
                vm.binaire = binaire_qemu(a)
                t0 = time.time()
            elif a.mode == "reboot":
                vm.ssh("echo tiger974 | sudo -S shutdown -r now", delai=30)
                t1 = time.time()
                while time.time() - t1 < 120 and vm.repond():
                    time.sleep(2)
                t0 = time.time()
            else:   # reset
                hmp(vm.mon, "system_reset")
                t0 = time.time()
            etat, det = attend_demarrage(vm, t0, a.delai, a.repos)
            res = {"cycle": i, "instance": slot, "etat": etat, "mode": a.mode,
                   "binaire": getattr(vm, "binaire", ""),
                   "duree": round(time.time() - t0),
                   "t_ssh": det.get("t_ssh"), "port": vm.port}
            if etat == "ok":
                open(os.path.join(cyc, "sante.txt"), "w").write(det.get("sortie", ""))
                ks = lit_kextstat(det.get("sortie", ""))
                oh = [x for x in ks if x[0].endswith("AppleUSBOHCI")]
                if oh:
                    res["ohci"] = "0x%x" % oh[0][1]
                if "/Library/Logs/panic.log" in det.get("sortie", ""):
                    res["panic_log_present"] = True
                journal("[%d] #%d : ok (ssh à %s s)" % (slot, i, det.get("t_ssh")))
                if a.mode == "froid":
                    shutil.rmtree(cyc, ignore_errors=True) if not a.garde else None
            else:
                journal("[%d] #%d : INCIDENT %s (%s)" % (slot, i, etat, det))
                inc = os.path.join(camp.dir, "incidents", "%04d-%s" % (i, etat))
                notes = {"cycle": i, "instance": vm.nom, "qemu": a.qemu or "référence",
                         "t": det.get("t")}
                info = collecte(vm.mon, inc, SYM, etat, notes) if vm.vivant() else dict(notes, motif=etat)
                if vm.log and os.path.exists(vm.log):
                    shutil.copy(vm.log, os.path.join(inc, "qemu.log")) if os.path.isdir(inc) else None
                if etat in ("panique", "gel") and vm.vivant() and not a.sans_panic_log:
                    recupere_panic_log(vm, info, inc)
                    open(os.path.join(inc, "incident.json"), "w").write(
                        json.dumps(info, indent=1, ensure_ascii=False))
                    open(os.path.join(inc, "incident.md"), "w").write(
                        rapport_incident(info, open(os.path.join(inc, "panique.txt")).read()
                                         if os.path.exists(os.path.join(inc, "panique.txt")) else ""))
                res["incident"] = os.path.relpath(inc, camp.dir)
                res["resume"] = resume_incident(info)
                vm.quitte()
                vivante = False
            camp.note(res)
            if a.mode == "froid":
                vm.quitte()
                vivante = False
    finally:
        vm.quitte()


def resume_incident(info):
    """Une ligne : texte de panique ou PC symbolisés."""
    s = info.get("panicstr_texte", "")
    cpus = info.get("cpus", [])
    pcs = "; ".join("cpu%s %s" % (c["cpu"], c.get("NIP", "?").split(" ", 1)[-1]) for c in cpus)
    return (s.strip()[:120] + " | " if s else "") + pcs


# ------------------------------------------------------------- rapport
def rapport(dossier):
    cycles = []
    with open(os.path.join(dossier, "cycles.jsonl")) as f:
        for l in f:
            cycles.append(json.loads(l))
    n = len([c for c in cycles if c["etat"] != "lancement"])
    types = {}
    for c in cycles:
        if c["etat"] in ("lancement",):
            continue
        types.setdefault(c["etat"], []).append(c)
    camp = json.load(open(os.path.join(dossier, "campagne.json")))
    l = ["# Banc d'endurance — %s" % os.path.basename(dossier.rstrip("/")), "",
         "Paramètres : `%s`" % " ".join("%s=%s" % (k, v) for k, v in camp["args"].items()
                                          if v not in (None, False) and k != "func"),
         "Commit du banc : %s ; base : %s" % (camp.get("commit"), camp.get("base")), "",
         "| type | n | taux | IC 95 % (Wilson) |", "|---|---|---|---|"]
    for t in sorted(types, key=lambda x: (x != "ok", x)):
        k = len(types[t])
        lo, hi = wilson(k, n)
        l.append("| %s | %d / %d | %.1f %% | %.1f – %.1f %% |" % (t, k, n, 100 * k / n, 100 * lo, 100 * hi))
    if "panique" not in types:
        lo, hi = wilson(0, n)
        l.append("| panique | 0 / %d | 0 %% | 0 – %.1f %% |" % (n, 100 * hi))
    ts = [c["t_ssh"] for c in types.get("ok", []) if c.get("t_ssh")]
    if ts:
        ts.sort()
        l += ["", "Démarrage jusqu'au ssh : médiane %d s, min %d, max %d." % (ts[len(ts) // 2], ts[0], ts[-1])]
    inc = [c for c in cycles if c.get("incident")]
    if inc:
        l += ["", "## Incidents", ""]
        for c in inc:
            l.append("- #%d (%s) `%s` : %s" % (c["cycle"], c["etat"], c["incident"], c.get("resume", "")))
    lanc = [c for c in cycles if c["etat"] == "lancement"]
    if lanc:
        l += ["", "%d lancement(s) de QEMU en échec (non comptés)." % len(lanc)]
    txt = "\n".join(l) + "\n"
    open(os.path.join(dossier, "rapport.md"), "w").write(txt)
    return txt


# ------------------------------------------------------------- cycles de jeu
def cycles_jeu(args):
    """N cycles lancement/arrêt d'un jeu de la matrice sur une instance."""
    sys.path.insert(0, os.path.join(WT, "tools", "matrice"))
    import importlib
    j = importlib.import_module("jeux." + args.jeu).JEU
    camp = Campagne(args)
    vm = VM(camp.nom, args.slot, args)
    G = "/Users/tiger/matrice"

    class H:        # ce que les modules de jeux attendent de l'hôte
        def ssh(self, cmd, delai=120, entree=None, binaire=False):
            return vm.ssh(cmd, delai=delai, entree=entree)

        def sortie(self, cmd, delai=120):
            return vm.ssh(cmd, delai=delai)[1]

        def depose(self, texte, chemin):
            return vm.ssh("cat > '%s'" % chemin, entree=texte)[0] == 0

        def processus(self, nom):
            out = self.sortie("ps -axco pid,command")
            return [int(p[0]) for p in (l.strip().split(None, 1) for l in out.splitlines()[1:])
                    if len(p) == 2 and p[1] == nom]

        def premier_plan(self, nom):
            self.ssh("osascript -e 'tell application \"System Events\" to set frontmost of "
                     "process \"%s\" to true'" % nom, delai=30)
    h = H()
    boot = os.path.join(camp.dir, "demarrage")
    os.makedirs(boot, exist_ok=True)
    try:
        if not vm.demarre(boot):
            raise SystemExit("QEMU ne démarre pas (%s)" % vm.log)
        etat, det = attend_demarrage(vm, time.time(), args.delai, args.repos)
        if etat != "ok":
            collecte(vm.mon, os.path.join(camp.dir, "incidents", "0000-demarrage-" + etat), SYM, etat)
            raise SystemExit("démarrage : %s" % etat)
        lanceur = open(os.path.join(WT, "tools", "matrice", "guest", "lance.command")).read()
        h.ssh("mkdir -p %s" % G)
        h.depose(lanceur, G + "/lance.command")
        h.ssh("chmod +x %s/lance.command" % G)
        for i in range(1, args.n + 1):
            gd = "%s/endurance-%04d" % (G, i)
            h.ssh("rm -rf %s; killall ScreenSaverEngine 2>/dev/null; true" % gd)
            h.depose("D='%s'\njeu() {\n%s\n}\n" % (gd, j.commande(args.jeu_mode).strip("\n")),
                     G + "/cellule.sh")
            h.ssh("mkdir -p %s && open %s/lance.command" % (gd, G))
            t0 = time.time()
            etat, muet, vu = "ok", 0, False
            while time.time() - t0 < args.duree:
                time.sleep(10)
                if vm.panique_en_cours():
                    etat = "panique"
                    break
                code, out = vm.ssh("cat %s/log.txt 2>/dev/null; echo @@; tail -5 %s/stdout.txt 2>/dev/null"
                                   % (gd, gd), delai=40)
                if code in (124, 255):
                    muet += 1
                    if muet >= 4:
                        etat = "gel"
                        break
                    continue
                muet = 0
                if "\nexit " in "\n" + out.split("@@")[0]:
                    etat = "sortie-du-jeu"
                    break
                if not vu and h.processus(j.processus):
                    vu = True
                    h.premier_plan(j.nom_ui())
            if etat == "ok" and not vu:
                etat = "pas-lance"
            res = {"cycle": i, "instance": args.slot, "etat": etat, "jeu": args.jeu,
                   "duree": round(time.time() - t0)}
            if etat != "ok":
                inc = os.path.join(camp.dir, "incidents", "%04d-%s" % (i, etat))
                if etat in ("panique", "gel"):
                    info = collecte(vm.mon, inc, SYM, etat, {"cycle": i, "instance": vm.nom})
                    res["resume"] = resume_incident(info)
                else:
                    os.makedirs(inc, exist_ok=True)
                    _, out = vm.ssh("cat %s/log.txt %s/stdout.txt 2>/dev/null" % (gd, gd))
                    open(os.path.join(inc, "jeu.txt"), "w").write(out)
                    res["resume"] = out.strip().splitlines()[-1][:200] if out.strip() else ""
                res["incident"] = os.path.relpath(inc, camp.dir)
                journal("#%d : INCIDENT %s %s" % (i, etat, res.get("resume", "")))
            else:
                journal("#%d : ok" % i)
            camp.note(res)
            if etat in ("panique", "gel"):
                break
            j.arreter(h)
            h.ssh("i=0; while ! grep -q '^exit ' %s/log.txt 2>/dev/null && [ $i -lt 20 ]; "
                  "do sleep 1; i=$((i+1)); done; osascript -e 'tell application \"Terminal\" to quit' "
                  "2>/dev/null; rm -rf %s; true" % (gd, gd), delai=60)
            if j.redemarrer_apres and args.redemarre:
                h.ssh("echo tiger974 | sudo -S shutdown -r now", delai=30)
                time.sleep(20)
                etat, det = attend_demarrage(vm, time.time(), args.delai, args.repos)
                if etat != "ok":
                    collecte(vm.mon, os.path.join(camp.dir, "incidents", "%04d-redemarrage-%s" % (i, etat)),
                             SYM, etat)
                    break
            time.sleep(args.pause)
    finally:
        vm.quitte()
    print(rapport(camp.dir))


# ------------------------------------------------------------- main
SYM = None


def main():
    global SYM
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sp = ap.add_subparsers(dest="cmd", required=True)
    for nom in ("demarrages", "jeu"):
        p = sp.add_parser(nom)
        p.add_argument("-n", type=int, default=20)
        p.add_argument("--nom", help="nom de la campagne (dossier bench/endurance/NOM)")
        p.add_argument("--qemu", help="qemu-system-ppc64 à essayer (défaut : celui de config.env)")
        p.add_argument("--smp", type=int, default=2)
        p.add_argument("--extra", help="EXTRA_ARGS passés à run_tiger.sh")
        p.add_argument("--delai", type=int, default=360, help="s sans ssh avant de déclarer un gel")
        p.add_argument("--repos", type=int, default=20, help="s d'observation après le premier ssh")
        p.add_argument("--sans-panic-log", action="store_true",
                       help="ne pas redémarrer après un incident pour lire panic.log")
    p = sp.choices["demarrages"]
    p.add_argument("--instances", type=int, default=1)
    p.add_argument("--mode", choices=("froid", "reboot", "reset"), default="froid",
                   help="froid : un QEMU neuf par démarrage ; reboot : shutdown -r dans "
                        "l'invité ; reset : system_reset au moniteur")
    p.add_argument("--garde", action="store_true", help="garder les dossiers des cycles réussis")
    p = sp.choices["jeu"]
    p.add_argument("--jeu", required=True, help="module de tools/matrice/jeux (mb, d3, prey…)")
    p.add_argument("--jeu-mode", default="fen", choices=("fen", "pe"))
    p.add_argument("--duree", type=int, default=90, help="s de jeu par cycle avant l'arrêt")
    p.add_argument("--pause", type=int, default=10)
    p.add_argument("--slot", type=int, default=5)
    p.add_argument("--redemarre", action="store_true",
                   help="redémarrer l'invité après un jeu qui l'exige (DOOM 3) ; "
                        "par défaut NON : c'est le kCGLBadDisplay qu'on cherche")
    p = sp.add_parser("rapport")
    p.add_argument("dossier")
    p = sp.add_parser("collecte", help="collecte d'incident sur une VM du banc déjà lancée")
    p.add_argument("moniteur")
    p.add_argument("dossier")
    a = ap.parse_args()
    if a.cmd == "rapport":
        print(rapport(a.dossier))
        return
    SYM = Symboliseur()
    if a.cmd == "collecte":
        print(json.dumps(collecte(a.moniteur, a.dossier, SYM, "manuel"), indent=1, ensure_ascii=False))
        return
    if not os.path.exists(BASE):
        raise SystemExit("base %s absente (voir docs/endurance.md §Préparation)" % BASE)
    if a.cmd == "jeu":
        cycles_jeu(a)
        return
    camp = Campagne(a)
    journal("campagne %s : %d démarrages, %d instance(s), mode %s, QEMU %s"
            % (camp.nom, a.n, a.instances, a.mode, a.qemu or "référence"))
    fils = [threading.Thread(target=instance, args=(camp, s), daemon=True)
            for s in range(a.instances)]
    for f in fils:
        f.start()
        time.sleep(20)          # décale les démarrages
    try:
        while any(f.is_alive() for f in fils):
            time.sleep(2)
    except KeyboardInterrupt:
        camp.arret = True
        journal("arrêt demandé : les instances finissent leur cycle")
        for f in fils:
            f.join()
    print(rapport(camp.dir))


if __name__ == "__main__":
    main()
