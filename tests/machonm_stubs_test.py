#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Stubs Mach-O : index indirects, imports, marqueurs LOCAL/ABS et tranche ppc."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/re"))
import machonm


def section(name, address, size, flags=0, first=0, stride=0):
    return struct.pack(">16s16s9I", name.encode(), b"__TEXT", address, size, 0,
                       2, 0, 0, flags, first, stride)


def fixture(bad_index=None):
    # Les stubs ne commencent pas au premier index indirect. Le symbole défini
    # ne doit pas disparaître de -n, et les imports ne doivent pas y entrer.
    sections = (section("__text", 0x1000, 32) +
                section("__picsymbolstub1", 0x2000, 128, 8, 1, 32) +
                section("__la_symbol_ptr", 0x3000, 4, 7))
    segment = struct.pack(">II16s8I", 1, 56 + len(sections), b"__TEXT",
                          0x1000, 0x3000, 0, 0, 7, 5, 3, 0) + sections
    strings = b"\0_memcmp\0_defined\0_pthread_mutex_lock\0"
    names = (strings.index(b"_memcmp"), strings.index(b"_defined"),
             strings.index(b"_pthread_mutex_lock"))
    symbols = (struct.pack(">IBBhI", names[0], 1, 0, 0, 0) +
               struct.pack(">IBBhI", names[1], 0xe, 1, 0, 0x1000) +
               struct.pack(">IBBhI", names[2], 1, 0, 0, 0))
    symoff = 28 + len(segment) + 24 + 80
    stroff = symoff + len(symbols)
    indirectoff = stroff + len(strings)
    symtab = struct.pack(">6I", 2, 24, symoff, 3, stroff, len(strings))
    dysym = [0xb, 80] + [0] * 18
    dysym[14:16] = [indirectoff, 5]
    indirect = [2, 0, 0x80000000, 0x40000000, 2]
    if bad_index is not None:
        indirect[1] = bad_index
    header = struct.pack(">7I", 0xfeedface, 18, 0, 8, 3,
                         len(segment) + len(symtab) + 80, 0)
    return (header + segment + symtab + struct.pack(">20I", *dysym) + symbols +
            strings + struct.pack(">5I", *indirect))


class MachOStubsTest(unittest.TestCase):
    def read(self, blob):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "bundle"
            p.write_bytes(blob)
            return machonm.MachO(p)

    def test_imports_and_local_absolute_entries(self):
        obj = self.read(fixture())
        self.assertEqual(obj.stubs(), [(0x2000, 32, "_memcmp"),
                                       (0x2060, 32, "_pthread_mutex_lock")])
        self.assertEqual(obj.symboles(), [(0x1000, "t", "_defined")])

    def test_universal_selects_ppc(self):
        ppc = fixture()
        header = (struct.pack(">II", 0xcafebabe, 2) +
                  struct.pack(">5I", 7, 3, 64, 4, 2) +
                  struct.pack(">5I", 18, 0, 128, len(ppc), 2))
        self.assertEqual(self.read(header.ljust(128, b"\0") + ppc).stubs(),
                         self.read(ppc).stubs())

    def test_bad_symbol_index_is_refused(self):
        with self.assertRaisesRegex(ValueError, "index de symbole"):
            self.read(fixture(bad_index=99)).stubs()


if __name__ == "__main__":
    unittest.main()
