#!/bin/sh
# GPL3 - Copyleft VERHILLE Arnaud
# install.sh — DANS l'invité, en root : construit POMPPCFsqrt.kext, l'installe dans
# /System/Library/Extensions, pose l'élément de démarrage qui le charge à chaque
# démarrage (/Library/StartupItems/POMPPCFsqrt) et le charge tout de suite.
#   cd ~/pomppc-build/fsqrt/POMPPCFsqrt && sudo sh install.sh
# Retrait : sudo sh install.sh --retirer
set -e
cd "$(dirname "$0")"
E=/System/Library/Extensions/POMPPCFsqrt.kext
S=/Library/StartupItems/POMPPCFsqrt

if [ "${1:-}" = "--retirer" ]; then
    kextunload -b net.pomppc.POMPPCFsqrt 2>/dev/null || true
    rm -rf "$E" "$S"
    echo "POMPPCFsqrt retiré"
    exit 0
fi

make
rm -rf "$E" && cp -R POMPPCFsqrt.kext "$E"
chown -R root:wheel "$E" && chmod -R 755 "$E"
rm -rf "$S" && mkdir -p "$S"
cp StartupItem/POMPPCFsqrt StartupItem/StartupParameters.plist "$S/"
chown -R root:wheel "$S" && chmod 755 "$S" "$S/POMPPCFsqrt" && chmod 644 "$S/StartupParameters.plist"
touch /System/Library/Extensions
kextstat | grep -q net.pomppc.POMPPCFsqrt || kextload "$E"
echo "POMPPCFsqrt installé"
