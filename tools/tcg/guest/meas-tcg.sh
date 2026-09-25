#!/bin/sh
# meas-tcg.sh <label> - DOOM 3 demo_mars_city1 pour les A/B du processeur emule
# (docs/tcg-g4.md). Derive de meas3.sh (lot 3) : meme lanceur (~/doom3-env.command,
# ~/lot3.env vide = plugin par defaut), meme fenetre T+50..T+280, mais :
#  - T (fin de la cinematique) ne depend plus d'un seuil absolu de 85 ms/image, qui
#    ne tiendrait pas si l'emulateur accelere : T = premiere image a >= 1500 dont les
#    200 images suivantes ET les 200 d'apres depassent 65 ms/image en moyenne
#    (la cinematique a un pic de ~65 ms/image sur 200 images, jamais 400 ; le jeu
#    tient ~86 ms/image avec le binaire de reference, ~69 s'il gagnait 20 %) ;
#  - pas de `sample` dans l invite (il perturbe la mesure) : a t+400 (t = seuil
#    grossier, le saut exact est affine par tools/tcg/d3win.py, au plus ~100 images
#    plus loin) le script ecrit
#    PROFIL dans ~/meas-<label>.txt, l'hote peut alors echantillonner QEMU ;
#  - fin a t+600 (>= T+450).
L=$1
killall ScreenSaverEngine 2>/dev/null
rm -f ~/d3-dump/frames.csv ~/meas-$L.txt ~/lot3.env
touch ~/lot3.env
open ~/doom3-env.command
n() { tail -1 ~/d3-dump/frames.csv 2>/dev/null | cut -d, -f1 | grep '^[0-9]' || echo 0; }
T() { awk -F, '$1 ~ /^[0-9]+$/ { e[$1]=$3; fb[$1]=$6;
        if ($1>=1900) { a=$1-400; b=a+200;
          if (a>=1500 && (e[b]-e[a])/200 > 65 && (e[$1]-e[b])/200 > 65 && fb[$1]-fb[a] <= 8) { print a; exit } } }' ~/d3-dump/frames.csv; }
i=0; t=""
while :; do
  sleep 5; i=$((i+1))
  if [ $i -gt 600 ]; then echo TIMEOUT >> ~/meas-$L.txt; exit 1; fi
  [ -z "$t" ] && t=$(T)
  if [ -n "$t" ] && [ "$(n)" -ge $((t+400)) ]; then break; fi
done
echo "T $t" > ~/meas-$L.txt
echo "PROFIL $(n)" >> ~/meas-$L.txt
while [ "$(n)" -lt $((t+600)) ]; do sleep 5; done
cp ~/d3-dump/frames.csv ~/frames-$L.csv; cp ~/d3-dump/note.txt ~/note-$L.txt 2>/dev/null
echo FINI >> ~/meas-$L.txt
