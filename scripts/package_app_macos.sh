#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# package_app_macos.sh — POMPPC.app et son .dmg, depuis le paquet de package_release_macos.sh.
#
#   scripts/package_release_macos.sh 0.4.1 && scripts/package_app_macos.sh 0.4.1
#
# Signature ad hoc : sans certificat Apple Developer, ni signature d'identité ni notarisation.
# macOS demande donc un accord au premier lancement (README).
# Sortie : dist/POMPPC.app, dist/POMPPC-<version>-macos-arm64.dmg et son .sha256.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="${1:?version attendue, par exemple 0.4.1}"
MIN_MACOS="${MIN_MACOS:-15.0}"
PKG="$ROOT/dist/pomppc-$VER-macos-arm64"
A="$ROOT/scripts/app-macos"
APP="$ROOT/dist/POMPPC.app"
DMG="$ROOT/dist/POMPPC-$VER-macos-arm64.dmg"
[ -x "$PKG/bin/qemu-system-ppc" ] || { echo "$PKG manque : package_release_macos.sh d'abord" >&2; exit 1; }

echo "▶ POMPPC.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
clang -arch arm64 -mmacosx-version-min="$MIN_MACOS" -O2 -Wall -Werror -o "$APP/Contents/MacOS/POMPPC" "$A/lanceur.c"
sed -e "s/@VERSION@/$VER/g" -e "s/@MIN_MACOS@/$MIN_MACOS/g" "$A/Info.plist.in" > "$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" >/dev/null
cp "$A/pomppc-app.sh" "$A/POMPPC.icns" "$APP/Contents/Resources/"
printf '%s\n' "$VER" > "$APP/Contents/Resources/version"
ditto "$PKG" "$APP/Contents/Resources/pomppc"
# Les données de l'utilisateur vivent dans ~/POMPPC, jamais dans l'app.
rm -rf "$APP/Contents/Resources/pomppc/.run" "$APP/Contents/Resources/pomppc/bench"

echo "▶ signature ad hoc"
codesign --force -s - --identifier net.pomppc.POMPPC "$APP"
codesign --verify --strict "$APP"

echo "▶ image disque"
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
ditto "$APP" "$STAGE/POMPPC.app"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -quiet -volname "POMPPC $VER" -srcfolder "$STAGE" -format UDZO -fs HFS+ "$DMG"
( cd "$ROOT/dist" && shasum -a 256 "$(basename "$DMG")" > "$(basename "$DMG").sha256" )
du -h "$DMG" | cut -f1 | sed "s|^|$DMG : |"
