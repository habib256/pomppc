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
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

GIB = 1 << 30


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
            good, line = False, ""
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
