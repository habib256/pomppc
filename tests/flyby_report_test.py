#!/usr/bin/env python3
import csv
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/guest'))
from flyby_report import report

WIDTH, HEIGHT = 800, 600


def rendered_frame():
    """A capture that looks like a rendered frame: many colours, everywhere."""
    row = bytes(bytearray((x * 7 + 4) % 251 if c == 0 else
                          (x * 3 + 8) % 241 if c == 1 else (x + 12) % 233
                          for x in range(WIDTH) for c in range(3)))
    return b'P6\n%d %d\n255\n' % (WIDTH, HEIGHT) + row * HEIGHT


class FlybyReportTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        (self.path / 'complete').touch()
        (self.path / 'exit-status.txt').write_text('0\n')
        (self.path / 'frame-74.ppm').write_bytes(rendered_frame())
        (self.path / 'manifest.txt').write_text(
            'scene=AS-Convoy intro cinematic\nresolution=800x600\n'
            'first_frame=13\nlast_frame=73\nsimulation_step_seconds=0.2\n')
        self.frames = [dict(frame=i, context='0x1', elapsed_ms=i*2000,
                            raw_vertices=i*300, raw_draws=i*10,
                            fallbacks=i*2, readbacks=i*3) for i in range(1, 80)]
        self.clock = [dict(frame=i, tick=i+4, fixed=1, benchmark=0, step=0.2, delta=0.2)
                      for i in range(1, 80)]
        self.write()

    def write(self):
        for name, rows in [('frames', self.frames), ('clock', self.clock)]:
            with (self.path / (name + '.csv')).open('w', newline='') as f:
                writer = csv.DictWriter(f, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)

    def test_exact_window_excludes_loading(self):
        result = report(self.path)
        self.assertEqual(result['intervals'], 60)
        self.assertEqual(result['fps'], 0.5)
        self.assertEqual(result['simulated_seconds'], 12)
        self.assertEqual(result['fallbacks_per_frame'], 2)

    def test_rejects_variable_step(self):
        self.clock[20]['delta'] = 0.4
        self.write()
        with self.assertRaisesRegex(ValueError, 'fixed simulation'):
            report(self.path)

    def test_rejects_missing_frame(self):
        del self.frames[20]
        del self.clock[20]           # sinon c'est le compte global qui refuse
        self.write()
        with self.assertRaisesRegex(ValueError, 'missing'):
            report(self.path)

    def test_rejects_nonfinite_clock(self):
        self.clock[20]['delta'] = 'nan'
        self.write()
        with self.assertRaisesRegex(ValueError, 'fixed simulation'):
            report(self.path)

    def test_rejects_multiple_ticks_per_swap(self):
        self.clock[20]['tick'] += 1
        self.write()
        with self.assertRaisesRegex(ValueError, 'one simulation tick'):
            report(self.path)

    def test_rejects_incomplete(self):
        (self.path / 'complete').unlink()
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            report(self.path)

    def test_rejects_crash_during_cleanup(self):
        (self.path / 'exit-status.txt').write_text('139\n')
        with self.assertRaisesRegex(ValueError, 'crashed during cleanup'):
            report(self.path)

    def test_rejects_context_switch(self):
        self.frames[20]['context'] = '0x2'
        self.write()
        with self.assertRaisesRegex(ValueError, 'multiple contexts'):
            report(self.path)

    def test_rejects_black_capture(self):
        (self.path / 'frame-74.ppm').write_bytes(
            b'P6\n800 600\n255\n' + bytes(WIDTH*HEIGHT*3))
        with self.assertRaisesRegex(ValueError, 'rendered frame'):
            report(self.path)

    def test_rejects_flat_capture(self):
        # A solid clear colour is not a rendered frame, and it used to pass
        # the 'not entirely black' check (bug hunt T12).
        (self.path / 'frame-74.ppm').write_bytes(
            b'P6\n800 600\n255\n' + b'\x12\x34\x56' * (WIDTH*HEIGHT))
        with self.assertRaisesRegex(ValueError, 'rendered frame'):
            report(self.path)

    def test_rejects_mostly_black_capture(self):
        # One bright corner on a black frame: 'any(pixel)' was true.
        pixels = bytearray(WIDTH*HEIGHT*3)
        for i in range(0, 20000*3, 3):
            pixels[i:i+3] = bytes([i % 251 + 4, i % 241 + 8, i % 233 + 12])
        (self.path / 'frame-74.ppm').write_bytes(
            b'P6\n800 600\n255\n' + bytes(pixels))
        with self.assertRaisesRegex(ValueError, 'rendered frame'):
            report(self.path)

    def test_rejects_frame_count_mismatch(self):
        # A swap counted twice outside the window keeps the window coherent
        # and still doubles the measured rate (bug hunt T7).
        self.frames.append(dict(frame=80, context='0x1', elapsed_ms=160000,
                                raw_vertices=1, raw_draws=1,
                                fallbacks=1, readbacks=1))
        self.write()
        with self.assertRaisesRegex(ValueError, 'disagree on frame count'):
            report(self.path)


if __name__ == '__main__':
    unittest.main()
