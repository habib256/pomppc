#!/usr/bin/env python3
"""Test de bout en bout du device « qgpu-pci », sans pilote invité.

Même principe que qfb_smoke.py : on s'arrête à l'invite Open Firmware, on
écrit le flux de commandes dans la fenêtre partagée (BAR0) en Forth, on
programme les registres (BAR1), on frappe le doorbell, puis on relit fence,
statut et pixels — par l'invité lui-même (l@), pas par l'hôte : c'est le
chemin exact que suivra le kext.

Le scénario est celui de tests/qgpu_core_test.c (rouge sur fond bleu, puis
un opcode invalide), avec les mêmes pixels témoins.

Depuis le protocole v9 le test enchaîne sur le DOORBELL ASYNCHRONE : rafale
qui remplit la file (QGPU_ST_QUEUE_FULL), barrière qui rattrape, relecture qui
n'apparaît qu'à l'avancement de la barrière, soumission fautive au milieu de la
file qui n'arrête pas les suivantes, interruption DONE, et enfin une soumission
synchrone pour vérifier que le mode v1–v8 est resté ce qu'il était.

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
# v9 : trois scènes de plus, à des offsets distincts, pour pouvoir en mettre
# plusieurs en file d'un coup — chacune avec sa propre zone de relecture.
CMD_A, CMD_B, CMD_C = 0x20000, 0x21000, 0x22000
RB_A, RB_C = 0x30000, 0x40000
W = H = 64
STRIDE = W * 4

# opcodes / longueurs : miroir de patches/qgpu/qgpu_proto.h
OP_CTX_CREATE, OP_CTX_BIND = 0x0001, 0x0003
OP_SURF_CREATE, OP_SURF_BIND, OP_SURF_READBACK = 0x0010, 0x0012, 0x0013
OP_CLEAR, OP_DRAW = 0x0020, 0x0030
REG_MAGIC, REG_VERSION, REG_CAPS = 0x00, 0x04, 0x08
REG_SHMEM_SIZE, REG_SUBMIT_OFF, REG_SUBMIT_LEN = 0x0C, 0x10, 0x14
REG_DOORBELL, REG_FENCE, REG_STATUS, REG_STATUS_PC = 0x18, 0x1C, 0x20, 0x24
REG_IRQ_MASK, REG_IRQ = 0x28, 0x2C
# v9
REG_QUEUE_FREE, REG_FENCE_SUBMITTED = 0x38, 0x3C
REG_SUBMIT_ST, REG_ERRORS, REG_QUEUE_DEPTH = 0x40, 0x44, 0x48
DOORBELL_GO, DOORBELL_ASYNC = 1, 2
ST_OK, ST_BAD_OPCODE, ST_QUEUE_FULL = 0, 3, 10
CAP_ASYNC = 0x8
IRQ_DONE = 0x1
BURST = 0x28           # 40 doorbells asynchrones d'affilée (file = 16)
SENTINEL = 0xDEADBEEF


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


def scene(color, rb_off):
    """Scène v9 : le contexte et la surface existent déjà (SCENE les a créés),
    on ne fait que les relier, effacer et relire."""
    return [
        hdr(OP_CTX_BIND, 2), 0,
        hdr(OP_SURF_BIND, 2), 1,
        hdr(OP_CLEAR, 4), 1, color, f2u(1.0),
        hdr(OP_SURF_READBACK, 8), 1, rb_off, STRIDE, 0, 0, W, H,
    ]


SCENE_A = scene(0x00FF00, RB_A)       # vert
SCENE_C = scene(0xFF00FF, RB_C)       # magenta


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

    # ── v9 : le doorbell asynchrone ────────────────────────────────────────
    # Un binaire QEMU antérieur à la v9 n'a pas ces registres : le dire
    # franchement plutôt que de laisser une avalanche d'échecs obscurs.
    version = read_val(regs + REG_VERSION)
    if (version or 0) < 9:
        print("\nÉCHEC : le device annonce le protocole v%s, la v9 est attendue."
              % version)
        print("  Ce binaire QEMU est périmé : ./scripts/build_qemu_qfb.sh")
        qemu.kill()
        return 1

    # Après les deux soumissions synchrones ci-dessus, le device doit être au
    # repos : rien en file, autant d'acceptées que de terminées.
    depth = read_val(regs + REG_QUEUE_DEPTH)
    sub0 = read_val(regs + REG_FENCE_SUBMITTED)
    db0 = read_val(regs + REG_DOORBELL)
    free0 = read_val(regs + REG_QUEUE_FREE)
    err0 = read_val(regs + REG_ERRORS)

    poke_words(shmem + CMD_A, SCENE_A)
    poke_words(shmem + CMD_B, BAD)
    poke_words(shmem + CMD_C, SCENE_C)

    # Des mots Forth pour les trois registres, puis un mot par scène : sans
    # cela une ligne de rafale dépasserait la soixantaine de caractères que
    # l'invite Open Firmware avale sans broncher.
    send(": qdb %x ;" % (regs + REG_DOORBELL))
    send(": qso %x ;" % (regs + REG_SUBMIT_OFF))
    send(": qsl %x ;" % (regs + REG_SUBMIT_LEN))
    send(": qa %x qso l! %x qsl l! 3 qdb l! ;" % (CMD_A, len(SCENE_A) * 4))
    send(": qb %x qso l! %x qsl l! 3 qdb l! ;" % (CMD_B, len(BAD) * 4))
    send(": qc %x qso l! %x qsl l! 3 qdb l! ;" % (CMD_C, len(SCENE_C) * 4))

    def drain(timeout=30.0):
        """Attend que QGPU_REG_DOORBELL retombe à 0 (plus rien en vol)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if read_val(regs + REG_DOORBELL) == 0:
                return True
            time.sleep(0.2)
        return False

    # (a) Rafale : BURST doorbells asynchrones d'affilée sur la scène A. La
    # file fait 16 places et une scène coûte bien plus qu'une écriture MMIO :
    # la file DOIT déborder, donc moins de BURST soumissions sont acceptées.
    # C'est la preuve que le device ne les a PAS exécutées dans l'écriture.
    send("%x qso l!" % CMD_A)
    send("%x qsl l!" % (len(SCENE_A) * 4))
    send(": qburst %x 0 do 3 qdb l! loop ;" % BURST)
    send("qburst", 0.5)
    sub1 = read_val(regs + REG_FENCE_SUBMITTED)
    subst1 = read_val(regs + REG_SUBMIT_ST)
    accepted = None if sub1 is None or sub0 is None else sub1 - sub0
    drained1 = drain()
    fence3 = read_val(regs + REG_FENCE)
    free1 = read_val(regs + REG_QUEUE_FREE)
    err1 = read_val(regs + REG_ERRORS)

    def px_at(base, x, y):
        v = read_val(shmem + base + y * STRIDE + x * 4)
        return None if v is None else v & 0xFFFFFF

    p_burst = px_at(RB_A, 8, 8)

    # (b) Trois soumissions asynchrones EN FILE, celle du milieu fautive. Les
    # deux zones de relecture sont d'abord salies : seule l'exécution des
    # scènes peut y remettre du vert et du magenta.
    send("%x %x l!" % (SENTINEL, shmem + RB_A + 8 * STRIDE + 8 * 4))
    send("%x %x l!" % (SENTINEL, shmem + RB_C + 8 * STRIDE + 8 * 4))
    send(": qseq qa qb qc ;")
    send("qseq", 0.5)
    sub2 = read_val(regs + REG_FENCE_SUBMITTED)
    drained2 = drain()
    fence4 = read_val(regs + REG_FENCE)
    err2 = read_val(regs + REG_ERRORS)
    status4 = read_val(regs + REG_STATUS)
    p_a, p_c = px_at(RB_A, 8, 8), px_at(RB_C, 8, 8)

    # (c) L'interruption DONE a bien été posée par le thread de rendu (par un
    # bottom half) ; elle est masquée, donc seule la ligne « en attente » la
    # montre. Elle s'acquitte comme en v8.
    irq1 = read_val(regs + REG_IRQ)
    send("%x %x l!" % (IRQ_DONE, regs + REG_IRQ))
    irq2 = read_val(regs + REG_IRQ)

    # (d) Le mode synchrone n'a pas bougé : au retour du `stw`, tout est là.
    send("%x %x l!" % (CMD_C, regs + REG_SUBMIT_OFF))
    send("%x %x l!" % (len(SCENE_C) * 4, regs + REG_SUBMIT_LEN))
    send("%x %x l!" % (DOORBELL_GO, regs + REG_DOORBELL), 1.0)
    sub3 = read_val(regs + REG_FENCE_SUBMITTED)
    fence5 = read_val(regs + REG_FENCE)
    db3 = read_val(regs + REG_DOORBELL)
    status5 = read_val(regs + REG_STATUS)

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
        # v9
        ("device asynchrone (caps)", ((caps or 0) & CAP_ASYNC) != 0, True),
        ("profondeur de file annoncée", (depth or 0) > 0, True),
        ("acceptées = terminées au repos", sub0, 2),
        ("rien en vol au repos", db0, 0),
        ("file vide au repos", free0, depth),
        ("compteur d'erreurs après l'opcode inconnu", err0, 1),
        ("rafale : file débordée", (accepted or 0) < BURST, True),
        ("rafale : au moins la file acceptée", (accepted or 0) >= (depth or 0), True),
        ("dernier doorbell refusé (file pleine)", subst1, ST_QUEUE_FULL),
        ("rafale : file drainée", drained1, True),
        ("barrière rattrape les acceptées", fence3, sub1),
        ("file de nouveau vide", free1, depth),
        ("un refus ne compte pas comme erreur", err1, err0),
        ("relecture visible après la barrière", p_burst, 0x00FF00),
        ("3 soumissions mises en file", None if sub2 is None else sub2 - sub1, 3),
        ("file drainée après la séquence", drained2, True),
        ("barrière = acceptées après la séquence", fence4, sub2),
        ("une seule erreur dans la séquence", None if err2 is None else err2 - err1, 1),
        ("statut = celui de la dernière terminée", status4, ST_OK),
        ("la scène avant l'erreur s'est exécutée", p_a, 0x00FF00),
        ("la scène APRÈS l'erreur aussi", p_c, 0xFF00FF),
        ("IRQ DONE posée par le thread de rendu", (irq1 or 0) & IRQ_DONE, IRQ_DONE),
        ("IRQ DONE acquittée", (irq2 or 0) & IRQ_DONE, 0),
        ("synchrone : une acceptée de plus", None if sub3 is None else sub3 - sub2, 1),
        ("synchrone : terminée au retour du stw", fence5, sub3),
        ("synchrone : plus rien en vol", db3, 0),
        ("synchrone : statut lisible tout de suite", status5, ST_OK),
    ]
    failed = 0
    for name, got, want in checks:
        ok = (got == want)
        failed += (not ok)
        shown = ("0x%x" % got) if isinstance(got, int) and not isinstance(got, bool) else str(got)
        print("%-30s %-12s %s" % (name, shown, "OK" if ok else "ÉCHEC (attendu %s)" % (want,)))
    print("backend hôte : caps=0x%x (1=soft, 2=gl, 8=doorbell asynchrone)" % (caps or 0))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
