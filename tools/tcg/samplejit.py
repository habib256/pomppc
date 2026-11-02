#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Rend un sample macOS aux PC invités du même journal QEMU out_asm (hôte arm64).

samplejit.py SAMPLE OUT_ASM [--helper REGEX] [--json FICHIER]
Sans --helper : coûts propres des cadres JIT ??? des fils TCG. Avec --helper :
coûts propres des fonctions choisies, rendus à leur appelant JIT. Aucun mélange
de processus : ASLR et réutilisation du tampon interdisent une carte d'une autre VM.
Les groupes de plusieurs adresses ne sont attribués que s'ils désignent tous le
même PC invité. Les adresses réutilisées pour des PC différents restent ambiguës.
Le journal est parcouru en flux, seules les adresses utiles sont conservées.
"""
import argparse
import bisect
import collections
import json
import re

from threadbusy import row, thr


def samples(path, helper=None):
    """(nom, coût propre, adresses JIT, appelant plutôt que PC interrompu)."""
    selected, stack, active, in_tree = [], [], False, False

    def flush(depth=-1):
        while stack and stack[-1][0] >= depth:
            _, n, name, pcs, children = stack.pop()
            if stack:
                stack[-1][4] += n
            own = n - children
            if not active or own <= 0:
                continue
            if helper is None and name.startswith("???"):
                selected.append((name, own, pcs, False))
            elif helper is not None and helper.search(name):
                caller = next((t[3] for t in reversed(stack) if t[2].startswith("???")), ())
                selected.append((name, own, caller, True))

    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("Call graph:"):
                in_tree = True
                continue
            if not in_tree:
                continue
            if line.startswith(("Total number in stack", "Sort by top", "Binary Images")):
                break
            t = thr.match(line)
            if t:
                flush()
                active = "TCG" in t.group(3)
                continue
            m = row.match(line.rstrip("\n"))
            if not m:
                continue
            depth = len(m.group("pre"))
            flush(depth)
            name = m.group("name").split("  (in ", 1)[0].strip()
            addresses = re.findall(r"\[(0x[^\]]+)\]", line)
            pcs = tuple(int(a, 16) for a in re.findall(r"0x[0-9a-fA-F]+", addresses[-1])) if addresses else ()
            stack.append([depth, int(m.group("n")), name, pcs, 0])
    flush()
    return selected


def mapping(path, needles):
    """PC hôte -> ensemble (PC invité, zone, instruction arm64).

    [D] Format QEMU v11.1.2, accel/tcg/translate-all.c, journal OUT : premier
    morceau = première instruction + prologue ; chemins lents et données à part.
    Accepte OBJD-H et la forme désassemblée arm64 à mots de 32 bits.
    """
    keys = sorted(needles)
    found = collections.defaultdict(set)
    active, guest, kind, address = False, None, None, None

    def record(start, data):
        lo, hi = bisect.bisect_left(keys, start), bisect.bisect_left(keys, start + len(data))
        for pc in keys[lo:hi]:
            offset = pc - start
            if offset % 4 == 0 and offset + 4 <= len(data):
                word = int.from_bytes(data[offset:offset + 4], "little")
                found[pc].add((guest, kind, word))

    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("OUT:"):
                active, guest, kind, address = True, None, None, None
                continue
            if line.startswith(("IN:", "PROLOGUE:", "----------------")):
                active = False
            if not active:
                continue
            m = re.match(r"\s*-- guest addr 0x([0-9a-fA-F]+)", line)
            if m:
                guest = int(m[1], 16)
                kind = "première+prologue" if "prologue" in line else "instruction"
                address = None
                continue
            if "-- tb slow" in line or line.lstrip().startswith("data:"):
                guest, kind, address = None, "lent/données", None
                continue
            m = re.match(r"\s*0x([0-9a-fA-F]+):\s*(.*)", line)
            if m:
                address = int(m[1], 16)
                word = re.match(r"([0-9a-fA-F]{8})(?:\s|$)", m[2])
                if word:
                    record(address, int(word[1], 16).to_bytes(4, "little"))
                continue
            if line.startswith("OBJD-H:") and address is not None:
                data = bytes.fromhex(line.split(":", 1)[1].strip())
                record(address, data)
                address += len(data)
    return found


def call(word):
    # [D] QEMU v11.1.2 tcg/aarch64/tcg-target.c.inc : BL imm26 et BLR Xn.
    return word & 0xfc000000 == 0x94000000 or word & 0xfffffc1f == 0xd63f0000


def attribute(selected, mapped):
    costs, missing = collections.Counter(), collections.Counter()
    for _, n, pcs, caller in selected:
        resolved = set()
        reason = None
        for pc in pcs:
            positions = (pc, pc - 4) if caller else (pc,)
            candidates = set()
            for pos in positions:
                entries = mapped.get(pos, ())
                identities = {(g, k) for g, k, _ in entries}
                if len(identities) > 1:
                    reason = "adresse réutilisée"
                    break
                for g, k, w in entries:
                    if g is not None and (not caller or call(w)):
                        candidates.add((g, k))
            if reason:
                break
            if len(candidates) != 1:
                reason = "appel ambigu" if candidates else "hors carte/appel non confirmé"
                break
            resolved.update(candidates)
        if not pcs:
            reason = "aucun cadre JIT/adresse"
        if len(resolved) > 1:
            reason = "groupe de PC distincts"
        if reason or len(resolved) != 1:
            missing[reason or "hors carte"] += n
        else:
            costs[next(iter(resolved))] += n
    return costs, missing


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("sample"); ap.add_argument("asm")
    ap.add_argument("--helper", help="regex des fonctions dont attribuer le coût propre à l'appelant JIT")
    ap.add_argument("--json")
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()
    selected = samples(a.sample, re.compile(a.helper) if a.helper else None)
    needles = {pc - d for _, _, pcs, caller in selected for pc in pcs for d in ((0, 4) if caller else (0,))}
    mapped = mapping(a.asm, needles)
    costs, missing = attribute(selected, mapped)
    total = sum(n for _, n, _, _ in selected)
    print("%d échantillons propres sélectionnés ; %d attribués ; %d non attribués" %
          (total, sum(costs.values()), sum(missing.values())))
    for (pc, kind), n in costs.most_common(a.top):
        print("%6.2f %%  %6d  0x%08x  %s" % (100 * n / max(total, 1), n, pc, kind))
    for reason, n in missing.most_common():
        print("non attribué : %d (%s)" % (n, reason))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(dict(total=total, attribues=[dict(pc=hex(pc), zone=k, n=n)
                      for (pc, k), n in costs.most_common()], non_attribues=dict(missing)), f, indent=2)
            f.write("\n")


if __name__ == "__main__":
    main()
