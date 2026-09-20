#!/usr/bin/env python3
import csv
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/guest'))
from flyby_report import report


class FlybyReportTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        (self.path / 'complete').touch()
        (self.path / 'exit-status.txt').write_text('0\n')
        (self.path / 'frame-74.ppm').write_bytes(b'P6\n800 600\n255\n' + b'\x12\x34\x56' * 480000)
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
        (self.path / 'frame-74.ppm').write_bytes(b'P6\n800 600\n255\n' + bytes(480000*3))
        with self.assertRaisesRegex(ValueError, 'scanout capture'):
            report(self.path)


if __name__ == '__main__':
    unittest.main()
