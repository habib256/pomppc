#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# install.sh — POMPPC guest package for Mac OS X 10.4 Tiger (PowerPC), prebuilt binaries.
# Runs INSIDE the guest, from the POMPPC_GUEST CD (scripts/make_guest_iso.sh); no Xcode needed.
#
#   sudo sh /Volumes/POMPPC_GUEST/install.sh            install everything, then reboot
#   sudo sh /Volumes/POMPPC_GUEST/install.sh --remove   remove everything, then reboot
#
#   POMPPCGPU.kext          → /System/Library/Extensions  (transport of the qgpu-pci device)
#   GLDriver-POMPPC.bundle  → /System/Library/Extensions  (OpenGL plugin, renders on the host GPU)
#   POMPPCFsqrt.kext        → /System/Library/Extensions  (advertises fsqrt: faster libm sqrt)
#   StartupItems/POMPPCFsqrt → /Library/StartupItems      (loads POMPPCFsqrt at boot)
#
# Nothing of Apple's is modified: without the qgpu-pci device there is no accelerator, so
# OpenGL.framework never loads the plugin. Same steps as guest/gldriver/install.sh and
# kext/POMPPCFsqrt/install.sh, which build from source with gcc-4.0.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
EXT=/System/Library/Extensions
SI=/Library/StartupItems/POMPPCFsqrt
RES=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources

if [ "$(id -u)" != 0 ]; then
  echo "run me as root: sudo sh $0 $*"; exit 1
fi

if [ "$1" = "--remove" ]; then
  kextunload -b net.pomppc.POMPPCGPU 2>/dev/null || true
  kextunload -b net.pomppc.POMPPCFsqrt 2>/dev/null || true
  rm -rf "$EXT/POMPPCGPU.kext" "$EXT/GLDriver-POMPPC.bundle" "$EXT/POMPPCFsqrt.kext" "$SI" \
         "$RES/GLDriver-POMPPC.bundle" "$RES/GLDriverPOMPPC.bundle"
  rm -f /System/Library/Extensions.mkext /System/Library/Extensions.kextcache
  touch "$EXT"
  echo "POMPPC guest package removed; reboot."
  exit 0
fi

for f in POMPPCGPU.kext GLDriver-POMPPC.bundle POMPPCFsqrt.kext StartupItems/POMPPCFsqrt; do
  [ -e "$HERE/$f" ] || { echo "missing $HERE/$f: incomplete CD"; exit 1; }
done
echo "POMPPC guest package $(head -1 "$HERE/VERSION" 2>/dev/null)"

put() { # put <source> <destination>
  rm -rf "$2"; cp -R "$1" "$2"
  chown -R root:wheel "$2"; chmod -R 755 "$2"
}

kextunload -b net.pomppc.POMPPCGPU 2>/dev/null || true
put "$HERE/POMPPCGPU.kext" "$EXT/POMPPCGPU.kext"
# older POMPPC layouts put the plugin in OpenGL.framework's Resources: GLEngine would load it twice
rm -rf "$RES/GLDriver-POMPPC.bundle" "$RES/GLDriverPOMPPC.bundle"
put "$HERE/GLDriver-POMPPC.bundle" "$EXT/GLDriver-POMPPC.bundle"
put "$HERE/POMPPCFsqrt.kext" "$EXT/POMPPCFsqrt.kext"
# POMPPCFsqrt links against com.apple.kernel (non-KPI symbols): its dependency must name the
# running kernel's exact version (8.6.0 on 10.4.6, 8.11.0 on 10.4.11).
sed "/<key>com.apple.kernel<\/key>/{n;s|<string>[^<]*</string>|<string>$(uname -r)</string>|;}" \
    "$HERE/POMPPCFsqrt.kext/Contents/Info.plist" > "$EXT/POMPPCFsqrt.kext/Contents/Info.plist"
chmod 644 "$EXT/POMPPCFsqrt.kext/Contents/Info.plist"
put "$HERE/StartupItems/POMPPCFsqrt" "$SI"
chmod 644 "$SI/StartupParameters.plist"
rm -f /System/Library/Extensions.mkext /System/Library/Extensions.kextcache
touch "$EXT"
echo "installed. Reboot, then check:"
echo "  kextstat | grep pomppc     (net.pomppc.POMPPCGPU and net.pomppc.POMPPCFsqrt)"
echo "  an OpenGL application reports GL_RENDERER = \"POMPPC qgpu (OpenGL host GPU)\""
