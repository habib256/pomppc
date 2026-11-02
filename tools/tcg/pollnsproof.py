#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""pollnsproof.py [QEMU_SRC] — contrat macOS de tcg/0058 contre g_poll.

Compile les fonctions réelles de util/qemu-timer.c avec glib-2.0 de cet hôte,
puis compare les retours, événements et erreurs de descripteurs réels. Compte
les appels à select pour vérifier les routes sans chronométrage fragile.
Aucun QEMU lancé. Défaut : $QEMU_SRC ou ~/src/qemu. Ne mesure pas la vitesse.
"""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    if sys.platform != "darwin":
        raise SystemExit("preuve réservée au chemin macOS de tcg/0058")
    root = Path(sys.argv[1] if len(sys.argv) > 1 else
                os.environ.get("QEMU_SRC", "~/src/qemu")).expanduser()
    source = root / "util/qemu-timer.c"
    text = source.read_text()
    start = text.index("int qemu_timeout_ns_to_ms(")
    end = text.index("void timer_init_full(", start)
    target = text[start:end]
    if "qemu_poll_ns_select" not in target:
        raise SystemExit("tcg/0058 absent de cet arbre")
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    version = subprocess.check_output(
        ["pkg-config", "--modversion", "glib-2.0"], text=True).strip()
    print("source : %s (sha256 %s)" % (source, hashlib.sha256(source.read_bytes()).hexdigest()),
          flush=True)
    print("glib-2.0 : " + version, flush=True)
    with tempfile.TemporaryDirectory(prefix="pomppc-pollns-") as tmp:
        directory = Path(tmp)
        (directory / "pollns-target.inc").write_text(target)
        binary = directory / "proof"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                       ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", tmp,
                        *(["-DHAVE_POLL_IDLE"] if "void qemu_poll_ns_set_cpus_idle(" in target else []),
                        str(Path(__file__).with_suffix(".c")), "-o", str(binary)] + flags,
                       check=True)
        modes = [(None, None), ("0", None), ("1", None), ("2", None)]
        if "void qemu_poll_ns_set_cpus_idle(" in target:
            modes += [("3", "0"), ("3", "1")]
        for mode, idle in modes:
            env = dict(os.environ)
            env.pop("POMPPC_POLL_NS", None)
            if mode is not None:
                env["POMPPC_POLL_NS"] = mode
            env.pop("POMPPC_PROOF_IDLE", None)
            if idle is not None:
                env["POMPPC_PROOF_IDLE"] = idle
            result = subprocess.run([str(binary)], env=env, timeout=15)
            if result.returncode:
                raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
