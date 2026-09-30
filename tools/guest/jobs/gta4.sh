#!/bin/sh
# GPL3 - Copyleft VERHILLE Arnaud
# gta4.sh <label> [VAR=valeur...] — scènes gltest de l'état (A4, bloc d'état :
# docs/protocole-v23-etat.md), jouées dans l'invité avec le plugin installé :
# verdict de chaque scène, empreinte md5 de l'image, lignes STATE / STATEBLK /
# VERDICT des notes. Même forme que gt5.sh (lots du verdict unique), avec les
# scènes qui posent de l'état que STATE_BLOCK traduit (ciseaux, stencil,
# brouillard, mélange, opération logique, modes et pointillés, décalage,
# points, éclairage, 1.4) et celles de GLSL. Binaire
# ~/pomppc-build/guest/gltest/gltest (gltest.c du dépôt, gcc-4.0). Sortie
# dans ~/gt-<label>/. À jouer par exemple :
#   sh gta4.sh ref POMPPC_GL_STATEBLK=0
#   sh gta4.sh blk POMPPC_GL_STATEBLK=1
#   sh gta4.sh chk POMPPC_GL_STATEBLK=1 POMPPC_GL_STATECHECK=1 POMPPC_GL_VERDICTCHECK=1
# puis comparer les résumés (même verdict, même md5 : identiques à l'octet).
L=$1; shift; O=~/gt-$L; rm -rf $O; mkdir -p $O
cd ~/pomppc-build/guest/gltest || exit 1
B=./gltest
for s in tri state clip fogz blendc stencil logicop polymode stipple sepspec offset depth \
         alpharep gouraud fill combine3 comb game arbvp arbfp varrayvbo vbocolor arbvp0vbo \
         cube texup texcache r4 prims rawprim mix glsl glslsmp glslfs glslvs; do
  rm -f gltest.ppm
  env "$@" POMPPC_GL_NOTE=$O/$s.note $B $s > $O/$s.log 2>&1; echo "rc $?" >> $O/$s.log
  [ -f gltest.ppm ] && md5 -q gltest.ppm > $O/$s.md5
done
for s in tex13 tex14 gl15 texlod mixte dlist lit texgen; do
  rm -f gltest.ppm
  env "$@" POMPPC_GL_NOTE=$O/$s.note $B $s 256 256 > $O/$s.log 2>&1; echo "rc $?" >> $O/$s.log
  [ -f gltest.ppm ] && md5 -q gltest.ppm > $O/$s.md5
done
cd $O; for f in *.log; do s=${f%.log}; r=$(grep -E '^(OK|ÉCHEC|ECHEC|FAIL)|échec|rc ' $f | grep -v img/s | tr '\n' ' '); echo "$s: $r $(cat $s.md5 2>/dev/null)"; done
echo "-- notes"; grep -h 'stateblk=\|STATEBLK\|VERDICT total\|VERDICT écart\|TEXMEMO écart\|STATE écart' *.note | sort | uniq -c | sort -rn | head -20
