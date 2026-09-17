#!/usr/bin/env python3
"""devloop.py — boucle de développement rapide dans l'invité Tiger.

Une VM Tiger reste allumée (single-user, headless) avec l'agent
tools/guest/agent.sh ; chaque « job » est un dossier contenant job.sh (et ce
qu'il faut compiler), transmis par une boîte aux lettres sur le disque brut.
Pas de reboot, pas de conversion d'image : une itération = le temps du job.

    devloop.py prepare            # (VM arrêtée, macOS) installe agent + boîtes aux lettres
    devloop.py start              # boote la VM en single-user et lance l'agent (frappe)
    devloop.py start --gui        # boote le bureau : agent lancé par un StartupItem,
                                  # jobs graphiques relayés par POMPPCGuiRunner (gui.sh)
    devloop.py run DOSSIER [--timeout S]   # exécute DOSSIER/job.sh, rapatrie out/
    devloop.py shot [FICHIER.png] # capture d'écran
    devloop.py type "texte\\n"     # frappe au clavier
    devloop.py stop

Disque : $DEVDISK (raw), par défaut le tiger-dev.raw du scratchpad n'est pas
connu du dépôt : il faut le passer explicitement.
"""
import json, os, socket, struct, subprocess, sys, tarfile, io, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
QEMU = os.environ.get("QEMU_BIN", os.path.expanduser("~/src/qemu/build/qemu-system-ppc"))
DISK = os.environ.get("DEVDISK")
STATE = os.path.join(ROOT, "bench", "devloop")
QMP = os.path.join(STATE, "qmp.sock")
BIOS = os.path.join(ROOT, "patches", "smp-mac99", "openbios-smp-screamer.elf")
MBX_SECTORS = 65536            # 32 Mio par boîte
MAGIC = b"PMBX"


def need_disk():
    if not DISK or not os.path.exists(DISK):
        sys.exit("DEVDISK=<image raw Tiger> requis")


def find_mailbox(tag):
    """Cherche la boîte `tag` (b'IN' ou b'OU') : chaque secteur commence par
    PMBX + tag + index 32 bits. Vérifie la contiguïté."""
    cache = os.path.join(STATE, "mailbox.json")
    st = os.stat(DISK)
    if os.path.exists(cache):
        c = json.load(open(cache))
        if c.get("mtime_disk_prepared") and tag.decode() in c:
            return c[tag.decode()]
    raise SystemExit("boîtes aux lettres inconnues : lancer `devloop.py prepare`")


def scan_mailboxes(nonce):
    """Chaque préparation marque ses secteurs d'un nonce : les blocs d'une
    préparation précédente restent dans l'image (libérés, jamais effacés) et
    un balayage sans nonce tombait sur la copie périmée (vu en vrai)."""
    want = {b"IN": None, b"OU": None}
    with open(DISK, "rb") as f:
        # les fichiers sont quelque part dans le volume : balayage par pas de 4 Kio
        pos = 0
        blk = 4096
        size = os.fstat(f.fileno()).st_size
        while pos < size and None in want.values():
            f.seek(pos)
            chunk = f.read(64 * 1024 * 1024)
            if not chunk:
                break
            i = 0
            while True:
                i = chunk.find(MAGIC, i)
                if i < 0:
                    break
                if (pos + i) % 512 == 0 and chunk[i + 10:i + 18] == nonce:
                    tag = chunk[i + 4:i + 6]
                    idx = struct.unpack(">I", chunk[i + 6:i + 10])[0]
                    if tag in want and want[tag] is None and idx == 0:
                        want[tag] = (pos + i) // 512
                i += 4
            pos += len(chunk)
    for tag, sec in want.items():
        if sec is None:
            raise SystemExit("boîte %s introuvable dans l'image" % tag)
        with open(DISK, "rb") as f:
            for k in (1, MBX_SECTORS // 2, MBX_SECTORS - 1):
                f.seek((sec + k) * 512)
                b = f.read(10)
                b = b + f.read(8)
                if b[:6] != MAGIC + tag or struct.unpack(">I", b[6:10])[0] != k or b[10:18] != nonce:
                    raise SystemExit("boîte %s non contiguë (secteur %d)" % (tag, k))
    return {"IN": want[b"IN"], "OU": want[b"OU"]}


GUI_USER = os.environ.get("GUI_USER", "tiger")


def install_gui_support(vol):
    """StartupItem de l'agent + relais de session + élément d'ouverture."""
    import plistlib, shutil
    si = os.path.join(vol, "Library", "StartupItems", "POMPPCAgent")
    if os.path.exists(si):
        shutil.rmtree(si)
    shutil.copytree(os.path.join(ROOT, "tools", "guest", "POMPPCAgent"), si)
    app = os.path.join(vol, "Applications", "POMPPCGuiRunner.app")
    if os.path.exists(app):
        shutil.rmtree(app)
    shutil.copytree(os.path.join(ROOT, "tools", "guest", "POMPPCGuiRunner.app"), app)
    lw = os.path.join(vol, "Users", GUI_USER, "Library", "Preferences", "loginwindow.plist")
    if os.path.exists(lw):
        with open(lw, "rb") as f:
            pl = plistlib.load(f)
        items = pl.setdefault("AutoLaunchedApplicationDictionary", [])
        if not any(i.get("Path") == "/Applications/POMPPCGuiRunner.app" for i in items):
            items.append({"Hide": False, "Path": "/Applications/POMPPCGuiRunner.app"})
            with open(lw, "wb") as f:
                plistlib.dump(pl, f, fmt=plistlib.FMT_XML)
    else:
        print("⚠ pas de %s : le relais graphique ne démarrera pas" % lw)


def write_conf(boxes):
    """Remonte l'image pour écrire /pomppc/agent.conf (les boîtes ne bougent pas)."""
    out = subprocess.check_output(["hdiutil", "attach", "-nobrowse", "-imagekey",
                                   "diskimage-class=CRawDiskImage", DISK]).decode()
    dev = out.split()[0]
    vol = [l.split("\t")[-1].strip() for l in out.splitlines() if "/Volumes/" in l][0]
    try:
        with open(os.path.join(vol, "pomppc", "agent.conf"), "w") as f:
            f.write("%d %d\n" % (boxes["IN"], boxes["OU"]))
        subprocess.call(["sync"])
    finally:
        subprocess.check_call(["hdiutil", "detach", dev, "-quiet"])


def prepare():
    """VM arrêtée : monte l'image (macOS), écrit agent + boîtes, démonte, localise."""
    need_disk()
    os.makedirs(STATE, exist_ok=True)
    nonce = os.urandom(8)
    out = subprocess.check_output(["hdiutil", "attach", "-nobrowse", "-imagekey",
                                   "diskimage-class=CRawDiskImage", DISK]).decode()
    dev = out.split()[0]
    vol = [l.split("\t")[-1].strip() for l in out.splitlines() if "/Volumes/" in l][0]
    try:
        d = os.path.join(vol, "pomppc")
        os.makedirs(d, exist_ok=True)
        subprocess.check_call(["cp", os.path.join(ROOT, "tools", "guest", "agent.sh"), d])
        install_gui_support(vol)
        for tag, name in ((b"IN", "mbx-in.bin"), (b"OU", "mbx-out.bin")):
            p = os.path.join(d, name)
            if os.path.exists(p):
                os.remove(p)
            with open(p, "wb") as f:
                pad = b"\0" * 494
                f.write(b"".join(MAGIC + tag + struct.pack(">I", k) + nonce + pad
                                 for k in range(MBX_SECTORS)))
                f.flush(); os.fsync(f.fileno())
        subprocess.call(["sync"])
    finally:
        subprocess.check_call(["hdiutil", "detach", dev, "-quiet"])
    boxes = scan_mailboxes(nonce)
    write_conf(boxes)
    boxes["mtime_disk_prepared"] = time.time()
    json.dump(boxes, open(os.path.join(STATE, "mailbox.json"), "w"))
    print("boîtes : inbox secteur %d, outbox secteur %d" % (boxes["IN"], boxes["OU"]))


class Qmp:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(QMP)
        self.f = self.s.makefile("rwb"); self.f.readline()
        self("qmp_capabilities")

    def __call__(self, cmd, **args):
        self.f.write((json.dumps({"execute": cmd, "arguments": args}) + "\n").encode())
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r


SHIFTED = {'_': 'minus', ':': 'semicolon', '>': 'dot', '<': 'comma', '|': 'backslash',
           '"': 'apostrophe', '?': 'slash', '+': 'equal', '~': 'grave_accent',
           '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7',
           '*': '8', '(': '9', ')': '0', '{': 'bracket_left', '}': 'bracket_right'}
PLAIN = {' ': 'spc', '-': 'minus', '/': 'slash', '.': 'dot', '\n': 'ret', ';': 'semicolon',
         '=': 'equal', ',': 'comma', "'": 'apostrophe', '`': 'grave_accent',
         '\\': 'backslash', '[': 'bracket_left', ']': 'bracket_right', '\t': 'tab'}


def type_text(q, text):
    for ch in text:
        if ch.isalpha():
            keys = ['shift', ch.lower()] if ch.isupper() else [ch]
        elif ch.isdigit():
            keys = [ch]
        elif ch in PLAIN:
            keys = [PLAIN[ch]]
        else:
            keys = ['shift', SHIFTED[ch]]
        q("send-key", keys=[{"type": "qcode", "data": k} for k in keys], **{"hold-time": 40})
        time.sleep(0.06)


def shot(q, path):
    q("screendump", filename=os.path.abspath(path), format="png")
    time.sleep(0.6)
    return open(path, "rb").read() if os.path.exists(path) else b""


def start(gui=False):
    need_disk()
    boxes = find_mailbox(b"IN"), find_mailbox(b"OU")
    os.makedirs(STATE, exist_ok=True)
    if os.path.exists(QMP):
        os.remove(QMP)
    backend = os.environ.get("GPU_BACKEND", "auto")
    args = [QEMU, "-M", "mac99,via=pmu", "-cpu", "g4", "-m", "1024", "-smp", "1",
            "-display", "none", "-bios", BIOS, "-g", os.environ.get("RES", "1024x768x32"),
            "-drive", "file=%s,format=raw,media=disk" % DISK,
            "-device", "usb-tablet",
            "-device", "qgpu-pci,id=gpu0,backend=%s%s" % (
                backend, ",trace=on" if os.environ.get("GPU_TRACE") else ""),
            "-prom-env", "auto-boot?=true",
            "-prom-env", "boot-device=hd:10,\\System\\Library\\CoreServices\\BootX",
            "-prom-env", "boot-args=%s" % ("-v" if gui else "-v -s"),
            "-qmp", "unix:%s,server=on,wait=off" % QMP]
    log = open(os.path.join(STATE, "qemu.log"), "wb")
    p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                         start_new_session=True)
    open(os.path.join(STATE, "qemu.pid"), "w").write(str(p.pid))
    time.sleep(3)
    q = Qmp()
    if gui:
        # bureau : l'agent part tout seul (StartupItem) ; on attend le bureau
        # stable puis on vérifie que l'agent répond par un job vide.
        time.sleep(60)
        prev, same, n = None, time.time(), 0
        while n < 90:
            s = shot(q, os.path.join(STATE, "boot.png")); n += 1
            if s == prev and time.time() - same >= 20:
                break
            if s != prev:
                prev, same = s, time.time()
            time.sleep(5)
        print("bureau stable (pid %d) ; capture %s/boot.png" % (p.pid, STATE))
        return
    time.sleep(35)
    prev, same, n = None, time.time(), 0
    while True:
        s = shot(q, os.path.join(STATE, "boot.png")); n += 1
        if s == prev and time.time() - same >= 15:
            break
        if s != prev:
            prev, same = s, time.time()
        if n > 60:
            sys.exit("écran jamais stable : voir %s/boot.png" % STATE)
        time.sleep(4)
    type_text(q, "mount -uw /\n"); time.sleep(4)
    type_text(q, "sh /pomppc/agent.sh %d %d\n" % boxes)
    time.sleep(5)
    shot(q, os.path.join(STATE, "agent.png"))
    print("VM prête (pid %d), agent lancé ; capture %s/agent.png" % (p.pid, STATE))


def run(folder, timeout):
    need_disk()
    inbox, outbox = find_mailbox(b"IN"), find_mailbox(b"OU")
    # Le bash 2.05 de Tiger déclare « binaire » un script dont la PREMIÈRE ligne
    # contient un octet non ASCII (vu en vrai : « cannot execute binary file »).
    js = open(os.path.join(folder, "job.sh"), "rb").read()
    # guilib.sh (gui_run) est toujours livré avec le job
    extra = [("guilib.sh", open(os.path.join(ROOT, "tools", "guest", "guilib.sh"), "rb").read())]
    if not js.startswith(b"#!"):
        js = b"#!/bin/sh\n" + js
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for name in sorted(os.listdir(folder)):
            if name == "job.sh":
                ti = tarfile.TarInfo("job.sh"); ti.size = len(js); ti.mode = 0o755
                t.addfile(ti, io.BytesIO(js))
            else:
                t.add(os.path.join(folder, name), arcname=name)
        for name, data in extra:
            ti = tarfile.TarInfo(name); ti.size = len(data); ti.mode = 0o644
            t.addfile(ti, io.BytesIO(data))
    data = buf.getvalue()
    n = (len(data) + 511) // 512
    if n + 1 > MBX_SECTORS:
        sys.exit("job trop gros")
    jid = "%d" % int(time.time() * 1000)
    with open(DISK, "r+b") as f:
        f.seek((inbox + 1) * 512); f.write(data + b"\0" * (n * 512 - len(data)))
        f.seek(inbox * 512); f.write(("JOB %s %d\n" % (jid, n)).encode().ljust(512, b"\0"))
        f.flush(); os.fsync(f.fileno())
    t0 = time.time()
    while time.time() - t0 < timeout:
        time.sleep(2)
        with open(DISK, "rb") as f:
            f.seek(outbox * 512)
            hdr = f.read(512).split(b"\0")[0].decode("latin-1").split()
            if len(hdr) == 3 and hdr[0] == "OUT" and hdr[1] == jid:
                m = int(hdr[2])
                f.seek((outbox + 1) * 512)
                tar = f.read(m * 512)
                dest = os.path.join(STATE, "jobs", jid)
                os.makedirs(dest, exist_ok=True)
                with tarfile.open(fileobj=io.BytesIO(tar)) as t:
                    t.extractall(dest, filter="data") if sys.version_info >= (3, 12) else t.extractall(dest)
                last = os.path.join(STATE, "last")
                if os.path.islink(last) or os.path.exists(last):
                    os.remove(last)
                os.symlink(dest, last)
                log = os.path.join(dest, "out", "log.txt")
                print(open(log, errors="replace").read() if os.path.exists(log) else "(pas de log)")
                print("→ %s/out" % dest)
                return 0
    q = Qmp()
    shot(q, os.path.join(STATE, "timeout.png"))
    print("TIMEOUT après %d s ; capture %s/timeout.png" % (timeout, STATE))
    return 1


def stop():
    try:
        q = Qmp(); q("quit")
    except OSError:
        pass
    pidf = os.path.join(STATE, "qemu.pid")
    if os.path.exists(pidf):
        try:
            os.kill(int(open(pidf).read()), 15)
        except OSError:
            pass
        os.remove(pidf)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return 1
    if a[0] == "prepare":
        prepare()
    elif a[0] == "start":
        start(gui="--gui" in a)
    elif a[0] == "run":
        to = int(a[a.index("--timeout") + 1]) if "--timeout" in a else 900
        return run(a[1], to)
    elif a[0] == "shot":
        p = a[1] if len(a) > 1 else os.path.join(STATE, "shot.png")
        shot(Qmp(), p); print(p)
    elif a[0] == "type":
        type_text(Qmp(), a[1].encode().decode("unicode_escape"))
    elif a[0] == "stop":
        stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
