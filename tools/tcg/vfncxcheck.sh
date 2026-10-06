#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vfncxcheck.sh ARBRE-QEMU [N] [RÉFÉRENCE] — le modèle de x-vfp-native-cmp de
# l'hôte (bloc « vfp-native-cmp » de target/ppc/int_helper.c, extrait TEL
# QUEL : SSE/AVX sur x86-64, NEON sur aarch64 depuis tcg/0038) sur des
# vecteurs fixes (tools/tcg/vfncxcheck.c), une empreinte par (sorte, uim).
#   - sur le PC x86-64 : sortie du modèle x86 ; si un qemu-aarch64 et le clang
#     du NDK sont là (QEMU_AARCH64=, NDK_BIN=), le modèle NEON est aussi
#     compilé pour aarch64 et exécuté sous émulation : les deux sorties doivent
#     être IDENTIQUES ;
#   - sur le M4 : le modèle NEON en natif, comparé à RÉFÉRENCE (la sortie x86
#     du PC, tools/tcg/vfncxcheck-ref.txt pour N = 20000).
# Ne prouve pas l'émetteur réel (voir vfptest c sous x-vfp-native-cmp-verify),
# seulement que les deux modèles décident et calculent pareil.
# docs/parite-arm64-0031-0035.md.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:?arbre QEMU requis}"
N="${2:-20000}"
REF="${3:-$HERE/vfncxcheck-ref.txt}"
OUT="${TMPDIR:-/tmp}/vfncxcheck.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
sed -n '/vfp-native-cmp: début/,/vfp-native-cmp: fin/p' "$SRC/target/ppc/int_helper.c" \
    > "$OUT/vfncxcheck-model.h"
grep -q vfnc_x86 "$OUT/vfncxcheck-model.h" || { echo "arbre sans tcg/0034 ($SRC)" >&2; exit 2; }
grep -q '__aarch64__' "$OUT/vfncxcheck-model.h" || echo "⚠ arbre sans le modèle NEON de tcg/0038"
# MUTATE='expression sed' : mute le modèle extrait (contre-épreuve : avec un
# mutant NEON de vfpcmpproof-mut.sh, le PC doit voir des ÉCARTS x86/NEON)
if [ -n "${MUTATE:-}" ]; then
  cp "$OUT/vfncxcheck-model.h" "$OUT/orig.h"
  sed -i.bak -e "$MUTATE" "$OUT/vfncxcheck-model.h"
  cmp -s "$OUT/orig.h" "$OUT/vfncxcheck-model.h" && { echo "mutation sans effet : $MUTATE" >&2; exit 2; }
fi
cc -O2 -Wall -I"$OUT" -o "$OUT/host" "$HERE/vfncxcheck.c"
"$OUT/host" "$N" > "$OUT/host.txt"
tail -1 "$OUT/host.txt"
rc=0
case "$(uname -m)" in
  x86_64)
    NDK="${NDK_BIN:-$(ls -d "$HOME"/android-sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin 2>/dev/null | sort | tail -1)}"
    QA="${QEMU_AARCH64:-$(command -v qemu-aarch64 || true)}"
    if [ -x "$NDK/clang" ] && [ -n "$QA" ] && [ -x "$QA" ]; then
      "$NDK/clang" --target=aarch64-linux-android35 -O2 -static -Wall -I"$OUT" \
          -o "$OUT/a64" "$HERE/vfncxcheck.c"
      "$QA" "$OUT/a64" "$N" > "$OUT/a64.txt"
      echo "modèle NEON sous $QA :"; tail -1 "$OUT/a64.txt"
      if diff "$OUT/host.txt" "$OUT/a64.txt" > "$OUT/diff.txt"; then
        echo "IDENTIQUES : modèle x86 = modèle NEON ($(grep -c . "$OUT/host.txt") lignes)"
      else
        echo "ÉCARTS :"; head -20 "$OUT/diff.txt"; rc=1
      fi
    else
      echo "(pas de qemu-aarch64 ou de clang aarch64 : modèle x86 seul)"
    fi
    if [ -n "${SAVE_REF:-}" ]; then cp "$OUT/host.txt" "$SAVE_REF"; echo "référence écrite : $SAVE_REF"; fi
    ;;
  arm64|aarch64)
    if [ -f "$REF" ] && [ "$N" = 20000 ]; then
      if diff "$REF" "$OUT/host.txt" > "$OUT/diff.txt"; then
        echo "IDENTIQUES à la référence x86 du PC ($REF)"
      else
        echo "ÉCARTS avec la référence x86 du PC :"; head -20 "$OUT/diff.txt"; rc=1
      fi
    else
      echo "(pas de référence pour N=$N : sortie seule)"; cat "$OUT/host.txt"
    fi
    ;;
esac
exit $rc
