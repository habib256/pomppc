#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# make_guest_iso.sh — grave le paquet invité de la publication (CD POMPPC_GUEST) : binaires
# déjà compilés dans Tiger (kext POMPPCGPU, plugin GLDriver-POMPPC, kext POMPPCFsqrt, gltest)
# et guest/package/install.sh, qui les installe sans Xcode Tools.
#
#   scripts/make_guest_iso.sh [sortie.iso]       # défaut : pomppc-guest.iso à la racine
#   PREBUILT=dossier scripts/make_guest_iso.sh   # défaut : disks/prebuilt
#
# Le dossier des binaires vient de l'invité (gcc-4.0, SDK 10.4u) : job tools/guest/jobs/prebuilt
# (+ make dans kext/POMPPCFsqrt). Refusé s'il est plus vieux que les sources qu'il compile.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/pomppc-guest.iso}"
PRE="${PREBUILT:-$ROOT/disks/prebuilt}"

for f in POMPPCGPU.kext GLDriver-POMPPC.bundle POMPPCFsqrt.kext VERSION; do
  [ -e "$PRE/$f" ] || { echo "$PRE/$f manque" >&2; exit 1; }
done
if [ -n "$(find "$ROOT/kext/POMPPCGPU" "$ROOT/kext/POMPPCFsqrt" "$ROOT/guest/gldriver" \
           "$ROOT/patches/qgpu/qgpu_proto.h" "$ROOT/patches/qgpu/qgpu_abi.h" -type f \
           \( -name '*.[chs]' -o -name '*.cpp' -o -name Makefile -o -name '*.plist' \) \
           -newer "$PRE/VERSION" 2>/dev/null | head -1)" ]; then
  echo "$PRE est plus vieux que les sources du kext ou du plugin : recompiler dans l'invité" >&2
  exit 1
fi

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
cp "$ROOT/guest/package/install.sh" "$PRE/VERSION" "$STAGE/"
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
cp -R "$PRE/POMPPCGPU.kext" "$PRE/GLDriver-POMPPC.bundle" "$PRE/POMPPCFsqrt.kext" "$STAGE/"
mkdir -p "$STAGE/StartupItems/POMPPCFsqrt" "$STAGE/tools"
cp "$ROOT/kext/POMPPCFsqrt/StartupItem/POMPPCFsqrt" \
   "$ROOT/kext/POMPPCFsqrt/StartupItem/StartupParameters.plist" "$STAGE/StartupItems/POMPPCFsqrt/"
for t in gltest glwin accelprobe; do [ -f "$PRE/$t" ] && cp "$PRE/$t" "$STAGE/tools/"; done
cat > "$STAGE/README.txt" <<EOF
POMPPC guest package $(head -1 "$PRE/VERSION") — Mac OS X 10.4 Tiger (PowerPC)

Paravirtual 3D GPU (qgpu) and fsqrt support for Tiger running in POMPPC's QEMU.
In Tiger, open Terminal and type:

    sudo sh /Volumes/POMPPC_GUEST/install.sh

then reboot. "sudo sh /Volumes/POMPPC_GUEST/install.sh --remove" uninstalls.
tools/gltest and tools/glwin are small OpenGL test programs.
Source code and documentation: https://github.com/habib256/pomppc (GPL-3.0-or-later).
EOF
rm -f "$OUT"
if command -v hdiutil >/dev/null; then
  hdiutil makehybrid -quiet -iso -joliet -hfs -default-volume-name POMPPC_GUEST -o "$OUT" "$STAGE"
elif command -v xorriso >/dev/null; then
  # Linux : hybride ISO 9660 (Rock Ridge, Joliet) + HFS+, que Tiger monte comme le HFS de hdiutil
  xorriso -as mkisofs -quiet -r -J -hfsplus -V POMPPC_GUEST -o "$OUT" "$STAGE"
else
  echo "ni hdiutil ni xorriso (apt install xorriso) : impossible de graver l'ISO" >&2; exit 1
fi
echo "$OUT ($(du -h "$OUT" | cut -f1))"
