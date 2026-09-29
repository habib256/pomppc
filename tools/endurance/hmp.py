# GPL3 - Copyleft VERHILLE Arnaud
"""hmp.py — client minimal du moniteur HMP de QEMU (socket unix), sans écho.

    python3 tools/endurance/hmp.py SOCKET info registers -a
"""
import re
import socket
import sys


def hmp(chemin, commande, delai=30):
    """Sortie texte de `commande` ; OSError si le moniteur est injoignable."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(delai)
    s.connect(chemin)
    try:
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
    finally:
        s.close()
    # l'écho de la commande est réécrit caractère par caractère (séquences
    # d'effacement) : on le retire avec les séquences, puis l'invite finale
    out = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out).replace("\r", "")
    lignes = [l for l in out.split("\n") if "(qemu)" not in l]
    if lignes and lignes[0].rstrip().endswith(commande.strip()):
        lignes = lignes[1:]
    return "\n".join(l for l in lignes if l.strip())


def mots(chemin, adresse, n, physique=True, cpu=None):
    """n mots de 32 bits lus à `adresse` (xp, ou x dans le contexte du vCPU `cpu`)."""
    res = []
    while n > 0:
        k = min(n, 256)
        cmd = "%s /%dwx 0x%x" % ("xp" if physique else "x", k, adresse)
        if not physique and cpu is not None:
            # `cpu N` ne vaut que pour la connexion qui l'envoie : une seule
            # connexion pour les deux commandes
            out = hmp_suite(chemin, ["cpu %d" % cpu, cmd])
        else:
            out = hmp(chemin, cmd)
        for l in out.splitlines():
            if ":" not in l:
                continue
            for w in l.split(":", 1)[1].split():
                if w.startswith("0x"):
                    res.append(int(w, 16))
        adresse += 4 * k
        n -= k
    return res


def hmp_suite(chemin, commandes, delai=30):
    """Plusieurs commandes sur UNE connexion ; sortie de la dernière."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(delai)
    s.connect(chemin)
    try:
        def jusqu_invite():
            b = b""
            while b"(qemu)" not in b:
                c = s.recv(65536)
                if not c:
                    break
                b += c
            return b
        jusqu_invite()
        out = ""
        for c in commandes:
            s.sendall(c.encode() + b"\n")
            out = jusqu_invite().decode("utf-8", "replace")
    finally:
        s.close()
    out = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out).replace("\r", "")
    lignes = [l for l in out.split("\n") if "(qemu)" not in l]
    if lignes and lignes[0].rstrip().endswith(commandes[-1].strip()):
        lignes = lignes[1:]
    return "\n".join(l for l in lignes if l.strip())


if __name__ == "__main__":
    print(hmp(sys.argv[1], " ".join(sys.argv[2:])))
