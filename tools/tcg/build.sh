#!/usr/bin/env bash
# build.sh — construit le greffon TCG ppcmix (tools/tcg/libppcmix.dylib|.so)
# contre les en-têtes d'un arbre QEMU (QEMU_SRC, défaut ~/src/qemu-tcg).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${QEMU_SRC:-$HOME/src/qemu-tcg}"
GLIB_CFLAGS="$(pkg-config --cflags glib-2.0)"
case "$(uname -s)" in
  Darwin) OUT="$HERE/libppcmix.dylib"; LD=(-bundle -Wl,-undefined,dynamic_lookup) ;;
  *)      OUT="$HERE/libppcmix.so";    LD=(-shared -fPIC) ;;
esac
# shellcheck disable=SC2086
cc -O2 -Wall -fPIC $GLIB_CFLAGS -I"$SRC/include/qemu" "$HERE/ppcmix.c" \
   "${LD[@]}" -lpthread -o "$OUT"
echo "$OUT"
