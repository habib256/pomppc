#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Assemble t2proof.S : GNU cross-tools, sinon assembleur natif de Tiger.

Le banc ne contient que des branches relatives : après liaison, sa section
texte peut être placée telle quelle à la base de la ROM mac99.
"""
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile


def darwin_source(source):
    source = source.replace("%r", "r").replace("%f", "f")
    # L'ancien as de Tiger ne connaît pas .rept. Les répétitions du banc
    # sont constantes, sans imbrication ; conserver leurs .set i,i+1.
    source = re.sub(r"(?m)^\s*\.rept\s+(\d+)\n(.*?)^\s*\.endr",
                    lambda m: m[2] * int(m[1]), source, flags=re.S)
    if re.search(r"\.(?:rept|endr)\b", source):
        raise ValueError("répétition non prise en charge par l'assembleur Tiger")
    return source.encode()


def macho_text(blob):
    magic, cpu, _, kind, ncmds, _, _ = struct.unpack_from(">7I", blob)
    if (magic, cpu, kind) != (0xFEEDFACE, 18, 2):
        raise ValueError("exécutable Mach-O PowerPC 32 bits attendu")
    offset = 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from(">II", blob, offset)
        if size < 8 or offset + size > len(blob):
            raise ValueError("commande Mach-O tronquée")
        if cmd == 1:  # LC_SEGMENT (32 bits)
            nsects = struct.unpack_from(">I", blob, offset + 48)[0]
            for i in range(nsects):
                section = struct.unpack_from(">16s16s9I", blob, offset + 56 + i * 68)
                if section[0].rstrip(b"\0") != b"__text":
                    continue
                _, length, start, _, _, nreloc, *_ = section[2:]
                if nreloc or start + length > len(blob):
                    raise ValueError("texte tronqué ou relocations non résolues")
                return blob[start:start + length]
        offset += size
    raise ValueError("section __text absente")


def main():
    here = pathlib.Path(__file__).resolve().parent
    output = pathlib.Path(sys.argv[1])
    if shutil.which("powerpc-linux-gnu-as") and shutil.which("powerpc-linux-gnu-objcopy"):
        with tempfile.TemporaryDirectory() as d:
            obj = str(pathlib.Path(d) / "proof.o")
            subprocess.run(["powerpc-linux-gnu-as", "-mregnames", "-many", "-o", obj,
                            str(here / "t2proof.S")], check=True)
            subprocess.run(["powerpc-linux-gnu-objcopy", "-O", "binary", obj,
                            str(output)], check=True)
    elif sys.platform == "darwin":
        ssh = here.parent / "guest" / "tssh.sh"
        command = ('d=$(mktemp -d /tmp/t2proof.XXXXXX) || exit 1; '
                   'trap \'rm -rf "$d"\' EXIT; cat > "$d/proof.s"; '
                   'as -arch ppc -o "$d/proof.o" "$d/proof.s" && '
                   'ld -arch ppc -static -e _start -o "$d/proof" "$d/proof.o" && '
                   'cat "$d/proof"')
        blob = subprocess.run([str(ssh), command], check=True, stdout=subprocess.PIPE,
                              input=darwin_source((here / "t2proof.S").read_text())).stdout
        output.write_bytes(macho_text(blob))
    else:
        sys.exit("Installer powerpc-linux-gnu-as et powerpc-linux-gnu-objcopy")


if __name__ == "__main__":
    main()
