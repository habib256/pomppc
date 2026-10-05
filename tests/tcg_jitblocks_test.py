#!/usr/bin/env python3
"""Check translation boundaries, ARM mnemonics, and repeated-PC accounting."""
import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("jitblocks", ROOT / "tools/tcg/jitblocks.py")
jitblocks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(jitblocks)


class JitBlocksTest(unittest.TestCase):
    def test_raw_bytes_from_qemu_without_disassembler(self):
        try:
            import capstone
        except ImportError:
            self.skipTest("raw log decoding requires capstone")
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "raw"
            log.write_text("OUT: [size=16]\n  -- guest addr 0x1000 + tb prologue\n"
                           "0x100000000:  \nOBJD-H: 600240f9000400910028201e00023fd6\n")
            block = jitblocks.read_code(log)[0x1000][0]
            self.assertEqual((block["arm"], block["env"], block["calls"]), (4, 1, 1))

    def test_actual_dump_shape_and_retranslation(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / "asm").write_text("""----------------
OUT: [size=16]
  -- guest addr 0x0000000000001000 + tb prologue
0x100000000: f9400260  ldr x0, [x19]
0x100000004: 91000400  add x0, x0, #1
  -- guest addr 0x0000000000001004
0x100000008: 1e202800  fadd s0, s0, s0
0x10000000c: d63f0200  blr x16
----------------
IN:
0x2000: f9400260  ldr x0, [x19]
----------------
OUT: [size=8]
  -- guest addr 0x0000000000001000 + tb prologue
0x100000010: f9000260  str x0, [x19, #8]
0x100000014: d65f03c0  ret
""")
            (p / "counts").write_text("# skipped_translations=0\npc,guest_insns,executions\n0x1000,2,10\n0x1000,1,20\n0x2000,1,5\n")
            code = jitblocks.read_code(p / "asm")
            self.assertEqual([(b["arm"], b["env"], b["calls"]) for b in code[0x1000]],
                             [(4, 1, 1), (2, 1, 0)])
            self.assertNotIn(0x1004, code)
            self.assertNotIn(0x2000, code)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                jitblocks.report(p / "counts", p / "asm", 10)
            self.assertIn("0x00001000 30 2..4 1 0..1 2", output.getvalue())
            self.assertIn("executions without emitted code: 5", output.getvalue())


if __name__ == "__main__":
    unittest.main()
