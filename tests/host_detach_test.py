#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Le processus détaché survit à la fermeture du groupe de son lanceur."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DetachTest(unittest.TestCase):
    def run_case(self, fallback):
        with tempfile.TemporaryDirectory(prefix="détachement ") as d:
            root = Path(d); record = root / "état.json"
            helper = root / "enfant.py"
            helper.write_text('''import json, os, sys, time
from pathlib import Path
p = Path(sys.argv[1])
p.write_text(json.dumps(dict(pid=os.getpid(), pgid=os.getpgrp(), sid=os.getsid(0), args=sys.argv[2:])))
end = time.monotonic() + 10
while not Path(str(p)+".release").exists() and time.monotonic() < end:
    time.sleep(.01)
Path(str(p)+".fini").touch()
''')
            script = '''. "$1"; shift
if [ "$1" = fallback ]; then
  command() {
    if [ "$1" = -v ] && [ "${2:-}" = setsid ]; then return 1; fi
    builtin command "$@"
  }
fi
shift
host_detach "$@" </dev/null >"$DETACH_TEST_LOG" 2>&1 &
while :; do sleep 1; done
'''
            tokens = ["un argument avec espaces", "littéral $(rien); $HOME"]
            parent = subprocess.Popen(
                ["bash", "-c", script, "detach-test", str(ROOT / "scripts/hostcompat.sh"),
                 "fallback" if fallback else "normal", sys.executable, str(helper), str(record)] + tokens,
                start_new_session=True, env=dict(os.environ, DETACH_TEST_LOG=str(root / "log")))
            try:
                until = time.monotonic() + 5
                info = None
                while time.monotonic() < until:
                    try:
                        info = json.loads(record.read_text()); break
                    except (FileNotFoundError, json.JSONDecodeError):
                        time.sleep(.01)
                self.assertIsNotNone(info, (root / "log").read_text())
                self.assertEqual(info["args"], tokens)
                self.assertEqual(info["sid"], info["pid"])
                self.assertNotEqual(info["pgid"], parent.pid)
                os.killpg(parent.pid, signal.SIGTERM)
                parent.wait(timeout=3)
                time.sleep(.05)
                os.kill(info["pid"], 0)  # le groupe parent a disparu, l'enfant vit encore
                Path(str(record) + ".release").touch()
                until = time.monotonic() + 3
                while not Path(str(record) + ".fini").exists() and time.monotonic() < until:
                    time.sleep(.01)
                self.assertTrue(Path(str(record) + ".fini").exists())
            finally:
                Path(str(record) + ".release").touch()
                if parent.poll() is None:
                    os.killpg(parent.pid, signal.SIGTERM)
                parent.wait(timeout=3)

    def test_platform_path(self):
        self.run_case(False)

    def test_without_setsid_command(self):
        self.run_case(True)


if __name__ == "__main__":
    unittest.main()
