#!/usr/bin/env python3
"""Check QEMU's actual C hash against its emitted TCG hash and page buckets.

    jcwordproof.py /path/to/patched/qemu

Extracts the C functions and interprets the actual TCG integer operations.
Both variants, every supported cache size and page-group size (tcg/0060,
x-jc-page-bits: 6 to 9 bits), all offsets of representative
32/64-bit pages, plus deterministic random PCs. Does not replace VM tests
of TB invalidation or MMU changes.
"""
import ctypes
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

src = Path(sys.argv[1])
header = (src / "accel/tcg/tb-hash.h").read_text()
start = header.index("static inline unsigned int tb_jmp_cache_hash_page")
end = header.index("\n#else", start)
functions = header[start:end]
translator = (src / "accel/tcg/translator.c").read_text()
start = translator.index("    tcg_gen_shri_i64(t, pc, sh);", translator.index("void translator_lookup_and_goto_ptr_inline"))
end = translator.index("    tcg_gen_or_i64(h, h, t);", start) + len("    tcg_gen_or_i64(h, h, t);")
operations = translator[start:end].splitlines()


def emitted(pc, bits, word, pbits):
    values = dict(pc=pc, sh=12 - pbits,
                  TB_JMP_PAGE_MASK=(1 << bits) - (1 << pbits),
                  TB_JMP_ADDR_MASK=(1 << pbits) - 1)
    active = True
    for line in operations:
        line = line.strip()
        if line == "if (tb_jmp_cache_word) {":
            active = word
        elif line == "} else {":
            active = not word
        elif line == "}":
            active = True
        elif active and line.startswith("tcg_gen_"):
            m = re.fullmatch(r"tcg_gen_(shri|xor|andi|or)_i64\((\w+), (\w+), (\w+)\);", line)
            if not m:
                raise ValueError("unmodelled TCG op: " + line)
            op, dest, a, b = m.groups()
            a = values[a]; b = int(b) if b.isdigit() else values[b]
            values[dest] = {"shri": lambda: a >> b, "xor": lambda: a ^ b,
                            "andi": lambda: a & b, "or": lambda: a | b}[op]()
    return values["h"]


with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    (p / "proof.c").write_text('''#include <stdint.h>
#include <stdbool.h>
typedef uint64_t vaddr;
unsigned tb_jmp_cache_bits;
unsigned tb_jmp_page_bits;
bool tb_jmp_cache_word;
#define TARGET_PAGE_BITS 12
#define TB_JMP_PAGE_BITS (tb_jmp_page_bits)
#define TB_JMP_ADDR_MASK ((1u << tb_jmp_page_bits) - 1)
#define TB_JMP_PAGE_MASK ((1u << tb_jmp_cache_bits) - (1u << tb_jmp_page_bits))
''' + functions + '''
unsigned hash(uint64_t pc) { return tb_jmp_cache_hash_func(pc); }
unsigned page(uint64_t pc) { return tb_jmp_cache_hash_page(pc); }
void config(unsigned bits, bool word, unsigned pbits)
{ tb_jmp_cache_bits = bits; tb_jmp_cache_word = word; tb_jmp_page_bits = pbits; }
''')
    subprocess.run(["cc", "-O2", "-Wall", "-Werror", "-shared", "-fPIC",
                    str(p / "proof.c"), "-o", str(p / "proof.so")], check=True)
    lib = ctypes.CDLL(str(p / "proof.so"))
    lib.hash.argtypes = lib.page.argtypes = [ctypes.c_uint64]
    lib.config.argtypes = [ctypes.c_uint, ctypes.c_bool, ctypes.c_uint]
    pages = [0, 0x1000, 0xfffff000, 0x100000000, 0x7fffffff000,
             0xfffffffffffff000]
    rng = random.Random(0x5eed)
    pages += [rng.getrandbits(64) & ~4095 for _ in range(8)]
    cases = 0
    spread = {}
    for pbits in range(6, 10):
        n = 1 << pbits
        for bits in range(12, 17):
            for word in (False, True):
                lib.config(bits, word, pbits)
                for page in pages:
                    bucket = lib.page(page)
                    slots = set()
                    for off in range(4096):
                        pc = page + off
                        actual = lib.hash(pc)
                        assert actual == emitted(pc, bits, word, pbits), (pbits, bits, word, hex(pc))
                        assert actual & ~(n - 1) == bucket, (pbits, bits, word, hex(pc), "page")
                        if word and not off & 3:
                            slots.add(actual)
                        if not word and pbits == 6:
                            tmp = pc ^ (pc >> 6)
                            assert actual == ((tmp >> 6) & ((1 << bits) - 64)) | (tmp & 63)
                        cases += 1
                    if word:
                        # the 1024 instruction slots of a page reach every slot of its group
                        assert len(slots) == n, (pbits, bits, hex(page), len(slots))
                        spread[pbits] = n
    print(f"C hash = emitted TCG, page buckets preserved: {cases} cases, zero differences;"
          f" word hash fills the whole group: {sorted(spread.values())} slots")
