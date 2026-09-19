# lib.sh — étapes communes des jobs GPU (sourcé ; stage.sh le joint au job).
SRC=$PWD/src; OUT=$PWD/out; SDK=/Developer/SDKs/MacOSX10.4u.sdk
RES=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources
EXT=/System/Library/Extensions

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

# plugin compilé et installé dans /System/Library/Extensions : GLEngine l'y
# charge par l'IOGLBundleName de l'accélérateur que publie le kext (tâche 4.2,
# docs/re/accelerateur-iokit.md). (GL_RESOURCES ne marche pas sur ce 10.4.6 :
# gpu-3d-tiger.md §5.1.)
plugin_install() {
  ( cd $SRC/guest/gldriver && make > $OUT/plugin-build.txt 2>&1 ) ||
    { tail -15 $OUT/plugin-build.txt; return 1; }
  rm -rf $EXT/GLDriver-POMPPC.bundle
  cp -R $SRC/guest/gldriver/GLDriver-POMPPC.bundle $EXT/
  chown -R root:wheel $EXT/GLDriver-POMPPC.bundle && chmod -R 755 $EXT/GLDriver-POMPPC.bundle
  plugin_layout
}

# Nombre de framebuffers publiés (IONDRVFramebuffer, POMPPCQFB…). L'ioreg de
# Tiger n'a ni -r ni -d, et -l y échoue en entier (« can't obtain
# properties ») : on compte les nœuds de l'arbre.
fb_count() {
  ioreg -w 0 2>/dev/null | grep -c -E '<class [A-Za-z0-9_]*(Framebuffer|QFB),'
}

# En single-user il n'y a AUCUN framebuffer (IONDRVSupport n'est chargé que par
# kextd, en multi-utilisateur) : aucun écran ne désigne l'accélérateur, CGL n'en
# trouve pas, et GLEngine ne chargerait pas le plugin. La boucle de dev en pose
# alors aussi une copie « à plat » dans Resources, comme avant la tâche 4.2 —
# GLEngine y charge <nom>.bundle/<nom>. En mode bureau cette copie est retirée
# (le plugin garde de toute façon un second exemplaire inactif).
plugin_layout() {
  rm -rf $RES/GLDriver-POMPPC.bundle
  if [ "$(fb_count)" = 0 ]; then
    mkdir -p $RES/GLDriver-POMPPC.bundle
    cp $EXT/GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC $RES/GLDriver-POMPPC.bundle/
    chown -R root:wheel $RES/GLDriver-POMPPC.bundle && chmod -R 755 $RES/GLDriver-POMPPC.bundle
    echo "plugin : pas de framebuffer (single-user), copie aussi dans Resources"
  fi
}

gltest_build() {
  ( cd $SRC/guest/gltest &&
    /usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o gltest gltest.c -framework OpenGL \
      > $OUT/gltest-build.txt 2>&1 ) || { tail -15 $OUT/gltest-build.txt; return 1; }
}
