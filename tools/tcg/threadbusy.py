#!/usr/bin/env python3
"""threadbusy.py FICHIER… — occupation de chaque fil d'un `sample` macOS de QEMU
(docs/smp-coeurs.md).

Pour chaque fil, les échantillons « en attente » sont ceux dont le sommet de pile
(self) est un appel bloquant du noyau (__psynch_cvwait, semaphore_wait_trap,
__select, poll, kevent, mach_msg_trap, __workq_kernreturn, __semwait_signal…) ;
le reste est « occupé ». Imprime, par fil occupé à plus de 1 %, la part d'un cœur
hôte (échantillons occupés / durée), puis la somme : le nombre de cœurs de l'hôte
que le processus QEMU consomme réellement."""
import re, sys

WAIT = re.compile(r"^(__psynch_cvwait|semaphore_wait_trap|semaphore_timedwait_trap|semaphore_wait_signal_trap|semaphore_timedwait_signal_trap|"
                  r"__select|__pselect|poll|__poll|kevent|kevent64|kevent_id|"
                  r"mach_msg_trap|mach_msg2_trap|mach_msg_overwrite_trap|"
                  r"__workq_kernreturn|__semwait_signal|__sigsuspend|__sigwait|"
                  r"__psynch_mutexwait|__ulock_wait|__ulock_wait2|nanosleep|__read_nocancel)$")
row = re.compile(r"^(?P<pre>[ +!:|]*)(?P<n>\d+) (?P<name>\S+)")
thr = re.compile(r"^\s+(\d+) Thread_(\d+)(.*)$")


def flush(cur, stack, depth=-1):
    while stack and stack[-1][0] >= depth:
        d, sn, sfn, kids = stack.pop()
        if stack:
            stack[-1][3] += sn
        if cur is not None and sn - kids > 0 and WAIT.match(sfn):
            cur["wait"] += sn - kids


def parse(path):
    threads, cur, stack, in_tree = [], None, [], False
    for l in open(path, errors="replace"):
        l = l.rstrip("\n")
        if l.startswith("Call graph:"):
            in_tree = True
            continue
        if in_tree and (l.startswith("Total number in stack") or l.startswith("Sort by top")
                        or l.startswith("Binary Images")):
            flush(cur, stack)
            break
        if not in_tree:
            continue
        m = thr.match(l)
        if m:
            flush(cur, stack)
            name = m.group(3).strip(" :") or ""
            name = re.sub(r"\s+DispatchQueue.*", "", name)
            cur = {"id": m.group(2), "name": name, "total": int(m.group(1)), "wait": 0}
            threads.append(cur); stack = []
            continue
        m = row.match(l)
        if not m or cur is None:
            continue
        depth, n, fn = len(m.group("pre")), int(m.group("n")), m.group("name")
        flush(cur, stack, depth)
        stack.append([depth, n, fn, 0])
    return threads


for path in sys.argv[1:]:
    # un fil peut apparaître en plusieurs blocs (files de dispatch) : fusion par id
    merged = {}
    ths = parse(path)
    dur = max(t["total"] for t in ths) if ths else 0
    for t in ths:
        m = merged.setdefault(t["id"], {"name": t["name"], "total": 0, "wait": 0})
        m["total"] += t["total"]; m["wait"] += t["wait"]
        if t["name"] and not m["name"]:
            m["name"] = t["name"]
    print("== %s (%d échantillons par fil)" % (path, dur))
    s = 0.0
    for tid, t in sorted(merged.items(), key=lambda kv: -(kv[1]["total"] - kv[1]["wait"])):
        busy = (t["total"] - t["wait"]) / dur if dur else 0
        s += busy
        if busy > 0.01:
            print("  %5.1f %%  %s %s" % (100 * busy, tid, t["name"] or "(sans nom)"))
    print("  somme : %.2f cœur hôte" % s)
