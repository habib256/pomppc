#!/usr/bin/env python3
"""Check the executed JIT mapping with no disk and a stopped CPU.

    jitcheck.py /path/qemu-system-ppc --runs 5 [--split-wx]

Uses QEMU's allocation report rather than a guessed vmmap region. Each process
is stopped at launch, terminated here, and waited for even if a check fails.
"""
import argparse
import subprocess
import tempfile
import time
from pathlib import Path


def check(binary, runs, split):
    failed = False
    for n in range(runs):
        with tempfile.TemporaryFile(mode="w+b") as log:
            args = [str(Path(binary).resolve()), "-M", "mac99,via=pmu", "-cpu", "g4",
                    "-m", "768", "-accel", "tcg,x-jit-near=on" + (",split-wx=on" if split else ""),
                    "-S", "-display", "none", "-monitor", "none", "-serial", "none", "-nic", "none"]
            p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            marker = "alias RX exécuté" if split else "tampon JIT"
            try:
                deadline = time.monotonic() + 10
                report = ""
                while time.monotonic() < deadline:
                    log.seek(0); report = log.read().decode(errors="replace")
                    if marker in report or p.poll() is not None:
                        break
                    time.sleep(0.1)
            finally:
                if p.poll() is None:
                    p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait()
            lines = [l for l in report.splitlines() if marker in l]
            good = len(lines) == 1 and "même fenêtre" in lines[0] and "AUTRE" not in lines[0]
            print(f"{n + 1}: {'OK' if good else 'FAIL'} " + (lines[0] if lines else report.strip()))
            failed |= not good
    return int(failed)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("binary"); ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--split-wx", action="store_true")
    a = ap.parse_args()
    if a.runs < 1:
        ap.error("--runs must be positive")
    raise SystemExit(check(a.binary, a.runs, a.split_wx))
