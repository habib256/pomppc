#!/bin/sh
# Mesure du téléversement de textures (tâche 2.5) : gltest texup, par format,
# conversion par l'HÔTE (v10, défaut), par l'INVITÉ (POMPPC_GL_TEX3=0), et rendu
# d'Apple seul (POMPPC_GL_DISABLE=1). Suppose le kext chargé et le plugin
# installé (job gpu). TEXUP_SIZE=… pour changer la taille (256 par défaut).
SRC=$PWD/src; SDK=/Developer/SDKs/MacOSX10.4u.sdk
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o gltest gltest.c -framework OpenGL || exit 1
for fm in ${FMTS:-rgba rgb rgb565 bgra}; do
  for mode in hote invite apple; do
    case $mode in
      hote)   e="" ;;
      invite) e="POMPPC_GL_TEX3=0" ;;
      apple)  e="POMPPC_GL_DISABLE=1" ;;
    esac
    r=$(env GLTEST_NOWS=1 TEXUP_FMT=$fm $e ./gltest texup 256 256 t.ppm 2>&1)
    printf "%-7s %-7s %s | %s\n" $fm $mode "$(echo "$r" | grep '^texup' | sed 's/.*: //')" \
      "$(echo "$r" | grep -E 'dernière image' | sed 's/^ *//')"
  done
done
