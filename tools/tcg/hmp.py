#!/usr/bin/env python3
"""hmp.py SOCKET "commande" [...] — envoie des commandes HMP au moniteur QEMU
(-monitor unix:SOCKET,server=on,wait=off) et imprime leurs réponses."""
import socket, sys, time

def recv_until_prompt(s, timeout=30.0):
    buf, t0 = b"", time.time()
    while not buf.rstrip().endswith(b"(qemu)") and time.time() - t0 < timeout:
        try:
            d = s.recv(65536)
        except socket.timeout:
            continue
        if not d:
            break
        buf += d
    return buf.decode("utf-8", "replace")

s = socket.socket(socket.AF_UNIX); s.settimeout(1.0); s.connect(sys.argv[1])
recv_until_prompt(s)
for cmd in sys.argv[2:]:
    s.sendall(cmd.encode() + b"\n")
    out = recv_until_prompt(s)
    lines = out.replace("\r", "").split("\n")
    # la 1re ligne est l'écho de la commande, la dernière l'invite
    print("\n".join(l for l in lines[1:] if not l.startswith("(qemu)")).strip())
