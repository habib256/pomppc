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

**Propres au PC** : `patches/diag/` (auto-profileur, plafonds), `tools/tcg/perfhw.sh` (compteurs
Intel), et l'arbre de mesure `~/src/qemu-ceil` (série et diag, -O2, sans PGO). `perf` exige
`perf_event_paranoid` ≤ 1, que l'utilisateur règle à la main ; la valeur revient à 4 à chaque
redémarrage.

## 3. Mac M4 : état au 27/10/2026, 8 h (dernier commit venu du M4 : `07eefa5`)

**Défauts sur Darwin:arm64** : depuis le 26/10 (`550b2a8`), la série du PC est allumée et
prouvée : `PPC32MT TLBPRECISE LMWINLINE DCBZINLINE FPNATIVECMP VFPNATIVECMP DIRTYSKIP PTEGMAP
JC2WAY FPKNOWN FPRFLAZY STFSKNOWN LFSKNOWN TLBBASE`. A/B : DOOM 3 −23,9 %, UT2004 −18,6 %,
Marble Blast −22,7 %. Garde : un binaire sans tcg/0037-0039 les laisse éteints.

**Fait les 25, 26 et 27/10** (`docs/parite-m4-2510.md` §5)
- Preuves des étapes 1 à 6 de la parité. tcg/0037 a été reposé après 0052 et corrigé.
- Disque de dev en 10.4.11 : `disks/tiger-dev-m4-1011.raw`.
- UT2004 sur le M4 : l'éclairage spot du GL d'Apple est calculé par qgpu (`5f25498`).
- Matrice `20261026-0900` : 16 vertes sur 17. Warcraft III est rouge, indépendamment de la
  série.
- Suite du M4 validée et profils CPU OS 9 éprouvés (`07eefa5`). `run-all.sh` : 283 OK.

**À faire sur le M4 à la prochaine séance, dans l'ordre**
1. `git pull`, puis **reconstruire `~/src/qemu`** : qgpu a changé (ALIAS de cmr, sans effet
   attendu sur le GL d'Apple qui les acceptait déjà) et 0056 entre dans `build_qemu_qfb.sh`. Sur
   arm64, `x-tcg-cold` est sans effet et l'annonce.
2. **Rejouer la cellule cmr** (`matrice.py -j cmr -m pe`). `cmr.py` a changé sur le PC :
   détection du titre blanc, alerte « Low-end machine », fenêtre après le chargement. Vérifier
   que le pilotage marche encore sur le M4 et que la course est mesurée.
3. Matrice entière sous les défauts actuels, pour vérifier que rien n'a bougé depuis le 26/10.
4. Parité de **tcg/0055** (rejeu des traces, outil de preuve prouvé sur le PC le 27/10 :
   `c92dacc`). À faire seulement si les traces de second niveau reprennent ; elles sont
   arrêtées depuis le 22/10.
5. Parité de **tcg/0056** seulement si le PC l'allume. Il faudra alors un saut long vers le
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
