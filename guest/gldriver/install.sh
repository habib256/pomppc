#!/bin/sh
# install.sh — installe (ou retire) le GPU paravirtuel dans le système Tiger.
#
#   sudo sh install.sh            # compile et installe kext + plugin OpenGL
#   sudo sh install.sh --remove   # retire les deux
#
# À lancer depuis une copie de l'arborescence du dépôt (le CD de
# scripts/make_kext_iso.sh, ou /pomppc) : ../../kext/POMPPCGPU et
# ../../patches/qgpu (qgpu_proto.h, qgpu_abi.h) doivent exister, ou
# KEXTSRC=<dossier des sources du kext> et PROTOSRC=<dossier du contrat>.
#
# Sans Xcode Tools (pas de gcc-4.0), les binaires déjà compilés du CD
# (../../prebuilt, job tools/guest/jobs/prebuilt) sont installés à la place ;
# PREBUILT=1 les impose même avec gcc.
#
#   kext   → /System/Library/Extensions/POMPPCGPU.kext (chargé au démarrage) ;
#            il publie un IOAccelerator dont IOGLBundleName = GLDriver-POMPPC
#   plugin → /System/Library/Extensions/GLDriver-POMPPC.bundle, que GLEngine
#            charge alors comme le pilote d'une carte, AVANT le GLDriver
#            d'Apple (docs/re/accelerateur-iokit.md). Sans le device, pas
#            d'accélérateur, donc pas de plugin : rien d'Apple n'est modifié.
#
# Les installations d'avant la tâche 4.2 posaient le plugin dans le dossier
# Resources d'OpenGL.framework : cette copie est retirée (sinon GLEngine
# chargerait le plugin deux fois).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KEXTSRC=${KEXTSRC:-$HERE/../../kext/POMPPCGPU}
EXT=/System/Library/Extensions
RES=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources

if [ "$1" = "--remove" ]; then
  kextunload -b net.pomppc.POMPPCGPU 2>/dev/null || true
  rm -rf "$EXT/POMPPCGPU.kext" "$EXT/GLDriver-POMPPC.bundle" \
         "$RES/GLDriver-POMPPC.bundle" "$RES/GLDriverPOMPPC.bundle"
  touch "$EXT"
  echo "retiré ; redémarrer pour oublier le renderer"
  exit 0
fi

PRE=$HERE/../../prebuilt
if [ -z "$PREBUILT" ] && [ -x /usr/bin/gcc-4.0 ]; then
  KEXT="$KEXTSRC/POMPPCGPU.kext"; BUNDLE="$HERE/GLDriver-POMPPC.bundle"
  echo "▶ kext"
  ( cd "$KEXTSRC" && make clean >/dev/null && make )
elif [ -d "$PRE/POMPPCGPU.kext" ] && [ -d "$PRE/GLDriver-POMPPC.bundle" ]; then
  KEXT="$PRE/POMPPCGPU.kext"; BUNDLE="$PRE/GLDriver-POMPPC.bundle"
  echo "▶ pas de gcc-4.0 : binaires déjà compilés ($(cat "$PRE/VERSION" 2>/dev/null | head -1))"
else
  echo "gcc-4.0 absent et pas de binaires déjà compilés : installer les Xcode Tools du DVD"
  exit 1
fi

kextunload -b net.pomppc.POMPPCGPU 2>/dev/null || true
rm -rf "$EXT/POMPPCGPU.kext"
cp -R "$KEXT" "$EXT/"
chown -R root:wheel "$EXT/POMPPCGPU.kext"
chmod -R 755 "$EXT/POMPPCGPU.kext"
rm -f /System/Library/Extensions.mkext /System/Library/Extensions.kextcache
touch "$EXT"
kextload "$EXT/POMPPCGPU.kext" 2>/dev/null || true

echo "▶ plugin OpenGL"
if [ "$BUNDLE" = "$HERE/GLDriver-POMPPC.bundle" ]; then
  # v19 : le contrat du plugin (qgpu_proto.h + qgpu_abi.h) vient de patches/qgpu/ ;
  # le kext, lui, n'a que qgpu_abi.h. PROTOSRC=<dossier> pour une autre source.
  PROTOSRC=${PROTOSRC:-$HERE/../../patches/qgpu}
  cp "$PROTOSRC/qgpu_proto.h" "$PROTOSRC/qgpu_abi.h" "$HERE/"
  ( cd "$HERE" && make clean >/dev/null && make )
fi
if [ ! -f "$BUNDLE/Contents/MacOS/GLDriver-POMPPC" ]; then
  echo "$BUNDLE : disposition d'avant la tâche 4.2 (binaire à plat) — recompiler"
  exit 1
fi
rm -rf "$RES/GLDriver-POMPPC.bundle" "$RES/GLDriverPOMPPC.bundle" "$EXT/GLDriver-POMPPC.bundle"
cp -R "$BUNDLE" "$EXT/"
chown -R root:wheel "$EXT/GLDriver-POMPPC.bundle"
chmod -R 755 "$EXT/GLDriver-POMPPC.bundle"

echo "✔ installé. Redémarrer, puis vérifier : kextstat | grep -i pomppc ;"
echo "  ioreg -c POMPPCAccelerator -w 0 (IOGLBundleName) et"
echo "  ioreg -c IONDRVFramebuffer -w 0 | grep IOAccel (le framebuffer qui le désigne) ;"
echo "  une application GL doit rapporter GL_RENDERER = « POMPPC qgpu (OpenGL host GPU) »."
