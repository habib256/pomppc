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
nulle pouvait donc perdre du PCM et avancer indûment le compteur d'images audio (depuis le 29/09, ce compteur suit le DMA : voir « Saccades de DOOM 3 »).
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

## Saccades de DOOM 3 (29/09/2026)

**Symptôme.** Le son de DOOM 3 saccade ; les autres jeux, pas ou presque.

**Ce qui se passe pendant une saccade.** Relevé de Mars City (partie
`game/demo_mars_city1`, joueur immobile, VM quotidienne relancée, DOOM 3 premier
jeu lancé) avec l'instrumentation de `patches/screamer/essais/diag-saccades.patch` :

| Mesure | Valeur |
|---|---|
| tampons CoreAudio hôte vides | 0 |
| silence inséré par le Screamer | 0 trame |
| anneau PCM | plein, 8192 trames |
| écart max entre callbacks | 17,8 ms |
| doorbells synchrones qgpu | 0 |
| IOProc de DOOM 3 (`iotrace`) | 4096 trames, 4-6 ms, marge ≥ 72 ms, aucun saut de temps d'échantillon |
| PCM lu par le DMA | **~47 ms de zéros exacts toutes les 92,9 ms** |

Le PCM que le DMA lit dans l'invité contient 84,6 % de trames nulles, soit 776 trous
de 10 ms ou plus en 84 s. Chaque trou finit au début d'un tampon d'E/S du HAL
(`out_st` ≡ 192 mod 4096 dans `iotrace`, fin des trous à 4096k + 192) : c'est la
**seconde moitié de chaque tampon de 4096 trames** qui manque. DOOM 3 calcule et livre
bien ses tampons, et l'hôte joue bien ce qu'il reçoit : c'est l'invité qui efface.

**Mécanisme.** Apple02DBDMAAudio tire sa position de lecture du compteur de trames du
Screamer (registre 5, lu ≈ 11 fois par seconde : la tête d'effacement d'IOAudioEngine,
quatre passages par tour du tampon de 16384 trames ; `CMDPTR` n'est jamais lu).
Elle efface tout ce qui est derrière ce compteur. Deux fautes du Screamer décalaient
ce compteur du DMA :

1. il avançait à la **sortie** de l'anneau (vers `AUD_write`), 8192 trames derrière le
   DMA quand l'anneau est plein, soit exactement la moitié du tampon de Tiger ;
2. l'écriture de 0 que fait le pilote au démarrage du moteur, juste avant de lancer le
   DMA, était **ignorée** (`Unimplemented register write`) : le compteur gardait le
   compte de toutes les lectures depuis le démarrage de la machine.

« Derrière le compteur » tombait donc devant le DMA, à une distance qui dépend du
passé. Au premier jeu après le démarrage (compteur à 0), elle vaut exactement 8192
trames. La tête d'effacement balaie alors [DMA + 4096, DMA + 8192], là où le HAL
d'un client à tampon de 4096 trames vient d'écrire (`out_st` = `now_st` + ~4115). La part
détruite dépend de la phase entre le minuteur d'effacement et l'IOProc. Ces deux
horloges dérivent l'une par rapport à l'autre, si bien que les saccades vont et viennent.

**Pourquoi DOOM 3 et pas les autres.** DOOM 3 (et Prey, même moteur) fixe le tampon
d'E/S à 4096 trames (`setting frame size to: 4096` dans sa console, 32768 octets par
appel dans `iotrace`) et écrit donc 93 à 186 ms devant le DMA. Marble Blast garde le
défaut de 512 trames et écrit ~540 trames devant : hors de la zone effacée pour le
décalage de 8192. Il ne serait touché que pour certains décalages, après d'autres
sessions sans redémarrage. Prey saccade aussi quand il est lancé le premier :
33,7 % de trames nulles, 1694 trous en 164 s. Selon l'historique du compteur, un
lancement de DOOM 3 peut aussi s'en tirer : plein écran, deuxième jeu de la session,
0,1 %.

**Preuve du mécanisme.** Sans toucher au compteur, un anneau de 2048 trames
(`SCREAMER_RING=2048`, décalage ramené à 2048) supprime les trous : 0 trame nulle en
85 s. Avec l'ancien comportement (`SCREAMER_FC_ANCIEN=1 SCREAMER_FCW_IGNORE=1`), les trous
reviennent au premier jeu.

**Correctif** (`patches/screamer/screamer.c`) : le compteur avance dans
`screamer_tx_copy`, c'est-à-dire avec la lecture du DMA, comme sur le matériel dont la
FIFO ne tient que quelques trames ; l'écriture du registre 5 est acceptée. L'anneau
garde ses 8192 trames. Le relevé montre l'ordre : écriture de 0 au démarrage du moteur,
puis début du DMA, dans la même milliseconde.

**Après** (même binaire instrumenté, interrupteurs par défaut) :

| Partie | Trames nulles | Trous ≥ 10 ms | Silence inséré | Tampons hôte vides |
|---|---|---|---|---|
| DOOM 3 fenêtre, premier jeu après démarrage, 272 s | 0,2 % (passages à zéro) | 0 | 0 | 0 |
| DOOM 3 plein écran, premier jeu, 183 s | 0,0 % | 0 | 0 | 0 |
| Prey fenêtre, 204 s | 0,4 % | 3 (chargement) | 0 | 0 |
| Marble Blast fenêtre, 94 s | 0,0 % | 0 | 0 | 0 |

Des extraits de 20 s avant et après, pris dans la même scène, sont rangés dans
`bench/son-doom3/*.wav` pour l'écoute (hors dépôt).
`python3 tests/screamer_audio_test.py` vérifie les deux points ; l'ancien fichier
échoue au test. `tests/run-all.sh` : 162 OK, 0 échec.

**À part.** Il reste un arrêt de la boucle principale de QEMU : 186 ms à la fin de la
cinématique de DOOM 3, avec 0 doorbell synchrone dans la seconde. Il a été vu une fois
en 13 parties et a donné 8 tampons hôte vides, soit 93 ms de silence. Sa cause n'est
pas établie ; ce n'est pas la saccade.

**Outils.**

| Outil | Rôle |
|---|---|
| `tools/guest/jobs/iotrace/iotrace.c` | bibliothèque à insérer (`DYLD_INSERT_LIBRARIES`) qui enveloppe l'IOProc CoreAudio d'une application Tiger : durée, marge, sauts de temps d'échantillon |
| `tools/son/partie.py` | une partie pilotée depuis l'hôte (lanceur `tools/guest/jobs/iotrace/d3son.command` copié en `~/son/d3.command`) |
| `tools/son/resume.py`, `bilan.py`, `zpos.py` | lecture du journal `SCREAMER_DIAG` |
| `tools/son/pcm.py` | trous, creux et sauts du PCM (`SCREAMER_PCM`), extrait WAV |
| `tools/son/iot.py` | résumé d'`iotrace.csv` |
