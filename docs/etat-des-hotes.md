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

## 2. PC x86-64 : état au 28/10/2026, 13 h 45

**Défauts sur Linux x86-64** : toute la série 0031-0052, avec `PPC32MT`, `LFSKNOWN` et
`TLBBASE`. Le binaire rapide PGO a été reconstruit le 28/10 à 12 h 36 et contient le correctif
qgpu de Colin McRae.

**Vitesse** : DOOM 3 entre 79 et 82 ms/image en fenêtre sur le binaire rapide ; Colin McRae
44,9-47,5 ms/image en plein écran. Journal détaillé : `docs/vitesse-doom3-x86.md`, §1 à §21.

**Fait les 27 et 28/10**
- Endurance : 683 démarrages sans incident (`docs/endurance.md` §10).
- Profil aux compteurs matériels (§19) : DOOM 3 est limité par le **chargement des
  instructions** (frontend 40,6 %).
- **Colin McRae marche sur le PC** (commit `5c2ea82`). Le pilote NVIDIA refusait
  `ALIAS x = fragment.color.primary;` dans deux programmes ARB. qgpu réécrit désormais ces
  `ALIAS` en `ATTRIB`/`PARAM` (`patches/qgpu/qgpu-gl.c`, `gl_prog_fix_alias`). Le pilotage des
  menus de `tools/matrice/jeux/cmr.py` a été rendu robuste à un hôte lent.
- **tcg/0056 `x-tcg-cold`** (`TCGCOLD=1`), qui range les chemins lents en zone froide :
  **éteint par défaut**. Gain de +2 à +4 % sur mb, zen et ut, neutre sur d3 et cmr.
  **Une partie cmr a gelé** l'invité sur 4 parties cmr avec la zone froide, et aucune des 15
  autres parties avec zone froide n'a gelé (§20).
- Mémoire directe (« fastmem ») et vérification TLB compacte **écartées** sur chiffres (§21).

**En arrivant sur le PC (préparé sur le M4 le 30/10, 11 h 30)** : le PC a pris trois retards
de livraison depuis le M4 (points 6, 7 et 8 ci-dessous). Les rattraper d'un bloc **avant**
les points 1 à 5, sinon les mesures se font sur un binaire et un plugin périmés.
- a. `git pull`, puis `QEMU_FAST=1 ./scripts/build_qemu_qfb.sh` : reconstruit `build/` puis
  `build-fast/` (PGO). Les nouveautés de `patches/qgpu/` (bandes de TEX_READBACK le 28/10,
  `QGPU_OP_TEX_PRESENT`, `QGPU_CAP_QE_DEFAULT` et la propriété `qe` le 29/10) y entrent. Les
  patches `tcg/0057` et `tcg/0058` s'appliquent aussi, mais leur code est sous `__APPLE__` :
  rien ne change sur Linux. Vérifier que le build ne se plaint pas de leur application.
- b. Reconstruire et réinstaller le **kext** et le **plugin** de l'invité (`install.sh` sans
  `QE`, personnalité « auto »), puis redémarrer l'invité. Le plugin porte le correctif
  `tex_bind_id` du 30/10.
- c. Sans QE (le défaut du PC) : matrice entière sur `build-fast`, pour vérifier que rien n'a
  régressé ; rejouer d3-pe et cmr-pe à 1920×1080, car le vidage y est maintenant complet.
- d. Ensuite seulement : la parité QE (point 7), avec, en VM jetable sous `QE=1`, la
  vérification de l'ombre d'un Terminal posé sur Zenerchi, Marble Blast et DOOM 3 en fenêtre.
  Le jeu doit apparaître sous l'ombre en dégradé, sans bande blanche ni noire. La méthode et
  les outils de sonde du WindowServer sont décrits dans `docs/quartz-extreme.md`, au journal
  du 30/10.

**À faire sur le PC, dans l'ordre**
1. Comprendre pourquoi `tools/tcg/matab.sh --restore` a relancé la VM quotidienne sur
   `build/` au lieu de `build-fast/` le 28/10 à 13 h 40.
2. Finir la chasse au gel de cmr sous `x-tcg-cold` : au moins 6 parties cmr par bras, ref
   contre cold, sur l'arbre `~/src/qemu-ceil` (modèle : `bench/cold-2810/chaine4.sh`). Allumer
   par défaut seulement si aucun gel n'apparaît et si la matrice entière gagne.
3. Profil croisé des 5 jeux, `perf` et `-perfmap` sur le binaire rapide (modèle :
   `bench/cold-2810/chaine5.sh` ; retirer l'attente de l'ancien pid). But : des postes communs à
   tous les jeux.
4. Fixer `plancher_ms_linux` de cmr dans `tools/matrice/jeux/cmr.py` : il vaut encore 88, la
   valeur du M4, alors que le PC mesure environ 46.
5. Faire valider à l'œil par l'utilisateur les références `bench/matrice/ref-linux/cmr-pe` et
   `d3-fen` (`matrice.py --valider`).

6. **Venu du M4 le 28/10** : `qgpu_proto.h` a changé (TEX_READBACK par bandes). Reconstruire
   `build-fast` et `build`, puis le plugin de l'invité (`.run/cmr/cycle.sh NORUN=1` ou
   l'équivalent du PC). Ensuite, rejouer d3-pe et cmr-pe : à 1920×1080, le vidage n'était
   pas complet avant.
7. **Venu du M4 le 29/10** : Quartz Extreme (P1 à P3, `docs/quartz-extreme.md`).
   `patches/qgpu/` a changé (`QGPU_OP_TEX_PRESENT`, `QGPU_CAP_QE_DEFAULT` et la propriété
   `qe` du device, version inchangée) : **reconstruire QEMU**, puis réinstaller kext et
   plugin (`install.sh` sans `QE` : personnalité « auto »). Sur le PC, `run_tiger.sh` ne
   pose pas `qe=on` : **QE reste éteint**, rien ne change pour les jeux. Hors QE, le plugin
   réserve les 4 derniers Kio de sa tranche (chaque moitié perd 4 Kio d'arène). Pour la
   parité : VM jetable avec `QE=1 ./run_tiger.sh` (pose `qe=on`), matrice entière et A/B en
   fenêtre (3 passes par bras, la passe unique d'UT est trop bruitée), tranche d'endurance
   comme au §13 de `docs/endurance.md` ; ensuite ajouter `Linux:x86_64` au test
   d'hôte de `run_tiger.sh`.
8. **Venu du M4 le 30/10** : correctif du plugin pour Quartz Extreme (`tex_bind_id`,
   ombre sur une fenêtre GL). Il ne touche ni le protocole ni le kext : il suffit de
   reconstruire et réinstaller le plugin de l'invité. Il n'a d'effet que si QE est allumé
   (`QE=1`). Les patches `tcg/0057` et `tcg/0058` sont propres à macOS (le PC peut les
   compiler, ils n'y font rien).

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
3. Parité de **tcg/0056** seulement si le PC l'allume. Il faudra alors un saut long vers le
   chemin lent sur aarch64 : le `b.cond` imm19 n'a qu'1 Mio de portée, pour une région de
   64 Mio.

**Propres au M4** : pas de `qemu-ppc` linux-user (les preuves passent par l'invité), outils
`ppc32mt-bench.sh` et `chargehote.py` portés sur macOS, et `references.csv`, le fichier de
références de la matrice **du M4** (le PC utilise `references-linux.csv` et ne touche jamais
`references.csv`).

## 4. À faire en passant d'un hôte à l'autre

- **Avant de quitter un hôte** : arrêter les campagnes en cours, remettre la VM quotidienne
  (`tools/tcg/matab.sh --restore`), mettre à jour cette page, puis commit et push.
- **En arrivant** : `git pull`, reconstruire si `patches/` a bougé, lire la section de l'hôte.
- Le disque Tiger raw de la VM quotidienne peut circuler par USB entre les hôtes ; il se
  prépare sur le M4 (`docs/gpu-3d-tiger.md` §5.1).
- Ce qui est commun aux deux hôtes : le frontal, le protocole qgpu, le kext et le plugin de
  l'invité, la matrice et ses jeux. Un changement de `patches/qgpu/qgpu_abi.h` impose de
  reconstruire le kext et de redémarrer l'invité sur **les deux** hôtes ; un changement de
  `qgpu_proto.h` impose de reconstruire QEMU et le plugin.
