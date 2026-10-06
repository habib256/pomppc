#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# a64syntax.sh ARBRE-QEMU [fichier.c …] — sur le PC x86-64 : passe au
# compilateur aarch64 (clang du NDK Android, `-fsyntax-only`, puis -c
# pour la génération de code) les fichiers de l'arbre construit pour x86-64
# qui portent du code propre à aarch64 : tcg/tcg.c (inclut
# tcg/aarch64/tcg-target.c.inc, émetteurs de tcg/0014, 0016, 0022, 0037, 0038)
# et target/ppc/int_helper.c (modèles NEON de 0022/0038). Les commandes sont
# celles de compile_commands.json de <arbre>/build, chemins x86_64 remplacés
# par aarch64, config-host.h recopié avec HOST_AARCH64 au lieu de HOST_X86_64.
# Ce N'EST PAS une construction pour le M4 (en-têtes bionic, glib x86-64) :
# seulement « le compilateur aarch64 accepte ce code ». Avertissements de
# compilation imprimés tels quels ; code de sortie 1 si une erreur.
# docs/parite-arm64-0031-0035.md.
set -uo pipefail
SRC="${1:?arbre QEMU (construit pour x86-64) requis}"; shift || true
B="$SRC/build"
NDK="${NDK_BIN:-$(ls -d "$HOME"/android-sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin 2>/dev/null | sort | tail -1)}"
CC="${A64CC:-$NDK/clang}"
[ -x "$CC" ] || { echo "clang aarch64 introuvable (NDK_BIN= ou A64CC=)" >&2; exit 2; }
OUT="${TMPDIR:-/tmp}/a64syntax.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
sed -e 's/^#define HOST_X86_64 1/#define HOST_AARCH64 1/' \
    -e 's/^#define CONFIG_AVX2_OPT/#undef CONFIG_AVX2_OPT/' \
    -e 's/^#define CONFIG_AVX512BW_OPT/#undef CONFIG_AVX512BW_OPT/' \
    "$B/config-host.h" > "$OUT/config-host.h"
FILES=("$@")
[ ${#FILES[@]} -gt 0 ] || FILES=(tcg/tcg.c target/ppc/int_helper.c)
rc=0
for f in "${FILES[@]}"; do
  CMD="$(python3 - "$B/compile_commands.json" "$f" "$OUT" "$SRC" <<'PY'
import json, shlex, sys
cc, f, out, src = sys.argv[1:]
for e in json.load(open(cc)):
    if e['file'].endswith('/' + f) and ('ppc64-softmmu' in e['output'] or 'libsystem' in e['output']):
        t = shlex.split(e['command'])[1:]
        o = ['-I' + out]
        skip = False
        for x in t:
            if skip:
                skip = False; continue
            if x in ('-o', '-MF', '-MQ'):
                skip = True; continue
            if x in ('-MD', '-m64', '-mcx16', '-msse2', '-c', '-Wold-style-declaration') or x.endswith('.c'):
                continue
            # en-têtes système de l'hôte x86-64 : seuls ceux de glib restent
            if x.startswith('-I/usr/') and 'glib-2.0' not in x:
                continue
            x = x.replace('host/include/x86_64', 'host/include/aarch64').replace('tcg/x86_64', 'tcg/aarch64')
            o.append(x)
        o += ['-Wno-unknown-warning-option', '-Wno-gnu-variable-sized-type-not-at-end',
              src + '/' + f]
        print(' '.join(shlex.quote(x) for x in o)); break
PY
)"
  [ -n "$CMD" ] || { echo "$f : absent de compile_commands.json" >&2; rc=1; continue; }
  echo "=== $f"
  if ( cd "$B" && eval "\"$CC\" --target=aarch64-linux-android35 -c -o $OUT/x.o $CMD" ) 2>&1 | \
       tee "$OUT/log" | grep -E 'error|warning' | grep -v 'warnings\? generated' ; then :; fi
  if grep -q 'error' "$OUT/log"; then echo "    ERREURS"; rc=1; else echo "    compilé (aarch64, -c)"; fi
  [ -f "$OUT/x.o" ] && cp "$OUT/x.o" "${A64OBJ_DIR:-$OUT}/$(basename "$f" .c).a64.o" 2>/dev/null
done
exit $rc
