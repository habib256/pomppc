#!/bin/sh
# fpgames — A/B du « flottant rapide » (docs/flottant-rapide.md) sur les VRAIS jeux.
#
# Se distingue du job `games` (qui mesure le PLUGIN) par trois points, imposés
# par ce qu'on mesure ici — le processeur émulé, pas le rendu :
#
#  * LANCEMENT DE CHAUFFE d'abord. Un premier lancement de Marble Blast compile
#    ses scripts .cs en .dso et remplit le cache de fichiers : il ne mesure pas
#    le jeu, il mesure l'installation. Idem pour Zenerchi.
#  * DURÉE LONGUE (DUR=300 par défaut) : la démo de Marble Blast ne se joue
#    toute seule qu'après les menus. Les fenêtres de 5 s du bilan sont ensuite
#    APPARIÉES PAR NOMBRE DE TRIANGLES (docs/gpu-3d-tiger.md §4.7) : ce sont
#    alors les mêmes images des deux côtés.
#  * TEMPS DE CHARGEMENT de Zenerchi : le jeu décode TOUTES ses musiques Ogg
#    Vorbis avant d'afficher son menu (100 % du profil dans ov_read/mdct). La
#    mesure est le délai lancement → PREMIÈRE IMAGE RENDUE, obtenu sans toucher
#    au plugin : le bilan périodique écrit sa première ligne 5 s après la
#    première image, on retranche 5. Résolution ~1 s, biais identique dans les
#    deux modes (la DIFFÉRENCE est donc exacte, le rapport à ±5 %).
#
#   GAMES="zen mb"   jeux à mesurer
#   DUR=300          secondes de mesure par jeu
#   WARM=1           lancement de chauffe avant la mesure (0 pour sauter)
#   WARMDUR=110      durée du lancement de chauffe
#   TAG=rapide       étiquette reprise dans les noms de fichiers
. ./lib.sh
. ./guilib.sh
GAMES=${GAMES:-"zen mb"}
DUR=${DUR:-300}
WARM=${WARM:-1}
WARMDUR=${WARMDUR:-110}
TAG=${TAG:-x}
DESK=/Users/tiger/Desktop
MB="$DESK/MarbleBlast Gold.app"
ZEN="$DESK/Zenerchi.app"
step() { sync; echo "fpgames: $*" > /dev/console; echo "== $*"; }

step "préparation ($TAG)"
for app in "MarbleBlast Gold.app" Zenerchi.app; do
  if [ ! -d "$DESK/$app" ] && [ -d "/Volumes/GAMES/$app" ]; then
    cp -R "/Volumes/GAMES/$app" "$DESK/" && echo "copié depuis le CD : $app"
  fi
  [ -d "$DESK/$app" ] && echo "présent : $app" || echo "ABSENT : $app"
done
chown -R tiger "$MB" "$ZEN" 2>/dev/null; chmod -R u+w "$MB" "$ZEN" 2>/dev/null
kextstat | grep -i pomppc | awk '{print "kext :", $6, $7}'
plugin_layout
echo "framebuffers : $(fb_count)"
sysctl -n hw.ncpu hw.activecpu 2>/dev/null | tr '\n' ' '; echo "(ncpu activecpu)"

W=/tmp/fpgames; rm -rf $W; mkdir -m 777 $W

# lancement de chauffe : on jette la mesure, on garde l'état du disque
chauffe() { # $1 étiquette  $2 exécutable  $3 durée
  step "chauffe $1 ($3 s)"
  gui_run "\"$2\" > $W/warm-$1.log 2>&1 &
p=\$!; sleep 8
osascript -e \"tell application \\\"System Events\\\" to set frontmost of (first process whose unix id is \$p) to true\" >/dev/null 2>&1
sleep $(($3 - 8)); kill \$p 2>/dev/null; sleep 4; kill -9 \$p 2>/dev/null; echo chauffe-finie" $(($3 + 90))
}

# ---------------------------------------------------------------- Zenerchi
# métrique = temps de chargement (lancement → première image)
mesure_zen() {
  step "zen mesure ($TAG)"
  gui_run "rm -f $W/zen-$TAG.txt
T0=\$(date +%s)
POMPPC_GL_STATS=$W/zen-$TAG.txt \"$ZEN/Contents/MacOS/Zenerchi\" > $W/zen-$TAG.log 2>&1 &
p=\$!
sleep 6
osascript -e \"tell application \\\"System Events\\\" to set frontmost of (first process whose unix id is \$p) to true\" >/dev/null 2>&1
n=0
while [ ! -s $W/zen-$TAG.txt ]; do
  sleep 1; n=\$((n+1))
  [ \$n -gt 400 ] && break
  kill -0 \$p 2>/dev/null || break
done
T1=\$(date +%s)
CPU=\$(ps -o time= -p \$p 2>/dev/null | tr -d ' ')
echo \"ZEN $TAG premiere_ligne_s=\$((T1-T0)) premiere_image_s=\$((T1-T0-5)) cpu_a_ce_moment=\$CPU\"
/usr/sbin/screencapture -x $W/zen-$TAG.png
sleep 45
/usr/sbin/screencapture -x $W/zen-$TAG-b.png
ps -o time=,rss= -p \$p 2>/dev/null | sed 's/^/ZEN $TAG fin cpu,rss: /'
kill \$p 2>/dev/null; sleep 3; kill -9 \$p 2>/dev/null; echo zen-fini" 600
  cp $W/zen-$TAG.txt $W/zen-$TAG.log $W/zen-$TAG.png $W/zen-$TAG-b.png $OUT/ 2>/dev/null
  echo "-- bilan zen ($TAG)"; cat $W/zen-$TAG.txt 2>/dev/null | head -20
}

# ------------------------------------------------------------ Marble Blast
# métrique = images/s par fenêtre de 5 s, appariées par triangles/image
mesure_mb() {
  step "mb mesure ($TAG, $DUR s)"
  gui_run "rm -f $W/mb-$TAG.txt
T0=\$(date +%s)
POMPPC_GL_STATS=$W/mb-$TAG.txt \"$MB/Contents/MacOS/MarbleBlast Gold\" > $W/mb-$TAG.log 2>&1 &
p=\$!; sleep 8
osascript -e \"tell application \\\"System Events\\\" to set frontmost of (first process whose unix id is \$p) to true\" >/dev/null 2>&1
echo \"MB $TAG demarrage_a \$((\$(date +%s)-T0))s\"
sleep $((DUR / 2 - 8)); /usr/sbin/screencapture -x $W/mb-$TAG.png
top -l 2 -s 5 -n 8 -o cpu > $W/mb-$TAG-top.txt 2>&1
/usr/bin/sample \$p 12 -file $W/mb-$TAG-sample.txt > /dev/null 2>&1
sleep $((DUR / 2 - 25)); /usr/sbin/screencapture -x $W/mb-$TAG-b.png
ps -o time= -p \$p 2>/dev/null | sed 's/^/MB $TAG cpu_total: /'
kill \$p 2>/dev/null; sleep 3; kill -9 \$p 2>/dev/null; echo mb-fini" $((DUR + 180))
  cp $W/mb-$TAG.txt $W/mb-$TAG.log $W/mb-$TAG.png $W/mb-$TAG-b.png \
     $W/mb-$TAG-top.txt $W/mb-$TAG-sample.txt $OUT/ 2>/dev/null
  echo "-- fenêtres mb ($TAG) : $(grep -c . $W/mb-$TAG.txt 2>/dev/null)"
  head -4 $W/mb-$TAG.txt 2>/dev/null
  echo "-- profil (20 premières lignes lourdes)"
  grep -E '^ +[0-9]+ ' $W/mb-$TAG-sample.txt 2>/dev/null | head -12
}

for g in $GAMES; do
  case $g in
    zen) [ "$WARM" = 1 ] && chauffe zen "$ZEN/Contents/MacOS/Zenerchi" 120
         mesure_zen ;;
    mb)  grep -E 'Video::(resolution|fullScreen)' "$MB/marble/client/prefs.cs" 2>/dev/null
         [ "$WARM" = 1 ] && chauffe mb "$MB/Contents/MacOS/MarbleBlast Gold" $WARMDUR
         mesure_mb ;;
  esac
done
step "fini"
sync
