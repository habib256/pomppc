# État des deux hôtes : PC x86-64 et Mac M4

L'utilisateur travaille **alternativement** sur deux machines, avec le même dépôt GitHub
(`origin/main`). Une séance d'agent se déroule sur **une seule** de ces machines. Avant d'agir,
l'agent doit savoir sur laquelle il se trouve et où en est le travail sur chacune. Cette page
donne cet état. **La mettre à jour à la fin de chaque séance**, dans le même commit que le
travail, avec la date et l'heure.

## 0. Sur quel hôte suis-je ?

| `uname -sm` | hôte | binaire QEMU quotidien | défauts de `run_tiger.sh` |
|---|---|---|---|
| `Linux x86_64` | **PC** (Ubuntu 26.04, Core i7-10700F, RTX 4060 Ti) | `~/src/qemu/build-fast` (PGO), référence `~/src/qemu/build` | branche `Linux:x86_64` |
| `Darwin arm64` | **M4** (macOS, GL d'Apple sur Metal) | `~/src/qemu` (reconstruit par `scripts/build_qemu_qfb.sh`) | branche `Darwin:arm64` |

Au début d'une séance :
1. `git pull` : l'autre hôte a peut-être poussé.
2. Lire la section de **cet** hôte ci-dessous, puis « À faire en passant d'un hôte à l'autre ».
3. Si `patches/` ou `patches/qgpu/` ont changé depuis la dernière construction locale,
   reconstruire QEMU. Sinon, on mesure un binaire qui ne correspond pas au dépôt.

## 1. Règles communes

- **Parité** : toute accélération prouvée sur un hôte reçoit son pendant sur l'autre, **au même
  niveau de preuve**, avant d'y être allumée. Si elle ne peut pas l'être, la raison est écrite
  dans la doc du patch.
- **Vitesse générique** : un levier se juge sur **toute la matrice** (mb, zen, ut, d3, cmr),
  jamais sur DOOM 3 seul.
- **A/B seulement l'hôte au repos** : aucune construction, aucun autre QEMU, aucun agent
  parallèle pendant la mesure (`tools/tcg/chargehote.py attendre`).
- **Ne jamais supprimer `.run/tiger.lock`** (flock : deux QEMU écriraient le même disque).
- Le PC n'est **pas publié** (paquets, annonces) tant que sa vitesse n'est pas acceptable.
  Cible : DOOM 3 à 50 ms/image en fenêtre.
- Les commits vont sur `main` puis sont poussés. Un essai non retenu est rangé sous une
  étiquette `archive/*`.

## 2. PC x86-64 : état au 31/10/2026, 5 h 30

**Défauts du PC** : Quartz Extreme (depuis le 30/10 au soir, `QE=0` l'éteint) et **zone froide
`x-tcg-cold`** (tcg/0056, depuis le 31/10, `TCGCOLD=0` l'éteint).

**Fait dans la nuit du 30 au 31/10** (`docs/vitesse-doom3-x86.md` §23, `docs/endurance.md` §15)
- Point 1, `matab.sh --restore` sur `build/` le 28/10 : `chaine4.sh` avait été tué pendant la
  partie `ref-3`, lancée avec `QEMU_BIN=~/src/qemu-ceil/build`. Cette VM est restée allumée, et
  le `--restore` final n'a jamais tourné. Le choix du binaire est juste. `--restore` retire
  maintenant les variables de binaire héritées et affiche le binaire lancé (`6510319`).
- Point 2, chasse au gel de cmr : 6 parties par bras sur `build-fast/`, aucun gel (cold 43,4
  contre ref 44,3 ms/image). A/B de la matrice : d3 −1,8 %, mb −1,2 %, zen −2,2 %, cmr −2,2 %,
  ut neutre. **Allumé par défaut.** Avec la zone froide, cmr compte 20 parties pilotées et 1 gel
  (le 28/10), aucun dans les 17 suivantes. Endurance : 10 démarrages et 8 cycles de jeu, 0
  incident.
- Point 3, profil croisé des 5 jeux (§23). Postes communs : la **recherche de blocs** (`qht`,
  `lookup_tb_ptr` : 10 % mb, 13,7 % zen, 6,1 % cmr, 3-4 % d3/ut) et la **base de temps** (`mftb`
  13,8 % sur ut, 1-3 % ailleurs).
- Point 4 : `plancher_ms_linux` de cmr fixé à 60 (binaire rapide 44,5-47,5 ×1,25).
- Point 7 : profil PGO de `build-fast/` réentraîné le 31/10 (cible ppc32mt, zone froide, 1 255
  `.gcda`, construction à 2 h 43). Ancien binaire gardé dans `~/src/qemu/build-fast-pgo2810/`
  (à effacer une fois l'A/B jugé). A/B neuf contre ancien, au repos : mb, zen, ut à −0,6 à
  −1,4 % ; cmr neutre (+0,3 %) ; d3 +1,0 % (80,4 contre 79,6, 4
  parties par bras, dans la dispersion). Neutre en tout : le nouveau profil est gardé (il suit la
  série et les défauts du jour).
- Fin de séance : VM quotidienne relancée par `matab.sh --restore` sur le binaire rapide (QE et
  zone froide).

**Vitesse** : DOOM 3 environ 77-80 ms/image sur le binaire rapide, Colin McRae environ 43,5
ms/image en plein écran. Journal : `docs/vitesse-doom3-x86.md`.

**À faire sur le PC, dans l'ordre**
1. Recherche de blocs (§23) : mesurer le taux d'échec du cache de sauts sous mb et zen, puis
   essayer un cache plus grand ou mieux indexé devant `qht_lookup_custom`. C'est le seul poste
   commun aux 5 jeux.
2. Base de temps en ligne (`mftb` sans helper) : 13,8 % du vCPU sur UT2004.
3. `do_invalidate_BAT` (2,4 % sur cmr) : comprendre pourquoi l'invité réécrit ses BAT si souvent.
4. Effacer `~/src/qemu/build-fast-pgo2810/` une fois l'A/B du profil jugé.

**Propres au PC** : `patches/diag/` (auto-profileur, plafonds), `tools/tcg/perfhw.sh` (compteurs
Intel), et l'arbre de mesure `~/src/qemu-ceil` (série et diag, -O2, sans PGO). `perf` exige
`perf_event_paranoid` ≤ 1, que l'utilisateur règle à la main ; la valeur revient à 4 à chaque
redémarrage.

## 3. Mac M4 : état au 30/10/2026, 11 h 30

**Fait le 30/10**
- **Émulateurs voisins, A/B au repos** (`docs/tcg-g4.md` §42, 4 parties par bras, mb zen ut
  d3 en fenêtre). `tcg/0057` `x-vcpu-qos` (`VCPUQOS=1`, fils des vCPU en QoS interactive) :
  **neutre**. `tcg/0058` `POMPPC_POLL_NS` (`POLLNS=1`, attente de la boucle principale à la
  microseconde par `select`) : zen **−18 %** mais ut **+6,6 %**. Les deux sont **éteints**
  par défaut, compilés dans `~/src/qemu`. Le PC n'est pas concerné : le code est propre à
  macOS.
- **Bug QE corrigé : bande blanche, noire ou « ancien jeu » sous l'ombre d'une fenêtre posée
  sur un jeu en fenêtre** (signalé par l'utilisateur). Dans le WindowServer, le chemin
  rapide des clés d'état liait la texture de surface vide au lieu de la texture d'export du
  jeu. Correctif : `tex_bind_id` dans le plugin, sans changer le protocole, ni QEMU, ni le
  kext. Vérifié en VM jetable sur Zenerchi, Marble Blast et DOOM 3 ; matrice mb/zen/d3 en
  fenêtre dans `bench/matrice/qe-ombre-3010`. Coins noirs de DOOM 3 et Prey : la fenêtre
  est `NSBackingStoreRetained` (relevé sous gdb), comme sur un vrai Mac ; pas un bug.
  **Le plugin corrigé est installé dans la VM quotidienne** (voir plus bas).
- **Fin de séance, 11 h 30** : la VM jetable est arrêtée et `git` est propre. La VM
  quotidienne a été relancée par `matab.sh --restore`, puis s'est fermée ; aucune VM ne tourne.
  Elle repart avec `./run_tiger.sh`. La suite est préparée pour le PC (section 2, « En arrivant
  sur le PC »).

## 3 bis. Mac M4 : état au 29/10/2026, 21 h 30

**Défauts sur Darwin:arm64** : depuis le 26/10 (`550b2a8`), la série du PC est allumée et
prouvée : `PPC32MT TLBPRECISE LMWINLINE DCBZINLINE FPNATIVECMP VFPNATIVECMP DIRTYSKIP PTEGMAP
JC2WAY FPKNOWN FPRFLAZY STFSKNOWN LFSKNOWN TLBBASE`. A/B : DOOM 3 −23,9 %, UT2004 −18,6 %,
Marble Blast −22,7 %. Garde : un binaire sans tcg/0037-0039 les laisse éteints.

**Fait le 28/10**
- `~/src/qemu` reconstruit (ALIAS de cmr, tcg/0056 compilé, `x-tcg-cold` sans effet sur
  arm64).
- **Cellule cmr-pe rejouée** : le pilotage du PC marche sur le M4 (course mesurée,
  43,7 ms/image pour un plancher de 88). Elle était d'abord **rouge** : sous ImGuiDock, le jeu
  prend 1920×1080, et le vidage autonome ne relisait pas une cible de 7,9 Mio, plus grande que
  l'arène de transfert de l'invité. La relecture était sautée sans un mot, et le reflet de la
  carrosserie manquait au rejeu.
  **Corrigé** : relecture par bandes de lignes. Côté protocole, `QGPU_CAP_TEX_READBACK_ROWS`
  (0x00100000) donne à TEX_READBACK une longueur 8 avec la fenêtre `y0, lignes`, sans changer
  `QGPU_PROTO_VERSION`. Côté plugin, les surfaces passent par SURF_READBACK/UPLOAD en bandes,
  les textures par TEX_IMAGE3 sans données puis des bandes de TEX_SUBIMAGE. Tour
  `20261028-1848` : **vert**, image 0,00/0,00 %.
- « 2/3 textures relues » expliqué : la texture 120 n'a **aucune image sur l'hôte** (en-tête
  0×0, jamais liée ni copiée dans le vidage). La note la compte maintenant à part : « 2/2 (1
  sans image) ». Un refus du device se distingue d'un niveau vide par un en-tête prérempli à
  0xFF. L'avertissement du rejeu sur la texture 54 vise la 1ʳᵉ image du vidage : elle commence
  au milieu d'une image et précède le renvoi de la texture (limite connue depuis le 27/09). La
  matrice compare une image plus loin. Tour `20261028-2113` : vert, 43,6 ms/image.
- **Matrice entière `20261028-1854` (commit `403593e`) : 17 cellules vertes sur 17**, dont
  Warcraft III, rouge le 26/10. Les 4 autres cellules ne sont pas automatisées. Toutes vont plus
  vite que le 26/10 : mb −14 à −16 %, d3 −10 %, prey −6 à −20 %, ut −2 à −5 %, nx −9 %. Seul
  cmr va moins vite (+11 %), mais il est passé de 1024×768 à 1920×1080 et la comparaison ne
  vaut pas. Ce gain n'est **pas attribué** : le frontal et l'état ont changé depuis le 26/10,
  et aucun A/B n'a été fait.
- Note : `matab.sh --restore` lance la VM sous ImGuiDock, et les parties de `matab.sh` sous le
  frontal natif. La résolution des jeux en plein écran en dépend (1920×1080 contre 1024×768 le
  26/10).

**Nuit d'endurance du 28 au 29/10 (`docs/endurance.md` §11)** : 342 démarrages (104
froids, 238 reboots) et 40 cycles de jeu (mb, zen, ut, d3), **aucun incident**. Borne haute
de Wilson à 95 % : 1,1 % pour les démarrages, 8,8 % pour les jeux. La VM quotidienne a été
relancée à la fin, puis arrêtée le 29/10 à 5 h 33 pour les essais de Quartz Extreme.

**Quartz Extreme, 29/10 (`docs/quartz-extreme.md` §1 ter et journal)** : dans une VM jetable
(`SNAPSHOT=1`, `QE=1`). **Éteint par défaut**, le disque quotidien n'est pas touché.
- **P1 fait** : bureau juste sous QE (menus, icônes, Dock, ombres, Génie). Trois causes :
  profondeur relue (la session repasse en milliers de couleurs), zone de la surface d'écran
  (forme `SetShape`) appliquée au viewport, aux ciseaux et à la présentation, présentation
  synchrone (curseur logiciel). Outil : `/var/tmp/pomppc-env` passe les variables `POMPPC_*`
  au WindowServer (vidage, notes).
- **P2 fait** : les fenêtres GL des jeux passent par l'hôte. Le plugin lie la fenêtre en
  surface accélérée (`glcBindSurface`). Au `Flush` du WindowServer, le kext présente la
  surface hôte du jeu dans sa région visible. Le compositeur lie la texture d'export pour ses
  transformations. Matrice en fenêtre sous QE : **mb vert 8,5 ms/image, zen vert 5,6**
  (6,2 et 3,5 hors QE).
- Côté matrice, les clients du kext sont comptés au-delà de ceux d'avant le jeu, car le
  WindowServer garde sa tranche sous QE.
- **P3 fait, QE allumé par défaut sur le M4** (`c792464`). Le jeu présente lui-même son
  échange par le noyau (méthode 5 du client 2D, région visible gardée à `SetShape`,
  TEX_PRESENT asynchrone), sans avis au WindowServer : pour une fenêtre qui n'est pas au
  premier plan, `CGSFlushSurface` lui faisait peindre le fond blanc (DOOM 3 clignotait au
  chargement, Marble Blast restait blanc). A/B au repos, même VM, 3 passes, en fenêtre :
  QE au niveau de QE éteint ou mieux (mb 6,1-6,3 contre 6,4, zen 2,9-3,1 contre 3,5, d3
  28,8-29,7 contre 29,8, ut 19,6-19,7 contre 19,9 ms/image). Matrice entière verte
  (cmr-pe rejouée seule). Endurance : §12 (ancien chemin) et §13 (chemin direct) de
  `docs/endurance.md`, aucun incident (156 démarrages, 32 cycles de jeu en fenêtre). **La VM
  quotidienne a QE depuis le 29/10, 21 h** (kext et plugin `71f9f76`, curseur compris, personnalité
  « auto », installés à 21 h 25).
- Défaut : `run_tiger.sh` pose `qe=on` sur Darwin:arm64 (`QE=0 ./run_tiger.sh` l'éteint) ;
  le kext en personnalité « auto » le suit. **QEMU reconstruit le 29/10 (19 h)**.
- Pièges de mesure : un frontal ImGuiDock resté ouvert après l'arrêt de sa VM tourne à
  60 % d'un cœur et double les temps en fenêtre ; la fenêtre de mesure d'UT (60 images)
  demande 3 passes.
- Piège : les traces du plugin (`POMPPC_GLTRACE`, vidages de tampons) ont **rempli le disque
  de l'invité** (16 Go). Les effacer après usage.

**À faire sur le M4, dans l'ordre**
1. Émulateurs voisins : QoS et attente à la microseconde mesurées le 30/10 (éteintes).
   Reste `vperm` en ligne (TBL/TBX), gain plafonné vers 0,3 % : en dernier. Suite possible
   de 0058 : `select` pour les délais courts seulement, profil d'UT.
2. Parité de **tcg/0055** (rejeu des traces, outil de preuve prouvé sur le PC le 27/10 :
   `c92dacc`). À faire seulement si les traces de second niveau reprennent ; elles sont
   arrêtées depuis le 22/10.
3. Parité de **tcg/0056** : **le PC l'a allumé le 31/10** (§4). Il faudra alors un saut long vers le
   chemin lent sur aarch64 : le `b.cond` imm19 n'a qu'1 Mio de portée, pour une région de
   64 Mio.

**Propres au M4** : pas de `qemu-ppc` linux-user (les preuves passent par l'invité), outils
`ppc32mt-bench.sh` et `chargehote.py` portés sur macOS, et `references.csv`, le fichier de
références de la matrice **du M4** (le PC utilise `references-linux.csv` et ne touche jamais
`references.csv`).

## 4. À faire en passant d'un hôte à l'autre

- **Venu du PC le 30/10 (pour le M4)**. Deux correctifs QE de l'invité sont à installer,
  sans changer le protocole ni QEMU :
  - kext : `805f504`, présentation asynchrone autour d'un curseur immobile ;
  - plugin : `f01d2d7`, `pomppc_qe_same`.

  Réinstaller kext et plugin (`install.sh` sans `QE`), puis redémarrer l'invité. Ensuite :
  - A/B au repos sur mb-pe, mb-fen et cmr-pe, avec l'ancien kext et plugin contre les
    nouveaux ;
  - vérifier que le curseur reste net sur un jeu en plein écran ;
  - une petite tranche d'endurance.

  **Zone froide** (`tcg/0056`, `x-tcg-cold`) : allumée sur le PC le 31/10 (−1 à −2 % sur 4 jeux
  sur 5). Sur arm64, elle est sans effet tant que le chemin lent n'a pas de saut long : le
  `b.cond` imm19 ne porte qu'à 1 Mio. C'est la parité à écrire avant de l'allumer sur le M4
  (rang 3 de la liste du M4).

  `tcg/0059` (vperm en ligne) est neutre sur le PC : sa version arm64 (TBL) n'est pas
  prioritaire. Sur le M4, vérifier que la série se compile toujours (l'opcode générique
  n'y est jamais émis) ; rien ne change sans `VPERMINLINE=1`.

- **Avant de quitter un hôte** : arrêter les campagnes en cours, remettre la VM quotidienne
  (`tools/tcg/matab.sh --restore`), mettre à jour cette page, puis commit et push.
- **En arrivant** : `git pull`, reconstruire si `patches/` a bougé, lire la section de l'hôte.
- Le disque Tiger raw de la VM quotidienne peut circuler par USB entre les hôtes ; il se
  prépare sur le M4 (`docs/gpu-3d-tiger.md` §5.1).
- Ce qui est commun aux deux hôtes : le frontal, le protocole qgpu, le kext et le plugin de
  l'invité, la matrice et ses jeux. Un changement de `patches/qgpu/qgpu_abi.h` impose de
  reconstruire le kext et de redémarrer l'invité sur **les deux** hôtes ; un changement de
  `qgpu_proto.h` impose de reconstruire QEMU et le plugin.
