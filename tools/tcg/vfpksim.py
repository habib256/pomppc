#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""vfpksim.py JOURNAL --perf PERF.DATA --map PERFMAP [--regles R] — simulation statique de
x-vfp-known (tcg/0053, docs/tcg-g4.md §42) sur les blocs d'un journal `-d in_asm,out_asm`
(tools/tcg/codeanat.py), pondérés par le temps de perf : combien de tests de porte et
d'opérande des opérations flottantes AltiVec natives (vaddfp vsubfp vmaddfp vnmsubfp, vcf*,
vcmp*fp) le levier retirerait, selon les règles de propagation.

État au début de chaque instruction : K = VR connus (toutes voies nulles ou normales), G =
porte connue. Une instruction dont le code x86 du chemin court contient un `call` (helper) ou
un saut vers une étiquette interne au bloc (étiquette TCG) tue tout ; sinon, une instruction
qui écrit un VR le retire de K, sauf les producteurs et (selon --regles) les propagateurs :

  base    producteurs seuls (vaddfp vsubfp vmaddfp vnmsubfp vcfsx vcfux)
  --liste helpers de la liste blanche (vperm, vmaxfp, vminfp, vrefp, vrsqrtefp, vexptefp,
          vlogefp, vrfi*) : n'écrivent que vD, n'ôtent rien à la porte
  prop    + vmrghw vmrglw, vspltw, vsldoi (décalage multiple de 4), vor vD,vA,vA,
            vxor/vsubuwm vD,vA,vA et vspltis[bhw] 0 (zéro)
"""
import argparse
import collections
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import codeanat as C  # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_64  # noqa: E402

VA_FP = {46: "vmaddfp", 47: "vnmsubfp"}
VX_FP = {10: "vaddfp", 74: "vsubfp"}
VX_CF = {842: "vcfsx", 778: "vcfux"}
VX_CT = {970: "vctsxs", 906: "vctuxs"}
VXR_CMP = {198: "vcmpeqfp", 454: "vcmpgefp", 710: "vcmpgtfp", 966: "vcmpbfp"}


# helpers qui n'écrivent que vD et ne font qu'ajouter des drapeaux à vec_status (§42.2)
SAFE = {"vperm", "vmaxfp", "vminfp", "vx266", "vx330", "vx394", "vx458", "vx522", "vx586",
        "vx650", "vx714"}


def fields(w):
    return (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31


def decode(w):
    """(nom, vD écrit ou None, opérandes lus, sorte)"""
    op = w >> 26
    d, a, b, c = fields(w)
    if op == 4:
        va = w & 0x3f
        vx = w & 0x7ff
        if va in VA_FP:
            return VA_FP[va], d, (a, b, c), "fp"
        if va == 43:
            return "vperm", d, (a, b, c), "autre"
        if va == 44:
            return "vsldoi", d, (a, b, ((w >> 6) & 15)), "vsldoi"
        if 32 <= va <= 47:
            return "va%d" % va, d, (a, b, c), "autre"
        if vx in VX_FP:
            return VX_FP[vx], d, (a, b), "fp"
        if vx in VX_CF:
            return VX_CF[vx], d, (b,), "cf"
        if vx in VX_CT:
            return VX_CT[vx], d, (b,), "ct"
        if (w & 0x3ff) in VXR_CMP:
            return VXR_CMP[w & 0x3ff], d, (a, b), "cmp"
        names = {140: "vmrghw", 396: "vmrglw", 652: "vspltw", 1156: "vor", 1220: "vxor",
                 1028: "vand", 1092: "vandc", 1284: "vnor", 908: "vspltisw", 844: "vspltish",
                 780: "vspltisb", 1152: "vsubuwm", 128: "vadduwm", 1604: "mtvscr",
                 1540: "mfvscr", 1034: "vmaxfp", 1098: "vminfp"}
        n = names.get(vx, "vx%d" % vx)
        if vx == 1604:
            return n, None, (b,), "mtvscr"
        return n, d, (a, b), n
    if op == 31:
        xo = (w >> 1) & 0x3ff
        if xo in (103, 359, 7, 39, 71, 6, 38):
            return {103: "lvx", 359: "lvxl", 7: "lvebx", 39: "lvehx", 71: "lvewx",
                    6: "lvsl", 38: "lvsr"}[xo], d, (), "ld"
    return None, None, (), None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("journal")
    ap.add_argument("--perf", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--regles", default="prop", choices=("base", "prop"))
    ap.add_argument("--liste", action="store_true",
                    help="helpers de la liste blanche (vperm, vmaxfp...) : vD seul tué")
    a = ap.parse_args()
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    blocks = C.parse(a.journal)
    bypc = {b["pc"]: b for b in blocks}
    wt, _ = C.time_by_pc(a.perf, a.map)
    st = collections.Counter()
    killers = collections.Counter()
    origin = collections.Counter()
    for pc, b in bypc.items():
        w = wt.get(pc, 0)
        if not w:
            continue
        words = dict(b["insns"])
        main_len = sum(len(code) for g, code in b["parts"] if g is not None)
        off = 0
        K, G = set(), False
        writer = {}
        for gpc, code in b["parts"]:
            if gpc is None:
                break
            wd = words.get(gpc, 0)
            name, vd, srcs, kind = decode(wd)
            # le code x86 de l'instruction : helper ? étiquette interne ?
            call = label = False
            for ins in md.disasm(code, off):
                if ins.mnemonic == "call":
                    call = True
                elif ins.mnemonic.startswith("j") and ins.op_str.startswith("0x"):
                    t = int(ins.op_str, 16)
                    if 0 <= t < main_len:
                        label = True
            off += len(code)
            if kind in ("fp", "cf", "cmp"):
                ops = srcs if kind != "cmp" else srcs
                gate = kind in ("fp", "cf")
                if gate:
                    st["porte"] += w
                    if G:
                        st["porte connue"] += w
                for r in ops:
                    st["opérandes"] += w
                    if r in K:
                        st["opérandes connus"] += w
                    origin[writer.get(r, "hors bloc")] += w
                # rechargements : un opérande lu deux fois (vA = vC...)
                if kind in ("fp", "cf"):
                    G = True
                if kind in ("fp", "cf"):
                    K.add(vd)
                else:
                    K.discard(vd)
                writer[vd] = name
                st["n " + kind] += w
                continue
            safe = a.liste and call and not label and name in SAFE
            if safe:
                K.discard(vd)
                writer[vd] = name
                continue
            if call or label:
                if K or G:
                    killers[(name or "%08x" % wd) + (" (call)" if call else " (label)")] += w
                K, G = set(), False
                if vd is not None:
                    writer[vd] = name
                continue
            if kind == "mtvscr":
                # porte : ne touche ni l'arrondi, ni no_hardfloat, ni inexact (§42.2)
                continue
            if vd is None:
                continue
            keep = False
            if a.regles == "prop":
                A = srcs[0] if srcs else None
                B = srcs[1] if len(srcs) > 1 else None
                if name in ("vmrghw", "vmrglw"):
                    keep = A in K and B in K
                elif name == "vspltw":
                    keep = B in K
                elif name == "vsldoi":
                    sh = srcs[2]
                    keep = sh % 4 == 0 and (A in K) and (sh == 0 or B in K)
                elif name == "vor" and A == B:
                    keep = A in K
                elif name in ("vxor", "vsubuwm") and A == B:
                    keep = True
                elif name in ("vspltisw", "vspltish", "vspltisb") and A == 0:
                    keep = True
            if keep:
                K.add(vd)
            else:
                K.discard(vd)
            writer[vd] = name
    T = st["opérandes"]
    print("règles %s" % a.regles)
    print("ops : fp %.0f cf %.0f cmp %.0f" % (st["n fp"], st["n cf"], st["n cmp"]))
    print("porte connue : %.1f %% des %d tests pondérés" % (100 * st["porte connue"] / st["porte"],
                                                           st["porte"]))
    print("opérandes connus : %.1f %%" % (100 * st["opérandes connus"] / T))
    print("origine des opérandes :")
    for k, v in origin.most_common(14):
        print("  %-12s %.1f %%" % (k, 100 * v / T))
    print("tueurs (état non vide tué) :")
    for k, v in killers.most_common(12):
        print("  %-24s %.1f %% des tests de porte" % (k, 100 * v / st["porte"]))


if __name__ == "__main__":
    main()
