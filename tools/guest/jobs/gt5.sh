#!/bin/sh
# gt5.sh <label> [VAR=valeur...] — scènes gltest des lots du verdict unique (lots 1-4
# et mémoire des unités), jouées dans l'invité avec le plugin installé :
# verdicts, empreinte md5 de l'image, lignes VERDICT / TEXMEMO / STATE des notes.
# Suite de ~/pomppc-mesures/gt3.sh (lot 3), avec les scènes du lot 4 (état) et
# r4 relevé R4 + R5. Binaire ~/pomppc-build/guest/gltest/gltest-r5 (gltest.c du
# dépôt, compilé par gcc-4.0). Sortie dans ~/gt-<label>/.
L=$1; shift; O=~/gt-$L; rm -rf $O; mkdir -p $O
cd ~/pomppc-build/guest/gltest || exit 1
B=./gltest-r5
for s in texup texcache texdelmid cube tex3d arbvp arbfp varrayvbo game vbocolor arbvp0vbo tri \
         state clip fogz blendc stencil; do
  rm -f gltest.ppm
  env "$@" POMPPC_GL_NOTE=$O/$s.note $B $s > $O/$s.log 2>&1; echo "rc $?" >> $O/$s.log
  [ -f gltest.ppm ] && md5 -q gltest.ppm > $O/$s.md5
done
for s in tex13 tex14 gl15 texlod mixte dlist lit texgen; do
  rm -f gltest.ppm
  env "$@" POMPPC_GL_NOTE=$O/$s.note $B $s 256 256 > $O/$s.log 2>&1; echo "rc $?" >> $O/$s.log
  [ -f gltest.ppm ] && md5 -q gltest.ppm > $O/$s.md5
done
rm -f gltest.ppm
env "$@" POMPPC_GL_NOTE=$O/r4.note $B r4 > $O/r4.log 2>&1; echo "rc $?" >> $O/r4.log
[ -f gltest.ppm ] && md5 -q gltest.ppm > $O/r4.md5
cd $O; for f in *.log; do s=${f%.log}; r=$(grep -E '^(OK|ÉCHEC|ECHEC|FAIL)|échec|rc ' $f | grep -v img/s | tr '\n' ' '); echo "$s: $r $(cat $s.md5 2>/dev/null)"; done
echo "-- VERDICT"; grep -h 'VERDICT total\|VERDICT écart\|TEXMEMO écart\|STATE écart' *.note
