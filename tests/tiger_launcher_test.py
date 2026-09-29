#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""Routage du lanceur Tiger, sans démarrer QEMU ni ouvrir de fenêtre."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
OPTS = "FASTFP SRTLB LFSINLINE VFPFAST VPERMFAST FPINLINE RETINLINE JCIDX ICBISYNC JITNEAR QGPU_GPU_COPY QGPU_GLSL".split()
with tempfile.TemporaryDirectory(prefix="tiger launcher ") as tmp:
    root = Path(tmp)
    for name in ("run_tiger.sh", "run_frontend.sh"):
        shutil.copy2(ROOT / name, root / name)
    # Un faux exécutable, mais le vrai wrapper run_frontend.sh.
    binary = root / "frontend/build/pomppc"
    binary.parent.mkdir(parents=True)
    binary.write_text('#!/bin/bash\nprintf "frontend:%s\\n" "$1"\n' +
                      "\n".join('printf "%s=%%s\\n" "$%s"' % (k, k) for k in OPTS) + "\n")
    binary.chmod(0o755)
    (root / "config.env").write_text('echo native-path\nexit 0\n')
    env = {k: v for k, v in os.environ.items() if k not in OPTS +
           ["POMPPC_FRONTEND", "DBUS_DISPLAY", "HEADLESS", "POMPPC_DISPLAY"]}

    def run(**extra):
        return subprocess.run([str(root / "run_tiger.sh")], cwd="/", env=dict(env, **extra),
                              capture_output=True, text=True, timeout=10)

    p = run()
    assert p.returncode == 0 and "frontend:" + str(root / "run_tiger.sh") in p.stdout, p
    assert all(k + "=1\n" in p.stdout for k in OPTS), p.stdout
    p = run(FASTFP="0", JITNEAR="0", QGPU_GLSL="0")
    assert p.returncode == 0 and all(k + "=0\n" in p.stdout for k in ("FASTFP", "JITNEAR", "QGPU_GLSL")), p
    for settings in ({"DBUS_DISPLAY": "1"}, {"HEADLESS": "1"},
                     {"POMPPC_FRONTEND": "native"}, {"POMPPC_DISPLAY": "cocoa"}):
        p = run(**settings)
        assert p.returncode == 0 and p.stdout.strip() == "native-path", (settings, p)
    assert run(POMPPC_FRONTEND="typo").returncode == 2
    binary.unlink()
    p = run()
    assert p.returncode != 0 and "Frontend non compilé" in p.stderr, p
print("lanceur Tiger : ImGuiDock, profil maximal, surcharges et gardes OK")

# Exercise the launcher's actual tablet-selection block without launching QEMU.
source = (ROOT / "run_tiger.sh").read_text()
block = source.split("TABLET_ARGS=()", 1)[1].split('exec "$BIN"', 1)[0]
script = 'TABLET_ARGS=()\n' + block + '\nprintf "DEVICE:%s\\n" "${TABLET_ARGS[*]}"\n'
base = {k: v for k, v in os.environ.items() if k not in ("TABLET", "DBUS_DISPLAY")}
for settings, tablet in (({}, False), ({"DBUS_DISPLAY": "1"}, True),
                         ({"DBUS_DISPLAY": "1", "TABLET": "0"}, False),
                         ({"TABLET": "1"}, True), ({"TABLET": "0"}, False)):
    p = subprocess.run(["bash", "-c", script], env=dict(base, **settings),
                       capture_output=True, text=True, timeout=5)
    assert p.returncode == 0 and ("usb-tablet,id=pointer0" in p.stdout) == tablet, (settings, p)
print("tablette Tiger : défaut ImGuiDock, mode natif et TABLET=0/1 OK")
