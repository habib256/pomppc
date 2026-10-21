#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# plugmd5 — les scènes gltest jouées sous le plugin dans plusieurs MODES (jeux
# de variables POMPPC_GL_*), images comparées À L'OCTET au premier mode (20/10/2026,
# leviers du plugin pour DOOM 3 sur le PC : docs/d3-plugin-x86.md §9).
#
#   MODES="ref: halves:POMPPC_GL_X=1" : liste « nom:VAR=v,VAR=v » (le premier fait foi)
#   SCENES="tri tex" : restreindre ; NOTE=1 : garder la note de chaque scène
#
# Rend le nombre d'écarts : une scène dont le md5 diffère du premier mode, ou
# dont le code de sortie diffère, ou qui n'est pas rendue par POMPPC.
set -e
. ./lib.sh
# variables posées par l'hôte au moment de préparer le job (stage.sh puis env.sh)
[ -f ./env.sh ] && . ./env.sh
SCENES=${SCENES:-"tri gouraud depth fill prims tex texfmt texpack texpersp comb units mix state
  stencil depthrt varray varrayvbo game lit texgen clip fogz bigstrip dlist mixte fusion blendc logicop
  polymode stipple occl caps entry v15 tex3d texlod sepspec cube tex13 tex14 tcprobe gl15 texcache
  alpharep texcross readpack drawpack texdelmid vbocolor rawprim offset forkdraw vpimm pixstore
  matbegin matsonde arbvp arbfp arbvp0 arbvp0vbo arbvpvar glsl glslvs glsldp d3idx d3inter rpevict"}
MODES=${MODES:-"ref:"}
echo "== kext"; kext_load
echo "== plugin"; plugin_install
ls -l $EXT/GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC | awk '{print "plugin", $5, "octets"}'
echo "== gltest"; gltest_build
cd $SRC/guest/gltest
scene_env() { case $1 in stencil|tex14) SENV=GLTEST_STENCIL=1 ;; *) SENV=GLTEST_NOWS=1 ;; esac; }
ko=0; n=0
for s in $SCENES; do
  scene_env $s
  ref=""; refrc=""; line="$s"
  for m in $MODES; do
    nom=${m%%:*}; vars=$(echo "${m#*:}" | tr ',' ' ')
    rm -f img.ppm
    # shellcheck disable=SC2086
    if env GLTEST_NOWS=1 $SENV GLTEST_REQUIRE=POMPPC $vars POMPPC_GL_NOTE=$OUT/$s-$nom.note \
         ./gltest $s 256 256 img.ppm > $OUT/$s-$nom.txt 2>&1
    then rc=0; else rc=$?; fi
    h=$(md5 -q img.ppm 2>/dev/null || echo absent)
    line="$line  $nom[rc=$rc $(echo $h | cut -c1-8)]"
    if [ -z "$refrc" ]; then ref=$h; refrc=$rc
    elif [ "$h" != "$ref" ] || [ "$rc" != "$refrc" ]; then
      ko=$((ko + 1)); line="$line ÉCART"
    fi
    [ $rc != 6 ] || { ko=$((ko + 1)); line="$line PAS-POMPPC"; }
    [ -n "$NOTE" ] || rm -f $OUT/$s-$nom.note
  done
  n=$((n + 1))
  echo "$line"
done
# le plugin compilé, rapatrié (à installer tel quel dans une autre VM)
( cd $EXT && tar cf $OUT/GLDriver-POMPPC.bundle.tar GLDriver-POMPPC.bundle ) || true
[ $ko -lt 250 ] || ko=250
echo "VERDICT : $n scènes, $ko écart(s)"
exit $ko
