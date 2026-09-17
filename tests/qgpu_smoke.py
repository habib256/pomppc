#!/usr/bin/env python3
"""Test de bout en bout du device « qgpu-pci », sans pilote invité.

Même principe que qfb_smoke.py : on s'arrête à l'invite Open Firmware, on
écrit le flux de commandes dans la fenêtre partagée (BAR0) en Forth, on
programme les registres (BAR1), on frappe le doorbell, puis on relit fence,
statut et pixels — par l'invité lui-même (l@), pas par l'hôte : c'est le
chemin exact que suivra le kext.

Le scénario est celui de tests/qgpu_core_test.c (rouge sur fond bleu, puis
un opcode invalide), avec les mêmes pixels témoins.

    python3 tests/qgpu_smoke.py                     # backend auto
    QGPU_BACKEND=soft python3 tests/qgpu_smoke.py   # force un backend
    QEMU_BIN=/chemin/qemu-system-ppc python3 tests/qgpu_smoke.py

Sortie : code 0 si tout passe.
"""
import json, os, socket, struct, subprocess, sys, tempfile, threading, time

QEMU = os.environ.get("QEMU_BIN",
                      os.path.expanduser("~/src/qemu/build/qemu-system-ppc"))
BACKEND = os.environ.get("QGPU_BACKEND", "auto")

CMD_OFF, VTX_OFF, RB_OFF = 0x1000, 0x4000, 0x10000
W = H = 64
STRIDE = W * 4

# opcodes / longueurs : miroir de patches/qgpu/qgpu_proto.h
OP_CTX_CREATE, OP_CTX_BIND = 0x0001, 0x0003
OP_SURF_CREATE, OP_SURF_BIND, OP_SURF_READBACK = 0x0010, 0x0012, 0x0013
OP_CLEAR, OP_DRAW = 0x0020, 0x0030
REG_MAGIC, REG_CAPS, REG_SUBMIT_OFF, REG_SUBMIT_LEN = 0x00, 0x08, 0x10, 0x14
REG_DOORBELL, REG_FENCE, REG_STATUS, REG_STATUS_PC = 0x18, 0x1C, 0x20, 0x24
ST_OK, ST_BAD_OPCODE = 0, 3


def hdr(op, n):
    return (op << 16) | n


def f2u(f):
    return struct.unpack(">I", struct.pack(">f", f))[0]


def vertex(x, y, r, g, b):
    return [f2u(x), f2u(y), f2u(0.0), f2u(1.0), f2u(r), f2u(g), f2u(b), f2u(1.0)]


SCENE = [
    hdr(OP_CTX_CREATE, 2), 0,
    hdr(OP_CTX_BIND, 2), 0,
    hdr(OP_SURF_CREATE, 5), 1, W, H, 1,
    hdr(OP_SURF_BIND, 2), 1,
    hdr(OP_CLEAR, 4), 1, 0x0000FF, f2u(1.0),
    hdr(OP_DRAW, 3), 3, VTX_OFF,
    hdr(OP_SURF_READBACK, 8), 1, RB_OFF, STRIDE, 0, 0, W, H,
]
VERTS = vertex(4, 4, 1, 0, 0) + vertex(60, 4, 1, 0, 0) + vertex(4, 60, 1, 0, 0)
BAD = [hdr(0, 1), hdr(0x7777, 1)]


def main():
    if not os.path.exists(QEMU):
        print("QEMU introuvable :", QEMU); return 2

    tmp = tempfile.mkdtemp(prefix="qgpu-smoke-")
    qmp_path = os.path.join(tmp, "qmp.sock")
    qemu = subprocess.Popen(
        [QEMU, "-M", "mac99,via=pmu", "-m", "512", "-nographic", "-vga", "none",
         "-device", "qgpu-pci,backend=%s" % BACKEND,
         "-prom-env", "auto-boot?=false",
         "-qmp", "unix:%s,server=on,wait=off" % qmp_path],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    buf = bytearray()
    threading.Thread(target=lambda: [buf.extend(b) for b in iter(
        lambda: qemu.stdout.read(1), b"")], daemon=True).start()

    def wait_for(token, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if token in buf:
                return True
            time.sleep(0.1)
        return False

    if not wait_for(b"0 >", 90):
        print("ÉCHEC : pas d'invite Open Firmware"); qemu.kill(); return 1

    # Les BARs sont assignés par OpenBIOS : on les lit côté hôte (QMP), au lieu
    # de supposer des adresses qui dépendent de l'ordre des devices.
    sock = socket.socket(socket.AF_UNIX); sock.connect(qmp_path)
    f = sock.makefile("rwb"); f.readline()

    def qmp(cmd, **args):
        f.write((json.dumps({"execute": cmd, "arguments": args}) + "\n").encode())
        f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r:
                return r

    qmp("qmp_capabilities")
    shmem = regs = None
    for bus in qmp("query-pci")["return"]:
        for dev in bus["devices"]:
            if dev["id"]["vendor"] == 0x1234 and dev["id"]["device"] == 0x0fb2:
                for r in dev["regions"]:
                    if r["bar"] == 0: shmem = r["address"]
                    if r["bar"] == 1: regs = r["address"]
    if shmem is None or regs is None:
        print("ÉCHEC : qgpu-pci absent de query-pci ou BARs non assignés")
        qemu.kill(); return 1
    print("BAR0 (fenêtre) = 0x%x, BAR1 (registres) = 0x%x" % (shmem, regs))

    def send(line, pause=0.25):
        qemu.stdin.write((line + "\n").encode()); qemu.stdin.flush()
        time.sleep(pause)

    # Un mot Forth par lecture, avec une étiquette unique pour le parsing.
    send(': rd ." VAL=" l@ . cr ;', 0.5)

    def poke_words(base, words):
        # Deux stores par ligne, pas plus : au-delà d'une soixantaine de
        # caractères l'invite Open Firmware perd la fin de la ligne (constaté :
        # les derniers mots de chaque ligne de six stores relus à zéro).
        line = []
        for i, w in enumerate(words):
            line.append("%x %x l!" % (w, base + 4 * i))
            if len(line) == 2:
                send(" ".join(line)); line = []
        if line:
            send(" ".join(line))

    def read_val(addr):
        start = len(buf)
        send("%x rd" % addr, 0.6)
        out = bytes(buf[start:]).decode("latin-1")
        i = out.rfind("VAL=")
        if i < 0:
            return None
        tok = out[i + 4:].split()
        return int(tok[0], 16) if tok else None

    poke_words(shmem + VTX_OFF, VERTS)
    poke_words(shmem + CMD_OFF, SCENE)
    # Relecture du flux écrit : si Open Firmware a avalé un caractère, on veut
    # le savoir ici, pas le deviner d'après un statut BAD_HEADER.
    bad_words = [(i, w, read_val(shmem + CMD_OFF + 4 * i))
                 for i, w in enumerate(SCENE)]
    bad_words = [(i, w, g) for i, w, g in bad_words if g != w]
    for i, w, g in bad_words:
        print("  mot %d : écrit 0x%x, relu %s" % (i, w, "0x%x" % g if g is not None else None))
    send("%x %x l!" % (CMD_OFF, regs + REG_SUBMIT_OFF))
    send("%x %x l!" % (len(SCENE) * 4, regs + REG_SUBMIT_LEN))
    send("1 %x l!" % (regs + REG_DOORBELL), 1.0)

    magic = read_val(regs + REG_MAGIC)
    caps = read_val(regs + REG_CAPS)
    fence1 = read_val(regs + REG_FENCE)
    status1 = read_val(regs + REG_STATUS)

    def px(x, y):
        # RGB seulement : depuis le protocole v2, l'octet haut est l'alpha
        v = read_val(shmem + RB_OFF + y * STRIDE + x * 4)
        return None if v is None else v & 0xFFFFFF

    p_in, p_out, p_corner = px(8, 8), px(60, 60), px(2, 2)

    # Seconde soumission : un opcode inconnu doit être signalé au bon index,
    # et la fence doit quand même avancer.
    poke_words(shmem + CMD_OFF, BAD)
    send("%x %x l!" % (len(BAD) * 4, regs + REG_SUBMIT_LEN))
    send("1 %x l!" % (regs + REG_DOORBELL), 1.0)
    fence2 = read_val(regs + REG_FENCE)
    status2 = read_val(regs + REG_STATUS)
    pc2 = read_val(regs + REG_STATUS_PC)

    qemu.kill()

    checks = [
        ("flux écrit relu à l'identique", len(bad_words), 0),
        ("signature 'qgp1'", magic, 0x71677031),
        ("caps non nulles", (caps or 0) != 0, True),
        ("fence après scène", fence1, 1),
        ("statut scène", status1, ST_OK),
        ("intérieur triangle rouge", p_in, 0xFF0000),
        ("fond bleu", p_out, 0x0000FF),
        ("coin bleu", p_corner, 0x0000FF),
        ("fence après erreur", fence2, 2),
        ("statut opcode inconnu", status2, ST_BAD_OPCODE),
        ("index de la commande fautive", pc2, 1),
    ]
    failed = 0
    for name, got, want in checks:
        ok = (got == want)
        failed += (not ok)
        shown = ("0x%x" % got) if isinstance(got, int) and not isinstance(got, bool) else str(got)
        print("%-30s %-12s %s" % (name, shown, "OK" if ok else "ÉCHEC (attendu %s)" % (want,)))
    print("backend hôte : caps=0x%x (1=soft, 2=gl)" % (caps or 0))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
