#!/usr/bin/env python3
"""Check the executed JIT mapping with no disk and a stopped CPU.

    jitcheck.py /path/qemu-system-ppc --runs 5 [--split-wx] [--rel32]

Uses QEMU's allocation report rather than a guessed vmmap region. Each process
is stopped at launch, terminated here, and waited for even if a check fails.

Modes (the verdict of each run):
  default   x-jit-near=on: the executed buffer in the 4 GiB window of the text
            ("même fenêtre"; on macOS with --split-wx, the RX alias, tcg/0027)
  --rel32   x-jit-rel32=on (tcg/0024, Linux x86-64): every helper call direct,
            the whole buffer within rel32 reach of the text ("directs (rel32)")

Linux, --split-wx: QEMU takes the memfd path (tcg/region.c), which neither
x-jit-near nor x-jit-rel32 nor tcg/0027 (Darwin only) touches and which prints
no placement line. The RX view is then read from /proc/<pid>/maps (the
"memfd:tcg-jit" r-x mapping) and judged against the text mapping of the
binary with the same criterion as the mode; the report says so.

Linux, without --split-wx: each run also prints, on an indented second line,
the placement read from /proc/<pid>/smaps (docs/tcg-g4.md §31): buffer base and
its 2 MiB alignment, gap between the buffer end and the start of QEMU's text,
the text base modulo 2 MiB (the only thing ASLR changes in the relative
placement), and how many of the buffer's rwx VMAs carry MADV_HUGEPAGE ("hg")
and are THP-eligible. Informational: it does not change the verdict.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

GIB = 1 << 30
MIB = 1 << 20


def smaps_placement(pid, binary):
    """Linux, anonymous rwx buffer: placement and THP state from /proc/<pid>/smaps."""
    vmas, cur = [], None
    try:
        f = open(f"/proc/{pid}/smaps")
    except OSError:
        return None
    with f:
        for line in f:
            m = re.match(r"([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)$", line)
            if m:
                cur = {"lo": int(m[1], 16), "hi": int(m[2], 16), "perm": m[3],
                       "path": m[4].strip(), "hg": False, "elig": False, "ahp": 0}
                vmas.append(cur)
            elif cur is None:
                continue
            elif line.startswith("VmFlags:"):
                cur["hg"] = " hg" in line
            elif line.startswith("THPeligible:"):
                cur["elig"] = line.split()[1] == "1"
            elif line.startswith("AnonHugePages:"):
                cur["ahp"] = int(line.split()[1])
    text = [v for v in vmas if v["path"] == binary]
    jit = [v for v in vmas if v["perm"].startswith("rwx") and not v["path"]]
    if not text or not jit:
        return None
    t0 = min(v["lo"] for v in text)
    b0, b1 = min(v["lo"] for v in jit), max(v["hi"] for v in jit)
    gap = f"fin à {(t0 - b1) // MIB} Mio sous le texte" if t0 > b1 else "loin du texte"
    return (f"placement : tampon {b0:#x} ({'aligné' if b0 % (2 * MIB) == 0 else 'NON aligné'} "
            f"2 Mio) {(b1 - b0) // MIB} Mio, {gap} {t0:#x} (texte mod 2 Mio = {t0 % (2 * MIB):#x}) ; "
            f"{len(jit)} VMA rwx, hg {sum(v['hg'] for v in jit)}, "
            f"THP éligibles {sum(v['elig'] for v in jit)}, "
            f"AnonHugePages {sum(v['ahp'] for v in jit)} kio")


def maps_report(pid, binary, rel32):
    """Linux: RX view of the split-wx memfd and text mapping, from /proc."""
    try:
        lines = open(f"/proc/{pid}/maps").read().splitlines()
    except OSError:
        return None
    rx = text = None
    for l in lines:
        f = l.split()
        lo, hi = (int(x, 16) for x in f[0].split("-"))
        if "memfd:tcg-jit" in l and "x" in f[1]:
            rx = (lo, hi)
        elif len(f) >= 6 and f[5] == binary and "x" in f[1]:
            text = (lo, hi)
    if not rx or not text:
        return None
    if rel32:
        far = max(text[1], rx[1]) - min(text[0], rx[0])
        good = far < 2 * GIB
        verdict = f"écart maximal {far >> 20} Mio : {'directs (rel32)' if good else 'INDIRECTS'}"
    else:
        good = rx[0] >> 32 == text[0] >> 32 and (rx[1] - 1) >> 32 == text[0] >> 32
        verdict = ("même" if good else "AUTRE") + " fenêtre de 4 Gio que le texte"
    return good, (f"linux split-wx (memfd, sans x-jit-near/rel32 ni 0027) : "
                  f"RX {rx[0]:#x}-{rx[1]:#x}, texte {text[0]:#x}-{text[1]:#x}, {verdict}")


def check(binary, runs, split, rel32):
    failed = False
    binary = str(Path(binary).resolve())
    linux = sys.platform.startswith("linux")
    accel = "tcg," + ("x-jit-rel32=on" if rel32 else "x-jit-near=on")
    if split:
        accel += ",split-wx=on"
    for n in range(runs):
        with tempfile.TemporaryFile(mode="w+b") as log:
            args = [binary, "-M", "mac99,via=pmu", "-cpu", "g4",
                    "-m", "768", "-accel", accel,
                    "-S", "-display", "none", "-monitor", "none", "-serial", "none", "-nic", "none"]
            p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            if rel32:
                marker = "appels des helpers"
            else:
                marker = "alias RX exécuté" if split else "tampon JIT"
            good, line, where = False, "", None
            try:
                deadline = time.monotonic() + 10
                report = ""
                while time.monotonic() < deadline:
                    log.seek(0); report = log.read().decode(errors="replace")
                    if p.poll() is not None:
                        break
                    if split and linux:
                        r = maps_report(p.pid, binary, rel32)
                        if r:
                            good, line = r
                            break
                    elif marker in report:
                        if linux and p.poll() is None:
                            time.sleep(0.3)     # régions et pages de garde posées après le rapport
                            where = smaps_placement(p.pid, binary)
                        break
                    time.sleep(0.1)
            finally:
                if p.poll() is None:
                    p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait()
            if not (split and linux):
                lines = [l for l in report.splitlines() if marker in l]
                line = lines[0] if lines else report.strip()
                if rel32:
                    good = len(lines) == 1 and "directs (rel32)" in lines[0]
                else:
                    good = len(lines) == 1 and "même fenêtre" in lines[0] and "AUTRE" not in lines[0]
                if rel32 and good:
                    place = [l for l in report.splitlines() if "tampon JIT" in l]
                    line = (place[0] + " | " if place else "") + line
            print(f"{n + 1}: {'OK' if good else 'FAIL'} " + (line or report.strip()))
            if where:
                print("   " + where)
            failed |= not good
    return int(failed)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary"); ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--split-wx", action="store_true")
    ap.add_argument("--rel32", action="store_true",
                    help="x-jit-rel32 (tcg/0024) au lieu de x-jit-near")
    a = ap.parse_args()
    if a.runs < 1:
        ap.error("--runs must be positive")
    raise SystemExit(check(a.binary, a.runs, a.split_wx, a.rel32))
