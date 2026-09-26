# Matrice de jeux automatisée (chantier A3)

`tools/matrice/` joue chaque jeu de la matrice (TODO §1) dans chaque mode — **fenêtre** et
**plein écran** — sur la VM quotidienne, et produit un tableau vert/rouge avec, pour chaque
cellule, ses preuves rangées. C'est le harnais qui doit autoriser A2 (découpage du plugin) et
A4 (travail déplacé vers l'hôte) sans peur : on le rejoue avant et après.

```sh
tools/matrice/matrice.py                     # tout : jeux automatisés, deux modes (~30 min)
tools/matrice/matrice.py -j mb,zen -m fen    # un sous-ensemble (jeux, modes)
tools/matrice/matrice.py --deux-passes       # mesure puis preuve séparées (plugin d'avant le 26/09 après midi)
tools/matrice/matrice.py --sans-vidage       # vitesse et replis seuls
tools/matrice/matrice.py --liste             # jeux et modes connus
tools/matrice/matrice.py --valider d3-pe     # référence d'image vue et validée à l'œil
tools/matrice/matrice.py --analyse bench/matrice/<tour>   # refaire l'analyse d'un tour rangé
tools/matrice/matrice.py --reprendre bench/matrice/<tour> -j ut -m pe   # rejouer des cellules
python3 tests/matrice_test.py                 # règles de scène et comparateur, hors VM
```

Prérequis : la VM quotidienne lancée (`./run_tiger.sh`), ssh (`tools/guest/tssh.sh uptime`),
moniteur `.run/mon.sock`, personne d'autre sur la VM. Sorties dans
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
   reste derrière et chaque échange se replie, `Swap60`) ;
4. la **règle de scène** du jeu (§4) dit quand la fenêtre de mesure est passée ;
5. preuve seulement : `touch /tmp/matrice-go`, puis on attend que le vidage couvre deux
   images (numéro d'image lu dans l'en-tête du premier et du dernier fichier, `od` dans
   l'invité) et la VM est arrêtée aussitôt pour la capture ; on attend enfin que le vidage ne
   grossisse plus. `frames.csv` ne peut pas servir d'horloge : le plugin ne le vide que
   toutes les 5 s (la première version attendait « 3 images dans frames.csv » et capturait
   après la fin du vidage de Marble Blast et d'UT2004) ;
6. arrêt du jeu (`kill`), fermeture de Terminal une fois `lance.command` fini (sinon dialogue),
   réglages rendus, nettoyage du jeu (CD démonté) ;
7. rapatriement du dossier de l'invité (tar par ssh), puis effacement dans l'invité ;
8. DOOM 3 : **redémarrage de l'invité** (après un kill, tout relancement échoue en
   `kCGLBadDisplay`, TODO §5) ; panique AppleUSBOHCI au démarrage : `system_reset` et on
   réessaie.

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
| Zenerchi | défaut (800×600) | **non automatisé** | menu d'accueil animé, images 1500..2500 |
| DOOM 3 Demo | `r_mode 3` (640×480) | `r_mode 5` (1024×768) | `+map game/demo_mars_city1`, joueur immobile après la cinématique : T+50..T+280 |
| Prey Demo | `r_mode 3` | `r_mode 5` | `+loadGame Auto___Fuite_____toute_vitesse` : L+300..L+600 après le chargement |
| UT2004 Demo | `StartupFullscreen=False` | `-fullscreen`, 1024×768 | caméra d'intro d'AS-Convoy, pas fixé à 0,2 s (`seed.c`), images 13..73 |
| Warcraft III | **non automatisé** | défaut (800×600, changement de mode) | menu principal, images 1200..1700 ; CD monté depuis `Warcraft III.toast` |
| Colin McRae 2005 | **non automatisé** | **non automatisé** | — |
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
  (aucun réglage connu, `LaunchCFMApp` ne passe pas d'arguments) ; Colin McRae (course
  atteinte au clavier, arrêt par le stub GDB de QEMU : `tools/guest/cycle.sh`,
  `killgame.py`, pas encore porté) ; RTCW (absent du disque quotidien).

Ajouter un jeu : un module `tools/matrice/jeux/<clé>.py` qui définit `JEU`, une instance de
`jeu.Jeu` (commande par mode, `fenetre(rows)`, réglages à sauvegarder, plancher).

## 5. Pièges et limites

- **Vitesse bruitée** : la colonne `charge_hote` dit si un autre QEMU tournait (l'agent TCG
  sur sa copie). Le 26/09, aucun autre QEMU pendant les tours. Les écarts de ±5 % entre deux
  parties restent la règle (`docs/tcg-g4.md` §14.7).
- **Le déclencheur ralentit le jeu** (§2) : jamais de ms/image prise pendant la preuve.
- **La capture doit tomber dans le vidage** : le vidage dure `dump_images` images (20 à 120) ;
  la VM est arrêtée dès que le vidage couvre deux images, ~0,5 s plus tard. Si elle sort quand
  même du vidage : « rejeu ≠ VM … capture hors du vidage ? ». C'est un échec de la preuve, pas
  forcément de l'image : relancer la cellule (`--reprendre <tour> -j <jeu> -m <mode>`).
- **Reprendre un tour** : `--reprendre bench/matrice/<tour> -j … -m …` rejoue la sélection et
  garde les autres cellules du tour. Un tour interrompu laisse au pire un jeu en marche et des
  réglages sauvegardés : le tour suivant arrête le jeu (redémarre l'invité si c'est DOOM 3),
  rend les réglages et démonte le CD de Warcraft III avant de commencer.
- **Marble Blast en fenêtre** : le vidage n'a pas de `SURF_PRESENT` (chaque échange est replié
  vers Apple), rien à rejouer en image : la cellule est rouge par ses replis.
- **Invité gelé** : quatre relevés ssh manqués de suite (~5 min) et la cellule est rouge
  (« l'invité ne répond plus »), l'invité est relancé par `system_reset`, puis, si le
  démarrage reste bloqué, QEMU est arrêté et `./run_tiger.sh` relancé (détaché). Vu le 26/09 :
  un gel pendant le chargement de DOOM 3 (lancement de preuve, juste après un redémarrage de
  l'invité), vCPU 0 bouclant en `0x268b4` interruptions coupées, vCPU 1 en `0xaf6b4`, pas
  de `panic.log` ; deux `system_reset` de suite sont ensuite restés bloqués au démarrage
  (« using 1966 buffer headers… ») ; un QEMU relancé est reparti en 30 s. Noté au TODO §5.
- **Écran de l'invité** : un Finder ouvert, Terminal, la souris de l'hôte au-dessus de la
  fenêtre QEMU (elle bouge le curseur de Tiger, et la caméra de certains jeux) : ne pas
  toucher à la fenêtre de la VM pendant un tour.
- **Rejoueur** : trois défauts de `tests/qgpu_replay.c` corrigés en route (26/09) — surface
  synthétique à la taille exacte présentée (une surface plus haute décalait tout : image noire
  de DOOM 3 en 640×480), relecture de la zone présentée (au lieu de 800×600 fixes),
  `TEX_CREATE` (v3) reconnu par le prologue et `TEX_DESTROY` d'une texture jamais vue
  (Marble Blast : 5 soumissions en erreur, texture blanche dans l'image).

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
| Colin McRae 2005 | les deux | non automatisé | pas encore porté (§4) | — | — |
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
  fine ; la matrice lit les en-têtes du vidage. TODO §2.
- **Marble Blast en fenêtre** : fenêtre de 1024×768 quelle que soit la résolution demandée,
  recouverte par la barre de menus, donc sans présentation directe : deux replis par image
  (Swap60, Swap58), ~38 ms/image au lieu de 12 en plein écran. TODO §6.
- **Gel de l'invité au chargement de DOOM 3** (1 lancement sur ~12), puis démarrages bloqués
  après `system_reset` : seul un QEMU relancé repart. TODO §5.
- **Rejoueur** (`tests/qgpu_replay.c`), corrigé : surface synthétique à la taille présentée,
  relecture de la zone présentée, `TEX_CREATE` v3 et `TEX_DESTROY` dans le prologue.
- **Le texte des menus de Warcraft III** est juste le 26/09 (image validée) : la ligne du
  TODO §6 date d'avant les corrections du chemin tableaux.
- **Anciennes vitesses du TODO §1** : « Marble Blast ~88 img/s », « Zenerchi ~50 img/s »
  venaient d'autres scènes (bureau du disque de dev, démo) ; à scène fixe : Marble Blast
  12,4 ms/image (~80 img/s) en plein écran, Zenerchi 5,2 ms/image au menu.
