#!/usr/bin/env bash
# make_kext_iso.sh — grave les sources des kexts (POMPPCQFB, POMPPCGPU) et du
# programme de test invité (guest/qgpu-test) sur un CD ISO, à monter dans
# l'invité Tiger pour les y compiler (l'hôte n'a pas de toolchain
# ppc-apple-darwin8). Arborescence du CD : kext/{POMPPCQFB,POMPPCGPU},
# guest/{qgpu-test,gldriver,gltest,net}, et prebuilt/ (kext, plugin, gltest,
# glwin déjà compilés) si disks/prebuilt existe.
#
#   ./scripts/make_kext_iso.sh
#   GPU=1 ./run_tiger.sh     # (re)grave l'ISO si besoin et l'insère lui-même
#   puis dans l'invité : cp -R /Volumes/POMPPCSRC /tmp/src &&
#                        sudo sh /tmp/src/guest/gldriver/install.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/disks/pomppc-src.iso}"

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
# Même disposition que le dépôt : le plugin retrouve le kext en ../../kext.
mkdir -p "$STAGE/kext" "$STAGE/guest" "$STAGE/patches/qgpu"
cp -R "$ROOT/kext/POMPPCQFB" "$ROOT/kext/POMPPCGPU" "$STAGE/kext/"
# v19 : le contrat du plugin et de qgpu_test (le kext n'a que qgpu_abi.h)
cp "$ROOT/patches/qgpu/qgpu_proto.h" "$ROOT/patches/qgpu/qgpu_abi.h" "$STAGE/patches/qgpu/"
cp -R "$ROOT/guest/qgpu-test" "$ROOT/guest/gldriver" "$ROOT/guest/gltest" \
      "$ROOT/guest/net" "$STAGE/guest/"
# Binaires déjà compilés (job tools/guest/jobs/prebuilt, rangés dans
# disks/prebuilt, hors git) : install.sh s'en sert quand l'invité n'a pas les
# Xcode Tools. Signalés s'ils sont plus vieux que les sources.
if [ -d "$ROOT/disks/prebuilt" ]; then
  cp -R "$ROOT/disks/prebuilt" "$STAGE/prebuilt"
  if [ -n "$(find "$ROOT/kext/POMPPCGPU" "$ROOT/guest/gldriver" "$ROOT/guest/gltest" -type f \
             \( -name '*.[chs]' -o -name '*.cpp' -o -name Makefile -o -name '*.plist' \) \
             -newer "$ROOT/disks/prebuilt/VERSION" 2>/dev/null | head -1)" ]; then
    echo "⚠  disks/prebuilt est plus ancien que les sources du kext ou du plugin :" >&2
    echo "   relancer le job tools/guest/jobs/prebuilt" >&2
  fi
fi

if command -v xorriso >/dev/null; then
  # -r et non -R : Rock Ridge « rationalisé » (propriétaire root, tout lisible
  # par tous, bits d'exécution gardés). Avec -R, la racine gardait le mode 700
  # de mktemp et l'uid de l'hôte : Tiger affichait le CD comme un dossier
  # interdit (vu le 19/09/2026, hôte Linux).
  xorriso -as mkisofs -quiet -r -J -V POMPPCSRC -o "$OUT" "$STAGE"
elif command -v hdiutil >/dev/null; then
  # macOS : pas de xorriso, hdiutil sait produire un ISO 9660/Joliet.
  rm -f "$OUT"
  hdiutil makehybrid -quiet -iso -joliet -default-volume-name POMPPCSRC -o "$OUT" "$STAGE"
else
  echo "ni xorriso (apt install xorriso) ni hdiutil : impossible de graver l'ISO" >&2; exit 1
fi
echo "✔ $OUT"
ls -lh "$OUT"
