#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# package_release_macos.sh — paquet publié pour macOS Apple Silicon : le dépôt au commit
# courant (git archive) + QEMU patché, qemu-img et le frontend, avec leurs dylibs Homebrew
# embarquées (bin/, lib/, share/qemu/), le CD invité (pomppc-guest.iso) et les licences tierces.
#
#   QEMU_SRC=~/src/qemu-release scripts/package_release_macos.sh v0.1.0
#
# Prérequis :
#   - QEMU construit par scripts/build_qemu_qfb.sh dans $QEMU_SRC au commit courant, de
#     préférence avec CONFIGURE_EXTRA=--disable-sdl (sdl2-compat charge SDL3 à l'exécution) ;
#   - frontend/build/pomppc construit (frontend/README.md) ;
#   - binaires invités dans $PREBUILT (défaut disks/prebuilt), voir scripts/make_guest_iso.sh.
# Sortie : dist/pomppc-<version>-macos-arm64.tar.xz et son .sha256.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="${1:?version attendue, p. ex. v0.1.0}"
QSRC="${QEMU_SRC:-$HOME/src/qemu-release}"
NAME="pomppc-$VER-macos-arm64"
DIST="$ROOT/dist"; OUT="$DIST/$NAME"

[ "$(uname -s)/$(uname -m)" = Darwin/arm64 ] || { echo "macOS arm64 seulement" >&2; exit 1; }
[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ] ||
  { echo "arbre de travail modifié : commiter d'abord (le paquet est le commit)" >&2; exit 1; }
for f in "$QSRC/build/qemu-system-ppc" "$QSRC/build/qemu-system-ppc64" "$ROOT/frontend/build/pomppc"; do
  [ -x "$f" ] || { echo "$f manque" >&2; exit 1; }
done

rm -rf "$OUT" && mkdir -p "$OUT"
echo "▶ dépôt $(git -C "$ROOT" rev-parse --short HEAD)"
git -C "$ROOT" archive HEAD | tar xf - -C "$OUT"

echo "▶ QEMU (ninja install)"
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
DESTDIR="$STAGE" ninja -C "$QSRC/build" install >/dev/null
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/share"
cp "$STAGE/usr/local/bin/"{qemu-system-ppc,qemu-system-ppc64,qemu-img} "$OUT/bin/"
cp -R "$STAGE/usr/local/share/qemu" "$OUT/share/"
cp "$ROOT/frontend/build/pomppc" "$OUT/bin/"

# --- dylibs : tout ce qui n'est pas au système, transitivement, dans lib/ ---
echo "▶ dylibs"
deps() { otool -L "$1" | awk 'NR>1{print $1}' | grep -v -e '^/System/' -e '^/usr/lib/' -e '^@' || true; }
# bash 3.2 de macOS : pas de tableau associatif ; lib/ fait l'ensemble, $ORIG la table
# nom → chemin réel Homebrew (pour les licences).
ORIG="$STAGE/origins.txt"; : > "$ORIG"
queue=("$OUT"/bin/*)
while [ ${#queue[@]} -gt 0 ]; do
  f="${queue[0]}"; queue=("${queue[@]:1}")
  for d in $(deps "$f"); do
    base="$(basename "$d")"
    if [ ! -e "$OUT/lib/$base" ]; then
      real="$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$d")"
      echo "$base $real" >> "$ORIG"
      cp "$real" "$OUT/lib/$base"; chmod u+w "$OUT/lib/$base"
      queue+=("$OUT/lib/$base")
    fi
    case "$f" in
      "$OUT"/bin/*) install_name_tool -change "$d" "@executable_path/../lib/$base" "$f" 2>/dev/null ;;
      *)            install_name_tool -change "$d" "@loader_path/$base" "$f" 2>/dev/null ;;
    esac
  done
  case "$f" in "$OUT"/lib/*) install_name_tool -id "@rpath/$(basename "$f")" "$f" 2>/dev/null ;; esac
done
left="$(for f in "$OUT"/bin/* "$OUT"/lib/*; do deps "$f"; done | sort -u)"
[ -z "$left" ] || { echo "références hors paquet restantes :" >&2; echo "$left" >&2; exit 1; }

echo "▶ signature ad hoc"
for f in "$OUT"/lib/*; do codesign --force -s - "$f" 2>/dev/null; done
ENT="$QSRC/accel/hvf/entitlements.plist"
for f in "$OUT"/bin/*; do
  case "$(basename "$f")" in
    qemu-system-*) codesign --force -s - --entitlements "$ENT" "$f" 2>/dev/null ;;
    *)             codesign --force -s - "$f" 2>/dev/null ;;
  esac
done
"$OUT/bin/qemu-system-ppc64" --version | head -1
"$OUT/bin/qemu-img" --version | head -1

# --- licences tierces : textes de licence des formules Homebrew embarquées ---
echo "▶ licences tierces"
TP="$OUT/THIRD-PARTY-LICENSES"; mkdir -p "$TP"
{
  echo "Third-party components bundled in $NAME"
  echo
  echo "QEMU $(cat "$QSRC/VERSION") (GPL-2.0, https://www.qemu.org) with POMPPC's patches (patches/, GPL-2.0-or-later);"
  echo "  source: https://gitlab.com/qemu-project/qemu.git tag v$(cat "$QSRC/VERSION") + scripts/build_qemu_qfb.sh."
  echo "OpenBIOS (GPL-2.0): patches/smp-mac99/openbios-smp-screamer.elf, source in"
  echo "  patches/smp-mac99/openbios-smp-screamer-source.patch."
  echo "Dear ImGui (MIT, https://github.com/ocornut/imgui), statically linked in bin/pomppc."
  echo
  echo "Dynamic libraries in lib/, copied from Homebrew (license texts in this folder):"
  sort "$ORIG" | while read -r base real; do
    keg="${real%/lib/*}"
    echo "  $base  ←  $(basename "$(dirname "$keg")") $(basename "$keg")"
  done
} > "$TP/README.txt"
while read -r base real; do
  keg="${real%/lib/*}"; formula="$(basename "$(dirname "$keg")")"
  for l in "$keg"/{COPYING,LICENSE,LICENCE,COPYRIGHT}*; do
    [ -f "$l" ] && cp "$l" "$TP/$formula-$(basename "$l")"
  done
done < "$ORIG"
[ -f "$ROOT/frontend/imgui/LICENSE.txt" ] && cp "$ROOT/frontend/imgui/LICENSE.txt" "$TP/imgui-LICENSE.txt"
cp "$QSRC/COPYING" "$TP/qemu-COPYING"

echo "▶ CD invité"
PREBUILT="${PREBUILT:-$ROOT/disks/prebuilt}" "$ROOT/scripts/make_guest_iso.sh" "$OUT/pomppc-guest.iso"

echo "▶ archive"
( cd "$DIST" && rm -f "$NAME.tar.xz" && tar cJf "$NAME.tar.xz" "$NAME" &&
  shasum -a 256 "$NAME.tar.xz" > "$NAME.tar.xz.sha256" )
du -h "$DIST/$NAME.tar.xz" | cut -f1 | sed "s|^|$DIST/$NAME.tar.xz : |"
