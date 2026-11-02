#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Les nettoyages des sondes respectent les noms et gardent les vidages utiles."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CleanupTest(unittest.TestCase):
    def test_probe_cleanups(self):
        names = {"jetable.bin", "b in.bin", "-bin-etrange", "bin\nsuite",
                 "swap-dt3.bin", "swap-prm3.bin", "swap-dt3-niv0.bin",
                 "clear-glstate.bin", "clear-gctxlow.bin", "prm.bin",
                 "dt7.bin", "texte.txt"}
        cases = {
            "wrapprobe": {"swap-dt3.bin", "swap-prm3.bin", "swap-dt3-niv0.bin", "texte.txt"},
            "v14probe": {"swap-dt3.bin", "swap-prm3.bin", "swap-dt3-niv0.bin",
                         "clear-glstate.bin", "clear-gctxlow.bin", "texte.txt"},
            "cubeprobe": {"swap-prm3.bin", "swap-dt3.bin", "swap-dt3-niv0.bin",
                          "prm.bin", "dt7.bin", "texte.txt"},
        }
        for job, expected in cases.items():
            with self.subTest(job=job), tempfile.TemporaryDirectory(prefix="sondes espace ") as tmp:
                out = Path(tmp)
                target = out / ("trace" if job == "cubeprobe" else "tr-test")
                target.mkdir()
                for name in names:
                    (target / name).write_text("conserver ou retirer, jamais découper le nom")
                (target / "dossier.bin").mkdir()
                source = (ROOT / "tools/guest/jobs" / job / "job.sh").read_text()
                begin = source.index('( cd "$OUT/')
                end = source.index("\n  )", begin) + len("\n  )")
                subprocess.run(["sh", "-c", source[begin:end]],
                               env=dict(os.environ, OUT=tmp, m="test"), check=True)
                self.assertEqual({p.name for p in target.iterdir()}, expected | {"dossier.bin"})


if __name__ == "__main__":
    unittest.main()
