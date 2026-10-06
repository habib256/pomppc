#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# fpproof.sh [arbre-QEMU] [vecteurs] [graine] — preuve hôte de
# patches/tcg/0007-ppc-fp-inline.patch (x-fp-inline, docs/tcg-g4.md §15).
# Extrait le bloc « fp-inline » TEL QUEL de target/ppc/fpu_helper.c de l'arbre
# (défaut ~/src/qemu-fp), compile tools/tcg/fpproof.c avec les -I/-D de
# fpu_helper.c (cible ppc64, comme le binaire des jeux) et le lie aux VRAIS
# objets de l'arbre construit : les helpers d'origine et helper_fp32_fast
# (target_ppc_fpu_helper.c.o), ppc_store_fpscr (target_ppc_cpu.c.o) et
# softfloat (fpu_softfloat.c.o).
# Quand l'arbre a x-fp-flat (patches/tcg/0013, §22), fpproof.c est compilé
# avec -DFPPROOF_FLAT : helper_fp32_flat/helper_fcmpu_flat de l'objet sont
# comparés à la séquence d'origine sur TOUS les vecteurs ; avec x-fp-native
# (patches/tcg/0014), ppc_fp32_native_slow (le talon hors ligne) aussi.
# Quand il a x-fp-native-cmp (patches/tcg/0033, §33), -DFPPROOF_NCMP : frsp
# fctiw fctiwz fcmpo fdivs fdiv contre leurs helpers, modèle et talon.
# BUILD=… : dossier de construction de l'arbre (défaut <arbre>/build).
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
B="${BUILD:-$SRC/build}"
P="$B/libqemu-ppc64-softmmu.a.p"
# softfloat : objet commun depuis QEMU 10 (libcommon), propre à la cible avant
SF="$B/libcommon.a.p/fpu_softfloat.c.o"
[ -f "$SF" ] || SF="$P/fpu_softfloat.c.o"
DFLAT=""
grep -q "helper_fp32_flat" "$SRC/target/ppc/fpu_helper.c" && DFLAT="-DFPPROOF_FLAT"
grep -q "ppc_fp32_native_slow" "$SRC/target/ppc/fpu_helper.c" && DFLAT="$DFLAT -DFPPROOF_NATIVE"
# x-fp-native-cmp (patches/tcg/0033, §33) : frsp fctiw fctiwz fcmpo fdivs fdiv
grep -q "fpi_short" "$OUT/fpproof-fast.h" && DFLAT="$DFLAT -DFPPROOF_NCMP"
FLAGS="$(python3 - "$B/compile_commands.json" <<'PY'
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
# Contre-épreuve de x-fp-flat : FLATMUT='expression sed' mute une COPIE de
# fpu_helper.c, recompilée avec la commande de l'arbre, à la place de l'objet.
FH="$P/target_ppc_fpu_helper.c.o"
if [ -n "${FLATMUT:-}" ]; then
  sed -e "$FLATMUT" "$SRC/target/ppc/fpu_helper.c" > "$OUT/fpu_helper_mut.c"
  if cmp -s "$OUT/fpu_helper_mut.c" "$SRC/target/ppc/fpu_helper.c"; then
    echo "mutation sans effet : $FLATMUT" >&2; exit 2
  fi
  CMD="$(python3 - "$B/compile_commands.json" "$OUT" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['output'].endswith('libqemu-ppc64-softmmu.a.p/target_ppc_fpu_helper.c.o'):
        t = shlex.split(e['command']); o = []; skip = False
        for i, x in enumerate(t):
            if skip:
                skip = False; continue
            if x in ('-o', '-MF', '-MQ'):
                skip = True; continue
            if x in ('-MD',) or x.endswith('fpu_helper.c'):
                continue
            o.append(x)
        o += ['-I' + e['directory'] + '/../target/ppc', '-c', sys.argv[2] + '/fpu_helper_mut.c',
              '-o', sys.argv[2] + '/fpu_helper_mut.o']
        print(' '.join(shlex.quote(x) for x in o)); break
PY
)"
  ( cd "$B" && eval "$CMD" ) || { echo "mutant non compilable" >&2; exit 2; }
  FH="$OUT/fpu_helper_mut.o"
fi
( cd "$B" && eval cc -O2 $DFLAT -Wall -Wno-shift-negative-value -Wno-unused-function -fno-strict-aliasing \
    "$FLAGS" -I"$SRC/target/ppc" -I"$OUT" "$HERE/fpproof.c" \
    "$FH" "$P/target_ppc_cpu.c.o" "$SF" \
    -lm "$(pkg-config --libs glib-2.0)" -lpthread -o "$OUT/fpproof" )
time "$OUT/fpproof" "$N" "$SEED"

