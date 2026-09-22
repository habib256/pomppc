#!/bin/sh
# Diagnostic du chargement du plugin par GLEngine (variantes de GL_RESOURCES).
#
# A, B, C sont des variantes CONNUES POUR ÉCHOUER sur ce 10.4.6 (GL_RESOURCES
# ne marche pas, kCGLBadCodeModule) : elles sont informatives et ne comptent
# pas. D — le plugin posé dans le Resources du système — est le chemin de
# secours réellement utilisé en single-user : celui-là doit rendre par POMPPC
# (GLTEST_REQUIRE, bug hunt T5), et son échec rougit le job (T6).
#
# NETTOYAGE (bug hunt T21) : le job copie un plugin dans le Resources du
# SYSTÈME. S'il mourait avant la ligne de `rm`, un second exemplaire du plugin
# restait là pour toutes les sessions suivantes — deux plugins chargés, et un
# diagnostic faux pour longtemps. Le `trap` le retire quoi qu'il arrive.
set -e
SRC=$PWD/src; OUT=$PWD/out; SDK=/Developer/SDKs/MacOSX10.4u.sdk
SYSRES=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources
ko=0
cleanup() {
  rm -rf $SYSRES/GLDriver-POMPPC.bundle
  rm -rf /tmp/sysonly /tmp/copy
  echo "nettoyage : $SYSRES/GLDriver-POMPPC.bundle retiré ($(ls -d $SYSRES/GLDriver-POMPPC.bundle 2>/dev/null || echo absent))"
}
trap cleanup EXIT HUP INT TERM
cd $SRC/guest/gldriver && make glres > /tmp/pb.txt 2>&1 || tail -5 /tmp/pb.txt
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o gltest gltest.c -framework OpenGL || exit 1
mkdir -p /tmp/sysonly && rm -f /tmp/sysonly/*
for b in $SYSRES/*.bundle; do ln -s $b /tmp/sysonly/; done
echo "== A : GL_RESOURCES = liens vers le système seulement"
GLTEST_NOWS=1 GL_RESOURCES=/tmp/sysonly ./gltest tri 64 64 a.ppm > $OUT/a.txt 2>&1 || true
sed -n '1,6p' $OUT/a.txt
mkdir -p /tmp/copy && rm -rf /tmp/copy/* && cp -R $SYSRES/*.bundle /tmp/copy/
echo "== B : GL_RESOURCES = copie réelle des bundles du système"
GLTEST_NOWS=1 GL_RESOURCES=/tmp/copy ./gltest tri 64 64 a.ppm > $OUT/b.txt 2>&1 || true
sed -n '1,6p' $OUT/b.txt
echo "== C : B + plugin"
cp -R $SRC/guest/gldriver/GLDriver-POMPPC.bundle /tmp/copy/
mkdir -p $OUT/trace
POMPPC_GLTRACE=$OUT/trace GLTEST_NOWS=1 GL_RESOURCES=/tmp/copy ./gltest tri 64 64 a.ppm \
  > $OUT/c.txt 2>&1 || true
sed -n '1,12p' $OUT/c.txt
echo "== D : plugin dans le Resources du système"
cp -R $SRC/guest/gldriver/GLDriver-POMPPC.bundle $SYSRES/
if env POMPPC_GLTRACE=$OUT/trace GLTEST_NOWS=1 GLTEST_REQUIRE=POMPPC \
     ./gltest tri 64 64 a.ppm > $OUT/d.txt 2>&1
then rc=0; else rc=$?; fi
sed -n '1,12p' $OUT/d.txt
echo "D : rc=$rc"
if [ $rc != 0 ]; then
  ko=$((ko + 1))
  echo "  ÉCHEC : D (plugin dans le Resources du système) rc=$rc \
(6 = GL_RENDERER n'est pas POMPPC : GLEngine ne l'a pas chargé)"
fi
head -30 $OUT/trace/trace.txt 2>/dev/null || true
echo "VERDICT : $ko échec(s)"
exit $ko
