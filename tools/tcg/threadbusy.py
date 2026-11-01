#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""threadbusy.py [--fonctions N] FICHIER… — occupation des fils d'un `sample` macOS de QEMU
(docs/smp-coeurs.md).

Pour chaque fil, les échantillons « en attente » sont ceux dont le sommet de pile
(self) est un appel bloquant du noyau (__psynch_cvwait, semaphore_wait_trap,
__select, poll, kevent, mach_msg_trap, __workq_kernreturn, __semwait_signal…) ;
le reste est « occupé ». Imprime, par fil occupé à plus de 1 %, la part d'un cœur
hôte (échantillons occupés / maximum d’échantillons par fil), puis la somme. C'est une
estimation par échantillonnage, pas un relevé de temps CPU. --fonctions N ajoute
les N premiers coûts propres de chaque fil actif, en % d'un cœur : les attentes
sont séparées du travail. Une attente du pilote GPU ne mesure pas le temps GPU."""
import argparse
import collections
import re

WAIT = re.compile(r"^(__psynch_cvwait|semaphore_wait_trap|semaphore_timedwait_trap|semaphore_wait_signal_trap|semaphore_timedwait_signal_trap|"
                  r"__select|__pselect|poll|__poll|kevent|kevent64|kevent_id|"
                  r"mach_msg_trap|mach_msg2_trap|mach_msg_overwrite_trap|"
                  r"__workq_kernreturn|__semwait_signal|__sigsuspend|__sigwait|"
                  r"__psynch_mutexwait|__ulock_wait|__ulock_wait2|nanosleep|__read_nocancel)$")
row = re.compile(r"^(?P<pre>[ +!:|]*)(?P<n>\d+) (?P<name>.+)")
thr = re.compile(r"^\s+(\d+) Thread_(\d+)(.*)$")


def flush(cur, stack, depth=-1):
    while stack and stack[-1][0] >= depth:
        d, sn, sfn, kids = stack.pop()
        if stack:
            stack[-1][3] += sn
        if cur is not None and sn - kids > 0:
            cur["self"][sfn] += sn - kids
            if WAIT.match(sfn):
                cur["wait"] += sn - kids


def parse(path):
    threads, cur, stack, in_tree = [], None, [], False
    with open(path, errors="replace") as lines:
        for l in lines:
            l = l.rstrip("\n")
            if l.startswith("Call graph:"):
                in_tree = True
                continue
            if in_tree and (l.startswith("Total number in stack") or l.startswith("Sort by top")
                            or l.startswith("Binary Images")):
                break
            if not in_tree:
                continue
            m = thr.match(l)
            if m:
                flush(cur, stack)
                name = m.group(3).strip(" :") or ""
                name = re.sub(r"\s+DispatchQueue.*", "", name)
                cur = {"id": m.group(2), "name": name, "total": int(m.group(1)), "wait": 0,
                       "self": collections.Counter()}
                threads.append(cur); stack = []
                continue
            m = row.match(l)
            if not m or cur is None:
                continue
            depth, n = len(m.group("pre")), int(m.group("n"))
            fn = m.group("name").split("  (in ", 1)[0].strip()
            flush(cur, stack, depth)
            stack.append([depth, n, fn, 0])
    flush(cur, stack)                  # même si le relevé n'a pas de pied de page
    return threads


def merged_threads(threads):
    # un fil peut apparaître en plusieurs blocs (files de dispatch) : fusion par id
    merged = {}
    for t in threads:
        m = merged.setdefault(t["id"], {"name": t["name"], "total": 0, "wait": 0,
                                       "self": collections.Counter()})
        m["total"] += t["total"]; m["wait"] += t["wait"]
        m["self"].update(t["self"])
        if t["name"] and not m["name"]:
            m["name"] = t["name"]
    return merged


def report(path, functions=0):
    merged = merged_threads(parse(path))
    dur = max((t["total"] for t in merged.values()), default=0)
    print("== %s (%d échantillons par fil)" % (path, dur))
    s = 0.0
    for tid, t in sorted(merged.items(), key=lambda kv: -(kv[1]["total"] - kv[1]["wait"])):
        busy = (t["total"] - t["wait"]) / dur if dur else 0
        s += busy
        if busy > 0.01:
            print("  %5.1f %%  %s %s" % (100 * busy, tid, t["name"] or "(sans nom)"))
            if functions:
                work = [(fn, n) for fn, n in t["self"].most_common() if not WAIT.match(fn)]
                print("      attente noyau : %.1f %%" % (100 * t["wait"] / dur))
                for fn, n in work[:functions]:
                    print("      %5.1f %%  %s" % (100 * n / dur, fn))
    print("  somme : %.2f cœur hôte" % s)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fonctions", type=int, default=0, metavar="N")
    ap.add_argument("fichiers", nargs="+")
    args = ap.parse_args()
    if args.fonctions < 0:
        ap.error("--fonctions doit être positif ou nul")
    for path in args.fichiers:
        report(path, args.fonctions)


if __name__ == "__main__":
    main()
