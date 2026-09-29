#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""epreuve_reset.py — épreuve « panique puis system_reset » sur la VM quotidienne.

    python3 tools/re/epreuve_reset.py MACH_KERNEL [N] [--normal]

Chaque tour : charge disque dans l'invité (le CPU 0 doit être dans le noyau,
pas au repos), panique SYNTHÉTIQUE par le stub GDB de QEMU (r3 = format
« System Failure: cpu=%d; code=%08X (%s) », PC = _panic : même signature que
le gel du 26/09, CPU 0 en _panic+0x254 interruptions coupées), 10 s
d'attente (l'OHCI lève sa ligne, personne ne la sert), autopsie par
kpanic.py, puis `system_reset` et attente du ssh (240 s au plus).
`--normal` : system_reset sur un invité vivant, sans panique (témoin).

Avant patches/openpic/0001 : démarrage bloqué à coup sûr après
« using 1966 buffer headers », CPU 0 bouclant entre AppleMPIC et
AppleUSBOHCI, source 28 (OHCI) de l'OpenPIC active. Après : ssh revient.

MACH_KERNEL : copie du /mach_kernel de l'invité (voir kpanic.py). Détruit
l'état de l'invité (panique) : VM quotidienne seulement, jeux fermés."""
import os, re, socket, subprocess, sys, time

ici = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ici)
import kpanic  # noqa: E402

ROOT = kpanic.ROOT
TSSH = os.path.join(ROOT, "tools", "guest", "tssh.sh")
MON = open(ROOT + "/.run/tiger.mon").read().strip()
GDB = ROOT + "/.run/gdb-epreuve.sock"


def ssh(c, delai=30):
    try:
        p = subprocess.run([TSSH, c], capture_output=True, text=True, timeout=delai)
        return p.returncode, p.stdout
    except subprocess.TimeoutExpired:
        return 124, ""


def le(x):
    return int.from_bytes(x.to_bytes(4, "big"), "little")


def source28(cmd):
    """IVPR de la source 28 (OHCI de KeyLargo) de l'OpenPIC de mac99 (0x80040000)."""
    o = cmd("xp /1wx 0x%x" % (0x80040000 + 0x10000 + 28 * 0x20))
    return le(int(o.split(":")[1].split()[0], 16))


def fausse_panique(cmd, d):
    """Met le CPU 0, pris en mode noyau hors repos, dans panic()."""
    if os.path.exists(GDB):
        os.remove(GDB)
    cmd("gdbserver unix:%s,server=on,wait=off" % GDB)
    brut = open(NOYAU, "rb").read()
    # __TEXT commence au décalage 0 du fichier et se charge à 0xe000 (10.4.6)
    fmt = 0xe000 + brut.find(b"System Failure: cpu=%d; code=%08X (%s)\n\0")
    s = 0xe000 + brut.find(b"panic\0")
    if fmt < 0xe000 or s < 0xe000:
        raise SystemExit("chaînes introuvables dans %s" % NOYAU)
    fait = False
    try:
        for _ in range(100):
            g = socket.socket(socket.AF_UNIX)
            g.settimeout(10)
            g.connect(GDB)

            def send(data):
                g.sendall(b"$" + data + b"#%02x" % (sum(data) & 255))
                buf = b""
                while True:
                    c = g.recv(65536)
                    if not c:
                        raise SystemExit("stub fermé")
                    buf += c
                    m = re.search(rb"\$([^#]*)#[0-9a-f]{2}", buf)
                    if m:
                        g.sendall(b"+")
                        return m.group(1)
            send(b"?")
            send(b"Hg1")
            pc = int(send(b"p40"), 16)
            msr = int(send(b"p41"), 16)
            # mode noyau (PR=0) et PAS au repos (POW=0) : écrire le PC d'un
            # CPU endormi dans machine_idle ne le fait pas repartir et abîme
            # le fil de repos (vu le 29/09)
            if not msr & 0x4000 and not msr & 0x40000 and 0x10000 <= pc < 0x30b8a8:
                for reg, val in ((3, fmt), (4, 0), (5, 0x99), (6, s)):
                    send(b"P%x=%016x" % (reg, val))
                send(b"P43=%016x" % pc)
                send(b"P40=%016x" % d["_panic"])
                fait = True
            send(b"D")
            g.close()
            if fait:
                break
            time.sleep(0.3)
    finally:
        cmd("gdbserver none")
    return fait


def attend_ssh(delai):
    t0 = time.time()
    while time.time() - t0 < delai:
        time.sleep(8)
        code, out = ssh("uptime", 15)
        if code == 0 and "up" in out:
            return int(time.time() - t0)
    return None


if len(sys.argv) < 2:
    raise SystemExit(__doc__)
NOYAU = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 3
normal = "--normal" in sys.argv
t, d = kpanic.symboles(NOYAU)
bons = 0
for k in range(n):
    if attend_ssh(300) is None:
        raise SystemExit("invité muet avant le tour %d" % (k + 1))
    time.sleep(30)                      # session graphique ouverte
    ssh("printf 'while :; do find /System -name x >/dev/null 2>&1; done\\n' > /tmp/charge.sh; "
        "nohup sh /tmp/charge.sh >/dev/null 2>&1 &")
    time.sleep(3)
    cmd = kpanic.moniteur(MON)
    if not normal and not fausse_panique(cmd, d):
        print("tour %d : pas de CPU 0 en mode noyau, tour sauté" % (k + 1))
        cmd.close()
        continue
    time.sleep(10)
    avant = source28(cmd)
    cmd("system_reset")
    cmd.close()
    dt = attend_ssh(240)
    if dt is not None:
        bons += 1
        print("tour %d : source 28 avant reset %08x (%s) ; ssh revenu en %d s"
              % (k + 1, avant, "active" if avant & 0x40000000 else "calme", dt), flush=True)
    else:
        cmd = kpanic.moniteur(MON)
        r = cmd("info registers -a")
        pcs = [int(l.split()[1], 16) for l in r.splitlines() if l.startswith("NIP")]
        print("tour %d : BLOQUÉ, source 28 %08x, PC %s" % (k + 1, source28(cmd),
              " ".join("%08x" % p for p in pcs)), flush=True)
        break
print("bilan : %d/%d démarrages après %s" % (bons, n, "reset" if normal else "panique + reset"))
