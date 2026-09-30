#!/usr/bin/env bash
# GPL3 - Copyleft VERHILLE Arnaud
# build.sh [NOM] — construit le greffon TCG NOM (défaut ppcmix ; fprun) en
# tools/tcg/libNOM.dylib|.so contre les en-têtes d'un arbre QEMU (QEMU_SRC,
# défaut ~/src/qemu-tcg).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${QEMU_SRC:-$HOME/src/qemu-tcg}"
NAME="${1:-ppcmix}"
GLIB_CFLAGS="$(pkg-config --cflags glib-2.0)"
case "$(uname -s)" in
  Darwin) OUT="$HERE/lib$NAME.dylib"; LD=(-bundle -Wl,-undefined,dynamic_lookup) ;;
  *)      OUT="$HERE/lib$NAME.so";    LD=(-shared -fPIC) ;;
esac
# shellcheck disable=SC2086
cc -O2 -Wall -fPIC $GLIB_CFLAGS -I"$SRC/include/qemu" "$HERE/$NAME.c" \
   "${LD[@]}" -lpthread -o "$OUT"
echo "$OUT"
