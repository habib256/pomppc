#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Comptage des coûts propres, attentes et blocs d'un même fil dans `sample`."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/tcg"))
import threadbusy


class ThreadBusyTest(unittest.TestCase):
    def parse(self, body):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "sample.txt"
            p.write_text("Call graph:\n" + body)
            return threadbusy.merged_threads(threadbusy.parse(p))

    def test_nested_wait_and_self_costs_without_double_counting(self):
        threads = self.parse("""    100 Thread_12: qgpu-render
    + 100 thread_start  (in libpthread) + 8 [0x1]
    +   70 render  (in qemu) + 4 [0x2] qgpu-pci.c:42
    +     50 nat_conv_attr  (in qemu) + 8 [0x3]
    +   30 qemu_cond_wait_impl  (in qemu) + 4 [0x4]
    +     30 __psynch_cvwait  (in libsystem_kernel) + 8 [0x5]
Total number in stack (recursive counted multiple, when >=5):
""")
        t = threads["12"]
        self.assertEqual(t["wait"], 30)
        self.assertEqual(dict(t["self"]), {"nat_conv_attr": 50, "render": 20,
                                          "__psynch_cvwait": 30})
        self.assertEqual(sum(t["self"].values()), t["total"])

    def test_eof_flushes_last_stack(self):
        t = self.parse("""    100 Thread_2: CPU 0/TCG
    + 100 qemu_thread_start  (in qemu) + 8 [0x1]
    +   25 ???  (in <unknown binary>) [0x2]
    +   75 __psynch_mutexwait  (in libsystem_kernel) + 8 [0x3]
""")["2"]
        self.assertEqual(t["wait"], 75)
        self.assertEqual(t["self"]["???"], 25)

    def test_repeated_thread_blocks_share_one_denominator(self):
        body = """    40 Thread_3   DispatchQueue_1: first
    + 40 work  (in qemu) + 4 [0x1]
    60 Thread_3: qgpu-render
    + 60 work  (in qemu) + 8 [0x2]
Sort by top of stack, same collapsed:
"""
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "sample.txt"
            p.write_text("Call graph:\n" + body)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                threadbusy.report(p, 3)
        self.assertIn("(100 échantillons par fil)", output.getvalue())
        self.assertIn("somme : 1.00 cœur hôte", output.getvalue())
        self.assertIn("100.0 %  work", output.getvalue())

    def test_objective_c_names_and_empty_report(self):
        t = self.parse("""    10 Thread_4: main
    + 10 -[NSApplication run]  (in AppKit) + 396 [0x1]
""")["4"]
        self.assertEqual(t["self"]["-[NSApplication run]"], 10)
        self.assertEqual(self.parse(""), {})


if __name__ == "__main__":
    unittest.main()
