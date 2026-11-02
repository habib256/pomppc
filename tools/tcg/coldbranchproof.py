#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Preuve préparatoire de 0056 sur M4 : relocations réelles et sauts longs exécutés.

Ne porte pas la zone froide dans QEMU et ne mesure pas un gain en jeu.
Compile reloc_pc19/26 du backend construit, puis exécute des portes conditionnelles
vers une autre page MAP_JIT à ±48/96 Mio. Aucun QEMU supplémentaire lancé.
"""
import hashlib
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tempfile


def main():
    if sys.platform != "darwin" or platform.machine() != "arm64":
        raise SystemExit("preuve réservée au Mac arm64")
    root = Path(sys.argv[1] if len(sys.argv) > 1 else
                os.environ.get("QEMU_SRC", "~/src/qemu")).expanduser()
    path = root / "tcg/aarch64/tcg-target.c.inc"
    source = path.read_text()
    begin = source.index("static bool reloc_pc26(")
    end = source.index("static bool reloc_pc14(", begin)
    print("source : %s (sha256 %s)" %
          (path, hashlib.sha256(path.read_bytes()).hexdigest()), flush=True)
    proof = Path(__file__).with_suffix(".c")
    print("preuve C : sha256 " + hashlib.sha256(proof.read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="pomppc-coldbranch-") as tmp:
        directory = Path(tmp)
        (directory / "coldbranch-target.inc").write_text(source[begin:end])
        binary = directory / "proof"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                       ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", tmp,
                        str(proof), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
