#!/bin/sh
# meas-tcg.sh <label> - DOOM 3 demo_mars_city1 pour les A/B du processeur emule
# (docs/tcg-g4.md). Derive de meas3.sh (lot 3) : meme lanceur (~/doom3-env.command,
# ~/lot3.env vide = plugin par defaut), meme fenetre T+50..T+280, mais :
#  - T (fin de la cinematique) ne depend d'aucun seuil absolu (85 puis 65 ms/image
#    jusqu'au 26/09 : avec x-fp-inline le jeu tourne a ~63 et T n'etait plus trouve) :
#    une fois l'image 5000 passee, S = 0,8 x le niveau du jeu lu sur les 400
#    dernieres images, T = premiere image a >= 1500 dont les 200 images suivantes ET
#    les 200 d'apres depassent S et sont stables a 15 % pres (replis <= 8), accepte
#    si la derniere image est a >= T+1000 (les 400 dernieres sont alors dans le
#    niveau ; meme regle que tools/matrice/jeux/d3.py, d3win.py l'affine) ;
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
T() { awk -F, '$1 ~ /^[0-9]+$/ { e[$1]=$3; fb[$1]=$6; if ($1>z) z=$1 }
        END { if (z < 5000 || !((z-400) in e)) exit;
          S = 0.8 * (e[z]-e[z-400]) / 400;
          for (f = 1900; f <= z; f++) { a=f-400; b=a+200;
            if (!(a in e) || !(b in e) || !(f in e)) continue;
            u=(e[b]-e[a])/200; v=(e[f]-e[b])/200; lo=(u<v)?u:v; hi=(u<v)?v:u;
            if (lo > S && lo > 0.85*hi && fb[f]-fb[a] <= 8) { if (z >= a+1000) print a; exit } } }' ~/d3-dump/frames.csv; }
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
