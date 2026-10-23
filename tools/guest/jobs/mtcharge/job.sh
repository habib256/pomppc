#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# mtcharge - longue charge SMP de la VM de dev, single-user (docs/ppc32-mttcg.md par.10) :
# pendant DUREE s, en meme temps,
#   - deux fils de compilation (gcc -O2 de regbench.c et vfptest.c, en boucle), chaque
#     objet compare au premier produit (md5) : un objet different = erreur ;
#   - atomtest en boucle (lwarx/stwcx., verrou, voisins ; ATOMN increments par fil) ;
#   - la scene gltest d3inter (plugin + GLEngine + kext), GLFR images toutes les GLPAUSE s,
#     md5 de l'image compare a la premiere.
# Le pilote (tools/tcg/ppc32mt-bench.sh mtcharge) joint regbench.c, vfptest.c, atomtest.c.
#   DUREE=3600 ATOMN=1000000 GLFR=60 GLPAUSE=20
# Bilan : lignes « charge <fil> tours=<n> erreurs=<e> », puis « charge bilan OK|ECHEC ».
. ./lib.sh
[ -f ./env.sh ] && . ./env.sh
DUREE=${DUREE:-3600}; ATOMN=${ATOMN:-1000000}; GLFR=${GLFR:-60}; GLPAUSE=${GLPAUSE:-20}
echo "ncpu $(sysctl -n hw.ncpu) ; $(uptime)"
hostinfo | grep -i processors
gcc -O2 -o atomtest atomtest.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
echo "== kext"; kext_load
echo "== plugin"; plugin_install
gltest_build
cp $SRC/guest/gltest/gltest /tmp/gltest-mc
FIN=$(( $(date +%s) + DUREE ))
W=/tmp/mtcharge; rm -rf $W; mkdir -p $W

compile() { # compile <fil> <source>
  n=0; e=0; ref=""
  while [ $(date +%s) -lt $FIN ]; do
    gcc -O2 -faltivec -c -o $W/$1.o $2 2>> $W/$1.err || e=$((e + 1))
    m=$(md5 -q $W/$1.o 2>/dev/null)
    [ -z "$ref" ] && ref=$m
    [ "$m" = "$ref" ] || { e=$((e + 1)); echo "tour $n : md5 $m != $ref" >> $W/$1.err; }
    n=$((n + 1))
  done
  echo "charge $1 tours=$n erreurs=$e" > $W/$1.bilan
}
atom() {
  n=0; e=0
  while [ $(date +%s) -lt $FIN ]; do
    ./atomtest $ATOMN 2 > $W/atom-last.txt 2>&1
    grep -q 'atom bilan OK' $W/atom-last.txt || { e=$((e + 1)); cat $W/atom-last.txt >> $W/atom.err; }
    n=$((n + 1))
  done
  echo "charge atom tours=$n erreurs=$e" > $W/atom.bilan
}
gl() {
  n=0; e=0; ref=""
  while [ $(date +%s) -lt $FIN ]; do
    rm -f $W/gl.ppm
    env GLTEST_NOWS=1 GLTEST_REQUIRE=POMPPC GLTEST_D3FR=$GLFR /tmp/gltest-mc d3inter 256 256 $W/gl.ppm > $W/gl-last.txt 2>&1 \
      || { e=$((e + 1)); cat $W/gl-last.txt >> $W/gl.err; }
    m=$(md5 -q $W/gl.ppm 2>/dev/null || echo absent)
    [ -z "$ref" ] && ref=$m
    [ "$m" = "$ref" ] || { e=$((e + 1)); echo "tour $n : md5 $m != $ref" >> $W/gl.err; }
    n=$((n + 1))
    sleep $GLPAUSE
  done
  echo "charge gl tours=$n erreurs=$e md5=$ref" > $W/gl.bilan
}
compile gcc1 regbench.c &
compile gcc2 vfptest.c &
atom &
gl &
# releve toutes les 10 min : l'invite avance-t-il ?
while [ $(date +%s) -lt $FIN ]; do
  sleep 600
  echo "releve $(date '+%H:%M:%S') $(uptime | sed 's/.*load/load/')"
done
wait
cat $W/*.bilan
cp $W/*.err $OUT/ 2>/dev/null
if grep -q 'erreurs=[1-9]' $W/*.bilan; then echo "charge bilan ECHEC"; exit 1; fi
echo "charge bilan OK"
exit 0
