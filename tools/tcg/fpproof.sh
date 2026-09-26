#!/usr/bin/env bash
# fpproof.sh [arbre-QEMU] [vecteurs] [graine] — preuve hôte de
# patches/tcg/0007-ppc-fp-inline.patch (x-fp-inline, docs/tcg-g4.md §15).
# Extrait le bloc « fp-inline » TEL QUEL de target/ppc/fpu_helper.c de l'arbre
# (défaut ~/src/qemu-fp), compile tools/tcg/fpproof.c avec les -I/-D de
# fpu_helper.c (cible ppc64, comme le binaire des jeux) et le lie aux VRAIS
# objets de l'arbre construit : les helpers d'origine et helper_fp32_fast
# (target_ppc_fpu_helper.c.o), ppc_store_fpscr (target_ppc_cpu.c.o) et
# softfloat (fpu_softfloat.c.o).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$HOME/src/qemu-fp}"
N="${2:-2000000}"
SEED="${3:-0x5eed}"
OUT="${TMPDIR:-/tmp}/fpproof.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
sed -n '/fp-inline: début/,/fp-inline: fin/p' "$SRC/target/ppc/fpu_helper.c" > "$OUT/fpproof-fast.h"
grep -q "helper_fp32_fast" "$OUT/fpproof-fast.h" || { echo "arbre sans le patch 0007 ($SRC)" >&2; exit 2; }
# Contre-épreuve : MUTATE='expression sed' mute le bloc extrait, et le banc
# prend alors le résultat du modèle (muté) au lieu de celui de l'objet.
if [ -n "${MUTATE:-}" ]; then
  sed -i.orig -e "$MUTATE" "$OUT/fpproof-fast.h"
  if cmp -s "$OUT/fpproof-fast.h" "$OUT/fpproof-fast.h.orig"; then
    echo "mutation sans effet : $MUTATE" >&2; exit 2
  fi
  export FPPROOF_MODEL=1
fi
P="$SRC/build/libqemu-ppc64-softmmu.a.p"
FLAGS="$(python3 - "$SRC/build/compile_commands.json" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['output'].endswith('libqemu-ppc64-softmmu.a.p/target_ppc_fpu_helper.c.o'):
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
( cd "$SRC/build" && eval cc -O2 -Wall -Wno-shift-negative-value -Wno-unused-function -fno-strict-aliasing \
    "$FLAGS" -I"$SRC/target/ppc" -I"$OUT" "$HERE/fpproof.c" \
    "$P/target_ppc_fpu_helper.c.o" "$P/target_ppc_cpu.c.o" "$P/fpu_softfloat.c.o" \
    -lm "$(pkg-config --libs glib-2.0)" -lpthread -o "$OUT/fpproof" )
time "$OUT/fpproof" "$N" "$SEED"

