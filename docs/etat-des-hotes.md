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
7. **Venu du M4 le 29/10** : le kext et le plugin ont changé (Quartz Extreme P1 à P3,
   `docs/quartz-extreme.md`), et `patches/qgpu/qgpu_proto.h` aussi (`QGPU_OP_TEX_PRESENT`,
   capacité `QGPU_CAP_TEX_PRESENT`, version inchangée) : **reconstruire QEMU**, puis
   réinstaller kext et plugin dans l'invité, sans `QE=1`, et repasser la matrice. Hors QE, le
   plugin réserve les **4 derniers Kio de sa tranche** (présentations du noyau) : chaque
   moitié perd 4 Kio d'arène. Ensuite, pour la parité de QE : VM jetable avec `QE=1`, matrice
   entière et tranche d'endurance comme au §12 de `docs/endurance.md`.

**Propres au PC** : `patches/diag/` (auto-profileur, plafonds), `tools/tcg/perfhw.sh` (compteurs
Intel), et l'arbre de mesure `~/src/qemu-ceil` (série et diag, -O2, sans PGO). `perf` exige
`perf_event_paranoid` ≤ 1, que l'utilisateur règle à la main ; la valeur revient à 4 à chaque
redémarrage.

## 3. Mac M4 : état au 29/10/2026, 12 h 15

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
- **P3 (vitesse, endurance) fait, défaut en attente** : TEX_PRESENT (le kext présente
  l'instantané de l'échange), avis au WindowServer sur un fil à part et cadencés à 60 Hz.
  A/B au même binaire, matrice entière QE éteint (`20261029-0936`) contre allumé
  (`20261029-0905`) : **16 vertes sur 16 des deux côtés**. Plein écran neutre ; en fenêtre
  mb +5 %, zen +16 %, ut +20 %, nx +5 %, d3 −37 % (une passe, à confirmer). Endurance QE
  (`docs/endurance.md` §12) : 30 froids, 60 reboots, 16 cycles de jeu en fenêtre, aucun
  incident. **QE reste éteint par défaut** : coût en fenêtre et pas encore validé sur le PC.
- La VM quotidienne tourne sous ImGuiDock (relancée par la suite d'endurance, sans QE).
- Piège : les traces du plugin (`POMPPC_GLTRACE`, vidages de tampons) ont **rempli le disque
  de l'invité** (16 Go). Les effacer après usage.

**À faire sur le M4, dans l'ordre**
1. Quartz Extreme, avant l'allumage par défaut : réduire le coût par image en fenêtre
   (copie SURF_TEX + avis, ut +20 %, zen +16 %), seconde passe de d3-fen, puis décision de
   l'utilisateur. L'allumage = personnalité `POMPPCQuartzExtreme` à vrai, `QE=0` pour
   éteindre, seulement après validation sur le PC.
2. Étude des émulateurs voisins (PowerEmu, ppc-osx-vgpu) : QoS « interactive » du fil vCPU
   sur macOS, attente en ns (kqueue) dans la boucle principale, permutations AltiVec en
   ligne (TBL/TBX). À mesurer en A/B, licence à vérifier fichier par fichier.
3. Parité de **tcg/0055** (rejeu des traces, outil de preuve prouvé sur le PC le 27/10 :
   `c92dacc`). À faire seulement si les traces de second niveau reprennent ; elles sont
   arrêtées depuis le 22/10.
4. Parité de **tcg/0056** seulement si le PC l'allume. Il faudra alors un saut long vers le
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
