#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# build_release_deps_macos.sh — bibliothèques du paquet macOS recompilées pour une version minimale.
#
# Les bouteilles Homebrew visent le macOS de la machine : après une mise à jour de
# l'hôte, le paquet publié exigerait ce macOS-là. Ce script recompile, aux versions
# et sommes de contrôle de Homebrew, les formules dont la bouteille vise trop haut.
# scripts/package_release_macos.sh les préfère ensuite à celles de Homebrew (RELEASE_DEPS).
#
#   scripts/build_release_deps_macos.sh [dossier]
#   (défaut : ~/src/pomppc-deps ; MIN_MACOS=26.0 ; FORMULES="pcre2 libpng libunistring glib")
#
# Sortie : <dossier>/<formule>/<version>/lib, avec les licences à la racine de chaque formule.
set -euo pipefail
DEPS="${1:-$HOME/src/pomppc-deps}"
export MACOSX_DEPLOYMENT_TARGET="${MIN_MACOS:-26.0}"
FORMULES="${FORMULES:-pcre2 libpng libunistring glib}"
JOBS="$(sysctl -n hw.ncpu)"
mkdir -p "$DEPS/src"

formule_info() {
  brew info --json=v2 "$1" | python3 -c '
import json, sys
d = json.load(sys.stdin)["formulae"][0]
s = d["urls"]["stable"]
print(d["versions"]["stable"], s["url"], s["checksum"])'
}

for f in $FORMULES; do
  read -r ver url sum <<< "$(formule_info "$f")"
  pre="$DEPS/$f/$ver"
  if [ -d "$pre/lib" ]; then
    echo "▶ $f $ver : déjà construit"
    continue
  fi
  echo "▶ $f $ver (macOS $MACOSX_DEPLOYMENT_TARGET minimum)"
  arch="$DEPS/src/$(basename "$url")"
  # Le miroir GNU de la formule peut refuser la connexion : la somme de contrôle juge la source.
  if [ ! -f "$arch" ]; then
    chemin="${url#https://ftpmirror.gnu.org/}"
    for u in "$url" "https://ftp.gnu.org/gnu/$chemin" "https://mirrors.kernel.org/gnu/$chemin"; do
      if curl -fsSL --retry 2 -o "$arch.tmp" "$u"; then
        mv "$arch.tmp" "$arch"
        break
      fi
    done
    [ -f "$arch" ] || { echo "$f : source introuvable" >&2; exit 1; }
  fi
  echo "$sum  $arch" | shasum -a 256 -c - >/dev/null
  rm -rf "$DEPS/src/$f-build"
  mkdir -p "$DEPS/src/$f-build"
  tar xf "$arch" -C "$DEPS/src/$f-build" --strip-components=1
  (
    cd "$DEPS/src/$f-build"
    case "$f" in
      glib)
        gt="$(brew --prefix gettext)"
        pc="$(formule_info pcre2 | cut -d' ' -f1)"
        export PKG_CONFIG_PATH="$DEPS/pcre2/$pc/lib/pkgconfig:$gt/lib/pkgconfig"
        export CPPFLAGS="-I$gt/include" LDFLAGS="-L$gt/lib"
        meson setup build --prefix="$pre" --libdir=lib --buildtype=release --wrap-mode=nofallback \
          -Dintrospection=disabled -Dtests=false -Ddtrace=disabled -Dbsymbolic_functions=false \
          -Dgio_module_dir="$(brew --prefix)/lib/gio/modules" >/dev/null
        meson compile -C build >/dev/null
        meson install -C build >/dev/null
        ;;
      *)
        ./configure --prefix="$pre" --disable-static >/dev/null
        make -j"$JOBS" >/dev/null
        make install >/dev/null
        ;;
    esac
    for l in COPYING* LICENSE* LICENCE* COPYRIGHT*; do
      if [ -f "$l" ]; then cp "$l" "$pre/"; fi
    done
  )
  rm -rf "$DEPS/src/$f-build"
done

# Rien au-dessus de la cible : le compilateur peut s'être fait ignorer.
echo "▶ contrôle"
bad=0
while IFS= read -r lib; do
  m="$(vtool -show-build "$lib" 2>/dev/null | awk '/minos/{print $2; exit}')"
  echo "  $m $lib"
  [ "$(printf '%s\n%s\n' "$m" "$MACOSX_DEPLOYMENT_TARGET" | sort -V | tail -1)" = "$MACOSX_DEPLOYMENT_TARGET" ] || bad=1
done < <(find "$DEPS" -path "$DEPS/src" -prune -o -type f -name '*.dylib' -print | sort)
[ "$bad" = 0 ] || { echo "bibliothèque au-dessus de macOS $MACOSX_DEPLOYMENT_TARGET" >&2; exit 1; }
