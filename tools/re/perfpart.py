#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""perfpart.py PERF.DATA [--ms X] [--images-par-s N] [--top K] — répartition, par poste,
d'un `perf record -p <QEMU>` pris sous Linux (matrice.py --sample-hote, QEMU lancé
avec EXTRA_ARGS="-perfmap -name Tiger,debug-threads=on" pour nommer le code JIT et
les fils). Le pendant Linux de tools/tcg/threadbusy.py et samplesum.py du M4.

Par fil : échantillons (999 Hz : part d'un cœur occupé), cycles, fréquence moyenne.
Pour les fils vCPU réunis : postes (code généré, recherche de blocs, softmmu,
AltiVec, flottant, lmw/stmw, base de temps, boucle d'exécution, verrous, noyau…),
part du temps vCPU occupé ; --ms X (ms/image de la cellule) les convertit en
ms de vCPU par image. Puis les K fonctions les plus lourdes (helpers par nom,
blocs JIT par adresse de l'invité)."""
import re
import subprocess
import sys
from collections import defaultdict

POSTES = [
    ("code généré (JIT)", lambda s, d: d.startswith("/tmp/perf-") or s.startswith("guest-")),
    ("recherche de blocs (lookup_tb_ptr, tb_lookup, qht)",
     lambda s, d: s in ("helper_lookup_tb_ptr", "tb_lookup", "tb_lookup_cmp", "qht_lookup_custom",
                        "tb_htable_lookup", "get_page_addr_code_hostp", "tb_jmp_cache_get_tb")
     or s.startswith("qht_")),
    ("traduction (tb_gen_code, tcg_*)",
     lambda s, d: s.startswith(("tb_gen_code", "tcg_", "gen_", "translator_", "ppc_tr_", "decode_",
                                "liveness_", "reachable_", "la_", "fold_", "tb_link", "tb_page",
                                "tb_phys", "tb_invalidate", "tb_flush", "do_tb_"))),
    ("lmw/stmw", lambda s, d: s in ("helper_lmw", "helper_stmw")),
    ("base de temps (mftb)", lambda s, d: s.startswith(("tbf_", "cpu_ppc_load_tb", "helper_load_tb",
                                                         "qemu_raw_clock", "cpu_ppc_get_tb"))),
    ("AltiVec (helpers v*, vfp, vperm)",
     lambda s, d: s.startswith(("helper_v", "helper_V", "vfp_", "vperm_", "helper_mtvscr",
                                "helper_mfvscr", "helper_lvsl", "helper_lvsr", "helper_stve", "helper_lve"))),
    ("flottant (softfloat, helpers f*)",
     lambda s, d: s.startswith(("float", "parts64_", "parts_", "helper_f", "helper_F", "do_float",
                                "helper_compute_fprf", "helper_todouble", "helper_tosingle",
                                "fpn_", "helper_fpn", "float_raise", "helper_reset_fpstatus",
                                "helper_float_check", "round_canonical", "frac_"))),
    ("softmmu (TLB, accès lents, MMU de l'invité)",
     lambda s, d: s.startswith(("probe_", "mmu_lookup", "do_ld", "do_st", "helper_ld", "helper_st",
                                "helper_le_", "helper_be_", "tlb_", "victim_tlb", "ppc_hash32",
                                "ppc_cpu_tlb", "ppc_xlate", "address_space_", "flatview_",
                                "qemu_ram_", "memory_region_", "physical_memory", "notdirty",
                                "cpu_physical_memory", "atomic_mmu", "dcbz_common", "helper_dcbz",
                                "int_ld", "int_st", "io_read", "io_write", "find_next_bit",
                                "get_ptr_rcu", "ppc_cpu_mmu_index", "cpu_ld", "cpu_st",
                                "helper_atomic", "store_helper", "load_helper"))),
    ("boucle d'exécution (cpu_exec, interruptions)",
     lambda s, d: s.startswith(("cpu_exec", "cpu_tb_exec", "cpu_handle", "ppc_cpu_exec",
                                "ppc_cpu_do_interrupt", "ppc_cpu_has_work", "powerpc_", "helper_raise",
                                "raise_exception", "helper_rfi", "helper_store_msr", "hreg_",
                                "cpu_loop_exit", "ppc_maybe_", "helper_mtmsr", "helper_sc",
                                "cpu_interrupt", "ppc_set_irq", "ppc_hw_interrupt",
                                "helper_store_sr", "helper_store_dbat", "helper_store_ibat",
                                "helper_tlbie", "helper_tlbia", "helper_icbi", "object_dynamic_cast",
                                "mttcg_cpu", "rr_cpu", "tcg_cpu", "cpu_", "qemu_cond", "qemu_wait"))),
    ("verrous (BQL, mutex)", lambda s, d: s.startswith(("qemu_mutex", "bql_", "pthread_mutex",
                                                         "__lll_", "qemu_futex", "qemu_event",
                                                         "qemu_lockcnt", "futex"))),
    ("libc (memcpy, memset…)", lambda s, d: "libc.so" in d),
    ("noyau", lambda s, d: d in ("[kernel.kallsyms]", "[unknown]") or s.startswith("0xffff")),
]

LIGNE = re.compile(r"^\s*(?P<comm>.+?)\s+(?P<tid>\d+)\s+(?P<per>\d+)\s+(?P<ip>[0-9a-f]+)\s+(?P<sym>.*?)\s+\((?P<dso>[^)]*)\)\s*$")


def poste(s, d):
    for nom, f in POSTES:
        if f(s, d):
            return nom
    return "autres (QEMU)"


def main():
    a = sys.argv[1:]
    opt = {"--ms": None, "--top": "30"}
    for k in list(opt):
        if k in a:
            i = a.index(k)
            opt[k] = a[i + 1]
            del a[i:i + 2]
    data = a[0]
    hz, dur = 999.0, None
    for l in subprocess.run(["perf", "script", "-i", data, "--header-only"], capture_output=True,
                            text=True).stdout.splitlines():
        m = re.search(r"sample duration :\s+([0-9.]+) ms", l)
        if m:
            dur = float(m.group(1)) / 1000
        m = re.search(r"sample_freq \} = (\d+)", l)
        if m:
            hz = float(m.group(1))
    p = subprocess.Popen(["perf", "script", "-i", data, "-F", "comm,tid,period,ip,sym,dso"],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace")
    fil_n, fil_c = defaultdict(int), defaultdict(int)
    vp, vs, vn = defaultdict(int), defaultdict(int), 0
    autres = defaultdict(lambda: defaultdict(int))
    for l in p.stdout:
        m = LIGNE.match(l)
        if not m:
            continue
        comm, per, sym, dso = m.group("comm"), int(m.group("per")), m.group("sym"), m.group("dso")
        if sym.startswith("[unknown]") or sym == "":
            sym = "?"
        fil_n[comm] += 1
        fil_c[comm] += per
        if "/TCG" in comm:
            vn += 1
            vp[poste(sym, dso)] += 1
            if dso.startswith("/tmp/perf-"):
                vs["[JIT] " + sym] += 1
            else:
                vs[sym] += 1
        else:
            autres[comm][sym if sym != "?" else dso] += 1
    dur = dur or 10.0
    print("durée %.2f s, %g Hz\n" % (dur, hz))
    print("| fil | part d'un cœur | Gcycles/s occupé |")
    print("|---|---|---|")
    for c in sorted(fil_n, key=lambda c: -fil_n[c]):
        busy = fil_n[c] / (hz * dur)
        if busy < 0.005:
            continue
        print("| %s | %.0f %% | %.2f |" % (c, 100 * busy, fil_c[c] / (busy * dur) / 1e9 if busy else 0))
    vbusy = vn / (hz * dur)
    ms = float(opt["--ms"]) if opt["--ms"] else None
    print("\nfils vCPU réunis : %.2f cœur occupé%s\n" % (
        vbusy, (" ; %.1f ms de vCPU par image (%.1f ms/image)" % (vbusy * ms, ms)) if ms else ""))
    print("| poste (fils vCPU) | part du temps vCPU occupé |%s" % (" ms vCPU/image |" if ms else ""))
    print("|---|---|%s" % ("---|" if ms else ""))
    for k, n in sorted(vp.items(), key=lambda x: -x[1]):
        print("| %s | %.1f %% |%s" % (k, 100 * n / vn, (" %.1f |" % (n / vn * vbusy * ms)) if ms else ""))
    print("\nFonctions les plus lourdes (fils vCPU, part du temps vCPU occupé) :")
    for s, n in sorted(vs.items(), key=lambda x: -x[1])[:int(opt["--top"])]:
        print("  %5.2f %%  %s" % (100 * n / vn, s))
    for c in sorted(autres, key=lambda c: -fil_n[c]):
        if fil_n[c] / (hz * dur) < 0.03:
            continue
        top = sorted(autres[c].items(), key=lambda x: -x[1])[:10]
        print("\n%s (%.0f %% d'un cœur) : %s" % (c, 100 * fil_n[c] / (hz * dur),
                                               ", ".join("%s %.1f %%" % (s, 100 * n / fil_n[c]) for s, n in top)))


if __name__ == "__main__":
    main()
