# TCG et le G4 émulé — relevé, A/B des cœurs, patches

Chantier « le G4 émulé lui-même » (`TODO.md` §5, 25/09/2026). Au-delà des lots du plugin,
le plafond des jeux est la vitesse à laquelle TCG exécute le code PowerPC. Ce document
relève **où part le temps du processeur émulé** (instructions exécutées, helpers, coût
hôte), mesure **1 contre 2 cœurs**, et décrit **le patch qui en est sorti** :
`patches/tcg/0001-ppc-sr-tlb.patch` (propriété de CPU `x-sr-tlb`) ; un second essai,
`patches/tcg/essais/0002-ppc-lmw-inline.patch` (`x-lmw-inline`), exact mais sans gain, n'est
pas appliqué.

**En une phrase** : sur Marble Blast, ni AltiVec ni le flottant ne dominent ; ce qui coûte,
c'est que QEMU **vide tout son TLB ~23 000 fois par seconde**, à chaque changement de
registre de segment de Tiger — le patch 0001 en retire 93 % et donne **+9,8 % d'images/s**
en SMP=2 (2 manches entrelacées, 109 fenêtres appariées).

Méthode : `docs/metrologie-boot.md` (scène fixe, plusieurs passes, entrelacement, médiane ;
le self % localise, il ne valide pas) et le protocole de `docs/flottant-rapide.md` (Marble
Blast joue sa démo seul, fenêtres de 5 s appariées par triangles/image).

---

## 0. Où ça a été mesuré, et ce qui attend la VM quotidienne

| | |
|---|---|
| Hôte | Mac Apple M4 (4 cœurs P + 6 E), macOS 26 |
| QEMU | 9.2.0 + tous les patches du dépôt, **copie isolée** `~/src/qemu-tcg` (git local : « base POMPPC », puis un commit par patch) ; `~/src/qemu` n'a pas été touché |
| Disque | **copie APFS** (`cp -c`) de `disks/tiger-dev.raw` dans le scratchpad — le disque de dev lui-même n'est pas modifié (équivalent plus strict de `SNAPSHOT=1`, qui rendait les boîtes aux lettres de `devloop` inutilisables) |
| Invité | Tiger 10.4.6 de dev : kext + plugin **v15** (`20260923-vtxlimit`, commit 2215ee1) ; le binaire de mesure est donc construit avec le `qgpu` **v15** de 2215ee1 (seul `hw/display/qgpu*` diffère du binaire de référence : tout `target/ppc`, `accel/tcg`, `fpu` est celui de `main`). Même plugin des deux côtés de chaque A/B |
| Jeu | Marble Blast Gold, démo jouée seule, 800×600 fenêtré ; `x-fast-fp=on` partout |
| Charge | la VM quotidienne est restée allumée **au repos** (7-9 % d'un cœur, relevé `top` au début de chaque config, `bench/tcg/res/load.txt`) |
| Journaux | `bench/tcg/res/` (non versionné, comme tout `bench/`) : bilans `POMPPC_GL_STATS` de chaque passe, `info jit`, `sample`, sortie du greffon, `bilan-1.txt` |

**DOOM 3 n'est sur aucun disque accessible sans la VM quotidienne** (ni `tiger-dev.raw`,
ni l'USB). Quand le coordinateur l'a annoncée libre, **l'accès (même un `ssh` en lecture)
a été refusé par le classifieur de permissions** : rien n'a été lancé dessus. Tout est
prêt pour la jouer (§6) : binaire v19 `~/src/qemu-tcg19/build/qsr` (le `qgpu` exact du
binaire de référence), `tools/tcg/d3run.sh` (partie complète : arrêt propre, relance avec
`SMP`/`SRTLB`, `meas-tcg.sh` dans l'invité, premier plan, `info jit`, `sample` hôte à
T+300, `frames.csv`, `d3win.py`), et `--restore` pour rendre la VM (binaire de
référence, SMP=2).

---

## 1. Relevé statique : ce que QEMU 9.2 traduit en ligne, ce qui passe par un helper

Lu dans `target/ppc/translate.c`, `translate/*.c.inc`, `helper.h` (arbre 9.2.0 + patches).
« Helper » = un appel de fonction C par instruction exécutée ; les drapeaux disent ce que
TCG doit faire autour de l'appel (`NO_RWG` : rien ; sans drapeau : **toutes** les
variables globales TCG — GPR, CR, XER, LR, CTR… — recopiées dans `env` avant l'appel et
relues après).

### 1.1 AltiVec

| Traduction | Instructions (G4) |
|---|---|
| **En ligne** (TCG `i128` / gvec) | `lvx lvxl stvx stvxl` (un chargement 128 bits), `lvsl lvsr`, `vand vandc vor vxor vnor`, `vaddu{b,h,w}m vsubu{b,h,w}m`, additions/soustractions saturées (gvec + `VSCR[SAT]`), `vaddcuw vsubcuw`, `vmax/vmin` entiers, `vavg*`, `vrl* vsl* vsr* vsra*` (par élément), `vsl vsr` (128 bits), **`vsel`** (`gvec_bitsel`), `vsplt{b,h,w}` et `vspltis*` (gvec dup), comparaisons entières `vcmpequ* vcmpgt[su]*` (+ CR6), `dst dss` (sans effet) |
| **Helper, `NO_RWG`** | **`vperm`** (boucle octet par octet), **`vsldoi`**, **`vmrgh{b,h,w} vmrgl{b,h,w}`**, `vupk*`, `vmul[eo]*`, `vmsum*`, `vmhadd*`, `vslo vsro`, `mtvscr mfvscr` |
| **Helper avec `env`, sans drapeau** | **tout le flottant** : `vaddfp vsubfp vmaddfp vnmsubfp vmaxfp vminfp vrefp vrsqrtefp vexptefp vlogefp vrfi* vcfux vcfsx vctuxs vctsxs vcmp{eq,ge,gt,b}fp` (4 appels softfloat par instruction ; hardfloat sous `x-fast-fp`) ; `vpk*` saturés, `vsum*` ; `lve{b,h,w}x stve{b,h,w}x` (chargement d'un élément) |

Les fonctions de `idSIMD_AltiVec` de DOOM 3 (transformations de sommets, `vmaddfp`,
`vperm` pour les tableaux non alignés, `vsldoi`, `vmrghw`, `lvsl`) tombent donc presque
toutes dans les deux dernières lignes. Deux améliorations sûres existent sans rien changer
au calcul : **`TCG_CALL_NO_RWG` sur les helpers flottants AltiVec** (ils ne lisent ni
n'écrivent aucune globale TCG : `vec_status` et les AVR vivent dans `env` hors globales —
sauf les `vcmp*fp.` qui écrivent CR6) et **`vsldoi`/`vmrg*` en ops `i64` en ligne**. Leur
intérêt dépend de la part d'AltiVec dans le code chaud : ~0 % sur Marble Blast (§2), **à
mesurer sur DOOM 3** (§6).

### 1.2 Entier, mémoire, système (G4 32 bits)

| Traduction | Instructions |
|---|---|
| En ligne | toute l'arithmétique et la logique (`add…`, `mullw`, `mulhw[u]`, `divw[u]`, `rlw*`, `slw srw srawi`, `cntlzw`, `cmp*`, `ext*`), les chargements/rangements simples, indexés, à mise à jour et inversés (`lwbrx`…), `lwarx/stwcx.` (cmpxchg), `mfcr mtcrf mcrf cr*`, `mfspr/mtspr` de LR/CTR/XER/SPRG/SRR0-1, `lfd stfd fmr fneg fabs fnabs`, `sync eieio` (barrière), `dcbt dcbtst dcbst dcbf` (rien) |
| **Helper** | **`lmw`** (sans drapeau), **`stmw`** (`NO_WG`), `lswi lswx stswi stswx`, **`sraw`** (sans drapeau !), `dcbz`, `icbi`, **`lfs lfsu lfsx stfs…`** (`todouble`/`tosingle`, `NO_RWG_SE`), tout le flottant scalaire (2 appels par instruction avec `fastfp/0002`), `fcmpu`, `frsp`, `fctiwz`, **`mftb`/`mfspr TBL/TBU`** (horloge hôte), `mtmsr`, `rfi`, `mtsr mtsrin` (vidage du TLB, §3), `tlbie`, `mtspr` des BAT et de SDR1 (vidage), `mfsr` |
| Sortie de bloc | `blr bctr` et `bclr/bcctr` conditionnels → `lookup_and_goto_ptr` (**`helper_lookup_tb_ptr`** à chaque retour de fonction et chaque appel indirect), `sc`, `rfi`, `isync`, `mtmsr` |

### 1.3 Ce que l'amont a fait depuis 9.2

`git fetch --shallow-exclude=v9.2.0 origin tag v11.1.1` dans la copie (14 560 commits) :
**rien sur ces points**. Aucun helper AltiVec passé en gvec, `lmw/stmw/sraw` toujours en
helper, `helper.h` identique pour toutes les lignes ci-dessus, `QEMU_NO_HARDFLOAT` toujours
forcé pour `TARGET_PPC` dans `fpu/softfloat.c`, et le `tlbie`/`store_sr` du MMU 32 bits
pose toujours `TLB_NEED_LOCAL_FLUSH` (vidage complet). Les commits `target/ppc` sont du
ménage (en-têtes, décodetree du flottant scalaire, PPE42, endianité des accès AltiVec
réécrite sans changer leur traduction) ; côté `accel/tcg`, rien sur le coût des vidages.
Il n'y a donc rien à rétroporter.

---

## 2. Profil dynamique : ce que le G4 exécute vraiment

### 2.1 L'outil : `tools/tcg/ppcmix`

Greffon TCG (`tools/tcg/ppcmix.c`, construit par `tools/tcg/build.sh` contre les en-têtes
de la copie ; le build macOS de QEMU 9.2 a `CONFIG_PLUGIN`) : un compteur **en ligne** par
instruction traduite, rangé par clé d'opcode (primaire + code étendu ; `mtspr`/`mfspr` par
numéro de SPR), en deux banques (commpage `pc >= 0xffff8000` — memcpy/bzero AltiVec de
Tiger — et reste). Un fil écrit l'instantané toutes les 10 s ; `tools/tcg/ppcmix.py` fait
la différence de deux instantanés, nomme les clés et classe chaque instruction selon le
tableau du §1 (`inline`, `helper`, `fp`, `sortie`).

    QEMU_EXTRA="-plugin tools/tcg/libppcmix.dylib,out=/tmp/mix.txt,interval=10" …
    python3 tools/tcg/ppcmix.py /tmp/mix.txt 200 400

### 2.2 Marble Blast

SMP=2, `x-sr-tlb=on`, fenêtre de 200 s au milieu de la passe de mesure de la démo
(`bench/tcg/res/mix-mb-bilan.txt`). Le greffon ralentit l'invité (534 M instructions/s
comptées) ; les **proportions** sont celles du jeu.

| Classe (§1) | part des instructions exécutées |
|---|---|
| en ligne | **90,96 %** |
| helper | 4,72 % |
| flottant scalaire (helpers softfloat) | 1,97 % |
| sortie de bloc (`blr`, `bctr`, `rfi`, `sc`, `isync`, `mtmsr`) | 2,35 % |
| **AltiVec, tout compris** | **3,07 %** — dont `lvx`/`stvx` **2,97 %** (en ligne, dans le memcpy de la commpage) et **0,10 %** en helper (`vperm` 0,045 %, `vmaddfp` 0,011 %…) |
| commpage (`pc >= 0xffff8000`) | 7,83 % |

Les helpers, par instruction : `lfs` 1,83 %, `stfs` 0,86 %, `stmw` 0,62 %, `lmw` 0,62 %,
`lfsx` 0,19 %, `dcbz` 0,15 %, `mftb` 0,12 %, `stfsx` 0,09 %, `sraw` 0,05 %, `vperm` 0,05 %.
Tête du classement : `bc` 15,1 %, `addi` 12,6 %, `lwz` 10,3 %, `cmpi` 4,7 %, `or` 4,4 %.

Ce qui compte pour la suite, par seconde (instructions système) :

| | / s |
|---|---|
| `mtsrin` (changement de registre de segment) | **213 000** |
| `mtmsr` | 208 000 |
| `rfi` | 55 700 |
| `sc` (dont 33 600 dans la commpage) | 42 500 |
| `isync` | 942 000 |
| `tlbie` (+ `tlbsync`) | 1 130 |
| `mtspr` DBAT | 1 200 |

Soit ~9 `mtsrin` par vidage complet du TLB en stock (23 000/s, §2.3) : **une entrée ou
sortie du noyau recharge ses registres de segment d'un bloc, et chaque bloc coûtait un
vidage de tout le TLB.** Les `tlbie` (~1 100/s) et les BAT expliquent les ~1 600 vidages
complets qui restent avec `x-sr-tlb`.

**AltiVec sur Marble Blast : 0,1 % des instructions en helper.** Rendre `vperm`, `vsldoi` ou
le flottant AltiVec plus rapides n'y changerait rien de mesurable ; la question reste
entière pour DOOM 3 (`idSIMD_AltiVec`), §6.

### 2.3 Traductions, invalidations, vidages : `info jit`

`tools/tcg/jitpoll.py` relève `info jit` (moniteur HMP) toutes les 10 s, sans greffon.

| Marble Blast, SMP=2, en jeu | stock | `x-sr-tlb=on` |
|---|---|---|
| traductions (TB) / s | 10 à 30 (pics à 600 au changement de niveau) | idem |
| invalidations de TB / s | ~0 | ~0 |
| **vidages complets du TLB / s** | **~22 900** | **~1 600** (`tlbie`, BAT) |
| mmu_idx effectivement nettoyés / s | ~39 000 | ~12 500 |
| (bureau au repos) | 4 800 vidages / s | |

Le code est chaud et stable (quelques dizaines de traductions par seconde) : **la
traduction ne coûte rien**, et `02-jmpcache-generation.patch` (générations du cache de
sauts) visait le bon symptôme — `tcg_flush_jmp_cache` à chaque vidage — mais pas sa cause.

### 2.4 Coût hôte : `sample` du processus QEMU

`sample <pid> 10` pendant la démo, `tools/tcg/samplesum.py` (par fil, inclusif par
fonction, self calculé sur l'arbre avec le code généré regroupé sous `???`).

Pourcentages du temps **occupé** des fils vCPU (hors `qemu_wait_io_event`) ; inclusif
(la fonction et ce qu'elle appelle), sauf « code généré » (self). Même scène (démo de
Marble Blast, ~100 s dans la passe de mesure), une passe chacun.

| | SMP=2 stock | SMP=2 `x-sr-tlb` | SMP=1 stock | SMP=1 `x-sr-tlb` |
|---|---|---|---|---|
| échantillons occupés (10 s, 1 ms) | 8 523 (2 fils à ~55 %) | 7 740 | 7 344 (1 fil à 100 %) | 7 649 |
| code généré par TCG (self) | 36,8 % | 30,6 % | 23,0 % | 28,9 % |
| `helper_lookup_tb_ptr` (chaque `blr`/`bctr`) | 22,1 % | 18,4 % | 28,1 % | 21,4 % |
| … dont `tb_htable_lookup` (cache de sauts vide) | 12,3 % | 8,9 % | **22,7 %** | 9,7 % |
| remplissage du TLB (`ppc_cpu_tlb_fill`) | 8,6 % | 5,2 % | **18,0 %** | 5,3 % |
| TLB d'instructions (`get_page_addr_code_hostp`) | 5,2 % | 3,8 % | 13,0 % | 4,2 % |
| vidage (`tlb_flush_by_mmuidx_async_work`) | 3,0 % | 1,5 % | 6,1 % | 1,4 % |
| … dont `tcg_flush_jmp_cache` | 2,3 % | 1,2 % | 4,4 % | 1,1 % |
| `helper_lmw` + `helper_stmw` | 7,0 % | 10,0 % | 6,0 % | 13,1 % |
| `probe_access_internal` (surtout lmw/stmw, dcbz) | 7,6 % | 6,5 % | 14,2 % | 8,3 % |
| BQL (`bql_lock_impl`) | 2,9 % | 9,2 % | 10,5 % | 10,2 % |
| `pthread_jit_write_protect_np` (entrée dans le code, W^X) | 1,8 % | 8,3 % | 2,1 % | 3,6 % |
| horloge (`cpu_get_clock`, `mftb`) | 1,5 % | 2,1 % | 0,7 % | 1,6 % |
| helpers flottants scalaires (softfloat compris) | 9,9 % ¹ | 2,3 % | 2,3 % | 3,0 % |
| AltiVec (tous les helpers `v*`, `lve*`, `stve*`) | 0,5 % | 0,1 % | 0,1 % | 0,1 % |

¹ Le premier échantillon (SMP=2 stock) a été pris la veille, ~70 s dans une passe, les trois
autres ~100 s dans la passe : pas exactement la même séquence de la démo (plus de physique),
d'où le flottant plus lourd. Les lignes TLB/cache de sauts, elles, suivent le mode, pas la
scène (même rapport entre SMP=1 stock et `x-sr-tlb`, pris le même jour).

Lecture :

- **Le vidage du TLB est le premier poste**, par ses conséquences plus que par lui-même :
  chaque vidage coûte le travail (3-6 %), puis les remplissages (TLB de données et
  d'instructions : 14-31 %) et les recherches de blocs dans la table de hachage parce que le
  cache de sauts a été vidé avec lui (12-23 %). En mono-cœur, c'est **plus de la moitié** du
  temps du vCPU. `x-sr-tlb` ramène ces trois postes à 5 %, 8-9 % et 9-10 %.
- **`helper_lookup_tb_ptr`** reste ensuite le premier helper (18-21 %) : chaque retour de
  fonction (`blr`) et chaque appel indirect sort du code chaîné. Chantier TCG générique.
- **BQL** : en SMP=2 avec `x-sr-tlb`, 9 % (plus 8 % d'attente de mutex) — `ppc_maybe_interrupt`
  et `cpu_interrupt_exittb` prennent le verrou global à **chaque** `mtmsr`/`rfi` qui touche
  EE, IR ou DR, c'est-à-dire à chaque entrée et sortie du noyau. Piste suivante (§7).
- **`lmw`/`stmw`** : 7-13 % — d'où l'essai 0002 (§5.4), qui montre que le helper n'est pas
  plus lent que l'équivalent en ligne : le coût est celui des accès eux-mêmes.
- **AltiVec et flottant ne pèsent rien sur Marble Blast** (§2.2). DOOM 3 reste à profiler.
- Le gain de `x-sr-tlb` est plus grand que ce que « vidage + remplissage » laissait
  prévoir en SMP=2 sur le papier, et le temps libéré se retrouve en partie dans le BQL et
  l'entrée dans le code (`pthread_jit_write_protect_np` : la synchronisation globale de
  `tlbie`, §3.3, fait sortir les deux vCPU).
