#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Routage du lanceur Tiger, sans démarrer QEMU ni ouvrir de fenêtre."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
OPTS = "FASTFP SRTLB LFSINLINE VFPFAST VPERMFAST FPINLINE RETINLINE JCIDX ICBISYNC MSRNOBQL JCBITS JITNEAR QGPU_GPU_COPY QGPU_GLSL".split()
# Défauts propres à l'hôte (docs/vitesse-doom3-x86.md §13, docs/jit-m4-2026-10-05.md).
HOSTOPTS = "VFPNATIVE TLBPRECISE LMWINLINE DCBZINLINE JITREL32 FPNATIVECMP VFPNATIVECMP LMWVEC JCWORD".split()
HOST = "%s:%s" % (os.uname().sysname, os.uname().machine)
HOSTON = {"Linux:x86_64": HOSTOPTS, "Darwin:arm64": ["VFPNATIVE", "LMWVEC", "JCWORD"]}.get(HOST, [])
with tempfile.TemporaryDirectory(prefix="tiger launcher ") as tmp:
    root = Path(tmp)
    for name in ("run_tiger.sh", "run_frontend.sh"):
        shutil.copy2(ROOT / name, root / name)
    # Un faux exécutable, mais le vrai wrapper run_frontend.sh.
    binary = root / "frontend/build/pomppc"
    binary.parent.mkdir(parents=True)
    binary.write_text('#!/bin/bash\nprintf "frontend:%s\\n" "$1"\n' +
                      "\n".join('printf "%s=%%s\\n" "${%s:-}"' % (k, k) for k in OPTS + HOSTOPTS) + "\n")
    binary.chmod(0o755)
    (root / "config.env").write_text('echo native-path\nexit 0\n')
    env = {k: v for k, v in os.environ.items() if k not in OPTS + HOSTOPTS +
           ["POMPPC_FRONTEND", "DBUS_DISPLAY", "HEADLESS", "POMPPC_DISPLAY"]}

    def run(**extra):
        return subprocess.run([str(root / "run_tiger.sh")], cwd="/", env=dict(env, **extra),
                              capture_output=True, text=True, timeout=10)

    p = run()
    assert p.returncode == 0 and "frontend:" + str(root / "run_tiger.sh") in p.stdout, p
    assert all(k + "=1\n" in p.stdout for k in OPTS if k != "JCBITS"), p.stdout
    assert "JCBITS=14\n" in p.stdout, p.stdout       # 2^14 entrées de cache de sauts (tcg/0011)
    assert all((k + "=1\n" if k in HOSTON else k + "=\n") in p.stdout for k in HOSTOPTS), (HOST, p.stdout)
    p = run(TLBPRECISE="0", JITREL32="0")
    assert p.returncode == 0 and "TLBPRECISE=0\n" in p.stdout and "JITREL32=0\n" in p.stdout, p
    p = run(JCBITS="12")
    assert p.returncode == 0 and "JCBITS=12\n" in p.stdout, p
    p = run(ICBISYNC="0", MSRNOBQL="0")
    assert p.returncode == 0 and "ICBISYNC=0\n" in p.stdout and "MSRNOBQL=0\n" in p.stdout, p
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

# Binaire rapide du PC (scripts/qemu_fast.sh, docs/binaire-rapide-x86.md) : le choix
# de build-fast/ selon présence, relevé, version, processeur, série et capacités,
# QEMU_FAST et QEMU_BIN — sans lancer QEMU (faux binaires, sondes de caps.sh simulées).
src = (ROOT / "run_tiger.sh").read_text()
assert src.index("pomppc_pick_qemu") < src.index('QEMU_BIN64="${QEMU_BIN}64"'), \
    "run_tiger.sh doit choisir le binaire avant d'en déduire le ppc64"
assert "${QEMU_BIN_LABEL" in src.split("▶ Tiger (QEMU", 1)[1].split("\n", 1)[0]
matab = (ROOT / "tools/tcg/matab.sh").read_text()
assert "set -- QEMU_FAST=0 ${QB:+QEMU_BIN=$QB} $vars" in matab, "les bras de matab partent de la référence"
FASTSH = str(ROOT / "scripts/qemu_fast.sh")
cpu = subprocess.run(["bash", "-c", 'source "$1"; pomppc_fast_cpu', "_", FASTSH],
                     capture_output=True, text=True).stdout.strip()
with tempfile.TemporaryDirectory(prefix="qemu fast ") as tmp:
    t = Path(tmp)
    (t / "repo/patches/essais").mkdir(parents=True)
    (t / "repo/patches/0001.patch").write_text("série A\n")
    hash_cmd = ["bash", "-c", 'source "$1"; pomppc_serie_hash "$2"', "_", FASTSH, str(t / "repo/patches")]
    serie = subprocess.run(hash_cmd, capture_output=True, text=True).stdout.strip()
    assert len(serie) == 16, serie
    (t / "repo/patches/essais/x.patch").write_text("essai\n")      # hors série
    assert subprocess.run(hash_cmd, capture_output=True, text=True).stdout.strip() == serie

    def fake(path, version="11.1.2"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#!/bin/sh\necho "QEMU emulator version %s (pomppc)"\n' % version)
        path.chmod(0o755)

    ref = t / "qemu/build/qemu-system-ppc"
    fast = t / "qemu/build-fast/qemu-system-ppc"
    fast64 = Path(str(fast) + "64")
    stamp = fast.parent / "pomppc-build.txt"
    fake(ref)
    fake(Path(str(ref) + "64"))

    def setup(present=True, ppc64=True, version="11.1.2", **st):
        for p in (fast, fast64, stamp):
            if p.exists():
                p.unlink()
        if not present:
            return
        fake(fast, version)
        if ppc64:
            fake(fast64, version)
        rel = dict(variante="native,nohard,lto,pgo", cpu=cpu, serie=serie, complet="oui")
        rel.update(st)
        stamp.write_text("".join("%s=%s\n" % kv for kv in rel.items()))

    pick = r'''
ROOT="$1"; QEMU_SRC_BIN="$2"; QEMU_BIN="$2"; QEMU_FAST_BIN="$3"
POMPPC_QEMU_VERSION=11.1.2; MACHINE="mac99,via=pmu"
qemu_machine_has()    { return "${STUB_SCREAMER:-0}"; }
qemu_has_device()     { return "${STUB_QGPU:-0}"; }
qemu_machine_smp_ok() { return "${STUB_SMP:-0}"; }
source "$4"
rc=0; pomppc_pick_qemu "${SMP_N:-2}" || rc=$?
printf 'RC=%s\nBIN=%s\nLABEL=%s\n' "$rc" "$QEMU_BIN" "$QEMU_BIN_LABEL"
'''
    base_env = {k: v for k, v in os.environ.items()
                if k not in ("QEMU_FAST", "USER_QEMU_BIN", "QEMU_BIN", "POMPPC_FAST_HOST")}
    base_env["POMPPC_FAST_HOST"] = "Linux:x86_64"

    def choose(**extra):
        p = subprocess.run(["bash", "-c", pick, "_", str(t / "repo"), str(ref), str(fast), FASTSH],
                           env=dict(base_env, **extra), capture_output=True, text=True, timeout=20)
        out = dict(line.split("=", 1) for line in p.stdout.splitlines() if "=" in line)
        return int(out.get("RC", "99")), out.get("BIN"), out.get("LABEL", ""), p.stderr

    # absent : référence, en silence ; absent mais exigé (QEMU_FAST=1) : refus
    setup(present=False)
    assert choose() == (0, str(ref), "binaire de référence", ""), choose()
    assert choose(QEMU_FAST="1")[0] == 1
    # présent, complet, à jour : pris, avec l'étiquette PGO
    setup()
    rc, b, lab, err = choose()
    assert (rc, b, lab) == (0, str(fast), "binaire rapide (PGO, -O3, -march=native)"), (rc, b, lab, err)
    assert choose(QEMU_FAST="1")[1] == str(fast)
    # QEMU_FAST=0 : référence ; QEMU_BIN explicite : imposé ; autre hôte : rien ne change
    assert choose(QEMU_FAST="0")[1:3] == (str(ref), "binaire de référence")
    assert choose(USER_QEMU_BIN="/ailleurs/qemu")[1:3] == (str(ref), "binaire imposé (QEMU_BIN)")
    assert choose(POMPPC_FAST_HOST="Darwin:arm64")[1] == str(ref)
    assert choose(QEMU_FAST="oui")[0] == 1
    # sans PGO : pris, étiquette honnête
    setup(variante="native,nohard,lto")
    assert choose()[2] == "binaire rapide (-O3, -march=native, LTO, sans PGO)"
    # SMP 1 : le ppc64 n'est pas exigé ; SMP 2 sans ppc64 : écarté
    setup(ppc64=False)
    assert choose(SMP_N="1")[1] == str(fast)
    rc, b, _, err = choose()
    assert b == str(ref) and "absent" in err, (b, err)
    # chaque raison d'écarter : avertissement + référence ; avec QEMU_FAST=1 : refus
    for st, why, extra in (
            (dict(variante="native,nohard,lto,pgo-gen"), "instrumenté", {}),
            (dict(cpu="AMD Ryzen 5 1600"), "construit pour", {}),
            (dict(serie="0123456789abcdef"), "autre série", {}),
            (dict(complet="non"), "incomplète", {}),
            (dict(version="9.2.0"), "QEMU 9.2.0", {}),
            ({}, "Screamer", {"STUB_SCREAMER": "1"}),
            ({}, "qgpu-pci", {"STUB_QGPU": "1"}),
            ({}, "-smp 2", {"STUB_SMP": "1"})):
        setup(**st)
        rc, b, lab, err = choose(**extra)
        assert (rc, b, lab) == (0, str(ref), "binaire de référence") and why in err, (st, why, rc, b, err)
        assert choose(QEMU_FAST="1", **extra)[0] == 1, (st, why)
    # pas de relevé (construit à la main) : écarté
    setup()
    stamp.unlink()
    assert choose()[1] == str(ref)
    # paquet publié (bin/ à la racine) : son binaire générique, jamais build-fast
    setup()
    fake(t / "repo/bin/qemu-system-ppc")
    assert choose()[1] == str(ref)
print("binaire rapide : présence, relevé, version, processeur, série, caps, QEMU_FAST, QEMU_BIN OK")
