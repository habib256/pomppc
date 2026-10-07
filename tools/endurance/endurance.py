#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
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
qcow2 NEUF (disks/tiger-endurance.qcow2 en lecture seule dessous, copie du
disque quotidien ARRÊTÉ), avec son répertoire d'exécution
(.run/endurance/<campagne>-<i> : verrou, moniteur, port ssh publié), son port
ssh (2240 + 10·i) et sans son (POMPPC_AUDIO_PROFILE=muet).

Hôtes : macOS (M4, banc d'origine) et Linux x86-64 (PC, porté le 06/10/2026 :
symboles par tools/re/machonm.py, PNG par Pillow ou ImageMagick, binaire
réellement lancé lu dans la bannière de run_tiger.sh, texte de panique par
tools/re/kpanic.py, charge de l'hôte relevée).
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
import serie                                  # noqa: E402
from symbolise import (MAIN, WT, Symboliseur, adresses_du_texte, court,  # noqa: E402
                       lit_kextstat)

BASE = os.environ.get("ENDURANCE_BASE", os.path.join(MAIN, "disks", "tiger-endurance.qcow2"))
# Base à console série (docs/endurance.md §8) : getty sur tty.pci-serial18, l'UART
# 16550 PCI que --serie ajoute en 0x12 (IRQ 29, seule ; en 0x11 elle partage l'IRQ 28
# de l'OHCI et ne reçoit rien).
BASE_SERIE = os.path.join(MAIN, "disks", "tiger-endurance-serie.qcow2")
SERIE_DEV = "-chardev socket,id=serie0,path=%s,server=on,wait=off " \
            "-device pci-serial,chardev=serie0,addr=0x12"
QEMU_IMG = os.path.expanduser("~/src/qemu/build/qemu-img")
RUN_TIGER = os.path.join(WT, "run_tiger.sh")
TSSH = os.path.join(WT, "tools", "guest", "tssh.sh")
BANC = os.path.join(MAIN, "bench", "endurance")
RUNDIR = os.path.join(MAIN, ".run", "endurance")
QUOTIDIEN = os.path.join(MAIN, "disks", "tiger.qcow2")
KPANIC = os.path.join(WT, "tools", "re", "kpanic.py")
# Le bras témoin « anciens défauts » (avant le 06/10/2026 sur le PC) : binaire de
# référence et les leviers TCG allumés ce jour-là éteints (run_tiger.sh).
ANCIENS_DEFAUTS = {"QEMU_FAST": "0", "TLBPRECISE": "0", "LMWINLINE": "0", "DCBZINLINE": "0",
                   "JITREL32": "0", "FPNATIVECMP": "0", "VFPNATIVECMP": "0", "LMWVEC": "0",
                   "JCWORD": "0"}
# Durée de jeu par cycle (s) et silence de frames.csv toléré (s), par jeu ; DOOM 3
# charge longtemps (cinématique puis niveau), UT2004 aussi.
DUREE_JEU = {"mb": 150, "zen": 120, "ut": 300, "d3": 600}
FIGE_JEU = {"mb": 120, "zen": 120, "ut": 180, "d3": 300}

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


def en_png(ppm, png):
    """PPM → PNG : sips sous macOS, Pillow ou ImageMagick ailleurs."""
    if sys.platform == "darwin":
        subprocess.run(["sips", "-s", "format", "png", ppm, "--out", png], capture_output=True)
        return os.path.exists(png)
    try:
        from PIL import Image
        Image.open(ppm).save(png)
        return True
    except Exception:           # Pillow absent ou PPM illisible : ImageMagick
        subprocess.run(["convert", ppm, png], capture_output=True)
        return os.path.exists(png)


def autres_qemu():
    """QEMU mac99 en marche sur l'hôte (les nôtres compris)."""
    ps = subprocess.run(["ps", "-Ao", "comm=,args="], capture_output=True, text=True).stdout
    n = 0
    for l in ps.splitlines():
        p = l.split(None, 1)
        if len(p) == 2 and "mac99" in p[1] and os.path.basename(p[0]).startswith("qemu"):
            n += 1
    return n


def heure_limite(hhmm):
    """Prochaine occurrence de HH:MM (heure locale), en secondes epoch."""
    h, m = (int(x) for x in hhmm.split(":"))
    now = datetime.datetime.now()
    t = now.replace(hour=h, minute=m, second=0, microsecond=0)
    if t <= now:
        t += datetime.timedelta(days=1)
    return t.timestamp()


def binaire_qemu(args, chemin=None):
    """Chemin, date et empreinte du qemu-system-ppc64 réellement lancé : le
    binaire de référence peut être reconstruit pendant une campagne. `chemin`
    est celui que run_tiger.sh annonce (« binaire : … ») : sur le PC, le
    lanceur prend le binaire RAPIDE build-fast/ et non build/ (QEMU_FAST)."""
    if chemin:
        p = chemin
    elif getattr(args, "qemu", None):
        p = args.qemu
    else:
        p = os.path.expanduser("~/src/qemu/build/qemu-system-ppc64")
    try:
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()[:16]
        return "%s (%s, sha256 %s)" % (p, time.strftime("%Y-%m-%d %H:%M:%S",
                                                       time.localtime(os.path.getmtime(p))), h)
    except OSError:
        return p


def base_de(args):
    """Base qcow2 de la campagne : ENDURANCE_BASE, sinon la base à console série
    avec --serie, sinon disks/tiger-endurance.qcow2."""
    if "ENDURANCE_BASE" in os.environ or not getattr(args, "serie", False):
        return BASE
    return BASE_SERIE


def env_campagne(args):
    """Variables passées à run_tiger.sh pour toute la campagne : --env K=V et,
    avec --anciens-defauts, le bras témoin d'avant le 06/10."""
    env = {}
    if getattr(args, "anciens_defauts", False):
        env.update(ANCIENS_DEFAUTS)
    for kv in getattr(args, "env", None) or []:
        k, _, v = kv.partition("=")
        env[k] = v
    return env


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
        self.binaire = self.chemin_bin = None
        self.etiquette = self.leviers = self.gpu = "?"
        self.serie = os.path.join(self.scr, "serie.sock") if getattr(args, "serie", False) else None

    @property
    def mon(self):
        return os.path.join(self.scr, "mon.sock")

    # --- vie du processus QEMU
    def demarre(self, dossier_log):
        if os.path.realpath(self.disque) == os.path.realpath(QUOTIDIEN):
            raise SystemExit("refus : disque quotidien")
        if os.path.exists(self.disque):
            os.remove(self.disque)
        subprocess.run([QEMU_IMG, "create", "-q", "-f", "qcow2", "-b", base_de(self.args), "-F", "qcow2",
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
        env.pop("POMPPC_FRONTEND", None)
        if self.args.qemu:
            env["QEMU_BIN"] = self.args.qemu.replace("qemu-system-ppc64", "qemu-system-ppc")
        extra = [self.args.extra] if self.args.extra else []
        if self.serie:
            # second canal (§8) : UART PCI vers un socket, getty dans la base
            try:
                os.remove(self.serie)
            except OSError:
                pass
            extra.append(SERIE_DEV % self.serie)
        if extra:
            env["EXTRA_ARGS"] = " ".join(extra)
        env.update(env_campagne(self.args))
        if getattr(self.args, "fenetre", False):
            # repli : fenêtre QEMU native au lieu de -display none (le backend GL
            # de qgpu a son propre contexte EGL, mais si un pilote le refusait
            # sans fenêtre, c'est le geste le moins intrusif)
            env.pop("HEADLESS", None)
            env["POMPPC_DISPLAY"] = "gtk" if sys.platform != "darwin" else "cocoa"
        self.log = os.path.join(dossier_log, "qemu.log")
        self.proc = subprocess.Popen([RUN_TIGER], cwd=WT, env=env, start_new_session=True,
                                     stdout=open(self.log, "w"), stderr=subprocess.STDOUT)
        t0 = time.time()
        # 90 s et non 60 : les sondages de capacités de run_tiger.sh (une
        # vingtaine de QEMU -S) s'allongent sur un hôte chargé
        while time.time() - t0 < 90:
            if self.proc.poll() is not None:
                return False
            try:
                with open(os.path.join(self.scr, "tiger.sshport")) as f:
                    self.port = int(f.read().strip() or 0)
                if os.path.exists(self.mon):
                    hmp(self.mon, "info status", delai=5)
                    self.lit_banniere()
                    return True
            except (OSError, ValueError):
                pass
            time.sleep(1)
        return False

    def lit_banniere(self):
        """Ce que run_tiger.sh a réellement lancé : binaire (le RAPIDE sur le
        PC par défaut), leviers allumés (ligne « ▶ Tiger … »), backend qgpu."""
        try:
            txt = open(self.log, errors="replace").read()
        except OSError:
            return
        m = re.search(r"^  binaire : (.+)$", txt, re.M)
        self.chemin_bin = m.group(1).strip() if m else None
        m = re.search(r"^▶ Tiger \(QEMU [^,]*, (.*?)\) : (.*)$", txt, re.M)
        self.etiquette = m.group(1) if m else "?"
        self.leviers = m.group(2) if m else "?"
        m = re.search(r"qgpu \(backend ([A-Za-z0-9_]+),", txt)
        self.gpu = m.group(1) if m else "?"
        self.binaire = "%s ; %s" % (self.etiquette, binaire_qemu(self.args, self.chemin_bin))

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
        # TSSH_MUX=0 : pas de connexion maîtresse partagée (tssh.sh la range dans
        # .run/ du dépôt principal) ; une maîtresse restée sur un invité qui
        # redémarre ou qui a gelé fait attendre chaque ssh jusqu'à ServerAlive
        env = dict(os.environ, TSSH_PORT=str(self.port or self.port_voulu), TSSH_MUX="0")
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
        """panic() a été appelé : panicstr != 0 (lu dans la mémoire de
        l'invité), OU un vCPU tourne dans panic(). panic() remet panicstr à 0
        au retour de Debugger(), AVANT sa boucle finale (`b .`, tools/re/kpanic.py) :
        le seul panicstr laissait passer une panique pour un gel."""
        if self.mot_noyau("panicstr"):
            return True
        if self.sym.panic:
            lo, hi = self.sym.panic
            try:
                cpus = lit_registres(hmp(self.mon, "info registers -a", delai=10))
            except OSError:
                return False
            return any(lo <= r.get("NIP", 0) < hi for r in cpus.values())
        return False


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
        nom = None
        # en physique d'abord (V=R) ; sinon par la MMU du vCPU 0 : sur le 10.4.11
        # la tête de liste (ndrv QEMU,VGA, chargé en dernier) est hors V=R et la
        # lecture physique s'arrêtait au premier maillon (07/10/2026)
        for physique, cpu in ((True, None), (False, 0), (False, 1)):
            try:
                w = mots(mon, p, 42, physique=physique, cpu=cpu)
            except OSError:
                continue
            if len(w) < 42:
                continue
            b = b"".join(x.to_bytes(4, "big") for x in w)
            n = b[12:76].split(b"\0", 1)[0]
            if n and all(32 <= c < 127 for c in n):
                nom = n
                break
        if nom is None:
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
        en_png(ppm, os.path.join(dossier, "ecran.png"))
        # tools/re/kpanic.py, VM arrêtée : le texte de la panique reconstitué
        # (format + arguments lus sur la pile de panic()), l'appelant, la pile
        # et la fin du msgbuf. Sans boot-arg de débogage, Tiger n'alloue pas
        # debug_buf : c'est souvent la SEULE source du texte.
        kp = os.path.join(dossier, "kpanic.txt")
        with open(kp, "w") as f:
            try:
                subprocess.run([sys.executable, KPANIC, os.path.join(sym.invite, "mach_kernel"),
                                vm_mon], stdout=f, stderr=subprocess.STDOUT, timeout=600)
            except subprocess.TimeoutExpired:
                f.write("kpanic.py : délai dépassé\n")
        kt = open(kp, errors="replace").read()
        info["kpanic_sortie"] = kt[:12000]
        for cle, motif in (("kpanic_format", r"format(?: \(r\d+ = [0-9a-f]+\))? « (.*?) »"),
                           ("kpanic_texte", r"^  texte : (.*)$"),
                           ("kpanic_appelant", r"^panic_caller = (.*)$")):
            m = re.search(motif, kt, re.M)
            if m:
                info[cle] = m.group(1).strip()
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
        if os.path.exists(p) and (f == "ecran-2.ppm" or os.path.exists(os.path.join(dossier, "ecran.png"))):
            os.remove(p)
    return info


def rapport_incident(info, texte):
    l = ["# Incident : %s" % info["motif"], "",
         "- date : %s" % info.get("date"), "- cycle : %s" % info.get("cycle", "?"),
         "- instance : %s, QEMU : %s" % (info.get("instance", "?"), info.get("qemu", "?")),
         "- panicstr : %s %s" % (info.get("panicstr", "?"), info.get("panicstr_texte", "")),
         "- kpanic.py : texte « %s » ; format « %s » ; appelant %s"
         % (info.get("kpanic_texte", "-"), info.get("kpanic_format", "-"),
            info.get("kpanic_appelant", "-")),
         "- charge de l'hôte : %s" % info.get("charge", "?"),
         "- sonde ssh (VM en marche) : %s ; ssh de 90 s : %s"
         % (info.get("sonde_ssh", "-"), info.get("ssh_90s", "-")),
         "- console série (VM en marche, avant tout reset) : %s" % info.get("serie", "-"),
         "- ssh après `launchctl load` de ssh.plist par la console : %s"
         % info.get("ssh_apres_rechargement", "-"),
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
    if info.get("kpanic_sortie"):
        l += ["", "## tools/re/kpanic.py (VM arrêtée)", "", "```", info["kpanic_sortie"].strip(), "```"]
    if texte:
        l += ["", "## Texte de panique (debug_buf)", "", "```", texte.strip(), "```"]
    if info.get("panique_symbolisee"):
        l += ["", "## Adresses du texte, symbolisées", ""]
        l.extend("- %s" % x for x in info["panique_symbolisee"])
    if info.get("panic_log"):
        l += ["", "## panic.log (démarrage suivant)", "", "```", info["panic_log"].strip(), "```"]
    l += ["", "Fichiers : ecran.png, kpanic.txt (texte, appelant, pile, msgbuf), registres.txt "
          "(arrêt), registres-2.txt (3 s après `cont`), kmods.txt, panique.txt, qemu.log ; "
          "avec --serie : serie.txt (relevé par la console avant tout reset), "
          "apres-reset.txt (même relevé par ssh après `system_reset`).", ""]
    return "\n".join(l)


# ------------------------------------------------------------- une campagne
def type_cycle(c):
    """Type d'une ligne de cycles.jsonl (les campagnes du M4 n'en avaient pas)."""
    return c.get("type") or ("jeu" if "jeu" in c else "demarrage")


class Campagne:
    """Un dossier bench/endurance/<nom>. Relancer avec le même --nom AJOUTE des
    cycles (tranches) : la numérotation reprend, et le banc refuse de mêler
    deux bras (variables de run_tiger.sh, QEMU imposé) dans un même dossier."""

    def __init__(self, args, type_principal):
        self.args = args
        self.type = type_principal
        self.nom = args.nom or time.strftime("%Y%m%d-%H%M")
        self.dir = os.path.join(BANC, self.nom)
        os.makedirs(self.dir, exist_ok=True)
        self.verrou = threading.Lock()
        self.fait = 0
        self.arret = False
        self.noyau_vu = None
        self.limite = heure_limite(args.jusqua) if getattr(args, "jusqua", None) else None
        self.jsonl = os.path.join(self.dir, "cycles.jsonl")
        self.base = {}
        if os.path.exists(self.jsonl):
            for l in open(self.jsonl):
                try:
                    c = json.loads(l)
                except ValueError:
                    continue
                t = type_cycle(c)
                self.base[t] = max(self.base.get(t, 0), int(c.get("cycle", 0)))
        self.compteurs = {}
        bras = {"env": env_campagne(args), "qemu": args.qemu, "smp": args.smp, "extra": args.extra}
        if getattr(args, "serie", False):
            bras["serie"] = True        # un périphérique de plus : un autre bras
        info = {"args": {k: v for k, v in vars(args).items() if k != "func"},
                "debut": time.strftime("%Y-%m-%d %H:%M:%S"), "bras": bras,
                "base": base_de(args), "worktree": WT, "invite": SYM.invite if SYM else None,
                "noyau": SYM.version if SYM else None, "hote": " ".join(os.uname()),
                "qemu_binaire": binaire_qemu(args),
                "commit": subprocess.run(["git", "-C", WT, "rev-parse", "--short", "HEAD"],
                                         capture_output=True, text=True).stdout.strip()}
        cj = os.path.join(self.dir, "campagne.json")
        if os.path.exists(cj):
            ancien = json.load(open(cj))
            if ancien.get("bras", bras) != bras:
                raise SystemExit("campagne %s : autre bras (%s) que celui demandé (%s) : "
                                 "prendre un autre --nom" % (self.nom, ancien.get("bras"), bras))
        else:
            json.dump(info, open(cj, "w"), indent=1, ensure_ascii=False)
        with open(os.path.join(self.dir, "lancements.jsonl"), "a") as f:
            f.write(json.dumps(info, ensure_ascii=False) + "\n")
        # charge de l'hôte, relevée toutes les 10 s (une autre session peut
        # charger l'hôte par moments : un LockTimeOut se lit avec elle)
        self.charges = []
        self.csv_charge = os.path.join(self.dir, "charge.csv")
        threading.Thread(target=self._releve_charge, daemon=True).start()

    def _releve_charge(self):
        while True:
            try:
                l1, l5, _ = os.getloadavg()
                q = autres_qemu()
            except OSError:
                l1 = l5 = q = -1
            t = time.time()
            with self.verrou:
                self.charges.append((t, l1, q))
                del self.charges[:-5000]
            with open(self.csv_charge, "a") as f:
                f.write("%s,%.2f,%.2f,%d\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), l1, l5, q))
            time.sleep(10)

    def charge_entre(self, t0, t1=None):
        """Charge sur 1 min (début, max, fin) et QEMU mac99 en marche (max) entre t0 et t1."""
        t1 = t1 or time.time()
        with self.verrou:
            r = [c for c in self.charges if t0 - 10 <= c[0] <= t1 + 10]
        if not r:
            l1 = os.getloadavg()[0]
            return {"l1_debut": round(l1, 1), "l1_max": round(l1, 1), "l1_fin": round(l1, 1),
                    "qemu_max": None}
        return {"l1_debut": round(r[0][1], 1), "l1_max": round(max(c[1] for c in r), 1),
                "l1_fin": round(r[-1][1], 1), "qemu_max": max(c[2] for c in r)}

    def fini(self):
        if self.arret:
            return True
        if self.limite and time.time() >= self.limite:
            return True
        if os.path.exists(os.path.join(self.dir, "ARRET")):     # touch ARRET : arrêt propre
            return True
        return False

    def prochain(self):
        """Numéro du prochain cycle principal (démarrage ou jeu), None si fini."""
        with self.verrou:
            if self.fini() or self.fait >= self.args.n:
                return None
            self.fait += 1
            return self.base.get(self.type, 0) + self.fait

    def numero(self, t):
        """Numéro d'un cycle secondaire (les démarrages d'une campagne de jeu)."""
        with self.verrou:
            self.compteurs[t] = self.compteurs.get(t, 0) + 1
            return self.base.get(t, 0) + self.compteurs[t]

    def note(self, res):
        res.setdefault("date", time.strftime("%Y-%m-%d %H:%M:%S"))
        with self.verrou:
            with open(self.jsonl, "a") as f:
                f.write(json.dumps(res, ensure_ascii=False) + "\n")

    def verifie_noyau(self, sortie):
        """Le noyau de l'invité est-il celui dont on a les symboles ? Sinon
        panicstr se lirait à une fausse adresse : la campagne s'arrête."""
        m = re.search(r"Darwin Kernel Version [0-9.]+", sortie)
        if not m:
            return True
        v = m.group(0)
        with self.verrou:
            if self.noyau_vu:
                return self.noyau_vu == v
            self.noyau_vu = v
        if v + ":" not in (SYM.version or ""):
            journal("⚠  noyau de l'invité « %s », symboles de « %s » (%s) : campagne arrêtée"
                    % (v, SYM.version, SYM.invite))
            self.arret = True
            return False
        return True


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
                code, out = vm.ssh("sysctl -n hw.ncpu; uname -v; uptime; pmset -g | grep -i sleep; "
                                   "kextstat; ls /Library/Logs/panic.log 2>/dev/null; "
                                   # §8 : l'échec de l'IPv6 de lo0 au démarrage, que les
                                   # « pas de ssh » ont tous ; ici, sur les démarrages sains
                                   "echo tiger974 | sudo -S -p '' dmesg | grep in6_ifattach",
                                   delai=60)
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
                # l'invité est-il resté vivant après le bureau ? Un seul ssh
                # raté ne suffit pas (29/09 : bureau intact, vCPU au repos, un
                # ssh de 20 s perdu sur un hôte chargé) : trois essais sur ~1 min
                for essai in range(3):
                    if vm.repond():
                        break
                    time.sleep(10)
                else:
                    return "gel", {"t": round(t), "apres_ssh": True}
                return "ok", det
            time.sleep(5)


def releve_ssh(vm):
    """serie.RELEVE exécuté par ssh, même format que serie.txt."""
    morceaux = []
    for nom, cmd in serie.RELEVE:
        code, out = vm.ssh(cmd, delai=90)
        morceaux.append("===== %s (code %s) : %s\n%s\n" % (nom, code, cmd, out.rstrip()))
    return "".join(morceaux)


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
            # le démarrage gelé a écrit dans system.log jusqu'au reset (sshd,
            # DHCP, launchd) : sa fin, lue au démarrage suivant
            code, out = vm.ssh("tail -400 /var/log/system.log 2>/dev/null", delai=60)
            if out.strip():
                open(os.path.join(dossier, "system.log"), "w").write(out)
            if vm.serie:
                # le même relevé que par la console, sur le démarrage d'après (sain)
                txt = releve_ssh(vm)
                open(os.path.join(dossier, "apres-reset.txt"), "w").write(txt)
                info["apres_reset"] = resume_releve(txt)
            return
        time.sleep(5)
    info["panic_log_redemarrage"] = "pas de ssh en 300 s après system_reset"


def resume_releve(texte):
    """Ce qui compte dans un relevé de l'invité (serie.RELEVE) pour un « pas de
    ssh » : port 22 à l'écoute, sshd dans launchd, adresses, erreurs IPv6 du msgbuf."""
    def section(nom):
        m = re.search(r"^===== %s \(code [^)]*\) : .*?\n(.*?)(?=^===== |\Z)" % re.escape(nom),
                      texte, re.M | re.S)
        return m.group(1) if m else ""
    net, lc, ifc, dm = (section("netstat-an"), section("launchctl"), section("ifconfig"),
                        section("dmesg"))
    r = {}
    if not texte.strip() or texte.startswith("!!") or "socket série" in texte:
        return {"releve": texte.strip()[:200]}
    r["ecoute_22"] = sorted(set(re.findall(r"^(tcp[46]) .*\*\.22 +\*\.\* +LISTEN", net, re.M)))
    r["sshd_dans_launchd"] = "com.openssh.sshd" in lc
    r["lo0_ipv4"] = bool(re.search(r"^lo0:.*?\n(?:\t.*\n)*?\tinet 127\.0\.0\.1", ifc, re.M))
    r["lo0_ipv6"] = bool(re.search(r"^lo0:.*?\n(?:\t.*\n)*?\tinet6 ::1 ", ifc, re.M))
    r["en0_ipv4"] = (re.findall(r"^en0:.*?\n(?:\t.*\n)*?\tinet (\S+)", ifc, re.M) or [None])[0]
    r["msgbuf_in6"] = [l.strip() for l in dm.splitlines() if "in6_" in l or "errno=" in l][:5]
    m = re.search(r"^-- 127\.0\.0\.1:22\n(.*?)^nc=", section("port22-local"), re.M | re.S)
    r["port22_local"] = (m.group(1).strip().splitlines() or ["rien"])[0] if m else "?"
    return r


def releve_serie(sock, dossier):
    """Relevé par la console série (VM en marche) ; notes pour incident.json."""
    t0 = time.time()
    try:
        statut, texte = serie.releve(sock, dossier)
    except Exception as e:          # le second canal ne doit jamais casser la collecte
        return {"serie": "échec du relevé : %r" % e}
    n = {"serie": (statut or "session ouverte") + " (%.0f s)" % (time.time() - t0)}
    if not statut:
        n["serie_resume"] = resume_releve(texte)
        n["serie"] += " ; " + json.dumps(n["serie_resume"], ensure_ascii=False)
    return n


def recharge_sshd(vm, notes):
    """Par la console : launchctl unload/load de ssh.plist, puis la sonde. Dit si
    le défaut est dans le service (launchd) ou en dessous (pile IP, NAT)."""
    try:
        c = serie.Console(vm.serie)
        if c.connexion(60):
            notes["ssh_apres_rechargement"] = "pas de session sur la console"
            return
        code, out = c.commande("sudo -k; " + serie.SUDO + "launchctl unload "
                               "/System/Library/LaunchDaemons/ssh.plist; " + serie.SUDO +
                               "launchctl load /System/Library/LaunchDaemons/ssh.plist; "
                               "netstat -an | grep '\\.22 '", 60)
        c.deconnexion()
        c.fermer()
    except OSError as e:
        notes["ssh_apres_rechargement"] = "console : %s" % e
        return
    notes["ssh_apres_rechargement"] = "%s ; netstat : %s" % (
        sonde_ssh(vm.port or vm.port_voulu), " | ".join(out.split()) or "rien")


def sonde_ssh(port, delai=15):
    """Connexion TCP brute au port redirigé : la bannière de sshd, ou ce qui manque."""
    import socket
    s = socket.socket()
    s.settimeout(delai)
    try:
        s.connect(("127.0.0.1", port))
        b = s.recv(200)
        return "bannière « %s »" % b.decode("latin-1").strip() if b else "connecté, fermé sans bannière"
    except socket.timeout:
        return "connecté (NAT), aucune bannière en %d s" % delai
    except OSError as e:
        return "connexion : %s" % e
    finally:
        s.close()


def incident(camp, vm, nom, etat, notes, t0, panic_log=True):
    """Collecte d'un incident AVANT tout reset ; (chemin relatif, résumé)."""
    inc = os.path.join(camp.dir, "incidents", "%s-%s" % (nom, etat))
    notes = dict(notes, instance=vm.nom, qemu=vm.binaire or camp.args.qemu or "référence",
                 leviers=vm.leviers, charge=camp.charge_entre(t0))
    if vm.vivant():
        # VM en marche, avant la collecte : sshd répond-il seulement ? (07/10 :
        # deux « gels » au démarrage avec le bureau intact et les deux vCPU au
        # repos ; la bannière dit si sshd écoute, `info usernet` l'état du NAT)
        os.makedirs(inc, exist_ok=True)
        notes["sonde_ssh"] = sonde_ssh(vm.port or vm.port_voulu)
        code, _ = vm.ssh("true", delai=90)
        notes["ssh_90s"] = "code %d" % code
        try:
            open(os.path.join(inc, "usernet.txt"), "w").write(hmp(vm.mon, "info usernet"))
        except OSError:
            pass
        if vm.serie and etat != "panique":
            # second canal, VM en marche, AVANT la collecte et tout reset (§8)
            notes.update(releve_serie(vm.serie, inc))
        info = collecte(vm.mon, inc, SYM, etat, notes)
        if vm.serie and etat == "gel" and not notes.get("sonde_ssh", "").startswith("bannière") \
                and vm.vivant():
            recharge_sshd(vm, info)
    else:
        os.makedirs(inc, exist_ok=True)
        info = dict(notes, motif=etat)
    if vm.log and os.path.exists(vm.log) and os.path.isdir(inc):
        shutil.copy(vm.log, os.path.join(inc, "qemu.log"))
    if etat in ("panique", "gel") and vm.vivant() and panic_log \
            and not camp.args.sans_panic_log:
        recupere_panic_log(vm, info, inc)
    p = os.path.join(inc, "panique.txt")
    open(os.path.join(inc, "incident.json"), "w").write(json.dumps(info, indent=1, ensure_ascii=False))
    open(os.path.join(inc, "incident.md"), "w").write(
        rapport_incident(info, open(p).read() if os.path.exists(p) else ""))
    return os.path.relpath(inc, camp.dir), resume_incident(info)


def res_demarrage(camp, vm, t, num, depart, etat, det, t0):
    """Ligne de cycles.jsonl d'un démarrage, et collecte s'il a échoué."""
    a = camp.args
    res = {"type": "demarrage", "cycle": num, "instance": vm.slot, "etat": etat,
           "mode": a.mode if hasattr(a, "mode") else "jeu", "depart": depart,
           "binaire": vm.binaire, "leviers": vm.leviers, "gpu": vm.gpu,
           "duree": round(time.time() - t0), "t_ssh": det.get("t_ssh"), "port": vm.port,
           "charge": camp.charge_entre(t0)}
    if etat == "ok":
        sortie = det.get("sortie", "")
        ks = lit_kextstat(sortie)
        oh = [x for x in ks if x[0].endswith("AppleUSBOHCI")]
        if oh:
            res["ohci"] = "0x%x" % oh[0][1]
        if "/Library/Logs/panic.log" in sortie:
            res["panic_log_present"] = True
        m = re.search(r"^\s*sleep\s+(\d+)", sortie, re.M)
        if m:
            res["pmset_sleep"] = int(m.group(1))
        m = re.search(r"in6_ifattach\S*: .*\(errno=(\d+)\)", sortie)
        if m:
            res["in6_lo0_errno"] = int(m.group(1))
        camp.verifie_noyau(sortie)
        journal("[%d] %s #%d : ok (%s, ssh à %s s, charge %s)"
                % (vm.slot, t, num, depart, det.get("t_ssh"), res["charge"]["l1_max"]))
    else:
        journal("[%d] %s #%d : INCIDENT %s (%s)" % (vm.slot, t, num, etat, det))
        prefixe = "%04d" % num if t == camp.type else "%s%04d" % (t[0], num)
        res["incident"], res["resume"] = incident(camp, vm, prefixe, etat,
                                                  {"cycle": num, "t": det.get("t")}, t0)
    return res


# ------------------------------------------------------------- démarrages
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
            depart = a.mode
            if not vivante or a.mode == "froid":
                vm.quitte()
                if not vm.demarre(cyc):
                    res = {"type": "demarrage", "cycle": i, "instance": slot, "etat": "lancement",
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
                depart = "froid"
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
            res = res_demarrage(camp, vm, "demarrage", i, depart, etat, det, t0)
            if etat == "ok":
                open(os.path.join(cyc, "sante.txt"), "w").write(det.get("sortie", ""))
                if vm.serie and a.releve_sain and i % a.releve_sain == 0:
                    # référence : le même relevé par la console sur un démarrage sain
                    n = releve_serie(vm.serie, cyc)
                    res["serie"] = n.get("serie_resume") or n.get("serie")
                if a.mode == "froid" and not a.garde:
                    shutil.rmtree(cyc, ignore_errors=True)
            else:
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
    s = (info.get("kpanic_texte") or info.get("panicstr_texte") or info.get("kpanic_format")
         or info.get("jeu_texte") or "")
    cpus = info.get("cpus", [])
    pcs = "; ".join("cpu%s %s" % (c["cpu"], c.get("NIP", "?").split(" ", 1)[-1]) for c in cpus)
    return (s.strip()[:160] + " | " if s else "") + pcs


# ------------------------------------------------------------- rapport
def mediane(v):
    v = sorted(v)
    return v[len(v) // 2] if v else None


def rapport(dossier):
    cycles = []
    with open(os.path.join(dossier, "cycles.jsonl")) as f:
        for l in f:
            try:
                cycles.append(json.loads(l))
            except ValueError:
                pass
    camp = json.load(open(os.path.join(dossier, "campagne.json")))
    l = ["# Banc d'endurance — %s" % os.path.basename(dossier.rstrip("/")), "",
         "Paramètres : `%s`" % " ".join("%s=%s" % (k, v) for k, v in camp["args"].items()
                                          if v not in (None, False, []) and k != "func"),
         "Bras (variables de run_tiger.sh) : `%s`" % (camp.get("bras", {}).get("env") or "défauts du lanceur"),
         "Commit du banc : %s ; base : %s ; noyau des symboles : %s"
         % (camp.get("commit"), camp.get("base"), camp.get("noyau", "?")),
         "Hôte : %s" % camp.get("hote", "?")]
    bins = sorted({c.get("binaire") for c in cycles if c.get("binaire")})
    levs = sorted({c.get("leviers") for c in cycles if c.get("leviers") and c.get("leviers") != "?"})
    if bins:
        l += ["", "Binaires lancés :"] + ["- `%s`" % b for b in bins]
    if levs:
        l += ["", "Leviers annoncés par run_tiger.sh :"] + ["- `%s`" % x for x in levs]
    types = {}
    for c in cycles:
        types.setdefault(type_cycle(c), []).append(c)
    for t in sorted(types, key=lambda x: x != "demarrage"):
        cs = [c for c in types[t] if c["etat"] != "lancement"]
        n = len(cs)
        l += ["", "## %s (%d cycles)" % ("Démarrages" if t == "demarrage" else "Cycles de jeu", n), "",
              "| type | n | taux | IC 95 % (Wilson) |", "|---|---|---|---|"]
        etats = {}
        for c in cs:
            etats.setdefault(c["etat"], []).append(c)
        for e in ("panique", "gel"):
            etats.setdefault(e, [])
        for e in sorted(etats, key=lambda x: (x != "ok", x)):
            k = len(etats[e])
            lo, hi = wilson(k, n)
            l.append("| %s | %d / %d | %.1f %% | %.1f – %.1f %% |"
                     % (e, k, n, 100 * k / n if n else 0, 100 * lo, 100 * hi))
        inc = [c for c in cs if c["etat"] != "ok"]
        lo, hi = wilson(len(inc), n)
        l.append("| **tout incident** | %d / %d | %.1f %% | %.1f – %.1f %% |"
                 % (len(inc), n, 100 * len(inc) / n if n else 0, 100 * lo, 100 * hi))
        if t == "demarrage":
            for dep in sorted({c.get("depart") or c.get("mode") for c in cs} - {None}):
                sous = [c for c in cs if (c.get("depart") or c.get("mode")) == dep]
                k = len([c for c in sous if c["etat"] != "ok"])
                lo, hi = wilson(k, len(sous))
                l.append("| départ %s | %d incident(s) / %d | | %.1f – %.1f %% |"
                         % (dep, k, len(sous), 100 * lo, 100 * hi))
            ts = [c["t_ssh"] for c in etats.get("ok", []) if c.get("t_ssh")]
            if ts:
                ts.sort()
                l += ["", "Démarrage jusqu'au ssh : médiane %d s, min %d, max %d."
                      % (mediane(ts), ts[0], ts[-1])]
        else:
            l += ["", "| jeu | cycles | incidents | IC 95 % du taux d'incident | images (médiane) |",
                  "|---|---|---|---|---|"]
            for j in sorted({c.get("jeu") for c in cs}):
                sous = [c for c in cs if c.get("jeu") == j]
                k = len([c for c in sous if c["etat"] != "ok"])
                lo, hi = wilson(k, len(sous))
                l.append("| %s | %d | %d | %.1f – %.1f %% | %s |"
                         % (j, len(sous), k, 100 * lo, 100 * hi,
                            mediane([c.get("images", 0) for c in sous])))
        ch = [c["charge"]["l1_max"] for c in cs if isinstance(c.get("charge"), dict)]
        if ch:
            l += ["", "Charge de l'hôte (max sur 1 min pendant le cycle) : médiane %.1f, max %.1f."
                  % (mediane(ch), max(ch))]
        if inc:
            l += ["", "### Incidents", ""]
            for c in inc:
                l.append("- #%s (%s%s, charge %s) `%s` : %s"
                         % (c["cycle"], c["etat"], ", " + c["jeu"] if c.get("jeu") else "",
                            (c.get("charge") or {}).get("l1_max", "?"), c.get("incident", ""),
                            c.get("resume", "")))
    lanc = [c for c in cycles if c["etat"] == "lancement"]
    if lanc:
        l += ["", "%d lancement(s) de QEMU en échec (non comptés)." % len(lanc)]
    txt = "\n".join(l) + "\n"
    open(os.path.join(dossier, "rapport.md"), "w").write(txt)
    return txt


def fisher_bilateral(a, n1, b, n2):
    """p bilatéral du test exact de Fisher pour a/n1 contre b/n2."""
    from math import comb
    k, n = a + b, n1 + n2
    if k == 0 or k == n:
        return 1.0

    def pr(x):
        return comb(n1, x) * comb(n2, k - x) / comb(n, k)
    p0 = pr(a)
    return min(1.0, sum(pr(x) for x in range(max(0, k - n2), min(k, n1) + 1)
                        if pr(x) <= p0 * (1 + 1e-9)))


def compare(dossiers, type_="demarrage", depart=None):
    """Taux d'incident de plusieurs campagnes (même type de cycle), IC de Wilson,
    et test exact de Fisher de chacune contre la première."""
    lignes, ref = [], None
    for d in dossiers:
        cs = [json.loads(l) for l in open(os.path.join(d, "cycles.jsonl")) if l.strip()]
        cs = [c for c in cs if type_cycle(c) == type_ and c["etat"] != "lancement"
              and (depart is None or (c.get("depart") or c.get("mode")) == depart)]
        n = len(cs)
        k = len([c for c in cs if c["etat"] != "ok"])
        lo, hi = wilson(k, n)
        p = "" if ref is None else "%.3f" % fisher_bilateral(k, n, ref[0], ref[1])
        ref = ref or (k, n)
        lignes.append("| %s | %d / %d | %.1f %% | %.1f – %.1f %% | %s |"
                      % (os.path.basename(d.rstrip("/")), k, n, 100 * k / n if n else 0,
                         100 * lo, 100 * hi, p))
    return "\n".join(["| campagne | incidents | taux | IC 95 % (Wilson) | Fisher (contre la 1re) |",
                      "|---|---|---|---|---|"] + lignes)


# ------------------------------------------------------------- cycles de jeu
class HoteBanc:
    """Ce que les modules de tools/matrice/jeux attendent de l'hôte, sur une VM du banc."""

    def __init__(self, vm):
        self.vm = vm

    def ssh(self, cmd, delai=120, entree=None, binaire=False):
        return self.vm.ssh(cmd, delai=delai, entree=entree)

    def sortie(self, cmd, delai=120):
        return self.vm.ssh(cmd, delai=delai)[1]

    def depose(self, texte, chemin):
        return self.vm.ssh("cat > '%s'" % chemin, entree=texte)[0] == 0

    def processus(self, nom):
        out = self.sortie("ps -axco pid,command")
        return [int(p[0]) for p in (l.strip().split(None, 1) for l in out.splitlines()[1:])
                if len(p) == 2 and p[1] == nom and p[0].isdigit()]

    def premier_plan(self, nom):
        return self.ssh("osascript -e 'tell application \"System Events\" to set frontmost of "
                        "process \"%s\" to true'" % nom, delai=30)[0] == 0

    def clients_kext(self):
        """Clients ouverts du kext POMPPCGPU (tools/matrice/hote.py) ; None si illisible."""
        out = self.sortie("ioreg -w0 -c POMPPCGPUUserClient | grep -c 'POMPPCGPUUserClient '; true")
        try:
            return int(out.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return None


G = "/Users/tiger/matrice"


def prepare_invite(h, jeux, mode):
    """Comme la matrice : lance.command, réglages de chaque jeu sauvegardés
    (une fois) puis écrits pour le mode. Le recouvrement qcow2 est jeté à la fin
    de l'instance : rien de tout cela n'atteint la base."""
    h.ssh("mkdir -p %s" % G)
    h.depose(open(os.path.join(WT, "tools", "matrice", "guest", "lance.command")).read(),
             G + "/lance.command")
    h.ssh("chmod +x %s/lance.command" % G)
    for j in jeux:
        for f in j.fichiers_reglages(mode):
            h.ssh("f='%s'; if [ -e \"$f.matrice-sauve\" ] || [ -e \"$f.matrice-absent\" ]; then :; "
                  "elif [ -e \"$f\" ]; then cp -p \"$f\" \"$f.matrice-sauve\"; "
                  "else touch \"$f.matrice-absent\"; fi" % f)
        try:
            j.preparer(h, mode)
        except Exception as e:
            journal("réglages de %s : %s: %s" % (j.cle, type(e).__name__, e))


def cycle_jeu(camp, vm, h, j, i):
    """Un lancement du jeu j, `duree` s de jeu, sans l'arrêter (l'appelant le fait)."""
    a = camp.args
    gd = "%s/endurance-%04d" % (G, i)
    h.ssh("rm -rf %s; killall ScreenSaverEngine 2>/dev/null; true" % gd)
    env = {"POMPPC_GL_STATS": "1", "POMPPC_GL_NOTE": gd + "/note.txt",
           "POMPPC_GL_FRAMES": gd + "/frames.csv"}
    env.update(j.env)
    cel = ["# écrit par tools/endurance/endurance.py (%s, cycle %d)" % (j.cle, i), "D='%s'" % gd]
    cel += ["%s='%s'; export %s" % (k, v, k) for k, v in env.items()]
    cel.append("jeu() {\n%s\n}" % j.commande(a.jeu_mode).strip("\n"))
    h.depose("\n".join(cel) + "\n", G + "/cellule.sh")
    h.ssh("mkdir -p %s && open %s/lance.command" % (gd, G))
    t0 = time.time()
    duree = a.duree or DUREE_JEU.get(j.cle, 180)
    fige = FIGE_JEU.get(j.cle, 180)
    etat, muet, vu, images, t_images, log = "ok", 0, False, 0, None, ""
    while time.time() - t0 < duree:
        time.sleep(10)
        if not vm.vivant():
            etat = "qemu-mort"
            break
        if vm.panique_en_cours():
            etat = "panique"
            break
        code, out = vm.ssh("cat %s/log.txt 2>/dev/null; echo @@; wc -l < %s/frames.csv 2>/dev/null"
                           % (gd, gd), delai=40)
        if code in (124, 255):
            muet += 1
            if muet >= 4:
                etat = "gel"
                break
            continue
        muet = 0
        log, _, n = out.partition("@@")
        if "\nexit " in "\n" + log:
            etat = "sortie-du-jeu"
            break
        n = int(n.strip()) if n.strip().isdigit() else 0
        if n > images:
            images, t_images = n, time.time()
        if not vu and h.processus(j.processus):
            vu = True
            h.premier_plan(j.nom_ui())
        if images and time.time() - t_images > fige:
            etat = "jeu-fige"           # le jeu ne présente plus d'image ; l'invité répond
            break
    if etat == "ok" and not vu:
        etat = "pas-lance"
    res = {"type": "jeu", "cycle": i, "instance": vm.slot, "etat": etat, "jeu": j.cle,
           "duree": round(time.time() - t0), "images": images, "binaire": vm.binaire,
           "leviers": vm.leviers, "gpu": vm.gpu, "charge": camp.charge_entre(t0)}
    if etat != "ok":
        notes = {"cycle": i, "jeu": j.cle, "t": round(time.time() - t0), "images": images}
        if etat in ("sortie-du-jeu", "pas-lance", "jeu-fige"):
            # ce que le jeu a dit : log.txt (état de sortie), stdout, rapport de plantage
            _, out = vm.ssh("cat %s/log.txt; echo ----; tail -60 %s/stdout.txt; echo ----; "
                            "ls -t ~/Library/Logs/CrashReporter 2>/dev/null | head -3; "
                            "f=~/Library/Logs/CrashReporter/'%s.crash.log'; "
                            "[ -f \"$f\" ] && tail -120 \"$f\"; true" % (gd, gd, j.processus), delai=60)
            m = re.search(r"^exit (\d+)", out, re.M)
            notes["jeu_texte"] = ("exit %s" % m.group(1) if m else etat) + " ; " + \
                (log.strip().splitlines()[-1][:120] if log.strip() else "")
            notes["jeu_sortie"] = out[-8000:]
        if etat == "sortie-du-jeu" or etat == "pas-lance":
            inc = os.path.join(camp.dir, "incidents", "%04d-%s" % (i, etat))
            os.makedirs(inc, exist_ok=True)
            open(os.path.join(inc, "jeu.txt"), "w").write(notes.get("jeu_sortie", ""))
            notes["charge"] = res["charge"]
            open(os.path.join(inc, "incident.json"), "w").write(
                json.dumps(notes, indent=1, ensure_ascii=False))
            res["incident"], res["resume"] = os.path.relpath(inc, camp.dir), notes["jeu_texte"]
        else:
            res["incident"], res["resume"] = incident(camp, vm, "%04d" % i, etat, notes, t0)
            if notes.get("jeu_sortie"):
                open(os.path.join(camp.dir, res["incident"], "jeu.txt"), "w").write(notes["jeu_sortie"])
        journal("[%d] jeu #%d %s : INCIDENT %s %s" % (vm.slot, i, j.cle, etat, res.get("resume", "")))
    else:
        journal("[%d] jeu #%d %s : ok (%d images, charge %s)"
                % (vm.slot, i, j.cle, images, res["charge"]["l1_max"]))
    return etat, res


def instance_jeu(camp, slot, jeux):
    """Une VM : démarrage, puis cycles de jeu à tour de rôle (jeux[i % len]) ;
    entre deux cycles, `shutdown -r` dans l'invité (--redemarre) ; après un
    incident qui laisse l'invité douteux, QEMU neuf sur un recouvrement neuf."""
    a = camp.args
    vm = VM(camp.nom, slot, a)
    h = HoteBanc(vm)
    vivante, echecs = False, 0
    try:
        while not camp.fini():
            if not vivante:
                vm.quitte()
                boot = os.path.join(camp.dir, "demarrages", "%d-%s" % (slot, time.strftime("%H%M%S")))
                os.makedirs(boot, exist_ok=True)
                if not vm.demarre(boot):
                    journal("[%d] QEMU ne démarre pas (%s)" % (slot, vm.log))
                    camp.note({"type": "demarrage", "cycle": camp.numero("demarrage"), "instance": slot,
                               "etat": "lancement", "detail": open(vm.log).read()[-2000:]})
                    echecs += 1
                    if echecs >= 3:
                        journal("[%d] trois lancements ratés de suite : instance arrêtée" % slot)
                        break
                    time.sleep(10)
                    continue
                echecs = 0
                if vm.gpu != "gl" and not a.gpu_quelconque:
                    journal("[%d] backend qgpu « %s » et non gl : les jeux ne seraient pas rendus "
                            "par le GPU de l'hôte ; campagne arrêtée (--gpu-quelconque pour passer)"
                            % (slot, vm.gpu))
                    camp.arret = True
                    break
                t0 = time.time()
                etat, det = attend_demarrage(vm, t0, a.delai, a.repos)
                camp.note(res_demarrage(camp, vm, "demarrage", camp.numero("demarrage"), "froid",
                                        etat, det, t0))
                if etat != "ok":
                    vm.quitte()
                    continue
                if camp.fini():
                    break
                prepare_invite(h, jeux, a.jeu_mode)
                vivante = True
            i = camp.prochain()
            if i is None:
                break
            j = jeux[(i - 1) % len(jeux)]
            etat, res = cycle_jeu(camp, vm, h, j, i)
            camp.note(res)
            if etat in ("panique", "gel", "jeu-fige", "qemu-mort"):
                vm.quitte()
                vivante = False
                continue
            # arrêt du jeu et remise en état, comme la matrice
            j.arreter(h)
            gd = "%s/endurance-%04d" % (G, i)
            h.ssh("i=0; while ! grep -q '^exit ' %s/log.txt 2>/dev/null && [ $i -lt 20 ]; "
                  "do sleep 1; i=$((i+1)); done; osascript -e 'tell application \"Terminal\" to quit' "
                  "2>/dev/null; rm -rf %s; true" % (gd, gd), delai=60)
            restants = h.clients_kext()
            if a.redemarre == "toujours" or (a.redemarre == "d3" and j.cle == "d3") \
                    or j.redemarrer_apres or restants:
                if camp.fini():
                    break
                vm.ssh("echo tiger974 | sudo -S shutdown -r now", delai=30)
                t1 = time.time()
                while time.time() - t1 < 120 and vm.repond():
                    time.sleep(2)
                t0 = time.time()
                etat, det = attend_demarrage(vm, t0, a.delai, a.repos)
                r = res_demarrage(camp, vm, "demarrage", camp.numero("demarrage"), "reboot", etat, det, t0)
                if restants:
                    r["apres_clients_kext"] = restants
                camp.note(r)
                if etat != "ok":
                    vm.quitte()
                    vivante = False
                    continue
            time.sleep(a.pause)
    finally:
        vm.quitte()


def lance_instances(camp, cible, slots, ecart, *extra):
    fils = [threading.Thread(target=cible, args=(camp, s) + extra, daemon=True) for s in slots]
    for f in fils:
        f.start()
        time.sleep(ecart)          # décale les démarrages
    try:
        while any(f.is_alive() for f in fils):
            time.sleep(2)
    except KeyboardInterrupt:
        camp.arret = True
        journal("arrêt demandé : les instances finissent leur cycle")
        for f in fils:
            f.join()
    print(rapport(camp.dir))


# ------------------------------------------------------------- main
SYM = None


def main():
    global SYM
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sp = ap.add_subparsers(dest="cmd", required=True)
    for nom in ("demarrages", "jeu"):
        p = sp.add_parser(nom)
        p.add_argument("-n", type=int, default=20, help="cycles au plus (démarrages ou jeux)")
        p.add_argument("--jusqua", metavar="HH:MM",
                       help="ne plus commencer de cycle après cette heure locale")
        p.add_argument("--nom", help="nom de la campagne (dossier bench/endurance/NOM) ; "
                                     "un nom déjà pris AJOUTE des cycles (même bras exigé)")
        p.add_argument("--qemu", help="qemu-system-ppc64 imposé (défaut : celui que choisit "
                                      "run_tiger.sh, le binaire rapide sur le PC)")
        p.add_argument("--smp", type=int, default=2)
        p.add_argument("--extra", help="EXTRA_ARGS passés à run_tiger.sh")
        p.add_argument("--env", action="append", metavar="K=V",
                       help="variable passée à run_tiger.sh (répétable), p. ex. TLBPRECISE=0")
        p.add_argument("--anciens-defauts", action="store_true",
                       help="bras témoin : " + " ".join("%s=%s" % kv for kv in ANCIENS_DEFAUTS.items()))
        p.add_argument("--invite", help="dossier des symboles de l'invité (mach_kernel, "
                                        "endurance-syms/) ; défaut $ENDURANCE_INVITE ou "
                                        "bench/endurance/invite")
        p.add_argument("--delai", type=int, default=360, help="s sans ssh avant de déclarer un gel")
        p.add_argument("--repos", type=int, default=20, help="s d'observation après le premier ssh")
        p.add_argument("--sans-panic-log", action="store_true",
                       help="ne pas redémarrer après un incident pour lire panic.log")
        p.add_argument("--instances", type=int, default=1)
        p.add_argument("--fenetre", action="store_true",
                       help="fenêtre QEMU native (POMPPC_DISPLAY) au lieu de HEADLESS=1")
        p.add_argument("--serie", action="store_true",
                       help="second canal : UART PCI vers .run/endurance/<inst>/serie.sock et "
                            "base à getty (disks/tiger-endurance-serie.qcow2) ; relevé par la "
                            "console à chaque incident (docs/endurance.md §8)")
        p.add_argument("--releve-sain", type=int, default=0, metavar="N",
                       help="avec --serie : relevé par la console d'un démarrage sain sur N "
                            "(référence pour comparer ; 0 = jamais)")
    p = sp.choices["demarrages"]
    p.add_argument("--mode", choices=("froid", "reboot", "reset"), default="froid",
                   help="froid : un QEMU neuf par démarrage ; reboot : shutdown -r dans "
                        "l'invité ; reset : system_reset au moniteur")
    p.add_argument("--garde", action="store_true", help="garder les dossiers des cycles réussis")
    p.add_argument("--slot", type=int, default=0, help="première instance (port 2240 + 10·slot)")
    p = sp.choices["jeu"]
    p.add_argument("--jeu", required=True,
                   help="module(s) de tools/matrice/jeux, à tour de rôle : mb,zen,ut,d3")
    p.add_argument("--jeu-mode", default="fen", choices=("fen", "pe"))
    p.add_argument("--duree", type=int, default=None,
                   help="s de jeu par cycle (défaut par jeu : %s)" % DUREE_JEU)
    p.add_argument("--pause", type=int, default=10)
    p.add_argument("--slot", type=int, default=5, help="première instance (port 2240 + 10·slot)")
    p.add_argument("--redemarre", choices=("toujours", "d3", "jamais"), default="toujours",
                   help="shutdown -r dans l'invité entre deux cycles (défaut toujours ; « jamais » "
                        "pour chercher le kCGLBadDisplay de DOOM 3 ; un client du kext resté "
                        "ouvert redémarre toujours)")
    p.add_argument("--gpu-quelconque", action="store_true",
                   help="accepter un backend qgpu autre que gl")
    p = sp.add_parser("rapport")
    p.add_argument("dossier")
    p = sp.add_parser("compare", help="taux d'incident de campagnes (bras), Wilson et Fisher")
    p.add_argument("dossiers", nargs="+")
    p.add_argument("--type", default="demarrage", choices=("demarrage", "jeu"))
    p.add_argument("--depart", help="seulement ces départs (froid, reboot)")
    p = sp.add_parser("collecte", help="collecte d'incident sur une VM du banc déjà lancée")
    p.add_argument("moniteur")
    p.add_argument("dossier")
    p.add_argument("--invite")
    p.add_argument("--serie", metavar="SOCKET",
                   help="relevé par la console série d'abord (VM en marche), dans DOSSIER/serie.txt")
    a = ap.parse_args()
    if a.cmd == "rapport":
        print(rapport(a.dossier))
        return
    if a.cmd == "compare":
        print(compare(a.dossiers, a.type, a.depart))
        return
    SYM = Symboliseur(a.invite or INVITE)
    if a.cmd == "collecte":
        notes = {}
        if a.serie:
            os.makedirs(a.dossier, exist_ok=True)
            notes.update(releve_serie(a.serie, a.dossier))
        print(json.dumps(collecte(a.moniteur, a.dossier, SYM, "manuel", notes), indent=1,
                         ensure_ascii=False))
        return
    base = base_de(a)
    if not os.path.exists(base):
        raise SystemExit("base %s absente (voir docs/endurance.md §Préparation, §8)" % base)
    if os.access(base, os.W_OK):
        journal("⚠  base %s inscriptible : chmod a-w (docs/endurance.md §2)" % base)
    if a.cmd == "jeu":
        sys.path.insert(0, os.path.join(WT, "tools", "matrice"))
        import importlib
        jeux = [importlib.import_module("jeux." + k.strip()).JEU for k in a.jeu.split(",") if k.strip()]
        camp = Campagne(a, "jeu")
        journal("campagne %s : %d cycles de jeu (%s) au plus%s, %d instance(s), noyau %s"
                % (camp.nom, a.n, ",".join(j.cle for j in jeux),
                   " jusqu'à " + a.jusqua if a.jusqua else "", a.instances, SYM.version[:40]))
        lance_instances(camp, instance_jeu, range(a.slot, a.slot + a.instances), 60, jeux)
        return
    camp = Campagne(a, "demarrage")
    journal("campagne %s : %d démarrages au plus%s, %d instance(s), mode %s, QEMU %s, noyau %s"
            % (camp.nom, a.n, " jusqu'à " + a.jusqua if a.jusqua else "", a.instances, a.mode,
               a.qemu or "celui de run_tiger.sh", SYM.version[:40]))
    lance_instances(camp, instance, range(a.slot, a.slot + a.instances), 20)


if __name__ == "__main__":
    main()
