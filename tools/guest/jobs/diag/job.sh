#!/bin/sh
# Diagnostic du chargement du plugin par GLEngine (variantes de GL_RESOURCES).
SRC=$PWD/src; OUT=$PWD/out; SDK=/Developer/SDKs/MacOSX10.4u.sdk
SYSRES=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources
cd $SRC/guest/gldriver && make glres > /tmp/pb.txt 2>&1 || tail -5 /tmp/pb.txt
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o gltest gltest.c -framework OpenGL || exit 1
mkdir -p /tmp/sysonly && rm -f /tmp/sysonly/* && for b in $SYSRES/*.bundle; do ln -s $b /tmp/sysonly/; done
echo "== A : GL_RESOURCES = liens vers le système seulement"
GLTEST_NOWS=1 GL_RESOURCES=/tmp/sysonly ./gltest tri 64 64 a.ppm 2>&1 | sed -n '1,6p'
mkdir -p /tmp/copy && rm -rf /tmp/copy/* && cp -R $SYSRES/*.bundle /tmp/copy/
echo "== B : GL_RESOURCES = copie réelle des bundles du système"
GLTEST_NOWS=1 GL_RESOURCES=/tmp/copy ./gltest tri 64 64 a.ppm 2>&1 | sed -n '1,6p'
echo "== C : B + plugin"
cp -R $SRC/guest/gldriver/GLDriver-POMPPC.bundle /tmp/copy/
mkdir -p $OUT/trace
POMPPC_GLTRACE=$OUT/trace GLTEST_NOWS=1 GL_RESOURCES=/tmp/copy ./gltest tri 64 64 a.ppm 2>&1 | sed -n '1,12p'
echo "== D : plugin dans le Resources du système"
cp -R $SRC/guest/gldriver/GLDriver-POMPPC.bundle $SYSRES/
POMPPC_GLTRACE=$OUT/trace GLTEST_NOWS=1 ./gltest tri 64 64 a.ppm 2>&1 | sed -n '1,12p'
rm -rf $SYSRES/GLDriver-POMPPC.bundle
head -30 $OUT/trace/trace.txt 2>/dev/null
