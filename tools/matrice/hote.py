# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""hote.py — gestes de l'hôte sur la VM quotidienne pour la matrice de jeux :
ssh dans l'invité (tools/guest/tssh.sh), moniteur HMP (.run/mon.sock),
capture d'écran figée, redémarrage de l'invité, rapatriement de dossiers.
"""
import fcntl
import os
import re
import socket
import subprocess
import sys
import time

ICI = os.path.dirname(os.path.abspath(__file__))
WT = os.path.dirname(os.path.dirname(ICI))


def depot_principal():
    """Racine du dépôt principal (où vivent .run/ et bench/), même depuis un worktree."""
    try:
        d = subprocess.run(["git", "-C", WT, "rev-parse", "--path-format=absolute",
                            "--git-common-dir"], capture_output=True, text=True).stdout.strip()
        if d.endswith("/.git"):
            return d[:-5]
    except OSError:
        pass
    return WT


MAIN = depot_principal()
RUN = os.path.join(MAIN, ".run")
LOCK = os.path.join(RUN, "tiger.lock")
TSSH = os.path.join(WT, "tools", "guest", "tssh.sh")   # lit .run/tiger.sshport


def moniteur():
    """Socket moniteur de la VM QUOTIDIENNE : celui que run_tiger.sh a publié
    dans .run/tiger.mon (jamais celui d'une VM SNAPSHOT=1 d'un autre agent)."""
    if os.environ.get("MATRICE_MON"):
        return os.environ["MATRICE_MON"]
    try:
        with open(os.path.join(RUN, "tiger.mon")) as f:
            return f.read().strip() or os.path.join(RUN, "mon.sock")
    except OSError:
        return os.path.join(RUN, "mon.sock")


def verrou_tenu(chemin=LOCK):
    """Vrai si un processus tient le flock de `chemin` (la VM quotidienne)."""
    try:
        fd = os.open(chemin, os.O_RDONLY)
    except OSError:
        return False
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
        return False
    except OSError:
        return True
    finally:
        os.close(fd)


def detenteurs(chemin=LOCK):
    """PID des processus qui ont `chemin` ouvert (lsof) : notre QEMU."""
    out = subprocess.run(["lsof", "-t", chemin], capture_output=True, text=True).stdout
    return [int(x) for x in out.split() if x.isdigit()]


class VMEnPause(Exception):
    """`cont` n'est pas passé : la VM reste arrêtée, le tour ne peut pas continuer."""


def journal(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class Hote:
    @property
    def mon(self):
        return moniteur()

    # ---------------------------------------------------------------- ssh
    def ssh(self, cmd, entree=None, delai=120, binaire=False):
        """(code, sortie) d'une commande dans l'invité ; code 255 = ssh en échec."""
        try:
            p = subprocess.run([TSSH, cmd], input=entree, capture_output=True,
                               timeout=delai, text=not binaire)
        except subprocess.TimeoutExpired:
            return 124, "" if not binaire else b""
        return p.returncode, p.stdout

    def sortie(self, cmd, delai=120):
        return self.ssh(cmd, delai=delai)[1]

    def depose(self, texte, chemin):
        """Écrit un fichier dans l'invité (texte ou octets)."""
        b = texte.encode() if isinstance(texte, str) else texte
        r = subprocess.run([TSSH, "cat > '%s'" % chemin], input=b, capture_output=True, timeout=60)
        return r.returncode == 0

    def rapatrie(self, dossier_invite, local):
        """Copie un dossier de l'invité (tar par ssh) dans `local`."""
        os.makedirs(local, exist_ok=True)
        parent, nom = os.path.split(dossier_invite.rstrip("/"))
        p1 = subprocess.Popen([TSSH, "cd '%s' && tar cf - '%s'" % (parent, nom)],
                              stdout=subprocess.PIPE)
        p2 = subprocess.run(["tar", "xf", "-", "-C", local], stdin=p1.stdout, capture_output=True)
        p1.stdout.close()
        p1.wait(timeout=1800)
        return p1.returncode == 0 and p2.returncode == 0

    def repond(self):
        return self.ssh("true", delai=20)[0] == 0

    def attend_ssh(self, delai=300):
        t0 = time.time()
        while time.time() - t0 < delai:
            if self.repond():
                return True
            time.sleep(5)
        return False

    # --------------------------------------------------------- moniteur
    def hmp(self, commande, delai=30):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(delai)
        s.connect(self.mon)

        def jusqu_invite():
            b = b""
            while b"(qemu)" not in b:
                c = s.recv(65536)
                if not c:
                    break
                b += c
            return b
        jusqu_invite()
        s.sendall(commande.encode() + b"\n")
        out = jusqu_invite().decode("utf-8", "replace")
        s.close()
        out = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out).replace("\r", "")
        return "\n".join(l for l in out.split("\n")
                         if l.strip() and "(qemu)" not in l and l.strip() != commande)

    NOYAU = os.path.join(RUN, "mach_kernel")

    def noyau_local(self):
        """Copie hôte du /mach_kernel de l'invité (.run/mach_kernel, non
        versionnée), prise tant que l'invité répond : kpanic.py en a besoin
        pour symboliser un gel. Chemin, ou None."""
        if os.path.exists(self.NOYAU) and os.path.getsize(self.NOYAU) > 1000000:
            return self.NOYAU
        try:
            with open(self.NOYAU + ".part", "wb") as f:
                r = subprocess.run([TSSH, "cat /mach_kernel"], stdout=f, timeout=120)
            if r.returncode == 0 and os.path.getsize(self.NOYAU + ".part") > 1000000:
                os.replace(self.NOYAU + ".part", self.NOYAU)
                return self.NOYAU
        except (OSError, subprocess.TimeoutExpired):
            pass
        return None

    def autopsie(self, dossier):
        """Invité gelé, AVANT tout reset : écran et tools/re/kpanic.py
        (PC/LR symbolisés, texte de panique, pile) dans `dossier`. Le gel
        du 26/09 était une panique dont Tiger ne garde aucune trace
        (docs/gel-doom3-baddisplay.md §2) : ce relevé est la seule preuve."""
        os.makedirs(dossier, exist_ok=True)
        try:
            self.capture(os.path.join(dossier, "gel.ppm"))
        except OSError:
            pass
        if not os.path.exists(self.NOYAU):
            journal("autopsie : pas de .run/mach_kernel, kpanic.py sauté")
            return
        with open(os.path.join(dossier, "kpanic.txt"), "w") as f:
            try:
                subprocess.run([sys.executable, os.path.join(WT, "tools", "re", "kpanic.py"),
                                self.NOYAU, self.mon], stdout=f, stderr=subprocess.STDOUT, timeout=600)
            except subprocess.TimeoutExpired:
                f.write("kpanic.py : délai dépassé\n")
        journal("autopsie : %s" % os.path.join(dossier, "kpanic.txt"))

    def capture(self, chemin):
        """screendump de l'écran de la VM (PPM)."""
        if os.path.exists(chemin):
            os.remove(chemin)
        self.hmp("screendump %s" % chemin)
        for _ in range(50):
            if os.path.exists(chemin) and os.path.getsize(chemin) > 1000:
                return True
            time.sleep(0.1)
        return False

    def capture_figee(self, chemin):
        """VM arrêtée (`stop`), capture, puis `cont` : l'image capturée est celle
        du dernier SURF_PRESENT exécuté, sans que le jeu n'avance entre-temps."""
        self.hmp("stop")
        try:
            time.sleep(0.3)             # le fil de rendu du device finit sa file
            ok = self.capture(chemin)
        finally:
            # une VM laissée en pause fausse tout le reste du tour : on insiste,
            # puis on arrête le tour plutôt que de mesurer une VM figée
            for essai in range(3):
                try:
                    self.hmp("cont")
                    break
                except OSError as e:
                    journal("cont refusé (%s), nouvel essai" % e)
                    time.sleep(2)
            else:
                raise VMEnPause("`cont` impossible après la capture : VM laissée en pause")
        return ok

    # ---------------------------------------------------------- invité
    def redemarre(self):
        """Redémarre l'invité (après DOOM 3 : kCGLBadDisplay) ; panique
        AppleUSBOHCI au démarrage (~1/10) : system_reset et on réessaie."""
        journal("redémarrage de l'invité")
        if self.repond():
            self.ssh("echo tiger974 | sudo -S shutdown -r now", delai=30)
            t0 = time.time()
            while time.time() - t0 < 120 and self.repond():
                time.sleep(3)
        else:                           # invité gelé (panique, blocage) : RESET d'emblée
            journal("l'invité ne répond pas : system_reset")
            self.hmp("system_reset")
        for essai in range(3):
            time.sleep(40)
            if self.attend_ssh(240):
                time.sleep(30)          # bureau au repos (Finder, Dock, mds)
                return True
            try:
                if essai < 1:
                    journal("pas de ssh après le redémarrage (panique au démarrage ?) : system_reset")
                    self.hmp("system_reset")
                else:
                    # 26/09 : après un gel en jeu, deux system_reset de suite restent
                    # bloqués au démarrage (« cluster IO buffer headers ») ; seul un
                    # QEMU relancé repart
                    if not self.relance_qemu():
                        return False
            except OSError:
                return False
        return False

    def relance_qemu(self):
        """quit au moniteur puis ./run_tiger.sh du dépôt principal, détaché.

        JAMAIS de suppression de .run/tiger.lock (bug hunt 4, 29/09/2026) :
        c'est un flock sur l'inode, et sous macOS QEMU ne verrouille pas
        l'image lui-même. Supprimer le chemin quand `quit` n'est pas arrivé
        (moniteur occupé, ou d'une autre VM) lançait un second QEMU sur
        tiger.qcow2 — corruption du HFS+. On attend que le verrou se libère ;
        sinon on abandonne, sans rien relancer."""
        journal("QEMU arrêté et relancé (run_tiger.sh)")
        avant = detenteurs()
        try:
            self.hmp("quit")
        except OSError as e:
            journal("quit au moniteur %s refusé : %s" % (self.mon, e))
        for _ in range(60):
            if not verrou_tenu():
                break
            time.sleep(1)
        else:
            journal("le verrou %s est toujours tenu (PID %s, avant quit : %s) : "
                    "QEMU non relancé, à arrêter à la main" % (LOCK, detenteurs(), avant))
            return False
        # Le banc conserve la fenêtre QEMU native ; le lanceur quotidien
        # utilise maintenant ImGuiDock par défaut.
        env = dict(os.environ, POMPPC_FRONTEND="native")
        subprocess.Popen(["./run_tiger.sh"], cwd=MAIN, env=env, start_new_session=True,
                         stdout=open(os.path.join(MAIN, ".run", "run_tiger-matrice.log"), "w"),
                         stderr=subprocess.STDOUT)
        return True

    def clients_kext(self):
        """Nombre de clients ouverts du kext POMPPCGPU (une tranche chacun,
        quatre au plus). Hors jeu, il doit être nul : un client qui reste
        après l'arrêt du jeu est une tranche perdue — et quatre tranches
        perdues donnent à DOOM 3 « no OpenGL-supported video card » (29/09).
        None si ioreg ne répond pas."""
        out = self.sortie("ioreg -w0 -c POMPPCGPUUserClient | grep -c 'POMPPCGPUUserClient '; true")
        try:
            return int(out.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return None

    def processus(self, nom):
        """pids des processus dont le nom (ps -c) vaut `nom`."""
        out = self.sortie("ps -axco pid,command")
        pids = []
        for l in out.splitlines()[1:]:
            p = l.strip().split(None, 1)
            if len(p) == 2 and p[1] == nom:
                pids.append(int(p[0]))
        return pids

    def premier_plan(self, nom):
        self.ssh("osascript -e 'tell application \"System Events\" to set frontmost of process \"%s\" to true'"
                 % nom, delai=30)

    def charge_hote(self):
        """Autres QEMU en marche sur l'hôte et charge moyenne : les mesures de
        vitesse sont bruitées quand une autre VM tourne (autre agent)."""
        try:
            # toute machine mac99 (les copies de QEMU des autres agents s'appellent
            # aussi qret…, qsr…), sauf la nôtre ; pas les shells dont la ligne de
            # commande cite QEMU (26/09 : `pgrep -f qemu-system` comptait un zsh et
            # manquait un QEMU renommé). « La nôtre », c'est le détenteur du verrou
            # disque, pas « tout ce qui démarre sur tiger.qcow2 » : ce motif
            # excluait aussi les VM SNAPSHOT=1 des autres agents (bug hunt 4).
            notre = set(detenteurs())
            ps = subprocess.run(["ps", "-Ao", "pid=,comm=,args="], capture_output=True,
                                text=True).stdout
            autres = []
            for l in ps.splitlines():
                p = l.split(None, 2)
                if len(p) < 3 or "mac99" not in p[2]:
                    continue
                if p[0].isdigit() and int(p[0]) in notre:
                    continue
                if os.path.basename(p[1]) in ("zsh", "bash", "sh", "python3", "Python", "pgrep"):
                    continue
                autres.append(l)
            if sys.platform == "darwin":
                la = subprocess.run(["sysctl", "-n", "vm.loadavg"], capture_output=True,
                                    text=True).stdout
                la = la.strip("{} \n").split()[0]
            else:
                la = open("/proc/loadavg").read().split()[0]
        except OSError:
            return "?"
        return "%s autre(s) QEMU, charge %s" % (len(autres), la)
