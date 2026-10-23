#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# hostcall — phase 0 du plugin hôte (docs/plan-plugin-hote.md §3 et §5) :
# mesures de la page d'appel synchrone. Demande une VM de dev lancée avec
# QEMU_EXTRA="-global qgpu-pci.x-hostcall=on" et un QEMU qui a la propriété.
#
#   1. kext chargé (il projette BAR2), micro-banc guest/qgpu-test/hcbench :
#      appel vide, fonction invitée, kext, sonnette, LIRE_VA contre memcpy,
#      replis, 256 Kio comparés ; puis trois ouvriers `hcbench ctx` en même
#      temps (même adresse virtuelle, contenus différents).
#   2. plugin installé, gltest compilé, scène SCENE (défaut d3inter) :
#      - sans variable, et POMPPC_GL_HOSTCALL=0 : images de référence ;
#      - HOSTCALL=2 : comparaisons invitées et natives sur l'état réel,
#        contrôle de lecture (GLEngine 5 pages et PCtx, octet à octet) —
#        ligne HCALL de la note ; l'image doit rester celle de la référence ;
#      - HOSTCALL=1 : les temps d'un appel isolé (froid), deux passes ;
#      - HOSTCALL=1 REPS=$REPS : les temps répétés (chaud), deux passes.
#   PHASE=bench|scene|all (défaut all) ; SCENE=d3inter ; REPS=20 ; CTXN=3000
set -e
. ./lib.sh
[ -f ./env.sh ] && . ./env.sh
PHASE=${PHASE:-all}; SCENE=${SCENE:-d3inter}; REPS=${REPS:-20}; CTXN=${CTXN:-3000}
SIZE=${SIZE:-"256 256"}
ko=0
echo "== kext"; kext_load
ioreg -c POMPPCGPU -w 0 | grep -E 'QGPUCaps"|QGPUCallPage' || true
if [ "$PHASE" != scene ]; then
  echo "== hcbench"
  ( cd $SRC/guest/qgpu-test && make hcbench > $OUT/hcbench-build.txt 2>&1 ) ||
    { tail -15 $OUT/hcbench-build.txt; exit 1; }
  HB=$SRC/guest/qgpu-test/hcbench
  otool -tv $HB > $OUT/hcbench-otool.txt 2>/dev/null || true
  if $HB bench > $OUT/bench.txt 2>&1; then rc=0; else rc=$?; fi
  cat $OUT/bench.txt; echo "bench rc=$rc"; [ $rc = 0 ] || ko=$((ko + 1))
  echo "== changements de processus (3 ouvriers, $CTXN tours)"
  $HB ctx 1 $CTXN > $OUT/ctx1.txt 2>&1 & p1=$!
  $HB ctx 2 $CTXN > $OUT/ctx2.txt 2>&1 & p2=$!
  $HB ctx 3 $CTXN > $OUT/ctx3.txt 2>&1 & p3=$!
  for p in $p1 $p2 $p3; do
    if wait $p; then :; else ko=$((ko + 1)); fi
  done
  cat $OUT/ctx1.txt $OUT/ctx2.txt $OUT/ctx3.txt
fi
if [ "$PHASE" != bench ]; then
  echo "== plugin"; plugin_install
  gltest_build
  cp $SRC/guest/gltest/gltest /tmp/gltest-hc
  nm -n $EXT/GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC > $OUT/nm.txt
  cd /tmp
  run() {   # run NOM VARIABLES…
    name=$1; shift
    rm -f /tmp/hc.ppm
    if env GLTEST_NOWS=1 GLTEST_REQUIRE=POMPPC "$@" POMPPC_GL_NOTE=$OUT/note-$name.txt \
         /tmp/gltest-hc $SCENE $SIZE /tmp/hc.ppm > $OUT/out-$name.txt 2>&1
    then r=0; else r=$?; fi
    m=$(md5 -q /tmp/hc.ppm 2>/dev/null || echo absent)
    echo "$name rc=$r md5 $m"
    eval "md5_$name=$m"
    grep -h HCALL $OUT/note-$name.txt 2>/dev/null || true
  }
  run def
  run off POMPPC_GL_HOSTCALL=0
  run chk POMPPC_GL_HOSTCALL=2
  run s1 POMPPC_GL_HOSTCALL=1
  run s2 POMPPC_GL_HOSTCALL=1
  run t1 POMPPC_GL_HOSTCALL=1 POMPPC_GL_HOSTCALL_REPS=$REPS
  run t2 POMPPC_GL_HOSTCALL=1 POMPPC_GL_HOSTCALL_REPS=$REPS
  for v in off chk s1 s2 t1 t2; do
    eval "m=\$md5_$v"
    if [ "$m" != "$md5_def" ]; then echo "  ÉCHEC : image $v ≠ def"; ko=$((ko + 1)); fi
  done
  if grep -h "HCALL" $OUT/note-chk.txt | grep -q -E "écart,|lecture fausse"; then
    echo "  ÉCHEC : écarts dans la note chk"; ko=$((ko + 1))
  fi
  if ! grep -q "HCALL .*hostcall 2" $OUT/note-chk.txt; then
    echo "  ÉCHEC : pas de ligne HCALL sous HOSTCALL=2"; ko=$((ko + 1))
  fi
fi
echo "ko=$ko"
exit $ko
