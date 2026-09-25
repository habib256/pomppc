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
