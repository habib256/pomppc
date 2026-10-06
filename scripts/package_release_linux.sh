#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# package_release_linux.sh — paquet publié pour Linux x86-64 : le dépôt au commit courant
# (git archive) + QEMU patché, qemu-img et le frontend (bin/, share/qemu/), le CD invité
# (pomppc-guest.iso) et les licences. Pendant de scripts/package_release_macos.sh.
#
#   QEMU_SRC=~/src/qemu-release scripts/package_release_linux.sh 0.3
#   GUEST_ISO=pomppc-0.3-guest.iso scripts/package_release_linux.sh 0.3
#
# Contrairement au paquet macOS, les bibliothèques ne sont PAS embarquées : sous Linux,
# GTK, GL/EGL (pilote de la carte), PulseAudio et glibc doivent venir du système, et
# les copier casse plus qu'elles ne réparent. Le paquet vise la distribution de l'hôte
# qui le construit ; DEPENDS.txt liste les paquets à installer (apt), tirés de ldd et de
# dpkg -S au moment de l'empaquetage.
#
# Le paquet est générique : $QEMU_SRC/build, jamais build-fast/ (-march=native, PGO :
# binaire propre au PC qui l'a construit, docs/binaire-rapide-x86.md).
#
# Prérequis :
#   - QEMU construit par scripts/build_qemu_qfb.sh dans $QEMU_SRC au commit courant ;
#   - frontend/build/pomppc construit (frontend/README.md) ;
#   - le CD invité : GUEST_ISO=<iso déjà gravé> (celui de la publication macOS du même
#     commit convient, il ne dépend pas de l'hôte), sinon gravé depuis $PREBUILT (défaut
#     disks/prebuilt) par scripts/make_guest_iso.sh.
# Sortie : dist/pomppc-<version>-linux-x86_64.tar.xz et son .sha256.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="${1:?version attendue, p. ex. 0.3}"
QSRC="${QEMU_SRC:-$HOME/src/qemu-release}"
NAME="pomppc-$VER-linux-x86_64"
DIST="$ROOT/dist"; OUT="$DIST/$NAME"

[ "$(uname -s)/$(uname -m)" = Linux/x86_64 ] || { echo "Linux x86-64 seulement" >&2; exit 1; }
[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ] ||
  { echo "arbre de travail modifié : commiter d'abord (le paquet est le commit)" >&2; exit 1; }
for f in "$QSRC/build/qemu-system-ppc" "$QSRC/build/qemu-system-ppc64" "$ROOT/frontend/build/pomppc"; do
  [ -x "$f" ] || { echo "$f manque" >&2; exit 1; }
done
[ -z "${GUEST_ISO:-}" ] || [ -f "$GUEST_ISO" ] || { echo "$GUEST_ISO manque" >&2; exit 1; }
# Le paquet est GÉNÉRIQUE : toujours $QSRC/build (-O2, x86-64 de base), jamais le
# binaire rapide build-fast/ (-march=native : SIGILL sur un autre processeur,
# docs/binaire-rapide-x86.md). Garde contre un build/ configuré à la main en natif.
if grep -qE -- '-march=native|-fprofile-(use|generate)' "$QSRC/build/build.ninja" 2>/dev/null; then
  echo "$QSRC/build est compilé en -march=native ou avec un profil PGO : le paquet doit" >&2
  echo "être générique (reconstruire build/ sans QEMU_OPT)" >&2; exit 1
fi

rm -rf "$OUT" && mkdir -p "$OUT"
echo "▶ dépôt $(git -C "$ROOT" rev-parse --short HEAD)"
git -C "$ROOT" archive HEAD | tar xf - -C "$OUT"

echo "▶ QEMU (ninja install)"
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
DESTDIR="$STAGE" ninja -C "$QSRC/build" install >/dev/null
mkdir -p "$OUT/bin" "$OUT/share"
cp "$STAGE/usr/local/bin/"{qemu-system-ppc,qemu-system-ppc64,qemu-img} "$OUT/bin/"
cp -R "$STAGE/usr/local/share/qemu" "$OUT/share/"
cp "$ROOT/frontend/build/pomppc" "$OUT/bin/"
strip --strip-unneeded "$OUT"/bin/*
"$OUT/bin/qemu-system-ppc64" --version | head -1
"$OUT/bin/qemu-img" --version | head -1

# --- dépendances : paquets de la distribution qui fournissent les bibliothèques ---
echo "▶ dépendances"
missing="$(for f in "$OUT"/bin/*; do ldd "$f"; done | awk '/not found/{print $1}' | sort -u)"
[ -z "$missing" ] || { echo "bibliothèques introuvables :" >&2; echo "$missing" >&2; exit 1; }
libs="$(for f in "$OUT"/bin/*; do ldd "$f"; done | awk '$2 == "=>" && $3 ~ /^\//{print $3}' | sort -u)"
pkgs="$(for l in $libs; do dpkg -S "$(readlink -f "$l")" 2>/dev/null || dpkg -S "$l" 2>/dev/null; done |
        cut -d: -f1 | sort -u)"
distro="$(. /etc/os-release && echo "$PRETTY_NAME")"   # sous-shell : os-release pose NAME
{
  echo "Built on $distro ($(uname -m)), $(date +%Y-%m-%d)."
  echo "Runtime packages (Debian/Ubuntu names), install with:"
  echo
  echo "  sudo apt install $(echo $pkgs)"
  echo
  echo "Other distributions: the same libraries under their own package names"
  echo "(ldd bin/qemu-system-ppc lists them)."
} > "$OUT/DEPENDS.txt"
echo "  $(echo "$pkgs" | wc -l) paquets → DEPENDS.txt"

# --- licences tierces ---
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
  echo "No shared library is bundled: they come from the system (DEPENDS.txt)."
} > "$TP/README.txt"
[ -f "$ROOT/frontend/imgui/LICENSE.txt" ] && cp "$ROOT/frontend/imgui/LICENSE.txt" "$TP/imgui-LICENSE.txt"
cp "$QSRC/COPYING" "$TP/qemu-COPYING"

echo "▶ CD invité"
if [ -n "${GUEST_ISO:-}" ]; then
  cp "$GUEST_ISO" "$OUT/pomppc-guest.iso"
  echo "$OUT/pomppc-guest.iso (repris de $GUEST_ISO)"
else
  PREBUILT="${PREBUILT:-$ROOT/disks/prebuilt}" "$ROOT/scripts/make_guest_iso.sh" "$OUT/pomppc-guest.iso"
fi

echo "▶ archive"
( cd "$DIST" && rm -f "$NAME.tar.xz" && tar cJf "$NAME.tar.xz" "$NAME" &&
  sha256sum "$NAME.tar.xz" > "$NAME.tar.xz.sha256" )
du -h "$DIST/$NAME.tar.xz" | cut -f1 | sed "s|^|$DIST/$NAME.tar.xz : |"
