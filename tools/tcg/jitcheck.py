#!/usr/bin/env python3
"""Check the executed JIT mapping with no disk and a stopped CPU.

    jitcheck.py /path/qemu-system-ppc --runs 5 [--split-wx]
    jitcheck.py /path/qemu-system-ppc64 --runs 20 --rel32     # Linux x86-64

Uses QEMU's allocation report rather than a guessed vmmap region. Each process
is stopped at launch, terminated here, and waited for even if a check fails.

--rel32 (tcg/0024, docs/tcg-g4.md §31): x-jit-rel32=on, SMP 2 as in production;
a run is OK when QEMU reports direct helper calls. Under Linux every run also
prints the placement read from /proc/<pid>/smaps: buffer base and its 2 MiB
alignment, gap between the buffer end and the start of QEMU's text, the text
base modulo 2 MiB (the only thing ASLR changes in the relative placement), and
how many of the buffer's executable VMAs carry MADV_HUGEPAGE ("hg") and are
THP-eligible.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

MIB = 1 << 20


def placement(pid, binary):
    """Linux: buffer and text placement from /proc/<pid>/smaps."""
    vmas, cur = [], None
    with open("/proc/%d/smaps" % pid) as f:
        for line in f:
            m = re.match(r"([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)$", line)
            if m:
                cur = {"lo": int(m[1], 16), "hi": int(m[2], 16), "perm": m[3],
                       "path": m[4].strip(), "hg": False, "elig": False, "ahp": 0}
                vmas.append(cur)
            elif line.startswith("VmFlags:"):
                cur["hg"] = " hg" in line
            elif line.startswith("THPeligible:"):
                cur["elig"] = line.split()[1] == "1"
            elif line.startswith("AnonHugePages:"):
                cur["ahp"] = int(line.split()[1])
    real = os.path.realpath(binary)
    text = [v for v in vmas if v["path"] and os.path.realpath(v["path"]) == real]
    jit = [v for v in vmas if v["perm"].startswith("rwx") and not v["path"]]
    if not text or not jit:
        return "placement : carte illisible"
    t0 = min(v["lo"] for v in text)
    b0, b1 = min(v["lo"] for v in jit), max(v["hi"] for v in jit)
    near = [v for v in jit if abs(v["lo"] - t0) < (4 << 30)]
    return ("tampon %#x (%s 2 Mio) %d Mio, fin à %d Mio sous le texte %#x (texte mod 2 Mio = %#x) ; "
            "%d VMA rwx, hg %d, THP éligibles %d, AnonHugePages %d kio"
            % (b0, "aligné" if b0 % (2 * MIB) == 0 else "NON aligné", (b1 - b0) // MIB,
               (t0 - b1) // MIB if t0 > b1 else -1, t0, t0 % (2 * MIB),
               len(jit), sum(v["hg"] for v in jit), sum(v["elig"] for v in jit),
               sum(v["ahp"] for v in jit))) + ("" if near else " (loin du texte)")


def check(binary, runs, split, rel32):
    failed = False
    accel = "tcg,x-jit-near=on" + (",split-wx=on" if split else "")
    smp = []
    if rel32:
        accel = "tcg,thread=multi,x-jit-near=on,x-jit-rel32=on"
        smp = ["-smp", "2"]
    for n in range(runs):
        with tempfile.TemporaryFile(mode="w+b") as log:
            args = [str(Path(binary).resolve()), "-M", "mac99,via=pmu", "-cpu", "g4",
                    "-m", "768", *smp, "-accel", accel,
                    "-S", "-display", "none", "-monitor", "none", "-serial", "none", "-nic", "none"]
            p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            marker = "alias RX exécuté" if split else ("appels des helpers" if rel32 else "tampon JIT")
            where = ""
            try:
                deadline = time.monotonic() + 10
                report = ""
                while time.monotonic() < deadline:
                    log.seek(0); report = log.read().decode(errors="replace")
                    if marker in report or p.poll() is not None:
                        break
                    time.sleep(0.1)
                if sys.platform.startswith("linux") and p.poll() is None:
                    time.sleep(0.3)         # régions et mprotect posés après le rapport
                    where = placement(p.pid, binary)
            finally:
                if p.poll() is None:
                    p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait()
            lines = [l for l in report.splitlines() if marker in l]
            if rel32:
                good = len(lines) == 1 and "directs (rel32)" in lines[0]
            else:
                good = len(lines) == 1 and "même fenêtre" in lines[0] and "AUTRE" not in lines[0]
            print(f"{n + 1}: {'OK' if good else 'FAIL'} " + (lines[0] if lines else report.strip()))
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
                    help="x-jit-rel32=on, SMP 2 (pass the qemu-system-ppc64 binary)")
    a = ap.parse_args()
    if a.runs < 1:
        ap.error("--runs must be positive")
    raise SystemExit(check(a.binary, a.runs, a.split_wx, a.rel32))
