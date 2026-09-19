#!/bin/sh
# Jeux réels en mode bureau (devloop.py start --gui), avec le bilan du plugin.
#
# Les jeux ne sont pas dans le dépôt : ils sont pris sur le bureau de
# l'utilisateur de test, ou copiés depuis un CD « GAMES » (CDROM=disks/…iso au
# démarrage de la VM). Chaque jeu tourne DUR secondes dans la session, avec
# POMPPC_GL_STATS : images/s, replis logiciels et motifs de refus avec leur
# premier cas (docs/gpu-3d-tiger.md §4.5). Une capture d'écran est prise au
# milieu. Suppose le kext et le plugin installés (job install).
#
#   GAMES="zen mb mb16" : zen = Zenerchi (plein écran), mb = Marble Blast Gold
#   tel que réglé, mb16 = Marble Blast forcé en « 800 600 16 » (prefs.cs)
#   DUR=60
. ./lib.sh
. ./guilib.sh
GAMES=${GAMES:-"zen mb mb16"}
DUR=${DUR:-80}
DESK=/Users/tiger/Desktop
MB="$DESK/MarbleBlast Gold.app"
ZEN="$DESK/Zenerchi.app"
step() { sync; echo "games: $*" > /dev/console; echo "== $*"; }

step "jeux"
for app in "MarbleBlast Gold.app" Zenerchi.app; do
  if [ ! -d "$DESK/$app" ] && [ -d "/Volumes/GAMES/$app" ]; then
    cp -R "/Volumes/GAMES/$app" "$DESK/" && echo "copié depuis le CD : $app"
  fi
  [ -d "$DESK/$app" ] || { echo "absent : $app"; exit 1; }
done
chown -R tiger "$MB" "$ZEN"; chmod -R u+w "$MB" "$ZEN"
kextstat | grep -i pomppc | awk '{print "kext :", $6, $7}'
plugin_layout      # mode bureau : pas de seconde copie dans Resources
ls -d $EXT/GLDriver-POMPPC.bundle $RES/GLDriver-POMPPC.bundle 2>&1

W=/tmp/games; rm -rf $W; mkdir -m 777 $W
# $1 étiquette, $2 exécutable
# Le jeu est mis au PREMIER PLAN (System Events, par son pid) : lancé depuis un
# script, il resterait derrière le Finder, présentation directe coupée — ce
# n'est pas l'usage réel. `top` échantillonne l'invité à mi-parcours : qui,
# du jeu ou du WindowServer, consomme le processeur émulé.
play() {
  step "$1 ($DUR s)"
  gui_run "POMPPC_GL_STATS=$W/$1.txt \"$2\" > $W/$1.log 2>&1 &
p=\$!; sleep 8
osascript -e \"tell application \\\"System Events\\\" to set frontmost of (first process whose unix id is \$p) to true\" 2>&1
sleep $((DUR / 2 - 8)); /usr/sbin/screencapture -x $W/$1.png
top -l 2 -s 5 -n 8 -o cpu > $W/$1-top.txt 2>&1
/usr/bin/sample \$p 10 -file $W/$1-sample.txt > /dev/null 2>&1
sleep $((DUR / 2 - 20))
kill \$p; sleep 3; kill -9 \$p 2>/dev/null; echo fini" $((DUR + 120))
  cp $W/$1.txt $W/$1.log $W/$1.png $W/$1-top.txt $W/$1-sample.txt $OUT/ 2>/dev/null
  echo "-- top (2e échantillon)"
  awk '/^Processes/{n++} n==2' $W/$1-top.txt 2>/dev/null | head -16
  echo "-- bilan"
  cat $W/$1.txt 2>/dev/null | tail -25
  echo "-- journal"; tail -5 $W/$1.log 2>/dev/null
}

PREFS="$MB/marble/client/prefs.cs"
for g in $GAMES; do
  case $g in
    zen)  play zen "$ZEN/Contents/MacOS/Zenerchi" ;;
    mb)   grep -E 'Video::(resolution|fullScreen)' "$PREFS"
          play mb "$MB/Contents/MacOS/MarbleBlast Gold" ;;
    mb16) cp "$PREFS" $W/prefs.cs.orig
          sed 's/^\$pref::Video::resolution = .*/$pref::Video::resolution = "800 600 16";/' \
            $W/prefs.cs.orig > "$PREFS"
          grep -E 'Video::(resolution|fullScreen)' "$PREFS"
          play mb16 "$MB/Contents/MacOS/MarbleBlast Gold"
          cp $W/prefs.cs.orig "$PREFS" ;;
  esac
done
