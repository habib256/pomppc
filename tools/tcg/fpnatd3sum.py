#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""fpnatd3sum.py [DOSSIER] — résume les parties DOOM 3 de tools/tcg/fpnatd3.py
(défaut <dépôt principal>/bench/tcg/fpnat/d3) : par mode (nom de partie sans
son numéro), ms/image de chaque partie sur la fenêtre T+50..T+280 et sur les
images qui suivent, médiane ; puis, pour chaque partie qui a un sample.txt,
la part du fil vCPU le plus occupé (hors attente) passée dans le flottant
scalaire (helpers de x-fp-inline, x-fp-flat, x-fp-native, séquence
d'origine, softfloat) et dans le code généré."""
import collections
import os
import re
import statistics
import sys

MAIN = "/Users/mercure/src/pomppc"
D = sys.argv[1] if len(sys.argv) > 1 else os.path.join(MAIN, "bench", "tcg", "fpnat", "d3")
RX_F = re.compile(r"fenêtre (\d+)\.\.(\d+) : ([\d.]+) ms/image")
RX_A = re.compile(r"après : images (\d+)\.\.(\d+) : ([\d.]+) ms/image")

# fonctions repères (inclusif) dans le fil vCPU
REPERES = [
    ("flottant scalaire (helpers)",
     r"^(helper_fp32_fast|helper_fp32_flat|helper_fcmpu_flat|ppc_fp32_native_slow|"
     r"helper_F(ADD|SUB|MUL|MADD|MSUB|NMADD|NMSUB)S|helper_fcmpu|helper_fprf_check_float64|"
     r"helper_float_check_status|helper_fpv_\w+|helper_fpn_verify)$"),
    ("AltiVec (helpers v*)", r"^helper_v"),
    ("lookup_tb_ptr", r"^helper_lookup_tb_ptr$"),
    ("lmw/stmw", r"^helper_(lmw|stmw)$"),
    ("TLB", r"^(tlb_fill_align|ppc_cpu_tlb_fill|ppc_xlate|mmu_lookup|probe_access\w*)$"),
    ("verrou global", r"^(bql_lock_impl|qemu_mutex_lock_impl)$"),
]


def fils(path):
    """[(total, [(profondeur, n, nom)])] des fils du sample."""
    row = re.compile(r"^(?P<pre>[ +!:|]*)(?P<n>\d+) (?P<name>.+?)(?:  \(in [^)]+\))?(?: \+ [^\[]*)?(?: \[[^\]]*\])?\s*$")
    thr = re.compile(r"^\s+(\d+) Thread_(\d+)")
    out, cur, tree = [], None, False
    for l in open(path, errors="replace"):
        if l.startswith("Call graph:"):
            tree = True
            continue
        if tree and (l.startswith("Total number in stack") or l.startswith("Sort by top")):
            break
        if not tree:
            continue
        m = thr.match(l)
        if m:
            cur = [int(m.group(1)), []]
            out.append(cur)
            continue
        m = row.match(l) if cur else None
        if m:
            cur[1].append((len(m.group("pre")), int(m.group("n")),
                           m.group("name").split("  (in ")[0].strip()))
    return out


def inclusif(ent, pat):
    rx = re.compile(pat)
    tot, pile = 0, []
    for d, n, name in ent:
        while pile and pile[-1] >= d:
            pile.pop()
        if rx.search(name):
            if not pile:
                tot += n
            pile.append(d)
    return tot


def main():
    parts = collections.defaultdict(list)
    for sub in sorted(os.listdir(D)):
        info = os.path.join(D, sub, "info.txt")
        if not os.path.exists(info):
            continue
        txt = open(info, errors="replace").read()
        f = RX_F.search(txt)
        a = RX_A.search(txt)
        mode = sub.rsplit("-", 1)[0]
        parts[mode].append((sub, float(f.group(3)) if f else None,
                            float(a.group(3)) if a else None))
    print("== ms/image (fenêtre T+50..T+280 ; images suivantes)")
    for mode, l in parts.items():
        fen = [x[1] for x in l if x[1]]
        apr = [x[2] for x in l if x[2]]
        print("%-8s %s   médiane %s ; après %s" % (
            mode, "  ".join("%s %s/%s" % (s, f, a) for s, f, a in l),
            "%.1f" % statistics.median(fen) if fen else "-",
            "%.1f" % statistics.median(apr) if apr else "-"))
    print("\n== sample : fil vCPU le plus occupé (hors attente), inclusif")
    for mode, l in parts.items():
        for sub, _, _ in l:
            p = os.path.join(D, sub, "sample.txt")
            if not os.path.exists(p):
                continue
            best = None
            for total, ent in fils(p):
                if inclusif(ent, r"^(mttcg_cpu_thread_fn|rr_cpu_thread_fn)$") == 0:
                    continue
                busy = total - inclusif(ent, r"^(qemu_wait_io_event|qemu_cond_wait_impl|__psynch_cvwait)$")
                if best is None or busy > best[0]:
                    best = (busy, ent)
            if not best:
                continue
            busy, ent = best
            cells = ["%s %.1f %%" % (lab, 100.0 * inclusif(ent, pat) / busy) for lab, pat in REPERES]
            print("%-10s occupé %d éch. ; %s" % (sub, busy, " ; ".join(cells)))


if __name__ == "__main__":
    main()
