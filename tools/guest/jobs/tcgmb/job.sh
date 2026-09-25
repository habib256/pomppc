#!/bin/sh
# tcgmb - Marble Blast Gold pour les A/B du processeur emule (docs/tcg-g4.md) :
# SMP=1 contre SMP=2, binaire QEMU de base contre binaire patche (patches/tcg/).
# Reprend le protocole de `fpgames` (docs/flottant-rapide.md) : lancement de
# chauffe (scripts .cs compiles, cache disque), puis demo jouee toute seule,
# bilan du plugin toutes les 5 s (POMPPC_GL_STATS), fenetres appariees par
# triangles/image cote hote (tools/tcg/mbpair.py). Rien d'autre ne tourne dans
# l'invite pendant la mesure (pas de `sample`, pas de capture).
#
#   DUR=240   secondes de mesure     WARM=1  chauffe (0 pour sauter)
#   TAG=x     etiquette des fichiers NPASS=1 passes de mesure (TAG-1, TAG-2...)
# Les valeurs peuvent venir d'un env.sh livre avec le job (tools/tcg/mbab.sh).
. ./lib.sh
. ./guilib.sh
[ -f ./env.sh ] && . ./env.sh
DUR=${DUR:-240}
WARM=${WARM:-1}
WARMDUR=${WARMDUR:-110}
TAG=${TAG:-x}
NPASS=${NPASS:-1}
MB="/Users/tiger/Desktop/MarbleBlast Gold.app"
step() { sync; echo "tcgmb: $*" > /dev/console; echo "== $*"; }
sysctl -n hw.ncpu hw.activecpu 2>/dev/null | tr '\n' ' '; echo "(ncpu activecpu)"
plugin_layout
W=/tmp/tcgmb; rm -rf $W; mkdir -m 777 $W
lance() { # $1 fichier de bilan (vide = sans)  $2 duree
  gui_run "T0=\$(date +%s)
if [ -n \"$1\" ]; then POMPPC_GL_STATS=$1 \"$MB/Contents/MacOS/MarbleBlast Gold\" > $W/mb.log 2>&1 &
else \"$MB/Contents/MacOS/MarbleBlast Gold\" > $W/warm.log 2>&1 & fi
p=\$!; sleep 8
osascript -e \"tell application \\\"System Events\\\" to set frontmost of (first process whose unix id is \$p) to true\" >/dev/null 2>&1
echo \"MB debut \$T0\"
sleep $(($2 - 8))
ps -o time= -p \$p 2>/dev/null | sed 's/^/MB cpu_total: /'
kill \$p 2>/dev/null; sleep 3; kill -9 \$p 2>/dev/null; echo \"MB fin \$(date +%s)\"" $(($2 + 90))
}
[ "$WARM" = 1 ] && { step "chauffe ($WARMDUR s)"; lance "" $WARMDUR; }
i=1
while [ $i -le $NPASS ]; do
  step "mesure $TAG-$i ($DUR s)"
  lance $W/mb-$TAG-$i.txt $DUR
  cp $W/mb-$TAG-$i.txt $OUT/ 2>/dev/null
  echo "-- fenetres $TAG-$i : $(grep -c . $W/mb-$TAG-$i.txt 2>/dev/null)"
  i=$((i+1))
done
cp $W/mb.log $OUT/ 2>/dev/null
step "fini"
sync
