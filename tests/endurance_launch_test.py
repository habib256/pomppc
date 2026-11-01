#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Arrêt du banc pendant le lancement : aucun travailleur lancé ne fuit."""
import contextlib
import io
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch, call

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/endurance"))
import endurance


class LaunchTest(unittest.TestCase):
    def run_case(self, slots, interrupted=False, start_error=None):
        camp = SimpleNamespace(arret=False, dir="unused")
        threads = []

        class Worker:
            def __init__(self, **kwargs):
                self.ident = None
                self.alive = False
                self.joined = False
                self.index = len(threads)
                threads.append(self)

            def start(self):
                if start_error and self.index == 1:
                    raise RuntimeError("start failed")
                self.ident = self.index + 1
                self.alive = interrupted or bool(start_error)

            def is_alive(self):
                return self.alive

            def join(self):
                if self.ident is None:
                    raise AssertionError("join d'un travailleur jamais lancé")
                if self.alive and not camp.arret:
                    raise AssertionError("travailleur actif sans demande d'arrêt")
                self.joined = True
                self.alive = False

        sleep_error = KeyboardInterrupt if interrupted else None
        with patch.object(endurance.threading, "Thread", Worker), \
                patch.object(endurance.time, "sleep", side_effect=sleep_error) as sleep, \
                patch.object(endurance, "journal"), \
                patch.object(endurance, "rapport", return_value="rapport"), \
                contextlib.redirect_stdout(io.StringIO()):
            error = None
            try:
                endurance.lance_instances(camp, None, slots, 60)
            except RuntimeError as exc:
                error = exc
        return camp, threads, sleep, error

    def test_single_worker_has_no_initial_stagger_wait(self):
        camp, workers, sleep, error = self.run_case([0])
        sleep.assert_not_called()
        self.assertFalse(camp.arret)
        self.assertIsNone(error)
        self.assertTrue(workers[0].joined)

    def test_stagger_is_only_between_workers(self):
        _, workers, sleep, error = self.run_case([0, 1, 2])
        self.assertEqual(sleep.call_args_list, [call(60), call(60)])
        self.assertIsNone(error)
        self.assertTrue(all(w.joined for w in workers))

    def test_interrupt_during_stagger_joins_only_started_workers(self):
        camp, workers, _, error = self.run_case([0, 1], interrupted=True)
        self.assertTrue(camp.arret)
        self.assertTrue(workers[0].joined)
        self.assertIsNone(workers[1].ident)
        self.assertFalse(workers[1].joined)
        self.assertIsNone(error)

    def test_failed_start_also_closes_existing_workers(self):
        camp, workers, _, error = self.run_case([0, 1], start_error=True)
        self.assertTrue(camp.arret)
        self.assertTrue(workers[0].joined)
        self.assertFalse(workers[1].joined)
        self.assertIsInstance(error, RuntimeError)


if __name__ == "__main__":
    unittest.main()
