# Audio Tiger : profil stable

Le carillon ImGuiDock est lu directement par macOS. Il ne mesure pas la stabilité
du son de Tiger, qui passe par le périphérique Screamer, les DMA/IRQ PowerPC,
le mélangeur QEMU puis CoreAudio.

## Profil du lanceur

Sur macOS, `run_tiger.sh` utilise désormais par défaut :

```
coreaudio,id=snd0,timer-period=5000,out.frequency=44100,out.buffer-length=11610,out.buffer-count=8
```

Le timer demandé passe de 10 à 5 ms. Le nombre de tampons passe de 4 à 8,
soit une capacité nominale d'environ 93 ms au lieu de 46 ms à 44,1 kHz.
CoreAudio peut négocier une autre taille avec le périphérique réel : ces chiffres
ne sont pas une mesure du délai total. Davantage de réserve amortit les retards,
au prix d'une latence potentiellement supérieure ; elle ne répare pas à elle seule
une émulation durablement trop lente. Ni suréchantillonnage ni changement du volume.

Retour réversible aux paramètres audio QEMU précédents :

```sh
POMPPC_AUDIO_PROFILE=default ./run_tiger.sh
```

`POMPPC_AUDIO_PROFILE=stable` est le défaut ; toute autre valeur est refusée.
Linux conserve son backend PulseAudio et ses réglages précédents. `NOSOUND=1`
reste disponible. Mac OS 9 n'est pas modifié par ce profil spécifique à Tiger.

## Correction Screamer

Le callback ignorait la quantité réellement acceptée par `AUD_write`, puis
retirait **tous** les échantillons demandés de la file. Une écriture partielle ou
nulle pouvait donc perdre du PCM et avancer indûment le compteur d'images audio.
Il n'avance désormais que de la quantité acceptée et conserve le reliquat.
Quand un fragment DMA différé est chargé et que la sortie a encore de la place,
il est transmis dans le même callback, sans attendre un nouveau tick.

`python3 tests/screamer_audio_test.py` compile la fonction réelle avec des sorties
audio/DMA simulées : écriture courte, écriture nulle, reprise sans perte ni
duplication, deux fragments consécutifs, file vide, sortie pleine, profils de
lanceur. L'ancien callback échoue sur l'écriture courte ; le nouveau passe.
Cela prouve la correction de ces cas, pas que tous les craquements rapportés
étaient causés par eux. Le QEMU quotidien doit être reconstruit puis la VM relancée.

Pour diagnostiquer une récidive, `trace-event audio_timer_delayed on` dans HMP
relève les ticks dépassant 1,5 fois la période demandée ; remettre `off` après
la mesure. Ces retards ne sont pas des compteurs de sous-alimentation CoreAudio.
Comparer au repos puis dans la même scène de jeu, sans autre banc lancé en
parallèle. La validation finale des craquements entendus reste une écoute sur
la sortie audio utilisée par l'utilisateur.

Contrôle du 27/09 : QEMU reconstruit (`bench/audio-stable-build.log`), profil
confirmé dans la ligne de commande de la VM, quatre lectures Cocoa dans Tiger
réussies et PCM stéréo 44,1 kHz enregistré (`bench/tiger-audio-stable.wav`).
Cette capture est prise **avant la sortie CoreAudio**, elle ne prouve pas
l'absence de craquements au haut-parleur. Pas de comparaison d'écoute A/B :
la session précédente avait été fermée avant le relevé initial.
Suite générale : 155 tests réussis, zéro échec, quatre ignorés ; les deux tests
frontend (`pointer_policy`, `startup_chime`) passent également.
