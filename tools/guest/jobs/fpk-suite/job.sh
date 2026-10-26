#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# fpk-suite - les preuves invitées de tcg/0045-0047, 0051 et 0052 en un démarrage, pour
# un hôte sans qemu-ppc linux-user (le M4 : docs/parite-m4-2510.md §5). Le pendant des
# bancs linux-user du PC (docs/tcg-g4.md §37.3, §39.2, §40.3, §41.3) sur le vrai émetteur
# en mode système. Chaque mode écrit sa ligne d'empreinte : elles doivent être identiques
# d'un jeu de propriétés à l'autre (référence, leviers, leviers sous les vérificateurs).
#   cp tools/guest/jobs/fpk-suite/job.sh tools/guest/jobs/fptest/fptest.c \
#      tools/guest/jobs/tlbbtest/tlbbtest.c /tmp/j && devloop.py run /tmp/j
# env.sh a cote (facultatif) : NK=programmes du mode k (défaut 4096), LFIN=borne du
# mode l en hexadécimal (défaut 100000000 : les 2^32 mots), N=tours de tlbbtest.
[ -f ./env.sh ] && . ./env.sh
OUT=$PWD/out
gcc -O2 -Wall -o fptest fptest.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
gcc -O2 -Wall -o tlbbtest tlbbtest.c >> $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
rc=0
run() { # run <fichier> <arguments de fptest…>
  f=$1; shift
  echo "== fptest $*"
  ./fptest "$@" > $OUT/$f.txt 2> $OUT/$f.err || rc=1
  grep -i "empreinte\|erreur\|divergen" $OUT/$f.txt $OUT/$f.err | tail -4
}
run simple
run d d 65536
run c c 65536
run k k ${NK:-4096}
run k7 k ${NK:-4096} 7
run l l 0 ${LFIN:-100000000}
echo "== tlbbtest ${N:-20000}"
./tlbbtest ${N:-20000} > $OUT/tlbbtest.txt 2>&1 || rc=1
tail -4 $OUT/tlbbtest.txt
exit $rc
