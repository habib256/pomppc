#!/usr/bin/env python3
"""moncmd.py — envoie une commande HMP au moniteur QEMU et affiche la réponse.

Le moniteur est exposé en socket UNIX par les lanceurs (`-monitor unix:...`) :
`.run/mon.sock` pour Tiger (run_tiger.sh, scripts/boot.sh) et `.run/os9-mon.sock`
pour Mac OS 9 (run_os9.sh).

    python3 scripts/moncmd.py .run/mon.sock "screendump /tmp/f.ppm"
    python3 scripts/moncmd.py .run/os9-mon.sock "info block"
    python3 scripts/moncmd.py .run/mon.sock quit

Code de sortie : 0 si la commande a été transmise et une réponse lue, 1 sinon
(socket absent, VM disparue, timeout). Les appelants (scripts/measure-boot.sh,
scripts/profile-boot.sh, ./mount) s'appuient sur ce code pour détecter une VM
morte, donc ne jamais sortir 0 sans réponse.
"""
import socket
import sys

TIMEOUT_S = 10.0


def moncmd(path, command):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(TIMEOUT_S)
    sock.connect(path)

    # La bannière du moniteur arrive avant la première invite « (qemu) ».
    def read_until_prompt():
        buf = b""
        while b"(qemu)" not in buf:
            chunk = sock.recv(4096)
            if not chunk:            # moniteur fermé = VM partie
                return buf, False
            buf += chunk
        return buf, True

    read_until_prompt()
    sock.sendall(command.encode() + b"\n")

    # 'quit' coupe la VM : le moniteur ferme sans réafficher d'invite.
    out, got_prompt = read_until_prompt()
    text = out.decode("utf-8", "replace")
    # Retirer l'écho de la commande et l'invite finale pour ne garder que la sortie.
    lines = [l for l in text.splitlines() if l.strip() not in ("", command)]
    lines = [l.replace("(qemu)", "").rstrip() for l in lines]
    print("\n".join(l for l in lines if l.strip()))
    sock.close()
    return got_prompt or command.strip().startswith("quit")


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 1
    path, command = sys.argv[1], sys.argv[2]
    try:
        return 0 if moncmd(path, command) else 1
    except (OSError, socket.timeout) as e:
        print("moncmd: %s: %s" % (path, e), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
