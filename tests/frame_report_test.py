#!/usr/bin/env python3
"""Regression checks for frame-time reporting and measurement windows."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "frame_report", Path(__file__).resolve().parents[1] / "tools/guest/frame_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class FrameReportTest(unittest.TestCase):
    def setUp(self):
        self.rows = [{"context": "a", "elapsed_ms": str(t)}
                     for t in (0, 20, 40, 60, 260)]

    def test_pause_is_included_in_throughput_and_tail(self):
        result = report.summarize(self.rows)
        self.assertAlmostEqual(result["fps"], 4000 / 260)
        self.assertEqual(result["median_ms"], 20)
        self.assertEqual(result["p99_ms"], 200)
        self.assertEqual(result["over_100_ms"], 1)

    def test_window_does_not_include_crossing_intervals(self):
        result = report.summarize(self.rows, start=.02, duration=.04)
        self.assertEqual(result["intervals"], 2)
        self.assertEqual(result["fps"], 50)

    def test_contexts_are_not_mistaken_for_extra_frames(self):
        mixed = self.rows + [{"context": "b", "elapsed_ms": "80"}]
        with self.assertRaises(ValueError):
            report.summarize(mixed)
        self.assertEqual(report.summarize(mixed, context="a")["intervals"], 4)

    def test_invalid_trace_is_not_reported_as_success(self):
        for rows in ([], self.rows[:1], self.rows + [self.rows[0]]):
            with self.assertRaises(ValueError):
                report.summarize(rows)


if __name__ == "__main__":
    unittest.main()
