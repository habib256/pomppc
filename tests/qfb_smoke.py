#!/usr/bin/env python3
"""Test de bout en bout du device « qfb-pci », sans pilote invité.

On s'arrête à l'invite Open Firmware, on programme les registres en Forth, on
remplit la VRAM, puis on capture l'écran côté hôte (QMP screendump) et on
vérifie les pixels obtenus.

    python3 tests/qfb_smoke.py            # 640x480, sortie lisible
    QEMU_BIN=/chemin/qemu-system-ppc python3 tests/qfb_smoke.py

Sortie : code 0 si tout passe.
"""
import json, os, socket, subprocess, sys, tempfile, threading, time

QEMU = os.environ.get("QEMU_BIN",
                      os.path.expanduser("~/src/qemu/build/qemu-system-ppc"))
W, H = 640, 480
VRAM, REGS = 0x82000000, 0x84000000      # BARs assignés par OpenBIOS sur mac99
BAND = 0x4000                            # 16 Kio = 4096 pixels de test

# Non-régression : un scanout qui déborde de la VRAM faisait ABORTER QEMU.
# MODE_BASE accepte n'importe quel offset < 32 Mio pendant que width/height sont
# bornés séparément, si bien qu'un invité pouvait programmer un mode dont les
# lignes sortent de la région. Les lectures sont repliées par qfb_read_byte(),
# mais pas la requête dirty : memory_region_snapshot_get_dirty() fait
# assert(start + length <= snap->end) et l'HÔTE tombe.
OVERFLOW_BASE = 0x1F00000                # 31 Mio, sur 32 Mio de VRAM

FORTH = f"""
{REGS:x} l@ .
{W:x} {REGS + 4:x} l!
{H:x} {REGS + 8:x} l!
18 {REGS + 0xc:x} l!
0 {REGS + 0x10:x} l!
{REGS + 0x14:x} l@ .
{VRAM:x} {W * H * 4:x} 20 fill
: reds {VRAM:x} dup {BAND:x} + swap do ff0000 i l! 4 +loop ;
reds
: greens {VRAM + BAND:x} dup {BAND:x} + swap do ff00 i l! 4 +loop ;
greens
"""


def main():
    if not os.path.exists(QEMU):
        print("QEMU introuvable :", QEMU); return 2

    tmp = tempfile.mkdtemp(prefix="qfb-smoke-")
    qmp_path = os.path.join(tmp, "qmp.sock")
    shot = os.path.join(tmp, "screen.ppm")

    qemu = subprocess.Popen(
        [QEMU, "-M", "mac99,via=pmu", "-m", "512", "-nographic", "-vga", "none",
         "-device", "qfb-pci,width=%d,height=%d,depth=8" % (W, H),
         "-prom-env", "auto-boot?=false",
         "-qmp", "unix:%s,server=on,wait=off" % qmp_path],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    buf = bytearray()
    threading.Thread(target=lambda: [buf.extend(b) for b in iter(
        lambda: qemu.stdout.read(1), b"")], daemon=True).start()

    deadline = time.time() + 90
    while time.time() < deadline and b"0 >" not in buf:
        time.sleep(0.2)
    if b"0 >" not in buf:
        print("ÉCHEC : pas d'invite Open Firmware"); qemu.kill(); return 1

    for line in FORTH.strip().splitlines():
        qemu.stdin.write((line + "\n").encode()); qemu.stdin.flush()
        time.sleep(0.6)
    time.sleep(3)

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
    r = qmp("screendump", filename=shot)
    time.sleep(1.5)
    if "error" in r:
        print("ÉCHEC screendump :", r); qemu.kill(); return 1

    # ── Non-régression : scanout débordant de la VRAM ──────────────────────
    # On repousse la base à 31 Mio en gardant un mode haut : les dernières
    # lignes tombent au-delà des 32 Mio. Avant correctif, le screendump qui
    # suit déclenchait l'assert de QEMU et tuait le processus.
    for line in ("%x %x l!" % (OVERFLOW_BASE, REGS + 0x10),
                 "%x %x l!" % (H, REGS + 8)):
        qemu.stdin.write((line + "\n").encode()); qemu.stdin.flush()
        time.sleep(0.5)
    time.sleep(1.0)
    overflow_shot = os.path.join(tmp, "overflow.ppm")
    try:
        qmp("screendump", filename=overflow_shot)
        time.sleep(1.0)
        survived = qemu.poll() is None
    except (BrokenPipeError, ConnectionResetError, ValueError):
        survived = False

    qemu.kill()

    data = open(shot, "rb").read()
    _, dims, _, pixels = data.split(b"\n", 3)
    w, h = map(int, dims.split())

    def px(x, y):
        o = (y * w + x) * 3
        return tuple(pixels[o:o + 3])

    checks = [
        ("dimensions", (w, h), (W, H)),
        ("stride lu",  b"a00" in buf, True),          # 640 * 4 octets
        ("bande rouge", px(4, 2),      (255, 0, 0)),
        ("bande verte", px(4, 8),      (0, 255, 0)),
        ("fond gris",   px(320, 240),  (32, 32, 32)),
        ("survie débordement", survived, True),   # non-régression : pas d'abort hôte
    ]
    failed = 0
    for name, got, want in checks:
        ok = (got == want)
        failed += (not ok)
        print("%-13s %-18s %s" % (name, got, "OK" if ok else "ÉCHEC (attendu %s)" % (want,)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
