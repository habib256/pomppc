#!/bin/sh
# Binaires prêts à installer, pour un Tiger SANS Xcode Tools : kext, plugin
# OpenGL, gltest et glwin, compilés ici puis rapportés dans out/prebuilt.
# scripts/make_kext_iso.sh les grave sur le CD (disks/prebuilt), et
# guest/gldriver/install.sh les installe quand gcc-4.0 manque.
. ./lib.sh
P=$OUT/prebuilt; mkdir -p $P
( cd $SRC/kext/POMPPCGPU && make clean >/dev/null 2>&1; make > $OUT/kext-build.txt 2>&1 ) ||
  { tail -15 $OUT/kext-build.txt; exit 1; }
( cd $SRC/guest/gldriver && make clean >/dev/null 2>&1; make > $OUT/plugin-build.txt 2>&1 ) ||
  { tail -15 $OUT/plugin-build.txt; exit 1; }
gltest_build || exit 1
( cd $SRC/guest/gltest &&
  /usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o glwin glwin.c -framework GLUT -framework OpenGL ) || exit 1
cp -R $SRC/kext/POMPPCGPU/POMPPCGPU.kext $SRC/guest/gldriver/GLDriver-POMPPC.bundle $P/
cp $SRC/guest/gltest/gltest $SRC/guest/gltest/glwin $P/
ls -lR $P | head -30
