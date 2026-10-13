#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# Epreuves en VM "GPU hote bloque" (GL3, KT4, K6) et A6 (L2) : le cote INVITE.
# Le cote hote (proprietes x-test-stall-ms / x-test-refuse-reset du device,
# system_reset, relevés des registres) est tools/guest/epreuve_gpu_bloque.py,
# qui depose dans le dossier du job un fichier `phase` (et `variant`, `kext`).
# docs/epreuve-gpu-bloque.md.
#
#   prep    kext du depot (et celui d'AVANT si src-old/ est la) compiles et
#           ranges dans /pomppc/st/, outils guest/qgpu-test, temoin vert
#   block   (GPU bloque par l'hote) un client met un job en vol, puis
#           kextunload + kextload : le device doit passer "casse" (le kext
#           refuse de demarrer), en ~5 s et pas davantage ; puis l'ecran
#           POMPPCQFB change de mode PENDANT la panne (qfbmode) : sans BQL
#           tenu 2 s par ecriture de registre (KT4)
#   geom    (device casse) meme changement de mode de l'ecran, seul
#   inflight (GPU bloque) un client met un job en vol et le laisse : l'hote
#           enchaine system_reset
#   repair  (GPU debloque) kextload : device repare, qgpu_test vert,
#           premiere creation acceptee
#   a6      kext `kext` (new|old), scenario `variant` :
#             full     file tenue pleine (stall 1,5 s pose par l'hote),
#                      client tue : CLIENT_RESET jamais parti ; un nouveau
#                      client ouvert apres vidange : autre tranche
#             wait     idem, les trois autres tranches tenues : attente
#             late     (stall 0,5 s) CLIENT_RESET accepte mais fini apres
#                      l'echeance du kext ; nouveau client aussitot
#             refuse   CLIENT_RESET refuse (hote), client tue, nouveau client
#             again    (apres refuse, refus leve) un client de plus : la
#                      tranche sale est relancee puis redonnee
#           Verdict : jamais QGPU_ST_LIMIT a la premiere creation.
. ./lib.sh
JOB=$PWD
ST=/pomppc/st
PHASE=$(cat $JOB/phase 2>/dev/null || echo prep)
VARIANT=$(cat $JOB/variant 2>/dev/null || echo full)
KEXT=$(cat $JOB/kext 2>/dev/null || echo new)
ko=0
fail() { ko=$((ko + 1)); echo "  ECHEC : $*"; }
step() { sync; echo "stall: $*" > /dev/console; echo "== $(date +%H:%M:%S) $*"; }
t() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
klog() { dmesg | grep -E "POMPPCGPU|slot " | tail -${1:-15}; }

load_kext() {  # load_kext CHEMIN : decharge puis charge, rend le code de kextload
  if kextstat | grep -q net.pomppc.POMPPCGPU; then
    kextunload -b net.pomppc.POMPPCGPU 2>&1 || true
  fi
  rm -rf /tmp/k.kext && cp -R "$1" /tmp/k.kext
  chown -R root:wheel /tmp/k.kext && chmod -R 755 /tmp/k.kext
  kextload -t /tmp/k.kext 2>&1 | tail -2
}

clients() { ioreg -c POMPPCGPU -w 0 2>/dev/null | sed -n 's/.*"QGPUClients" = \([0-9]*\).*/\1/p' | head -1; }

qfb_load() {
  if ! kextstat | grep -q POMPPCQFB; then
    step "$PHASE : POMPPCQFB charge (aucun mode programme tant que personne n'ouvre l'ecran)"
    rm -rf /tmp/q.kext && cp -R $ST/POMPPCQFB.kext /tmp/q.kext && chown -R root:wheel /tmp/q.kext
    kextload -t /tmp/q.kext 2>&1 | tail -1
  fi
}

# Changement de geometrie de qfb-pci pendant la panne (K6, KT4) : l'ecran est
# ouvert (mode par defaut programme), puis passe au mode 1 en 16 bits.
geom() {
  step "$PHASE : POMPPCQFB ouvert (mode par defaut) puis mode 1 en 16 bits, pendant la panne"
  a=$(t); $ST/qfbmode 0 1 1 > $OUT/qfbmode.txt 2>&1 & q=$!
  i=0; while kill -0 $q 2>/dev/null && [ $i -lt 40 ]; do sleep 1; i=$((i + 1)); done
  kill -9 $q 2>/dev/null; b=$(t)
  cat $OUT/qfbmode.txt
  echo "qfbmode : $a -> $b"
  grep -q "ouverture serveur 0x0 " $OUT/qfbmode.txt || fail "qfbmode n'a pas ouvert l'ecran"
}

case $PHASE in
prep)
  step "prep : kext du depot"
  kext_load || exit 1
  mkdir -p $ST
  rm -rf $ST/POMPPCGPU.kext && cp -R /tmp/POMPPCGPU.kext $ST/
  if [ -d $JOB/src-old/kext/POMPPCGPU ]; then
    step "prep : kext d'AVANT (src-old)"
    ( cd $JOB/src-old/kext/POMPPCGPU && make clean >/dev/null 2>&1; make > $OUT/kext-old-build.txt 2>&1 ) ||
      { tail -15 $OUT/kext-old-build.txt; exit 1; }
    rm -rf $ST/POMPPCGPU-old.kext && cp -R $JOB/src-old/kext/POMPPCGPU/POMPPCGPU.kext $ST/POMPPCGPU-old.kext
  fi
  step "prep : POMPPCQFB (ecran de l'epreuve K6)"
  ( cd $SRC/kext/POMPPCQFB && make > $OUT/qfb-build.txt 2>&1 ) ||
    { tail -10 $OUT/qfb-build.txt; exit 1; }
  rm -rf $ST/POMPPCQFB.kext && cp -R $SRC/kext/POMPPCQFB/POMPPCQFB.kext $ST/
  step "prep : outils"
  ( cd $SRC/guest/qgpu-test && make > $OUT/qgpu-test-build.txt 2>&1 ) ||
    { tail -10 $OUT/qgpu-test-build.txt; exit 1; }
  cp $SRC/guest/qgpu-test/qgpu_test $SRC/guest/qgpu-test/slotpeer \
     $SRC/guest/qgpu-test/qfbmode $ST/
  if $ST/qgpu_test > $OUT/qgpu_test.txt 2>&1; then echo "qgpu_test : vert"; else fail "qgpu_test rc=$?"; tail -5 $OUT/qgpu_test.txt; fi
  ioreg -c POMPPCGPU -w 0 | grep -E '"QGPU(Version|Clients|Caps)"' | sed 's/^[ |]*//'
  ls -la $ST
  ;;

block)
  qfb_load
  if ! kextstat | grep -q net.pomppc.POMPPCGPU; then
    step "block : kext charge (device encore sain : le reset n'est pas un job)"
    load_kext $ST/POMPPCGPU.kext
  fi
  step "block : client avec un job en vol"
  $ST/slotpeer async1 600 > $OUT/peer.txt 2>&1 &
  peer=$!
  sleep 4
  cat $OUT/peer.txt
  step "block : kextunload"
  a=$(t); kextunload -b net.pomppc.POMPPCGPU 2>&1; b=$(t)
  echo "kextunload : $a -> $b"
  step "block : kextload (ecriture de MAGIC, reset borne du device)"
  a=$(t); rm -rf /tmp/k.kext && cp -R $ST/POMPPCGPU.kext /tmp/k.kext && chown -R root:wheel /tmp/k.kext
  kextload -t /tmp/k.kext 2>&1 | tail -2; b=$(t)
  echo "kextload : $a -> $b ($(echo "$b - $a" | bc) s)"
  kill -9 $peer 2>/dev/null; wait $peer 2>/dev/null
  cat $OUT/peer.txt
  n=$(clients)
  echo "QGPUClients apres rechargement : '${n:-absent}'"
  klog 12
  if dmesg | grep -q "without QGPU_CAP_CLIENTS\|client slots, this kext holds"; then
    echo "kext : demarrage refuse (device casse) -- attendu"
  else
    fail "le kext n'a pas refuse de demarrer : device non casse ?"
  fi
  geom
  ;;

geom)
  qfb_load
  geom
  ;;

inflight)
  step "inflight : client avec un job en vol, laisse en vol"
  load_kext $ST/POMPPCGPU.kext
  ( $ST/slotpeer async1 600 > /dev/console 2>&1 & )
  sleep 4
  ;;

repair)
  step "repair : kextload"
  a=$(t); load_kext $ST/POMPPCGPU.kext; b=$(t)
  echo "kextload : $a -> $b"
  n=$(clients); echo "QGPUClients : '${n:-absent}'"
  [ "$n" = 4 ] || fail "kext non demarre apres reparation"
  klog 4
  if $ST/qgpu_test > $OUT/qgpu_test.txt 2>&1; then echo "qgpu_test : vert"; else fail "qgpu_test rc=$?"; tail -8 $OUT/qgpu_test.txt; fi
  $ST/slotpeer create; r=$?
  [ $r = 0 ] || fail "premiere creation : rc=$r"
  ;;

a6)
  K=$ST/POMPPCGPU.kext; [ "$KEXT" = old ] && K=$ST/POMPPCGPU-old.kext
  step "a6 $VARIANT : kext $KEXT"
  if [ "$VARIANT" != again ]; then
    load_kext $K
    n=$(clients); [ "$n" = 4 ] || { fail "kext $KEXT non demarre"; exit $ko; }
  fi
  dmesg > $OUT/dmesg-avant.txt
  holders=""
  if [ "$VARIANT" = again ]; then
    step "a6 again : nouveau client, refus leve"
  else
    step "a6 $VARIANT : client A (contexte cree, file remplie)"
    KEEP=""; case $VARIANT in full|wait) KEEP=keep ;; esac
    $ST/slotpeer fill 600 $KEEP > $OUT/A.txt 2>&1 &
    A=$!
    i=0
    while ! grep -q "asynchrones" $OUT/A.txt && [ $i -lt 60 ]; do sleep 1; i=$((i + 1)); done
    cat $OUT/A.txt
    if [ "$VARIANT" = wait ]; then
      for h in 1 2 3; do
        $ST/slotpeer hold 300 > $OUT/H$h.txt 2>&1 &
        holders="$holders $!"
      done
      sleep 3
      cat $OUT/H1.txt $OUT/H2.txt $OUT/H3.txt | grep ouvert
    fi
    step "a6 $VARIANT : kill -9 A pendant que ses soumissions sont en file"
    a=$(t); kill -9 $A; wait $A 2>/dev/null; b=$(t)
    echo "A tue : $a -> $b"
    sleep 1
    if [ "$VARIANT" = full ] || [ "$VARIANT" = wait ]; then
      echo "(attente de la vidange de la file : 30 s)"; sleep 30
    fi
  fi
  step "a6 $VARIANT : client B, premiere creation"
  a=$(t); $ST/slotpeer create > $OUT/B.txt 2>&1; r=$?; b=$(t)
  cat $OUT/B.txt
  echo "B : rc=$r ($a -> $b)"
  case $r in
    0) echo "B : premiere creation acceptee" ;;
    3) fail "B : QGPU_ST_LIMIT a la premiere creation (objets de A laisses dans la tranche)" ;;
    2) echo "B : ouverture refusee (aucune tranche propre) -- admis, jamais LIMIT" ;;
    *) fail "B : autre refus rc=$r" ;;
  esac
  for h in $holders; do kill $h 2>/dev/null; done
  for h in $holders; do wait $h 2>/dev/null; done
  dmesg > $OUT/dmesg-apres.txt
  echo "--- journal du kext (nouvelles lignes) ---"
  diff $OUT/dmesg-avant.txt $OUT/dmesg-apres.txt | sed -n 's/^> //p' | grep -E "POMPPCGPU|slot " | tail -20
  ;;
*)
  echo "phase inconnue : $PHASE"; exit 2 ;;
esac
echo "stall $PHASE : $ko echec(s)"
exit $ko
