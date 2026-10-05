#!/usr/bin/env python3
"""Rank emitted ARM64 block costs by executions, from hotblocks + out_asm.

    jitblocks.py hotblocks.csv jit.log --top 30

PCs may have several translations (flags, recompilation). Counts are grouped
by PC; emitted-code ranges retain that ambiguity. No attribution to a helper
is inferred from an indirect BLR. x19 is TCG_AREG0 on QEMU aarch64.
"""
import argparse
import csv
import re
from collections import defaultdict


def read_code(path):
    code = defaultdict(list)
    block = None
    address = 0
    decoder = None
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("----------------"):
                block = None
            m = re.match(r"OUT: \[size=(\d+)\]", line)
            if m:
                block = dict(bytes=int(m[1]), pc=None, arm=0, calls=0, env=0)
                continue
            if block is None:
                continue
            m = re.search(r"-- guest addr 0x([\da-fA-F]+)", line)
            if m and block["pc"] is None:
                block["pc"] = int(m[1], 16)
                code[block["pc"]].append(block)
            if line.startswith("OBJD-H:"):
                if decoder is None:
                    try:
                        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
                    except ImportError as exc:
                        raise RuntimeError("raw OBJD-H log requires Python capstone") from exc
                    decoder = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
                raw = bytes.fromhex(line.split(":", 1)[1].strip())
                for insn in decoder.disasm(raw, address):
                    block["arm"] += 1
                    block["calls"] += insn.mnemonic in ("bl", "blr")
                    block["env"] += insn.mnemonic.startswith(("ld", "st")) and bool(
                        re.search(r"\[x19(?:,|\])", insn.op_str))
                address += len(raw)
                continue
            addr = re.match(r"\s*0x([\da-fA-F]+):", line)
            if addr:
                address = int(addr[1], 16)
            m = re.match(r"\s*0x[\da-fA-F]+:\s+(?:(?:[\da-fA-F]{8}|[\da-fA-F]{2})\s+)+([a-z][a-z0-9.]*)\s*(.*)", line)
            if m:
                op, args = m.groups()
                block["arm"] += 1
                block["calls"] += op in ("bl", "blr")
                block["env"] += op.startswith(("ld", "st")) and bool(re.search(r"\[x19(?:,|\])", args))
    return code


def report(count_path, code_path, top, pc_min=0, pc_max=(1 << 64) - 1):
    counts = defaultdict(int)
    with open(count_path) as f:
        lines = f.readlines()
        for line in lines:
            match = re.match(r"# skipped_translations=(\d+)", line)
            if match and int(match[1]):
                print(f"# profile truncated: {match[1]} translations not instrumented")
        rows = csv.DictReader(line for line in lines if not line.startswith("#"))
        for row in rows:
            pc, n = int(row["pc"], 16), int(row["executions"])
            if pc_min <= pc <= pc_max and n:
                counts[pc] += n
    code = read_code(code_path)
    print("pc executions ARM/execution direct-x19-accesses/execution calls/execution translations")
    ranked = sorted(counts, key=lambda pc: counts[pc] * max(
        (b["arm"] for b in code[pc]), default=0), reverse=True)
    for pc in ranked[:top]:
        blocks = code[pc]
        def span(key):
            vals = [b[key] for b in blocks]
            return "?" if not vals else str(min(vals)) if min(vals) == max(vals) else f"{min(vals)}..{max(vals)}"
        print(f"0x{pc:08x} {counts[pc]} {span('arm')} {span('env')} {span('calls')} {len(blocks)}")
    missing = sum(n for pc, n in counts.items() if not code[pc])
    print(f"# executions without emitted code: {missing}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("counts"); ap.add_argument("assembly")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--pc-min", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--pc-max", type=lambda x: int(x, 0), default=(1 << 64) - 1)
    a = ap.parse_args()
    report(a.counts, a.assembly, a.top, a.pc_min, a.pc_max)
