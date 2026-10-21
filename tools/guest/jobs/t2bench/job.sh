#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# t2bench — banc du traducteur de second niveau (docs/plan-traducteur-rapide.md §8-9),
# VM de dev en single-user : la scène gltest d3inter (la boucle ARB2 d'idTech 4 réduite :
# plugin, GLEngine, kext — le tiers « plugin » d'une image de DOOM 3) jouée FR images,
# REPS passes chronométrées ; md5 de l'image (doit être identique d'un réglage à l'autre).
#   FR=150 REPS=3 BUILD=1 (kext, plugin et gltest d'abord ; 0 : déjà faits dans ce démarrage)
. ./lib.sh
[ -f ./env.sh ] && . ./env.sh
FR=${FR:-150}; REPS=${REPS:-3}
if [ "${BUILD:-1}" = 1 ] || [ ! -x /tmp/gltest-t2 ]; then
  echo "== kext"; kext_load
  echo "== plugin"; plugin_install
  gltest_build
  cp $SRC/guest/gltest/gltest /tmp/gltest-t2
fi
t() { perl -MTime::HiRes=time -e 'printf "%.3f", time'; }
cd /tmp
# chauffe (une passe courte, non comptée)
env GLTEST_NOWS=1 GLTEST_REQUIRE=POMPPC GLTEST_D3FR=20 /tmp/gltest-t2 d3inter 256 256 /tmp/t2.ppm > $OUT/warm.txt 2>&1
k=0
while [ $k -lt $REPS ]; do
  k=$((k + 1))
  rm -f /tmp/t2.ppm
  t0=$(t)
  env GLTEST_NOWS=1 GLTEST_REQUIRE=POMPPC GLTEST_D3FR=$FR /tmp/gltest-t2 d3inter 256 256 /tmp/t2.ppm > $OUT/out-$k.txt 2>&1
  rc=$?
  t1=$(t)
  echo "passe $k rc=$rc $(perl -e "printf '%.3f', $t1 - $t0") s md5 $(md5 -q /tmp/t2.ppm 2>/dev/null || echo absent)"
done
exit 0
