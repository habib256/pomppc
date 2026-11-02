#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Attribution d'un sample arm64 : coûts propres, appelants et ambiguïtés."""
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/tcg"))
import samplejit as J


class SampleJitTest(unittest.TestCase):
    def test_self_and_helper_callers(self):
        text = """Call graph:
    10 Thread_1: CPU 0/TCG
      10 root
        10 ???  (in <unknown binary>)  [0x1004]
          6 cpu_ppc_load_tbu  (in qemu)  [0x9000]
            4 tbf_load  (in qemu)  [0x9010,0x9014]
    9 Thread_2: main
      9 ???  (in <unknown binary>)  [0x2000]
"""
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "sample"; p.write_text(text)
            own = J.samples(p)
            helpers = J.samples(p, re.compile(r"^(cpu_ppc_load_tbu|tbf_load)$"))
        self.assertEqual(sum(n for _, n, _, _ in own), 4)
        self.assertEqual(sum(n for _, n, _, _ in helpers), 6)
        self.assertEqual({pcs for _, _, pcs, _ in helpers}, {(0x1004,)})
        mapped = {0x1000: {(0x200, "instruction", 0xd63f0000)},
                  0x1004: {(0x204, "instruction", 0xd503201f)}}
        costs, missing = J.attribute(helpers, mapped)
        self.assertEqual(costs, {(0x200, "instruction"): 6})
        self.assertFalse(missing)
        self.assertEqual(J.attribute(own, mapped)[0], {(0x204, "instruction"): 4})

    def test_adjusted_callsite_and_non_call(self):
        item = [("clock", 3, (0x1000,), True)]
        self.assertEqual(J.attribute(item, {0x1000: {(0x200, "instruction", 0x94000001)}})[0],
                         {(0x200, "instruction"): 3})
        for word in (0x14000000, 0xd61f0000, 0xd503201f):
            costs, missing = J.attribute(item, {0x1000: {(0x200, "instruction", word)}})
            self.assertFalse(costs)
            self.assertEqual(sum(missing.values()), 3)

    def test_group_not_arbitrarily_split(self):
        item = [("clock", 5, (0x1004, 0x2004), True)]
        mapped = {0x1000: {(0x300, "instruction", 0xd63f0200)},
                  0x2000: {(0x300, "instruction", 0xd63f0200)}}
        self.assertEqual(J.attribute(item, mapped)[0], {(0x300, "instruction"): 5})
        mapped[0x2000] = {(0x400, "instruction", 0xd63f0200)}
        costs, missing = J.attribute(item, mapped)
        self.assertFalse(costs)
        self.assertEqual(missing["groupe de PC distincts"], 5)
        del mapped[0x2000]
        self.assertEqual(sum(J.attribute(item, mapped)[1].values()), 5)

    def test_reused_address_stays_ambiguous(self):
        log = """OUT: [size=4]
  -- guest addr 0x300
0x1000: d63f0200 blr x16
OUT: [size=4]
  -- guest addr 0x400
0x1000: d63f0200 blr x16
"""
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "asm"; p.write_text(log)
            mapped = J.mapping(p, {0x1000})
        costs, missing = J.attribute([("clock", 7, (0x1004,), True)], mapped)
        self.assertFalse(costs)
        self.assertEqual(missing["adresse réutilisée"], 7)

    def test_raw_and_native_maps_with_data_boundary(self):
        log = """OUT: [size=16]
  -- guest addr 0x200 + tb prologue
0x1000:
OBJD-H: 00003fd61f2003d5
  -- guest addr 0x204
0x1008: d63f0200 blr x16
  data: [size=4]
0x100c:
OBJD-H: 00003fd6
"""
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "asm"; p.write_text(log)
            mapped = J.mapping(p, {0x1000, 0x1004, 0x1008, 0x100c, 0x1010})
        self.assertEqual(mapped[0x1000], {(0x200, "première+prologue", 0xd63f0000)})
        self.assertEqual(mapped[0x1008], {(0x204, "instruction", 0xd63f0200)})
        self.assertNotIn(0x1010, mapped)
        self.assertFalse(J.attribute([("clock", 2, (0x1010,), True)], mapped)[0])

    def test_two_adjacent_calls_not_chosen_arbitrarily(self):
        mapped = {0x1000: {(0x200, "instruction", 0xd63f0200)},
                  0x1004: {(0x204, "instruction", 0xd63f0200)}}
        costs, missing = J.attribute([("clock", 4, (0x1004,), True)], mapped)
        self.assertFalse(costs)
        self.assertEqual(missing["appel ambigu"], 4)


if __name__ == "__main__":
    unittest.main()
