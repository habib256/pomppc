#!/usr/bin/env python3
"""samplesum.py FICHIER [N] — résume un `sample <pid>` macOS du processus QEMU.

1. par fil : échantillons totaux, et part « code généré » (cadres ??? dans
   le tampon du JIT) / helpers / softfloat / verrous / TLB / traduction ;
2. le « Sort by top of stack » (self) des N premières fonctions ;
3. inclusif, par fil, de fonctions repères (cpu_exec, tb_gen_code, tlb_fill,
   helpers, softfloat, bql…).

Le self % localise, il ne valide pas (docs/metrologie-boot.md)."""
import re, sys, collections

path = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 40
lines = open(path, errors="replace").read().split("\n")

# --- arbre d'appels par fil ------------------------------------------------
# lignes : "    <indent>+ ! : | 1234 fonction  (dans lib) + 0x.. [0x..]"
row = re.compile(r"^(?P<pre>[ +!:|]*)(?P<n>\d+) (?P<name>.+?)(?:  \(in (?P<lib>[^)]+)\))?(?: \+ [^\[]*)?(?: \[[^\]]*\])?\s*$")
thr = re.compile(r"^\s+(\d+) Thread_(\d+)(.*)$")
threads = []           # (nom, total, [(profondeur, n, fonction, lib)])
cur = None
in_tree = False
for l in lines:
    if l.startswith("Call graph:"):
        in_tree = True
        continue
    if in_tree and (l.startswith("Total number in stack") or l.startswith("Sort by top of stack")
                    or l.startswith("Binary Images")):
        in_tree = False
    if not in_tree:
        continue
    m = thr.match(l)
    if m:
        cur = ["Thread_%s%s" % (m.group(2), m.group(3)), int(m.group(1)), []]
        threads.append(cur)
        continue
    if cur is None:
        continue
    m = row.match(l)
    if not m:
        continue
    depth = len(m.group("pre"))
    name = m.group("name").split("  (in ")[0].strip()
    cur[2].append((depth, int(m.group("n")), name, m.group("lib") or ""))

REPERES = [
    ("cpu_exec", r"^cpu_exec$"),
    ("  tb_gen_code (traduction)", r"^tb_gen_code$"),
    ("  code JIT (???)", r"^\?\?\?"),
    ("  tlb_fill / ppc_xlate", r"^(tlb_fill_align|ppc_cpu_tlb_fill|ppc_xlate|ppc_hash32_xlate)$"),
    ("  tlb_flush (travail)", r"^(tlb_flush_by_mmuidx_async_work|tlb_flush_by_mmuidx|tlb_flush)$"),
    ("  tcg_flush_jmp_cache", r"^tcg_flush_jmp_cache$"),
    ("  helper_lookup_tb_ptr", r"^helper_lookup_tb_ptr$"),
    ("  tb_lookup/qht", r"^(tb_lookup|tb_htable_lookup|qht_lookup_custom|qht_lookup)$"),
    ("  helper_* (tous)", r"^helper_"),
    ("  softfloat (float*/parts*)", r"^(float|parts|round_canonical|uint|int32_to|pick_nan)"),
    ("  bql / verrous", r"^(bql_lock_impl|bql_lock|qemu_mutex_lock_impl|__psynch_mutexwait|_pthread_mutex_firstfit_lock_slow)$"),
    ("  mmio (memory_region_dispatch)", r"^(memory_region_dispatch_read|memory_region_dispatch_write)$"),
    ("  io_readx/io_writex", r"^(io_readx|io_writex|do_ld_mmio_beN|do_st_mmio_leN|int_ld_mmio_beN|int_st_mmio_leN)"),
    ("  cpu_handle_interrupt", r"^cpu_handle_interrupt$"),
    ("  qemu_wait_io_event / cond_wait", r"^(qemu_wait_io_event|qemu_cond_wait_impl|__psynch_cvwait)$"),
]

def inclusive(entries, pat):
    """Somme des n des cadres qui correspondent, sans compter deux fois un
    cadre imbriqué sous un autre cadre qui correspond déjà."""
    rx = re.compile(pat)
    tot, stack = 0, []   # profondeurs des correspondances ouvertes
    for depth, n, name, lib in entries:
        while stack and stack[-1] >= depth:
            stack.pop()
        if rx.search(name):
            if not stack:
                tot += n
            stack.append(depth)
    return tot

print("== fils (échantillons ; ms si 1 éch./ms)")
for name, total, ent in sorted(threads, key=lambda t: -t[1]):
    if total < 5:
        continue
    print("%6d  %s" % (total, name[:90]))
    for lab, pat in REPERES:
        v = inclusive(ent, pat)
        if v:
            print("        %6d  %5.1f%%  %s" % (v, 100.0 * v / total, lab))

# --- self (top of stack) ---------------------------------------------------
print("\n== self (Sort by top of stack), %d premiers" % N)
start = None
for i, l in enumerate(lines):
    if l.startswith("Sort by top of stack"):
        start = i
        break
if start is not None:
    tot = 0
    ent = []
    for l in lines[start + 1:]:
        m = re.match(r"^\s+(.+?)\s+(\d+)\s*$", l)
        if not m:
            if l.strip() == "":
                continue
            break
        ent.append((m.group(1), int(m.group(2))))
        tot += int(m.group(2))
    for name, n in ent[:N]:
        print("%6d  %5.1f%%  %s" % (n, 100.0 * n / max(tot, 1), name[:100]))
    print("%6d  total" % tot)

# --- inclusif par fonction, fils vCPU seulement (ceux qui exécutent du JIT) --
print("\n== inclusif par fonction, fils vCPU (mttcg/rr_cpu_thread_fn), hors attente")
agg = collections.Counter()
busy = 0
for name, total, ent in threads:
    if inclusive(ent, r"^(mttcg_cpu_thread_fn|rr_cpu_thread_fn)$") == 0:
        continue
    busy += total - inclusive(ent, r"^(qemu_wait_io_event|qemu_cond_wait_impl|__psynch_cvwait)$")
    names = set(e[2] for e in ent)
    for fn in names:
        if fn.startswith("???"):
            continue
        agg[fn] += inclusive(ent, "^" + re.escape(fn) + "$")
print("occupé (hors attente) : %d éch." % busy)
for fn, v in agg.most_common(N + 15):
    print("%6d  %5.1f%%  %s" % (v, 100.0 * v / max(busy, 1), fn[:90]))

# --- self calculé sur l'arbre, fils vCPU : le code généré (???) regroupé -----
print("\n== self (arbre), fils vCPU, hors attente ; ??? = code généré par TCG")
selfc = collections.Counter()
for name, total, ent in threads:
    if inclusive(ent, r"^(mttcg_cpu_thread_fn|rr_cpu_thread_fn)$") == 0:
        continue
    for i, (depth, n, fn, lib) in enumerate(ent):
        # enfants directs = lignes suivantes de profondeur > depth jusqu'au retour,
        # dont la profondeur est la plus petite rencontrée
        j, mind, child = i + 1, None, 0
        while j < len(ent) and ent[j][0] > depth:
            if mind is None:
                mind = ent[j][0]
            if ent[j][0] == mind:
                child += ent[j][1]
            j += 1
        s = n - child
        if s > 0:
            selfc["??? (code généré)" if fn.startswith("???") else fn] += s
wait = sum(v for k, v in selfc.items() if k in ("__psynch_cvwait",))
tot = sum(selfc.values()) - wait
print("occupé : %d éch." % tot)
for fn, v in selfc.most_common(N + 5):
    if fn == "__psynch_cvwait":
        continue
    print("%6d  %5.1f%%  %s" % (v, 100.0 * v / max(tot, 1), fn[:90]))
