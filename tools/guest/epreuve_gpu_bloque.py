#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""epreuve_gpu_bloque.py — épreuves en VM « GPU hôte bloqué » (GL3, KT4, K6)
et A6 (L2 : créneau rendu malgré un nettoyage échoué), côté HÔTE.

Pilote une VM de dev (tools/guest/devloop.py, single-user) dont le device
qgpu-pci a les propriétés de test x-test-stall-ms et x-test-refuse-reset
(patches/qgpu/qgpu-pci.c, 14/10/2026), et le job invité tools/guest/jobs/stall.
Procédure et verdicts : docs/epreuve-gpu-bloque.md.

    export DEVDISK=disks/tiger-dev-stall.raw QEMU_BIN=~/src/qemu-stall/build/qemu-system-ppc
    export SND=none GPU_TRACE=1 QEMU_EXTRA="-device qfb-pci -global qgpu-pci.scanout=qfb"
    python3 tools/guest/devloop.py start
    tools/guest/epreuve_gpu_bloque.py prep [--old-rev REV]   # kexts, outils, témoin
    tools/guest/epreuve_gpu_bloque.py gl3                    # GL3, KT4, K6 (reset compris)
    tools/guest/epreuve_gpu_bloque.py a6 [--kext new|old]    # A6 : full, wait, late, refuse, again
    tools/guest/epreuve_gpu_bloque.py set stall 600000 | set refuse on | regs

Le journal de l'épreuve (horodaté) va sur la sortie et dans --log ; celui de
QEMU est bench/devloop/qemu.log (lignes « qgpu-pci: »).
"""
import argparse, json, os, re, shutil, socket, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
STATE = os.path.join(ROOT, "bench", "devloop")
QMP = os.path.join(STATE, "qmp.sock")
DEVLOOP = os.path.join(HERE, "devloop.py")
GPU = "/machine/peripheral/gpu0"         # id=gpu0 posé par devloop.py
REG_CAPS, REG_CLIENTS, REG_FENCE, REG_DOORBELL = 0x08, 0x80, 0x1C, 0x18
LOG = [None]


def say(*a):
    line = "%s %s" % (time.strftime("%H:%M:%S"), " ".join(str(x) for x in a))
    print(line, flush=True)
    if LOG[0]:
        with open(LOG[0], "a") as f:
            f.write(line + "\n")


class Qmp:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(QMP)
        self.f = self.s.makefile("rwb")
        self.f.readline()
        self("qmp_capabilities")

    def __call__(self, cmd, **args):
        self.f.write((json.dumps({"execute": cmd, "arguments": args}) + "\n").encode())
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r

    def close(self):
        self.s.close()


def qmp(cmd, **args):
    q = Qmp()
    try:
        return q(cmd, **args)
    finally:
        q.close()


def hmp(line):
    return qmp("human-monitor-command", **{"command-line": line}).get("return", "")


def qom_set(prop, value):
    r = qmp("qom-set", path=GPU, property=prop, value=value)
    if "error" in r:
        sys.exit("qom-set %s : %s" % (prop, r["error"]))
    say("qom-set %s = %s" % (prop, value))


def bar1():
    """Adresse de BAR1 (registres) du qgpu-pci (1234:0fb2), par `info pci`."""
    txt = hmp("info pci")
    blocks = txt.split("Bus ")
    for b in blocks:
        if "1234:0fb2" in b or "PCI device 1234:0fb2" in b:
            m = re.search(r"BAR1: 32 bit memory at (0x[0-9a-f]+)", b)
            if m:
                return int(m.group(1), 16)
    return None


def regs(label=""):
    base = bar1()
    if base is None:
        say("regs%s : BAR1 introuvable (BAR non programmé ?)" % label)
        return {}
    out = {}
    for name, off in (("CAPS", REG_CAPS), ("CLIENTS", REG_CLIENTS), ("FENCE", REG_FENCE),
                      ("EN_FILE", REG_DOORBELL)):
        txt = hmp("xp /1wx 0x%x" % (base + off))
        m = re.search(r":\s*(0x[0-9a-f]+)", txt)
        out[name] = int(m.group(1), 16) if m else None
    say("registres%s (BAR1 0x%x) : %s" % (label, base, ", ".join(
        "%s=%s" % (k, ("0x%x" % v) if v is not None else "?") for k, v in out.items())))
    return out


def qemu_log_mark():
    p = os.path.join(STATE, "qemu.log")
    return os.path.getsize(p) if os.path.exists(p) else 0


def qemu_log_since(mark, pat=r"qgpu-pci: (?!soumission|reg\[)|warning"):
    p = os.path.join(STATE, "qemu.log")
    with open(p, "rb") as f:
        f.seek(mark)
        lines = f.read().decode("utf-8", "replace").splitlines()
    return [l for l in lines if re.search(pat, l)]


def show_qemu(mark, n=40):
    for l in qemu_log_since(mark)[-n:]:
        say("  qemu| " + l)


def stage(phase, variant="", kext="", old_rev=None):
    d = os.path.join(STATE, "stall-job")
    subprocess.check_call(["sh", os.path.join(ROOT, "tools", "guest", "jobs", "stage.sh"),
                           "stall", d], stdout=subprocess.DEVNULL)
    for name, val in (("phase", phase), ("variant", variant), ("kext", kext)):
        if val:
            open(os.path.join(d, name), "w").write(val + "\n")
    # l'écran de l'épreuve K6 (stage.sh ne copie que POMPPCGPU)
    shutil.copytree(os.path.join(ROOT, "kext", "POMPPCQFB"),
                    os.path.join(d, "src", "kext", "POMPPCQFB"))
    if old_rev:
        old = os.path.join(d, "src-old")
        os.makedirs(old, exist_ok=True)
        arch = subprocess.run(["git", "-C", ROOT, "archive", old_rev, "kext/POMPPCGPU"],
                              check=True, capture_output=True).stdout
        subprocess.run(["tar", "-x", "-C", old], input=arch, check=True)
    return d


def run_job(phase, variant="", kext="", old_rev=None, timeout=600):
    d = stage(phase, variant, kext, old_rev)
    say("job %s %s %s" % (phase, variant, kext))
    r = subprocess.run([sys.executable, DEVLOOP, "run", d, "--timeout", str(timeout)],
                       capture_output=True, text=True)
    for l in r.stdout.splitlines():
        say("  invité| " + l)
    # Verdict : la dernière ligne « stall PHASE : N echec(s) » du job (un agent
    # d'avant le code de sortie rend toujours 0 à devloop.py run).
    m = re.findall(r"stall %s : (\d+) echec" % phase, r.stdout)
    rc = int(m[-1]) if m else (r.returncode or 99)
    say("job %s %s %s : %d échec(s)%s" % (phase, variant, kext, rc,
                                         "" if m else " (pas de verdict du job)"))
    return rc


def wait_single_user(timeout=240):
    """Après un system_reset : attendre l'invite single-user puis relancer
    l'agent (même frappe que devloop.py start)."""
    boxes = json.load(open(os.path.join(STATE, "mailbox.json")))
    time.sleep(60)
    sys.path.insert(0, HERE)
    import devloop                                  # noqa: E402  (QMP, frappe)
    q = devloop.Qmp()
    try:
        devloop.wait_stable(q, first=0, still=15, name="reboot.png")
        devloop.type_text(q, "mount -uw /\n")
        time.sleep(4)
        devloop.type_text(q, "sh /pomppc/agent.sh %d %d\n" % (boxes["IN"], boxes["OU"]))
        time.sleep(5)
    finally:
        q.s.close()
    say("invité redémarré en single-user, agent relancé")


class Sampler(threading.Thread):
    """Latence du moniteur (query-status, toutes les 50 ms) pendant un job :
    un BQL tenu longtemps (KT4 : 2 s par écriture de géométrie) s'y voit.
    Seule connexion QMP pendant ce temps (le serveur n'en sert qu'une)."""
    def __init__(self):
        super().__init__(daemon=True)
        self.stop, self.worst, self.n, self.at = False, 0.0, 0, None
        self.slow = []          # (heure, latence) des réponses de plus de 0,25 s
        self.send = None        # commande à passer par CETTE connexion

    def run(self):
        q = Qmp()
        try:
            while not self.stop:
                cmd, self.send = self.send, None
                t0 = time.time()
                if cmd:
                    q(cmd)
                    say("%s envoyé (%.2f s)" % (cmd, time.time() - t0))
                    t0 = time.time()
                q("query-status")
                dt = time.time() - t0
                if dt > self.worst:
                    self.worst, self.at = dt, time.strftime("%H:%M:%S")
                if dt > 0.25:
                    self.slow.append((time.strftime("%H:%M:%S", time.localtime(t0)), dt))
                    say("moniteur : réponse en %.2f s (demandée à %s)" % (
                        dt, time.strftime("%H:%M:%S", time.localtime(t0))))
                self.n += 1
                time.sleep(0.05)
        finally:
            q.close()


def job_sampled(phase, **kw):
    smp = Sampler()
    smp.start()
    try:
        rc = run_job(phase, **kw)
    finally:
        smp.stop = True
        smp.join()
    say("moniteur pendant le job %s : %d réponses, pire latence %.2f s (à %s)"
        % (phase, smp.n, smp.worst, smp.at))
    return rc, smp.worst


def cmd_gl3(a):
    """1) GL3/KT4/K6 : rechargement du kext sous blocage, changement de mode
    de l'écran pendant la panne, déblocage, réparation par rechargement.
    2) GL3 : system_reset sous blocage, (changement de mode pendant la
    panne, sauf --sans-geom), déblocage, réparation. --part 1 ou 2 pour n'en
    jouer qu'une."""
    t0 = time.time()
    rc_block = rc_rep1 = rc_in = rc_geom = rc_rep2 = 0
    worst_block = worst_geom = 0.0
    if a.part in ("all", "1"):
        rc_block, worst_block, rc_rep1 = gl3_part1()
    if a.part in ("all", "2"):
        rc_in, rc_geom, worst_geom, rc_rep2 = gl3_part2(not a.sans_geom)
    say("gl3 : block %d, repair %d, inflight %d, geom %d, repair après reset %d échec(s) ; "
        "pire latence du moniteur : block %.2f s, geom %.2f s (%.0f s)"
        % (rc_block, rc_rep1, rc_in, rc_geom, rc_rep2, worst_block, worst_geom,
           time.time() - t0))
    return rc_block or rc_rep1 or rc_in or rc_geom or rc_rep2


def gl3_part1():
    m0 = qemu_log_mark()
    regs(" avant")
    qom_set("x-test-stall-ms", 600000)
    rc_block, worst_block = job_sampled("block")
    regs(" après rechargement du kext sous blocage")
    show_qemu(m0)
    m1 = qemu_log_mark()
    qom_set("x-test-stall-ms", 0)
    time.sleep(2)
    regs(" débloqué, avant tout reset")
    show_qemu(m1)
    m2 = qemu_log_mark()
    rc_rep1 = run_job("repair")
    regs(" après réparation par rechargement")
    show_qemu(m2)
    return rc_block, worst_block, rc_rep1


def gl3_part2(geom):
    rc_geom, worst_geom = 0, 0.0
    say("--- 2) system_reset sous blocage ---")
    qom_set("x-test-stall-ms", 600000)
    rc_in = run_job("inflight")
    regs(" job en vol")
    m3 = qemu_log_mark()
    say("system_reset, GPU toujours bloqué")
    smp = Sampler()
    smp.start()
    time.sleep(1)
    smp.send = "system_reset"
    time.sleep(15)
    smp.stop = True
    smp.join()
    say("system_reset : moniteur %d réponses en 15 s, pire latence %.2f s (à %s)"
        % (smp.n, smp.worst, smp.at))
    show_qemu(m3)
    wait_single_user()
    regs(" après redémarrage, toujours bloqué")
    if geom:
        m35 = qemu_log_mark()
        rc_geom, worst_geom = job_sampled("geom")
        regs(" après changement de mode de l'écran pendant la panne")
        show_qemu(m35)
    m4 = qemu_log_mark()
    qom_set("x-test-stall-ms", 0)
    time.sleep(2)
    regs(" débloqué, avant tout reset")
    show_qemu(m4)
    m5 = qemu_log_mark()
    rc_rep2 = run_job("repair")
    regs(" après réparation")
    show_qemu(m5)
    return rc_in, rc_geom, worst_geom, rc_rep2


def cmd_a6(a):
    rcs = {}
    for variant, stall, refuse in (("full", 1500, False), ("wait", 1500, False),
                                   ("late", 500, False), ("refuse", 0, True),
                                   ("again", 0, False)):
        if a.only and variant not in a.only.split(","):
            continue
        m = qemu_log_mark()
        qom_set("x-test-stall-ms", stall)
        qom_set("x-test-refuse-reset", refuse)
        rcs[variant] = run_job("a6", variant, a.kext)
        qom_set("x-test-stall-ms", 0)
        qom_set("x-test-refuse-reset", False)
        show_qemu(m, 30)
    say("a6 kext %s : %s" % (a.kext, ", ".join("%s rc=%d" % kv for kv in rcs.items())))
    return 1 if any(rcs.values()) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", help="journal de l'épreuve (ajouté)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prep"); p.add_argument("--old-rev")
    p = sub.add_parser("gl3"); p.add_argument("--part", default="all", choices=("all", "1", "2"))
    p.add_argument("--sans-geom", action="store_true",
                   help="partie 2 sans changement de mode pendant la panne")
    p = sub.add_parser("a6"); p.add_argument("--kext", default="new", choices=("new", "old"))
    p.add_argument("--only", help="variantes, séparées par des virgules")
    p = sub.add_parser("set"); p.add_argument("what", choices=("stall", "refuse"))
    p.add_argument("value")
    sub.add_parser("regs")
    a = ap.parse_args()
    LOG[0] = a.log
    if a.cmd == "prep":
        return run_job("prep", old_rev=a.old_rev, timeout=900)
    if a.cmd == "gl3":
        return cmd_gl3(a)
    if a.cmd == "a6":
        return cmd_a6(a)
    if a.cmd == "set":
        if a.what == "stall":
            qom_set("x-test-stall-ms", int(a.value))
        else:
            qom_set("x-test-refuse-reset", a.value in ("1", "on", "true"))
        return 0
    if a.cmd == "regs":
        regs()
        return 0


if __name__ == "__main__":
    sys.exit(main())
