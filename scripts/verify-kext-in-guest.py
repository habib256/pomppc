#!/usr/bin/env python3
"""Vérifie le kext POMPPCGPU DANS l'invité Tiger, à l'aveugle.

Boote disks/tiger.qcow2 en single-user (boot-args -s), attend que l'écran se
stabilise (l'invite du shell), tape au clavier — via QMP send-key — :

    mount -uw /
    sh /pomppc/verify.sh

puis surveille les traces du device qgpu-pci (stderr de QEMU) : l'écriture
d'IRQ_MASK signe le chargement du kext, les « soumission … statut » signent
le programme de test. Des captures PNG de l'écran sont prises dans OUT/ pour
lecture humaine, et le verdict écrit par verify.sh se relit ensuite dans
/pomppc/result.txt du disque (cf. --read-result, macOS seulement).

Prérequis côté disque : les Xcode Tools (gcc 4.0) et /pomppc/ (sources +
verify.sh) injectés — voir docs/gpu-3d-tiger.md § « Vérification dans l'invité ».

    python3 scripts/verify-kext-in-guest.py            # disque persistant
    SNAPSHOT=1 python3 scripts/verify-kext-in-guest.py # writes jetés (pas de result.txt)
    python3 scripts/verify-kext-in-guest.py --read-result   # monte le disque et lit result.txt
    python3 scripts/verify-kext-in-guest.py --inject --yes  # recopie kext/ et guest/ dans /pomppc/ du disque

VERDICT (bug hunt T16) : il ne suffit plus que la trace contienne « fence 2 »,
sous-chaîne qu'une trace bavarde peut produire toute seule. On y lit les
SOUMISSIONS, une par une, et on exige la signature de qgpu_test : la scène de
référence acceptée (soumission longue, statut OK) ET la sonde d'opcode inconnu
(statut BAD_OPCODE au mot 1), sans aucun autre statut d'erreur.

VERROU (T17) : l'image est prise par un `flock` sur <image>.lock, le même que
tools/guest/devloop.py — deux outils qui pilotent la même image en même temps
la corrompent. Et `--inject` RÉÉCRIT le qcow2 (conversion en raw, montage,
reconversion) : il le dit et demande confirmation (ou --yes).
"""
import fcntl, json, os, re, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.environ.get("QEMU_BIN", os.path.expanduser("~/src/qemu/build/qemu-system-ppc"))
DISK = os.environ.get("DISK", os.path.join(ROOT, "disks", "tiger.qcow2"))
BIOS = os.path.join(ROOT, "patches", "smp-mac99", "openbios-smp-screamer.elf")
OUT = os.environ.get("OUT", os.path.join(ROOT, "bench", "guest-verify"))
BOOT_WAIT = int(os.environ.get("BOOT_WAIT", "40"))       # s avant la 1re capture
STABLE_S = int(os.environ.get("STABLE", "20"))            # écran figé pendant N s = invite
RUN_TIMEOUT = int(os.environ.get("RUN_TIMEOUT", "1500"))  # s pour compiler + charger + tester
_DISK_LOCK = None

# « qgpu-pci: soumission off=0x… len=… -> statut S (pc P), fence F »
SUBMIT_RE = re.compile(r"soumission off=0x[0-9a-f]+ len=(\d+) -> statut (\d+) "
                       r"\(pc (\d+)\), fence (\d+)")
ST_OK, ST_BAD_OPCODE = 0, 3


def lock_disk(what):
    """Verrou exclusif sur l'image (même convention que devloop.py)."""
    global _DISK_LOCK
    try:
        f = open(DISK + ".lock", "a+")
    except OSError as exc:
        print("⚠ verrou %s.lock impossible (%s) : pas de garde contre un "
              "second outil" % (DISK, exc))
        return
    try:
        fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.seek(0)
        sys.exit("image %s déjà prise par un autre outil (%s.lock : %s)"
                 % (DISK, DISK, f.read().strip() or "?"))
    f.seek(0); f.truncate()
    f.write("pid %d %s\n" % (os.getpid(), what)); f.flush()
    _DISK_LOCK = f


def submissions(path):
    """Les soumissions vues par le device, relues dans la trace de QEMU."""
    with open(path, "rb") as f:
        text = f.read().decode("latin-1")
    return [{"len": int(m.group(1)), "st": int(m.group(2)),
             "pc": int(m.group(3)), "fence": int(m.group(4))}
            for m in SUBMIT_RE.finditer(text)]


def verdict(subs):
    """La signature de qgpu_test : scène de référence + sonde d'opcode."""
    scene = [s for s in subs if s["len"] >= 64 and s["st"] == ST_OK]
    probe = [s for s in subs if s["st"] == ST_BAD_OPCODE and s["pc"] == 1]
    odd = sorted(set(s["st"] for s in subs) - {ST_OK, ST_BAD_OPCODE})
    fence = max([s["fence"] for s in subs] or [0])
    return {"scene": len(scene), "probe": len(probe), "autres_statuts": odd,
            "fence": fence, "total": len(subs),
            "ok": bool(scene) and bool(probe) and not odd and fence >= 2}


SHIFTED = {'_': 'minus', ':': 'semicolon', '>': 'dot', '<': 'comma', '|': 'backslash',
           '"': 'apostrophe', '?': 'slash', '+': 'equal', '~': 'grave_accent',
           '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7',
           '*': '8', '(': '9', ')': '0', '{': 'bracket_left', '}': 'bracket_right'}
PLAIN = {' ': 'spc', '-': 'minus', '/': 'slash', '.': 'dot', '\n': 'ret', ';': 'semicolon',
         '=': 'equal', ',': 'comma', "'": 'apostrophe', '`': 'grave_accent',
         '\\': 'backslash', '[': 'bracket_left', ']': 'bracket_right', '\t': 'tab'}


def qcodes(ch):
    if ch.isalpha():
        return (['shift', ch.lower()] if ch.isupper() else [ch])
    if ch.isdigit():
        return [ch]
    if ch in PLAIN:
        return [PLAIN[ch]]
    if ch in SHIFTED:
        return ['shift', SHIFTED[ch]]
    raise ValueError("caractère non mappé : %r" % ch)


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(path)
        self.f = self.s.makefile("rwb"); self.f.readline()
        self("qmp_capabilities")

    def __call__(self, cmd, **args):
        self.f.write((json.dumps({"execute": cmd, "arguments": args}) + "\n").encode())
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r

    def type(self, text, delay=0.06):
        for ch in text:
            keys = [{"type": "qcode", "data": k} for k in qcodes(ch)]
            self("send-key", keys=keys, **({"hold-time": 40}))
            time.sleep(delay)

    def shot(self, path):
        self("screendump", filename=path, format="png")
        time.sleep(0.5)
        return open(path, "rb").read() if os.path.exists(path) else b""


def with_disk(readonly, fn):
    """macOS : convertit le qcow2 en raw, le monte, appelle fn(volume), et
    reconvertit vers le qcow2 si le montage était en écriture.

    En écriture, c'est une RÉÉCRITURE COMPLÈTE de l'image : le qcow2 est
    remplacé par le résultat de la reconversion. Ça se dit (bug hunt T17)."""
    if not readonly:
        print("⚠ %s va être RÉÉCRIT en entier (qcow2 → raw → montage → qcow2)."
              % DISK)
        print("  Toute VM qui l'utilise doit être arrêtée ; une sauvegarde est "
              "prudente (qemu-img convert, ou cp).")
        if "--yes" not in sys.argv:
            if not sys.stdin.isatty():
                sys.exit("refusé : relancer avec --yes pour accepter la réécriture")
            if input("  Continuer ? (oui/non) ").strip().lower() not in ("oui", "o", "yes", "y"):
                sys.exit("abandon")
    qemu_img = os.path.join(os.path.dirname(QEMU), "qemu-img")
    tmp = tempfile.mkdtemp(prefix="tiger-disk-")
    raw = os.path.join(tmp, "tiger.raw")
    subprocess.check_call([qemu_img, "convert", "-O", "raw", DISK, raw])
    cmd = ["hdiutil", "attach", "-nobrowse", "-imagekey", "diskimage-class=CRawDiskImage", raw]
    if readonly:
        cmd.insert(2, "-readonly")
    out = subprocess.check_output(cmd).decode()
    dev = out.split()[0]
    try:
        vol = [l.split("\t")[-1].strip() for l in out.splitlines() if "/Volumes/" in l][0]
        fn(vol)
        subprocess.call(["sync"])
    finally:
        subprocess.call(["hdiutil", "detach", dev, "-quiet"])
    if not readonly:
        subprocess.check_call([qemu_img, "convert", "-O", "qcow2", raw, DISK + ".new"])
        os.replace(DISK + ".new", DISK)
    subprocess.call(["rm", "-rf", tmp])


def inject():
    """Recopie kext/POMPPCGPU, kext/POMPPCQFB, guest/qgpu-test et guest/verify.sh
    dans /pomppc/ du disque invité (les Xcode Tools doivent déjà y être)."""
    def do(vol):
        dst = os.path.join(vol, "pomppc")
        subprocess.call(["rm", "-rf", dst]); os.makedirs(dst)
        for d in ("kext/POMPPCGPU", "kext/POMPPCQFB", "guest/qgpu-test"):
            subprocess.check_call(["cp", "-R", os.path.join(ROOT, d), dst])
        subprocess.check_call(["cp", os.path.join(ROOT, "guest", "verify.sh"), dst])
        # sur le disque, qgpu-test est à côté de POMPPCGPU, pas sous guest/
        for f in ("qgpu_test.c", "Makefile"):
            p = os.path.join(dst, "qgpu-test", f)
            t = open(p).read().replace("../../kext/POMPPCGPU/", "../POMPPCGPU/")
            open(p, "w").write(t)
        print("sources injectées dans", dst)
    with_disk(False, do)


def read_result():
    def do(vol):
        p = os.path.join(vol, "pomppc", "result.txt")
        print(open(p).read() if os.path.exists(p) else "(pas de result.txt)")
        ppm = os.path.join(vol, "pomppc", "qgpu-test", "out.ppm")
        if os.path.exists(ppm):
            os.makedirs(OUT, exist_ok=True)
            subprocess.call(["cp", ppm, os.path.join(OUT, "guest-out.ppm")])
            print("surface invité copiée :", os.path.join(OUT, "guest-out.ppm"))
    with_disk(True, do)


def main():
    if "--read-result" in sys.argv:
        lock_disk("read-result"); read_result(); return 0
    if "--inject" in sys.argv:
        lock_disk("inject"); inject(); return 0
    lock_disk("verify")
    os.makedirs(OUT, exist_ok=True)
    qmp_path = os.path.join(tempfile.mkdtemp(prefix="guest-verify-"), "qmp.sock")
    log = open(os.path.join(OUT, "qemu-stderr.log"), "wb")
    args = [QEMU, "-M", "mac99,via=pmu", "-cpu", "g4", "-m", "1024", "-smp", "1",
            "-display", "none", "-bios", BIOS,
            "-drive", "file=%s,format=qcow2,media=disk" % DISK,
            "-device", "usb-tablet",
            "-device", "qgpu-pci,id=gpu0,backend=%s,trace=on" % os.environ.get("GPU_BACKEND", "auto"),
            "-prom-env", "auto-boot?=true",
            "-prom-env", "boot-device=hd:10,\\System\\Library\\CoreServices\\BootX",
            "-prom-env", "boot-args=-v -s",
            "-qmp", "unix:%s,server=on,wait=off" % qmp_path]
    if os.environ.get("SNAPSHOT"):
        args.append("-snapshot")
    print("▶", " ".join(args))
    qemu = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
    time.sleep(3)
    q = Qmp(qmp_path)

    def stderr_has(token):
        with open(log.name, "rb") as f:
            return token.encode() in f.read()

    # 1. attendre l'invite : l'écran ne change plus pendant STABLE_S secondes
    print("… boot single-user (%d s avant la première capture)" % BOOT_WAIT)
    time.sleep(BOOT_WAIT)
    prev, same_since, n = None, time.time(), 0
    while True:
        shot = q.shot(os.path.join(OUT, "boot-%02d.png" % n)); n += 1
        if shot == prev:
            if time.time() - same_since >= STABLE_S:
                break
        else:
            prev, same_since = shot, time.time()
        if n > 60:
            print("ÉCHEC : l'écran ne se stabilise pas (voir %s/boot-*.png)" % OUT)
            q("quit"); return 1
        time.sleep(5)
    print("  écran stable après %d captures → on suppose l'invite du shell" % n)

    # 2. taper les commandes
    q.type("mount -uw /\n"); time.sleep(4)
    q.type("sh /pomppc/verify.sh\n")
    q.shot(os.path.join(OUT, "typed.png"))

    # 3. surveiller : chargement du kext (IRQ_MASK) puis soumissions. Un écran
    #    figé pendant DONE_STABLE s après la frappe = verify.sh a rendu son verdict
    #    (il écrit une ligne par étape ; une compilation gcc 4.0 sous TCG dure
    #    quelques minutes sans rien afficher, d'où une fenêtre longue).
    DONE_STABLE = int(os.environ.get("DONE_STABLE", "240"))
    t0 = time.time(); seen_kext = seen_run = False
    v = verdict([])
    prev, same_since = None, time.time()
    while time.time() - t0 < RUN_TIMEOUT:
        time.sleep(10)
        if not seen_kext and stderr_has("reg[0x28]"):
            seen_kext = True; print("  ✔ kext chargé (IRQ_MASK écrit) après %d s" % (time.time() - t0))
        v = verdict(submissions(log.name))
        if v["ok"]:
            seen_run = True
            print("  ✔ qgpu_test exécuté : %(total)d soumissions, %(scene)d scène(s) "
                  "acceptée(s), %(probe)d sonde(s) d'opcode, fence %(fence)d" % v)
            break
        shot = q.shot(os.path.join(OUT, "progress.png"))
        if shot != prev:
            prev, same_since = shot, time.time()
        elif time.time() - same_since >= DONE_STABLE and time.time() - t0 > 60:
            print("  écran figé depuis %d s : verify.sh a probablement terminé" % DONE_STABLE); break
    q.shot(os.path.join(OUT, "after.png"))
    time.sleep(8)
    q.shot(os.path.join(OUT, "final.png"))
    q.type("sync\n"); time.sleep(3)
    q.type("halt\n"); time.sleep(8)
    try:
        q("quit")                      # halt a pu déjà arrêter QEMU (PMU)
    except (BrokenPipeError, ConnectionResetError, OSError):
        pass
    try:
        qemu.wait(timeout=30)
    except subprocess.TimeoutExpired:
        qemu.kill()
    log.close()
    with open(log.name, "rb") as f:
        tr = [l for l in f.read().decode("latin-1").splitlines() if "qgpu" in l]
    print("\n".join(tr[-12:]))
    v = verdict(submissions(log.name))
    seen_run = v["ok"]
    print("captures :", OUT)
    print("soumissions vues : %(total)d (scène de référence %(scene)d, sonde "
          "d'opcode %(probe)d, fence %(fence)d)" % v)
    if v["autres_statuts"]:
        print("⚠ statuts inattendus dans la trace :", v["autres_statuts"])
    print("kext chargé :", seen_kext, "| test exécuté :", seen_run)
    return 0 if (seen_kext and seen_run) else 1


if __name__ == "__main__":
    sys.exit(main())
