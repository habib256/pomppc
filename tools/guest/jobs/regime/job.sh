#!/bin/sh
# regime - lecture du regime de vitesse d'un demarrage (docs/tcg-g4.md §14) :
# micro-banc regbench (NTOUR tours de six noyaux), puis Marble Blast comme
# `tcgmb` (chauffe WARMDUR s, NPASS passes de DUR s, bilan POMPPC_GL_STATS),
# puis, si REBOOT=1, redemarrage PROPRE de l'invite dans le MEME processus
# QEMU (shutdown -r en tache de fond, le job rend la main tout de suite).
#   NTOUR=3 DUR=150 WARM=1 WARMDUR=110 NPASS=1 MB=1 TAG=x REBOOT=0
. ./lib.sh
. ./guilib.sh
[ -f ./env.sh ] && . ./env.sh
NTOUR=${NTOUR:-3}; DUR=${DUR:-150}; WARM=${WARM:-1}; WARMDUR=${WARMDUR:-110}
NPASS=${NPASS:-1}; MBON=${MB:-1}; TAG=${TAG:-x}; REBOOT=${REBOOT:-0}
MBAPP="/Users/tiger/Desktop/MarbleBlast Gold.app"
step() { sync; echo "regime: $*" > /dev/console; echo "== $*"; }
echo "uptime: $(uptime)"
gcc -O2 -mdynamic-no-pic -o regbench regbench.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
step "regbench ($NTOUR tours)"
./regbench $NTOUR > $OUT/bench-$TAG.txt 2>&1
cat $OUT/bench-$TAG.txt | tr '\n' ' '; echo
if [ "$MBON" = 1 ]; then
  plugin_layout > /dev/null
  W=/tmp/regmb; rm -rf $W; mkdir -m 777 $W
  lance() { # $1 fichier de bilan (vide = sans)  $2 duree
    gui_run "T0=\$(date +%s)
if [ -n \"$1\" ]; then POMPPC_GL_STATS=$1 \"$MBAPP/Contents/MacOS/MarbleBlast Gold\" > $W/mb.log 2>&1 &
else \"$MBAPP/Contents/MacOS/MarbleBlast Gold\" > $W/warm.log 2>&1 & fi
p=\$!; sleep 8
osascript -e \"tell application \\\"System Events\\\" to set frontmost of (first process whose unix id is \$p) to true\" >/dev/null 2>&1
echo \"MB debut \$T0\"
sleep $(($2 - 8))
kill \$p 2>/dev/null; sleep 3; kill -9 \$p 2>/dev/null; echo \"MB fin \$(date +%s)\"" $(($2 + 90)) > /dev/null
  }
  [ "$WARM" = 1 ] && { step "chauffe ($WARMDUR s)"; lance "" $WARMDUR; }
  i=1
  while [ $i -le $NPASS ]; do
    step "mesure $TAG-$i ($DUR s)"
    lance $W/mb-$TAG-$i.txt $DUR
    cp $W/mb-$TAG-$i.txt $OUT/ 2>/dev/null
    echo "-- fenetres $TAG-$i : $(grep -c fps $W/mb-$TAG-$i.txt 2>/dev/null)"
    i=$((i+1))
  done
fi
step "fini"
sync
if [ "$REBOOT" = 1 ]; then
  ( sleep 5; sync; sync; /sbin/shutdown -r now ) > /dev/null 2>&1 &
  echo "redemarrage demande dans 5 s"
fi
