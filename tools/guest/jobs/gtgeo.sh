#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# gtgeo.sh <label> — scènes gltest de géométrie (A4, volet géométrie,
# docs/protocole-v23-geometrie.md) jouées dans l'invité, plugin installé, dans
# cinq modes : ref (voies d'avant), sane (POMPPC_GL_RAWSANE=1), natshm
# (POMPPC_GL_NATSHM=1), tout (les deux), apple (POMPPC_GL_DISABLE=1, la
# référence). Pour chaque scène et mode : verdict de gltest, md5 de l'image,
# lignes GEOMHOST et replis de la note. Binaire ~/pomppc-build/guest/gltest/gltest
# (gltest.c du dépôt, gcc-4.0). Sortie dans ~/gtgeo-<label>/.
L=${1:-x}; O=~/gtgeo-$L; rm -rf $O; mkdir -p $O
cd ~/pomppc-build/guest/gltest || exit 1
B=./gltest
SC=${SCENES:-"tri varray varrayvbo mixte dlist bigstrip fusion lit texgen clip fogz game vbocolor rawprim matbegin prims gouraud polymode stipple arbvp arbvp0 arbvp0vbo arbvpvar arbvp0cmr glsl glslvs glsldp"}
for s in $SC; do
  for m in ref sane natshm tout apple; do
    case $m in
      ref) e="GLTEST_REQUIRE=POMPPC" ;;
      sane) e="GLTEST_REQUIRE=POMPPC POMPPC_GL_RAWSANE=1" ;;
      natshm) e="GLTEST_REQUIRE=POMPPC POMPPC_GL_NATSHM=1" ;;
      tout) e="GLTEST_REQUIRE=POMPPC POMPPC_GL_RAWSANE=1 POMPPC_GL_NATSHM=1" ;;
      apple) e="POMPPC_GL_DISABLE=1" ;;
    esac
    rm -f gltest.ppm
    # shellcheck disable=SC2086
    env $e POMPPC_GL_NOTE=$O/$s-$m.note POMPPC_GL_STATS=$O/$s-$m.stats \
        $B $s 256 256 > $O/$s-$m.log 2>&1
    echo "rc $?" >> $O/$s-$m.log
    [ -f gltest.ppm ] && { md5 -q gltest.ppm > $O/$s-$m.md5; cp gltest.ppm $O/$s-$m.ppm; }
  done
done
cd $O
for s in $SC; do
  line="$s:"
  for m in ref sane natshm tout apple; do
    v=$(grep -E '^(OK|ÉCHEC|ECHEC)' $s-$m.log | head -1 | cut -c1-24)
    line="$line  $m[$(grep '^rc' $s-$m.log | cut -c4-) $(cut -c1-8 $s-$m.md5 2>/dev/null) $v]"
  done
  echo "$line"
done
echo "-- GEOMHOST / replis"
grep -h 'GEOMHOST\|geom-host\|fallback ' *.note *.stats 2>/dev/null | sort | uniq -c | sort -rn | head -40
