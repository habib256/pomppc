#!/usr/bin/env bash
# vfpproof.sh [arbre-QEMU] [vecteurs] — preuve hôte de patches/tcg/0003-ppc-vfp-fast.patch.
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
OBJ="$SRC/build/libqemu-ppc-softmmu.a.p/fpu_softfloat.c.o"
FLAGS="$(python3 - "$SRC/build/compile_commands.json" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['output'].endswith('libqemu-ppc-softmmu.a.p/fpu_softfloat.c.o'):
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
( cd "$SRC/build" && eval cc -O2 -Wall -fno-strict-aliasing "$FLAGS" -I"$OUT" \
    "$HERE/vfpproof.c" "$OBJ" -lm "$(pkg-config --libs glib-2.0)" -lpthread -o "$OUT/vfpproof" )
time "$OUT/vfpproof" "$N"
