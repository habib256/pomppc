#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vfpproof.sh [arbre-QEMU] [vecteurs] — preuve hôte de patches/tcg/0003-ppc-vfp-fast.patch
# (et, avec VFPPROOF_NATIVE=1, du modèle de x-vfp-native, tcg/0022).
# Extrait les fonctions « vfp-fast » TELLES QUELLES de target/ppc/int_helper.c de
# l'arbre (défaut ~/src/qemu-tcg19), compile tools/tcg/vfpproof.c contre elles et
# contre le VRAI objet softfloat de l'arbre construit (fpu_softfloat.c.o, avec les
# -I/-D de sa compilation, comme tests/run-all.sh pour fastfp-diff), et le lance.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$HOME/src/qemu-tcg19}"
N="${2:-2000000}"
OUT="${TMPDIR:-/tmp}/vfpproof.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
sed -n '/vfp-fast: début/,/vfp-fast: fin/p' "$SRC/target/ppc/int_helper.c" > "$OUT/vfpproof-fast.h"
grep -q "vfp_fma4" "$OUT/vfpproof-fast.h" || { echo "arbre sans le patch 0003 ($SRC)" >&2; exit 2; }
# VFPPROOF_NATIVE=1 : le modèle de l'émetteur x86 de x-vfp-native (tcg/0022), sa
# porte extraite de cpu_init.c ; VFPPROOF_MUT=k (dans l'environnement) : mutant k
NAT=
if [ -n "${VFPPROOF_NATIVE:-}" ]; then
  sed -n '/vfp-native-gate: début/,/vfp-native-gate: fin/p' "$SRC/target/ppc/cpu_init.c" \
    > "$OUT/vfpproof-gate.h"
  grep -q "ppc_vfn_gate_init" "$OUT/vfpproof-gate.h" || { echo "arbre sans le patch 0022 ($SRC)" >&2; exit 2; }
  NAT=-DVFPPROOF_NATIVE
fi
# QEMU 9.2 : softfloat est compilé par cible ; 10 et plus : une fois, dans libcommon
# VFPPROOF_BUILT=<arbre construit> : softfloat et options de compilation d'un autre
# arbre de même version (quand l'arbre prouvé n'est pas construit, docs/tcg-g4.md §28)
BUILT="${VFPPROOF_BUILT:-$SRC}"
SFO=libqemu-ppc-softmmu.a.p/fpu_softfloat.c.o
V10=
[ -f "$BUILT/build/$SFO" ] || { SFO=libcommon.a.p/fpu_softfloat.c.o; V10=-DVFPPROOF_QEMU10; }
OBJ="$BUILT/build/$SFO"
FLAGS="$(python3 - "$BUILT/build/compile_commands.json" "$SFO" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['output'] == sys.argv[2]:
        t = shlex.split(e['command']); out = []; i = 0
        while i < len(t):
            if t[i].startswith(('-I', '-iquote', '-isystem', '-D', '-include')):
                out.append(t[i])
                if t[i] in ('-iquote', '-isystem', '-include', '-I', '-D'):
                    i += 1; out.append(t[i])
            i += 1
        print(' '.join(shlex.quote(x) for x in out)); break
PY
)"
( cd "$BUILT/build" && eval cc -O2 -Wall -fno-strict-aliasing $V10 $NAT "$FLAGS" -I"$OUT" \
    "$HERE/vfpproof.c" "$OBJ" -lm "$(pkg-config --libs glib-2.0)" -lpthread -o "$OUT/vfpproof" )
time "$OUT/vfpproof" "$N"
