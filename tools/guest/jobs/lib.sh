# lib.sh — étapes communes des jobs GPU (sourcé ; stage.sh le joint au job).
SRC=$PWD/src; OUT=$PWD/out; SDK=/Developer/SDKs/MacOSX10.4u.sdk
RES=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources

# kext compilé puis chargé depuis /tmp (jamais installé : un kext qui panique
# se répare par un redémarrage de la VM). Déchargé d'abord s'il l'était.
kext_load() {
  ( cd $SRC/kext/POMPPCGPU && make clean >/dev/null 2>&1; make > $OUT/kext-build.txt 2>&1 ) ||
    { tail -15 $OUT/kext-build.txt; return 1; }
  kextstat | grep -q net.pomppc.POMPPCGPU && kextunload -b net.pomppc.POMPPCGPU 2>&1
  rm -rf /tmp/POMPPCGPU.kext && cp -R $SRC/kext/POMPPCGPU/POMPPCGPU.kext /tmp/
  chown -R root:wheel /tmp/POMPPCGPU.kext && chmod -R 755 /tmp/POMPPCGPU.kext
  kextload -t /tmp/POMPPCGPU.kext 2>&1 | tail -1
}

# plugin compilé et installé dans OpenGL.framework (GL_RESOURCES ne marche pas
# sur ce 10.4.6 : docs/gpu-3d-tiger.md §5.1)
plugin_install() {
  ( cd $SRC/guest/gldriver && make > $OUT/plugin-build.txt 2>&1 ) ||
    { tail -15 $OUT/plugin-build.txt; return 1; }
  rm -rf $RES/GLDriver-POMPPC.bundle && cp -R $SRC/guest/gldriver/GLDriver-POMPPC.bundle $RES/
  chown -R root:wheel $RES/GLDriver-POMPPC.bundle && chmod -R 755 $RES/GLDriver-POMPPC.bundle
}

gltest_build() {
  ( cd $SRC/guest/gltest &&
    /usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o gltest gltest.c -framework OpenGL \
      > $OUT/gltest-build.txt 2>&1 ) || { tail -15 $OUT/gltest-build.txt; return 1; }
}
