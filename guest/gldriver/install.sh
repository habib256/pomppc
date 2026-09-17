#!/bin/sh
# install.sh — installe (ou retire) le GPU paravirtuel dans le système Tiger.
#
#   sudo sh install.sh            # compile et installe kext + plugin OpenGL
#   sudo sh install.sh --remove   # retire les deux
#
# À lancer depuis une copie de l'arborescence du dépôt (le CD de
# scripts/make_kext_iso.sh, ou /pomppc) : ../../kext/POMPPCGPU doit exister,
# ou KEXTSRC=<dossier des sources du kext>.
#
#   kext   → /System/Library/Extensions/POMPPCGPU.kext (chargé au démarrage)
#   plugin → /System/Library/Frameworks/OpenGL.framework/Versions/A/Resources/
#            GLDriver-POMPPC.bundle, que GLEngine charge pour toute application
#            à côté du GLDriver d'Apple. Le renderer « POMPPC qgpu » est
#            annoncé accéléré : CGL le préfère dès qu'il est disponible.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KEXTSRC=${KEXTSRC:-$HERE/../../kext/POMPPCGPU}
EXT=/System/Library/Extensions
RES=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources

if [ "$1" = "--remove" ]; then
  kextunload -b net.pomppc.POMPPCGPU 2>/dev/null || true
  rm -rf "$EXT/POMPPCGPU.kext" "$RES/GLDriver-POMPPC.bundle" "$RES/GLDriverPOMPPC.bundle"
  touch "$EXT"
  echo "retiré ; redémarrer pour oublier le renderer"
  exit 0
fi

[ -x /usr/bin/gcc-4.0 ] || { echo "gcc-4.0 absent : installer les Xcode Tools du DVD"; exit 1; }

echo "▶ kext"
( cd "$KEXTSRC" && make clean >/dev/null && make )
rm -rf "$EXT/POMPPCGPU.kext"
cp -R "$KEXTSRC/POMPPCGPU.kext" "$EXT/"
chown -R root:wheel "$EXT/POMPPCGPU.kext"
chmod -R 755 "$EXT/POMPPCGPU.kext"
rm -f /System/Library/Extensions.mkext /System/Library/Extensions.kextcache
touch "$EXT"
kextload "$EXT/POMPPCGPU.kext" 2>/dev/null || true

echo "▶ plugin OpenGL"
cp "$KEXTSRC/qgpu_proto.h" "$HERE/qgpu_proto.h"
( cd "$HERE" && make clean >/dev/null && make )
rm -rf "$RES/GLDriver-POMPPC.bundle" "$RES/GLDriverPOMPPC.bundle"
cp -R "$HERE/GLDriver-POMPPC.bundle" "$RES/"
chown -R root:wheel "$RES/GLDriver-POMPPC.bundle"
chmod -R 755 "$RES/GLDriver-POMPPC.bundle"

echo "✔ installé. Vérifier : ioreg -c POMPPCGPU -r ; une application GL doit"
echo "  rapporter GL_RENDERER = « POMPPC qgpu (OpenGL host GPU) »."
