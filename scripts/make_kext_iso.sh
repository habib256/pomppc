#!/usr/bin/env bash
# make_kext_iso.sh — grave les sources du kext sur un CD ISO, à monter dans
# l'invité Tiger pour l'y compiler (l'hôte Linux n'a pas de toolchain
# ppc-apple-darwin8).
#
#   ./scripts/make_kext_iso.sh
#   QFB=1 SNAPSHOT=1 \
#   EXTRA_ARGS="-drive file=disks/pomppcqfb-src.iso,if=ide,media=cdrom" ./run_tiger.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/disks/pomppcqfb-src.iso}"

command -v xorriso >/dev/null || { echo "xorriso manquant (apt install xorriso)" >&2; exit 1; }

xorriso -as mkisofs -quiet -R -J -V POMPPCQFB -o "$OUT" "$ROOT/kext/POMPPCQFB"
echo "✔ $OUT"
ls -lh "$OUT"
