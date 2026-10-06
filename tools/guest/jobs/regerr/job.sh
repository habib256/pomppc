#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# Épreuve de QGPU_CAP_CLIENT_ERRORS (07/10/2026, TODO « QGPU_REG_ERRORS par
# client ») : deux clients, l'un provoque des refus, l'autre ne doit plus
# repasser en synchrone.
#
#   client fautif  : errpeer (guest/qgpu-test), un flux refusé toutes les
#                    $PEER_MS ms dans SA tranche, en tâche de fond ;
#   client sain    : gltest $SCENE (60 images, doorbell asynchrone), avec
#                    POMPPC_GL_STATS : « N sync fallback(s) » au bilan.
#
# Joué deux fois, si le dossier src-old/ est là (plugin d'AVANT, même kext,
# même device) : AVANT, le plugin sain doit compter au moins un repli
# synchrone ; APRÈS, aucun, et au moins un « foreign error move ». Chaque
# plugin est d'abord joué SEUL (0 repli attendu des deux côtés : le repli
# vient bien du voisin).
#
#   single-user (devloop.py start) ; stage.sh regerr DOSSIER, puis copier le
#   plugin d'avant dans DOSSIER/src-old/ (même disposition que src/).
. ./lib.sh
JOB=$PWD
SCENE=${SCENE:-game}
PEER_MS=${PEER_MS:-10}
ko=0
fail() { ko=$((ko + 1)); echo "  ÉCHEC : $*"; }
step() { sync; echo "regerr: $*" > /dev/console; echo "== $*"; }

step "kext"
kext_load || exit 1
ioreg -c POMPPCGPU -w 0 2>/dev/null | grep -E '"QGPU(Version|Caps)"' | sed 's/^[ |]*//' || true

step "qgpu_test + errpeer"
( cd $SRC/guest/qgpu-test && make > $OUT/qgpu-test-build.txt 2>&1 ) ||
  { tail -10 $OUT/qgpu-test-build.txt; exit 1; }
if $SRC/guest/qgpu-test/qgpu_test > $OUT/qgpu_test.txt 2>&1; then rq=0; else rq=$?; fi
grep -E "par tranche|^OK|^ÉCHEC" $OUT/qgpu_test.txt
[ $rq = 0 ] || fail "qgpu_test rc=$rq"
if $SRC/guest/qgpu-test/errpeer 5 0 > $OUT/errpeer-seul.txt 2>&1; then rp=0; else rp=$?; fi
cat $OUT/errpeer-seul.txt
[ $rp = 0 ] || fail "errpeer seul rc=$rp (refus non comptés à sa tranche)"

gltest_build || exit 1
cd $SRC/guest/gltest

# stats LABEL FICHIER : « sync fallback(s) » et « foreign error move(s) » du bilan
stats() {
  fb=$(sed -n 's/.* \([0-9]*\) sync fallback(s).*/\1/p' "$2" | tail -1)
  fo=$(sed -n 's/.* \([0-9]*\) foreign error move(s).*/\1/p' "$2" | tail -1)
  mode=$(sed -n 's/.*submit \(a*sync\): .*/\1/p' "$2" | tail -1)
  echo "$1 : mode final ${mode:-?}, ${fb:-?} repli(s) synchrone(s), ${fo:--} mouvement(s) étranger(s)"
}

# jouer LABEL : gltest seul, puis gltest avec le client fautif
jouer() {
  step "$1 : $SCENE seul"
  if env GLTEST_NOWS=1 GLTEST_REQUIRE=POMPPC POMPPC_GL_STATS=1 ./gltest $SCENE 128 128 \
       $1-seul.ppm > $OUT/$1-seul.txt 2>&1; then r=0; else r=$?; fi
  echo "$1 seul : gltest rc=$r"; [ $r = 0 ] || fail "$1 seul : gltest rc=$r"
  stats "$1 seul" $OUT/$1-seul.txt
  SEUL_FB=$fb
  step "$1 : $SCENE + errpeer"
  $SRC/guest/qgpu-test/errpeer 1000000 $PEER_MS > $OUT/$1-peer.txt 2>&1 &
  peer=$!
  sleep 2
  mkdir -p $OUT/trace-$1
  if env GLTEST_NOWS=1 GLTEST_REQUIRE=POMPPC POMPPC_GL_STATS=1 POMPPC_GLTRACE=$OUT/trace-$1 \
       ./gltest $SCENE 128 128 $1-voisin.ppm > $OUT/$1-voisin.txt 2>&1; then r=0; else r=$?; fi
  kill -TERM $peer 2>/dev/null || true
  wait $peer || true
  cat $OUT/$1-peer.txt
  echo "$1 + errpeer : gltest rc=$r"; [ $r = 0 ] || fail "$1 + errpeer : gltest rc=$r"
  stats "$1 + errpeer" $OUT/$1-voisin.txt
  grep -c "QGPU_REG_ERRORS" $OUT/trace-$1/trace.txt 2>/dev/null |
    sed "s/^/$1 : lignes QGPU_REG_ERRORS dans la trace : /" || true
  grep -m3 "QGPU_REG_ERRORS" $OUT/trace-$1/trace.txt 2>/dev/null || true
}

if [ -d $JOB/src-old/guest/gldriver ]; then
  step "plugin d'AVANT"
  SRC0=$SRC; SRC=$JOB/src-old
  plugin_install || exit 1
  SRC=$SRC0
  jouer avant
  [ "$SEUL_FB" = 0 ] || fail "avant, seul : $SEUL_FB repli(s) sans voisin"
  [ -n "$fb" ] && [ "$fb" -ge 1 ] ||
    echo "  (avant : aucun repli — le défaut ne s'est pas montré sur ce passage)"
fi

step "plugin d'APRÈS"
plugin_install || exit 1
jouer apres
[ "$SEUL_FB" = 0 ] || fail "après, seul : $SEUL_FB repli(s) sans voisin"
[ "$fb" = 0 ] || fail "après + errpeer : $fb repli(s) synchrone(s)"
[ -n "$fo" ] && [ "$fo" -ge 1 ] || fail "après + errpeer : aucun mouvement étranger vu"

echo "regerr : $ko échec(s)"
exit $ko
