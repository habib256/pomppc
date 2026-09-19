#!/usr/bin/env python3
"""devloop.py — boucle de développement rapide dans l'invité Tiger.

Une VM Tiger reste allumée (single-user, headless) avec l'agent
tools/guest/agent.sh ; chaque « job » est un dossier contenant job.sh (et ce
qu'il faut compiler), transmis par une boîte aux lettres sur le disque brut.
Pas de reboot, pas de conversion d'image : une itération = le temps du job.

    devloop.py prepare            # (VM arrêtée) installe agent + boîtes aux lettres :
                                  # par hdiutil sur macOS, DANS l'invité ailleurs
    devloop.py start              # boote la VM en single-user et lance l'agent (frappe)
    devloop.py start --gui        # boote le bureau : agent lancé par un StartupItem,
                                  # jobs graphiques relayés par POMPPCGuiRunner (gui.sh)
    devloop.py run DOSSIER [--timeout S]   # exécute DOSSIER/job.sh, rapatrie out/
    devloop.py shot [FICHIER.png] # capture d'écran
    devloop.py type "texte\\n"     # frappe au clavier
    devloop.py click X Y [LxH]    # clic souris (LxH = résolution courante)
    devloop.py shutdown [--gui]   # ARRÊT SÛR (single-user, ou bureau) : voir plus bas
    devloop.py stop               # coupe le courant — n'utiliser qu'en dernier recours

Disque : $DEVDISK (raw), par défaut le tiger-dev.raw du scratchpad n'est pas
connu du dépôt : il faut le passer explicitement. CDROM=image[:image…] ajoute
des lecteurs en lecture seule au démarrage (le DVD de Tiger pour installer les
Xcode Tools, par exemple).

Variables d'environnement :
    QEMU_BIN   binaire QEMU (défaut ~/src/qemu/build/qemu-system-ppc ; en SMP>1
               le même nom suffixé « 64 »)
    CPU_OPTS   options ajoutées au modèle de CPU. `-cpu g4` devient
               `-cpu g4,$CPU_OPTS` — p. ex. CPU_OPTS=x-fast-fp=on pour le mode
               « flottant rapide ». Vide (défaut) = `-cpu g4` inchangé.
    DEVDISK, CDROM, SMP, SND, RES, NET, GPU_BACKEND, GPU_TRACE, GUI_USER

Rejouer un banc avec un autre binaire et un autre mode de CPU :

    export DEVDISK=disks/tiger-dev.raw
    QEMU_BIN=~/src/qemu-fastfp/build/qemu-system-ppc CPU_OPTS=x-fast-fp=on \\
        python3 tools/guest/devloop.py start
    tools/guest/jobs/stage.sh fpbench /tmp/j
    python3 tools/guest/devloop.py run /tmp/j --timeout 900
    python3 tools/guest/devloop.py shutdown

ARRÊT : `stop` coupe la VM comme une panne de courant. Fait pendant un job, ou
racine montée en écriture sans `sync`, il ABÎME le HFS+ (« blocks on volume not
allocated », Input/output error). `shutdown` fait la séquence sûre en
single-user : Ctrl-C à l'agent, `sync`, `mount -ur /`, `sync`, fermeture de la
connexion QMP, puis `halt` — qui démonte les volumes et éteint la VM lui-même
(`stop` n'est appelé que si `halt` n'a rien donné). Vérifié : après un
`shutdown`, `/sbin/fsck -fy` au démarrage suivant dit « appears to be OK ».
En mode BUREAU il n'y a pas d'invite où taper : `shutdown --gui` passe par
l'agent (job minuscule qui lance `shutdown -h now` en tâche de fond et rend la
main tout de suite, sinon `run` attendrait son délai entier).
Réparation après un arrêt brutal : démarrer en
single-user et, AVANT `mount -uw /`, `/sbin/fsck -fy`, `reboot`, puis `fsck` de
nouveau jusqu'à « appears to be OK ».
"""
import json, os, shutil, socket, struct, subprocess, sys, tarfile, tempfile, io, time

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
        # l'agent lit les secteurs sec+1.. en brut : la boîte DOIT être contiguë
        with open(DISK, "rb") as f:
            f.seek(sec * 512)
            data = f.read(MBX_SECTORS * 512)
        for k in range(0, MBX_SECTORS, 97):
            blk = data[k * 512:k * 512 + 18]
            if blk[:6] != MAGIC + tag or struct.unpack(">I", blk[6:10])[0] != k or blk[10:18] != nonce:
                raise SystemExit("boîte %s fragmentée (secteur %d) : refaire prepare" % (tag, k))
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
    if not shutil.which("hdiutil"):
        return prepare_guest()
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


SETUP_SH = r"""#!/bin/sh
# setup.sh — préparation de la VM de dev PAR L'INVITÉ (devloop.py prepare sans
# hdiutil). Lancé à la main (frappe) en single-user, racine, / monté en écriture.
#   sh /cd/setup.sh /cd <nonce en hexadécimal> <utilisateur de session>
CD=$1; HEX=$2; U=$3
echo "setup: début"
mkdir -p /pomppc
cp "$CD/agent.sh" /pomppc/agent.sh
rm -rf /Library/StartupItems/POMPPCAgent /Applications/POMPPCGuiRunner.app
mkdir -p /Library/StartupItems
cp -R "$CD/POMPPCAgent" /Library/StartupItems/POMPPCAgent
cp -R "$CD/POMPPCGuiRunner.app" /Applications/POMPPCGuiRunner.app
chown -R root:wheel /pomppc /Library/StartupItems/POMPPCAgent /Applications/POMPPCGuiRunner.app
chmod -R 755 /Library/StartupItems/POMPPCAgent /Applications/POMPPCGuiRunner.app
if [ -f "$CD/loginwindow.plist" ]; then
  cp "$CD/loginwindow.plist" /Users/$U/Library/Preferences/loginwindow.plist
  chown $(stat -f %%u /Users/$U) /Users/$U/Library/Preferences/loginwindow.plist
fi
# Boîtes aux lettres : 512 octets par secteur, PMBX + étiquette + index + nonce.
# L'agent et l'hôte les lisent en secteurs bruts CONSÉCUTIFS : chaque boîte est
# préallouée d'un seul tenant (fcntl F_PREALLOCATE = 42, F_ALLOCATECONTIG |
# F_ALLOCATEALL, F_PEOFPOSMODE), sinon HFS+ la découpe (vu : un morceau à 6 Mio).
for t in IN OU; do
  f=/pomppc/mbx-in.bin; [ $t = OU ] && f=/pomppc/mbx-out.bin
  rm -f $f
  perl -e 'my ($t, $n, $h, $f) = @ARGV; my $x = pack("H*", $h); my $z = "\0" x 494;
           open(F, ">", $f) or die "$f : $!";
           fcntl(F, 42, pack("LlNNNNNN", 6, 3, 0, 0, 0, $n * 512, 0, 0))
             or die "$f : préallocation contiguë refusée ($!)\n";
           for my $k (0 .. $n - 1) { print F "PMBX", $t, pack("N", $k), $x, $z }
           close(F) or die "$f : $!";' \
       $t %(sectors)d $HEX $f || echo "setup: ÉCHEC boîte $t"
done
sync; sync
echo "setup: fini"
"""


def vm_args(gui, cdroms=()):
    """Ligne de commande QEMU de la VM de dev (single-user sauf `gui`)."""
    smp = int(os.environ.get("SMP", "1"))
    qemu = QEMU + "64" if smp > 1 else QEMU
    extra = ["-accel", "tcg,thread=multi"] if smp > 1 else []
    ram = "1024"
    snd = os.environ.get("SND", "")
    if snd in ("1", "none"):
        drv = ("coreaudio" if sys.platform == "darwin" else "pa") if snd == "1" else "none"
        extra += ["-audiodev", "%s,id=snd0" % drv, "-global", "screamer.audiodev=snd0"]
        ram = "768"
    for cd in cdroms:
        extra += ["-drive", "file=%s,format=raw,media=cdrom,readonly=on" % cd]
    backend = os.environ.get("GPU_BACKEND", "auto")
    # CPU_OPTS s'ajoute au modèle : CPU_OPTS=x-fast-fp=on → -cpu g4,x-fast-fp=on
    cpu = "g4"
    if os.environ.get("CPU_OPTS"):
        cpu += "," + os.environ["CPU_OPTS"].lstrip(",")
    return [qemu, "-M", "mac99,via=pmu", "-cpu", cpu, "-m", ram, "-smp", str(smp),
            *extra,
            "-display", "none", "-bios", BIOS,
            "-g", os.environ.get("RES", "1024x768x32"),
            "-drive", "file=%s,format=raw,media=disk" % DISK,
            "-device", "usb-tablet",
            *(["-netdev", "user,id=net0", "-device", "sungem,netdev=net0"]
              if os.environ.get("NET") == "1" else ["-nic", "none"]),
            "-device", "qgpu-pci,id=gpu0,backend=%s%s" % (
                backend, ",trace=on" if os.environ.get("GPU_TRACE") else ""),
            "-prom-env", "auto-boot?=true",
            "-prom-env", "boot-device=hd:10,\\System\\Library\\CoreServices\\BootX",
            "-prom-env", "boot-args=%s" % ("-v" if gui else "-v -s"),
            "-qmp", "unix:%s,server=on,wait=off" % QMP]


def boot_vm(gui, cdroms=()):
    os.makedirs(STATE, exist_ok=True)
    if os.path.exists(QMP):
        os.remove(QMP)
    log = open(os.path.join(STATE, "qemu.log"), "wb")
    p = subprocess.Popen(vm_args(gui, cdroms), stdin=subprocess.DEVNULL, stdout=log,
                         stderr=log, start_new_session=True)
    open(os.path.join(STATE, "qemu.pid"), "w").write(str(p.pid))
    time.sleep(3)
    return p, Qmp()


def wait_stable(q, first=35, still=15, name="boot.png"):
    """Attend un écran immobile `still` secondes (single-user : l'invite)."""
    time.sleep(first)
    prev, same, n = None, time.time(), 0
    while True:
        s = shot(q, os.path.join(STATE, name)); n += 1
        if s == prev and time.time() - same >= still:
            return
        if s != prev:
            prev, same = s, time.time()
        if n > 90:
            sys.exit("écran jamais stable : voir %s/%s" % (STATE, name))
        time.sleep(4)


def prepare_guest():
    """Sans hdiutil (Linux) : c'est l'INVITÉ qui se prépare. Démarrage en
    single-user avec un CD (agent, StartupItem, relais de session, setup.sh,
    loginwindow.plist complété ici) ; l'invité recopie tout et crée lui-même ses
    boîtes aux lettres, marquées d'un nonce ; l'hôte les retrouve dans l'image,
    fait écrire agent.conf, puis arrête la VM. Aucune écriture HFS+ côté hôte."""
    import plistlib
    os.makedirs(STATE, exist_ok=True)
    nonce = os.urandom(8)
    stage = tempfile.mkdtemp(prefix="devloop-prep-")
    try:
        shutil.copy(os.path.join(ROOT, "tools", "guest", "agent.sh"), stage)
        shutil.copytree(os.path.join(ROOT, "tools", "guest", "POMPPCAgent"),
                        os.path.join(stage, "POMPPCAgent"))
        shutil.copytree(os.path.join(ROOT, "tools", "guest", "POMPPCGuiRunner.app"),
                        os.path.join(stage, "POMPPCGuiRunner.app"))
        with open(os.path.join(stage, "setup.sh"), "w") as f:
            f.write(SETUP_SH % {"sectors": MBX_SECTORS})
        lw = loginwindow_plist()
        if lw is not None:
            items = lw.setdefault("AutoLaunchedApplicationDictionary", [])
            if not any(i.get("Path") == "/Applications/POMPPCGuiRunner.app" for i in items):
                items.append({"Hide": False, "Path": "/Applications/POMPPCGuiRunner.app"})
            with open(os.path.join(stage, "loginwindow.plist"), "wb") as f:
                plistlib.dump(lw, f, fmt=plistlib.FMT_XML)
        else:
            print("⚠ loginwindow.plist illisible : le relais graphique ne démarrera pas")
        iso = os.path.join(STATE, "prepare.iso")
        subprocess.check_call(["xorriso", "-as", "mkisofs", "-quiet", "-R", "-J",
                               "-V", "POMPPCPREP", "-o", iso, stage])
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    p, q = boot_vm(False, [iso])
    try:
        boxes = prepare_in_guest(p, q, nonce)
    except BaseException:
        stop()                      # ne jamais laisser une VM orpheline
        raise
    boxes["mtime_disk_prepared"] = time.time()
    json.dump(boxes, open(os.path.join(STATE, "mailbox.json"), "w"))
    print("boîtes : inbox secteur %d, outbox secteur %d ; capture %s/prepare.png"
          % (boxes["IN"], boxes["OU"], STATE))


def prepare_in_guest(p, q, nonce):
    wait_stable(q)
    type_text(q, "mount -uw /\n"); time.sleep(4)
    # le CD d'un lecteur IDE de mac99 : /dev/diskNs0 (la tranche de session) ;
    # mount_cd9660 sur /dev/diskN rend « Invalid argument » (vu en vrai)
    type_text(q, "mkdir -p /cd; for d in 1 2 3 4; do mount_cd9660 /dev/disk${d}s0 /cd 2>/dev/null "
                 "&& break; done; sh /cd/setup.sh /cd %s %s\n" % (nonce.hex(), GUI_USER))
    # perl écrit 2 × 32 Mio : on attend que l'écran se fige de nouveau
    wait_stable(q, first=20, still=12, name="prepare.png")
    boxes = scan_mailboxes(nonce)
    type_text(q, "echo %d %d > /pomppc/agent.conf; chown root:wheel /pomppc/agent.conf; "
                 "sync; sync\n" % (boxes["IN"], boxes["OU"]))
    time.sleep(6)
    shot(q, os.path.join(STATE, "prepare.png"))
    type_text(q, "umount /cd; halt\n")
    try:
        p.wait(timeout=60)
    except subprocess.TimeoutExpired:
        stop()
    return boxes


def loginwindow_plist():
    """loginwindow.plist de l'utilisateur de session, LU dans l'image par 7z
    (HFS+ embarqué dans une enveloppe HFS : décalages calculés ici)."""
    import plistlib
    if not shutil.which("7z"):
        return None
    tmp = tempfile.mkdtemp(prefix="devloop-lw-")
    try:
        with open(DISK, "rb") as f:
            f.seek(512)
            n = struct.unpack(">I", f.read(512)[4:8])[0]
            part = None
            for i in range(n):
                f.seek(512 * (1 + i))
                e = f.read(512)
                if e[48:80].split(b"\0")[0] == b"Apple_HFS":
                    part = struct.unpack(">II", e[8:16])
            if not part:
                return None
            base = part[0] * 512
            f.seek(base + 1024)
            m = f.read(162)
            if m[:2] == b"BD" and m[0x7C:0x7E] == b"H+":        # enveloppe HFS
                alblk = struct.unpack(">I", m[0x14:0x18])[0]
                alst = struct.unpack(">H", m[0x1C:0x1E])[0]
                sb, bc = struct.unpack(">HH", m[0x7E:0x82])
                base += alst * 512 + sb * alblk
                size = bc * alblk
            else:
                size = part[1] * 512
        img = os.path.join(tmp, "vol.hfs")
        # copie creuse du seul volume HFS+ (7z ne lit pas l'enveloppe)
        subprocess.check_call(["dd", "if=" + DISK, "of=" + img, "bs=4M", "conv=sparse",
                               "iflag=skip_bytes,count_bytes", "skip=%d" % base,
                               "count=%d" % size, "status=none"])
        rel = "Users/%s/Library/Preferences/loginwindow.plist" % GUI_USER
        subprocess.call(["7z", "x", "-y", "-o" + tmp, img, "*/" + rel],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for root, _, files in os.walk(tmp):
            if "loginwindow.plist" in files and root.endswith("Preferences"):
                return plistlib.load(open(os.path.join(root, "loginwindow.plist"), "rb"))
        return None
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


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


def click(q, x, y, screen=(1024, 768)):
    """Clic gauche en (x, y) pixels d'écran, par la tablette USB (absolue)."""
    ax = int(x * 32767 / (screen[0] - 1)); ay = int(y * 32767 / (screen[1] - 1))
    q("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}}])
    time.sleep(0.2)
    for down in (True, False):
        q("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
        time.sleep(0.12)


def start(gui=False):
    need_disk()
    boxes = find_mailbox(b"IN"), find_mailbox(b"OU")
    cds = [c for c in os.environ.get("CDROM", "").split(":") if c]
    p, q = boot_vm(gui, cds)
    if gui:
        # bureau : l'agent part tout seul (StartupItem) ; on attend le bureau
        # stable puis on vérifie que l'agent répond par un job vide.
        wait_stable(q, first=60, still=20)
        print("bureau stable (pid %d) ; capture %s/boot.png" % (p.pid, STATE))
        return
    wait_stable(q)
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


def wait_gone(pid, halt_timeout, how):
    """Attend que le processus QEMU disparaisse de lui-même ; `stop()` en repli."""
    pidf = os.path.join(STATE, "qemu.pid")
    t0 = time.time()
    while pid and time.time() - t0 < halt_timeout:
        try:
            os.kill(pid, 0)
        except OSError:
            if os.path.exists(pidf):
                os.remove(pidf)
            print("VM arrêtée par %s en %d s (volume démonté proprement)" % (how, time.time() - t0))
            return True
        time.sleep(2)
    print("%s n'a pas éteint la VM en %d s : extinction forcée" % (how, halt_timeout))
    stop()
    return False


def shutdown_gui(halt_timeout=180):
    """ARRÊT SÛR d'une VM lancée en mode BUREAU (`start --gui`).

    Il n'y a pas d'invite single-user où taper : on passe par l'agent, avec un
    job minuscule qui demande l'extinction EN TÂCHE DE FOND puis rend la main —
    `shutdown -h now` tue l'agent, un job qui l'appellerait au premier plan ne
    rendrait jamais son résultat et `run` attendrait son délai entier.
    """
    pidf = os.path.join(STATE, "qemu.pid")
    pid = int(open(pidf).read()) if os.path.exists(pidf) else None
    d = tempfile.mkdtemp(prefix="devloop-halt-")
    try:
        with open(os.path.join(d, "job.sh"), "w") as f:
            f.write("#!/bin/sh\nsync; sync\n"
                    "( sleep 5; sync; sync; /sbin/shutdown -h now ) &\n"
                    "echo 'extinction demandee dans 5 s'\n")
        run(d, 120)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    wait_gone(pid, halt_timeout, "`shutdown -h now`")


def shutdown(wait=8, halt_timeout=120):
    """ARRÊT SÛR d'une VM lancée en single-user (mode bureau : `shutdown --gui`).

    `stop()` envoie `quit` à QEMU : c'est une coupure de courant. Fait pendant un
    job, ou racine montée en écriture sans `sync`, il ABÎME le HFS+ (vu en vrai :
    « blocks on volume not allocated », Input/output error ; réparation par
    `/sbin/fsck -fy` AVANT tout `mount -uw /`). La séquence sûre :

      1. Ctrl-C : l'agent rend la main au shell single-user (aucun job en cours —
         `run` est synchrone, il rend la main quand le job est fini) ;
      2. `sync` deux fois ;
      3. `mount -ur /` : la racine repasse en lecture seule. Échoue souvent en
         « mount_hfs: Resource busy » (un fichier reste ouvert en écriture) —
         ce n'est PAS bloquant, l'étape 4 fait le travail ;
      4. `halt` : Darwin synchronise et DÉMONTE les volumes, le journal HFS+ est
         refermé et le volume marqué propre. C'est ce qui remplace la coupure ;
         la VM s'éteint d'elle-même (mac99 via=pmu) ;
      5. FERMER la connexion QMP (une seule à la fois) et n'appeler `stop()` que
         si QEMU est encore là après `halt_timeout`.

    La capture bench/devloop/shutdown.png montre le déroulé.
    """
    pidf = os.path.join(STATE, "qemu.pid")
    pid = int(open(pidf).read()) if os.path.exists(pidf) else None
    q = Qmp()
    try:
        # Ctrl-C à l'agent : retour à l'invite du shell single-user
        q("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                            {"type": "qcode", "data": "c"}], **{"hold-time": 100})
        time.sleep(3)
        type_text(q, "\n")
        time.sleep(2)
        for cmd, pause in (("sync; sync\n", wait),
                           ("mount -ur /\n", wait),
                           ("sync; sync\n", wait),
                           ("mount\n", 4)):
            type_text(q, cmd)
            time.sleep(pause)
        shot(q, os.path.join(STATE, "shutdown.png"))
        type_text(q, "sync; sync; halt\n")
    finally:
        try:
            q.f.close(); q.s.close()
        except OSError:
            pass
    # `halt` démonte puis coupe l'alimentation : on lui laisse le temps
    wait_gone(pid, halt_timeout, "`halt`")


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
    elif a[0] == "click":
        # click X Y [LxH] : coordonnées dans la résolution d'écran courante
        scr = tuple(int(v) for v in a[3].split("x")) if len(a) > 3 else (1024, 768)
        click(Qmp(), int(a[1]), int(a[2]), scr)
    elif a[0] == "shutdown":
        if "--gui" in a:
            shutdown_gui()
        else:
            shutdown(int(a[a.index("--wait") + 1]) if "--wait" in a else 8)
    elif a[0] == "stop":
        stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
