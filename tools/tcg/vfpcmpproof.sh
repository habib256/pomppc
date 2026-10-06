#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vfpcmpproof.sh [arbre-QEMU] [vecteurs] [graine] — preuve hôte du modèle de
# patches/tcg/0034-ppc-vfp-native-cmp.patch (x-vfp-native-cmp, docs/tcg-g4.md §34).
# Extrait de target/ppc/int_helper.c de l'arbre (défaut ~/src/qemu-fp), TELS
# QUELS, les helpers d'origine des comparaisons et conversions AltiVec et le
# modèle de l'émetteur x86_64 (bloc « vfp-native-cmp »), compile
# tools/tcg/vfpcmpproof.c avec les -I/-D de int_helper.c et le lie au VRAI objet
# softfloat de l'arbre construit. MUTATE='expression sed' : mute le modèle
# extrait (contre-épreuve). L'émetteur réel : vfpcmp-user.sh, vfpcmp-mut.sh.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$HOME/src/qemu-fp}"
N="${2:-20000}"
SEED="${3:-0x5eed}"
OUT="${TMPDIR:-/tmp}/vfpcmpproof.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
python3 - "$SRC/target/ppc/int_helper.c" "$OUT" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
def between(a, b, incl_b=True):
    i = src.index(a)
    j = src.index(b, i)
    return src[i:j + (len(b) if incl_b else 0)]
ref = [
    between('#define SATCVT(from, to,', '#undef SATCVTU'),
    between('static inline void set_vscr_sat(', '\n}\n'),
    between('#define VCF(suffix, cvt, element)', '#undef VCF\n'),
    between('#define VCMPFP_DO(suffix,', '#define VCT(suffix', incl_b=False),
    between('#define VCT(suffix, satcvt, element)', '#undef VCT\n'),
]
open(sys.argv[2] + '/vfpcmpproof-ref.h', 'w').write('\n'.join(ref))
model = [
    between('static inline bool vfp_can_use_fpu(', '\n}\n'),
    between('/* vfp-native-cmp: début */', '/* vfp-native-cmp: fin */'),
]
open(sys.argv[2] + '/vfpcmpproof-model.h', 'w').write('\n'.join(model))
PY
if [ -n "${MUTATE:-}" ]; then
  cp "$OUT/vfpcmpproof-model.h" "$OUT/orig.h"
  # sed -i.bak : -i seul est GNU (sous macOS, « -i -e » prend -e pour suffixe)
  sed -i.bak -e "$MUTATE" "$OUT/vfpcmpproof-model.h"
  cmp -s "$OUT/orig.h" "$OUT/vfpcmpproof-model.h" && { echo "mutation sans effet : $MUTATE" >&2; exit 2; }
fi
B="${BUILD:-$SRC/build}"
SF="$B/libcommon.a.p/fpu_softfloat.c.o"
FLAGS="$(python3 - "$B/compile_commands.json" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['output'].endswith('libqemu-ppc64-softmmu.a.p/target_ppc_int_helper.c.o'):
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
( cd "$B" && eval cc -O2 -Wall -Wno-unused-function -fno-strict-aliasing \
    "$FLAGS" -I"$SRC/target/ppc" -I"$OUT" "$HERE/vfpcmpproof.c" "$SF" \
    -lm "$(pkg-config --libs glib-2.0)" -lpthread -o "$OUT/vfpcmpproof" )
time "$OUT/vfpcmpproof" "$N" "$SEED"
