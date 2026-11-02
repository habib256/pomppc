#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# build_release_deps_macos.sh — bibliothèques du paquet macOS recompilées pour une version minimale.
#
# Les bouteilles Homebrew visent le macOS de la machine : copiées telles quelles, le paquet
# publié exigerait ce macOS-là. Le paquet n'a pas besoin de Homebrew à l'exécution : ce script
# recompile, aux versions et sommes de contrôle de Homebrew, toutes ses bibliothèques.
# scripts/package_release_macos.sh les préfère ensuite à celles de Homebrew (RELEASE_DEPS).
#
#   scripts/build_release_deps_macos.sh [dossier]
#   (défaut : ~/src/pomppc-deps ; MIN_MACOS=15.0 ; FORMULES="…" pour n'en construire que certaines)
#
# Sortie : <dossier>/<formule>/<version>/lib, avec les licences à la racine de chaque formule.
set -euo pipefail
DEPS="${1:-$HOME/src/pomppc-deps}"
export MACOSX_DEPLOYMENT_TARGET="${MIN_MACOS:-15.0}"
# dans l'ordre des dépendances
FORMULES="${FORMULES:-pcre2 libpng libunistring gettext gmp nettle libtasn1 libidn2 p11-kit gnutls zstd jpeg-turbo libusb pixman glib libslirp glfw}"
JOBS="$(sysctl -n hw.ncpu)"
mkdir -p "$DEPS/src"

formule_info() {
  brew info --json=v2 "$1" | python3 -c '
import json, sys
d = json.load(sys.stdin)["formulae"][0]
s = d["urls"]["stable"]
print(d["versions"]["stable"], s["url"], s["checksum"])'
}

meson_install() {
  local prefixe="$1"; shift
  meson setup build --prefix="$prefixe" --libdir=lib --buildtype=release --wrap-mode=nofallback "$@" >/dev/null
  meson compile -C build >/dev/null
  meson install -C build >/dev/null
}

cmake_install() {
  local prefixe="$1"; shift
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefixe" \
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_NAME_DIR="$prefixe/lib" "$@" >/dev/null
  cmake --build build -j"$JOBS" >/dev/null
  cmake --install build >/dev/null
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
    # tout ce qui est déjà construit ici passe avant Homebrew
    PKG_CONFIG_PATH=""; CPPFLAGS=""; LDFLAGS=""
    for d in "$DEPS"/*/*/lib; do
      [ -d "$d" ] || continue
      PKG_CONFIG_PATH="$d/pkgconfig:$PKG_CONFIG_PATH"
      CPPFLAGS="-I${d%/lib}/include $CPPFLAGS"
      LDFLAGS="-L$d $LDFLAGS"
    done
    export PKG_CONFIG_PATH CPPFLAGS LDFLAGS
    case "$f" in
      glib)
        meson_install "$pre" -Dintrospection=disabled -Dtests=false -Ddtrace=disabled \
          -Dbsymbolic_functions=false -Dgio_module_dir="$pre/lib/gio/modules" ;;
      p11-kit)
        meson_install "$pre" -Dsystemd=disabled -Dman=false -Dgtk_doc=false -Dbash_completion=disabled \
          -Dsystem_config="$pre/etc" -Dmodule_config="$pre/etc/pkcs11/modules" \
          -Dtrust_paths=/private/etc/ssl/cert.pem -Dtest=false ;;
      pixman)
        meson_install "$pre" -Dgtk=disabled -Dlibpng=disabled -Dtests=disabled -Ddemos=disabled ;;
      libslirp)
        meson_install "$pre" ;;
      gettext)
        # libintl seul : gettext-runtime, sans les outils ni leurs dépendances
        cd gettext-runtime
        am_cv_func_iconv_works=yes ./configure --prefix="$pre" --disable-static --disable-java \
          --disable-csharp --with-included-gettext >/dev/null
        make -j"$JOBS" >/dev/null
        make install >/dev/null
        cd ..
        ;;
      gmp)
        ./configure --prefix="$pre" --disable-static --with-pic \
          --build="aarch64-apple-darwin$(uname -r | cut -d. -f1)" >/dev/null
        make -j"$JOBS" >/dev/null
        make install >/dev/null
        ;;
      nettle)
        ./configure --prefix="$pre" --libdir="$pre/lib" --enable-shared --disable-static --disable-documentation >/dev/null
        make -j"$JOBS" >/dev/null
        make install >/dev/null
        ;;
      gnutls)
        ./configure --prefix="$pre" --disable-static --disable-doc --disable-tests --disable-libdane \
          --disable-heartbeat-support --with-p11-kit --disable-tools --sysconfdir="$pre/etc" \
          --without-brotli --without-zstd \
          --with-default-trust-store-file=/private/etc/ssl/cert.pem >/dev/null
        make -j"$JOBS" >/dev/null
        make install >/dev/null
        ;;
      zstd)
        make -C lib -j"$JOBS" lib-release PREFIX="$pre" >/dev/null
        make -C lib install-pc install-shared install-includes PREFIX="$pre" >/dev/null
        ;;
      jpeg-turbo)
        cmake_install "$pre" -DWITH_JPEG8=1 -DENABLE_STATIC=0 ;;
      glfw)
        cmake_install "$pre" -DBUILD_SHARED_LIBS=ON -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF ;;
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
# Ni bouteille repêchée par un configure (elle reviendrait dans le paquet avec son macOS), ni
# @rpath, que l'empaquetage ne suit pas.
repeche="$(find "$DEPS" -path "$DEPS/src" -prune -o -type f -name '*.dylib' -exec otool -L {} + | grep -E '^[[:space:]]*(/opt/homebrew|/usr/local|@rpath)' | sort -u || true)"
[ -z "$repeche" ] || { echo "références hors de ce dossier :" >&2; echo "$repeche" >&2; exit 1; }
