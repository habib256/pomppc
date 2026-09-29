#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""kpanic.py — autopsie d'un invité Tiger gelé, depuis l'hôte, par le moniteur QEMU.

    python3 tools/re/kpanic.py MACH_KERNEL [SOCKET_MONITEUR]

MACH_KERNEL : copie sur l'hôte du /mach_kernel de l'invité
(`tools/guest/tssh.sh "cat /mach_kernel" > mach_kernel`, invité vivant).
Moniteur : .run/tiger.mon du dépôt principal par défaut.

Imprime, pour chaque vCPU, PC / LR / MSR(EE) symbolisés contre mach_kernel ;
puis, s'il y a eu panique : le format (`panicstr`), l'appelant
(`panic_caller`), le TEXTE reconstitué avec les arguments variables lus sur
la pile du CPU arrêté dans panic(), et sa chaîne d'appels ; enfin la fin du
journal du noyau (msgbuf).

Pourquoi (29/09/2026) : le « gel au chargement de DOOM 3 » (vCPU 0 bouclant
en 0x268b4, EE coupé) est `_panic+0x254`, le `b .` qui suit
« panic: We are hanging here... » : une PANIQUE, pas un interblocage. Sans
boot-arg de débogage, Tiger n'en garde AUCUNE trace : `debug_buf` n'est
alloué (debug_log_init) que par kdp_register_send_receive, donc Debugger()
ne fait pas PESavePanicInfo (pas de panic.log au démarrage suivant) ; le texte
part par kdb_printf vers la console seule, jamais dans le msgbuf. Il ne reste
que la mémoire de l'invité : `panicstr`, `panic_caller` et la pile de panic().
0xaf6b4 (l'autre vCPU) est `_machine_idle+0x194` : il est au repos.

Lectures : `xp` (physique) pour les variables du noyau, en V=R sur PPC ; `x`
par la MMU du CPU choisi (`cpu N`) pour les piles, hors V=R."""
import bisect, os, re, socket, subprocess, sys

ici = os.path.dirname(os.path.abspath(__file__))
_c = subprocess.run(["git", "-C", ici, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                    capture_output=True, text=True).stdout.strip()
ROOT = _c[:-5] if _c.endswith("/.git") else os.path.dirname(os.path.dirname(ici))


def moniteur(chemin):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(30)
    s.connect(chemin)

    def lire():
        b = b""
        while b"(qemu)" not in b:
            c = s.recv(65536)
            if not c:
                raise SystemExit("moniteur fermé")
            b += c
        return b
    lire()

    def cmd(c):
        s.sendall(c.encode() + b"\n")
        out = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", lire().decode("utf-8", "replace")).replace("\r", "")
        return "\n".join(l for l in out.split("\n")
                         if l.strip() and "(qemu)" not in l and not l.strip().endswith(c))
    # le moniteur HMP d'un socket UNIX ne sert qu'UN client : fermer avant d'en rouvrir
    cmd.close = s.close
    return cmd


def symboles(noyau):
    t, d = [], {}
    for l in subprocess.run(["nm", "-n", noyau], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 3:
            a = int(p[0], 16)
            d[p[2]] = a
            if p[1] in "tT":
                t.append((a, p[2]))
    return t, d


def nommer(t, a):
    i = bisect.bisect_right([x[0] for x in t], a) - 1
    if i < 0 or a - t[i][0] > 0x10000:
        return "?"
    return "%s+0x%x" % (t[i][1], a - t[i][0])


def mots(cmd, adr, n, virt=False):
    """n mots de 32 bits, en adresse physique (virt : par la MMU du CPU courant)."""
    out, r = cmd("%s /%dwx 0x%x" % ("x" if virt else "xp", n, adr)), []
    for l in out.splitlines():
        if re.match(r"^[0-9a-f]{8,16}: 0x", l):
            r += [int(v, 16) for v in l.split(":", 1)[1].split()]
    return r[:n]


def octets(cmd, adr, n, virt=False):
    r = bytearray()
    while n > 0:
        k = min(n, 1024)
        for l in cmd("%s /%dbx 0x%x" % ("x" if virt else "xp", k, adr)).splitlines():
            if re.match(r"^[0-9a-f]{8,16}: 0x", l):
                r += bytes(int(v, 16) for v in l.split(":", 1)[1].split())
        adr += k
        n -= k
    return bytes(r)


def formater(cmd, fmt, args):
    """printf du noyau, assez pour un message de panique (%s lu par la MMU)."""
    out, i = [], iter(args)

    def rep(m):
        conv = m.group(2)
        if conv == "%":
            return "%"
        v = next(i, 0)
        if conv == "s":
            b = bytearray()
            for w in mots(cmd, v, 32, virt=True):
                b += w.to_bytes(4, "big")
            return texte(bytes(b))
        drap = m.group(1).replace("l", "")
        if conv in "xX":
            return ("%" + drap + conv) % v
        if conv in "dui":
            return str(v - (1 << 32) if conv in "di" and v & 0x80000000 else v)
        if conv == "p":
            return "0x%08x" % v
        if conv == "c":
            return chr(v & 0xff)
        return "?"
    return re.sub(r"%([-#0-9.l]*)([sxXduipc%])", rep, fmt)


def kexts(cmd, d):
    """Liste des kexts chargés (chaîne `kmod` du noyau, kmod_info_t de
    xnu-792 : next +0, name +12, address +148, size +152), lue par la MMU du
    CPU courant du moniteur (qui doit être en mode noyau)."""
    r = []
    p = mots(cmd, d["_kmod"], 1) if "_kmod" in d else []
    p = p[0] if p else 0
    while p and len(r) < 300:
        b = octets(cmd, p, 168, virt=True)
        if len(b) < 168:
            break
        nxt = int.from_bytes(b[0:4], "big")
        nom = texte(b[12:76])
        adr = int.from_bytes(b[148:152], "big")
        taille = int.from_bytes(b[152:156], "big")
        r.append((adr, taille, nom))
        p = nxt
    return r


def nommer_kext(ks, a):
    for adr, taille, nom in ks:
        if adr <= a < adr + taille:
            return "%s+0x%x" % (nom, a - adr)
    return "?"


def texte(b):
    return b.split(b"\0")[0].decode("latin-1", "replace")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    noyau = sys.argv[1]
    try:
        mon = sys.argv[2] if len(sys.argv) > 2 else open(ROOT + "/.run/tiger.mon").read().strip()
    except OSError:
        mon = ROOT + "/.run/mon.sock"
    t, d = symboles(noyau)
    cmd = moniteur(mon)
    print(cmd("info status"))
    regs = cmd("info registers -a")
    cpus = []
    for bloc in re.split(r"\n(?=CPU#)", regs):
        m = re.search(r"CPU#(\d+)", bloc)
        pc = re.search(r"NIP\s+([0-9a-f]+)", bloc)
        lr = re.search(r"LR\s+([0-9a-f]+)", bloc)
        msr = re.search(r"MSR\s+([0-9a-f]+)", bloc)
        gpr = {}
        for g in re.finditer(r"GPR(\d\d)((?:\s+[0-9a-f]{8,16})+)", bloc):
            for k, v in enumerate(g.group(2).split()):
                gpr[int(g.group(1)) + k] = int(v, 16)
        if m and pc:
            p, l_, ms = int(pc.group(1), 16), int(lr.group(1), 16) if lr else 0, int(msr.group(1), 16) if msr else 0
            gpr["lr"] = l_
            cpus.append((int(m.group(1)), p, gpr))
            print("CPU %s : PC %08x %s | LR %08x %s | MSR %08x EE=%d PR=%d | r1 %08x"
                  % (m.group(1), p, nommer(t, p), l_, nommer(t, l_), ms, (ms >> 15) & 1,
                     (ms >> 14) & 1, gpr.get(1, 0)))
    hors = [(n, a) for n, p, g in cpus for a in (p, g.get("lr", 0)) if a and nommer(t, a) == "?"]
    if hors:
        ks = []
        for n, p, g in cpus:          # un CPU en mode noyau pour lire la chaîne kmod
            cmd("cpu %d" % n)
            ks = kexts(cmd, d)
            if ks:
                break
        cmd("cpu 0")
        for n, a in hors:
            print("  CPU %d : %08x = %s" % (n, a, nommer_kext(ks, a) if ks else "? (chaîne kmod illisible)"))
    fmt = None
    if "_panicstr" in d:
        p = mots(cmd, d["_panicstr"], 1)
        print("panicstr = %s" % ("%08x" % p[0] if p else "?"))
        if p and p[0]:
            fmt = texte(octets(cmd, p[0], 256))
            print("  format « %s »" % fmt.rstrip())
    if "_panic_caller" in d:
        c = mots(cmd, d["_panic_caller"], 1)
        if c and c[0]:
            print("panic_caller = %08x %s" % (c[0], nommer(t, c[0])))
    # CPU arrêté dans panic() : ses arguments variables (r4-r10 rangés à
    # r1+0x9c par le prologue, cadre de 0x80) et sa pile, par la MMU de CE CPU
    # (piles noyau hors V=R) : `cpu N` puis `x`.
    lo, hi = d.get("_panic", 0), d.get("_log", 0)
    for n, p, gpr in cpus:
        if not (lo <= p < hi) or 1 not in gpr:
            continue
        cmd("cpu %d" % n)
        r1 = gpr[1]
        args = mots(cmd, r1 + 0x9c, 7, virt=True)
        if not fmt and gpr.get(24):
            # panic() remet panicstr à 0 au retour de Debugger(), AVANT la
            # boucle finale ; le format reste dans r24 (mr r24,r3 au prologue)
            b = bytearray()
            for w in mots(cmd, gpr[24], 64, virt=True):
                b += w.to_bytes(4, "big")
            fmt = texte(bytes(b))
            print("  format (r24 = %08x) « %s »" % (gpr[24], fmt.rstrip()))
        print("CPU %d dans panic : arguments %s" % (n, " ".join("%08x" % a for a in args)))
        if fmt and args:
            print("  texte : %s" % formater(cmd, fmt, args).rstrip())
        print("  pile (chaîne arrière) :")
        f = r1
        for _ in range(24):
            w = mots(cmd, f, 3, virt=True)
            if len(w) < 3 or not w[0] or w[0] <= f or w[0] - f > 0x10000:
                break
            f = w[0]
            lr_ = mots(cmd, f + 8, 1, virt=True)
            if not lr_:
                break
            print("    %08x %s" % (lr_[0], nommer(t, lr_[0])))
        cmd("cpu 0")
    if "_debug_buf" in d and "_debug_buf_ptr" in d:
        base = mots(cmd, d["_debug_buf"], 1)
        fin = mots(cmd, d["_debug_buf_ptr"], 1)
        if base and fin and base[0] and fin[0] > base[0]:
            n = min(fin[0] - base[0], 4096)
            print("debug_buf (%d octets) :" % n)
            print(texte(octets(cmd, base[0], n)))
        else:
            print("debug_buf vide (%s, %s)" % (base, fin))
    if "_msgbufp" in d:
        mp = mots(cmd, d["_msgbufp"], 1)
        if mp and mp[0]:
            # Tiger : struct msgbuf { long magic, bufx, bufr; char bufc[MSG_BSIZE]; }
            # sur une page (MSG_BSIZE = 4096 - 3 × 4), tampon EN LIGNE à +0xc
            magic, bufx, bufr = mots(cmd, mp[0], 3)
            size, bufc = 4096 - 12, mp[0] + 12
            if magic == 0x63061 and bufx < size:
                b = octets(cmd, bufc, size)
                b = b[bufx:] + b[:bufx]          # du plus ancien au plus récent
                print("journal du noyau (msgbuf, %d octets, fin) :" % size)
                print(b.replace(b"\0", b"").decode("latin-1", "replace")[-3000:])
            else:
                print("msgbuf illisible (magic %x bufx %x)" % (magic, bufx))

if __name__ == "__main__":
    main()
