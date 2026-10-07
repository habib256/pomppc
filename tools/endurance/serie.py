# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""serie.py — second canal vers l'invité du banc : une console série, sans ssh.

Le banc ajoute à chaque instance un UART 16550 sur PCI (`-device pci-serial`)
relié à un socket unix de l'hôte (`.run/endurance/<campagne>-<i>/serie.sock`).
Tiger 10.4.11 le prend avec Apple16X50Serial.kext (correspondance de classe PCI
0x0700) et le publie en /dev/tty.pci-serial17 ; la base
`disks/tiger-endurance-serie.qcow2` y lance un getty (ligne de /etc/ttys,
docs/endurance.md §8). Ce module ouvre une session (tiger / tiger974), exécute
des commandes et rend leur sortie — même quand le port 22 ne répond plus.

    python3 tools/endurance/serie.py SOCKET 'netstat -an' ['ifconfig -a' …]
    python3 tools/endurance/serie.py SOCKET --releve DOSSIER     # le relevé du banc
"""
import os
import re
import socket
import sys
import time

UTILISATEUR, MOT_DE_PASSE = "tiger", "tiger974"

# Le relevé d'incident (et d'un démarrage sain, pour comparer). sudo -S lit le
# mot de passe sur l'entrée : `sudo -k` d'abord, pour qu'il le demande toujours.
SUDO = "echo %s | sudo -S -p '' " % MOT_DE_PASSE
RELEVE = [
    ("date", "date; uptime; sysctl kern.boottime"),
    ("ifconfig", "ifconfig -a"),
    ("netstat-an", "netstat -an"),
    ("netstat-rn", "netstat -rn"),
    ("arp", "arp -an"),
    ("ipconfig", "ipconfig getifaddr en0; ipconfig getpacket en0"),
    ("port22-local", "for h in 127.0.0.1 10.0.2.15; do echo \"-- $h:22\"; "
                     "(sleep 4; echo) | nc -w 4 $h 22 2>&1 | head -2; echo \"nc=$?\"; done"),
    ("ps", "ps axww -o pid,ppid,stat,lstart,time,command"),
    ("launchctl", "sudo -k; " + SUDO + "launchctl list"),
    ("lsof-i", "sudo -k; " + SUDO + "lsof -nP -i"),
    ("dmesg", "sudo -k; " + SUDO + "dmesg | tail -200"),
    ("system.log", "tail -250 /var/log/system.log"),
    ("scutil", "printf 'show State:/Network/Global/IPv4\\nshow State:/Network/Interface/en0/IPv4\\n"
               "show State:/Network/Interface/lo0/IPv4\\nquit\\n' | scutil"),
]


class Console:
    """Une session de shell sur la console série (socket unix de QEMU)."""

    def __init__(self, chemin, delai=10):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(delai)
        self.s.connect(chemin)
        self.tampon = b""
        self.journal = []           # tout ce qui a été lu, pour le dossier d'incident

    def fermer(self):
        try:
            self.s.close()
        except OSError:
            pass

    def envoie(self, texte):
        self.s.sendall(texte.encode("latin-1"))

    def lit_jusqua(self, motif, delai):
        """Lit jusqu'à `motif` (regex, octets) ; (correspondance, texte lu) ou (None, texte)."""
        fin = time.time() + delai
        rx = re.compile(motif)
        while True:
            m = rx.search(self.tampon)
            if m:
                lu, self.tampon = self.tampon[:m.end()], self.tampon[m.end():]
                self.journal.append(lu)
                return m, lu.decode("latin-1")
            reste = fin - time.time()
            if reste <= 0:
                lu, self.tampon = self.tampon, b""
                self.journal.append(lu)
                return None, lu.decode("latin-1")
            self.s.settimeout(min(reste, 1.0))
            try:
                b = self.s.recv(65536)
            except socket.timeout:
                continue
            if not b:
                lu, self.tampon = self.tampon, b""
                return None, lu.decode("latin-1")
            self.tampon += b

    def connexion(self, delai=60):
        """Ouvre une session ; '' si c'est fait, sinon ce qui manque."""
        # un retour chariot réveille getty (« login: ») ou un shell resté ouvert
        etat = ""
        fin = time.time() + delai
        while time.time() < fin:
            self.envoie("\r")
            m, lu = self.lit_jusqua(rb"(ogin: ?$|ogin: |[$#] $|assword:)", 8)
            if not m:
                etat = "rien sur la console en %d s" % delai
                continue
            k = m.group(1)
            if k.startswith(b"ogin"):
                self.envoie(UTILISATEUR + "\r")
                m, lu = self.lit_jusqua(rb"assword:", 15)
                if not m:
                    etat = "pas de « Password: » après le nom"
                    continue
                # login coupe l'écho APRÈS avoir écrit « Password: », par un
                # tcsetattr(TCSAFLUSH) qui jette ce qui est déjà arrivé : un mot de
                # passe envoyé aussitôt est perdu (« Login incorrect »)
                time.sleep(1.5)
                self.envoie(MOT_DE_PASSE + "\r")
                m, lu = self.lit_jusqua(rb"([$#] $|incorrect)", 30)
                if not m or b"incorrect" in m.group(1):
                    etat = "connexion refusée (%r)" % lu[-80:]
                    continue
            elif k.startswith(b"assword"):
                self.envoie("\r")           # login: interrompu : on recommence
                continue
            # shell : invite sans couleur ni écho, pour délimiter les sorties
            self.envoie("unset PROMPT_COMMAND; PS1='@> '; stty -echo -onlcr cols 250; "
                        "export TERM=dumb\r")
            # resynchronisation : l'écho de la ligne ci-dessus (« PS1='@> ' »)
            # ressemble à l'invite ; la marque de fin d'une commande, non
            code, _ = self.commande("true", 15)
            if code == 0:
                return ""
            etat = "pas d'invite après la connexion"
        return etat or "rien sur la console en %d s" % delai

    def commande(self, cmd, delai=60):
        """(code, sortie) ; code None si la fin n'est pas venue à temps."""
        self.envoie("%s 2>&1; echo \"@@F\"\"IN@@ $?\"\r" % cmd)
        m, lu = self.lit_jusqua(rb"@@FIN@@ (\d+)\r?\n", delai)
        sortie = lu[:m.start()] if m else lu
        sortie = sortie.replace("\r", "")
        if m:
            self.lit_jusqua(rb"@> $", 5)
            return int(m.group(1)), sortie
        return None, sortie

    def deconnexion(self):
        try:
            self.envoie("exit\r")
        except OSError:
            pass


def releve(chemin, dossier=None, commandes=RELEVE, delai_connexion=60):
    """Relevé complet par la console ; écrit DOSSIER/serie.txt (et la trace brute
    serie-brut.txt). Rend (statut, texte) ; statut '' si la session a été ouverte."""
    t0 = time.time()
    morceaux = []
    try:
        c = Console(chemin)
    except OSError as e:
        statut = "socket série injoignable : %s" % e
        texte = statut + "\n"
        c = None
    if c:
        statut = c.connexion(delai_connexion)
        if statut:
            morceaux.append("!! %s\n" % statut)
        else:
            for nom, cmd in commandes:
                code, out = c.commande(cmd, 90 if nom in ("ps", "lsof-i") else 60)
                morceaux.append("===== %s (code %s) : %s\n%s\n" % (nom, code, cmd, out.rstrip()))
            c.deconnexion()
        morceaux.append("===== fin du relevé en %.0f s\n" % (time.time() - t0))
        texte = "".join(morceaux)
        brut = b"".join(c.journal)
        c.fermer()
        if dossier:
            open(os.path.join(dossier, "serie-brut.txt"), "wb").write(brut)
    if dossier:
        open(os.path.join(dossier, "serie.txt"), "w").write(texte)
    return statut, texte


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    sock = sys.argv[1]
    if sys.argv[2] == "--releve":
        d = sys.argv[3] if len(sys.argv) > 3 else None
        if d:
            os.makedirs(d, exist_ok=True)
        statut, texte = releve(sock, d)
        print(texte)
        sys.exit(1 if statut else 0)
    c = Console(sock)
    statut = c.connexion()
    if statut:
        print("!!", statut)
        sys.exit(1)
    for cmd in sys.argv[2:]:
        code, out = c.commande(cmd)
        print("$ %s   (code %s)\n%s" % (cmd, code, out.rstrip()))
    c.deconnexion()
    c.fermer()


if __name__ == "__main__":
    main()
