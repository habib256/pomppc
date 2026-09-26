# TCG et le G4 émulé — relevé, A/B des cœurs, patches

Chantier « le G4 émulé lui-même » (`TODO.md` §4, 25/09/2026). Au-delà des lots du plugin,
le plafond des jeux est la vitesse à laquelle TCG exécute le code PowerPC. Ce document
relève **où part le temps du processeur émulé** (instructions exécutées, helpers, coût
hôte), mesure **1 contre 2 cœurs**, et décrit **les patches qui en sont sortis** :
`patches/tcg/0001-ppc-sr-tlb.patch` (propriété de CPU `x-sr-tlb`), puis, pour DOOM 3,
`0002-ppc-lfs-inline` (`x-lfs-inline`, §8), `0003-ppc-vfp-fast` (`x-vfp-fast`, §9) et
`0004-ppc-vperm-fast` (`x-vperm-fast`, §10), puis `0006-tcg-jit-near` (`x-jit-near`, §14) et
`0007-ppc-fp-inline` (`x-fp-inline`, le flottant scalaire simple sans ses helpers, §15), puis
`0008-tcg-ret-inline` (`x-ret-inline`, `x-jc-idx` : les sorties indirectes, §16) ; trois essais exacts mais sans gain,
`patches/tcg/essais/0002-ppc-lmw-inline.patch` (`x-lmw-inline`) et
`essais/0005-ppc-vfp-nrwg.patch` (`x-vfp-nrwg`, §11) et `essais/0009-ppc-isync-chain.patch`
(`x-isync-chain`, §16.8), ne sont pas appliqués.

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

---

## 3. Le patch 0001 : TLB gardé d'un jeu de segments à l'autre (`x-sr-tlb`)

### 3.1 Le défaut

Tiger (xnu PPC 32 bits) donne au noyau son propre espace d'adressage : il **recharge les
registres de segment** (`mtsr`/`mtsrin`) à l'entrée et à la sortie du noyau et à chaque
changement de tâche, et monte/démonte des fenêtres (segments de copie `copyin`/`copyout`).
Dans QEMU 9.2 (`target/ppc/mmu_helper.c`, `helper_store_sr`), toute écriture qui change un
registre de segment pose `TLB_NEED_LOCAL_FLUSH`, et le prochain point de synchronisation
(`isync`, `sync`, `rfi`, exception) **vide le TLB logiciel entier** — les 16 `mmu_idx` — et
**tout le cache de sauts** du vCPU. Ensuite chaque page touchée repasse par
`ppc_cpu_tlb_fill` → `ppc_hash32_xlate` (BAT, segment, recherche dans la table de hachage),
et chaque bloc par `tb_htable_lookup`. Un commentaire de l'amont l'assume (« invalidating
256 MB of virtual memory in 4 kB pages is way longer than flushing the whole TLB ») ;
mais le noyau ne fait qu'**alterner** entre quelques jeux de segments, et le TLB de
l'utilisateur est vidé pour revenir exactement au même jeu.

### 3.2 La conception

Sur un G4, seuls deux `mmu_idx` traduisent par segments : **0** (MSR[PR]=1, utilisateur)
et **1** (superviseur) ; 2 et 3 sont le mode réel, où les segments ne jouent aucun rôle.
Chaque `mmu_idx` traduit retient **le jeu de segments sous lequel ses entrées ont été
remplies** (`sr_snap[i][0..15]`), et n'est vidé que **quand il va servir sous un autre
jeu**.

Invariant (dans `cpu.h`) :

> `sr_snap_ok[i]` ⇒ toutes les entrées du TLB de `mmu_idx` *i* ont été remplies alors
> que `env->sr[0..15] == sr_snap[i][0..15]`.

| Événement | Stock | `x-sr-tlb` |
|---|---|---|
| `mtsr`/`mtsrin` change une valeur | `TLB_NEED_LOCAL_FLUSH` | `sr_gen++`, `TLB_NEED_SR_CHECK` (pas de vidage) |
| remplissage d'une entrée de *i* ∈ {0,1} | — | si `env->sr` ≠ `sr_snap[i]` : `sr_snap_ok[i] = false` (entrées mélangées) |
| point de synchronisation avec `SR_CHECK` | vidage complet | **contrôle** des `mmu_idx` courants (instruction et donnée) |
| changement de MSR (`hreg_compute_hflags` : exception, `rfi`, `mtmsr`) | — | **contrôle** des `mmu_idx` qui deviennent courants |
| contrôle de *i* | — | si `!sr_snap_ok[i]` ou `sr_snap[i]` ≠ `env->sr` : `tlb_flush_by_mmuidx(1 << i)` (vide aussi le cache de sauts), puis `sr_snap[i] = env->sr`, `ok` |
| vidage local complet (`tlbie`, SDR1…) | vidage | vidage, puis tous les `sr_snap` recalés (TLB vide) |
| `tlbie` | local | **local + global à la prochaine `sync`** (§3.3) |

`sr_gen` évite la comparaison de 16 mots dans le cas courant (`sr_snap_gen[i] == sr_gen` :
rien n'a changé depuis la dernière vérification). Le `mmu_idx` qui n'est pas courant garde
ses entrées : il n'est contrôlé qu'au moment où le CPU y revient, ce qui passe
nécessairement par un changement de MSR (seul moyen de changer de `mmu_idx` sur un G4 ;
aucune instruction 32 bits n'accède à la mémoire avec un autre `mmu_idx` que le courant).

**Pourquoi c'est juste.** (1) Entre l'écriture d'un registre de segment et la
synchronisation, les deux modes laissent servir les anciennes traductions — c'est
l'architecture (effet garanti seulement après `isync`/`rfi`/exception), et le code stock
fait exactement pareil. (2) À chaque point où le stock aurait vidé *i* et où le patch le
garde, `sr_snap[i] == env->sr` et toutes les entrées ont été remplies sous ce jeu : elles
sont celles que le remplissage recalculerait (les PTE n'ont pas changé, sinon il y aurait
eu `tlbie`, qui vide tout ; les BAT non plus, leurs écritures vident). (3) Le cache de
sauts n'est pas vérifié contre la page physique (`tb_lookup`) : il est vidé avec chaque
`mmu_idx` vidé, et une entrée de cache d'un autre `mmu_idx` porte d'autres `flags` (le
`mmu_idx` est dans les hflags), elle ne peut pas être prise. (4) Les `mmu_idx` 2/3 (mode
réel) ne dépendent pas des segments : ne plus les vider au changement de segment est exact.

### 3.3 SMP : `tlbie` devient global (un défaut latent de QEMU)

Premier passage du vérificateur (§4) en SMP=2 : une divergence, une entrée utilisateur
gardée dont la traduction avait disparu. Cause : dans QEMU 9.2, `tlbie` sur le MMU 32 bits
ne pose que `TLB_NEED_LOCAL_FLUSH` — **l'autre vCPU n'est jamais prévenu**, alors qu'un
7400 SMP diffuse `tlbie` (et `tlbsync` + `sync` attendent les autres processeurs). En stock,
le défaut est **masqué** : l'autre vCPU vide tout son TLB plusieurs milliers de fois par
seconde à son prochain changement de segment. Avec `x-sr-tlb`, ces vidages disparaissent :
le patch pose donc aussi `TLB_NEED_GLOBAL_FLUSH` sur `tlbie`, que la `sync` suivante
(`check_tlb_flush(env, true)`) transforme en `tlb_flush_all_cpus_synced`. Hors `x-sr-tlb`,
rien ne change (et le défaut latent reste — voir §7).

### 3.4 Ce qui n'a pas été fait

Le noyau alterne aussi, **en mode superviseur**, entre jeux de segments (fenêtres de copie,
tâche courante) : ~10 000 vidages d'un seul `mmu_idx` par seconde subsistent. Une seconde
étape donnerait **plusieurs étiquettes par mode** (les `mmu_idx` 4 à 7 sont libres sur un
G4 : le bit HV n'existe pas) : `mmu_idx` = mode + emplacement choisi par jeu de segments,
éviction du moins récent. Plus de code, plus de hflags ; à mesurer d'abord par un compteur
par `mmu_idx`.

---

## 4. La preuve de 0001 : `x-sr-tlb-verify`

Un changement de TLB ne se prouve pas par un test natif de fonctions : la preuve est un
**mode vérificateur** dans le binaire lui-même. `x-sr-tlb-verify=N` : une fois sur N, là
où le patch **garde** un `mmu_idx` que le stock aurait vidé, chaque entrée valide (table
principale et table des victimes) est **retraduite** par `ppc_xlate()` depuis la table des
pages, et la page physique comparée à celle que l'entrée garde. Pas de vérification tant
qu'un vidage local/global est en attente sur ce CPU (les entrées périmées y sont
légitimes, et le vidage est imminent). Une divergence imprime le CPU, le `mmu_idx`,
l'adresse, les deux pages et ce que les **autres** CPU ont en attente.

| Passe (SMP=2, `x-sr-tlb=on,x-sr-tlb-verify=64`) | contrôles gardés vérifiés | entrées retraduites | divergences |
|---|---|---|---|
| démarrage jusqu'au bureau (avant §3.3) | — | — | 1 |
| démarrage jusqu'au bureau (après §3.3) | 319 488 | 56 523 129 | **0** |
| démarrage + Marble Blast 110 + 240 s | 3 717 120 | 822 373 599 | 1 (cpu 1, `mmu_idx` 0) |

| démarrage + Marble Blast, **SMP=1** (patch final) | **3 453 952** | **936 658 825** | **0** |

La divergence restante en SMP est du type « l'autre CPU vient d'invalider la PTE et n'a pas
encore exécuté son `tlbie`/`sync` » : pendant cette fenêtre, l'architecture autorise
l'usage de l'entrée périmée (sur le matériel comme dans le stock). `x-sr-tlb-verify` ne
modifie rien à l'exécution, sauf le bit R des PTE retraduites que `ppc_hash32_xlate` pose
(comme le remplissage qui aurait suivi le vidage) : mode preuve seulement.

---

## 5. A/B

### 5.1 Protocole

`tools/tcg/mbab.sh` (disque de dev uniquement) : une config = un démarrage de la VM
(bureau, `x-fast-fp=on`), un lancement de chauffe de Marble Blast (110 s), puis `NPASS`
passes de 240 s (`tools/guest/jobs/tcgmb`, bilan `POMPPC_GL_STATS` toutes les 5 s),
arrêt propre. Configs **entrelacées** : `s2off s2on s1off s1on s2on s2off s1on s1off`
(deux manches, ordre inversé), 2 passes chacune — **4 passes, 188 fenêtres par config**.
`tools/tcg/mbpair.py` apparie les fenêtres de jeu (≥ 1 000 triangles/image) dont les
triangles/image diffèrent de moins de 2 % ; `tools/tcg/mbreport.sh` fait le bilan. Même
binaire des deux côtés (`qsr`, la propriété seule change).

### 5.2 1 contre 2 cœurs (TODO §5 « SMP », seuil +15 %)

| | img/s, paires | rapport |
|---|---|---|
| stock : SMP=2 → SMP=1 | 66,0 → 61,6 | **−6,7 %** (médiane des paires −7,7 %, 129 paires) |
| `x-sr-tlb` : SMP=2 → SMP=1 | 71,8 → 67,0 | −6,8 % (131 paires) |

**Deux cœurs rapportent ~7 % sur Marble Blast, sous le seuil de +15 %.** Le jeu est
monofil ; le second vCPU prend le WindowServer, le noyau et le fil de son. Pendant la démo,
chaque fil vCPU est occupé ~55 % du temps (échantillons hors `qemu_wait_io_event`), la
somme ≈ 1,1 cœur hôte. Le coût connu de SMP=2 est la panique AppleUSBOHCI au démarrage
(~1/10) et, désormais, le défaut `tlbie` du §3.3 (sans `x-sr-tlb`, masqué).

### 5.3 `x-sr-tlb`

| | manche 1 | manche 2 | **ensemble** (4 passes/côté) |
|---|---|---|---|
| SMP=2 : off → on | +9,6 % | +10,0 % | **+9,8 %** (109 paires ; médiane +8,4 %, quartiles +5,6 / +12,8 %) |
| SMP=1 : off → on | −0,6 % | +20,5 % | +9,5 % (110 paires ; quartiles −0,7 / +20,1 %) |

En SMP=2 la mesure est reproductible (deux manches à 0,4 point). En SMP=1 elle ne l'est
pas : la même config varie de 10 % d'une manche à l'autre (off : −9,5 % de r1 à r2 ; on :
+11,4 %), le mode mono-cœur (TCG `thread=single`, `qemu-system-ppc`) est plus sensible à
son démarrage. Le gain moyen y est le même, mais il faut plus de manches pour le
conclure.

### 5.4 Le patch 0002 : `lmw`/`stmw` en ligne (`x-lmw-inline`)

Le profil (§2.4) met `helper_lmw` + `helper_stmw` à 7-13 % du temps occupé : Apple GCC et
CodeWarrior sauvent les registres non volatils par `stmw r13-r31` en prologue et les
rechargent par `lmw` en épilogue. Essai : `x-lmw-inline` (`patches/tcg/essais/0002`) traduit
`lmw`/`stmw` en `32 − r` accès mot en ligne **quand `[EA, EA + 4·(32 − r))` tient dans une
page** — la même traduction et les mêmes droits pour chaque mot, donc soit le premier
accès fautif et rien n'est fait, soit aucun : le tout-ou-rien du helper (qui sonde toute la
plage avant d'écrire quoi que ce soit) est conservé ; à cheval sur deux pages, le helper.

Preuve : `tools/guest/jobs/lmwtest` exécute les vraies instructions dans l'invité — `lmw`
et `stmw` de r13…r31 à 16 décalages (alignés, non alignés, dans une page, à cheval), les
registres relus un par un, la zone autour relue ; puis des fautes à cheval sur une page
protégée (`PROT_READ` pour `stmw`, `PROT_NONE` pour `lmw`). **Sortie identique octet pour
octet** entre `x-lmw-inline=off` et `=on` (empreinte FNV `2575eafce78adf66`, 608 cas + 14
fautes ; « octets écrits avant la faute : 0 » dans les deux modes). Un `sample` pendant le
banc confirme que le chemin en ligne est pris (plus aucun `helper_stmw`/`helper_lmw`).

Gain : **banc de 20 millions de paires `stmw`/`lmw` de 19 registres : 1 564 et 1 582 ms →
1 544 et 1 548 ms (−1,3 %)**. Le chemin rapide du helper (`probe_contiguous` puis une boucle
de `ldl_be_p`) coûte autant que 19 accès TCG avec leur contrôle de TLB en ligne ; le
coût est celui des accès. Sur Marble Blast, où ces instructions pèsent 7-13 % du temps, le
gain attendu est de l'ordre du pour-cent, sous le bruit de mesure (quartiles ±3 %) :
**piste fermée, patch non appliqué** (gardé dans `patches/tcg/essais/`, il s'applique
par-dessus 0001).

---

## 6. Ce qui attend la VM quotidienne (DOOM 3)

Tout est prêt ; il faut l'accord de l'utilisateur pour arrêter/relancer `tiger.qcow2`.

    # une partie : arrêt propre, relance avec le binaire v19 de la copie, mesure T+50..T+280
    bash tools/tcg/d3run.sh d3-s2off 2 0 1     # SMP=2, x-sr-tlb éteint, sample hôte à T+300
    bash tools/tcg/d3run.sh d3-s2on  2 1 1     # SMP=2, x-sr-tlb allumé
    bash tools/tcg/d3run.sh d3-s1off 1 0
    bash tools/tcg/d3run.sh d3-s1on  1 1
    # … deux parties par mode au moins, entrelacées ; profil d'instructions :
    EXTRA_ARGS="-plugin $PWD/tools/tcg/libppcmix.dylib,out=$PWD/bench/tcg/d3-mix.txt,interval=10" \
        bash tools/tcg/d3run.sh d3-mix 2 1
    python3 tools/tcg/ppcmix.py bench/tcg/d3-mix.txt <début> <fin>   # instantanés de la fenêtre
    # puis rendre la VM :
    bash tools/tcg/d3run.sh --restore          # binaire de référence, SMP=2, FASTFP défaut

Le binaire `~/src/qemu-tcg19/build/qsr` porte le `qgpu` v19 **exact** du binaire de
référence (fichiers copiés de `~/src/qemu/hw/display/`, `qgpu_proto.h` identique à celui de
`main`) : le plugin `20260924-liste` l'accepte. `target/ppc` y est identique octet pour
octet à celui de `~/src/qemu-tcg` (0001, et l'essai 0002 éteint).

`meas-tcg.sh` (invité) reprend `meas3.sh` du lot 3 avec une détection de T qui ne dépend
plus d'un seuil absolu de 85 ms/image (qui cesserait de marcher si l'émulateur accélérait) ;
`d3win.py` affine T au saut lui-même et imprime aussi le T de la règle du lot 3 : sur les
journaux du lot 3, les deux coïncident à une image près et redonnent 86,5 et 85,2 ms/image.
Reste à faire sur DOOM 3 : (1) l'A/B cœurs et `x-sr-tlb` ; (2) le profil `ppcmix` (part
d'AltiVec dans `idSIMD_AltiVec`, et donc l'intérêt de `NO_RWG` et de `vsldoi`/`vmrg` en
ligne, §1.1) ; (3) `sample` hôte.

---

## 6 bis. DOOM 3 joué (25/09/2026)

Binaire `~/src/qemu-tcg19/build/qsr64`, plugin `20260924-liste`, `tools/tcg/d3run.sh`,
fenêtre T+50..T+280 après la cinématique, parties entrelacées ; journaux dans
`bench/tcg/d3/` (non versionné).

| SMP=2 | parties (ms/image) | médiane | moyenne |
|---|---|---|---|
| `x-sr-tlb` éteint | 93,5 84,5 83,1 95,7 95,8 84,0 | 89,0 | 89,4 |
| `x-sr-tlb` allumé | 80,0 92,3 78,4 79,6 80,1 92,3 | 80,1 | 83,8 |

Chaque partie tombe dans un régime rapide ou lent, uniforme sur toute la fenêtre (médiane =
moyenne dans la partie), sans lien avec le mode : éteint ~84 / ~95, allumé ~79 / ~92. Le
patch déplace les deux régimes (−6 % et −3 %) ; le tirage des régimes fait l'écart de
médiane (−10 %). Cause des régimes non trouvée (placement des fils vCPU sur les cœurs de
l'hôte ?). Les compteurs `info jit` montrent 3× moins de vidages et de remplissages du TLB.

SMP=1, une partie par mode : 74,6 (éteint), 103,2 (allumé) — non concluant, à rejouer.

Profil d'instructions (`ppcmix`, SMP=2, `x-sr-tlb`, 50 s de jeu, 598 M instr./s) :
inline 79,9 %, helper 12,6 %, flottant 5,5 %, sorties 2,0 %. AltiVec **5,3 %** (helper
2,8 % : `vperm` 1,0, `vmaddfp` 0,5, `vmrghw` 0,4, `vmrglw` 0,3, `vaddfp` 0,2 ; en ligne :
`lvx` 1,4, `stvx` 0,7). Premier helper : **`lfs` 5,5 % et `stfs` 2,5 %** (+ `lfsx`/`stfsx`
0,8 %), avant AltiVec. `mtsrin` 50 000/s contre 213 000 sur Marble Blast.

`timedemo` : la démo de DOOM 3 (mode restreint) refuse une démo hors de ses archives
(`couldn't open demos/ab.demo`) et une archive ajoutée (`Sys_Error: Corrupted zz_ab.pk4`) :
pas de rejeu déterministe possible, d'où les parties réelles.

Suites dans l'ordre du profil : `lfs`/`stfs` en ops TCG (cas normal en ligne), puis
`vperm`/`vmrg*` en gvec et `NO_RWG` sur les helpers flottants AltiVec.

## 7. Suites

- **`x-sr-tlb` par défaut** (`SRTLB=1` dans `run_tiger.sh`) après la mesure DOOM 3 et une
  semaine de jeu ; le vérificateur peut tourner en fond (`CPU_OPTS=x-sr-tlb-verify=1024`).
- **Le défaut `tlbie` en SMP stock** (§3.3) est indépendant du patch : à signaler en amont,
  et à corriger chez nous même sans `x-sr-tlb` (poser le global dans `ppc_tlb_invalidate_one`
  pour `POWERPC_MMU_32B` quand `-smp > 1`). Candidat pour la panique AppleUSBOHCI ~1/10.
- Étiquettes multiples par mode (§3.4) si le compteur montre que le superviseur alterne.
- **BQL à chaque `mtmsr`/`rfi`** : `ppc_maybe_interrupt` et `cpu_interrupt_exittb` prennent
  le verrou global — 208 000 `mtmsr` et 56 000 `rfi` par seconde sur Marble Blast ; 9-10 %
  du temps vCPU dans `bql_lock_impl` plus ~8 % d'attente de mutex. Piste : un chemin sans
  verrou quand `CPU_INTERRUPT_HARD` est déjà dans l'état voulu (à faire avec soin : l'état
  des interruptions est partagé avec le fil d'E/S).
- `helper_lookup_tb_ptr` : chaque `blr` passe par un helper (18-21 %) ; une pile de retours
  prédits est un chantier TCG générique, plus lourd.
- `lfs`/`stfs` (2,7 % des instructions sur Marble Blast, 8,8 % sur DOOM 3) : fait, §8
  (`x-lfs-inline`, toutes les entrées en ligne, sans branchement).
- Le flottant et AltiVec ne sont pas le plafond sur Marble Blast ; **DOOM 3 peut dire
  autre chose** (`idSIMD_AltiVec`) : `ppcmix` sur `demo_mars_city1` en premier, avant
  tout patch AltiVec (`NO_RWG` sur les helpers flottants, `vsldoi`/`vmrg*` en ligne).

---

## 8. Le patch 0002 : `lfs`/`stfs` sans helper (`x-lfs-inline`)

### 8.1 Pourquoi

Profil DOOM 3 (§6 bis) : `lfs` 5,5 % et `stfs` 2,5 % des instructions exécutées, plus
`lfsx`/`stfsx`/`lfsu`/`stfsu` ~0,8 %, **toutes par un appel de helper** :
`gen_qemu_ld32fs` charge le mot puis appelle `helper_todouble` (la fonction DOUBLE de
l'ISA), `gen_qemu_st32fs` appelle `helper_tosingle` (SINGLE) puis range. Le `sample` hôte
d'une partie (`bench/tcg/d3/d3-s2on-a/sample.txt`, `x-sr-tlb`) leur donne **5,1 % du temps
occupé des vCPU en propre** (`helper_todouble` 3,4 %, `helper_tosingle` 1,7 %), sans compter
l'appel lui-même dans le code généré. Les deux helpers ne sont que des manipulations de
bits (`TCG_CALL_NO_RWG_SE`) : ils se traduisent en ops TCG entières.

### 8.2 La traduction

Une propriété de CPU, `x-lfs-inline` (défaut éteint, lue par le traducteur seulement :
`ctx->lfs_inline`), change les deux fonctions communes à `lfs lfsu lfsx lfsux` et `stfs stfsu
stfsx stfsux` (et aux `lxsspx`/`stxsspx` VSX, sans objet sur un G4). Le chargement et le
rangement restent **les mêmes accès** (même memop `MO_UL` à l'endianité du contexte, même
`mmu_idx`, même adresse) : une faute se produit au même endroit, avant ou après une
conversion qui n'a aucun effet de bord. **Aucun branchement** dans le bloc : un saut vers
le helper pour les cas rares aurait coûté, à chaque `lfs`, la fin de bloc de base de TCG
(globales resynchronisées puis relues) ; les cas rares sont calculés en ligne eux aussi.

`lfs` (DOUBLE), `u` = le mot chargé, étendu à 64 bits — 13 ops :

| | |
|---|---|
| `abs = u & 0x7fffffff`, `sign = (u ^ abs) << 32` | |
| `k = max(clz64(abs), 40)` | 40 pour un normal, un infini, un NaN ; 40 + s pour un dénormal (s = `clz32 − 8` du helper, 1..23) ; 64 pour 0 |
| `m = abs << (k − 11)` | la fraction ; le bit de poids fort d'un dénormal arrive au bit 52 |
| `e = (936 − k) << 52` | biais d'exposant 896 (1023 − 127) pour un normal, 896 − s pour un dénormal, dont le bit de tête ajoute le 1 implicite |
| `e = 0x700 << 52` si `abs ≥ 0x7f800000` | infini/NaN : 0xff + 0x700 = 0x7ff, fraction gardée, sNaN non calmé (comme le helper) |
| `e = 0` si `abs = 0` | zéro signé |
| `ret = (m + e) \| sign` | |

`stfs` (SINGLE : **pas d'arrondi**, la fraction est tronquée, comme le matériel) — 13 ops :

| | |
|---|---|
| `exp = x[62:52]` | |
| `hi = x[63:62] << 30 \| x[58:29]` | `exp > 896` : normaux simples et au-delà, infini, NaN, et les exposants hors plage que l'ISA laisse indéfinis, traités comme le helper |
| `lo = x[63] << 31 \| ((1 << 52 \| x[51:0]) >> min(926 − exp, 63))` | `exp ≤ 896` : dénormal pour 874 ≤ exp, zéro signé en dessous (un décalage ≥ 53 laisse 0) |
| `ret = exp > 896 ? hi : lo` | seuls les 32 bits bas sont rangés |

Tous les décalages restent dans [0, 63] : aucun comportement « non spécifié » de TCG. Les
ops choisies (`clz`, `umax`/`umin`, `movcond`, `extract`, `deposit`) ont toutes une
instruction arm64 (`clz`, `csel`, `ubfx`, `bfi`).

### 8.3 La preuve

**Hôte, exhaustive** (`tools/tcg/lfsproof.sh [arbre]`) : `helper_todouble` et
`helper_tosingle` sont **extraits tels quels** de `target/ppc/fpu_helper.c` de l'arbre, et
comparés à un modèle C des ops (une ligne par op, même ordre, mêmes sémantiques de TCG) ; le
script vérifie aussi que la suite des `tcg_gen_*` de l'arbre est celle du modèle.

| | cas | divergences |
|---|---|---|
| `lfs` : tous les motifs de float32 | **4 294 967 296** (2^32) | **0** |
| (contrôle : helper = conversion `float → double` du FPU hôte, hors NaN) | 4 278 190 082 | 0 |
| `stfs` : tous les mots hauts d'un float64 (signe, exposant, 20 bits hauts de fraction) × 8 mots bas | **34 359 738 368** (2^35) | **0** |
| `stfs` : chaque exposant × signe × fractions `1<<b`, `(1<<b)−1`, `~0>>b` | 651 264 | 0 |

6 s sur le M4 (16 fils). Contre-épreuve : cinq mutations du modèle (seuil de dénormal, test
d'infini, borne de `stfs`, `umax`, largeur du `deposit`) sont toutes détectées (2 à
14 663 286 784 divergences chacune).

**Invité** (`tools/guest/jobs/lfstest`) : les vraies instructions, dans Tiger, SMP=2,
comparées à une référence entière (le code des helpers recompilé par le GCC de l'invité) et
hachées : `lfs` + `stfd` sur les **2^32 motifs**, `lfd` + `stfs` sur 2^32 float64 (chaque mot
haut, mot bas dérivé), les 651 264 cas limites, les six formes `x`/`u`/`ux` sur 2^22 motifs
chacune (adresse de mise à jour relue), et les fautes (`lfs` d'une page `PROT_NONE`, `stfs`
vers une page `PROT_READ` : signal, mémoire intacte).

| `x-lfs-inline` | divergences avec la référence | empreinte |
|---|---|---|
| éteint | 0 (sur 8 590 586 624 conversions + formes) | `5bb38b919aa9a8b0` |
| allumé | 0 | `5bb38b919aa9a8b0` |

**Sortie identique octet pour octet** (hors ligne du banc). Banc (1 milliard de paires
`lfs`/`stfs`, boucle C déroulée par 4) : **3 622 → 2 000 ms (−45 %)**, soit ~1,6 ns de moins
par paire : le chemin en ligne est bien pris.

### 8.4 Marble Blast : non mesurable

Protocole du §5.1 (`tools/tcg/mbab.sh`, copie APFS du disque de dev, binaire `qsr15` =
même `target/ppc` avec le `qgpu` v15 du disque de dev, SMP=2, `x-fast-fp` et `x-sr-tlb`
des deux côtés, VM quotidienne au repos à 7 % d'un cœur), **5 démarrages par mode**,
entrelacés `ref lfs lfs ref ref lfs` puis `lfs ref ref lfs` (démarrages 7-10 : même binaire
plus 0003-0005, éteints), 2 passes de 240 s chacun ; journaux `bench/tcg/res/{r,q}*`.
Moyenne des fenêtres de jeu de chaque passe (img/s) :

| démarrage | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| mode | ref | lfs | lfs | ref | ref | lfs | lfs | ref | ref | lfs |
| passes | 63,7 / 65,6 | 71,8 / 72,2 | 71,2 / 70,7 | 66,5 / 65,7 | 72,3 / 72,2 | 68,0 / 68,2 | 73,4 / 72,7 | 66,2 / 65,8 | 73,6 / 72,6 | 68,9 / 68,5 |

**Chaque démarrage tombe dans un régime lent (~66 img/s) ou rapide (~72-73), uniforme sur
ses deux passes, sans lien avec le mode** — le même phénomène que les deux régimes de
DOOM 3 (§6 bis), vu ici pour la première fois sur Marble Blast. Toutes paires confondues :
+3,4 % (309 paires, médiane +2,2 %, quartiles −3,8 / +8,9 %), ce qui ne dit que le tirage
des régimes. À régime égal : rapide ref 72,3 / 73,1 contre lfs 72,0 / 71,0 / 73,1 (≈ 0) ;
lent ref 64,7 / 66,1 / 66,0 contre lfs 68,1 / 68,7 (+4 %). Marble Blast n'a que 2,7 %
d'instructions `lfs`/`stfs` : l'attendu (~1 %) est sous la dispersion des régimes. **Le gain
de 0002 sur Marble Blast n'est pas mesurable** avec ce protocole ; le banc (§8.3) l'établit
à l'échelle de l'instruction, DOOM 3 (8,8 % de `lfs`/`stfs`) est le vrai juge.

La cause des régimes devient la question de métrologie numéro un (TODO §4) : un
démarrage entier est lent ou rapide, sur Marble Blast comme sur DOOM 3.

### 8.5 DOOM 3

À faire sur la VM quotidienne (§12). Attendu : les deux helpers pèsent 5,1 % du temps
occupé des vCPU en propre ; le banc dit ~0,8 ns gagnées par instruction, soit, à ~50 M
`lfs`/`stfs` par seconde, ~40 ms de vCPU par seconde : **−3 à −5 % de ms/image**.

---

## 9. Le patch 0003 : flottant AltiVec à 4 voies (`x-vfp-fast`)

### 9.1 Pourquoi

Même `sample` : `helper_vmaddfp` **5,1 %** du temps occupé (dont `float32_muladd` 5,0 % en
propre), `helper_vaddfp` 0,9 %, pour 0,5 % et 0,2 % des instructions — environ **18 ns par
`vmaddfp`**. Sous `x-fast-fp`, `float32_muladd` calcule déjà chaque voie sur le FPU hôte
(hardfloat) ; ce qui coûte, c'est quatre appels, et dans chacun `can_use_fpu`, le contrôle
des entrées, `fmaf`, le contrôle du résultat, un aller-retour entre registres entiers et
flottants.

### 9.2 La conception

`x-vfp-fast` (défaut éteint, lu à l'exécution par le helper) : `vaddfp`, `vsubfp`,
`vmaddfp`, `vnmsubfp` essaient d'abord `vfp_add4` / `vfp_fma4` (`int_helper.c`), qui
prennent **exactement la décision par voie de `float32_gen2()` / `float32_muladd()`**
(`fpu/softfloat.c`), pour les quatre voies d'un coup :

| | condition (sinon : voie « logicielle ») |
|---|---|
| `can_use_fpu` | `!no_hardfloat`, inexact déjà posé, arrondi au plus proche, pas de re-biaisage — constant sur les 4 voies (une voie peut poser des drapeaux, jamais en effacer) |
| entrées | toutes zéro ou normales (un dénormal part au logiciel ; sous NJ, softfloat l'aurait d'abord mis à zéro : la boucle d'origine le fait) |
| résultat add/sub | infini → drapeau overflow ; `|r| ≤ FLT_MIN` sauf deux entrées nulles → logiciel |
| résultat muladd | `a` ou `c` nul → gardé (produit nul exact, softfloat l'ajoute sur l'hôte aussi) ; infini → overflow ; `|r| ≤ FLT_MIN` → logiciel |

Si **les quatre** voies passent, les résultats sont ceux de l'hôte et le seul drapeau que
hardfloat pourrait poser (overflow) est posé pareil ; sinon la fonction rend faux sans rien
avoir écrit, et le helper exécute sa boucle d'origine. Résultats **et drapeaux** sont donc
ceux d'avant dans tous les cas, par construction. Sans `x-fast-fp`, `no_hardfloat` est posé :
le chemin rapide n'est jamais pris. Hôte x86 : pas de `fmaf` rapide (softfloat peut y forcer
la FMA logicielle, `force_soft_fma`), seulement add/sub.

### 9.3 La preuve

**Hôte** (`tools/tcg/vfpproof.sh [arbre] [N]`) : `vfp_*` **extraites telles quelles**
d'`int_helper.c`, liées au **vrai** `fpu_softfloat.c.o` de l'arbre (avec les `-I/-D` de sa
compilation) ; pour chaque vecteur, « helper patché » contre « boucle d'origine » sur deux
`float_status` identiques au départ : 4 résultats au bit près **et** `float_status` entier
(drapeaux) égaux. Les 8 états (`no_hardfloat` × amorcé × NJ), 4 opérations ; catalogue de 64
valeurs limites croisé (64² pour add/sub, 64³ pour les FMA, plus l'annulation exacte
`b = −a·c`), et des vecteurs aléatoires (une moitié à voies « douces » pour que le chemin
rapide soit pris, l'autre avec extrêmes, zéros signés, dénormaux, infinis, NaN calmes et
signalants, annulations).

| N = 20 000 000 par (opération, état) | vecteurs | par le chemin rapide | divergences |
|---|---|---|---|
| vaddfp + vsubfp + vmaddfp + vnmsubfp | **648 454 144** (2 593 816 576 voies) | 97 486 278 | **0** |

19 s sur le M4. Contre-épreuve : cinq mutations (condition « deux zéros », `<` au lieu de
`≤ FLT_MIN`, drapeau overflow oublié, dénormaux admis, amorçage ignoré) sont toutes
détectées (388 à 10 521 534 divergences).

**Invité** (`tools/guest/jobs/vfptest`, `-faltivec`) : les vraies instructions, VSCR[NJ] à 0
puis à 1, catalogue de 40 valeurs croisé sur une voie (40³) et 2^22 vecteurs aléatoires par
valeur de NJ, 4 instructions chacun ; plus la partie `vperm` du §10. **Empreinte identique**
(`f3a6de5986c49679`) avec `x-vfp-fast` et `x-vperm-fast` éteints, allumés, et allumés avec
l'essai `x-vfp-nrwg` (§11).

Banc (50 M × (2 `vmaddfp` + `vaddfp` + `vsubfp`), chaîne dépendante, valeurs normales) :
**1 585 → 1 313 ms (−17 %)**, ~1,4 ns par instruction. Moins que les ~18 ns du profil ne le
laissaient espérer : dans le banc tout est chaud ; le reste du coût est l'appel lui-même
(helper sans drapeau : globales resynchronisées) et les accès aux AVR.

### 9.4 Attendu sur DOOM 3

`vmaddfp` + `vaddfp` : ~5 M/s ; à 1,4 ns (banc) ~0,7 %, jusqu'à ~2 % si le gain en jeu
suit les 18 ns du profil. Marble Blast n'exécute presque pas d'AltiVec flottant (§2.2) : pas
d'A/B sur lui.

---

## 10. Le patch 0004 : `vperm` par table (`x-vperm-fast`)

`helper_VPERM` (`vperm` : 1,0 % des instructions de DOOM 3, 2,7 % du temps occupé) boucle
sur 16 octets avec un choix `a`/`b` par octet. `x-vperm-fast` (lu par le traducteur :
`ctx->vperm_fast`) fait appeler à la place `helper_VPERM_FAST` (même drapeau
`TCG_CALL_NO_RWG`) : une consultation dans une table de 32 octets. Sur un hôte petit-boutiste,
`VsrB(i)` est `u8[15 − i]` ; avec `T = b.u8[0..15]` puis `a.u8[0..15]`,
`résultat.u8[j] = T[31 − (c.u8[j] & 31)] = T[~c.u8[j] & 31]` — sur arm64, **un seul `tbl`**
sur deux registres (`vqtbl2q_u8`), plus un `mvn` et un `and`. Version C portable pour les
autres hôtes (et grand-boutiste). Les trois opérandes sont lus avant l'écriture de `r`.

Preuve hôte (`tools/tcg/vpermproof.sh`) : `helper_VPERM` et `helper_VPERM_FAST` extraits
tels quels, comparés sur chaque valeur d'octet de contrôle (256) à chaque position (16) × 64
couples aléatoires, 50 M vecteurs aléatoires et les recouvrements `r = a`, `r = b`,
`r = c`, `a = b = c = r` : **50 662 144 cas, 0 divergence** ; deux mutations (masque 15,
table inversée) détectées. Invité : partie `vperm` de `vfptest` (chaque octet de contrôle à
chaque position, 2^22 × 2 vecteurs aléatoires dont `r = a = c`), empreinte identique.
Banc (50 M × 4 `vperm` dépendants) : **707 → 474 ms (−33 %)**, ~1,2 ns par `vperm`.
Attendu sur DOOM 3 : ~6 M `vperm`/s, **~0,7 %**.

---

## 11. Essai 0005 : les helpers flottants AltiVec en `NO_RWG` (`x-vfp-nrwg`)

`vaddfp`/`vsubfp`/`vmaddfp`/`vnmsubfp` sont déclarés sans drapeau : TCG resynchronise toutes
les globales avant l'appel et les relit après. Ils n'en touchent aucune (AVR et `vec_status`
vivent dans `env` hors globales, aucune exception possible) : `TCG_CALL_NO_RWG` est licite.
`patches/tcg/essais/0005-ppc-vfp-nrwg.patch` ajoute des jumeaux `*_nrwg` choisis au
traduction sous `x-vfp-nrwg`. Même empreinte `vfptest` ; banc 1 313 → 1 346 ms (**aucun
gain**, dans le bruit) : dans une boucle AltiVec il y a peu de globales vivantes à
resynchroniser. **Non appliqué** (s'applique par-dessus 0004).

---

## 12. Ce qui attend la VM quotidienne (DOOM 3)

Binaire : `~/src/qemu-tcg19/build/qsr64` (branche `fp-inline` de la copie : base, 0001,
`qgpu` v19 **identique au binaire de référence**, 0002, 0003, 0004, essai 0005 — toutes les
propriétés nouvelles éteintes par défaut). Le `d3run.sh` passe l'environnement à
`run_tiger.sh`, donc `CPU_OPTS` suffit (il marche aussi avec le `run_tiger.sh` de `main`) :

    # référence : x-sr-tlb seul (défaut de run_tiger.sh)
    bash tools/tcg/d3run.sh d3-ref 2 1
    # les trois ensemble d'abord
    CPU_OPTS=x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on bash tools/tcg/d3run.sh d3-all 2 1
    # … 6 parties par mode, entrelacées (ref all all ref ref all …), médiane ;
    # si le total gagne, 0002 seul pour l'attribuer :
    CPU_OPTS=x-lfs-inline=on bash tools/tcg/d3run.sh d3-lfs 2 1
    bash tools/tcg/d3run.sh --restore

Attendu : 0002 −3 à −5 %, 0003 −0,7 à −2 %, 0004 ~−0,7 % ; **les trois : −4 à −7 % de
ms/image**, à lire contre les deux régimes par partie du §6 bis (~6 % d'écart à eux seuls).

Piège vu en construisant ces binaires : **recopier un binaire signé par-dessus un fichier
existant** (`cp qemu-system-ppc64 qsr64` sur un `qsr64` déjà lancé une fois) le fait tuer au
lancement par macOS (`SIGKILL (Code Signature Invalid)`, rapport dans
`~/Library/Logs/DiagnosticReports/`, `qemu.log` vide) : `rm` puis `cp`. Les binaires de la
copie : `qsr`/`qsr64` (`qgpu` v19, VM quotidienne) et `qsr15`/`qsr1564` (`qgpu` v15, disque
de dev), même `target/ppc`.

## 13. DOOM 3 joué avec `tcg/0002-0004` (25/09/2026, soir)

Binaire `~/src/qemu-tcg19/build/qsr64`, SMP=2, `x-sr-tlb` des deux côtés, six paires
entrelacées (ordre alterné), `demo_mars_city1` T+50..T+280 ; journaux `bench/tcg/d3/fp-*`.

| Mode | parties (ms/image) | médiane | moyenne |
|---|---|---|---|
| référence | 80,9 80,1 92,9 93,1 80,4 80,7 | 80,8 | 84,7 |
| `x-lfs-inline,x-vfp-fast,x-vperm-fast` | 80,4 80,2 79,7 73,9 79,6 74,4 | 79,7 | 78,0 |

- **Régime rapide contre régime rapide** (4 référence, 4 patches) : 80,5 contre 80,0 de
  moyenne, **−0,7 %**, dans le sens attendu mais sous le bruit.
- **Répartition des régimes** : la référence a 4 parties à ~80 et 2 à ~93 ; les patches ont 4
  à ~80 et **2 à ~74**, un niveau jamais vu sur les 30 parties jouées depuis le 25/09 matin, et
  aucune à ~93. Hypothèse : le régime lent et le niveau à 74 sont le même « tirage » de
  l'hôte, que les patches font passer de 93 à 74 (−20 %) — ce qui voudrait dire que le régime
  lent est plus sensible au flottant. Non prouvé : 2 parties sur 6 de chaque côté, écart dans
  les deux sens possible par hasard.
- `submit`/`wait` du plugin identiques d'un mode à l'autre : le gain, s'il existe, est côté
  processeur.

Conclusion : **aucune régression** (preuves exhaustives, images justes, zéro repli en plus),
gain **entre 0,7 % et 8 %** selon qu'on lit la médiane ou la moyenne. Le trancher exige de
comprendre les régimes (TODO §4) ; un test direct : forcer le régime (fils vCPU épinglés,
`taskpolicy`) et rejouer six paires dans chaque régime. **Cause trouvée au §14** : le
placement du tampon du JIT ; le §14.5 relit ces chiffres.

## 14. Les deux régimes (25/09/2026, nuit)

**En une phrase** : à chaque lancement, macOS pose le tampon du JIT de QEMU (1 Gio, `MAP_JIT`)
soit dans la **même fenêtre de 4 Gio** que le texte de QEMU (`0x1xxxxxxxx`), soit à
**`0x300000000`** (un lancement sur deux à trois) ; dans le second cas, chaque appel de helper
depuis le code généré et chaque retour est un saut indirect dont la cible n'a pas les mêmes
bits 63..32 que le saut, et **l'Apple M4 prédit ces sauts-là plus lentement** (+0,4 à 1,5 ns
par appel). Tout le processus perd **~6 % sur Marble Blast**, davantage sur DOOM 3. Le
correctif `tcg/0006` (`x-jit-near`, `JITNEAR=1`) pose le tampon près du texte : le régime
lent disparaît.

Journaux : `bench/reg/` du worktree de l'agent (non versionné : `res/`, `camp1-bilan.txt`,
`campagne{1,2}.log`). Outils : `tools/tcg/regab.sh` (un processus QEMU neuf par étiquette :
bureau, `vmmap`, job `tools/guest/jobs/regime` = micro-banc `regbench` puis Marble Blast
110 s de chauffe + 150 s de mesure, redémarrage **propre de l'invité dans le même
processus**, second job, arrêt), `tools/tcg/regidx.py` (indice de régime : médiane des
rapports img/s aux fenêtres de même triangles/image des cinq démarrages rapides du §8.4 ;
1,00 = rapide), `tools/tcg/regreport.py`, `tools/tcg/jitwhere.sh`, `tools/tcg/farcall.c`.
Disque : copie APFS de `tiger-dev.raw` ; toutes les propriétés de `run_tiger.sh` allumées
(`x-fast-fp`, `x-sr-tlb`, `x-lfs-inline`, `x-vfp-fast`, `x-vperm-fast`) ; VM quotidienne au
repos (7-8 % d'un cœur, `top` dans chaque `res/*-info.txt`).

### 14.1 Le régime suit le processus hôte, pas l'invité

Campagne 1 (SMP=2, binaire `qsr1564`, sans patch de placement) : 7 processus, 2 démarrages de
l'invité chacun (`shutdown -r` entre les deux, même processus QEMU). Base du tampon du JIT
relevée par `vmmap` au bureau.

| processus | tampon du JIT | fenêtre du texte ? | indice démarrage 1 / 2 | img/s 1 / 2 |
|---|---|---|---|---|
| p00 | `0x300000000` | **autre** | 0,957 / 0,962 | 69,5 / 69,8 |
| p01 | `0x300000000` | **autre** | 0,929¹ / 0,954 | 66,7 / 68,9 |
| p02 | `0x128e04000` | même | 1,007 / 1,024 | 72,8 / 74,1 |
| p03 | `0x300000000` | **autre** | 0,953 / 0,960 | 68,8 / 69,2 |
| p04 | `0x127604000` | même | 1,003 / 1,003 | 72,2 / 72,9 |
| p05 | `0x11e604000` | même | 1,011 / 1,022 | 73,0 / 74,4 |
| p06 | `0x11e604000` | même | 1,002 / 1,008 | 72,4 / 73,0 |

¹ `sample` hôte de 10 s pendant la passe (p01 à p06 : dans la passe du 1er démarrage).

- **Le redémarrage de l'invité ne change jamais le régime** (7 sur 7, écart intra-processus
  ≤ 2,5 %) : l'état pris par Tiger au démarrage (pages physiques, commpage, ordonnanceur,
  vCPU au repos) est hors de cause — hypothèse 3 éliminée, sans avoir besoin de `savevm`.
  Le régime est une propriété du **processus QEMU**.
- **Le placement du tampon du JIT prédit le régime sans erreur** (14 démarrages sur 14) :
  fenêtre autre ⇒ indice 0,93-0,96 (68,8 img/s de moyenne), même fenêtre ⇒ 1,00-1,02
  (73,1) ; **+6,2 %**.
- Le micro-banc `regbench` (appels `bl`/`blr`, lectures sur 64 Mo, `fmadds`, `getppid`,
  ping-pong par tube, `memcpy`) est **identique dans les deux régimes** (`call` 616-643 ms,
  `fp` 610-650, `sys` 1 870-2 040, `ctx` 3 850-4 110) : il ne sert pas d'indicateur. Le
  §14.3 dit pourquoi : ses boucles n'ont que deux ou trois sites d'appel.
- `sample` hôte (p01 lent contre p02 rapide, fil vCPU le plus chargé) : **mêmes
  proportions** — code généré 55,8 / 56,5 %, `helper_lookup_tb_ptr` 14,6 / 14,1 %, helpers
  28,4 / 27,1 %, verrous 3,2 / 3,1 %, occupation 61 / 62 %. Le profil ne change pas, tout est
  plus lent : signature d'un coût matériel uniforme, pas d'un autre chemin logiciel.

### 14.2 Où le noyau pose le tampon

`tools/tcg/jitwhere.sh` lance QEMU arrêté (`-S`) et relit sa carte : sur 12 lancements, 8 dans
la fenêtre du texte (`0x107…`-`0x128…`), **4 à `0x300000000`**. Un programme C de dix lignes
(`mmap` de 1 Gio `MAP_JIT`) fait pareil (4 sur 12, puis 9 sur 16) : ce n'est pas QEMU, c'est
la disposition aléatoire de l'espace d'adresses de macOS. Dans un tirage « loin », la plus
grande place libre de la fenêtre `0x1xxxxxxxx` (entre les bibliothèques et les piles, sous le
cache partagé à `0x18…`) ne fait que **768-960 Mio** : le noyau ignore alors toute indication
d'adresse (même `0x110000000`, même sans `MAP_JIT`) et prend `0x300000000`. Proportion
observée en jeu : 3 lents sur 7 (campagne 1), 5 sur 10 (§8.4), 11 sur 24 sur DOOM 3 (§6 bis,
§13, en lisant les parties lentes comme « loin »). Le texte de QEMU est toujours à
`0x100000000` plus un glissement de moins de 80 Mio.

### 14.3 Le mécanisme : les sauts indirects hors fenêtre sur le M4

`tools/tcg/farcall.c` reproduit ce que TCG émet — N blocs de code généré, chacun charge
l'adresse d'un helper par `MOVZ/MOVK` ×4 (la même suite dans les deux cas), `BLR`, puis
branche au bloc suivant — dans un tampon posé près du texte ou à `0x300000000` :

| sites d'appel | 1 | 2 | 4 | **8** | 16 | 32 | 64 | 128 | 256 | 512 | 1024 | 2048 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| près (ns/appel) | 1,29 | 1,02 | 0,85 | 0,83 | 1,03 | 1,11 | 1,15 | 1,49 | 1,85 | 2,08 | 4,05 | 4,94 |
| loin (ns/appel) | 1,29 | 0,99 | 0,87 | **1,54** | 1,86 | 1,97 | 1,91 | 1,97 | 2,31 | 2,43 | 5,53 | 6,35 |

- Jusqu'à 4 sites, rien ; **dès 8 sites, +0,4 à 1,5 ns par appel** (≈ 2-6 cycles), y compris
  quand le prédicteur ne rate pas (64 sites : +65 %). Le code généré de Tiger a des centaines
  de milliers de sites (`helper_lookup_tb_ptr` à chaque `blr`, les helpers flottants, les
  accès lents à la mémoire) ; une boucle de micro-banc n'en a que deux ou trois, d'où le
  `regbench` plat.
- **C'est la fenêtre, pas la distance** : helpers recopiés à `0x380004000`, appelants à
  `0x300000000` (2 Gio plus bas, même fenêtre de 4 Gio) → 1,17 ns, rapide ; appelants à
  `0x3f0000000` (0,25 Gio plus bas, **autre** fenêtre) → 1,86 ns, lent ; appelants à
  `0x10xxxxxxx` (10 Gio) → 1,86 ns. Tout se passe comme si le prédicteur de sauts indirects
  ne gardait que les 32 bits bas de la cible et prenait les bits hauts dans l'adresse du saut,
  un chemin plus lent servant les autres cas (lecture, pas documentation d'Apple).
- Ordre de grandeur : ~600 M instructions invitées par seconde sur DOOM 3, dont ~12 % en
  helpers, plus un `lookup_tb_ptr` par `blr` : quelques dizaines de millions d'appels par
  seconde et par vCPU, soit, à ~1 ns l'aller-retour, **5-10 % du temps vCPU** — l'écart vu.

Hypothèses 1 et 4 (cœurs P/E, fréquence, MTTCG) : le placement explique à lui seul 14
démarrages sur 14, et le forçage (§14.4) le prouve dans les deux sens ; le placement des fils
n'a donc pas été mesuré plus loin (`powermetrics` exige `sudo`, refusé sur l'hôte : pas tenté
autrement). Les « deux modes de boot » de 2026-08 sous Linux x86-64 (23,3 / 28,6 s,
`docs/metrologie-boot.md`) sont d'une autre machine et d'un autre processeur : rien ne dit
qu'ils aient la même cause.

### 14.4 Le correctif : `tcg/0006` (`x-jit-near`), et l'A/B par forçage

`patches/tcg/0006-tcg-jit-near.patch` (`tcg/region.c`, `accel/tcg/tcg-all.c`,
`include/tcg/startup.h` ; s'applique au QEMU de référence, fichiers identiques) : deux
propriétés de l'**accélérateur**, éteintes par défaut.

- `-accel tcg,x-jit-near=on` : `mmap(NULL, taille)`, garder le tampon s'il est entièrement
  dans la fenêtre de 4 Gio du texte, sinon le rendre et réessayer 64 Mio plus petit, jusqu'à
  512 Mio (dans les tirages « loin », 832-960 Mio tiennent ; DOOM 3 et Marble Blast
  génèrent ~500 Mio de code sans vidage). La taille retenue devient celle du tampon
  (`tcg_region_init` repart de `region.total_size`). Sans place, repli sur le choix du
  noyau, dit dans le journal. 10 lancements sur 10 dans la fenêtre (`JITOPT=x-jit-near=on
  tools/tcg/jitwhere.sh 10 …`). Effet de bord : les appels de helpers passent de
  `MOVZ/MOVK/MOVK/BLR` à `ADRP/ADD/BLR` (cible à moins de 4 Gio).
- `-accel tcg,x-jit-addr=0x…` : adresse demandée (essais ; `0x300000000` force le lent).
- Dans tous les cas QEMU imprime sur stderr `tcg: tampon JIT <début>-<fin> (<qui>), texte
  <adresse> : même|AUTRE fenêtre de 4 Gio` — **le détecteur de régime**, lu dès le lancement.

Lanceur : `JITNEAR=1 ./run_tiger.sh` (sondé ; `TCG_OPTS=…` pour des propriétés brutes de
l'accélérateur) ; `TCG_OPTS=` aussi pour `devloop.py`. `build_qemu_qfb.sh` applique le patch
(marqueurs) et sonde la propriété. Binaires de test : `~/src/qemu-tcg19/build/qjit`/`qjit64`
(`qgpu` v19, VM quotidienne) et `qjit15`/`qjit1564` (`qgpu` v15, disque de dev), branches
`regime` et `regime15` de la copie (0001-0005 + 0006 ; `regime15` revient au `qgpu` v15).

Campagne 2 (SMP=2, `qjit1564`, un démarrage d'invité par processus, entrelacée
`près loin loin près près loin loin près`) :

| placement forcé | indices | img/s | moyenne |
|---|---|---|---|
| `x-jit-near=on` | 1,014 0,997 1,008 1,013 | 74,0 72,4 73,2 73,7 | **73,3** |
| `x-jit-addr=0x300000000` | 0,958 0,955 0,952 0,960 | 69,5 69,2 68,8 69,7 | **69,3** |

**Forcer le placement force le régime**, dans les deux sens, 4 sur 4 de chaque côté :
**+5,8 %** pour « près », et l'écart entre démarrages d'un même mode tombe à **2,2 % (près)
et 1,3 % (loin)**, contre ~11 % tous tirages mêlés au §8.4.

SMP=1 (`qjit15`, `thread=single`, entrelacée `près loin loin près près loin`) :

| placement forcé | indices | img/s | moyenne |
|---|---|---|---|
| `x-jit-near=on` | 1,011 1,025 1,009 | 73,2 74,5 73,5 | **73,7** |
| `x-jit-addr=0x300000000` | 0,948 0,960 0,940 | 68,1 69,0 67,3 | **68,1** |

Même effet en mono-cœur (**+8,2 %**) : le régime n'est pas une affaire de MTTCG ni du
second vCPU (hypothèse 4 éliminée) ; les deux régimes de DOOM 3 en SMP=1 (74,6 / 103,2,
§6 bis) en sont sans doute un tirage. En passant : **SMP=1 près (73,7) vaut SMP=2 près
(73,3)** sur ce protocole (passe de 150 s), alors que le §5.2 donnait −7 % à SMP=1 avec des
placements tirés au hasard — la question « un ou deux cœurs » (TODO §4) est à reprendre à
placement forcé.

### 14.5 Relecture de DOOM 3 (§6 bis, §13)

Les parties de DOOM 3 ont trois niveaux : référence ~80 / ~93 ms/image, patches `0002-0004`
~74 / ~80. Lecture proposée, **à confirmer par les parties du §14.6** : ~80 et ~93 sont la
référence près et loin, ~74 et ~80 les patches près et loin. Alors :

- le « −0,7 % rapide contre rapide » du §13 comparait la référence *près* (80,5) aux patches
  *loin* (80,0) ; à placement égal les patches gagneraient **~−7,5 % près (80 → 74) et ~−14 %
  loin (93 → 80)** — davantage loin parce qu'ils retirent des appels de helpers, justement ce
  qui coûte hors de la fenêtre ;
- l'écart de ~16 % entre les deux régimes de la référence dépasse celui de Marble Blast
  (6 %) : DOOM 3 appelle davantage de helpers (flottant scalaire, `lfs`/`stfs` avant `0002`) ;
- `x-sr-tlb` (§6 bis) : éteint ~84 / ~95, allumé ~79 / ~92 ; à placement égal −6 % et −3 %,
  la lecture du §6 bis tient.

### 14.6 Ce que ça change pour la mesure, et la partie DOOM 3 à jouer

- **Détecter** : binaires avec `tcg/0006`, la ligne `tcg: tampon JIT … AUTRE fenêtre` ; tout
  binaire : `vmmap <pid> | grep -m1 'rwx/rwx SM=ZER'`, une base à `0x3…` = régime lent
  (`tools/tcg/d3run.sh` l'écrit désormais dans `info.txt`).
- **Forcer** : `JITNEAR=1` (ou `TCG_OPTS=x-jit-near=on`) pour tous les A/B ; les mesures
  déjà faites se trient par placement ou, à défaut, par niveau.
- **Par défaut** : à allumer après la confirmation DOOM 3 (`JITNEAR` à 1 dans `run_tiger.sh`,
  `tcg/0006` dans le binaire de référence) — un gain réel pour l'utilisateur un lancement
  sur deux environ, sans rien changer à la traduction.

Parties DOOM 3 à jouer (VM quotidienne, depuis le worktree de cette branche, binaire `qjit`
de la copie ; `d3run.sh` passe l'environnement au `run_tiger.sh` du worktree) :

    # référence et patches, placement forcé PRÈS, entrelacés, trois parties chacun
    for i in 1 2 3; do
      QEMU_BIN=~/src/qemu-tcg19/build/qjit JITNEAR=1 LFSINLINE=0 VFPFAST=0 VPERMFAST=0 \
          bash tools/tcg/d3run.sh jn-ref-$i 2 1
      QEMU_BIN=~/src/qemu-tcg19/build/qjit JITNEAR=1 bash tools/tcg/d3run.sh jn-all-$i 2 1
    done
    # contrôle : une partie de chaque, forcée LOIN
    QEMU_BIN=~/src/qemu-tcg19/build/qjit TCG_OPTS=x-jit-addr=0x300000000 \
        LFSINLINE=0 VFPFAST=0 VPERMFAST=0 bash tools/tcg/d3run.sh jf-ref-1 2 1
    QEMU_BIN=~/src/qemu-tcg19/build/qjit TCG_OPTS=x-jit-addr=0x300000000 \
        bash tools/tcg/d3run.sh jf-all-1 2 1
    bash tools/tcg/d3run.sh --restore

Attendu si le §14.5 est juste : `jn-ref` ~80, `jn-all` ~74, `jf-ref` ~93, `jf-all` ~80, moins
de 2 % d'écart entre les parties d'un même mode ; chaque `info.txt` dit « même fenêtre »
(« AUTRE » pour `jf-*`).

### 14.7 DOOM 3 confirmé (25/09/2026, 22 h), `x-jit-near` allumé par défaut

Binaire `~/src/qemu-tcg19/build/qjit`, VM quotidienne, SMP=2, `x-sr-tlb`, `demo_mars_city1`
T+50..T+280 ; placement relevé dans chaque `info.txt` (`bench/tcg/d3/jn-*`, `jf-*`).

| Placement | Patches `0002-0004` | parties (ms/image) |
|---|---|---|
| près (`x-jit-near`) | éteints | 79,9 79,8 80,9 |
| près (`x-jit-near`) | allumés | **95,3** 73,9 73,9 |
| loin (`x-jit-addr=0x300000000`) | éteints | 92,4 |
| loin (`x-jit-addr=0x300000000`) | allumés | 79,3 |

- **Le placement fait le régime** : loin reproduit ~93 et ~80, près ~80 et ~74, comme prévu
  au §14.5. Écart entre parties d'un même mode : 1,4 % (référence), 0 % (patches, hors la
  partie à 95,3).
- **Patches `0002-0004` à placement égal : −7,5 % près** (80,2 → 73,9), **−14 % loin**
  (92,4 → 79,3). Le « −0,7 % » du §13 était un artefact de placement.
- **Placement + patches contre l'ancien pire cas** (loin, sans patches) : 92,4 → 73,9, −20 %.
- **Une partie aberrante** (`jn-all-1`, 95,3) : tampon bien près, aucun vidage, même taille
  de code, mais **lente de bout en bout**, cinématique comprise (~40 ms/image contre ~32).
  Un autre facteur de l'hôte, non lié au JIT, 1 fois sur 8 ; non expliqué (cœurs P/E,
  autre processus, thermique ?). Toute mesure garde donc une vérification : la cinématique
  (T−400..T) sert de témoin de régime.

Suite : `JITNEAR` allumé par défaut dans `run_tiger.sh`, `tcg/0006` dans le binaire de
référence (reconstruit le 25/09 à 23 h ; binaire précédent en `*.avant-jitnear`).

---

## 15. Le patch 0007 : le flottant scalaire simple sans ses helpers (`x-fp-inline`)

26/09/2026. Copie isolée `~/src/qemu-fp` (branche `fp-scalar` = `regime15` + 0007, `qgpu` v15
pour le disque de dev ; branche `fp-scalar19` = `regime` + 0007, `qgpu` v19 identique au
binaire de référence, pour la VM quotidienne). `~/src/qemu` n'a pas été touché.

### 15.1 Pourquoi

Sur DOOM 3, ~19 % du temps vCPU est dans le flottant scalaire (`helper_FMULS/FMADDS/FADDS/
FSUBS`, `helper_fcmpu`, `do_float_check_status` 4,5 %, `compute_fprf` 2,5 %). Le profil
d'instructions (`bench/tcg/d3/d3-mix.txt`, 50 s de jeu, §6 bis) dit lesquelles :

| instruction | M/s | | instruction | M/s |
|---|---|---|---|---|
| `fmuls` | 8,8 | | `fmsubs` | 1,8 |
| `fmadds` | 6,9 | | `frsp` | 0,7 |
| `fcmpu` | 5,8 | | `fnmsubs` | 0,6 |
| `fadds` | 5,8 | | `fnmadds` | 0,3 |
| `fsubs` | 4,0 | | tout le double précision | < 1 |

Les huit premières font ~97 % du flottant scalaire exécuté. Avec `x-fast-fp` (et le 0002 de
`patches/fastfp`), chacune coûte encore **deux appels de helper sans drapeau** (toutes les
globales TCG resynchronisées et relues deux fois) : l'opération (`helper_FMULS` →
`float64r32_mul`, dont le chemin hardfloat est pris), puis `helper_fprf_check_float64`
(FPRF, puis `do_float_check_status`) ; `fcmpu` : `helper_fcmpu` puis
`helper_float_check_status`. ~6 ns par instruction.

### 15.2 Ce que fait la séquence d'origine dans le cas courant

Dans l'état où tourne un jeu sous Tiger, le FPSCR est **amorcé** au sens de `x-fast-fp`
(`FPSCR[XX] = 1`, `XE = OE = UE = 0`, docs/flottant-rapide.md §2.3), arrondi au plus proche.
Si en plus chaque opérande est exactement un float32 nul ou normal (ce que donne `lfs`), et
que le résultat n'a ni débordé ni sous-débordé, alors `reset_fpstatus` → helper →
`fprf_check_float64` ne fait que :

1. rendre le float32 correctement arrondi (ce que calcule le FPU hôte : une seule opération
   IEEE simple, `fmaf` pour `fmadds` & co. — PowerPC arrondit `a·c + b` une seule fois) ;
2. laisser `fp_status.float_exception_flags = inexact` (l'amorce ; aucun autre drapeau) ;
3. écrire FPRF depuis le résultat (float64 : ±zéro `0x02/0x12`, ±normal `0x04/0x08`) et
   poser FI (`do_float_check_status` : pas de OX/UX, pas de `float_inexact_excp` puisque
   amorcé, FI = 1).

`fcmpu` sans NaN, FPSCR amorcé (il ne dépend pas de RN) : CR[bf] et FPCC = 8/4/2, FI = 1.

### 15.3 La traduction

`x-fp-inline` (propriété de CPU, défaut éteint, lue par le traducteur ; n'agit qu'avec
`x-fast-fp`, `ctx->fp_inline = env->fp_inline && env->fp_prime_mask`) :

    r = helper_fp32_fast(a, b, c, op)        appel PUR (TCG_CALL_NO_RWG_SE) : l'op float32
                                             sur le FPU hôte, ou FPI_FAIL (un NaN)
    si (fpscr & (XX|XE|OE|UE|RN)) != XX  ou  r == FPI_FAIL : aller à lent
    frT = r ; FPSCR = (FPSCR & ~(FPRF|FI)) | FPRF(r) | FI ; drapeaux = inexact
    aller à fin
    lent :   la séquence d'origine, inchangée (reset_fpstatus, helper, fprf_check_float64)
    fin :    (Rc=1) CR1 depuis le FPSCR

**Un seul branchement** par instruction ; FPRF en 11 ops TCG. `helper_fp32_fast` ne lit ni
n'écrit aucune globale TCG : son appel ne force aucune resynchronisation des globales. Il rend `FPI_FAIL` si
un opérande n'est pas un float32 nul ou normal (même test que `f64_is_f32_zon` de
softfloat : NaN, infini, dénormal simple, double sans équivalent simple → helpers), si le
résultat est infini, ou si (`fmuls`, `fmadds`…) il est de module ≤ `FLT_MIN` alors que le
produit n'est pas nul (sous-dépassement possible, zéro compris). `fadds`/`fsubs` gardent
tout résultat fini : une somme de deux float32 qui tombe sous `FLT_MIN` est **exacte**
(ni UX ni XX), et FPRF se lit sur son élargissement float64, qui est un normal — les
helpers disent la même chose (vérifié : la mutation « rejeter les sommes minuscules » ne
change rien, voir §15.5). `fnmadds`/`fnmsubs` : négation **après** l'arrondi, zéro compris.

`fcmpu` est entièrement en ligne (aucun appel) : porte « amorcé » et « ni NaN »
(`(x & ~signe) > 0x7ff0…`), puis comparaison entière sur la clé signe-grandeur → complément
à deux (`x < 0 ? −(x & ~signe) : x`, qui confond −0 et +0), CR[bf] et FPCC écrits en ligne.

### 15.4 Pourquoi c'est exact (et ce qui a été vérifié à la main)

- **Porte = état amorcé + RN = 00** : c'est exactement `ppc_fp_primed()` plus l'arrondi. Le
  mode d'arrondi de softfloat est toujours celui de `FPSCR[RN]` : rien dans le code généré
  n'écrit `cpu_fpscr` directement, toutes les écritures passent par `ppc_store_fpscr`, qui
  recale `fp_status` (arrondi, re-biaisage OE/UE). NI n'est pas modélisé par QEMU ; VE et
  ZE ne ferment pas la porte (aucune opération invalide ni division possible sur le chemin
  court), ce que la phase P5 de `fptest` vérifie.
- **Exception différée périmée** : `do_float_check_status` lève une exception si
  `exception_index` vaut déjà `PROGRAM|FP` et que `MSR[FE0|FE1]` ≠ 0. Cet état périmé ne
  naît qu'avec une trappe armée (OE/UE/XE/VE) et MSR[FE] = 0 (le helper le pose sans
  lever) ; il est consommé au prochain retour à la boucle principale, et MSR[FE] ne peut
  passer à 1 que par `mtmsr`/`rfi`/exception, qui y retournent tous. Donc sur le chemin
  court, soit il n'y a rien de périmé, soit MSR[FE] = 0 et la séquence d'origine ne lève
  rien non plus. Le vérificateur compare en plus `exception_index` avant/après.
- **FI, FX, FR** : le chemin court pose FI = 1 comme `x-fast-fp` amorcé, ne touche ni FX ni
  XX (déjà à 1), ni FR (jamais modélisé). Bit pour bit le FPSCR de la séquence d'origine.
- **Drapeaux softfloat** : le chemin court écrit `inexact` dans `fp_status` (valeur que la
  séquence d'origine y laisse), pour qu'un lecteur ultérieur ne voie aucune différence.
- **Double → simple** : les opérandes acceptés sont exactement représentables en float32, la
  conversion en `float` est exacte ; le résultat float32 s'élargit exactement. `fmadds` par
  `fmaf` (un arrondi), jamais `a*c+b` (build sans `-ffast-math`, sans contraction :
  `fmaf` explicite).
- **Sous-dépassement « avant arrondi »** (PowerPC détecte la petitesse avant l'arrondi) : un
  résultat qui s'arrondit à `FLT_MIN` exactement peut avoir sous-dépassé ; d'où `≤ FLT_MIN`
  et non `< FLT_MIN` (mutation détectée : 3 230 divergences).

### 15.5 La preuve

**Hôte** (`tools/tcg/fpproof.sh [arbre] [N] [graine]`) : `fpproof.c` est lié aux **vrais**
objets de l'arbre construit — `target_ppc_fpu_helper.c.o` (les helpers d'origine *et*
`helper_fp32_fast`), `target_ppc_cpu.c.o` (`ppc_store_fpscr`), `fpu_softfloat.c.o` — et
le bloc « fp-inline » de `fpu_helper.c` (porte, FPRF, `fcmpu`) en est extrait tel quel.
Pour chaque vecteur (opération, opérandes, FPSCR de départ rangé par `ppc_store_fpscr`,
MSR[FE] au hasard) : si le chemin court est pris, résultat, FPSCR entier, drapeaux
softfloat, exception levée et `exception_index` doivent être égaux à ceux de la séquence
d'origine. Vecteurs : catalogue croisé de 106 valeurs (float32 : ±0, dénormaux, `FLT_MIN`
et voisins, `FLT_MAX`, ±∞, NaN silencieux et signalants, 2^24±1… ; doubles : 0,1, 1/3,
dénormaux doubles, `DBL_MIN`, exposants 0x380/0x47f juste hors plage simple, bits bas
posés, NaN doubles), `fmadds` & co. en croisement complet a×b×c (1,2 M triplets), dans trois
familles de FPSCR (porte ouverte avec le reste au hasard ; FPSCR au hasard ; porte fermée
d'un seul bit : RN = 1, 2, 3, XE, OE, UE ou XX effacé) ; puis N vecteurs aléatoires par
opération (float32 quelconques, près du débordement, près du dénormal, mantisses courtes
pour les demi-ulp, float32 à un bit près, doubles quelconques, annulations exactes).

| `fpproof.sh ~/src/qemu-fp N` | vecteurs | par le chemin court | divergences |
|---|---|---|---|
| N = 20 M, graine 0x5eed | 174 427 024 | 97 528 970 | **0** |
| N = 100 M, graine 0x1234abcd | 814 427 024 | 485 606 552 | **0** |

« modèle ≠ objet » (le `helper_fp32_fast` extrait et recompilé contre celui de l'objet) : 0.
Contre-épreuve (`tools/tcg/fpproof-mut.sh`) : **10 mutations sur 10 détectées** (débordement
d'une somme accepté 783 divergences ; `FLT_MIN` exact accepté 3 230 ; `fmadds` en deux
arrondis 393 218 ; porte sans RN 260 257 ; clé de `fcmpu` sans −0 = +0 : 5 ; FPRF de −0
faux 3 082 ; exposant dénormal simple admis 18 806 ; sNaN admis par `fcmpu` 15 919 ; tout
zéro de `fmuls` accepté 28 909 ; débordement accepté 321 670). Deux mutations essayées en
premier n'étaient pas des erreurs : rejeter moins de sommes minuscules (elles sont exactes :
le code les accepte désormais) et ne tester que `c` pour le produit nul (plus de replis,
jamais faux).

**Invité** (`tools/guest/jobs/fptest`) : les vraies instructions dans Tiger (SMP=2, disque
de dev), `fadds` … `fnmsubs`, `fcmpu`, opérandes chargés par `lfs` et `lfd` (catalogue de
106 valeurs croisé, `fmadds` & co. en a×b×c complet en P1, puis 2^18 vecteurs aléatoires par
opération et par état), dans onze états du FPSCR : P0 FPSCR = 0 à chaque instruction, P1 XX
(le cas du chemin court), P2-P4 XX et RN = 1/2/3, P5 XX+VE+ZE, P6-P8 XX et XE/OE/UE armés,
P9 FPSCR qui s'accumule, P10 FPSCR au hasard. Résultat, FPSCR (`mffs`) et CR hachés :

| `x-fp-inline` | instructions | empreinte |
|---|---|---|
| éteint | 30 124 880 | `755efae4e391b7ea` |
| allumé + `x-fp-verify` | 30 124 880 | `755efae4e391b7ea` |

**Sortie identique octet pour octet.** Et le vérificateur (`x-fp-verify` : chaque passage par
le chemin court refait par la séquence d'origine, résultat, FPSCR, drapeaux et
`exception_index` comparés, plus le modèle C) pendant ce tour : **8 653 066 vérifiés, 0
divergence**. Rejoué sur le binaire final (même code, champs de `DisasContext` déplacés pour
que le patch s'applique au QEMU de référence) : même empreinte, 7 392 559 vérifiés, 0 divergence.

**Tour réel** (même VM `x-fp-verify`, démarrage + bureau + Marble Blast 110 s de chauffe
et 240 s de démo) : **3 039 670 933 passages vérifiés, 0 divergence**. Taux de chemin court
en jeu : **99,0 %** — replis : `fcmpu` 24,7 M sur FPSCR non amorcé (processus sans aucun
résultat inexact encore), `fsubs` 6,5 M sur opérandes/résultat (1,9 %), le reste < 0,01 %.

### 15.6 Gains

Banc invité (`BANC=10000000 ./fptest banc` : chaîne dépendante `fmadds fmuls fmsubs fadds` ;
transformation 4×4 de 1 024 sommets comme un jeu, `lfs`/12 `fmadds`/4 `fmuls`/`stfs` ;
min/max par `fcmpu` + branchement), même binaire, SMP=2, `x-jit-near`, trois tours chacun :

| | éteint | allumé | |
|---|---|---|---|
| chaîne (40 M op.) | 261 / 259 / 251 ms | 202 / 201 / 204 ms | **−21 %** (6,4 → 5,1 ns/op) |
| sommets (160 M op.) | 1 118 / 1 118 / 1 094 ms | 596 / 595 / 591 ms | **−46 %** (7,0 → 3,7 ns/op) |
| `fcmpu` (20 M) | 175 / 173 / 174 ms | 100 / 105 / 102 ms | **−41 %** |

Marble Blast (disque de dev, `tools/tcg/mbab.sh`, binaire `qfp15`, SMP=2, `x-jit-near`
forcé et `x-sr-tlb`, `x-lfs-inline`, `x-vfp-fast`, `x-vperm-fast` des deux côtés ; deux
manches entrelacées, `off on on off off on on off` puis `on off off on on off`, 7 démarrages
par mode, 2 passes de 240 s chacun ; journaux `bench/tcg/res/{f,g}*`, non versionnés).
**Bruit fort** : la VM quotidienne jouait en même temps (100 à 145 % d'un cœur hôte pendant
13 démarrages sur 14, relevé `load.txt`).

| démarrage (ordre) | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mode | off | on | on | off | off | on | on | off | on | off | off | on | on | off |
| img/s (moyenne des fenêtres de jeu) | 68,1 | 67,5 | 69,1 | 64,9 | 61,9 | 66,8 | 66,9 | 57,2 | 65,0 | 62,5 | 70,8 | 73,7 | 74,1 | 64,3 |

Moyenne par démarrage : éteint 64,2 img/s (57,2 à 70,8), allumé 69,0 (65,0 à 74,1). Paires de
fenêtres (440, triangles/image à ±2 %) : **+7,9 %** (rapport des moyennes), médiane des
rapports **+6,6 %**, quartiles −2,3 / +18,7 %. Seul démarrage à hôte calme (le premier,
éteint, 68,1) : aucun démarrage allumé n'a eu ces conditions. Le sens est net (les 7
démarrages allumés tous au-dessus de la moyenne des éteints), l'ampleur ne l'est pas :
**+5 à +8 %**, à confirmer hôte calme. Marble Blast exécute ~8 M instructions concernées
par seconde (tour vérifié, §15.5), dont 99 % par le chemin court.

**DOOM 3 attendu** : ~33 M instructions concernées par seconde ; −1,3 à −3,8 ns chacune
(banc) ⇒ 45 à 125 ms de vCPU par seconde, soit **−4 à −10 % de ms/image** si le fil du jeu
est la limite (74 ms/image aujourd'hui). À mesurer (§15.8).

### 15.7 Essayé et classé

- **Opérations directement en instructions flottantes aarch64 dans le code généré** (nouvelles
  ops TCG dans le backend, sans appel). Borne mesurée avant d'écrire le backend : un binaire
  d'expérience où `helper_fp32_fast` rend son premier opérande sans rien calculer (résultats
  faux, chemin court toujours pris) fait chaîne 63 ms, sommets 325 ms, `fcmpu` 84 ms. Le
  reste du coût (chaîne 202 → 63 ms) est la latence du calcul lui-même — `fmov` GPR→FP,
  `fcvt` double→simple, `fmadd`, `fcvt` simple→double, `fmov` FP→GPR, ~19 cycles — qu'une op
  en ligne paierait aussi, puisque les FPR vivent en mémoire comme des doubles. Le gain
  possible se limite à l'appel et aux tests branchus (~0,5-1 ns/op), pour une modification
  du cœur de TCG (op nouvelle, contraintes, registres flottants réservés dans le backend
  arm64, repli sur les autres hôtes). **Pas fait** ; à reconsidérer seulement si les FPR
  passent un jour dans des registres flottants de l'hôte.
- **Le double précision** (`fmadd`, `fmul`, `fsub`… < 1 M/s sur DOOM 3), `frsp` (0,7 M/s),
  `fdivs`, `fctiwz` : hors du patch, trop peu fréquents.
- **FPSCR non amorcé** (XX = 0) : le chemin court exigerait de savoir si le résultat est
  inexact (FI, XX, FX exacts) ; sous Tiger c'est ~1 % des cas (§15.5) : helpers.

### 15.8 L'A/B DOOM 3 à jouer (VM quotidienne)

Binaire `~/src/qemu-fp/build/qfp` (`qfp64` en SMP=2) : branche `fp-scalar19` de la copie
(`regime` = base + 0001 + `qgpu` v19 **identique au binaire de référence** + 0002-0005 +
0006, puis 0007 ; toutes les propriétés nouvelles éteintes par défaut). Depuis le worktree
de cette branche (son `run_tiger.sh` sait `FPINLINE`), placement du JIT forcé près
(`JITNEAR` est allumé par défaut), trois parties par mode, entrelacées :

    for i in 1 2 3; do
      QEMU_BIN=~/src/qemu-fp/build/qfp FPINLINE=0 bash tools/tcg/d3run.sh fpi-ref-$i 2 1
      QEMU_BIN=~/src/qemu-fp/build/qfp FPINLINE=1 bash tools/tcg/d3run.sh fpi-on-$i 2 1
    done
    # facultatif, pas pour la vitesse : une partie vérifiée (bilan « fp-verify » dans
    # bench/tcg/d3/fpi-verif/run_tiger.log, 0 divergence attendu)
    QEMU_BIN=~/src/qemu-fp/build/qfp FPINLINE=1 FPVERIFY=1 bash tools/tcg/d3run.sh fpi-verif 2 1
    bash tools/tcg/d3run.sh --restore

Lire chaque `info.txt` (« même fenêtre ») et la cinématique comme témoin de régime (§14.7) ;
écarter une partie lente de bout en bout. Attendu : `fpi-ref` ~74 ms/image, `fpi-on` 67-71.
Joué : §15.9.

### 15.9 DOOM 3 joué (26/09/2026, 11 h)

VM quotidienne seule sur l'hôte (le QEMU du disque de dev était arrêté), binaire
`~/src/qemu-fp/build/qfp64` (`fp-scalar19`), SMP=2, `x-fast-fp`, `x-sr-tlb`, `x-lfs-inline`,
`x-vfp-fast`, `x-vperm-fast` des deux côtés, `x-jit-near`, `demo_mars_city1`, parties
entrelacées `ref on ref on ref on`, puis une partie `FPVERIFY=1` à part ; journaux
`bench/tcg/d3/fpi-*` (non versionné). Placement relevé dans chaque `info.txt` : **« même
fenêtre » 7 fois sur 7**. Témoin de régime : la cinématique (T−400..T).

| partie | `x-fp-inline` | T+50..T+280 | T+50..T+450 | cinématique |
|---|---|---|---|---|
| `fpi-ref-1` | éteint | 74,7 | 74,4 | 32,3 |
| `fpi-on-1` | allumé | **65,1** | 65,0 | 29,0 |
| `fpi-ref-2` | éteint | 74,1 | 74,1 | 32,5 |
| `fpi-on-2` | allumé | **65,2** | 65,0 | 29,6 |
| `fpi-ref-3` | éteint | 74,4 | 74,2 | 33,0 |
| `fpi-on-3` | allumé | **64,8** | 64,8 | 33,5 |

(ms/image.) **Médianes : 74,4 → 65,1 ms/image, −12,5 %** (T+50..T+450 : 74,2 → 65,0,
−12,4 %). Écart entre parties d'un même mode : 0,8 % (référence), 0,6 % (allumé) ; aucune
partie lente de bout en bout, cinématiques dans le régime normal (29-34 ms/image ; la
partie aberrante du §14.7 en avait ~40). Le gain dépasse l'attendu (−4 à −10 %) : sur DOOM 3
le flottant scalaire pesait ~19 % du vCPU, et le fil du jeu est bien la limite. En
images/s : 13,4 → 15,4.

Partie vérifiée (`FPINLINE=1 FPVERIFY=1`, bilan à la sortie de QEMU dans
`fpi-verif/run_tiger.log`) : **4 558 916 143 passages vérifiés, 0 divergence**. Replis :
31,3 M (0,7 %), surtout `fsubs` 17,4 M et `fcmpu` 13,2 M sur FPSCR non amorcé, `fmsubs` 0,66 M
sur opérandes. (Sa mesure de vitesse n'a pas de sens : le vérificateur ralentit le jeu, la
détection de T s'en trouve décalée.)

**Gel au chargement** (vCPU 0 bouclant en 0x268b4, signalé ~1 lancement sur 12 par l'agent
de la matrice A3) : **0 sur 7** lancements ici, dont 4 avec `x-fp-inline` ; rien ne dit
qu'il soit plus fréquent avec le patch (trop peu de lancements pour dire qu'il l'est moins).

**Verdict** : `x-fp-inline` est exact (preuves du §15.5, 4,6 milliards de passages vérifiés
en jeu) et rapporte **−12,5 % de ms/image sur DOOM 3**. Proposition : l'allumer par défaut
(`FPINLINE` à 1 dans `run_tiger.sh`, `tcg/0007` dans le binaire de référence) — décision de
l'utilisateur. La VM quotidienne a été rendue sur le QEMU de référence `~/src/qemu`, sans jeu.

---

## 16. Les sorties indirectes : `tcg/0008` (`x-ret-inline`, `x-jc-idx`) et l'essai `tcg/0009` (`x-isync-chain`)

26/09/2026. Copie isolée `~/src/qemu-ret` (clone local de `~/src/qemu-fp`) : branche `ret15b`
(= `fp-scalar` + 0008 + essai 0009, `qgpu` v15, disque de dev) et `ret19b` (= `fp-scalar19`
+ 0008, `qgpu` v19 identique au binaire de référence, VM quotidienne) ; toutes deux portent
`tcg/0007` comme la référence. `~/src/qemu` n'a pas été touché. Binaires :
`~/src/qemu-ret/build/qret15`/`qret1564` (dev), `qret19`/`qret1964` (quotidienne).

### 16.1 Le poste

Chaque `blr`, `bctr`, `bclr`/`bcctr` conditionnel, chaque branchement vers une autre page et
chaque fin de bloc `DISAS_CHAIN` sort du code chaîné par `lookup_and_goto_ptr` :
un appel de `helper_lookup_tb_ptr` (état relu dans `env`, `curr_cflags`, test des points
d'arrêt, sonde du cache de sauts de 4 096 entrées, retour), puis un saut indirect. Sur un
**raté** du cache de sauts, `tb_htable_lookup` : traduction de la page de code par le TLB
(`get_page_addr_code`, remplissage au besoin) et recherche dans la table de hachage `qht`.

Mesures de fréquence (compteurs du mode preuve, §16.4, Marble Blast seul, SMP=2) :

| | stock | `x-jc-idx` |
|---|---|---|
| sorties indirectes servies par le cache de sauts | 1 104,6 M (**75,6 %**) | 1 099,8 M (77,6 %) |
| ratés | 356,2 M | 317,3 M |
| … entrée vidée (même pc, bloc retiré par un vidage du TLB ou une invalidation) | 257,6 M | 213,7 M |
| … autre pc (conflit dans la table de 4 096) | 140,5 M | 135,9 M |
| … même pc, autres drapeaux | 4,6 M | 6,2 M |

(Ratés comptés sur les trois sources : boucle principale, helper, recherche en ligne.) **Un
retour sur quatre rate le cache de sauts**, surtout parce qu'il est vidé en entier à chaque
vidage du TLB — même d'un seul `mmu_idx`, même d'un `mmu_idx` qui n'avait aucune entrée
(`tlb_flush_by_mmuidx_async_work` appelle `tcg_flush_jmp_cache` sans condition) : ~7 000
vidages par seconde sur Marble Blast avec `x-sr-tlb`.

Profil d'instructions (DOOM 3, `bench/tcg/d3/d3-mix.txt`, 478 M instr./s) : `bclr` 6,6 M/s,
`bcctr` 1,5 M/s ; retours à la boucle principale : `isync` **0,54 M/s**, `mtmsr` 0,16,
`rfi` 0,035, `sc` 0,023. Sur Marble Blast (§2.2) : `isync` **942 000/s**, `mtmsr` 208 000.

### 16.2 La conception

**`x-ret-inline`** (propriété de CPU, lue par le traducteur) : aux sorties indirectes, la
sonde de `tb_lookup()` est émise dans le code généré (`translator_lookup_and_goto_ptr_inline`,
`accel/tcg/translator.c`, appelée par `gen_goto_ptr_exit` de `target/ppc/translate.c`) :

    h   = tb_jmp_cache_hash_func(nip)              5 ops, la même fonction
    e   = &cpu->tb_jmp_cache->array[h]
    tb  = e->tb ; si NULL → helper
    si e->pc != nip                → helper
    si tb->flags != env->hflags    → helper        (hflags relu à l'exécution)
    si tb->cflags != cpu->tcg_cflags → helper      (CF_INVALID ne passe jamais)
    si cpu->breakpoints non vide   → helper        (check_for_breakpoints)
    can_do_io = 1 ; goto_ptr tb->tc.ptr
    helper : helper_lookup_tb_ptr, goto_ptr        (inchangé)

Exactement les comparaisons du helper ; `tb->cs_base` n'est pas comparé, il vaut 0 pour tout
bloc PowerPC (`cpu_get_tb_cpu_state`). `curr_cflags()` n'ajoute rien à `tcg_cflags` hors pas
à pas gdb, `one-insn-per-tb` et `-d nochain` : ces modes posent `CF_NO_GOTO_PTR`/`CF_NO_GOTO_TB`
dans les blocs qu'ils produisent, et `translator_can_goto_ptr_inline` n'émet alors pas la
sonde (le pas à pas gdb vide aussi tous les blocs) ; les basculer à chaud n'est pas pris en
charge avec la propriété allumée. Pas de sonde non plus avec un greffon TCG. Le bloc atteint
teste `icount_decr` dans son prologue comme d'habitude : interruptions, demandes de sortie et
travail en file restent vus au même endroit.

**`x-jc-idx`** (propriété de CPU, lue par `accel/tcg/cputlb.c`) : le cache de sauts retient,
pour chaque entrée, le `mmu_idx` d'instruction sous lequel elle a été trouvée
(`jc->idx[]`, écrit à l'insertion par le CPU propriétaire) ; un vidage du TLB de certains
`mmu_idx` ne jette que les entrées de ceux-là (`tcg_flush_jmp_cache_idx`), et **rien** si
aucun des `mmu_idx` demandés n'avait d'entrée (`to_clean == 0`). Pour ne pas parcourir la
table entière à chaque vidage (~7 000 par seconde : un premier parcours octet par octet
coûtait 3,5 % du temps vCPU, deux fois le `tcg_flush_jmp_cache` stock), chaque `mmu_idx`
tient le journal des emplacements remplis depuis son dernier vidage (1 024 au plus, sinon
parcours complet) : le vidage ne visite que ceux-là, en vérifiant que l'emplacement n'a pas
été repris sous un autre `mmu_idx` entre-temps. Invariant : une entrée de
`mmu_idx` *i* implique le bit *i* de `tlb.c.dirty` (elle a été trouvée par une entrée du TLB
de code de *i* remplie depuis le dernier vidage de *i*, et ce vidage a jeté les entrées plus
anciennes). Une entrée d'un autre `mmu_idx` que ceux vidés reste juste : sa traduction passe
par un TLB qui n'a pas changé ; comme en stock, une entrée n'est pas revalidée si le TLB a
simplement évincé puis rechargé la page (l'architecture autorise la traduction périmée
jusqu'au `tlbie`, qui vide tout). Les hflags PowerPC contiennent le `mmu_idx` d'instruction :
une entrée n'est prise que sous le même.

**`x-isync-chain`** (essai `tcg/0009`) : `isync` finit son bloc par une recherche du bloc
suivant (`DISAS_CHAIN_UPDATE`) au lieu d'un retour à `cpu_exec`. Rien de ce qu'`isync`
synchronise n'a besoin de la boucle principale : le vidage local du TLB qu'il déclenche est
synchrone, le bloc suivant est recherché avec la nouvelle traduction (jamais par `goto_tb`),
le code modifié a été invalidé par l'écriture elle-même, et interruptions, demandes de
sortie et travail en file sont vus par le prologue du bloc suivant.

### 16.3 Le mode preuve `x-ret-verify`

Chaque bloc pris dans le cache de sauts — par la boucle principale, par le helper, et par la
sonde en ligne (qui appelle alors `helper_lookup_tb_ptr_check`) — est comparé à ce que donne
**une recherche physique complète maintenant** (traduction de la page de code sans
exception invitée, `pomppc_code_phys_nofault`, puis `qht`), c'est-à-dire ce que le code
stock aurait trouvé après avoir vidé son cache ; pour la sonde en ligne, `pc`, `flags`,
`cflags` sont recalculés comme le helper (`cpu_get_tb_cpu_state`, `curr_cflags`, points
d'arrêt). Un bloc divergent n'est pas exécuté (repli sur le helper stock) et est imprimé ;
un bloc invalidé par l'autre vCPU entre la lecture et la vérification est compté à part
(« course », la même fenêtre existe en stock). Bilan sur stderr toutes les 2^26
vérifications et à la sortie de QEMU ; le mode compte aussi les ratés par cause et les
vidages du cache de sauts faits et évités.

### 16.4 La preuve

`tools/tcg/retproof.sh` (disque de dev, copie APFS ; propriétés de `run_tiger.sh` par
défaut allumées partout, `x-jit-near`) : démarrage du bureau, `smctest` (ci-dessous), Marble
Blast (110 s de chauffe + 240 s de démo), arrêt propre ; les VM de preuve tournaient en
priorité de fond (`taskpolicy -b`, cœurs E) pendant que la matrice occupait la VM quotidienne.

| config | SMP | blocs vérifiés (boucle / helper / en ligne) | divergences | courses |
|---|---|---|---|---|
| `x-ret-verify` seul (cache stock) | 2 | 122 M / 1 139 M / — | **0** | 43 |
| `x-ret-inline` | 2 | 136 M / — / 1 190 M | **0** | 24 |
| `x-ret-inline` + `x-jc-idx` (sans `smctest`) | 2 | 131 M / — / 2 634 M | **0** | 0 |
| `x-ret-inline` + `x-jc-idx` | 2 | 142 M / — / 1 190 M | **0** | 15 |
| `x-ret-inline` + `x-jc-idx` | **1** | 68 M / — / 1 148 M | **0** | 0 |
| + `x-isync-chain` | 2 | 32 M / — / 1 314 M | **0** | 26 |
| + `x-isync-chain` | **1** | 28 M / — / 1 212 M | **0** | 0 |
| `x-ret-verify` seul, Marble Blast seul | 2 | 107 M / 1 105 M / — | **0** | 0 |
| `x-ret-inline` + `x-jc-idx`, Marble Blast seul | 2 | 112 M / — / 1 100 M | **0** | 0 |
| version finale (journal par `mmu_idx`), cœurs P | 2 | 448 M / — / 6 524 M | **0** | 115 |
| version finale (journal par `mmu_idx`), cœurs P | **1** | 158 M / — / 6 674 M | **0** | 0 |
| **DOOM 3**, VM quotidienne, version finale (`ret-verif`) | 2 | 259 M / — / 6 990 M | **0** | 0 |

**34 milliards de blocs pris dans le cache de sauts vérifiés, 0 divergence**, en SMP=2 et
SMP=1, sur Marble Blast et sur DOOM 3. Les « courses » n'apparaissent qu'avec `smctest` en SMP=2 (le code réécrit par
l'autre vCPU) et existent déjà sans les patches. `x-isync-chain` fait tomber les recherches de
la boucle principale de ~110 M à ~30 M par tour : les trois quarts des retours à `cpu_exec`
étaient des `isync`.

**Code modifié dans l'invité** (`tools/guest/jobs/smctest`, empreinte FNV par essai) :

- A — JIT maison : `li r3,K ; blr` dans une page RWX, appelée à chaud, `K` réécrit
  (`dcbst/sync/icbi/isync`), 400 000 appels ;
- B — retour dans du code réécrit **pendant l'appel** : un talon appelle (`bctrl`) un
  patcheur C qui réécrit l'instruction à l'adresse de retour du talon ; le `blr` du patcheur
  doit trouver le nouveau code (2 000 fois, après 100 appels à chaud) ;
- C — même adresse virtuelle, deux pages physiques (deux pages d'un fichier au code
  différent, `mmap MAP_FIXED` tour à tour, 3 000 fois) ;
- D — deux processus (`fork`), même adresse, code différent (copie sur écriture), qui
  alternent par un tube 5 000 fois : chaque alternance change les registres de segment ;
- E — deux fils : l'un appelle `f` en boucle, l'autre la réécrit 20 000 fois ;
- F — une fonction C recopiée à 4 000 adresses successives d'un tampon, puis effacée.

A, B, C, D, F : **0 erreur et empreintes identiques** dans toutes les configurations (stock,
`x-ret-inline`, `+x-jc-idx`, `+x-isync-chain`, SMP=2 et SMP=1). E : 0 erreur en SMP=1 ; en
SMP=2, **erreurs dans toutes les configurations, QEMU sans aucune propriété compris**
(§16.7) — défaut de QEMU 9.2 indépendant de ces patches.


### 16.5 Gains

**DOOM 3** (VM quotidienne, rendue ensuite au binaire de référence sans jeu ; binaire
`~/src/qemu-ret/build/qret19`, branche `ret19b` = `fp-scalar19` + 0008 ; plugin
`20260926-memo` ; SMP=2, propriétés par défaut de `run_tiger.sh` des deux côtés, `x-jit-near`,
**« même fenêtre » 8 fois sur 8** ; `tools/tcg/retd3.sh`, parties entrelacées ref / on ;
hôte calme, VM de dev arrêtée ; journaux `bench/tcg/d3/ret-*`, non versionnés) :

| partie | `x-ret-inline` + `x-jc-idx` | T+50..T+280 | T+50..T+450 |
|---|---|---|---|
| `ret-ref-1` | éteints | 65,3 | 65,6 |
| `ret-on-1` | allumés | **61,2** | 61,3 |
| `ret-ref-2` | éteints | 65,7 | 66,0 |
| `ret-on-2` | allumés | **61,2** | 61,3 |
| `ret-ref-3` | éteints | 65,9 | 66,1 |
| `ret-on-3` | allumés | **61,5** | 61,5 |
| `ret-prof-ref` (avec `sample`) | éteints | 65,7 | — |
| `ret-prof-on` (avec `sample`) | allumés | 61,7 | — |

(ms/image.) **Médianes des trois parties entrelacées : 65,7 → 61,2 ms/image, −6,8 %**
(T+50..T+450 : 66,0 → 61,3, −7,1 %). Écart entre parties d'un même mode : 0,9 % et 0,5 %.
Les deux parties de profil (un `sample` de 10 s après la fenêtre) disent la même chose. En
images/s : 15,2 → 16,3.

**Marble Blast** (disque de dev, `tools/tcg/mbab.sh`, binaire `qret15`, SMP=2, propriétés
par défaut partout, `x-jit-near` ; deux manches entrelacées
`off rj ri rji | rji ri rj off`, 2 passes de 240 s par démarrage ; journaux
`bench/tcg/res/a*`). **Bruit fort** : la VM quotidienne tournait un jeu pendant 5 démarrages
sur 8 (98 à 125 % d'un cœur, `load.txt`).

| comparaison (paires de fenêtres ±2 % de triangles) | rapport des moyennes | médiane des rapports |
|---|---|---|
| référence → `x-ret-inline` | +4,4 % (122 paires) | +2,3 % |
| référence → `x-ret-inline` + `x-jc-idx` | **+6,6 %** (126 paires) | +6,2 % |
| `x-ret-inline` → `+ x-jc-idx` | +3,5 % | +3,5 % |
| `x-ret-inline` + `x-jc-idx` → `+ x-isync-chain` | −4,3 % | −2,8 % |

Par démarrage (img/s) : référence 60,7 / 69,8 ; `rj` 63,8 / 74,0 ; `ri` 66,9 / 67,6 ; `rji`
64,8 / 64,9 (les deux `rji` à hôte calme). Le sens de `x-ret-inline` et `x-jc-idx` est le même
que sur DOOM 3 ; l'ampleur est à reprendre hôte calme. `x-jc-idx` a été mesuré ici dans sa
première version (parcours complet) ; le journal par `mmu_idx` retire le coût du parcours
(3,5 % du temps vCPU, §16.6), il ne peut que l'améliorer.

### 16.6 Profils à jour (`sample` du processus QEMU, fils vCPU, temps occupé)

| poste (inclusif) | MB réf. | MB `rj` | D3 réf. | D3 on |
|---|---|---|---|---|
| code généré (self) | 43,6 % | 51,2 % | 49,0 % | 59,2 % |
| `helper_lookup_tb_ptr` | **20,5 %** | 11,9 % | **15,9 %** | 5,7 % |
| … dont `tb_htable_lookup` (raté du cache de sauts) | 10,2 % | 9,9 % | 4,5 % | 4,5 % |
| `tcg_flush_jmp_cache` / `_idx` | 1,4 % | 3,4 % ¹ | — | — |
| verrou global (`bql_lock_impl` + attente) | 5,1 % + 5,2 % | 4,0 % + 4,1 % | 3,5 % + 3,4 % | 3,0 % + 3,0 % |
| TLB (`tlb_fill`, `probe_access`, `mmu_lookup`) | ~6-9 % | ~7 % | ~5 % | ~5 % |
| `helper_lmw` + `helper_stmw` | 5,5 % | 5,9 % | 5,1 % | 5,0 % |
| flottant scalaire restant (`helper_fp32_fast`) | 2,2 % | — | **8,2 %** | 8,4 % |
| AltiVec (`vmaddfp`, `vfp_fma4`) | — | — | 4,3 % | 4,5 % |
| horloge (`mftb`, `cpu_get_clock`) | 1,4 % | — | ~2 % | ~2-3 % |
| `pthread_jit_write_protect_np` (retours à `cpu_exec`) | 1,6 % | 1,8 % | 1,0 % | 0,9 % |
| `hreg_store_msr` / `ppc_maybe_interrupt` (`mtmsr`, `rfi`) | 2,9 % / 1,5 % | 3,2 % | < 1 % | < 1 % |

¹ Première version de `x-jc-idx` (parcours complet de la table) ; supprimé par le journal.

Lecture : la sonde en ligne fait disparaître la partie « réussite » de `helper_lookup_tb_ptr`
(son coût passe dans le code généré, plus court) ; il reste **les ratés du cache de sauts**
(`tb_htable_lookup` : 10 % sur Marble Blast, 4,5 % sur DOOM 3). Sur DOOM 3 ce sont surtout des
**conflits** dans la table de 4 096 entrées (partie vérifiée : 494 M « autre pc » contre 225 M
« entrée vidée », taux de réussite 91 %) ; sur Marble Blast surtout des **vidages** (le
`mmu_idx` utilisateur est vidé à chaque changement de processus par `x-sr-tlb`, §3.4). Le
verrou global (`mtmsr`/`rfi`) pèse 6-10 %, `lmw`/`stmw` 5 %, le flottant scalaire restant
8 % sur DOOM 3.

### 16.7 Découvert en route : le code réécrit par l'autre vCPU (`smctest` E)

L'essai E de `smctest` (un fil réécrit `li r3,g ; blr` 20 000 fois avec
`dcbst/sync/icbi/sync/isync`, publie `g` ; l'autre fil lit `g`, fait `isync` et appelle la
fonction) **échoue en SMP=2 dans toutes les configurations, QEMU sans aucune propriété
compris** (`BASEPROPS=""` : ni `x-sr-tlb` ni `x-jit-near`) : l'appelant exécute un bloc
périmé **pendant des dizaines de réécritures** (« génération 9 vue après 64 (mémoire 64,
rappel 9) » : la mémoire contient bien le nouveau code, un second appel rend encore l'ancien),
et même après `pthread_join` (« E-fin : 19971 au lieu de 20000 »). En SMP=1 : 0 erreur.
C'est donc un défaut de QEMU 9.2 en MTTCG : une écriture d'un vCPU dans une page dont l'autre
vCPU a traduit du code peut ne plus invalider ce code. Deux courses de `accel/tcg/cputlb.c`
ont été lues (le calcul de `TLB_NOTDIRTY` hors verrou dans `tlb_set_page_full`, et
`tlb_set_dirty` qui retire `TLB_NOTDIRTY` après un test fait hors verrou dans
`notdirty_write`) ; les refermer (essai `x-smc-fix`, test refait sous le verrou) **ne change
rien** : la cause est ailleurs, non trouvée. Portée pour Tiger : un programme qui écrit du
code sur un processeur et l'exécute sur l'autre (JIT multifil ; les jeux du dépôt n'en ont
pas). À reprendre à part (TODO §4) ; `smctest` en est le test de non-régression. Un essai en
`thread=single` (SMP=2 sans MTTCG) n'a pas conclu : l'invité a redémarré pendant le test.

### 16.8 Essayé et classé

- **`x-isync-chain`** (`patches/tcg/essais/0009`) : exact (0 divergence, §16.4), fait tomber
  les retours à `cpu_exec` des trois quarts, mais Marble Blast n'est pas plus rapide (−4 %
  contre `x-ret-inline` + `x-jc-idx`, les deux démarrages `rji` à hôte calme). La boucle
  principale ne coûtait que ~3 % (`pthread_jit_write_protect_np` compris), et la recherche
  qui suit un `isync` rate le cache de sauts aussi souvent que celle de la boucle (l'`isync`
  suit un changement de registres de segment et un vidage). Non appliqué.
- **Pile de retours prédits** (empiler l'adresse de retour et le bloc à chaque `bl`, les
  comparer au `blr`) : pas faite. Le bloc de retour n'est pas connu au `bl` ; il faudrait un
  emplacement par site d'appel et la même invalidation que le cache de sauts (vidages,
  `tb_flush`, blocs invalidés), pour le même coût au `blr` (une comparaison, un saut
  indirect). Le gain d'une vraie pile (retour prédit par le processeur hôte, `ret` au lieu de
  `br`) demanderait que le code généré garde une pile d'appels hôte à travers les blocs :
  hors de portée de TCG.
- **Chaînage direct des `bctr` monomorphes** : non fait ; la sonde en ligne en prend déjà
  l'essentiel (le saut indirect restant est bien prédit par le processeur hôte quand la cible
  ne change pas).
- **`x-smc-fix`** (§16.7) : sans effet sur le défaut, retiré.

### 16.9 Suites

- **Allumer `x-ret-inline` et `x-jc-idx` par défaut** (`RETINLINE`/`JCIDX` à 1 dans
  `run_tiger.sh`, `tcg/0008` dans le binaire de référence) : décision de l'utilisateur ;
  DOOM 3 −6,8 %, Marble Blast +4 à +7 % (bruité), 34 milliards de blocs vérifiés.
- **Cache de sauts plus grand** : sur DOOM 3 les deux tiers des ratés restants sont des
  conflits. Avec le journal par `mmu_idx`, le vidage par `mmu_idx` ne dépend plus de la
  taille de la table : passer de 4 096 à 16 384 entrées (256 Kio par vCPU) ne coûterait que
  les vidages complets (`tb_flush`, plages), rares. Épreuve : taux de réussite du mode preuve
  et A/B DOOM 3.
- **Étiquettes multiples par mode pour `x-sr-tlb`** (§3.4) : les ratés « entrée vidée » de
  Marble Blast et les remplissages du TLB viennent du vidage du `mmu_idx` utilisateur à
  chaque changement de processus.
- **Verrou global à chaque `mtmsr`/`rfi`** : 6-10 % du temps vCPU (TODO §4).
- **Le défaut du §16.7** (code réécrit par l'autre vCPU).

Commande de l'A/B DOOM 3 (jouée ci-dessus ; pour la rejouer, depuis le worktree de cette
branche, VM de dev arrêtée) :

    QEMU_BIN=~/src/qemu-ret/build/qret19 tools/tcg/retd3.sh 3
    # = pour i dans 1..3 : d3run.sh ret-ref-$i 2 1 1 ; RETINLINE=1 JCIDX=1 d3run.sh ret-on-$i 2 1 1
    #   puis d3run.sh --restore (binaire de référence, SMP=2)
    # partie vérifiée (bilan « ret-verify » dans bench/tcg/d3/ret-verif/run_tiger.log) :
    QEMU_BIN=~/src/qemu-ret/build/qret19 RETINLINE=1 JCIDX=1 RETVERIFY=1 \
        tools/tcg/d3run.sh ret-verif 2 1 0 && tools/tcg/d3run.sh --restore

`tools/tcg/d3run.sh` prend désormais son `sample` avant de sortir sur `FINI` : avec la règle
de fin de cinématique à seuil relatif, `PROFIL` et `FINI` arrivent dans le même relevé, et le
`sample` n'était jamais pris.
