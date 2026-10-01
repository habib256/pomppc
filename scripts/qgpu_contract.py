#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Référence des constantes qgpu : stdout, ou --check contre docs/protocole.md."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MARKER = "<!-- qgpu-constants -->"


def reference():
    lines = [MARKER, "", "## Référence exhaustive des constantes", "",
             "Générée par `python3 scripts/qgpu_contract.py` depuis les deux en-têtes.",
             "Les expressions C sont conservées ; les longueurs `QGPU_LEN_*` incluent",
             "l'en-tête. Les commentaires et contrats détaillés restent dans les sources.", ""]
    for name in ("qgpu_abi.h", "qgpu_proto.h"):
        src = (ROOT / "patches/qgpu" / name).read_text()
        src = re.sub(r"/\*.*?\*/", "", src, flags=re.S).replace("\\\n", " ")
        lines += ["### " + name, "", "| Symbole | Valeur / expression |", "|---|---|"]
        for match in re.finditer(r"^#define[ \t]+(QGPU_\w+(?:\([^\n]*?\))?)[ \t]+([^\n]+)", src, re.M):
            symbol, value = match.groups()
            value = " ".join(value.split()).replace("|", "\\|")
            lines.append("| `%s` | `%s` |" % (symbol, value))
        lines.append("")
    return "\n".join(lines)


if __name__ == "__main__":
    expected = reference()
    if sys.argv[1:] == ["--check"]:
        doc = (ROOT / "docs/protocole.md").read_text()
        actual = doc[doc.find(MARKER):] if MARKER in doc else ""
        if actual != expected:
            sys.exit("protocole.md : constantes périmées ; régénérer avec scripts/qgpu_contract.py")
        print("contrat qgpu : constantes alignées sur les en-têtes")
    elif sys.argv[1:]:
        sys.exit("usage: qgpu_contract.py [--check]")
    else:
        print(expected, end="")
