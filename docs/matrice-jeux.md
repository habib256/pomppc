# Matrice de jeux automatisée (chantier A3)

Depuis le 27/09, la capture utilise `POMPPC_GL_CAPTURE=<chemin>` : après au moins
deux images depuis le début du vidage, le plugin attend la barrière d'un
`SURF_PRESENT` vidé, publie atomiquement son numéro dans ce fichier et suspend
les dessins du processus. Le harnais confirme ce numéro dans `frames.csv`,
capture la VM figée, puis crée `<chemin>.resume` dans un `finally`.
L'attente du plugin est bornée à 30 s ; `<chemin>.expired` rend la preuve invalide.
Sans cette option, aucune attente de capture n'est ajoutée au rendu normal.
`POMPPC_GL_CAPTURE_DELAY` fixe le nombre minimal d'images depuis le début du
vidage (2 par défaut, 12 pour Colin McRae qui échange plusieurs surfaces).
Le numéro est conservé dans `capture-frame.txt` et l'analyse ne compare que les
présentations de cette image. Les anciens tours sans ce fichier restent analysables
par recherche de la meilleure image, comme auparavant. Les tolérances ne changent pas.
Le rejeu conserve aussi le format de présentation : RGB1555 est relu en 16 bits
puis développé en RGB comme le scanout VGA de la VM quotidienne (décalage de
trois bits, sans réplication), et le découpage utilise deux octets par pixel au lieu
de quatre. Le format figure en septième colonne de `presents.txt` ; les anciens
fichiers à six colonnes restent interprétés comme du 32 bits.

Marble Blast fenêtre : le module règle **les deux** exports Torque,
`~/Library/MarbleBlast/{common,marble}/client/prefs.cs`, à 800×600 ; chacun pouvait
réintroduire 1024×768. Les fichiers et leurs caches `.dso` sont restaurés après le tour.
Premier contrôle : `bench/matrice/priorites-mb-fen`, capture/rejeu 0,00 %, 9,9 ms/image.
Le tour `priorites-capture-01` confirme Marble Blast, DOOM 3 et Prey en fenêtre :
trois cellules vertes, capture/rejeu 0,00 %, respectivement 9,9, 34,1 et 70,6 ms/image.
La série de dix tours consécutifs DOOM 3/Prey est **reportée à la demande de
l'utilisateur** ; ce premier tour ne la remplace pas.

`tools/matrice/` joue chaque jeu de la matrice (TODO §1) dans chaque mode — **fenêtre** et
**plein écran** — sur la VM quotidienne, et produit un tableau vert/rouge avec, pour chaque
cellule, ses preuves rangées. C'est le harnais qui doit autoriser A2 (découpage du plugin) et
A4 (travail déplacé vers l'hôte) sans peur : on le rejoue avant et après.

```sh
tools/matrice/matrice.py                     # tout : jeux automatisés, deux modes (~36 min)
tools/matrice/matrice.py -j mb,zen -m fen    # un sous-ensemble (jeux, modes)
tools/matrice/matrice.py --deux-passes       # mesure puis preuve séparées (plugin d'avant le 26/09 après midi)
tools/matrice/matrice.py --sans-vidage       # vitesse et replis seuls
tools/matrice/matrice.py -j d3 -m fen --sans-vidage --env POMPPC_GL_STSKIP=0 --sample 10
                                             # A/B d'un drapeau du plugin, `sample` de 10 s dans
                                             # l'invité après la fenêtre (sample.txt de la cellule ;
                                             # tools/re/sampleplug.py le résume), --sortie DOSSIER
tools/matrice/matrice.py --liste             # jeux et modes connus
tools/matrice/matrice.py --valider d3-pe     # référence d'image vue et validée à l'œil
tools/matrice/matrice.py --analyse bench/matrice/<tour>   # refaire l'analyse d'un tour rangé
tools/matrice/matrice.py --reprendre bench/matrice/<tour> -j ut -m pe   # rejouer des cellules
python3 tests/matrice_test.py                 # règles de scène et comparateur, hors VM
```

Prérequis : la VM quotidienne lancée (`./run_tiger.sh`), ssh (`tools/guest/tssh.sh uptime`),
moniteur `.run/mon.sock` (chemin et port ssh effectifs publiés par le lanceur dans
`.run/tiger.mon` et `.run/tiger.sshport`, que la matrice lit), personne d'autre sur la VM.
La matrice ne supprime jamais `.run/tiger.lock` : si le QEMU qu'elle relance ne libère pas le
verrou en 60 s, elle s'arrête sans rien relancer. Sorties dans
`bench/matrice/<AAAAMMJJ-HHMM>/` du dépôt principal (non versionné) : `tableau.md`,
`resultats.csv`, un dossier par cellule ; `bench/matrice/dernier` pointe sur le dernier tour.

## 1. Les trois preuves d'une cellule

Un jeu est **vert** dans un mode quand les trois preuves y sont (TODO §1) :

1. **Image juste.** Le plugin vide les soumissions à partir du déclencheur
   (`POMPPC_GL_DUMP_TRIGGER`, vidage autonome : tout l'état et toutes les textures réémis). Le
   vidage est **rejoué en natif** sur l'hôte (`tests/qgpu_replay.c`, backend `gl`, le même
   cœur et le même backend que le device). Trois contrôles :
   - **rejeu = VM** : pendant le vidage, la VM est arrêtée (`stop` au moniteur), l'écran est
     capturé (`screendump`) puis la VM repart (`cont`). L'une des images rejouées doit être
     identique, à la tolérance près, au rectangle de la capture où le jeu l'a présentée
     (position lue dans le `SURF_PRESENT` : décalage en mémoire vidéo et pas). Tolérance :
     écart moyen ≤ 0,3 par composante et ≤ 0,2 % de pixels à plus de 16 d'écart (le curseur
     logiciel de Tiger, ~0,1 %). En pratique **0,00 / 0,00 %** quand la capture tombe dans le
     vidage : l'hôte rend à l'octet ce que la VM a affiché. Une tolérance de 1,5 / 1 % (premier
     essai) laissait passer une image voisine (chrono de Marble Blast différent) ;
   - **référence** : le vidage de référence de la cellule (rangé, voir §3) est rejoué avec le
     code du moment et doit redonner l'image de référence (≤ 0,5 / 0,1 %) : c'est ce qui
     attrape une régression du cœur ou des backends de l'hôte ;
   - **validée à l'œil** : la référence a été regardée une fois et déclarée juste
     (`--valider`). Une image non vide ni unie est aussi exigée (écart-type, teintes).
2. **Zéro repli** : le compteur `fallbacks` de `frames.csv` ne bouge pas de la fenêtre de
   mesure à la fin du vidage, sur les deux lancements. **En fenêtre**, le plugin rend
   volontairement un échange sur 90 à Apple (`DIRECT_REFRESH`, rafraîchissement de la mémoire
   de la fenêtre côté WindowServer) : 2 replis par tranche de 90 images sont admis.
3. **Vitesse** : ms/image sur la fenêtre de mesure de la scène fixe, sous le **plancher** du
   jeu (une garde contre la régression, ~1,3 × la mesure du 26/09).

## 2. Déroulé d'une cellule

Par défaut, **un lancement** par cellule : la fenêtre de mesure (vitesse, replis), puis dans
le même lancement la preuve (déclencheur posé, vidage, capture figée, replis).

Jusqu'au 26/09 après midi il en fallait deux (`--deux-passes` les rejoue : mesure sans
déclencheur, puis preuve) : tant que le fichier déclencheur n'existait pas, le plugin faisait un
`access()` par dessin texturé (`draw_probe`, `cube_probe`, le vidage), et DOOM 3 passait de 77 à
139 ms/image. Le plugin ne regarde plus le fichier qu'une fois par image (`dump_trigger`,
`pomppc_accel.c`) : DOOM 3 plein écran **64,2 ms/image déclencheur armé, contre 62,8 sans**
(tour `20260926-1519` contre `20260926-1452`, VM de l'agent TCG allumée à côté).

Un lancement (`Cellule.jouer`, `tools/matrice/matrice.py`) :

1. réglages du jeu sauvegardés dans l'invité (`<fichier>.matrice-sauve`), puis écrits pour le
   mode (Marble Blast, UT2004 : `-fullscreen`, `UT2004.ini`…) ;
2. `~/matrice/cellule.sh` déposé (variables `POMPPC_GL_*`, commande du jeu), puis
   `open ~/matrice/lance.command` : passer par Terminal met le jeu dans la session graphique ;
   `env VAR= open` ne transmet rien quand Terminal tourne déjà, d'où le fichier ;
3. toutes les 10 s, relevé **incrémental** de `frames.csv` (`tail -c +N`) — relire tout le
   fichier coûtait jusqu'à 35 % du processeur invité à `sshd` et ralentissait le jeu —, remise
   au premier plan (osascript) au début puis dès que les replis montent (lancé par ssh, le jeu
   reste derrière et chaque échange se replie, `Swap60`). Depuis le 06/10, jamais d'osascript
   qui puisse tomber dans une fenêtre fixe (§5, « Mesure et rafales ») : System Events est
   lancé avant le jeu, la remise au premier plan part dès que le processus du jeu existe, et
   des replis vus pendant la fenêtre sont notés (`premier_plan` dans le résultat) et traités
   après elle ;
4. la **règle de scène** du jeu (§4) dit quand la fenêtre de mesure est passée ;
5. preuve seulement : `touch /tmp/matrice-go`, attente de `capture-ready`, confirmation
   de son numéro dans `frames.csv`, capture figée puis acquittement `.resume` ; on attend
   enfin que le vidage ne grossisse plus. Un jeu déterministe peut déclarer
   `dump_image = n` : le déclencheur `@n` démarre alors le vidage à une image fixe
   (Nexuiz), avec le même rendez-vous de capture. Le plugin expire après 30 s si le
   harnais disparaît ; un plugin sans rendez-vous produit une preuve refusée ;
6. arrêt du jeu (`kill`), fermeture de Terminal une fois `lance.command` fini (sinon dialogue),
   réglages rendus, nettoyage du jeu (CD démonté) ;
7. rapatriement du dossier de l'invité (tar par ssh), puis effacement dans l'invité ;
8. contrôle des clients du kext (`ioreg -c POMPPCGPUUserClient`) : il n'en reste aucun
   hors jeu, sinon la cellule le note et l'invité est **redémarré** (tranches perdues).
   Depuis le 29/09, DOOM 3 ne redémarre plus l'invité d'office : le `kCGLBadDisplay`
   après un kill ne se reproduit plus (0 sur 25, `docs/gel-doom3-baddisplay.md` §1).
   Colin McRae le fait encore. Panique AppleUSBOHCI au démarrage : `system_reset` et
   on réessaie.

Un tour interrompu laisse au pire des `*.matrice-sauve` : le tour suivant les rend d'abord.

## 3. Références d'image : où et comment

- **Fichiers hors dépôt** : `bench/matrice/ref/<jeu>-<mode>/` (vidage jusqu'à l'image retenue
  + `image.ppm` / `image.png`), 10 à 60 Mio par cellule. Ils ne vont pas dans git (bench/ est
  ignoré) : ce sont des données de jeux commerciaux et ils pèsent.
- **Empreinte versionnée** : `tools/matrice/references.csv` (cellule, numéro d'image, sha256
  de l'image, nombre et taille des fichiers, date, `validee`, note). Une référence altérée ou
  perdue se voit (empreinte), une validation est un commit.
- **Création** : sans référence, le tour en crée une avec l'image rejouée retenue ; la
  cellule reste rouge (« à valider ») tant qu'on ne l'a pas regardée : ouvrir
  `bench/matrice/ref/<cellule>/image.png`, puis `--valider <cellule>` et committer le CSV.
- **Renouveler** : supprimer le dossier et la ligne du CSV ; le tour suivant la recrée.

Les scènes ne sont pas toutes déterministes (le temps des jeux suit l'horloge) : la référence
n'est pas comparée à l'image du tour, mais **son propre vidage est rejoué** à chaque tour.
UT2004 est déterministe (pas de simulation fixé), les autres non.

## 4. Jeux, modes, scènes (`tools/matrice/jeux/*.py`, un module par jeu)

| Jeu | Fenêtre | Plein écran | Scène fixe, fenêtre de mesure |
|---|---|---|---|
| Marble Blast Gold | `-windowed` | `-fullscreen` | `-mission …/beginner/gems.mis` : bille immobile au départ, images 900..1500 |
| Zenerchi | défaut (800×600) | **non automatisé** | menu d'accueil animé, repéré sur la scène (15 dessins par image tenus 100 images) : M+200..M+1200 (images 1500..2500 jusqu'au 29/09, voir §7) |
| DOOM 3 Demo | `r_mode 3` (640×480) | `r_mode 5` (1024×768) | `+map game/demo_mars_city1`, joueur immobile après la cinématique : T+50..T+280 |
| Prey Demo | `r_mode 3` | `r_mode 5` | `+loadGame Auto___Fuite_____toute_vitesse` : L+300..L+600 après le chargement |
| UT2004 Demo | `StartupFullscreen=False` | `-fullscreen`, 1024×768 | caméra d'intro d'AS-Convoy, pas fixé à 0,2 s (`seed.c`), images 13..73 |
| Warcraft III | **non automatisé** | défaut (800×600, changement de mode) | menu principal, images 1200..1700 ; CD monté depuis `Warcraft III.toast` |
| Colin McRae 2005 | **non automatisé** (pas de mode fenêtre) | défaut (800×600, changement de mode) | départ d'ESP 1 Selardu en contre la montre, voiture arrêtée : 600 échanges réguliers après la touche COURSE (3 échanges par image du jeu) |
| Nexuiz 2.5.2 (DarkPlaces) | `+vid_fullscreen 0`, 800×600 | `+vid_fullscreen 1`, 1024×768 | `-benchmark demos/demo1` (timedemo, déterministe) : images 120..600, vidage à l'image fixe 870 |
| Nexuiz 2.5.2 (GLSL, `nxg`) | **non automatisé** | **non automatisé** | profil `+r_glsl 1` prévu : GLSL coupé par le jeu faute de `GL_ARB_fragment_shader` |
| RTCW | **non automatisé** | **non automatisé** | — |

Détails par jeu dans l'en-tête de chaque module. Points durs :

- **DOOM 3** : T = fin de la cinématique, règle de `meas-tcg.sh` / `d3win.py` **durcie** —
  trois tranches de 200 images stables (à 15 % près) au-dessus du seuil au lieu de deux. La
  règle d'origine a été trompée une fois par un passage lent de la cinématique. **Seuil
  relatif depuis le 26/09** : S = 0,8 × le niveau du jeu lu sur les 400 dernières images,
  évalué seulement à partir de l'image 5000 et T accepté 1000 images derrière la dernière (la
  règle tourne en direct). Le seuil fixe de 65 ms/image ne trouvait plus T avec
  `x-fp-inline` (jeu à ~63 ms/image : tour `20260926-1333` rouge, « scène non atteinte »).
  Le pic de la cinématique vaut ~0,5 × le niveau sur trois tranches, quelle que soit la
  vitesse ; `d3win.py` (hors ligne) retrouve avec S relatif le T de ses 49 parties rangées
  à 15 images près.
- **Marble Blast** : la démo de l'écran-titre (15 à 120 ms/image selon le passage) n'est pas
  une scène fixe ; `-mission` l'est.
- **Prey** : la sauvegarde « Fuite » ouvre sur une cinématique scriptée ; la scène avance
  avec l'horloge, l'image de preuve n'est donc pas la même d'un tour à l'autre. Fin du
  chargement = dernière image de plus d'une seconde, dès que 300 images la suivent (la
  première règle, « plus de 2 s puis 300 images de moins d'une seconde », a attendu 20 min en
  plein écran : le chargement finit par des images de 1,9-2 s).
- **Non automatisés** : Zenerchi en plein écran (case du menu Options, rangée dans
  `prefs.dat` chiffré ; ni Option+Entrée ni Cmd+F ne basculent) ; Warcraft III en fenêtre
  (aucun réglage connu, `LaunchCFMApp` ne passe pas d'arguments) ; Colin McRae en fenêtre
  (le dialogue d'options n'offre que résolution, couleurs, FSAA) ; RTCW (absent du disque
  quotidien).
- **Colin McRae** (27/09) : « Jouer » du dialogue d'options par osascript, puis écran titre
  attendu (capture à ≥ 50 teintes), puis **Entrées tenues par le moniteur** (`sendkey ret
  300` : l'Entrée d'osascript ne passe pas les menus) jusqu'à COURSE ; une Entrée perdue dans
  une transition est renvoyée (pas de pause de chargement 40 s après la dernière). ms/image
  = ms par échange × `echanges_par_image` (3 : plein écran + deux échanges de la cible
  cachée 800×600). La voiture change d'un tour à l'autre (tirage du jeu), pas le décor.
  Arrêt `sudo killall -9`, invité redémarré après.
- **Nexuiz** (27/09) : `-benchmark demos/demo1` rejoue la démo image par image (timedemo)
  puis quitte (~2017 échanges, ~190 s) ; les ~107 premiers échanges sont le chargement.
  L'image n du plugin est la **même image de la démo** d'un tour à l'autre et d'un mode à
  l'autre (sommets par image identiques sauf 85 images sur 2017, texte et particules ; image
  de preuve identique en fenêtre et en plein écran, chrono 0:38). D'où la fenêtre fixe
  120..600 et le vidage à l'image fixe 870 (`@870`), assez loin après 600 (~18 s) pour que
  la matrice soit dans son attente ; la capture tombe à l'image 872. 17 Mio par image
  vidée : 24 images. **Premier plan** : lancé par son exécutable (le script
  `nexuiz-osx-agl` fait `exec nexuiz-osx-agl-bin`, qui n'est pas le `CFBundleExecutable`
  du paquet) et non par LaunchServices, le jeu est un processus « background only »
  (`vid_agl.c` n'appelle pas `TransformProcessType`) : osascript ne le met jamais devant, le
  plugin voit « not frontmost » et replie chaque échange (Swap60 + le glFinish Swap5c). La
  matrice (et le lanceur `~/nexuiz.command`) préchargent
  `tools/guest/launchers/premierplan.c` (`TransformProcessType` + `SetFrontProcess` dans un
  constructeur), compilé dans l'invité. Profil `glsl` (`nxg`, `+r_glsl 1`) automatisé
  depuis le protocole v21 ; le profil ARB force `+r_glsl 0`.

Ajouter un jeu : un module `tools/matrice/jeux/<clé>.py` qui définit `JEU`, une instance de
`jeu.Jeu` (commande par mode, `fenetre(rows)`, réglages à sauvegarder, plancher), ou `JEUX`,
une liste d'instances (profils d'un même jeu, Nexuiz).

## 5. Pièges et limites

- **Vitesse bruitée** : la colonne `charge_hote` dit si un autre QEMU tournait (l'agent TCG
  sur sa copie) et la charge de l'hôte, relevées au début de la cellule ET après la fenêtre de
  mesure. Depuis le 29/09, « notre » QEMU est le détenteur de `tiger.lock` : une VM
  SNAPSHOT=1 d'un autre agent, qui démarre elle aussi sur `tiger.qcow2`, compte bien comme
  « autre ». La charge compte aussi ce qui n'est pas QEMU (Spotlight, tests d'un autre projet :
  charge 40-53 le 29/09, mesures inexploitables). Le 26/09, aucun autre QEMU pendant les
  tours. Les écarts de ±5 % entre deux parties restent la règle (`docs/tcg-g4.md` §14.7).
- **Le déclencheur ralentit le jeu** (§2) : jamais de ms/image prise pendant la preuve.
- **Mesure et rafales** (06/10/2026, `docs/tcg-g4.md` §31). Ce que la matrice fait dans
  l'invité pendant une partie se voit dans la vitesse. Mesuré dans la VM de dev (deux sondes
  de calcul, SMP=2) : un `osascript … System Events … set frontmost` coûte **3,1 s de
  processeur invité au premier appel** (System Events démarre), 1,0 s ensuite ; un `ssh` neuf
  (échange de clés DH et RSA d'OpenSSH 4.5) 2,2 s vu de l'hôte, ~0,2 s de processeur invité ;
  la même commande multiplexée 0,09 s et rien de mesurable. Sur UT2004 (fenêtre fixe 13..73,
  ~4 s de jeu), l'osascript du premier relevé qui voyait des images faisait un paquet de
  10-30 images à 100-190 ms qui tombait dans la fenêtre ou non selon la phase des relevés :
  +5 à +11 ms/image (« deux régimes » de `x-jit-rel32`, en fait de la mesure). D'où :
  - les jeux à fenêtre fixe la déclarent (`images_fenetre = (a, b)`, `jeu.py` ; UT2004, Marble
    Blast, Nexuiz, Warcraft III) et `plan_permis()` (`matrice.py`) refuse tout osascript
    quand l'image a est à moins de 6 s au rythme courant, jusqu'à l'image b ; DOOM 3, Prey,
    Zen, Colin McRae (fenêtres décidées en cours de partie, loin du début) gardent l'ancienne
    règle ;
  - System Events est lancé avant le jeu, et les deux remises au premier plan du début partent
    dès que le processus du jeu existe, pendant le chargement ; un appel qui échoue (processus
    pas encore connu de System Events) ne compte pas ;
  - `tools/guest/tssh.sh` multiplexe ses connexions (`ControlMaster`, socket
    `.run/tssh-<port>`, `ControlPersist` 300 s, `TSSH_PERSIST=` ; `TSSH_MUX=0` pour l'ancien
    comportement) : chaque relevé ne coûte plus d'échange de clés ;
  - relire une ancienne campagne `ut-fen` : `tools/tcg/utrafales.py <campagne>` (rapport de
    chaque image à la même image des autres parties, pas de simulation fixe ; excès dans la
    fenêtre ; instants des rafales).
- **La capture doit correspondre à une présentation terminée** : rendez-vous du plugin
  décrit en tête, confirmé par `frames.csv`. Un marqueur absent, expiré ou dont l'image
  n'existe pas dans le rejeu rend la cellule rouge. Les anciens tours lisaient les en-têtes
  des `*.bin` pendant que le jeu continuait ; `surfaces.txt` et la latence SSH pouvaient
  décaler leur capture. Ils restent analysables, mais ne disposent pas de cette preuve
  de synchronisation.
- **Reprendre un tour** : `--reprendre bench/matrice/<tour> -j … -m …` rejoue la sélection et
  garde les autres cellules du tour. Un tour interrompu laisse au pire un jeu en marche et des
  réglages sauvegardés : le tour suivant arrête le jeu (redémarre l'invité si c'est DOOM 3),
  rend les réglages et démonte le CD de Warcraft III avant de commencer.
- **Marble Blast en fenêtre** : utiliser les deux fichiers de préférences réglés par le
  module ; une fenêtre 1024×768 sur le bureau 1024×768 perd sa présentation directe.
- **Invité gelé** : quatre relevés ssh manqués de suite (~5 min) et la cellule est rouge
  (« l'invité ne répond plus ») ; autopsie d'abord (`gel/gel.ppm`, `gel/kpanic.txt` :
  registres symbolisés et texte de panique, depuis le 29/09), puis l'invité est relancé
  par `system_reset`, puis, si le
  démarrage reste bloqué, QEMU est arrêté et `./run_tiger.sh` relancé (détaché). Vu le 26/09 :
  un gel pendant le chargement de DOOM 3 (lancement de preuve, juste après un redémarrage de
  l'invité), vCPU 0 bouclant en `0x268b4` interruptions coupées, vCPU 1 en `0xaf6b4`, pas
  de `panic.log` ; deux `system_reset` de suite sont ensuite restés bloqués au démarrage
  (« using 1966 buffer headers… ») ; un QEMU relancé est reparti en 30 s. **Relu le 29/09**
  (`docs/gel-doom3-baddisplay.md`) : `0x268b4` = `_panic+0x254`, c'est une panique dont
  Tiger ne garde aucune trace, et `0xaf6b4` = `_machine_idle` ; au prochain cas, lancer
  `python3 tools/re/kpanic.py mach_kernel` AVANT le reset. Les resets bloqués venaient de
  l'OpenPIC de QEMU (interruption OHCI restée en attente), corrigé par
  `patches/openpic/0001`.
  Le 27/09 à 15:50, lancement de diagnostic UT2004 : panique `CPU 1`, code
  `0000000A (Lock timeout)`, PC `0x000AA010`, LR `0x00003820`, R1 `0xFE054021`.
  Pas de `/Library/Logs/panic.log` après reprise. `system_reset` est resté bloqué
  au même message de buffers ; relance de QEMU, vérification automatique du disque
  puis retour du bureau. Capture locale : `bench/utweapon-priorites/panic.png`.
  Le lancement suivant du jeu a réussi ; cause non établie, ne pas attribuer ce
  panic au défaut de texture sans preuve.
- **Écran de l'invité** : un Finder ouvert, Terminal, la souris de l'hôte au-dessus de la
  fenêtre QEMU (elle bouge le curseur de Tiger, et la caméra de certains jeux) : ne pas
  toucher à la fenêtre de la VM pendant un tour.
- **Rejoueur** : trois défauts de `tests/qgpu_replay.c` corrigés en route (26/09) — surface
  synthétique à la taille exacte présentée (une surface plus haute décalait tout : image noire
  de DOOM 3 en 640×480), relecture de la zone présentée (au lieu de 800×600 fixes),
  `TEX_CREATE` (v3) reconnu par le prologue et `TEX_DESTROY` d'une texture jamais vue
  (Marble Blast : 5 soumissions en erreur, texture blanche dans l'image).
- **Vidage autonome et rendu vers texture** (27/09, plugin `20260927-rtt`) : le vidage relit
  l'état qui n'existe que sur l'hôte (textures copiées, surfaces) et écrit `surfaces.txt`.
  Deux défauts vus au premier tour (`20260927-0236`, 10 rouges sur 11) : le rejeu reliait
  encore le contexte à une surface devinée malgré `surfaces.txt` (images unies) ; la
  relecture attendait l'échange suivant alors que DOOM 3 et Prey copient avant dans une
  texture hôte (`BAD_ARG`). Corrigés : tour `20260927-0307`, 10 vertes.
- **Surface lue ou écrite sans être liée dans le vidage** (07/10, DOOM 3 fenêtre, tour
  `20260926-2156` du M4) : le device n'exige aucune liaison pour `SURF_READBACK`/`UPLOAD`,
  `DEPTH_*`, `STENCIL_*` ni `SURF_TEX` (la surface existe dès son `SURF_CREATE`, une surface
  jamais dessinée se relit noire : `glClear` à 0 à la création) ; le prologue du rejoueur ne
  créait la surface qu'au `SURF_BIND` ou d'après la présentation → `NO_SURF` et fin de la
  soumission. C'est le cas des vidages **sans `surfaces.txt`** (avant le 27/09) : la
  relecture/réécriture d'état du déclenchement (`CTX_BIND` puis `SURF_READBACK`/`SURF_UPLOAD`
  de la surface du contexte) y tombait. Le prologue crée maintenant la surface (assez grande
  pour le rectangle) sans la lier, et un tel transfert juste après un `CTX_BIND` relie ce
  contexte à cette surface quand sa liaison était inconnue ou devinée (le plugin n'en émet que
  sur la surface du contexte qu'il vient de lier). Avec `surfaces.txt`, rien ne change : toutes
  les surfaces vivantes sont déjà créées, un identifiant inconnu n'existe pas non plus sur le
  device et le `NO_SURF` reste le sien. Épreuves : vidage synthétique (relecture de la surface 3
  avant tout `SURF_BIND`, renvoyée dans la surface présentée) — avant 1 soumission en erreur et
  carré blanc, après 0 et carré noir comme le device, backends gl et logiciel ; DOOM 3 fenêtre
  de la matrice du PC du 06-07/10 privé de son `surfaces.txt` — avant 2 `NO_SURF` (soumissions
  `000020`/`000021`, image 5077), après 0 et les 20 images identiques à l'octet au rejeu
  avec `surfaces.txt` ; les huit rejeux de `bench/tcg/ab/x86-final/{ref,final}-1/` (mb, zen,
  ut, d3 en fenêtre, avec `surfaces.txt`) inchangés à l'octet. Le plafond de 60 mots du
  prologue pour `CTX_CREATE` et `SURF_CREATE` sur `SURF_BIND` (reste d'un `pre[64]`, au-delà
  la surface n'était plus créée) suit celui des autres créations (4090).

## 6 quater. La matrice du PC Linux (nuit du 02 au 03/10/2026)

PC Linux (i7-10700F, RTX 4060 Ti, pilote NVIDIA 595.91.07), VM quotidienne `disks/tiger.qcow2`
(**Tiger 10.4.6**, SMP 2, fenêtre QEMU native), QEMU de référence 11.1.2 avec toute la série
(tcg/0017-0019, usbhid/0001, `QGPU_CAP_FIXED4`), backend GL sur le GPU de l'hôte, paquet invité
du 02/10 (plugin `FIXED4`, kexts POMPPCGPU et POMPPCFsqrt). Hôte au repos (0 autre QEMU).

**Ce que ce PC a** : Marble Blast, Zenerchi, UT2004. Les autres jeux (DOOM 3, Prey, Colin
McRae, Warcraft III, Nexuiz, RTCW) ne sont que sur le M4 : cinq cellules sur seize.

**Portage** : rejoueur compilé contre EGL (`GL_LIBS`), PNG par ImageMagick, charge par
`/proc/loadavg` ; `tools/tcg/matab.sh` tire le dépôt de git (il avait le chemin du M4 en dur).
**Références par hôte** : une image rendue par NVIDIA n'est pas celle d'Apple ; sous Linux, le
manifeste est `tools/matrice/references-linux.csv` et les fichiers `bench/matrice/ref-linux/`
(le premier tour avait réécrit l'entrée `mb-fen` du manifeste du M4, remise en état). **Planchers
par hôte** : `plancher_ms_linux`, même règle (1,25 × le temps typique ; le pire observé pour
UT2004, très dispersé). Revus le 07/10 sur le binaire rapide et les défauts du 06/10
(`docs/vitesse-doom3-x86.md` §13.5-13.7, campagnes `x86-fast` et `x86-final`, hôte au repos) :
**DOOM 3 140** (il n'en avait pas et prenait les 76 du M4 ; 4 parties, 108,9-112,9 ms/image),
**Marble Blast 19** (26 avant ; 1 partie, 14,9), **Zenerchi 8** (10 avant ; 1 partie, 5,9,
arrondi au-dessus), **UT2004 104 inchangé** (1 partie à 48,7 ; 1,25 × 48,7 = 61 mettrait en
rouge les tours en fin de session ou sur le binaire de référence, 54-83 observés : à reprendre
sur au moins trois tours complets du binaire rapide). DOOM 3 reste rouge tant que sa référence
n'est pas validée à l'œil (`matrice.py --valider d3-fen`). Le premier passage sur
la VM a demandé : la clé de la matrice (`.run/cmr/id_rsa`, déposée par mot de passe ; `expect`
manque ici), `~/matrice/ut-seed.dylib` compilé dans la VM de dev (pas de gcc dans la VM
quotidienne).

| tour | Marble Blast fen / pe | Zenerchi fen | UT2004 fen / pe |
|---|---|---|---|
| `20261003-0003` (références créées) | — (jeu non lancé, aléa) / 19,8 | 7,9 | 60,5 / 74,7 |
| `20261003-0018` (références validées) | 20,8 / 20,0 | 7,9 | 82,8 / 83,4 |
| `20261003-0157` (planchers du PC) | **vert** 22,0 / **vert** 21,0 | **vert** 7,8 | **vert** 82,7 / rouge (image) 59,3 |

Images justes partout où il y a une image (rejeu = VM et référence à 0,00), sauf une fois :

- **UT2004 plein écran, « rejeu ≠ VM »** (`20261003-0157/ut-pe`) : la capture de la VM (image
  130, confirmée par `frames.csv`) montre une surface irisée en gros plan ; **aucune** des 60
  images rejouées du vidage n'en approche (écart moyen minimal 32). Ce n'est donc pas une
  capture prise hors du vidage : l'écran vivant a montré ce que le flux, rejoué depuis l'état
  vidé, ne produit pas. Rejouée quatre fois : verte 4 sur 4 (1 sur 7 cette nuit). Le M4 l'a vu
  aussi (`docs/protocole-v23-etat.md` §4) ; cause non trouvée.

Vitesse : UT2004 est limité par l'émulation (`sample` dans l'invité : le fil principal calcule
— moteur, particules —, la présentation pèse 4 % ; côté hôte, les deux vCPU à 96 %, le rendu GL
à 15 %) ; d'où le facteur ~2 contre le M4, dont un cœur va bien plus vite. Sa dispersion est
réelle et s'explique : sur un QEMU tout juste relancé et sans vidage (A/B), 58,5 à 63,3 ms/image ;
dans une session de 35 min déjà passée par d'autres jeux, 65,8 à 67,7 sans vidage et **70,1 à
77,9 avec** (même session, trois parties chacun) ; les tours complets, avec vidage et en fin de
session, vont de 59 à 83. Le mode vidage coûte ~10 % à UT2004 (~0,4 ms à Zenerchi, §5). Un gel au démarrage (deux vCPU au même
PC du noyau, écran noir) a été levé par un `system_reset` (procédure de la matrice).

A/B en jeu de tcg/0017-0019 (avant/après) : `docs/tcg-g4.md` §25.6.

## 6 ter. Le tour du 27/09/2026 après midi (Nexuiz porté)

Tour `bench/matrice/20260927-1018` (plugin `20260927-dumpat` = `20260927-rtt` + déclencheur
`@n`, QEMU v20, un lancement par cellule, aucun autre QEMU, 36 min) : **12 vertes sur 13
automatisées**, rouge Marble Blast en fenêtre (connu). ms/image : Marble Blast plein écran
10,3 ; Zenerchi 4,5 ; DOOM 3 60,4 / 59,5 ; Prey 68,9 / 68,7 ; UT2004 26,1 / 26,0 ;
Warcraft III 17,3 ; Colin McRae 72,2 ; **Nexuiz 107,1 / 107,4** (demo1, images 120..600 ;
10 replis en fenêtre pour 22 admis, 0 en plein écran). Rejeu = VM et rejeu de la référence :
0,00 / 0,00 % partout. Références Nexuiz créées au tour `20260927-1008` (image 872, la même
démo image dans les deux modes) et validées à l'œil. Vidage Nexuiz : ~240 Mio par cellule
(24 images), référence ~80 Mio.

Le premier essai de Nexuiz (27/09 matin, lanceur `~/nexuiz.command`) repliait deux échanges
par image en fenêtre : processus « background only », jamais au premier plan (§4). Avec
`premierplan.dylib` : 0 repli hors rafraîchissements, `demo1` 8,9 → 10,1 img/s.

## 6 bis. Le tour du 27/09/2026 (Colin McRae porté)

Tour `bench/matrice/20260927-0307` (plugin `20260927-rtt`, QEMU v20, un lancement par
cellule, aucun autre QEMU, 29 min) : **10 vertes sur 11 automatisées**, rouge Marble Blast
en fenêtre (connu). ms/image : Marble Blast plein écran 10,2 ; Zenerchi 4,4 ; DOOM 3 60,3 /
60,5 ; Prey 69,9 / 70,1 ; UT2004 25,8 / 26,3 ; Warcraft III 17,3 ; **Colin McRae 72,6**
(plein écran, par image du jeu). Rejeu = VM et rejeu de la référence : 0,00 / 0,00 %
partout.

## 6. Le tour du 26/09/2026

Tour `bench/matrice/20260926-0923` (commit `c3a7317` + corrections de ce jour), deux
lancements par cellule, aucun autre QEMU sur l'hôte pendant le tour. Il a été interrompu une
fois par le gel de l'invité (§5) et complété par `--reprendre` ; durée cumulée des cellules
**48 min** (DOOM 3 : 10 min par mode, quatre lancements et quatre redémarrages), analyse
comprise ~50 min. Sorties : ~720 Mio par tour (vidages), références 263 Mio.

| Jeu | Mode | Verdict | Image (rejeu/VM ; réf.) | Replis (admis) | ms/image (plancher) |
|---|---|---|---|---|---|
| Marble Blast Gold | fenêtre | **rouge** | non prouvée (aucun `SURF_PRESENT`) | 4188 (48) | 37,8 (16) |
| Marble Blast Gold | plein écran | **vert** | 0,00 / 0,00 % ; 0,00 / 0,00 % | 0 | 12,4 (16) |
| Zenerchi | fenêtre | **vert** | 0,00 / 0,00 % ; 0,00 / 0,00 % | 97 (196) | 5,2 (7) |
| Zenerchi | plein écran | non automatisé | réglage dans `prefs.dat` chiffré | — | — |
| DOOM 3 Demo | fenêtre | **vert** | 0,00 / 0,00 % ; 0,00 / 0,00 % | 13 (30) | 78,1 (100) |
| DOOM 3 Demo | plein écran | **vert** | 0,00 / 0,00 % ; 0,00 / 0,00 % | 0 | 73,8 (100) |
| Prey Demo | fenêtre | **vert** | 0,00 / 0,00 % ; 0,00 / 0,00 % | 12 (28) | 69,6 (90) |
| Prey Demo | plein écran | **vert** | 0,00 / 0,00 % ; 0,00 / 0,00 % | 0 | 69,8 (90) |
| UT2004 Demo | fenêtre | **vert** | 0,00 / 0,00 % ; 0,00 / 0,00 % | 15 (34) | 29,5 (38) |
| UT2004 Demo | plein écran | **vert** | 0,00 / 0,00 % ; 0,00 / 0,00 % | 0 | 29,0 (38) |
| Warcraft III | fenêtre | non automatisé | pas de réglage de fenêtre | — | — |
| Warcraft III | plein écran | **vert** | 0,00 / 0,00 % ; 0,00 / 0,00 % | 0 | 19,3 (25) |
| Colin McRae 2005 | fenêtre | non automatisé | pas de mode fenêtre | — | — |
| Colin McRae 2005 | plein écran | **vert** (27/09, tour `20260927-0228` + validation) | 0,00 / 0,00 % | 0 | 69,1 (90) |
| RTCW | les deux | non automatisé | absent du disque quotidien | — | — |

**9 cellules vertes sur 11 automatisées**, 1 rouge (Marble Blast en fenêtre), 6 non
automatisées. Les planchers sont ~1,3 × ces mesures : ils gardent contre une régression, ils
ne jugent pas la vitesse absolue.

## 7. Défauts trouvés en route

- **Le déclencheur de vidage ralentit les jeux** (plugin) : un `access()` par dessin texturé
  tant que le fichier n'existe pas ; DOOM 3 77 → 139 ms/image, Marble Blast 12 → 36,
  Warcraft III 19 → 44. **Corrigé le 26/09** (`dump_trigger`, une fois par image) : un
  lancement par cellule.
- **`frames.csv` n'est écrit que toutes les 5 s** (plugin) : inutilisable comme horloge
  fine ; la matrice lit les en-têtes du vidage. **Plugin corrigé le 26/09**
  (`20260926-memo` : vidé à chaque image quand `POMPPC_GL_DUMP_TRIGGER` est posé) ; la
  matrice l'utilise désormais avec le rendez-vous de présentation décrit ci-dessus.
- **Marble Blast en fenêtre** : fenêtre de 1024×768 quelle que soit la résolution demandée,
  recouverte par la barre de menus, donc sans présentation directe : deux replis par image
  (Swap60, Swap58), ~38 ms/image au lieu de 12 en plein écran. Corrigé le 27/09 en
  réglant les deux exports de préférences (voir en tête).
- **Gel de l'invité au chargement de DOOM 3** (1 lancement sur ~12), puis démarrages bloqués
  après `system_reset` : seul un QEMU relancé repart. TODO §5.
- **Rejoueur** (`tests/qgpu_replay.c`), corrigé : surface synthétique à la taille présentée,
  relecture de la zone présentée, `TEX_CREATE` v3 et `TEX_DESTROY` dans le prologue.
- **Le texte des menus de Warcraft III** est juste le 26/09 (image validée) : la ligne du
  TODO §6 date d'avant les corrections du chemin tableaux.
- **Fausse régression de Zenerchi (29-30/09)** : 4,4 ms/image au 27/09, 6,0 au tour
  `20260929-1234`, 7,9 au tour `20260929-2344`, lue comme un coût fixe ajouté par les bug
  hunts. Ce n'en était pas un : la fenêtre FIXE 1500..2500 tombait sur le chargement du menu.
  L'écran de l'éditeur dure un temps, pas un nombre d'images, et tourne selon le lancement à
  ~3,4 ms/image (menu vers l'image 1650) ou ~15 ms/image (menu vers l'image 490) ; au 27/09
  les premières images, plus lentes (relectures et replis corrigés depuis), mettaient le
  menu vers l'image 1250, juste avant la fenêtre. Les deux régimes se voient avec la même
  pile (8af9efc comme HEAD). Bissection (30/09, hôte au repos, charge 1,4-2,1, médiane de
  blocs de 200 images au menu, `--sans-vidage` sauf mention) :

  | Pile (QEMU / kext / plugin) | Zenerchi fenêtre 1500..2500 | Zenerchi au menu | Marble Blast plein écran 900..1500 |
  |---|---|---|---|
  | référence b9004cc / HEAD / HEAD | 3,9-4,1 (5 passes) | 4,01-4,07 | 9,4 / 9,7 / 10,2 |
  | idem, avec vidage (tour normal) | 7,7 / 7,8 | 4,32 / 4,38 | — |
  | `*.avant-bughunt` / HEAD / HEAD | 3,9 / 4,0 / 4,0 | 3,95-4,02 | 9,8 / 9,9 |
  | `*.avant-bughunt` / HEAD / 8af9efc | 3,9 / 3,9 / 3,9 | 3,94-4,00 | — |
  | idem, avec vidage | **4,3 / 7,7** | 4,25 / 4,25 | — |
  | `*.avant-bughunt` / 8af9efc / 8af9efc | 4,0 / 4,0 / 4,0 | 4,01-4,04 | 9,5 / 9,7 |

  Aucune couche ne coûte (écarts ≤ 2 %, dans le bruit) ; le tour du 29/09 midi (6,0) avait
  une charge hôte de 5,3 et Marble Blast à 20 ms/image. Marble Blast plein écran va de 9,3 à
  10,3 d'un tour à l'autre depuis le 26/09 : son 10,2 n'est pas un écart. Corrigé dans
  `jeux/zen.py` (fenêtre repérée sur la scène, plancher 7 → 6) : 4,5 / 4,4 / 4,4 ms/image
  (tours `zenreg-corr-*`) ; tour complet `20260930-0128` : 16 vertes sur 16. Le mode vidage (un `access()` et un `fflush` par image) coûte
  ~0,4 ms/image à Zenerchi : les vitesses de la matrice l'incluent.
- **Anciennes vitesses du TODO §1** : « Marble Blast ~88 img/s », « Zenerchi ~50 img/s »
  venaient d'autres scènes (bureau du disque de dev, démo) ; à scène fixe : Marble Blast
  12,4 ms/image (~80 img/s) en plein écran, Zenerchi 5,2 ms/image au menu.
