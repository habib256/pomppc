#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Exécuter les relais de 0066 et tester ses pools/retours d'erreur réels.

Extrait les fonctions de l'arbre QEMU, sans réimplanter l'algorithme testé.
Le harnais remplace seulement l'émission des callbacks mémoire/FP : ceux-ci
doivent aussi être exercés dans la VM et sous leurs vérificateurs.
"""
import argparse
import hashlib
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tempfile


def function(source, name):
    begin = source.rfind("\n", 0, source.index(name + "(")) + 1
    start = source.index("{", begin)
    # Les fonctions extraites n'ont pas d'accolades dans leurs chaînes/commentaires.
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end] + "\n"


def main():
    if sys.platform != "darwin" or platform.machine() != "arm64":
        raise SystemExit("preuve réservée au Mac arm64")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", nargs="?", default=os.environ.get("QEMU_SRC", "~/src/qemu"))
    parser.add_argument("--mutants", action="store_true")
    args = parser.parse_args()
    root = Path(args.qemu).expanduser()
    groups = {
        "reloc": ("tcg/aarch64/tcg-target.c.inc",
                  ["reloc_pc26", "reloc_pc19", "reloc_ldst", "tcg_out_cold_veneers"]),
        "finalize": ("tcg/tcg.c", ["tcg_out_pool_finalize", "tcg_out_cold_finalize"]),
        "regions": ("tcg/region.c", ["tcg_cold_region_count"]),
    }
    for proof_source in (Path(__file__), Path(__file__).with_suffix(".c")):
        print(proof_source.name, hashlib.sha256(proof_source.read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="pomppc-coldzone-") as tmp:
        directory = Path(tmp)
        extracted = {}
        for group, (filename, names) in groups.items():
            p = root / filename
            source = p.read_text()
            print(filename, hashlib.sha256(p.read_bytes()).hexdigest(), flush=True)
            extracted[group] = "\n".join(function(source, name) for name in names)
        binary = directory / "proof"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", tmp,
            str(Path(__file__).with_suffix(".c")), "-o", str(binary)]
        mutants = [
            ("reloc", "branch, B, 0", "branch, BL, 0", "BL détruit LR"),
            ("reloc", "return reloc_pc26(label, target);", "return reloc_pc19(label, target);", "portée courte"),
            ("finalize", "s->pool_labels = NULL;", "/* liste chaude conservée */", "pools mélangés"),
            ("finalize", "s->data_gen_ptr = hot_data;", "(void)hot_data;", "curseur chaud perdu"),
            ("regions", "64 * MiB", "128 * MiB", "région SMP=1 trop grande"),
        ] if args.mutants else []
        for mutation in [None] + mutants:
            sources = dict(extracted)
            if mutation:
                group, before, after, label = mutation
                assert before in sources[group], label
                sources[group] = sources[group].replace(before, after)
            for group, source in sources.items():
                (directory / (group + ".inc")).write_text(source)
            subprocess.run(command, check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            if mutation:
                if result.returncode == 0:
                    raise SystemExit("mutant non détecté : " + label)
                print("mutant détecté :", label, "code", result.returncode, flush=True)
            else:
                print(result.stdout, end="", flush=True)
                if result.returncode:
                    raise SystemExit(result.stderr or result.returncode)


if __name__ == "__main__":
    main()
