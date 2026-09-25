# TCG et le G4 émulé — relevé, A/B des cœurs, patches

Chantier « le G4 émulé lui-même » (`TODO.md` §4, 25/09/2026). Au-delà des lots du plugin,
le plafond des jeux est la vitesse à laquelle TCG exécute le code PowerPC. Ce document
relève **où part le temps du processeur émulé** (instructions exécutées, helpers, coût
hôte), mesure **1 contre 2 cœurs**, et décrit **les patches qui en sont sortis** :
`patches/tcg/0001-ppc-sr-tlb.patch` (propriété de CPU `x-sr-tlb`), puis, pour DOOM 3,
`0002-ppc-lfs-inline` (`x-lfs-inline`, §8), `0003-ppc-vfp-fast` (`x-vfp-fast`, §9) et
`0004-ppc-vperm-fast` (`x-vperm-fast`, §10) ; deux essais exacts mais sans gain,
`patches/tcg/essais/0002-ppc-lmw-inline.patch` (`x-lmw-inline`) et
`essais/0005-ppc-vfp-nrwg.patch` (`x-vfp-nrwg`, §11), ne sont pas appliqués.

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
