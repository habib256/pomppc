# Déplacer le travail du plugin vers l'hôte (23/10/2026)

**Statut (23/10) : phase 0 faite, porte NON passée — appel vide 0,16 µs, mais lecture d'état
native ×0,6 à ×1,5 sur le travail réel (§5). Projet arrêté sous cette forme.**

Choix de l'utilisateur après le levier E (`docs/plugin-allegement.md` §7) : les petits leviers
du plugin sont épuisés (d3q neutre en jeu, `d3-plugin-x86.md` §10.6 ; UVWIDE −1,5 %), alors que
le plugin fait encore **17 à 31 % du fil du jeu** (profil `bench/tcg/ab/gprof-2210b-*`, six
leviers A+B allumés) :

| jeu (ms/image) | plugin | dont dessin des tableaux | dont verdict au dispatch |
|---|---|---|---|
| Marble Blast (9,8) | 25 % | — (`geom_begin` 10,6 %) | 4,8 % |
| Zen (5,1) | 17 % | — | 5,2 % |
| UT2004 (42,6) | 30 % | 13,5 % | 10,2 % |
| DOOM 3 (94,9) | 31 % | 17,3 % | 8,8 % |

Dans DOOM 3, c'est ~28 ms par image, soit ~23 µs de code PowerPC traduit par TCG pour chacun
des ~1 250 dessins. Le même travail, en code natif de l'hôte, coûterait de l'ordre de 20 à
50 fois moins.

## 1. L'idée : un appel synchrone, pas un DMA

`d3-plugin-x86.md` §1 a écarté la lecture des tableaux de l'invité par l'hôte, mais **en
asynchrone** : quand le fil de rendu traite le dessin, le jeu a déjà réécrit sa mémoire, et
lire en place exigerait un instantané. Ici, l'appel est **synchrone**, dans le fil du vCPU,
au moment où le plugin l'aurait fait lui-même :

1. le plugin écrit les arguments dans BAR0 (déjà projeté chez lui), puis une écriture 32 bits
   dans une **page d'appel** MMIO, projetée elle aussi dans son espace par le kext ;
2. sous TCG, cette écriture n'est **pas une sortie de VM** : c'est un appel au gestionnaire
   MMIO de QEMU, dans le fil du vCPU, sans noyau invité ni `mach_msg` (la sonnette actuelle
   passe par le kext, `IOConnectMethod*`, et prend le BQL : `qgpu-pci.c` l. 604) ;
3. le gestionnaire lit l'état de GLEngine et les tableaux **dans la mémoire virtuelle du
   processus courant**, par la MMU du vCPU (segments et table de hachage du G4, ceux de
   l'instant), fait le travail en natif, écrit le résultat (verdict, commandes du flux) dans
   BAR0 et rend la main. L'état lu est exactement celui que le plugin aurait lu.

Ce qui tombe des objections du §1 : pas d'instantané (synchrone) ; pas d'épinglage par le
kext (QEMU traduit l'adresse virtuelle à l'appel ; une page absente rend « repli » et le
plugin fait le travail lui-même, comme aujourd'hui) ; la parité arm64 est presque gratuite
(du C du device, pas du code du JIT ; `pomppc-parite-hotes`).

## 2. Ce qui pourrait passer à l'hôte

Tout ce qui **lit** l'état de GLEngine et **écrit** dans le flux, sans appeler GLEngine :
verdict au dispatch (`geom_ok`, `texture_ok`, `geom_format`, clé), `send_state`,
`geom_send_all` (comparaisons de matrices et de lumières), plan d'attributs, balayage et
recopie des indices (le `memcpy` natif lit la mémoire invitée directement : 1,8 Mio/image
dans DOOM 3). Reste dans l'invité : ce qui appelle GLEngine ou le kext (téléversement des
textures par `gle*`, crochetage, soumission), et tout ce que l'hôte refuse (repli).

Coût caché à surveiller : la **traduction des adresses**. Chaque lecture d'une structure de
GLEngine est une adresse virtuelle invitée ; il faut un petit cache de pages (VA → pointeur
hôte) valide le temps d'un appel, invalidé par tout changement de segment ou de table.

## 3. Phase 0 : la porte (mesures, pas de levier)

Trois questions, chiffrées sur le PC (binaire rapide, VM de dev) :

1. **Coût d'un appel vide** depuis l'espace utilisateur de l'invité : écriture dans la page
   d'appel → gestionnaire → retour, avec la région MMIO **sans BQL**
   (`memory_region_enable_lockless_io` ou équivalent de QEMU 11.1.2). À comparer à un appel
   de fonction de ~200 instructions dans l'invité, et à la sonnette actuelle par le kext.
2. **Coût de lecture de la mémoire virtuelle invitée** depuis le gestionnaire : traduction
   par la MMU du vCPU (`ppc_xlate`/`cpu_get_phys_page_debug` ou le TLB de TCG), avec un
   cache de pages par appel ; débit pour 64 o, 1 Kio et 64 Kio lus en plusieurs pages, contre
   un `memcpy` de l'invité.
3. **Un travail réel** : `geom_send_all` (ou la clé du verdict, `vd_key_of`) refait en natif
   sur l'état de `d3inter`, comparé à la version invitée, en instructions invitées retirées et
   en µs par dessin.

**Porte** : un appel vide sous ~1 µs, et la lecture d'état native au moins 10× plus rapide que
le code invité. Sinon, le projet s'arrête là (comme le second niveau du traducteur,
`plan-traducteur-rapide.md` §10.8).

Coûts d'entrée connus : un type de mémoire de plus dans le user client (projection de la page
d'appel) — c'est `qgpu_abi.h`, donc **kext reconstruit et redémarrage** (`pomppc-proto-kext-rebuild`) ;
QEMU à reconstruire (les deux cibles, `run_tiger.sh` lance `qemu-system-ppc64`).

## 4. Suite si la porte passe

Phase 1 : le verdict au dispatch et `geom_send_all` en natif derrière `POMPPC_GL_HOSTCALL`
(éteint, `=2` contrôle : les deux calculs, comparés), preuves `plugmd5`/`gpu`, A/B des quatre
jeux. Phase 2 : le dessin des tableaux (plan, indices, `send_state`).

## 5. Résultats de la phase 0 (23/10/2026) — la porte ne passe pas

**Verdict : appel vide sous 1 µs, oui (0,16 µs) ; lecture d'état native ≥ 10× plus rapide
que le code invité, non (×0,6 à ×1,5 sur le travail réel, ×3,3 sur 17 Kio d'état, ×13,6
seulement pour une copie de 64 Kio d'un seul tenant).** Comme prévu au §3, le projet s'arrête
là sous cette forme ; ce qui est construit reste éteint par défaut (branche
`worktree-agent-a7bf31d231ad94544`, non fusionnée).

### 5.1 Ce qui a été construit (tout éteint par défaut)

- **Device** (`patches/qgpu/qgpu-pci.c`) : propriété `x-hostcall=on` → BAR2, 4 Kio de MMIO,
  `QGPU_CAP_HOSTCALL`. Une écriture 32 bits de l'offset absolu (dans BAR0) d'un bloc d'appel
  exécute l'appel dans le fil du vCPU (`current_cpu`), avant que le `stw` ne rende la main.
  Traduction des adresses virtuelles par `tlb_vaddr_to_host` (le TLB de TCG, rempli au besoin
  par la table de hachage en mode sonde, sans faute ; NULL → statut « repli »), cache de 8
  pages valable le temps d'un appel. Opérations (`qgpu_proto.h`, `QGPU_HC_*`) : `NOP`,
  `READ_VA` (va, longueur → offset de BAR0), `CMP_LIST` (jusqu'à 32 triplets source, cache,
  longueur → masque des différences). Le bloc rend aussi le nombre de pages traduites et la
  durée du gestionnaire (ns de l'hôte). `x-hostcall-bql=on` sert la même page sous le BQL
  (mesure seulement). Sans `x-hostcall`, le PCI vu par l'invité est celui d'avant.
- **Sans BQL** : `memory_region_enable_lockless_io` existe dans QEMU 11.1.2, mais **seul
  `address_space_rw` l'honore** ; sous TCG, `cputlb.c` prend `BQL_LOCK_GUARD()` pour tout
  accès MMIO d'un bloc traduit. `patches/tcg/0050-tcg-mmio-lockless.patch` sert sans BQL une
  région de device déclarée ainsi (`mr->lockless_io && mr->dev` : la région des accès non
  assignés, elle aussi « lockless », garde le BQL). Sur mac99 la seule région concernée est
  `qgpu.call` (ACPI et HPET, les autres utilisateurs, sont des devices x86).
- **Kext** (`qgpu_abi.h`, donc reconstruit) : `QGPU_UC_MEM_CALL` (= 1) projette BAR2 dans le
  client (`kIOMapInhibitCache`) ; absente → `kIOReturnUnsupported`. Propriété ioreg
  `QGPUCallPage`.
- **Plugin** `20261023-hc0` : `POMPPC_GL_HOSTCALL=1` projette la page, prend 128 Kio en haut
  de sa tranche, et à chaque `geom_send_all` fait la même série de comparaisons (celles de
  `geom_send_all`, sans les sauts de MTXDIRTY/LIGHTDIRTY : fenêtre, profondeur, modèle-vue,
  projection, matrices de texture des unités du format, lumières si éclairage, plans de
  découpe, valeurs courantes) deux fois — `memcmp` dans l'invité, puis `CMP_LIST` natif — et
  compare les deux masques. `=2` ajoute un contrôle de lecture : l'état de GLEngine
  (0x4240 octets à partir de `g − 0x240`, cinq pages) et le `PCtx` lus par `READ_VA`, comparés
  octet à octet. `POMPPC_GL_HOSTCALL_REPS=n` répète chaque mesure. Ligne `HCALL` de la note.
  Le plugin fait par ailleurs exactement ce qu'il faisait.
- **Micro-banc** `guest/qgpu-test/hcbench.c` et **job** `tools/guest/jobs/hostcall` (banc,
  trois ouvriers de changement de processus, puis `d3inter` en sept modes, images comparées).

### 5.2 Méthode

QEMU 11.1.2 construit à part (`~/src/qemu-hostcall`, copie de `~/src/qemu` + device +
tcg/0050, `-O3 -march=native`, **sans PGO ni LTO** : le chemin MMIO du binaire rapide peut
être un peu plus court). VM de dev `tiger-dev-plugin2.raw`, single-user, SMP 2 (ppc64,
MTTCG), RTX, avec **les options CPU et TCG de la VM quotidienne** (relevées dans un
`run_tiger.log` : `x-sr-tlb`, `x-msr-nobql`, `x-lmw-vector`, `x-dcbz-inline`, `x-jit-*`,
`x-jc-*`…). Temps par `mach_absolute_time` dans l'invité ; durée du gestionnaire mesurée par
le device ; instructions invitées par le greffon `hotblocks` (instantanés autour de
`d3inter`, `HOSTCALL=0` puis `=1`, `tools/re/hotplug.py` et un relevé par bloc pour `memcmp`
de libSystem, 0x901311a0).

### 5.3 Question 1 : le coût d'un appel

| appel (µs) | chaud (boucle) | isolé, après `getppid` | isolé, après balayage de 4 Mio |
|---|---|---|---|
| page d'appel, `NOP`, sans BQL | **0,157–0,161** (gestionnaire 0,013) | **0,55** | **0,66** |
| page d'appel, `NOP`, sous BQL (`x-hostcall-bql`) | 0,192 | | |
| fonction invitée de 244 instructions (`f200`) | 0,080 | 0,117 | 0,102 |
| kext, `QGPU_UC_READ_REG` (aller-retour sans doorbell) | 8,2–8,8 | | |
| sonnette actuelle, `QGPU_UC_SUBMIT` synchrone d'un NOP | 31–34 | | |

L'appel vide coûte **50 fois moins qu'un aller-retour par le kext et 200 fois moins que la
sonnette**, mais **deux fonctions invitées de 244 instructions** à chaud (TCG exécute une
boucle d'ALU à ~3 G instructions/s) et 5 à 6 fois plus isolé. Le gestionnaire lui-même pèse
13 ns : le reste est la sortie MMIO de TCG (écriture en voie lente, `io_prepare`, répartition)
et, à froid, les remplissages du TLB pour la page d'appel et le bloc. Sans BQL, le gain
n'est que de 0,03 µs (non disputé : un second vCPU qui tient le BQL l'allongerait).

Sans les options de la VM quotidienne (devloop nu), le kext coûte 44 µs et la sonnette
82–90 µs, et le `memcpy` invité est 3 fois plus lent : à ne pas comparer.

### 5.4 Question 2 : lire la mémoire virtuelle de l'invité

Boucle chaude, `READ_VA` vers BAR0 contre `memcpy` de l'invité (même source) :

| taille | pages | natif (µs) | dont gestionnaire | `memcpy` invité (µs) | rapport |
|---|---|---|---|---|---|
| 64 o | 1 | 0,257 | 0,021 | 0,054 | ×0,2 |
| 1 Kio | 2 | 0,274 | 0,042 | 0,371 | ×1,4 |
| 4 Kio | 1 | 0,299 | 0,044 | 0,669 | ×2,2 |
| 64 Kio | 17 | 1,493 | 1,177 | 20,31 | **×13,6** |

Dans `d3inter` (appels isolés, au milieu du rendu), l'état de GLEngine (16 960 octets, cinq
pages) : **natif 0,93 µs** (gestionnaire 0,55) contre **`memcpy` invité 3,07 µs (×3,3)**.
Le `memcpy` de l'invité tient ~3,2 Go/s (lmw/stmw vectoriels, dcbz en ligne) : la marge du
natif n'apparaît qu'au-delà de quelques Kio d'un seul tenant.

### 5.5 Question 3 : un travail réel, les comparaisons de `geom_send_all` sur `d3inter`

2 120 dessins, ~300 octets comparés par dessin (9 entrées en moyenne : pas d'éclairage,
peu d'unités), 608 dessins avec un changement :

| par dessin | invité (`memcmp`) | natif (`CMP_LIST`) | dont gestionnaire | rapport |
|---|---|---|---|---|
| appel isolé (cas réel), 2 passes | 0,85–0,86 µs | 1,38–1,39 µs | 0,36 µs | **×0,6** |
| même, avec le contrôle de lecture | 0,92 µs | 1,33 µs | 0,34 µs | ×0,7 |
| répété 20 fois (chaud) | 0,59–0,60 µs | 0,39–0,40 µs | 0,13–0,16 µs | ×1,5 |
| instructions invitées | **~900** (boucle 225 + `memcmp` 675, 9 appels) | ~10 (4 `stw`, 1 `lwz`) + l'appel | | |

L'appel natif retire bien ~900 instructions invitées par dessin, mais il coûte plus de
temps qu'elles : à ~1 G instructions/s (ce que tient le code du plugin sous TCG), 900
instructions font 0,85 µs, et un appel isolé en coûte 1,4.

### 5.6 Correction

- `hcbench` : 64 o à 256 Kio (65 pages, au-delà du cache de 8) relus **justes** ; page
  jamais touchée et adresse nulle → **repli** (statut 1, 0 octet) ; la même page, une fois
  touchée → lue juste. `CMP_LIST` : égal → 0, un octet changé → bit posé.
- **Changement de processus** : trois `hcbench ctx` en même temps sur deux vCPU, chacun
  réécrit 4 pages **à la même adresse virtuelle** (0x50000000, contenu propre au processus et
  au tour), cède le processeur, relit par la page d'appel : 3 × 3 000 lectures, **0 fausse,
  0 repli**, ~3 000 changements de contexte par processus (`task_info(TASK_EVENTS_INFO).csw` ;
  `getrusage` rend 0 sous Tiger), intervalles d'exécution chevauchants.
- `d3inter`, `HOSTCALL=2` : 2 120 comparaisons natives, **0 écart** avec l'invité, 0 repli ;
  4 240 lectures (GLEngine, PCtx), **0 fausse**. Images identiques dans les sept modes
  (`def`, `off`, `chk`, deux isolés, deux répétés : md5 `0aa2adab…`).
- `plugmd5`, **68 scènes**, `ref` contre `HOSTCALL=2` : **0 écart d'image** ; sur l'ensemble,
  6 312 comparaisons natives (2 824 avec un changement), **0 écart, 0 repli** ; 12 624
  lectures d'état, **0 fausse** (dont `forkdraw`, et les scènes Begin/End `mbimm`, `litdirty`).
- Sans `x-hostcall` (device par défaut) et le nouveau kext : job `gpu` **56 OK, 0 échec**
  (`qgpu_test` 0) ; `QGPUCallPage = 0` ; `hcbench` rend 2 ; le plugin sous `HOSTCALL=2` note
  « éteint (device sans QGPU_CAP_HOSTCALL) » et rend la même image.

### 5.7 Pourquoi la porte échoue, et ce qui resterait

1. **Le code invité n'est pas 20 à 50 fois plus lent que le natif sur ce travail** : les
   comparaisons et copies du plugin sont liées à la mémoire, et TCG les exécute à ~1 G
   instructions/s, `memcpy` à ~3 Go/s. Les 23 µs par dessin de DOOM 3 sont ~12 000
   instructions réparties, pas des instructions lentes.
2. **Un appel coûte 0,16 µs à chaud, 0,55–0,66 µs isolé** — de 150 à 650 instructions du
   plugin. Le gestionnaire natif (13 ns à 0,36 µs) n'est pas le problème ; la sortie MMIO de
   TCG et les ratés du TLB à froid le sont. Le BQL n'y est presque pour rien (0,03 µs).
3. **Le volume par appel est petit** : ~300 octets d'état par dessin pour `geom_send_all`.
   Seules les copies de dizaines de Kio d'un seul tenant atteignent ×10 ; les indices de
   DOOM 3 (1,8 Mio par image, ~1,4 Kio par dessin) n'y sont pas (×1,4 à ×2,2), et même
   copiés d'un bloc ils ne pèsent que ~0,6 ms par image au `memcpy` invité.

Ce que la phase 0 n'a pas mesuré : un travail **lié à la logique** (verdict, clé, plan
d'attributs : branches et pointeurs, où le natif devrait gagner davantage que ×3) appelé
**une seule fois par dessin** pour plusieurs µs de travail invité — seule granularité où le
coût fixe s'amortit. Ce serait porter en C de l'hôte une large part du verdict et de
`send_state` (la connaissance de GLEngine de `pomppc_accel.c`), avec repli complet ; le
plafond, si tout le chemin d'un dessin (~23 µs) passait à ×3, serait de l'ordre de 15 µs par
dessin (~19 ms par image de DOOM 3), mais rien ici ne le prouve. C'est un autre projet, à
décider par l'utilisateur ; la porte de celui-ci est fermée.

Pièges notés en route :

- `memory_region_enable_lockless_io` n'a **aucun effet sous TCG** sans tcg/0050 (cputlb.c
  prend le BQL inconditionnellement) ; `io_mem_unassigned` est aussi « lockless ».
- devloop nu ≠ VM quotidienne : sans `CPU_OPTS`/`TCG_OPTS` relevés dans `run_tiger.log`, les
  allers-retours par le noyau sont 5 fois plus chers et `memcpy` 3 fois plus lent.
- Sous Tiger, `getrusage` rend 0 changement de contexte : `task_info(TASK_EVENTS_INFO)`.
- `tiger-dev-plugin2.raw` porte un agent d'avant le code de sortie (en-tête à 3 champs) :
  `devloop.py run` rend toujours 0, lire le verdict du job (`ko=`, `VERDICT`).
- Un worktree neuf n'a pas `bench/devloop/mailbox.json` : le copier du dépôt principal (les
  copies du disque de dev ont les mêmes boîtes).

Reproduire (QEMU construit avec tcg/0050 et le device ; `OPTS_CPU` = la ligne `cpu=` d'un
`run_tiger.log` récent, sans `g4,` ni `x-tier2`) :

```
DEVDISK=…/tiger-dev-plugin2.raw SMP=2 QEMU_BIN=~/src/qemu-hostcall/build/qemu-system-ppc \
  CPU_OPTS=$OPTS_CPU TCG_OPTS=x-jit-near=on,x-jit-rel32=on,x-jc-bits=14,x-jc-word=on,x-jc-2way=on \
  QEMU_EXTRA="-global qgpu-pci.x-hostcall=on" SSH_PORT=0 python3 tools/guest/devloop.py start
tools/guest/jobs/stage.sh hostcall /tmp/j ; printf 'REPS=20\nCTXN=3000\n' > /tmp/j/env.sh
python3 tools/guest/devloop.py run /tmp/j --timeout 1800
```
