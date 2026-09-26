"""hote.py — gestes de l'hôte sur la VM quotidienne pour la matrice de jeux :
ssh dans l'invité (tools/guest/tssh.sh), moniteur HMP (.run/mon.sock),
capture d'écran figée, redémarrage de l'invité, rapatriement de dossiers.
"""
import os
import re
import socket
import subprocess
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
MON = os.environ.get("MATRICE_MON", os.path.join(MAIN, ".run", "mon.sock"))
TSSH = os.path.join(WT, "tools", "guest", "tssh.sh")


def journal(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class Hote:
    def __init__(self):
        self.mon = MON

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
            self.hmp("cont")
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
                    self.relance_qemu()
            except OSError:
                return False
        return False

    def relance_qemu(self):
        """quit au moniteur puis ./run_tiger.sh du dépôt principal, détaché."""
        journal("QEMU arrêté et relancé (run_tiger.sh)")
        try:
            self.hmp("quit")
        except OSError:
            pass
        for _ in range(30):
            if subprocess.run(["pgrep", "-f", "tiger.qcow2"], capture_output=True).returncode:
                break
            time.sleep(1)
        lock = os.path.join(MAIN, ".run", "tiger.lock")
        if os.path.exists(lock):
            os.remove(lock)
        subprocess.Popen(["./run_tiger.sh"], cwd=MAIN, start_new_session=True,
                         stdout=open(os.path.join(MAIN, ".run", "run_tiger-matrice.log"), "w"),
                         stderr=subprocess.STDOUT)

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
            ps = subprocess.run(["pgrep", "-fl", "qemu-system"], capture_output=True, text=True).stdout
            autres = [l for l in ps.splitlines() if "tiger.qcow2" not in l]
            la = subprocess.run(["sysctl", "-n", "vm.loadavg"], capture_output=True, text=True).stdout
            la = la.strip("{} \n").split()[0]
        except OSError:
            return "?"
        return "%s autre(s) QEMU, charge %s" % (len(autres), la)
