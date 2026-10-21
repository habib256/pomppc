#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""codeanat.py JOURNAL [--perf PERF.DATA --map PERFMAP] [--top N] — anatomie du code x86-64
que TCG émet pour les blocs d'un journal QEMU `-d in_asm,out_asm` (formes OBJD-T/OBJD-H,
désassemblées ici par capstone). Phase 0 du traducteur de second niveau
(docs/plan-traducteur-rapide.md) : combien d'instructions hôte par instruction invité, et
lesquelles un niveau 2 supprimerait.

Chaque instruction hôte est rangée dans un poste :

  etat-ld / etat-st   accès à CPUPPCState par rbp (TCG_AREG0) à déplacement ≥ 0 : registres
                      de l'invité rechargés / rangés (GPR, CR, XER, LR, CTR, FPR, VR, nip…)
  neg                 rbp à déplacement < 0 : état négatif de CPUState (exit_request,
                      can_do_io), TLB logiciel (masque, table), cache de sauts
  tlb                 reste du chemin court du TLB (décalage, masque, comparaison, addend)
                      : instructions entre le premier accès « neg » d'une instruction de
                      chargement/rangement et l'accès à la mémoire de l'invité
  memoire             accès mémoire de l'invité et autres mémoires (tables, pile)
  saut                jmp/jcc/call/ret
  calcul              le reste (le travail réel)

Les chemins lents de fin de bloc (« tb slow paths ») sont comptés à part (hors chemin court).
Pondération : chaque bloc compte une fois, ou (--perf/--map, tools/tcg/regions.py) au prorata
du temps échantillonné de son adresse de départ.
"""
import argparse
import collections
import re
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RBP, X86_REG_RSP

POSTES = ("etat-ld", "etat-st", "neg", "tlb", "memoire", "saut", "calcul")


def parse(path):
    """Blocs : dict(pc, insns=[(gpc, mot)], parts=[(gpc|None, bytes)], slow=bytes)."""
    blocks = []
    cur_in = None
    cur = None
    gpc = None
    with open(path, errors="replace") as f:
        lines = f.read().split("\n")
    i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith("IN:"):
            cur_in = []
            i += 1
            base = None
            while i < len(lines) and lines[i].strip() and not lines[i].startswith("OP"):
                m = re.match(r"0x([0-9a-f]+):", lines[i])
                if m and base is None:
                    base = int(m.group(1), 16)
                if lines[i].startswith("OBJD-T:"):
                    h = lines[i].split(":", 1)[1].strip()
                    for k in range(0, len(h), 8):
                        cur_in.append((base + len(cur_in) * 4, int(h[k:k + 8], 16)))
                i += 1
            continue
        m = re.match(r"OUT: \[size=(\d+)\]", l)
        if m:
            cur = dict(pc=None, insns=cur_in or [], parts=[], slow=b"")
            blocks.append(cur)
            gpc = None
            i += 1
            continue
        if cur is not None:
            m = re.match(r"\s*-- guest addr 0x([0-9a-f]+)", l)
            if m:
                gpc = int(m.group(1), 16)
                if cur["pc"] is None:
                    cur["pc"] = gpc
                cur["parts"].append([gpc, b""])
            elif "-- tb slow paths" in l:
                cur["parts"].append([None, b""])
            elif l.startswith("OBJD-H:") and cur["parts"]:
                cur["parts"][-1][1] += bytes.fromhex(l.split(":", 1)[1].strip())
            elif l.startswith("----------------") or l.startswith("PROLOGUE"):
                cur = None
        i += 1
    return [b for b in blocks if b["pc"] is not None]


def is_ldst(w):
    op = w >> 26
    if 32 <= op <= 55:
        return True
    if op == 31:
        xo = (w >> 1) & 0x3ff
        return xo in (23, 55, 87, 119, 279, 311, 343, 375, 151, 183, 215, 247, 407, 439,
                      535, 567, 599, 631, 663, 695, 727, 759, 534, 662, 790, 918, 20, 150,
                      7, 39, 71, 103, 135, 167, 199, 231, 359, 487, 1014, 533, 597, 661, 725)
    return op in (46, 47)


def guest_class(w):
    op = w >> 26
    if is_ldst(w):
        return "ldst"
    if op in (59, 63):
        return "fp" if not (op == 63 and ((w >> 1) & 0x3ff) in (0, 32)) else "cmp"
    if op == 4:
        return "vmx"
    if op in (16, 18, 19):
        return "branch"
    if op in (10, 11) or (op == 31 and ((w >> 1) & 0x3ff) in (0, 32)):
        return "cmp"
    rc = w & 1
    if op in (28, 29) or (rc and op in (20, 21, 23, 31)):
        return "int.rc"          # forme « point » : pose CR0
    if op == 31 and ((w >> 1) & 0x3ff) in (339, 467, 19, 144, 371, 83, 146):
        return "spr"
    return "int"


def classify(md, code, ldst):
    """Compte par poste les instructions d'un fragment ; renvoie (Counter, n)."""
    c = collections.Counter()
    in_tlb = False
    for ins in md.disasm(code, 0):
        mn = ins.mnemonic
        mem = [o for o in ins.operands if o.type == X86_OP_MEM]
        rbp = [o for o in mem if o.mem.base == X86_REG_RBP]
        if mn.startswith("j") or mn in ("call", "ret"):
            # le jne vers le chemin lent clôt la séquence du TLB
            c["tlb" if in_tlb and mn == "jne" else "saut"] += 1
            continue
        if rbp:
            d = rbp[0].mem.disp
            if d < 0:
                c["neg"] += 1
                if ldst:
                    in_tlb = True
                continue
            # écriture : destination mémoire (premier opérande, AT&T inversé par capstone : Intel)
            st = ins.operands and ins.operands[0].type == X86_OP_MEM and \
                ins.operands[0].mem.base == X86_REG_RBP and mn not in ("cmp", "test")
            c["etat-st" if st else "etat-ld"] += 1
            continue
        if mem and not (len(mem) == 1 and mn == "lea"):
            if in_tlb and mem[0].mem.index == 0 and mem[0].mem.base != X86_REG_RSP \
                    and mn in ("cmp", "mov") and mem[0].mem.disp in (0, 8, 0x18, 0x10, 4):
                c["tlb"] += 1          # cmp comparateur / mov addend dans l'entrée du TLB
                continue
            c["memoire"] += 1
            in_tlb = False
            continue
        c["tlb" if in_tlb else "calcul"] += 1
    return c


def time_by_pc(perf, mp):
    sys.path.insert(0, __file__.rsplit("/", 1)[0])
    import regions as R
    h = R.HostIndex(R.load_map(mp))
    per, tot, jit, _ = R.load_perf(perf, h, mp)
    t = collections.Counter()
    for (pc, n), c in per.items():
        t[pc] += c
    return t, tot


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("journal")
    ap.add_argument("--perf")
    ap.add_argument("--map")
    ap.add_argument("--top", type=int, default=0)
    a = ap.parse_args()
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    blocks = parse(a.journal)
    # un bloc par adresse (le dernier traduit)
    bypc = {}
    for b in blocks:
        bypc[b["pc"]] = b
    wt, vtot = (time_by_pc(a.perf, a.map) if a.perf else (None, None))
    tot = collections.Counter()
    slow_ins = 0.0
    g_insns = 0.0
    wsum = 0.0
    per_kind = collections.defaultdict(collections.Counter)   # classe d'instruction invité
    per_kind_n = collections.Counter()
    rows = []
    for pc, b in bypc.items():
        w = wt.get(pc, 0) if wt is not None else 1
        if w == 0:
            continue
        words = dict(b["insns"])
        bc = collections.Counter()
        n_g = 0
        for gpc, code in b["parts"]:
            if gpc is None:
                slow_ins += w * sum(1 for _ in md.disasm(code, 0))
                continue
            wd = words.get(gpc, 0)
            c = classify(md, code, is_ldst(wd))
            bc.update(c)
            n_g += 1
            k = guest_class(wd)
            per_kind[k].update({p: w * v for p, v in c.items()})
            per_kind_n[k] += w
        for p, v in bc.items():
            tot[p] += w * v
        g_insns += w * n_g
        wsum += w
        rows.append((w, pc, n_g, sum(bc.values()), bc))
    ht = sum(tot.values())
    print("%d blocs distincts, %s" % (len(rows), "pondérés par le temps perf (%.1f %% du vCPU)" % (
        100 * wsum / vtot) if wt is not None else "chacun une fois"))
    print("instructions hôte par instruction invité (chemin court) : %.1f ; chemins lents de fin "
          "de bloc : %.1f de plus (hors chemin court)" % (ht / g_insns, slow_ins / g_insns))
    print("\n| poste | par instruction invité | part |")
    print("|---|---|---|")
    for p in POSTES:
        print("| %s | %.2f | %.1f %% |" % (p, tot[p] / g_insns, 100 * tot[p] / ht))
    print("\n| classe invité | part des instr. invité | hôte/invité | " + " | ".join(POSTES) + " |")
    print("|---|---|---|" + "---|" * len(POSTES))
    for k in sorted(per_kind, key=lambda k: -per_kind_n[k]):
        n = per_kind_n[k]
        s = sum(per_kind[k].values())
        print("| %s | %.1f %% | %.1f | %s |" % (k, 100 * n / g_insns, s / n, " | ".join(
            "%.1f" % (per_kind[k][p] / n) for p in POSTES)))
    if a.top:
        print("\nblocs les plus lourds :")
        for w, pc, n, h, bc in sorted(rows, key=lambda r: -r[0])[:a.top]:
            print("  0x%08x poids %5.0f  %3d instr. invité  %5.1f hôte/invité  etat %4.1f  neg+tlb %4.1f" % (
                pc, w, n, h / n, (bc["etat-ld"] + bc["etat-st"]) / n, (bc["neg"] + bc["tlb"]) / n))


if __name__ == "__main__":
    main()
