#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""codeanat32.py JOURNAL PERF.DATA [--top N] [--dump PC,PC…] — anatomie du code x86-64 que TCG
émet pour le G4 sur la cible 32 bits (qemu-system-ppc, docs/anatomie-ppc32.md), pondérée par
les instructions hôte RÉELLEMENT retirées.

JOURNAL : `-d in_asm,out_asm -D …` du même démarrage (formes OBJD-T / OBJD-H, adresses hôte
imprimées par disas()). PERF.DATA : `perf record -e instructions:u[,cycles:u] -c P -p <qemu>`
(tools/tcg/ppc32mt-bench.sh, PERFREC=…). Chaque échantillon est rangé dans l'instruction hôte
exacte qui l'a pris (adresse IP → bloc du journal → instruction), donc la part d'un poste est
une part d'instructions hôte retirées (« pondérées »), pas de code statique.

Postes (par instruction hôte, d'après le code émis réel) :

  entete     tête du bloc (« tb prologue ») : vérification d'interruption (icount_decr),
             et les rangements de can_do_io (début et dernière instruction du bloc)
  ae         adresse effective d'un chargement/rangement (avant le TLB, hors env)
  tlb        chemin rapide du TLB logiciel : mov/shr/and masque/add table/lea/and/cmp/jne/
             mov addend (le jne vers le chemin lent compris)
  acces      l'accès à la mémoire de l'invité (movbe, mov, movzbl… ; movbe = accès ET
             inversion)
  bswap      inversion d'octets EN PLUS de l'accès : bswap, rol $8 (16 bits), xor de mise à
             zéro avant un movbe 16 bits, extension après un movbe 16 bits
  env-ld     lecture d'un registre invité dans CPUPPCState (rbp ≥ 0)
  env-st     écriture d'un registre invité dans CPUPPCState
  cr         calcul d'un champ du CR (cmp*, forme « . » : test/cmp/cmov/set/or après l'op)
  xer        calcul de XER[CA] (addc, adde, subfc, addic, srawi…) au-delà de l'op elle-même
  bc-test    test de la condition d'un bc/bclr/bcctr (and/test/cmp sur le CR, CTR--)
  saut       jmp/jcc/ret, sorties directes de bloc (goto_tb, exit_tb)
  sortie-ind sortie indirecte d'un bclr/bcctr : recherche en ligne du cache de sauts
             (x-ret-inline, x-jc-*), lookup_and_goto_ptr
  appel      call vers un helper et préparation des arguments
  barriere   mfence, lock (stwcx. en cmpxchg, sync)
  neg        autre accès rbp < 0 (CPUState hors TLB)
  calcul     le reste (le travail de l'instruction)
  lent       chemins lents de fin de bloc (ratés du TLB, sorties)

Les échantillons hors du code généré sont rangés par DSO (helpers dans le binaire QEMU,
libc, …).
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_REG, X86_OP_IMM, X86_REG_RBP

POSTES = ("entete", "ae", "tlb", "acces", "bswap", "env-ld", "env-st", "cr", "xer", "bc-test",
          "saut", "sortie-ind", "appel", "barriere", "neg", "calcul", "lent")

# CPUPPCState (qemu-system-ppc, 11.1.2 + série) — gdb sur build-mt :
ENV = [(0x0, 0x80, "gpr"), (0x100, 0x104, "lr"), (0x104, 0x108, "ctr"),
       (0x108, 0x128, "crf"), (0x128, 0x140, "xer"), (0x140, 0x14c, "reserve"),
       (0x14c, 0x150, "msr"), (0x160, 0x164, "nip"), (0x164, 0x168, "access_type")]
CAN_DO_IO = -0x10                # CPUState.neg.can_do_io, relatif à env
ARGREGS = {"rdi", "rsi", "rdx", "rcx", "r8", "r9", "edi", "esi", "edx", "ecx", "r8d", "r9d"}


def envkind(d):
    for a, b, k in ENV:
        if a <= d < b:
            return k
    if 0x12a40 <= d < 0x12e80:
        return "fp/vr"             # vsr (FPR + VR), vscr, fpscr
    if 0x1a30 <= d < 0x2a30:
        return "spr"
    if 0x13404 <= d < 0x13408:
        return "hflags"            # comparé par la recherche en ligne du cache de sauts
    return "autre"


# ---------------------------------------------------------------- invité
LD_D = {32, 33, 34, 35, 40, 41, 42, 43}
ST_D = {36, 37, 38, 39, 44, 45}
FP_D = {48, 49, 50, 51, 52, 53, 54, 55}
LD_X = {23, 55, 87, 119, 279, 311, 343, 375, 534, 790}
ST_X = {151, 183, 215, 247, 407, 439, 662, 918}
FP_X = {535, 567, 599, 631, 663, 695, 727, 759, 983}
VX_X = {7, 39, 71, 103, 359, 135, 167, 199, 231, 487}
STR_X = {533, 597, 661, 725}
CA_XO9 = {10, 138, 202, 234, 8, 136, 200, 232}
SPR_X = {339, 467, 19, 144, 371, 83, 146, 210, 242, 595, 659, 512}
SYS_X = {598, 854, 306, 566, 86, 54, 278, 246, 982, 1014, 470}


def guest_class(w):
    op = w >> 26
    xo = (w >> 1) & 0x3ff
    if op in LD_D:
        return "ld"
    if op in ST_D:
        return "st"
    if op in FP_D:
        return "ldst-fp"
    if op in (46, 47):
        return "lmw/stmw"
    if op == 31:
        if xo in LD_X:
            return "ld"
        if xo in ST_X:
            return "st"
        if xo in FP_X:
            return "ldst-fp"
        if xo in VX_X:
            return "ldst-vmx"
        if xo in STR_X:
            return "lswx/stsw"
        if xo in (20, 150):
            return "lwarx/stwcx"
        if xo in (0, 32):
            return "cmp"
        if ((w >> 1) & 0x1ff) in CA_XO9 or xo in (824, 792):
            return "int.ca"
        if xo in SPR_X:
            return "spr/cr"
        if xo in SYS_X:
            return "sys/cache"
        return "int.rc" if (w & 1) else "int"
    if op in (10, 11):
        return "cmp"
    if op in (8, 12, 13):
        return "int.ca"
    if op == 16:
        return "bc"
    if op == 18:
        return "b"
    if op == 19:
        if xo in (16, 528):
            return "bclr/bcctr"
        if xo in (50, 150, 18):
            return "sys/cache"
        return "spr/cr"            # mcrf, crand, cror…
    if op in (59, 63):
        return "fp"
    if op == 4:
        return "vmx"
    if op == 17:
        return "sys/cache"
    if op in (28, 29):
        return "int.rc"
    if op in (20, 21, 23):
        return "int.rc" if (w & 1) else "int"
    return "int"


LDST = {"ld", "st", "ldst-fp", "ldst-vmx", "lmw/stmw", "lswx/stsw", "lwarx/stwcx"}
BRANCH = {"bc", "b", "bclr/bcctr"}


# ---------------------------------------------------------------- journal
HOSTLINE = re.compile(r"^0x([0-9a-f]+):\s*$")


def index_log(path):
    """Passe 1 : (début hôte, taille, pc, décalage du bloc OUT, décalage de SON bloc IN)
    de chaque bloc. Le bloc IN est le dernier vu au même pc avant le OUT : deux vCPU
    peuvent intercaler leurs journaux, et deux processus de l'invité ont du code
    différent aux mêmes adresses (le mot invité doit venir de la même traduction)."""
    blocks = []
    pending = {}
    with open(path, "rb") as f:
        pos = 0
        pend = None
        in_off = None
        for line in f:
            if line.startswith(b"IN:"):
                in_off = pos
            elif in_off is not None and line.startswith(b"0x") and line.rstrip().endswith(b":"):
                pending[int(line.split(b":")[0], 16)] = in_off
                in_off = None
            elif line.startswith(b"OUT: [size="):
                pend = [pos, int(line[11:line.index(b"]")])]
            elif pend is not None and line.startswith(b"  -- guest addr 0x"):
                pc = int(line[18:34], 16)
                pend.append(pc)
            elif pend is not None and len(pend) == 3 and line.startswith(b"0x"):
                h = int(line.split(b":")[0], 16)
                blocks.append((h, pend[1], pend[2], pend[0], pending.pop(pend[2], None)))
                pend = None
            pos += len(line)
    return blocks


def read_in(f, off):
    """Mots invité d'un bloc IN à son décalage : {pc: mot}."""
    words = {}
    if off is None:
        return words
    f.seek(off)
    f.readline()
    base = None
    n = 0
    for line in f:
        if line.startswith(b"0x"):
            if base is None:
                base = int(line.split(b":")[0], 16)
            continue
        if line.startswith(b"OBJD-T:"):
            h = line[7:].strip()
            for k in range(0, len(h), 8):
                words[base + 4 * n] = int(h[k:k + 8], 16)
                n += 1
            continue
        break
    return words


def read_block(f, off):
    """Lit un bloc OUT à son décalage : [(gpc|None, kind, hôte, octets)]."""
    f.seek(off)
    f.readline()
    parts = []
    cur = None
    for raw in f:
        l = raw.decode("ascii", "replace").rstrip("\n")
        if l.startswith("  -- guest addr 0x"):
            gpc = int(l[18:34], 16)
            cur = [gpc, "prologue" if "prologue" in l else "insn", None, bytearray()]
            parts.append(cur)
        elif l.startswith("  -- tb slow paths"):
            cur = [None, "lent", None, bytearray()]
            parts.append(cur)
        elif l.startswith("  data:") or l.startswith("OUT:") or l.startswith("IN:") \
                or l.startswith("-----") or l.startswith("PROLOGUE") or l.startswith("OP"):
            break
        elif cur is not None:
            m = HOSTLINE.match(l)
            if m:
                if cur[2] is None:
                    cur[2] = int(m.group(1), 16)
            elif l.startswith("OBJD-H:"):
                cur[3] += bytes.fromhex(l[7:].strip())
            elif not l.strip():
                continue
            else:
                break
    return parts


def guest_words(path, want):
    """pc → mot invité, pour les pc voulus (blocs IN ; dernier vu)."""
    words = {}
    with open(path, "rb") as f:
        inb = False
        base = None
        n = 0
        for line in f:
            if line.startswith(b"IN:"):
                inb, base, n = True, None, 0
                continue
            if not inb:
                continue
            if line.startswith(b"0x"):
                if base is None:
                    base = int(line.split(b":")[0], 16)
                continue
            if line.startswith(b"OBJD-T:"):
                h = line[7:].strip()
                for k in range(0, len(h), 8):
                    pc = base + 4 * n
                    n += 1
                    if pc in want:
                        words[pc] = int(h[k:k + 8], 16)
                continue
            if not line.strip() or line.startswith(b"OUT") or line.startswith(b"OP"):
                inb = False
    return words


# ---------------------------------------------------------------- classement hôte
def regname(md, r):
    return md.reg_name(r) if r else ""


def classify_chunk(md, code, addr, gcls, kind, w=0):
    """[(adresse, poste, sous-poste, texte)] pour un fragment de code."""
    out = []
    insns = list(md.disasm(bytes(code), addr))
    head = 0
    if kind == "prologue":
        # tête : lecture d'icount_decr, test, jl vers la sortie ; le reste du fragment est
        # la première instruction invité
        for k, i in enumerate(insns):
            if i.mnemonic.startswith("j"):
                head = k + 1
                break
    if kind == "lent":
        return [(i.address, "lent", "", i.mnemonic + " " + i.op_str) for i in insns]
    ldst = gcls in LDST
    state = "avant"              # ldst : avant → tlb → après
    seen_test = False
    call_idx = [k for k, i in enumerate(insns) if i.mnemonic == "call"]
    for k, ins in enumerate(insns):
        mn = ins.mnemonic
        txt = mn + " " + ins.op_str
        ops = ins.operands
        mem = [o for o in ops if o.type == X86_OP_MEM]
        rbp = [o for o in mem if o.mem.base == X86_REG_RBP]
        dst = regname(md, ops[0].reg) if ops and ops[0].type == X86_OP_REG else ""
        p, sub = None, ""
        if mn == "mov" and rbp and rbp[0].mem.disp == CAN_DO_IO:
            p, sub = "entete", "can_do_io"   # translator.c, set_can_do_io
        elif k < head:
            p, sub = "entete", "icount"      # lecture d'icount_decr, test, jl
        elif mn == "mfence" or ins.prefix[0] == 0xf0 or mn in ("lfence", "sfence"):
            p = "barriere"
        elif ldst and state == "avant" and mn == "mov" and dst in ("rdi", "edi") and \
                len(ops) == 2 and ops[1].type == X86_OP_REG and k + 1 < len(insns) and \
                insns[k + 1].mnemonic == "shr":
            state = "tlb"
            p = "tlb"
        elif state == "tlb":
            p = "tlb"
            if mn == "mov" and dst == "rdi" and mem and mem[0].mem.base and \
                    regname(md, mem[0].mem.base) == "rdi":
                state = "acces"      # addend chargé : l'instruction suivante accède
        elif state == "acces" and mem and (regname(md, mem[0].mem.index) == "rdi" or
                                           regname(md, mem[0].mem.base) == "rdi"):
            p = "acces"
            state = "apres"
        elif state == "acces" and mn == "xor" and len(ops) == 2 and ops[0].type == X86_OP_REG \
                and ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg:
            p = "bswap"              # mise à zéro avant movbe 16 bits
        elif mn in ("bswap",) or (mn in ("rol", "ror") and ops and ops[0].size == 2):
            p = "bswap"
        elif state == "apres" and k > 0 and insns[k - 1].mnemonic == "movbe" and \
                insns[k - 1].operands[0].type == X86_OP_REG and insns[k - 1].operands[0].size == 2 \
                and mn in ("movsx", "movzx", "movsxd"):
            p = "bswap"              # extension après movbe 16 bits
        elif rbp:
            d = rbp[0].mem.disp
            if d < 0:
                p = "neg"
            else:
                st = ops and ops[0].type == X86_OP_MEM and ops[0].mem.base == X86_REG_RBP \
                    and mn not in ("cmp", "test")
                p = "env-st" if st else "env-ld"
                sub = envkind(d)
        elif mn == "call":
            p = "appel"
        elif call_idx and any(0 < c - k <= 6 for c in call_idx) and dst in ARGREGS and \
                mn in ("mov", "lea", "movabs", "movzx", "movsxd", "xor"):
            p = "appel"
        elif mn.startswith("j") or mn == "ret":
            p = "saut" if gcls != "bclr/bcctr" else "sortie-ind"
        elif gcls in BRANCH:
            # bc conditionnel : avant le premier saut, test de la condition ; après, sorties
            p = "saut" if gcls != "bclr/bcctr" else "sortie-ind"
        elif gcls == "cmp":
            p = "cr"
        elif gcls == "int.rc" and (seen_test or mn in ("test", "cmp")):
            seen_test = True
            p = "cr"
        elif gcls == "int.ca" and k > 0 and any(o[1] == "calcul" for o in out):
            p = "xer"
        elif ldst and state == "avant":
            p = "ae"
        else:
            p = "calcul"
        out.append((ins.address, p, sub, txt))
    if gcls in ("bc", "bclr/bcctr"):
        bo = (w >> 21) & 31
        cond = (bo & 0x14) != 0x14
        if cond:
            for k, o in enumerate(out):
                if o[3].startswith("j"):
                    break
                if o[1] in ("saut", "sortie-ind"):
                    out[k] = (o[0], "bc-test", o[2], o[3])
    if gcls == "bclr/bcctr":
        out = [(a, "sortie-ind" if p == "neg" else p, s2, t) for a, p, s2, t in out]
    return out


# ---------------------------------------------------------------- perf
def load_samples(data, last=0.0):
    """{événement: Counter(ip)}, {événement: Counter(dso)} (dso des échantillons).
    last > 0 : seulement les `last` dernières secondes de l'enregistrement."""
    rows = []
    p = subprocess.Popen(["perf", "script", "-i", data, "-F", "time,event,ip,dso"],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                         errors="replace")
    for l in p.stdout:
        s = l.split()
        if len(s) < 3:
            continue
        try:
            t = float(s[0].rstrip(":"))
            ip = int(s[2], 16)
        except ValueError:
            continue
        ev = s[1].rstrip(":").split(":")[0]
        dso = s[3].strip("()") if len(s) > 3 else "?"
        rows.append((t, ev, ip, dso))
    p.wait()
    ips = collections.defaultdict(collections.Counter)
    dsos = collections.defaultdict(collections.Counter)
    tmax = max(r[0] for r in rows)
    tmin = min(r[0] for r in rows)
    t0 = tmax - last if last > 0 else tmin
    for t, ev, ip, dso in rows:
        if t >= t0:
            ips[ev][ip] += 1
            dsos[ev][dso] += 1
    print("enregistrement : %.1f s ; fenêtre analysée : %.1f s" % (tmax - tmin, tmax - t0))
    return ips, dsos


def exec_rows(rows):
    """Lignes du chemin exécuté d'un bloc : sans chemins lents, sans les talons de sortie
    non chaînés (mov nip après le jmp de goto_tb, lea rax,[rip…] ; jmp vers l'épilogue)."""
    out = []
    skip = 0
    for k, r in enumerate(rows):
        if skip:
            skip -= 1
            continue
        if r[1] == "lent":
            continue
        if r[2] == "nip" and k > 0 and rows[k - 1][5].startswith("jmp 0x"):
            continue
        if r[5].startswith("lea rax, [rip") and k + 1 < len(rows) and rows[k + 1][5].startswith("jmp 0x"):
            skip = 1
            continue
        out.append(r)
    return out


def uniform(blockinfo, smp, title):
    """Échantillons répartis uniformément sur le chemin exécuté de chaque bloc : exécutions
    estimées = échantillons du bloc / longueur du chemin ; insensible au glissement et aux
    biais d'attribution à l'intérieur d'un bloc (docs/anatomie-ppc32.md §2)."""
    tot = collections.Counter()
    cls = collections.defaultdict(collections.Counter)
    cls_n = collections.Counter()
    sub = collections.Counter()
    g_ins = 0.0
    entries = 0.0
    for h, (pc, ng, rows) in blockinfo.items():
        c = sum(smp.get(r[0], 0) for r in rows)
        if not c:
            continue
        ex = exec_rows(rows)
        if not ex:
            continue
        w = c / len(ex)
        entries += w
        gl = set()
        for r in ex:
            tot[r[1]] += w
            cls[r[3]][r[1]] += w
            if r[1] in ("entete", "env-ld", "env-st"):
                sub[r[1] + ":" + r[2]] += w
            if r[4] is not None and r[4] not in gl:
                gl.add(r[4])
                cls_n[r[3]] += w
        g_ins += w * len(gl)
    ht = sum(tot.values())
    print("\n=== %s, réparti uniformément dans chaque bloc" % title)
    print("instructions hôte par instruction invité : %.1f ; par bloc exécuté : %.1f ; instructions "
          "invité par bloc : %.1f" % (ht / g_ins, ht / entries, g_ins / entries))
    print("\n| poste | part du code généré | par instruction invité |")
    print("|---|---|---|")
    for p in POSTES:
        print("| %s | %.1f %% | %.2f |" % (p, 100 * tot[p] / ht, tot[p] / g_ins))
    print("\nsous-postes :", ", ".join("%s %.1f %%" % (k, 100 * v / ht) for k, v in sub.most_common(16)))
    print("\n| classe invité | part des instr. invité | part du code généré | hôte/invité | " +
          " | ".join(POSTES) + " |")
    print("|---|---|---|---|" + "---|" * len(POSTES))
    for g in sorted(cls, key=lambda g: -sum(cls[g].values())):
        sm = sum(cls[g].values())
        n = cls_n[g] or 1e-9
        print("| %s | %.1f %% | %.1f %% | %.1f | %s |" % (g, 100 * cls_n[g] / g_ins, 100 * sm / ht, sm / n, " | ".join(
            ("%.1f" % (cls[g][p] / n) if cls[g][p] else "·") for p in POSTES)))


def lbr_entries(data, blockinfo, hidx, last=0.0):
    """Entrées de bloc par les LBR (perf record -b) : pour chaque branchement pris qui arrive
    au début d'un bloc, d'où il vient (goto_tb avant/arrière, saut indirect, boucle)."""
    starts = {h: pc for h, (pc, ng, rows) in blockinfo.items()}
    instr = {}
    for h, (pc, ng, rows) in blockinfo.items():
        for r in rows:
            instr[r[0]] = (h, r[1], r[5])
    p = subprocess.Popen(["perf", "script", "-i", data, "-F", "time,brstack"],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace")
    rows = []
    for l in p.stdout:
        s = l.split()
        if not s:
            continue
        try:
            t = float(s[0].rstrip(":"))
        except ValueError:
            continue
        rows.append((t, s[1:]))
    p.wait()
    if not rows:
        print("\n(pas de LBR dans %s)" % data)
        return
    tmax = max(r[0] for r in rows)
    t0 = tmax - last if last > 0 else 0
    kinds = collections.Counter()
    n = 0
    for t, br in rows:
        if t < t0:
            continue
        for e in br:
            f = e.split("/")
            if len(f) < 2:
                continue
            try:
                fr, to = int(f[0], 16), int(f[1], 16)
            except ValueError:
                continue
            if to not in starts:
                continue
            n += 1
            src = instr.get(fr)
            if src is None:
                kinds["hors bloc (boucle principale, helper)"] += 1
                continue
            sh, sp, txt = src
            if txt.startswith("jmp 0x"):
                k = "goto_tb vers l'avant" if starts[to] > starts[sh] else "goto_tb vers l'arrière"
            elif txt.startswith("jmp r"):
                k = "saut indirect (%s)" % sp
            else:
                k = "autre (%s)" % txt.split()[0]
            kinds[k] += 1
    print("\n=== entrées de bloc (LBR, %d branchements vers un début de bloc)" % n)
    for k, v in kinds.most_common():
        print("  %-45s %.1f %%" % (k, 100 * v / n))


LOGIC_RA = {28, 60, 444, 412, 316, 476, 124, 284, 24, 536, 792, 824, 26, 954, 922}
UPD_X = {55, 119, 311, 375, 567, 631, 183, 247, 439, 695, 759}


def gpr_writes(w):
    """GPR que l'instruction peut écrire (prudent : rD et rA si inconnu)."""
    op = w >> 26
    rd, ra = (w >> 21) & 31, (w >> 16) & 31
    xo = (w >> 1) & 0x3ff
    if op in (14, 15, 7, 8, 12, 13):
        return {rd}
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29):
        return {ra}
    if 32 <= op <= 45:
        r = set()
        if op in LD_D:
            r.add(rd)
        if op & 1:
            r.add(ra)
        return r
    if op == 46:
        return set(range(rd, 32))
    if 48 <= op <= 55:
        return {ra} if op & 1 else set()
    if op in (10, 11, 16, 18, 19, 59, 63, 4):
        return set()
    if op == 31:
        if xo in (0, 32, 150, 151, 215, 407, 662, 918, 663, 727, 983, 231, 487, 135, 167, 199,
                  86, 54, 278, 246, 982, 1014, 598, 854, 306, 566, 467, 144, 146, 210, 242):
            return set()
        if xo in LOGIC_RA:
            return {ra}
        r = {rd}
        if xo in UPD_X:
            r = ({rd} if xo in (55, 119, 311, 375) else set()) | {ra}
        return r
    return {rd, ra}


def ldst_form(w):
    """(genre, rA, D, mise-à-jour) d'un chargement/rangement entier/flottant en forme D,
    sinon None. genre : 'l' ou 's'."""
    op = w >> 26
    if 32 <= op <= 45 or 48 <= op <= 55:
        st = op in ST_D or op in (52, 53, 54, 55)
        d = w & 0xffff
        d = d - 0x10000 if d & 0x8000 else d
        return ("s" if st else "l", (w >> 16) & 31, d, bool(op & 1))
    return None


def leviers(blockinfo, smp, words):
    """Chiffrage statique pondéré de deux leviers (docs/anatomie-ppc32.md §4) :
    1. rangements de registres invité redondants : un rangement dans env suivi, dans le même
       bloc et sans appel ni sortie entre les deux, d'un autre rangement au même endroit (il
       n'existe que parce qu'un chargement/rangement invité entre les deux exige les globaux
       en mémoire) ;
    2. accès dont la page est forcément celle d'un accès précédent du même genre, à la même
       base non modifiée (forme D, |ΔD| < 4096 : vérifiable par une comparaison de page) :
       échantillons de leur chemin du TLB."""
    tot = 0
    red = collections.Counter()
    st_all = collections.Counter()
    tlb_all = 0
    tlb_same = collections.Counter()
    entries = 0
    hdr = collections.Counter()
    gotb = collections.Counter()
    for h, (pc, ng, rows) in blockinfo.items():
        if rows and rows[0][2] == "icount":
            entries += smp.get(rows[0][0], 0)
        for k, r in enumerate(rows):
            if r[1] == "entete":
                hdr[r[2]] += smp.get(r[0], 0)
            if r[1] == "saut" and r[5].startswith("jmp 0x") and k + 1 < len(rows) and \
                    rows[k + 1][2] == "nip":
                m = re.search(r"\], (0x[0-9a-f]+)$", rows[k + 1][5])
                if m:
                    tgt = int(m.group(1), 16)
                    gotb["avant" if tgt > pc else "arrière"] += smp.get(r[0], 0)
        last_st = {}
        base = {}
        lastg = None
        for k, (addr, p, sub, g, gpc, txt) in enumerate(rows):
            c = smp.get(addr, 0)
            tot += c
            if p == "lent":
                continue
            if gpc != lastg:
                lastg = gpc
                w = words[h].get(gpc)
                cur_same = None
                if w is not None:
                    f = ldst_form(w)
                    if f and f[1] != 0:
                        kind, ra, d, upd = f
                        prev = base.get(ra)
                        if prev is not None and abs(prev[1] - d) < 4096:
                            cur_same = "%s après %s" % (kind, prev[0])
                    for r in gpr_writes(w):
                        base.pop(r, None)
                    if f and f[1] != 0:
                        kind, ra, d, upd = f
                        if upd:
                            base[ra] = (kind, 0)
                        elif ra not in gpr_writes(w):
                            base[ra] = (kind, d)
                    if g in ("sys/cache", "spr/cr", "lwarx/stwcx", "lmw/stmw", "lswx/stsw"):
                        base.clear()
            if p == "tlb":
                tlb_all += c
                if cur_same:
                    tlb_same[cur_same] += c
            if p in ("appel", "saut", "sortie-ind"):
                last_st.clear()
                if p == "appel":
                    base.clear()
            if p == "env-st":
                m = re.search(r"\[rbp(?: \+ (0x[0-9a-f]+))?\]", txt)
                if m:
                    o = int(m.group(1) or "0", 16)
                    st_all[sub] += c
                    if o in last_st:
                        red[last_st[o][1]] += last_st[o][0]
                    last_st[o] = (c, sub)
    print("\n=== leviers (chiffrage statique pondéré, part du code généré)")
    print("tête de bloc : %s ; entrées de bloc (1re instruction de la tête) %.2f %% ; sorties goto_tb "
          "(jmp chaîné) : %s" % (", ".join("%s %.2f %%" % (k, 100 * v / tot) for k, v in hdr.most_common()),
                                 100 * entries / tot, ", ".join("%s %.2f %%" % (k, 100 * v / tot) for k, v in gotb.most_common())))
    if entries:
        print("  part des entrées de bloc par un goto_tb vers l'avant : %.1f %% ; vers l'arrière : %.1f %%" % (
            100 * gotb["avant"] / entries, 100 * gotb["arrière"] / entries))
    print("rangements dans env : %s" % ", ".join("%s %.2f %%" % (k, 100 * v / tot) for k, v in st_all.most_common()))
    print("  dont redondants (même registre rangé de nouveau plus loin, sans appel ni sortie) : %s ; total %.2f %%" % (
        ", ".join("%s %.2f %%" % (k, 100 * v / tot) for k, v in red.most_common()), 100 * sum(red.values()) / tot))
    print("chemin du TLB : %.2f %% ; dont accès à la page d'un accès précédent (même base, forme D) : %s ; total %.2f %%" % (
        100 * tlb_all / tot, ", ".join("%s %.2f %%" % (k, 100 * v / tot) for k, v in tlb_same.most_common()),
        100 * sum(tlb_same.values()) / tot))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("journal")
    ap.add_argument("perf")
    ap.add_argument("--event", default="instructions")
    ap.add_argument("--last", type=float, default=0.0,
                    help="seulement les S dernières secondes (t2bench : les passes gltest)")
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--lbr", action="store_true", help="entrées de bloc d'après les LBR (perf record -b)")
    ap.add_argument("--dump", default="", help="pc de blocs à imprimer, annotés (virgules)")
    ap.add_argument("--dump-top", type=int, default=0, help="imprime les N blocs les plus lourds")
    a = ap.parse_args()
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    ips, dsos = load_samples(a.perf, a.last)
    evs = sorted(ips)
    ev = a.event if a.event in ips else evs[0]
    print("événements : %s ; pondération : %s" % (", ".join("%s %d" % (e, sum(ips[e].values())) for e in evs), ev))
    blocks = index_log(a.journal)
    blocks.sort()
    starts = [b[0] for b in blocks]
    # chevauchements (tampon réutilisé) : le dernier écrit gagne — on garde l'ordre du fichier
    allip = set()
    for e in evs:
        allip.update(ips[e])
    need = {}
    for ip in allip:
        i = bisect.bisect_right(starts, ip) - 1
        if i >= 0 and ip < blocks[i][0] + blocks[i][1]:
            # dernier bloc du fichier qui commence à cette adresse
            need[blocks[i][0]] = blocks[i]
    # dernier bloc du fichier pour chaque début hôte
    last = {}
    for b in blocks:
        if b[0] in need and (b[0] not in last or b[3] > last[b[0]][3]):
            last[b[0]] = b
    print("%d blocs dans le journal, %d touchés par les échantillons" % (len(blocks), len(last)))
    parsed = {}
    words = {}                      # début hôte → {pc: mot} de SA traduction
    with open(a.journal, "rb") as f:
        for h, b in last.items():
            parts = read_block(f, b[3])
            parsed[h] = (b, parts)
            words[h] = read_in(f, b[4])
    print("blocs sans leur bloc IN : %d" % sum(1 for h in words if not words[h]))

    # instruction hôte → (poste, sous-poste, classe invité, bloc)
    hidx = {}
    blockinfo = {}
    for h, (b, parts) in parsed.items():
        rows = []
        wb = words[h]
        for gpc, kind, ha, code in parts:
            if ha is None:
                continue
            g = guest_class(wb[gpc]) if gpc is not None and gpc in wb else "?"
            for addr, p, sub, txt in classify_chunk(md, code, ha, g, kind, wb.get(gpc, 0)):
                hidx[addr] = (p, sub, g, h)
                rows.append((addr, p, sub, g, gpc, txt))
        ng = len({r[4] for r in rows if r[4] is not None})
        blockinfo[h] = (b[2], ng, rows)

    for e in evs:
        jit = collections.Counter()
        per_cls = collections.defaultdict(collections.Counter)
        sub_env = collections.Counter()
        per_block = collections.Counter()
        unres = 0
        for ip, c in ips[e].items():
            r = hidx.get(ip)
            if r is None:
                i = bisect.bisect_right(starts, ip) - 1
                if i >= 0 and ip < blocks[i][0] + blocks[i][1]:
                    unres += c
                continue
            p, sub, g, h = r
            jit[p] += c
            per_cls[g][p] += c
            per_block[h] += c
            if p in ("env-ld", "env-st", "entete"):
                sub_env[p + ":" + sub] += c
        tot = sum(ips[e].values())
        tj = sum(jit.values())
        print("\n=== %s : %d échantillons ; code généré %.1f %% (non résolus dans le code : %d)" % (
            e, tot, 100 * tj / tot, unres))
        print("hors code généré, par DSO :")
        dd = collections.Counter()
        for d, c in dsos[e].items():
            dd[d.rsplit("/", 1)[-1]] += c
        for d, c in dd.most_common(8):
            print("  %-40s %.1f %%" % (d[:40], 100 * c / tot))
        print("\n| poste | part du code généré | part du total |")
        print("|---|---|---|")
        for p in POSTES:
            print("| %s | %.1f %% | %.1f %% |" % (p, 100 * jit[p] / tj, 100 * jit[p] / tot))
        print("\nenv et tête par sous-poste :", ", ".join("%s %.1f %%" % (k, 100 * v / tj) for k, v in sub_env.most_common(14)))
        print("\n| classe invité | part du code généré | " + " | ".join(POSTES) + " |")
        print("|---|---|" + "---|" * len(POSTES))
        for g in sorted(per_cls, key=lambda g: -sum(per_cls[g].values())):
            s = sum(per_cls[g].values())
            print("| %s | %.1f %% | %s |" % (g, 100 * s / tj, " | ".join(
                ("%.0f" % (100 * per_cls[g][p] / s) if per_cls[g][p] else "·") for p in POSTES)))
        if e == ev:
            heavy = per_block.most_common(max(a.top, a.dump_top))
            print("\nblocs les plus lourds (%s) :" % e)
            for h, c in heavy[:a.top]:
                pc, ng, rows = blockinfo[h]
                print("  pc 0x%08x  %.2f %%  %d instr. invité, %d hôte (statique)" % (
                    pc, 100 * c / tj, ng, len(rows)))
    leviers(blockinfo, ips[ev], words)
    uniform(blockinfo, ips[ev], ev)
    if a.lbr:
        lbr_entries(a.perf, blockinfo, hidx, a.last)
    dump = {int(x, 16) for x in a.dump.split(",") if x}
    dump_h = set()
    if a.dump_top:
        bc = collections.Counter()
        for ip, c in ips[ev].items():
            r = hidx.get(ip)
            if r:
                bc[r[3]] += c
        for h, c in bc.most_common(a.dump_top):
            dump_h.add(h)
    if dump or dump_h:
        smp = ips[ev]
        for h, (pc, ng, rows) in blockinfo.items():
            if pc not in dump and h not in dump_h:
                continue
            print("\n----- bloc pc 0x%08x (hôte 0x%x)" % (pc, h))
            lastg = None
            for addr, p, sub, g, gpc, txt in rows:
                if gpc != lastg:
                    w = words[h].get(gpc)
                    print("  [0x%08x %s %s]" % (gpc or 0, "%08x" % w if w is not None else "--------", g))
                    lastg = gpc
                print("    %6d  %-9s %-7s %s" % (smp.get(addr, 0), p, sub, txt))


if __name__ == "__main__":
    main()
