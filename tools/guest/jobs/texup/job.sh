#!/bin/sh
# Mesure du téléversement de textures (tâche 2.5) : gltest texup, par format,
# conversion par l'HÔTE (v10, défaut), par l'INVITÉ (POMPPC_GL_TEX3=0), et rendu
# d'Apple seul (POMPPC_GL_DISABLE=1). Suppose le kext chargé et le plugin
# installé (job gpu). TEXUP_SIZE=… pour changer la taille (256 par défaut).
#
# Rend le nombre d'échecs (bug hunt T6) : le rc du job était celui du dernier
# `printf`. Les deux modes accélérés exigent GL_RENDERER = POMPPC (T5) — une
# mesure faite par le rendu d'Apple n'est pas une mesure de notre chaîne, et
# c'est exactement ce qui arrive quand le plugin n'est pas chargé.
set -e
SRC=$PWD/src; OUT=$PWD/out; SDK=/Developer/SDKs/MacOSX10.4u.sdk
cd $SRC/guest/gltest
/usr/bin/gcc-4.0 -arch ppc -O1 -isysroot $SDK -o gltest gltest.c -framework OpenGL || exit 1
ko=0
for fm in ${FMTS:-rgba rgb rgb565 bgra}; do
  for mode in hote invite apple; do
    case $mode in
      hote)   e="GLTEST_REQUIRE=POMPPC" ;;
      invite) e="GLTEST_REQUIRE=POMPPC POMPPC_GL_TEX3=0" ;;
      apple)  e="POMPPC_GL_DISABLE=1" ;;
    esac
    if env GLTEST_NOWS=1 TEXUP_FMT=$fm $e ./gltest texup 256 256 t-$fm-$mode.ppm \
         > $OUT/texup-$fm-$mode.txt 2>&1
    then rc=0; else rc=$?; fi
    printf "%-7s %-7s rc=%d %s | %s\n" $fm $mode $rc \
      "$(grep '^texup' $OUT/texup-$fm-$mode.txt | sed 's/.*: //')" \
      "$(grep -E 'dernière image' $OUT/texup-$fm-$mode.txt | sed 's/^ *//')"
    if [ $rc != 0 ]; then
      if [ $mode = apple ]; then echo "   ⚠ échec sous Apple (référence)"
      else ko=$((ko + 1)); echo "   ÉCHEC : texup $fm / $mode rc=$rc"; fi
    fi
  done
done
[ $ko -lt 250 ] || ko=250
echo "VERDICT : $ko échec(s)"
exit $ko
