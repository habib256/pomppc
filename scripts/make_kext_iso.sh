#!/usr/bin/env bash
# make_kext_iso.sh — grave les sources des kexts (POMPPCQFB, POMPPCGPU) et du
# programme de test invité (guest/qgpu-test) sur un CD ISO, à monter dans
# l'invité Tiger pour les y compiler (l'hôte n'a pas de toolchain
# ppc-apple-darwin8). Arborescence du CD : kext/{POMPPCQFB,POMPPCGPU},
# guest/{qgpu-test,gldriver,gltest,net}.
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
mkdir -p "$STAGE/kext" "$STAGE/guest"
cp -R "$ROOT/kext/POMPPCQFB" "$ROOT/kext/POMPPCGPU" "$STAGE/kext/"
cp -R "$ROOT/guest/qgpu-test" "$ROOT/guest/gldriver" "$ROOT/guest/gltest" \
      "$ROOT/guest/net" "$STAGE/guest/"

if command -v xorriso >/dev/null; then
  xorriso -as mkisofs -quiet -R -J -V POMPPCSRC -o "$OUT" "$STAGE"
elif command -v hdiutil >/dev/null; then
  # macOS : pas de xorriso, hdiutil sait produire un ISO 9660/Joliet.
  rm -f "$OUT"
  hdiutil makehybrid -quiet -iso -joliet -default-volume-name POMPPCSRC -o "$OUT" "$STAGE"
else
  echo "ni xorriso (apt install xorriso) ni hdiutil : impossible de graver l'ISO" >&2; exit 1
fi
echo "✔ $OUT"
ls -lh "$OUT"
