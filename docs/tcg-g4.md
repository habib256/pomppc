# TCG et le G4 émulé — relevé, A/B des cœurs, patches

Chantier « le G4 émulé lui-même » (`TODO.md` §4, 25/09/2026). Au-delà des lots du plugin,
le plafond des jeux est la vitesse à laquelle TCG exécute le code PowerPC. Ce document
relève **où part le temps du processeur émulé** (instructions exécutées, helpers, coût
hôte), mesure **1 contre 2 cœurs**, et décrit **les patches qui en sont sortis** :
`patches/tcg/0001-ppc-sr-tlb.patch` (propriété de CPU `x-sr-tlb`), puis, pour DOOM 3,
`0002-ppc-lfs-inline` (`x-lfs-inline`, §8), `0003-ppc-vfp-fast` (`x-vfp-fast`, §9) et
`0004-ppc-vperm-fast` (`x-vperm-fast`, §10), puis `0006-tcg-jit-near` (`x-jit-near`, §14) et
`0007-ppc-fp-inline` (`x-fp-inline`, le flottant scalaire simple sans ses helpers, §15), puis
`0008-tcg-ret-inline` (`x-ret-inline`, `x-jc-idx` : les sorties indirectes, §16), puis
`0010-tcg-smc-mttcg` (le code réécrit par l'autre vCPU : trois courses de QEMU corrigées et
`x-icbi-sync`, §17, allumée par défaut depuis l'A/B du §20), puis `0011-tcg-jc-bits`
(`x-jc-bits`, le cache de sauts de 16 384 entrées, §18) et `0012-ppc-msr-nobql`
(`x-msr-nobql`, `mtmsr`/`rfi` sans verrou global, §19), puis `0013-ppc-fp-flat` (`x-fp-flat`)
et `0014-tcg-fp-native` (`x-fp-native`, le flottant scalaire simple par le FPU de l'hôte dans
le code généré, une op TCG nouvelle, §22), puis `0015-ppc-tb-fast` (`x-tb-fast`, la base de
temps par le compteur de l'hôte, §23) et `0016-tcg-fp-native64` (`x-fp-native64`, le flottant
double par la même op, §24) ; trois essais exacts mais sans gain,
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
(§16.7) — défaut de QEMU 9.2 indépendant de ces patches, trouvé et corrigé au §17.


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
rien** à lui seul. Un essai en `thread=single` (SMP=2 sans MTTCG) n'a pas conclu : l'invité a
redémarré pendant le test.

**Cause trouvée le 29/09 (§17)** : une troisième course, dominante, masquait les deux
premières — la page de code n'est protégée qu'**après** que le traducteur a lu le code ;
les trois sont réelles et corrigées ensemble par `tcg/0010`, et `x-icbi-sync` ferme la
fenêtre restante (écriture invalidée avant d'être faite).

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
- **`x-smc-fix`** (§16.7) : sans effet sur le défaut à lui seul, retiré ; ses deux
  corrections sont reprises sans condition dans `tcg/0010` (§17), avec la troisième course.

### 16.9 Suites

- **Allumer `x-ret-inline` et `x-jc-idx` par défaut** (`RETINLINE`/`JCIDX` à 1 dans
  `run_tiger.sh`, `tcg/0008` dans le binaire de référence) : décision de l'utilisateur ;
  DOOM 3 −6,8 %, Marble Blast +4 à +7 % (bruité), 34 milliards de blocs vérifiés.
- **Cache de sauts plus grand** (fait, §18 : `x-jc-bits=14`, DOOM 3 −1,9 %) : sur DOOM 3 les deux tiers des ratés restants sont des
  conflits. Avec le journal par `mmu_idx`, le vidage par `mmu_idx` ne dépend plus de la
  taille de la table : passer de 4 096 à 16 384 entrées (256 Kio par vCPU) ne coûterait que
  les vidages complets (`tb_flush`, plages), rares. Épreuve : taux de réussite du mode preuve
  et A/B DOOM 3.
- **Étiquettes multiples par mode pour `x-sr-tlb`** (§3.4) : les ratés « entrée vidée » de
  Marble Blast et les remplissages du TLB viennent du vidage du `mmu_idx` utilisateur à
  chaque changement de processus.
- **Verrou global à chaque `mtmsr`/`rfi`** : 6-10 % du temps vCPU (TODO §4) ; fait, §19
  (`x-msr-nobql`, DOOM 3 −0,6 %).
- **Le défaut du §16.7** (code réécrit par l'autre vCPU) : corrigé, §17.

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

---

## 17. Le code réécrit par l'autre vCPU : `tcg/0010` (trois courses de QEMU, `x-icbi-sync`)

29/09/2026. Copie isolée `~/src/qemu-smc` (worktree de `~/src/qemu`, branches `smc-base` =
l'arbre de référence, `smc-fix` = + 0010, `smc-diag` = instrumentation, `smc-stat` =
compteurs) ; disque de dev cloné (`cp -c`), VM en single-user, SMP=2, propriétés de
`run_tiger.sh` par défaut (`x-jit-near` compris) sauf mention. `~/src/qemu` n'a pas été touché.

### 17.1 Le défaut

`smctest` E (§16.4, §16.7) : un fil réécrit `li r3,g ; blr` 20 000 fois (`dcbst ; sync ;
icbi ; sync ; isync`, puis publie `g`), l'autre lit `g`, fait `isync` et appelle la fonction ;
il doit rendre au moins `g`. Sur l'arbre de référence (5 exécutions) : **5 sur 5 en erreur,
1 524 épisodes périmés, 86 millions d'appels périmés** ; le bloc périmé tient jusqu'à la
fin d'une rafale de 64 réécritures (l'écrivain dort toutes les 64), et parfois au-delà du
`pthread_join` (« E-fin : 19968 au lieu de 20000 »). SMP=1 : 0 erreur.

### 17.2 Diagnostic

Instrumentation (`patches/tcg/essais/0010-smcdbg-diag.patch`, qui porte aussi un premier
état des correctifs ; `POMPPC_SMCDBG=1` ; hors du patch appliqué) : chaque bloc de
8 octets retient le premier mot lu par le traducteur ; la recherche du bloc (`tb_lookup`)
compare ce mot à la mémoire ; après 20 000 recherches d'un bloc périmé, QEMU imprime l'état
de la page (bit `DIRTY_MEMORY_CODE`, bloc dans la liste de la page), toutes les entrées de
TLB des deux vCPU qui pointent sur la page (avec `TLB_NOTDIRTY`), et les derniers
événements de la page, tirés d'un anneau global : remplissage de TLB (`F`, avec
`NOTDIRTY`), `notdirty_write` (`N`), `tlb_unprotect_code` (`U`), retrait de `NOTDIRTY` par
`tlb_set_dirty` (`S`), `tlb_protect_code` (`P`) et remise de `NOTDIRTY` dans les TLB des
autres vCPU (`R`, avec le nombre d'entrées touchées).

Premier relevé (arbre de référence) : bloc valide, dans la liste de la page, page protégée,
**et aucun événement entre la protection et le relevé** alors que la mémoire a avancé de
64 générations. Les 64 écritures ont eu lieu **avant** `P` : pendant que le vCPU 0
traduisait (il avait lu le code), le vCPU 1 écrivait par une entrée sans `NOTDIRTY`, la page
n'ayant plus de bloc (`U` puis `S`). **Course 1** : `tb_gen_code` lit le code, puis
`tb_link_page` → `tb_page_add` protège la page ; une écriture de l'autre vCPU entre les
deux n'est jamais vue, et le bloc reste valide jusqu'à la prochaine écriture piégée.

Une fois la course 1 fermée, les relevés montrent la **course 2** : `S 1` du vCPU 0 (test
`!cpu_physical_memory_is_clean` fait hors verrou, puis `tlb_set_dirty`), et entre les
deux la protection du vCPU 1 : `R 0` (l'entrée du vCPU 0 portait encore `NOTDIRTY`, rien à
remettre), puis `tlb_set_dirty` retire `NOTDIRTY` d'une page qui a de nouveau du code :
toutes les écritures suivantes du vCPU 0 passent sans invalider, jusqu'au prochain vidage
de son TLB. La **course 3** est la même, au remplissage (`tlb_set_page_full` calcule
`NOTDIRTY` avant de prendre le verrou du TLB, l'entrée installée après la remise des
drapeaux échappe aux deux) : lue dans le code, jamais vue dans les relevés.

Les courses 2 et 3 sont celles de l'essai `x-smc-fix` (§16.7) : réelles, mais masquées par
la course 1, bien plus fréquente — d'où « sans effet » le 26/09.

Reste, les trois fermées : des erreurs d'**une** génération, isolées (« génération 7573
vue après 7574 (mémoire 7574, rappel 7573) »), corrigées à l'écriture suivante.
`notdirty_write` invalide les blocs **avant** que l'écriture soit faite (ordre de
`mmu_lookup`, et `probe_access` rend un pointeur écrit plus tard par l'appelant, `dcbz`) :
l'autre vCPU peut retraduire l'ancien code entre l'invalidation et l'écriture. Ce n'est plus
une perte (l'écriture suivante invalide), mais le protocole de l'invité (`icbi`, `sync`,
drapeau, `isync`) ne suffit plus à garantir le nouveau code : sur un vrai G4, `icbi` retire
la ligne de **tous** les caches d'instructions ; dans QEMU, `helper_icbi` ne fait qu'un
chargement.

### 17.3 Le correctif

- **Course 1**, `tb_lock_page0` / `tb_lock_page1` (`accel/tcg/tb-maint.c`) : la page est
  protégée (`tlb_protect_code`) **quand le traducteur en prend le verrou, avant de lire le
  code**, si elle n'a pas encore de bloc. Toute écriture ultérieure passe par
  `notdirty_write`, dont l'invalidation attend le verrou de page tenu par le traducteur, puis
  invalide le bloc lié. Aussi au redémarrage `-3` (page 0 relâchée puis reprise). La
  protection de `tb_page_add` reste (déjà faite, elle ne coûte qu'un test du bit).
- **Course 2**, `tlb_set_dirty` : le test « la page est-elle propre ? » est refait sous le
  verrou du TLB. `tlb_protect_code` efface le bit `CODE` puis prend ce verrou pour remettre
  `NOTDIRTY` : si le test sous verrou voit le bit encore sale, la remise viendra après ; s'il
  le voit propre, on ne retire rien.
- **Course 3**, `tlb_set_page_full` : `NOTDIRTY` est calculé sous le verrou du TLB, même
  raisonnement.
- **`x-icbi-sync`** (propriété de CPU, éteinte par défaut dans QEMU, **éteinte aussi par
  `run_tiger.sh`** jusqu'à l'A/B DOOM 3 du §20 ; allumée par défaut depuis le 30/09,
  `ICBISYNC=0` l'éteint) : `helper_icbi` invalide les blocs qui recouvrent
  sa ligne de cache (`tb_invalidate_phys_line_sync`). `icbi` suit l'écriture dans l'ordre du
  programme : le bloc retraduit trop tôt est jeté avant que l'écrivain publie. Le
  recouvrement est testé sous le verrou de la page (une traduction en cours est attendue).
  Chemin court : une page dont le bit `CODE` est sale n'a aucun bloc, et une traduction qui
  ne l'a pas encore effacé lira l'écriture (barrière avant la lecture du bit ; le traducteur
  efface le bit, par une opération atomique séquentielle, avant de lire le code). L'adresse
  physique vient de `probe_access_full` (le chargement d'origine vient de remplir le TLB).

Les trois courses sont des défauts francs de QEMU en MTTCG : corrigées sans condition.

### 17.4 La preuve

`smctest` (`tools/guest/jobs/smctest`, désormais `-t LETTRES -r TOURS` et `ICBI=N`),
exécutions de l'essai E (20 000 réécritures chacune) :

| binaire | propriétés | exécutions E | exécutions en erreur | épisodes périmés | appels périmés |
|---|---|---|---|---|---|
| référence | `run_tiger.sh` | 5 | **5** | 1 524 | 86 002 222 |
| référence | aucune | 1 | **1** | — | 11 715 449 |
| courses 1-3 fermées | `run_tiger.sh`, sans `x-icbi-sync` | 200 | **188** | 802 | 840 (1 génération) |
| `tcg/0010` | `run_tiger.sh` + `x-icbi-sync` | 50 + 500 | **0** | 0 | 0 |
| `tcg/0010` | `x-icbi-sync` seule | 200 | **0** | 0 | 0 |
| `tcg/0010` final (chemin court) | `run_tiger.sh` + `x-icbi-sync` | 300 | **0** | 0 | 0 |
| `tcg/0010` final, `N=4` (80 000 réécritures) | idem | 50 | **0** | 0 | 0 |

A, B, C, D, F : **empreintes identiques** à celles de l'arbre de référence (et entre elles)
dans toutes les configurations, SMP=2 et SMP=1 (`qfix`, 32 bits) : `A cd01e214aab92265`,
`B c37bce5023096e85`, `C ac26223f71fdace5`, `D 952b31e1968f5b8d`, `F 7308784105b8fd59`,
et `E aa4d72ebdba6b6eb` avec 0 erreur (l'empreinte E de la référence varie avec ses erreurs).

Non-régression : `tests/run-all.sh` 156 OK, 0 échec ; `--slow` avec le binaire corrigé :
`qfb_smoke` vert, `qgpu_smoke` en échec **identique** avec le binaire de référence
(« table LAYOUT : contextes par client 0x20, attendu 4 », antérieur). Les preuves de
`tools/tcg` (`lfsproof`, `fpproof`, `vfpproof`, `vpermproof`) portent sur des helpers que
0010 ne touche pas.

Rejouer (disque de dev, SMP=2 ; les propriétés de `run_tiger.sh` dans `CPU_OPTS`) :

    QEMU_BIN=<build>/qemu-system-ppc SMP=2 CPU_OPTS=…,x-icbi-sync=on \
        python3 tools/guest/devloop.py start
    mkdir /tmp/j && cp tools/guest/jobs/smctest/* /tmp/j
    printf 'TESTS=E\nREPS=300\n' > /tmp/j/env.sh      # ou rien : A-F une fois ; ICBI=N : banc
    python3 tools/guest/devloop.py run /tmp/j          # out/smctest.txt : « tour k erreurs 0 »

### 17.5 Fréquence dans Tiger, coût

Compteurs (`patches/tcg/essais/0010-smcstat.patch`, par-dessus 0010) sur un démarrage du bureau, 4 min de démo Marble Blast, puis
`smctest` : pour chaque course, le cas où l'arbre de référence aurait perdu l'invalidation
(course 1 : mémoire du bloc changée entre la protection et le lien, sur une page neuve ;
course 2 : test sous verrou qui refuse de retirer `NOTDIRTY` ; course 3 : `NOTDIRTY` posé
sous verrou alors que le test hors verrou ne l'aurait pas posé).

| phase | protections de page | course 1 | course 2 | course 3 | `icbi` | `icbi` par le chemin long | `icbi` qui invalident |
|---|---|---|---|---|---|---|---|
| démarrage + Marble Blast | ~24 600 | **0** | **0** | **0** | 14,4 M | 47 | **1** |
| `smctest` (A-F) | ~40 700 | 2 | 7 929 | 0 | 2,1 M | 17 584 | 17 457 |

**Démarrage et Marble Blast ne déclenchent aucune des trois courses** : il faut du code
réécrit pendant que l'autre vCPU l'exécute ou le traduit (JIT multifil, `smctest`). Ce
défaut n'explique donc pas, à lui seul, la panique AppleUSBOHCI au démarrage ni le gel de
DOOM 3 (non mesuré sous DOOM 3, VM quotidienne). `x-icbi-sync` : 14 millions d'`icbi` au
démarrage (le noyau synchronise chaque page de code chargée), presque tous par le chemin
court. Banc d'`icbi` (`smctest icbi 20000000`, même VM, `x-icbi-sync` éteint → allumé) :

| cas | éteint | allumé |
|---|---|---|
| `icbi` sur une page sans code | 8,5 ns | 17,6 ns |
| `icbi` sur une page à blocs, autre ligne | 9,3 ns | 20 ns |
| `icbi` de la ligne d'un bloc, puis appel (retraduction) | 25 ns | 7,2 µs |

+9 à 11 ns par `icbi` : 0,13 s de temps vCPU sur les 14 millions d'un démarrage. La
retraduction n'a lieu que si l'invité synchronise une ligne de code déjà traduite (une fois
sur tout le démarrage et Marble Blast).

### 17.6 Suites

- Les trois courses touchent tout invité MTTCG (x86 compris, sans `icbi`) : proposables en
  amont. La fenêtre « invalidé avant d'être écrit » n'a pas de correctif générique simple
  (`probe_access` rend un pointeur écrit plus tard) ; pour PowerPC, `x-icbi-sync` suffit.
- Chercher la cause des incidents SMP ailleurs (banc d'endurance) ; `smctest` E reste le
  test de non-régression de ce chapitre.

---

## 18. Le cache de sauts plus grand : `tcg/0011` (`x-jc-bits`)

30/09/2026. Copie isolée `~/src/qemu-vit` (worktree de `~/src/qemu`, branche `vit-jc` :
`vit-base` = l'arbre de `build_qemu_qfb.sh` à `05a47fe`, puis un commit par patch) ;
`~/src/qemu` n'a pas été touché. Binaires `~/src/qemu-vit/build/qvit`/`qvit64` (0011 + 0012,
propriétés éteintes par défaut). VM quotidienne, SMP=2, propriétés de `run_tiger.sh` par
défaut des deux côtés.

### 18.1 La conception

**`x-jc-bits`** (propriété de l'**accélérateur**, 12 à 16, 12 = QEMU d'origine,
`JCBITS=14 ./run_tiger.sh`) : 2^N entrées de cache de sauts par vCPU. Globale et non par
CPU : les blocs sont partagés entre vCPU, et la sonde en ligne de `x-ret-inline` (§16.2)
fige le hachage dans le code généré. Elle est figée dès la création du premier CPU
(`tb_jmp_cache_bits_frozen`, un `qom-set` ultérieur est refusé). Les tableaux de
`CPUJumpCache` sont dimensionnés pour 2^16 (1 Mio + 64 Kio + journal de 512 Kio par vCPU,
alloués à zéro : seules les pages touchées coûtent) ; seules les 2^N premières entrées
servent. Suivent la taille : le hachage (`tb_jmp_cache_hash_func`, `_hash_page`, et sa copie
émise par `translator_lookup_and_goto_ptr_inline`), le vidage par page
(`tb_jmp_cache_clear_page`, 2^(N/2) entrées), le seuil du vidage par plage, le vidage
complet, le parcours complet de `x-jc-idx` et la longueur de son journal par `mmu_idx`
(un quart de la table : 1 024 à 2^12, comme avant).

**Pourquoi c'est exact.** Le cache de sauts n'est qu'un cache : une entrée n'est prise que
si `pc`, `flags`, `cflags` sont égaux (et, en ligne, les mêmes comparaisons, §16.2). La
taille ne change que *où* une entrée est rangée ; il faut seulement que tous ceux qui
calculent l'emplacement calculent le même, et que le vidage par page couvre toutes les
entrées de la page. Les deux tiennent pour tout N de 12 à 16 : avec P = ⌊N/2⌋ et
s = 12 − P, la partie haute de l'indice, bits [P, N), vaut les bits [12, N + s) de
`pc ^ (pc >> s)`, qui ne dépendent que des bits ≥ 12 de `pc` (le numéro de page) ; toutes
les adresses d'une page tombent donc dans les 2^P entrées qui commencent à
`tb_jmp_cache_hash_page(page)`. Le hachage est lu dans une variable globale écrite une
seule fois avant tout CPU.

**Preuve** (mode `x-ret-verify`, §16.3, qui compte aussi les ratés par cause et, depuis
0011, les parcours complets du journal) : une partie de DOOM 3 par taille, démarrage compris
(`tools/tcg/matab.sh jc-verif`, `jc-verif2`) :

| entrées | blocs pris dans le cache vérifiés | divergences | réussite | ratés : entrée vidée / autre pc / autres drapeaux | parcours complets |
|---|---|---|---|---|---|
| 4 096 | 7 930 M | **0** | 91,37 % | 2,68 / 5,83 / 0,11 % | 46 847 |
| 16 384 | 7 786 M | **0** | **92,59 %** | 2,83 / 4,46 / 0,12 % | 6 335 |
| 65 536 | 7 758 M | **0** | 92,83 % | 2,86 / 4,19 / 0,12 % | 2 173 |
| 16 384, hachage replié (essai) | 7 891 M | **0** | 91,99 % | 2,79 / 5,11 / 0,11 % | 6 618 |

**23 milliards de blocs vérifiés, 0 divergence.** Les ratés passent de 8,63 à 7,41 % des
sorties indirectes (−14 %) ; les conflits (« autre pc ») de 5,83 à 4,46 %, puis 4,19 % à
65 536 entrées : ils **plafonnent**, ce n'est donc plus la capacité. La cause probable est
la structure du hachage : les adresses d'une même page n'ont que 2^(N/2) emplacements
(128 à 2^14) pour 1 024 instructions possibles. Un essai de hachage qui replie tout le
numéro de page (`x-jc-mix`, `patches/tcg/essais/0011-tcg-jc-mix.patch` : sans lui, les
bits de page au-dessus de 12 + N ne comptent pas, et le code à la même adresse basse de deux
régions de 16 Mio se heurte) fait **moins bien** (91,99 %) : non retenu.

### 18.2 A/B DOOM 3

`tools/tcg/matab.sh ab-jc 6 "j12:" "j14:TCG_OPTS=x-jc-bits=14"` : parties entrelacées
A B B A…, chacune dans un QEMU neuf, cellule `d3-fen` de la matrice sans vidage (fenêtre
repérée sur la scène) ; aucun autre QEMU, charge de l'hôte relevée avant et après chaque
fenêtre (1,36 à 2,55). Bilan `bench/tcg/ab/ab-jc/bilan.txt`.

| | n | médiane | min..max | dispersion |
|---|---|---|---|---|
| 4 096 entrées | 6 | 61,6 ms/image | 61,1..61,8 | 1,1 % |
| 16 384 entrées | 6 | **60,4 ms/image** | 60,1..62,0 | 3,1 % |

**−1,9 % (médianes)** ; cinq parties sur six à 16 384 sont sous la plus rapide à 4 096 (la
sixième, 62,0, est la plus lente des douze) ; Mann-Whitney U = 6, p ≈ 0,058 (bilatéral, exact).
Plus que ce qu'annonce la part de `tb_htable_lookup` (4,5 % × 14 % ≈ 0,6 %) : un raté coûte
aussi le saut indirect mal prédit et la sortie du code généré.

### 18.3 Décision

**Allumé par défaut** (`JCBITS=14` dans `run_tiger.sh`) : exact (0 divergence), gain
supérieur à la dispersion de la référence, coût mémoire négligeable, sans régression sur la
matrice complète (§21). 2^16 n'apporterait que 0,24 point de réussite de plus.

---

## 19. `mtmsr` et `rfi` sans verrou global : `tcg/0012` (`x-msr-nobql`)

### 19.1 Le poste

Chaque `mtmsr` (`helper_store_msr` → `hreg_store_msr`) et chaque `rfi` (`do_rfi`) appelle
`ppc_maybe_interrupt`, qui prend le verrou global (BQL) pour poser ou retirer
`CPU_INTERRUPT_HARD` — presque toujours à la valeur qu'il a déjà. `rfi` y ajoute
`cpu_interrupt_exittb` (BQL encore) pour demander `CPU_INTERRUPT_EXITTB`, que la boucle
principale retire aussitôt… en reprenant le BQL (`cpu_handle_interrupt` : `interrupt_request`
≠ 0). Un `mtmsr` qui change `IR`/`DR` fait de même. Soit trois prises du BQL par `rfi`, en
concurrence avec l'autre vCPU et les fils des devices (le GPU paravirtuel le tient à chaque
commande). Profils (§16.6) : verrou 3,0 % + 3,0 % d'attente sur DOOM 3, 4 + 4 % sur Marble
Blast ; 0,16 M `mtmsr`/s et 0,035 M `rfi`/s sur DOOM 3, 208 000 `mtmsr`/s sur Marble Blast.

### 19.2 La conception

**Décision sans verrou.** Quand `ppc_maybe_interrupt` est appelée par le vCPU lui-même
(`qemu_cpu_is_self`), sans le BQL, sous TCG, avec la propriété : lire l'état, et si
`CPU_INTERRUPT_HARD` vaut déjà ce que la fonction d'origine poserait, ne rien faire (si le
bit est posé et reste dû, poser `icount_decr.high = -1` comme le ferait `cpu_interrupt` du
vCPU lui-même) ; sinon, le chemin d'origine sous le BQL.

Entrées de la décision : `env->pending_interrupts`, écrit par les autres fils **sous le BQL**
(`ppc_set_irq`, qui appelle ensuite `ppc_maybe_interrupt` sous le BQL), et l'état du vCPU
(`msr`…), écrit par lui seul. `CPU_INTERRUPT_HARD` n'est écrit que par
`ppc_maybe_interrupt` (sous le BQL) et par `ppc_cpu_exec_interrupt` du vCPU lui-même.

**Compteur de séquence** `env->irq_seq` : chaque `ppc_maybe_interrupt` sous le BQL (un seul
écrivain à la fois) le rend impair, **barrière complète**, lit les entrées et met
`HARD` à jour, puis le rend pair (écriture *release*). Le lecteur sans verrou :

    (écriture de msr faite)      barrière complète
    s = irq_seq (acquire)        s impair → chemin sous verrou
    r = interruption due ?       h = bit HARD            barrière de lecture
    irq_seq ≠ s ou r ≠ h         → chemin sous verrou   ; sinon terminé

**Pourquoi c'est exact** (le résultat final de `HARD` est celui de la sérialisation
d'origine). Soit une mise à jour sous verrou U d'un autre fil, concurrente de la décision D :

- U finie avant la première lecture de `irq_seq` par D : D lit la valeur paire écrite par U
  (*acquire* / *release*), donc voit `pending_interrupts` et `HARD` tels que U les a laissés ;
  sa décision est celle que le code d'origine prendrait maintenant sous le verrou.
- U commencée (impair) avant la dernière lecture de D : D voit un impair ou une valeur
  changée → chemin sous verrou, sérialisé après U.
- U commence après la dernière lecture de D : U écrit l'impair, barrière, lit `msr` ; D a
  écrit `msr`, barrière, lu `irq_seq` sans voir l'impair. C'est le motif de Dekker (deux
  écritures suivies, après barrière complète, de la lecture de l'écriture de l'autre) :
  l'un des deux voit forcément l'écriture de l'autre ; D ne l'a pas vue, donc U lit le
  nouveau `msr`. Le résultat de U, calculé sur les dernières entrées, est définitif —
  exactement ce que donnait l'ordre d'origine (le vCPU prenait le BQL après U).

La décision ne peut donc laisser `HARD` faux qu'en l'absence de toute mise à jour
concurrente, et dans ce cas elle compare à l'état courant : rien n'est perdu. Les lectures
de `pending_interrupts` sans verrou peuvent être déchirées par une écriture concurrente :
alors `irq_seq` a changé, et la décision est jetée. Rebouclage de `irq_seq` (2^32
mises à jour pendant une seule décision) : exclu. Les propriétés éteintes, rien ne change
(pas même le compteur).

**`EXITTB` supprimé** à `rfi` et au `mtmsr` qui change `IR`/`DR`, quand ils viennent du bloc
traduit (`hreg_store_msr_tb`, appelée par `helper_store_msr` et `do_rfi` seulement) : ces
blocs finissent par `DISAS_EXIT` / `DISAS_EXIT_UPDATE`, c'est-à-dire `exit_tb(NULL, 0)`
(ou l'exception de pas à pas), donc retour à la boucle principale avec `last_tb = NULL` —
tout ce qu'`EXITTB` obtenait (pas de chaînage du bloc précédent au suivant). Les autres
appelants (`ppc_store_msr` du gdbstub, de la migration, du reset ; le passage en veille
`POW`) gardent le chemin d'origine.

**Mode preuve `x-msr-nobql-verify`** : après chaque décision sans verrou, prendre le BQL et
recalculer comme le code d'origine ; sous le verrou, aucune mise à jour n'est en cours, et
une mise à jour pas encore commencée n'a pas encore écrit `pending_interrupts` : le
résultat doit être égal à `HARD`. Tout écart est imprimé, compté (« divergence ») et
corrigé. Bilan sur stderr toutes les 2^26 décisions et à la sortie.

### 19.3 La preuve

| épreuve | configuration | résultat |
|---|---|---|
| banc d'endurance `msr-verif-reboot`, 2 instances | `x-msr-nobql` + `-verify` | 30 démarrages sur 30 (shutdown -r), 0 panique, 0 gel ; **378 M décisions sans verrou vérifiées, 0 divergence** ; 0,06 % de passages au verrou ; 162 M `EXITTB` supprimés |
| banc d'endurance `msr-jc14-reboot`, 2 instances | `x-msr-nobql` + `x-jc-bits=14`, sans preuve | 40 sur 40, 0 panique, 0 gel |
| `smctest` A-F, puis E × 100 (copie du disque de dev, single-user, SMP=2) | configuration retenue (`x-jc-bits=14`, `x-msr-nobql`, `x-icbi-sync`) + `x-msr-nobql-verify` + `x-ret-verify` | **0 erreur**, empreintes A-F identiques à celles du §17.4 (`A cd01e214aab92265` … `F 7308784105b8fd59`, `E aa4d72ebdba6b6eb` à chaque tour) ; 137 M décisions sans verrou, 0 divergence ; 6,3 G blocs pris dans le cache de sauts vérifiés, 0 divergence (32 749 « courses » : blocs invalidés par l'autre vCPU entre lecture et vérification, que `x-icbi-sync` multiplie sous `smctest`) |

### 19.4 A/B DOOM 3

`tools/tcg/matab.sh ab-msr 6 "m0:" "m1:CPU_OPTS=x-msr-nobql=on"` (même protocole que §18.2,
cache de sauts d'origine des deux côtés, charge 1,18 à 2,37) :

| | n | médiane | min..max | dispersion |
|---|---|---|---|---|
| éteint | 6 | 61,7 ms/image | 61,4..62,0 | 1,0 % |
| `x-msr-nobql` | 6 | **61,3 ms/image** | 61,1..61,5 | 0,7 % |

**−0,6 %**, petit mais net : une seule paire inversée sur 36 (Mann-Whitney U = 1,
p ≈ 0,004 bilatéral).

**Marble Blast** (`ab-msr-mb`, cellule `mb-pe`, même protocole) : 9,8 → 10,1 ms/image
(médianes), mais 8 et 15 % de dispersion dans chaque mode (le jeu tourne selon le
lancement à ~9,5 ou ~10,2) : Mann-Whitney p ≈ 0,20, **non concluant** dans un sens comme
dans l'autre. La cellule de la matrice n'a pas la résolution voulue (0,1 ms/image sur 10).

Pourquoi si peu, alors que le verrou pesait 3 + 3 % sur DOOM 3 ? Ce poste compte toutes les
prises du BQL, dont celles du GPU paravirtuel et de la boucle d'E/S ; `hreg_store_msr` /
`ppc_maybe_interrupt` ne pesaient que < 1 % (§16.6). La part `mtmsr`/`rfi` retirée est
celle-là.

### 19.5 Décision

**Allumé par défaut** (`MSRNOBQL=1`) : exact (378 M décisions vérifiées sous verrou,
0 divergence ; 70 redémarrages de l'invité en SMP=2 sans incident), gain petit mais net
sur DOOM 3 (−0,6 %, p ≈ 0,004), sans régression sur la matrice complète (§21). Marble
Blast reste à mesurer avec un banc plus fin (`tools/tcg/mbab.sh`, fenêtres appariées).

---

## 20. `x-icbi-sync` : l'A/B DOOM 3 (`tcg/0010`)

La propriété (§17.3) était exacte mais éteinte faute d'A/B DOOM 3. `tools/tcg/matab.sh
ab-icbi 6 "i0:" "i1:ICBISYNC=1"` (même protocole que §18.2, cache de sauts d'origine,
`x-msr-nobql` éteint, charge 1,30 à 2,78) :

| | n | médiane | min..max | dispersion |
|---|---|---|---|---|
| éteint | 6 | 61,8 ms/image | 61,3..63,1 | 2,9 % |
| `x-icbi-sync` | 6 | 61,6 ms/image | 61,3..62,3 | 1,6 % |

−0,4 %, dans le bruit : **aucun coût mesurable**, comme l'annonçait le banc d'`icbi` (§17.5 :
+9 à 11 ns par `icbi`, presque tous par le chemin court). **Allumé par défaut**
(`ICBISYNC=1`) : c'est une correction d'exactitude (le code réécrit par l'autre vCPU selon
le protocole PowerPC est vu), gratuite, sans régression sur la matrice complète (§21).

---

## 21. La matrice complète sur la configuration retenue (30/09/2026)

Binaire `~/src/qemu-vit/build/qvit64`, `x-jc-bits=14`, `x-msr-nobql`, `x-icbi-sync`, les
autres propriétés de `run_tiger.sh` par défaut ; tour `bench/tcg/ab/matrice-vit` (preuves
d'image complètes, vidage et rejeu), aucun autre QEMU, charge 1,7 à 3,2. Comparaison au
tour de référence `bench/matrice/20260930-0128` (binaire de référence, mêmes jeux) :

| cellule | référence | retenue | plancher |
|---|---|---|---|
| Marble Blast fenêtre / plein écran | 9,9 / 9,7 | 10,1 / 10,0 | 13 |
| Zenerchi fenêtre | 4,4 | 3,9 | 6 |
| DOOM 3 fenêtre / plein écran | 61,7 / 61,6 | **60,0 / 60,3** | 76 |
| Prey fenêtre / plein écran | 70,9 / 70,3 | 70,9 / 70,9 | 89 |
| UT2004 fenêtre / plein écran | 27,2 / 27,5 | 26,4 / 26,6 | 36 |
| Warcraft III plein écran | 17,8 | 17,8 | 22 |
| Colin McRae plein écran | 71,5 | 66,7 | 88 |
| Nexuiz fenêtre / plein écran | 110,7 / 109,4 | 109,0 / 108,7 | 137 |
| Nexuiz GLSL fenêtre / plein écran | 41,7 / 41,1 | 40,5 / 41,1 | 52 |

(ms/image, une partie par cellule : seules les A/B des §18-§20 mesurent un gain.) **16
vertes sur 16 automatisées, images toutes justes, replis dans les tolérances, aucune
cellule au-dessus de son plancher.** Marble Blast un peu plus lent d'un tour à l'autre,
dans son bruit connu (9,3 à 10,3 depuis le 26/09, §19.4).

---

## 22. Le flottant scalaire « natif » : `tcg/0013` (`x-fp-flat`) et `tcg/0014` (`x-fp-native`)

30/09/2026. Copie isolée `~/src/qemu-fpnat` (clone APFS de `~/src/qemu`, qui n'a pas été
touché ; `qgpu` identique octet pour octet à la référence), construite dans `bfn/` ; binaires
rangés dans `~/src/qemu-fpnat/bin/<nom>/qemu-system-ppc64` (`fx` : l'essai de borne ; `nat2` :
0013 + 0014, celui des mesures ; `nat3` : le même avec le vérificateur affiné du §22.7, le
binaire final ; `mut1..6` : les mutants). Bancs invités sur un clone du disque
de dev (`bench/tcg/fpnat/dev.raw`, `tools/tcg/fpnatab.sh`), DOOM 3 sur des recouvrements neufs
de `disks/tiger-endurance.qcow2` (`tools/tcg/fpnatd3.py`) : la VM quotidienne n'a pas servi.
**Hôte chargé pendant toutes les mesures** : deux ou trois autres QEMU (chantier A4) tournaient,
charge 5 à 11 (`charge.txt` de chaque campagne) ; seuls les écarts entre modes entrelacés
comptent, pas les valeurs absolues.

### 22.1 La question

Le §15.7 avait classé « des opérations flottantes aarch64 dans le code généré » : tant que les
FPR vivent en mémoire comme des doubles, une op en ligne paie encore `fmov`, `fcvt`, le calcul,
`fcvt`, `fmov` et ne gagne que l'appel. Question reposée : FPR (ou temporaires) tenus dans des
registres flottants de l'hôte le temps d'un bloc, calcul en `fadd`/`fmul`/`fmadd` natifs —
faisable dans TCG 9.2, exact, et que rapporte-t-il ?

### 22.2 Ce que TCG 9.2 permet (lu dans le code)

- **Aucune opération flottante dans TCG** : les ops vectorielles (`TCG_TYPE_V64/V128`) sont
  entières (add, and, décalages, cmp, bitsel…). Du flottant natif demande une op nouvelle dans le
  cœur de TCG et dans le backend.
- **Un registre V ne survit à rien** : sur arm64 tous les V allouables sont « call-clobbered »
  (V8-V15, sauvés par l'ABI, sont exclus de l'allocation) et **chaque accès mémoire invité**
  (`qemu_ld/st` : `TCG_OPF_CALL_CLOBBER`) libère tous les registres sauvés par l'appelant. Dans du
  code flottant (`lfs` … `fmadds` … `stfs`), une valeur tenue en registre V serait déversée à
  chaque `lfs`/`stfs`. Garder les FPR en registres « le temps d'un bloc » demanderait des
  globales TCG de type V64, V8-V15 allouables pour le seul V64 et sauvés par le prologue, et un
  chemin lent sans branchement par instruction : hors de portée d'un patch — et la borne
  (§22.3) dit que l'essentiel du gain n'est pas là.
- **Ce qui coûte aujourd'hui** : le **branchement** que `x-fp-inline` émet à chaque instruction
  (« chemin court ou helpers ») termine un bloc de base TCG — globales réécrites, temporaires
  déversés, tout relu ensuite, à chaque `fadds` — puis le calcul fait dans le helper.

**Le temps vCPU aujourd'hui** (`sample` de 10 s du QEMU pendant la scène fixe de DOOM 3,
configuration retenue, partie `ref-1` du §22.9 ; `tools/tcg/samplesum.py`,
`tools/tcg/fpnatd3sum.py`) : le fil vCPU du jeu est occupé **71 %** du temps (l'autre 37 %), le
reste en attente (vCPU arrêté par l'invité) — le jeu n'est plus lié au seul vCPU. Du temps
occupé des deux fils (7 521 échantillons) :

| poste | part |
|---|---|
| code généré (self) | 60,8 % |
| flottant scalaire restant (`helper_fp32_fast`) | **7,6 %** (+ sa porte, FPRF et le branchement, dans le code généré) |
| AltiVec (`vfp_fma4`, `helper_vmaddfp`, `vsldoi`, `vperm`, `vfp_add4`) | ~6,5 % |
| verrou global (attente) | 4,1 % |
| TLB (`probe_access`, `mmu_lookup`, `tlb_fill`) | ~5 % |
| `lmw`/`stmw` | 2,1 % |
| horloge (`mftb` : `cpu_get_clock`, `mach_absolute_time`) | ~2,5 % |
| sorties indirectes (`tb_lookup`, `qht`) | ~2 % |

Le flottant scalaire simple pèse donc toujours ~8 % du temps vCPU en helper, plus sa part du
code généré ; le double précision reste négligeable (§15.7).

### 22.3 La borne : ce que coûte chaque morceau (`x-fpx-mode`, banc invité)

Binaire d'essai `bin/fx` (`patches/tcg/essais/0013-ppc-fpx-borne.patch`) : la propriété
`x-fpx-mode` change la traduction des sept instructions de `x-fp-inline` et de `fcmpu` :

| mode | ce que fait chaque instruction | exact ? |
|---|---|---|
| 0 | `x-fp-inline` tel quel (appel pur, porte, **branchement**, FPRF en ligne) | oui |
| 1 | l'appel de `helper_fp32_fast`, FPRF en ligne, **sans porte ni branchement** | non (NaN, dénormaux) |
| 4 | `x-fp-inline` avec un helper qui rend frA sans calculer (l'essai du §15.7) | non |
| 2 | ni appel ni branchement : frT = frA, FPRF en ligne | non (plancher) |
| 3 | frT = frA seulement | non (plancher) |

`BANC=10000000 fptest banc` (chaîne dépendante `fmadds fmuls fmsubs fadds`, 40 M op. ;
transformation 4×4 de sommets, 160 M op. ; « min/max », voir la réserve du §22.7), disque de
dev, SMP=2, propriétés de `run_tiger.sh` par défaut, deux passes `0 1 2 3 4 4 3 2 1 0`, trois
tours par démarrage (`bench/tcg/fpnat/borne1`, `tools/tcg/fpnatsum.py`) :

| mode | chaîne | sommets | min/max |
|---|---|---|---|
| 0 `x-fp-inline` | 245 ms — 6,14 ns/op | 703 ms — 4,40 ns/op | 129 ms |
| 1 sans branchement | 243 ms — 6,09 | **549 ms — 3,43** | 97 ms |
| 4 sans calcul (avec branchement) | 85 ms — 2,14 | 424 ms — 2,65 | 104 ms |
| 2 plancher avec FPRF | 34 ms — 0,86 | 188 ms — 1,18 | 74 ms |
| 3 plancher | 18 ms — 0,46 | 111 ms — 0,69 | 57 ms |

(médianes de 6 tours, dispersion ±3 %.) Par opération :

- **le branchement** (0 → 1) : −1,0 ns sur les sommets (−22 %), rien sur la chaîne (bornée par
  la latence) ;
- **le calcul dans le helper** (0 → 4) : 1,75 ns (sommets) ; 4,0 ns (chaîne : `fmov`, `fcvt`,
  `fmadd`, `fcvt`, `fmov` et les tests, en série) ;
- l'appel lui-même ~0,4 ns, FPRF en ligne ~0,5 ns ; le plancher (lectures et écritures de
  `env`) 0,5-1,2 ns.

Borne hôte (`tools/tcg/fpnatbench.c` : mêmes motifs en C natif, FPR relus et écrits en mémoire
à chaque opération comme le fait le code généré) :

| | chaîne | sommets |
|---|---|---|
| appel de `helper_fp32_fast` | 5,8 ns/op | 2,8 ns/op |
| même calcul, mêmes tests, **en ligne** | 6,2 | 2,1 |
| rien (lectures/écritures, FPRF) | 1,5 | 1,5 |
| suite entière **en registres** (FPR lus au début, un test cumulé, écrits à la fin) | **2,3** | 1,8 |

Avec les FPR en mémoire, le calcul en ligne gagne ~0,7 ns/op en débit et rien en latence (le
§15.7 avait raison sur ce point) ; seuls des FPR en registres coupent la latence d'une chaîne
(6 → 2,3 ns), ce que TCG ne tient pas (§22.2). Mais dans le code généré, « en ligne » veut aussi
dire **sans branchement ni appel** : c'est là le gros du gain (modes 1 et 2).

**Projection DOOM 3** (avant de construire) : ~2,6 M instructions concernées par image
(34,7 M/s à 74 ms/image au §15.1, même travail par image depuis), gain de 0,4 (motif chaîne) à
1,4 ns (motif sommets) chacune ⇒ **1 à 3,6 ms/image, 2 à 6 %** : au-dessus du seuil de 2 %.

### 22.4 `tcg/0013` : un appel, aucun branchement (`x-fp-flat`)

`x-fp-flat` (propriété de CPU, défaut éteint, prime sur `x-fp-inline`, n'agit qu'avec
`x-fast-fp` ; `FPFLAT=1 ./run_tiger.sh`) traduit les mêmes huit instructions en **un appel** et
**aucun branchement** :

    (r, fpscr) = helper_fp32_flat(env, frA, frB, frC, FPSCR, op | frT << 8)   NO_WG, rend un i128
    frT = r ; FPSCR = fpscr ; (Rc=1) CR1 depuis le FPSCR
    fcmpu : (crf, fpscr) = helper_fcmpu_flat(env, frA, frB, FPSCR, bf) ; CR[bf] = crf

Le choix est fait **en C**. Le helper est une **fonction feuille** (sans cadre de pile) qui fait
le chemin court de `x-fp-inline` — `fpi_gate()`, `fpi_fp32()` (le calcul de `helper_fp32_fast`,
désormais toujours en ligne), `fpi_fpscr_arith()`, drapeaux softfloat = `inexact` ; sinon il
passe la main à `fp32_flat()`, hors ligne, qui repart des mêmes entrées et fait **la séquence
d'origine elle-même**, dans le même ordre, avec l'adresse de retour du code généré :
`reset_fpstatus`, l'opération (`float64r32_*` et son `flags_handler`, `do_fmadds`), **frT écrit
en mémoire**, `compute_fprf_float64`, `do_float_check_status` (`fcmpu` : `do_fcmpu`, nouveau,
puis `do_float_check_status`). Ce que ça demande :

- **`TCG_CALL_NO_WG`** : TCG réécrit les globales sales avant l'appel (une exception FP
  différée peut être levée depuis le helper, `cpu_restore_state` les veut à jour) et garde ses
  copies après. Le helper n'écrit aucune globale du point de vue de TCG : le nouveau FPSCR (et le
  champ CR de `fcmpu`) revient dans le résultat et le code généré l'affecte. La séquence
  d'origine écrit bien `env->fpscr` (et `env->crf[bf]`) en mémoire — la valeur même qui est
  rendue.
- **Le FPSCR passe en argument** : le chemin court ne lit rien en mémoire (§22.6).
- **frT écrit avant `float_check_status`** : dans la séquence d'origine, l'exception différée
  est levée *après* la mise à jour de frT ; le chemin lent l'écrit donc lui-même
  (`*cpu_fpr_ptr(env, frT) = r`), le code généré le réécrit ensuite.
- **L'adresse de retour** : `do_fcmpu`, `do_fmadds`, les `flags_handler` et
  `do_float_check_status` reçoivent celle que le helper plat a capturée (et non `GETPC()`, qui
  désignerait le helper).
- `x-fp-verify` couvre `x-fp-flat` : sous le vérificateur, tout passe par `fp32_flat()`, dont
  le chemin court appelle `helper_fpv_arith`/`_fcmpu` du §15.

### 22.5 `tcg/0014` : le chemin court en instructions flottantes de l'hôte (`x-fp-native`)

Une op TCG nouvelle, **`INDEX_op_ppc_fp32`** (cœur de TCG : `tcg-opc.h`, `tcg_gen_ppc_fp32`,
`tcg_op_supported`, contrainte `C_N2_I4`, l'optimiseur oublie ses copies mémoire ; backend
arm64 seulement, `TCG_TARGET_HAS_ppc_fp32`), faite comme un accès mémoire invité : **chemin
rapide en ligne, chemin lent hors ligne** (une étiquette de plus dans `ldst_labels`, émise en fin
de bloc).

    sorties : r (CR pour fcmpu), FPSCR ; entrées : frA, frB, frC, FPSCR (i64 en GPR)
    constantes : op | frT << 8, le helper lent, (offset | valeur) des drapeaux softfloat
    drapeaux : TCG_OPF_CALL_CLOBBER | TCG_OPF_SIDE_EFFECTS (comme un qemu_ld)

Chemin rapide (`tcg_out_ppc_fp32`, ~50 instructions pour `fmadds`, aucun branchement avant le
dernier) : la porte (`(fpscr & (XX|XE|OE|UE|RN)) ^ XX`) ; pour chaque opérande le test de
`fpi_zon()` en six instructions (`lsl`, `sub`, `tst`, deux `ccmp`, `csinc`) ; `fmov`, `fcvt`
simple, `fadd`/`fsub`/`fmul`/`fmadd` **simple précision** — `fmsubs` : `fneg` de frB avant,
`fnmadds`/`fnmsubs` : `fneg` après l'arrondi, exactement comme `helper_fp32_fast` ; `fcvt`
double, `fmov` ; le test du résultat (somme finie ; produit nul, ou `FLT_MIN < |r| < inf`) ;
FPRF et FI en entier ; chaque test raté incrémente un registre, **un seul `cbnz`** vers le
talon ; sur le chemin court, `strh` des drapeaux softfloat (`inexact`). `fcmpu` : porte, pas de
NaN, clé signe-grandeur → complément à deux (`csneg`), `csel` 8/4/2, FPCC et FI.

Le **talon hors ligne** passe les opérandes par les temporaires réservés (aucun conflit
d'affectation possible), `x0 = env`, `w4 = op | frT << 8`, `x5` = l'adresse de retour dans le
code généré (comme les chemins lents mémoire), appelle **`ppc_fp32_native_slow`** — c'est
`fp32_flat()` du §22.4 — et rend `x0:x1` dans les sorties. Registres de travail : les GPR sauvés
par l'appelant qui ne tiennent ni entrée ni sortie, et V0-V3 (l'op est `CALL_CLOBBER` : rien de
vivant n'y reste). Sans backend arm64, `x-fp-native` est `x-fp-flat`.

`x-fp-verify` avec `x-fp-native` : après chaque op, `helper_fpn_verify` refait la séquence
d'origine depuis le FPSCR de départ et compare résultat, FPSCR, drapeaux et état d'exception ;
tout l'état touché est remis ensuite (pas quand MSR[FE] ≠ 0 : compté à part, « non vérifiés »).

### 22.6 Ce qu'a appris `x-fp-flat`

Le premier `x-fp-flat` était **plus lent** que `x-fp-inline` (sommets 898 contre 696 ms) alors
que le mode 1 de la borne gagnait 22 %. Deux causes, trouvées l'une après l'autre :

1. `TCG_CALL_NO_WG` réécrit les globales sales avant l'appel, et le helper relisait aussitôt
   `env->fpscr` : chaque instruction flottante attendait la précédente **par la mémoire**. Le
   FPSCR passe désormais en argument — sans effet mesurable à lui seul (913 ms) ;
2. le vrai coût : `helper_fp32_flat` sauvait **six paires de registres** à chaque appel
   (prologue imposé par le chemin lent) et appelait `helper_fp32_fast` hors ligne. Banc hôte
   des objets réels : 4,5 ns par appel contre 2,5 pour `helper_fp32_fast`. D'où la fonction
   feuille du §22.4 : 3,2-3,5 ns par appel.

Même ainsi, `x-fp-flat` ne rend qu'une partie du mode 1 (§22.8) : l'appel `NO_WG` synchronise
les globales à chaque instruction et le helper fait FPRF et les drapeaux que le mode 1 laissait
au code généré (où TCG supprime ceux qui sont écrasés). Il reste utile comme **chemin lent de
`x-fp-native`** et comme repli sur un hôte qui n'est pas arm64.

### 22.7 La preuve

**Hôte** (`tools/tcg/fpproof.sh`, qui compile `fpproof.c` avec `-DFPPROOF_FLAT
-DFPPROOF_NATIVE` quand l'arbre a les patches, lié aux vrais objets de l'arbre construit) : pour
chaque vecteur du §15.5 (catalogue croisé, puis aléatoire ; FPSCR dans trois familles, dont
toutes les trappes armées ; MSR[FE] au hasard), `helper_fp32_flat`/`helper_fcmpu_flat` **et**
`ppc_fp32_native_slow` (le talon de `x-fp-native`) sont appelés depuis le même état que la
séquence d'origine, **chemin court ou non** : résultat, frT en mémoire, FPSCR, drapeaux,
exception levée ou non, `exception_index`, CR doivent être égaux.

| `fpproof.sh ~/src/qemu-fpnat 20000000` (graine 0x5eed) | vecteurs | exceptions levées | divergences |
|---|---|---|---|
| chemin court de `x-fp-inline` (§15.5) | 174 427 024 (97 691 628 courts) | — | **0** |
| `x-fp-flat`, tous chemins | 174 427 024 | 5 537 173 | **0** |
| talon de `x-fp-native`, tous chemins | 174 427 024 | 5 537 173 | **0** |

Contre-épreuves : `tools/tcg/fpflat-mut.sh` (copie de `fpu_helper.c` mutée et recompilée) :
**10 mutations sur 10 détectées** (frT non écrit avant l'exception différée 1 116 057
divergences ; sans `reset_fpstatus` 12,5 M ; `fmuls` lent sur frB 62 996 ; drapeaux du chemin
court oubliés 1,1 M ; porte retirée du chemin court 1,1 M ; `fcmpu` lent sans FI 15 282 ;
`fnmsubs` avec les drapeaux de `fnmadds` 1,6 M ; ancien FPSCR rendu 12 M ; CR faux pour
l'égalité 168 ; CR lu avant la comparaison 30 509) ; `fpproof-mut.sh` (le modèle de
`x-fp-inline`, dont `fpi_fp32` fait désormais partie) : 10 sur 10.

**Encodages** : les 25 formes brutes émises par `tcg_out_ppc_fp32` comparées à celles de
l'assembleur de l'hôte : 0 différence.

**Patches** : 0013 puis 0014 appliqués aux fichiers de `~/src/qemu` redonnent l'arbre de
travail à l'octet.

**Invité** (`tools/guest/jobs/fptest`, les vraies instructions dans Tiger, disque de dev,
SMP=2 ; 30 124 880 instructions, onze états du FPSCR, §15.5) :

| binaire, mode | empreinte | vérificateur |
|---|---|---|
| `x-fp-inline` (référence) | `755efae4e391b7ea` | — |
| `x-fp-flat` + `x-fp-verify` | `755efae4e391b7ea` | 7 001 644 chemins courts vérifiés, **0 divergence** |
| `x-fp-native` + `x-fp-verify` | `755efae4e391b7ea` | **32 288 599 ops vérifiées** (6 999 243 par le chemin court), **0 divergence**, 3 149 non vérifiées (MSR[FE]) |

L'empreinte est celle du §15.5 (identique octet pour octet à QEMU sans `x-fp-inline`) ; elle
l'est aussi pour `x-fp-flat` et `x-fp-native` **sans** vérificateur (le chemin court de la
fonction feuille de `x-fp-flat` n'est pris que hors vérificateur).

Contre-épreuve invitée (`tools/tcg/fpnat-mut.sh` : un binaire par mutation de l'émetteur arm64,
job `fptest` avec `NRAND=16384`, `x-fp-native` + `x-fp-verify`, `bench/tcg/fpnat/mut-nat1`) :
**6 mutations sur 6 détectées** — frB non testé 45 369 divergences ; `|r| = FLT_MIN` accepté
3 419 ; `fmsubs` sans la négation de frB 149 222 ; `fcmpu` avec −0 ≠ +0 319 ; porte sans RN
174 440 ; FPRF de −0 faux 2 794 (sur ~8,6 M opérations vérifiées chacune, empreintes toutes
différentes).

**Tour réel** (`tools/tcg/fpnatd3.py`, DOOM 3 sur un recouvrement de `tiger-endurance.qcow2`,
démarrage de Tiger, bureau, chargement, cinématique et scène fixe, `x-fp-native` +
`x-fp-verify`) : **12 504 396 235 opérations vérifiées, dont 12 425 672 585 par le chemin
court natif, 0 divergence**, 0 non vérifiée (`bench/tcg/fpnat/d3/natv-2/qemu.log`). Une
première partie vérifiée n'en couvrait que 12 % : **DOOM 3 tourne avec MSR[FE] ≠ 0** et le
vérificateur sautait alors toute opération ; il ne saute plus que celles où la séquence
d'origine pourrait lever (trappe armée au FPSCR, ou exception différée en attente) — aucune
dans ce tour. Taux du chemin court natif en jeu : **99,4 %** (replis : `fsubs` 40,7 M,
`fcmpu` 35,3 M, `fmsubs` 2,6 M, le reste < 0,1 M ; au §15.9 les mêmes venaient surtout d'un
FPSCR non amorcé).

### 22.8 Gains sur le banc invité

Binaire final `bin/nat2`, trois modes entrelacés `ref plat nat nat plat ref ref plat nat`
(`bench/tcg/fpnat/banc-nat2`), trois tours par démarrage ; hôte chargé (charge 5-7, trois
autres QEMU) :

| mode | chaîne | sommets | min/max |
|---|---|---|---|
| `x-fp-inline` (référence) | 254 ms — 6,35 ns/op | 731 ms — 4,57 ns/op | 124 ms |
| `x-fp-flat` | 248 ms (−2 %) | 690 ms (−6 %) | 120 ms (−3 %) |
| **`x-fp-native`** | **223 ms (−12 %)** | **480 ms (−34 %)** | **94 ms (−24 %)** |

(médianes de 9 tours pour la référence, 6 pour les autres, tous joués entre 14 h 56 et 15 h 20 ;
dispersion ±2 %. `plat-3` et `nat-3`, joués plus tard sur un hôte plus calme — 577 et 411 ms
sur les sommets — sont écartés : leur rapport, 0,71, est celui du tableau.) Le natif descend
sous le mode 1 de la borne (549 ms,
sans branchement mais avec appel) : il reste ~1,8 ns par opération au-dessus du plancher (1,2),
le coût des ~50 instructions et des déversements qu'impose `CALL_CLOBBER`.

**Réserve sur « min/max »** (le banc « fcmpu » du §15.6) : sous le vérificateur de `x-fp-flat`,
il ne passait que ~2 000 `fcmpu` là où l'on en attendait 60 M. Le gcc de Tiger a compilé ces
comparaisons en **`fsubs` + `fsel`** (`fptest.s` avec `DIS=1` : 4 `fsel`, 0 `fcmpu`,
`bench/tcg/fpnat/dis1`) : ce banc mesure `fsubs`, pas `fcmpu` — le §15.6 lui attribuait à
tort −41 % de `fcmpu`. `fcmpu` n'est exercé que par l'équivalence de `fptest` et par les jeux.

### 22.9 DOOM 3 sur recouvrement (jeu de contrôle, pas l'A/B)

`tools/tcg/fpnatd3.py` : une partie par QEMU neuf sur un recouvrement neuf de
`disks/tiger-endurance.qcow2` (sans écran, `qgpu` `backend=auto`), `demo_mars_city1` en
fenêtre 640×480, règle de fenêtre de la matrice (T+50..T+280), binaire `bin/nat2`, SMP=2,
propriétés de `run_tiger.sh` par défaut ; `bench/tcg/fpnat/d3/`, `tools/tcg/fpnatd3sum.py`.
**Pas l'hôte au repos** : une VM du chantier A4 tournait à côté pendant presque toutes les
parties (charge 2 à 4) ; une longue mesure A4 a coupé la série en deux (20 h 00, puis 1 h 28 -
3 h 10).

| partie (ordre) | ref-1 | nat-1 | plat-1 | plat-2 | nat-2 | ref-2 | ref-3 | nat-3 | nat-4 | ref-4 | ref-5 | nat-5 | nat-6 | ref-6 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ms/image | 59,6 | 63,7 | 62,5 | 62,0 | 57,6 | 66,8 | 60,8 | 57,7 | 57,6 | 59,9 | 59,8 | 57,1 | 57,6 | 59,8 |

- **`x-fp-inline` (réf.) : médiane 59,85 ; `x-fp-native` : 57,6 ms/image, −3,8 %** (6 + 6 ;
  Mann-Whitney U = 5, p ≈ 0,04 bilatéral). Sur les huit dernières, strictement entrelacées et
  dans les mêmes conditions : 59,85 → 57,6, les quatre natives sous les quatre références. Une
  partie lente dans chaque mode (nat-1 63,7, ref-2 66,8), toutes deux dans la même période
  chargée.
- `x-fp-flat` : 62,0 et 62,5, dans la période où la référence faisait 66,8 : non concluant.
- Profil (`sample` de 10 s, fil vCPU le plus occupé) : les helpers du flottant scalaire passent
  de **7,8 % à 0,3-0,5 %** du temps occupé.
- Le gain (~2,2 ms/image) est dans la projection du §22.3 (1 à 3,6 ms/image) ; le fil vCPU du
  jeu n'étant occupé qu'à ~70 % (§22.2), tout le temps vCPU gagné ne se retrouve pas à
  l'image.

### 22.10 Essayé et classé

- **FPR tenus en registres flottants de l'hôte le temps d'un bloc** (la question du §22.1) :
  non construit. Dans TCG 9.2 un registre V ne survit ni à un appel ni à un accès mémoire
  invité (§22.2) ; il faudrait des globales V64, V8-V15 allouables et sauvés par le prologue, et
  une vérification sans branchement par instruction. La borne hôte dit ce que ça rapporterait
  en plus de `x-fp-native` : la latence d'une chaîne (≈ 5,6 → 2,3 ns/op) — pas le débit (3,0
  → ~1,8). À reconsidérer seulement si le profil montrait des chaînes de flottant scalaire
  longues et dominantes.
- **Supprimer `CALL_CLOBBER`** de l'op native (le talon sauverait lui-même tous les registres
  appelant-sauvés, V compris) : pas fait ; le déversement évité par op est incertain, le talon
  coûterait ~400 octets de sauvegarde à chaque repli.
- **Élimination statique des tests d'opérande** (un résultat de `fmuls`/`fmadds` natif est un
  simple normal ou nul) : pas fait — faux quand le chemin lent a produit le résultat (NaN,
  dénormal), et rien dans le code généré ne dit lequel des deux a tourné.
- **`x-fp-flat` seul** : exact mais ne rend qu'une partie de la borne (−6 % sur les sommets) ;
  gardé comme chemin lent de `x-fp-native` et repli des hôtes non arm64, pas proposé seul.

### 22.11 L'A/B DOOM 3 à jouer (VM quotidienne)

Binaire `~/src/qemu-fpnat/bin/nat3/qemu-system-ppc64` (`nat2` plus le vérificateur affiné du
§22.7, code hors vérificateur identique) : l'arbre de référence (0001-0012,
`qgpu` v19 identique octet pour octet) + 0013 + 0014, toutes les propriétés nouvelles éteintes
par défaut (ses fichiers de données par `~/src/qemu-fpnat/bin/share/qemu`, lien vers le
`qemu-bundle` de `bfn/`). Depuis le dépôt principal (son `run_tiger.sh` passe `CPU_OPTS`), hôte
au repos, aucun autre QEMU :

    QEMU_BIN=~/src/qemu-fpnat/bin/nat3/qemu-system-ppc tools/tcg/matab.sh ab-fpnat 6 \
        "n0:" "n1:CPU_OPTS=x-fp-native=on"
    # une partie vérifiée, pas pour la vitesse (bilan « fp-native-verify » dans
    # bench/tcg/ab/fpnat-verif/v1-1/run_tiger.log, 0 divergence attendu)
    QEMU_BIN=~/src/qemu-fpnat/bin/nat3/qemu-system-ppc tools/tcg/matab.sh fpnat-verif 1 \
        "v0:CPU_OPTS=x-fp-verify=on" "v1:CPU_OPTS=x-fp-native=on,x-fp-verify=on"
    tools/tcg/matab.sh --restore

Après fusion, `FPNATIVE=1` fait la même chose que `CPU_OPTS=x-fp-native=on`. Attendu (§22.9) :
`n0` ~60 ms/image, `n1` ~57,5-58 (−3 à −4 %) ; partie vérifiée : 0 divergence, 0 non
vérifiée.
Puis, avant de l'allumer par défaut (`FPNATIVE` à 1, 0013 et 0014 dans le binaire de
référence), un tour de la matrice complète sur ce binaire, comme au §21.

---

## 23. La base de temps par le compteur de l'hôte : `tcg/0015` (`x-tb-fast`)

01/10/2026. Copie isolée `~/src/qemu-tbfp` (clone APFS de la référence, construite dans
`btf/`), VM quotidienne.

### 23.1 Le poste

Le profil du 01/10 (`docs/vitesse-profil-2026-10-01.md`) met `mftb`/`mftbu`
(`helper_load_tbu`, `cpu_ppc_load_tbl`) à 2-3 % du temps vCPU de chaque jeu. Un `mftb` coûte
**27 ns** dans l'invité (`tools/guest/jobs/tbtest`). Le chemin d'origine :
`qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)` = seqlock + `cpu_clock_offset` + `get_clock()`
(`clock_gettime(CLOCK_MONOTONIC)`, 23 ns sur l'hôte), puis `muldiv64()` et sa division 128 bits.

Banc hôte (`tools/tcg/tbclock.c`) : sur ce Mac, `cntvct_el0` bat à **1 GHz** (`cntfrq_el0`) et se
lit en 0,9 ns. Deux surprises :

- `CLOCK_MONOTONIC` de macOS n'a qu'**une microseconde de résolution** : dans l'invité, 92 % des
  lectures successives de la base de temps rendaient la même valeur (pas de 25 ticks) ;
- il est **ralenti par la synchronisation horaire** : −12,5 ppm contre `cntvct` (−62 µs toutes
  les 5 s), alors que `CLOCK_MONOTONIC_RAW` et `CLOCK_UPTIME_RAW` le suivent à la ns.

Un premier essai (TB calculée depuis `cntvct`, calée une fois sur `get_clock()`) dérivait donc
de centaines de µs contre la valeur d'origine en quelques minutes (`x-tb-verify`).

### 23.2 La conception

`x-tb-fast` (propriété de CPU, éteinte par défaut ; `TBFAST=1 ./run_tiger.sh`) :

- **l'horloge de QEMU elle-même** devient le compteur : `get_clock()` = `cntvct_el0 + K`
  (`include/qemu/timer.h`, `qemu_raw_clock_enable()` dans `util/qemu-timer-common.c`). K est
  calé une fois, au démarrage de la machine, contre `CLOCK_MONOTONIC` (20 000 échantillons
  encadrés de deux lectures du compteur : borne basse de l'écart, tolérance de 100 ns pour le
  pas de 41,7 ns de l'horloge de macOS) : pas de saut à la bascule. L'horloge de QEMU devient
  linéaire, à la ns, en 1 ns par lecture ; elle ne suit plus les corrections de fréquence de
  l'hôte (12 ppm), ce qui ne gêne pas l'invité. Refusé (horloge d'origine, message) si le
  compteur ne bat pas à 1 GHz ;
- **`mftb`** fait le même calcul que l'origine sans division 128 bits :
  `(cpu_clock_offset + get_clock()) / (1e9 / tb_freq) + tb_offset` (le plancher de `muldiv64`
  quand `tb_freq` divise 1e9 : 25 MHz ⇒ 40), l'offset lu sous le seqlock
  (`cpu_get_clock_offset()`, nouveau).

`x-tb-verify` recalcule la valeur d'origine juste après chaque lecture rapide : l'écart
rapide − origine doit être 0, ou négatif du nombre de ticks écoulés entre les deux lectures.

### 23.3 La preuve

| épreuve | résultat |
|---|---|
| `tbtest` + bureau + DOOM 3, Prey, Nexuiz (`x-tb-verify`) | **1 610 612 737 lectures, 0 écart positif**, écart −785..0 ticks ; 598 lectures d'origine retardées de plus de 1 µs (préemption entre les deux) |
| `tbtest` (monotonie, fréquence) | 0 recul, 24,99 MHz |

### 23.4 Gains

`mftb` dans l'invité : 27 → 12 ns avec le vérificateur, **5,5 ns** sans. Effet de bord : la TB
avançant à la ns, le chronométrage de `dcba` du noyau (`commpage_time_dcba`, au démarrage) peut
la juger rentable — selon le démarrage (vu une fois sur deux) : `hw.optional.dcba` = 1, bit 0x20
de `_cpu_capabilities`. `dcba` est un nop dans QEMU et les
routines de la libc qui s'en servent écrivent tout le bloc : résultat inchangé.

**Allumé par défaut le 01/10 au soir** (`TBFAST`, avec `FPNATIVE64`), à la demande de
l'utilisateur, sans tour de matrice ; binaire de référence reconstruit (précédent en
`~/src/qemu/build/*.avant-tbfp`).

### 23.5 A/B en jeu

`QEMU_BIN=~/src/qemu-tbfp/btf/qemu-system-ppc tools/tcg/matab.sh ab-tb 6 "t0:" "t1:TBFAST=1"
d3,nx fen` (VM quotidienne, `POMPPCFsqrt` chargé, hôte au repos, charge ≤ 2,9, 0 autre QEMU) :

| cellule | éteint (médiane, min..max) | `x-tb-fast` | écart |
|---|---|---|---|
| DOOM 3 fenêtre | 56,5 (56,2..57,6) | 56,2 (55,4..56,3) | **−0,6 %** |
| Nexuiz ARB fenêtre | 72,9 (72,4..73,7) | 72,3 (71,6..72,5) | **−0,8 %** |

Gain faible mais de même sens sur les deux jeux, et presque sans recouvrement : le profil
promettait 2-3 % du temps vCPU, mais le temps d'image n'en récupère qu'une partie (le helper
reste appelé, seule sa durée baisse).

## 24. Le flottant double par le FPU de l'hôte : `tcg/0016` (`x-fp-native64`)

01/10/2026, même copie isolée.

### 24.1 Le poste

`x-fp-native` (§22) ne couvre que le simple. Le profil du 01/10 met le double (`helper_FMADD`,
`FNMSUB`, `FSUB`, `FADD`, `do_float_check_status`, FPRF) à ~7 % du temps vCPU de Nexuiz, ~4 % de
Prey. En jeu, 98 % de ces opérations prennent le chemin court (§24.4).

### 24.2 La conception

Les formes double de l'op `INDEX_op_ppc_fp32` : sélecteurs `FPI_DADD..FPI_DNMSUB` (8 à 14,
`internal.h`), même porte, même talon hors ligne, même chemin lent (`fp32_flat()`, qui fait
désormais aussi `float64_add/sub/mul` et `do_fmadd` avec l'adresse de retour du code généré).

- **Modèle C** (`fpi_fp64()`, `fpu_helper.c`) : opérandes zéro ou normaux en double
  (`fpi_zon64()`) ; une opération IEEE double arrondie au plus proche par l'hôte ; retour aux
  helpers sur débordement, sur une **somme minuscule non nulle** (exacte, mais FPRF dirait
  « dénormal »), sur un **produit non nul minuscule, DBL_MIN compris** (dépassement par le bas,
  la tininess du PowerPC étant détectée avant l'arrondi). `fnmadd`/`fnmsub` : négation après
  l'arrondi, zéro compris.
- **Émetteur arm64** : `pfp_zon64()` (`t = x << 1` ; `t == 0` ou `t − 2^53 < 0x7fe·2^53`), les
  instructions `fadd`/`fsub`/`fmul`/`fmadd`/`fneg` en double (ftype 01), aucune conversion ; test
  du résultat : celui du simple avec k1 = DBL_MIN « << 1 », plus, pour les sommes, `r == 0` ou
  `|r| ≥ DBL_MIN`.
- **Traduction** : `fadd fsub fmul fmadd fmsub fnmadd fnmsub` passent par `do_fpd_*`
  (`x-fp-native64` : l'op ; sans backend arm64 : un appel du helper plat ; sinon les helpers
  d'origine).

### 24.3 La preuve

| épreuve | résultat |
|---|---|
| `fptest d 65536` et `fptest d 1048576` (formes double, 11 états du FPSCR, opérandes doubles : sous-normaux, voisins de DBL_MIN, produits qui sous-débordent ou débordent, annulations exactes ou à 1 ulp), éteint contre allumé | **identiques octet pour octet** (87 672 964 instructions, empreinte `4bdf8376a39ed79a`) ; `fptest` simple inchangé |
| `x-fp-verify` pendant `fptest d` | 134 M opérations vérifiées, 0 divergence |
| `x-fp-verify` en jeu (DOOM 3, Prey, Nexuiz, fenêtre) | **19 981 664 257 opérations vérifiées, 0 divergence**, dont en double : `fadd` 96 M, `fsub` 459 M, `fmul` 391 M, `fmadd` 385 M, `fnmsub` 77 M (98 % par le chemin court) |
| mutants de l'émetteur (opérandes infinis acceptés ; somme sous-normale acceptée ; produit = DBL_MIN accepté ; `fnmadd` nié avant l'arrondi) | **4/4 détectés** par `fptest d` (sortie différente) et par `x-fp-verify` (372 à 95 253 divergences) |

### 24.4 Gains

Banc invité (`fptest banc-d 10000000`, 3 tours, même démarrage de VM par mode) :

| | éteint | `x-fp-native64` |
|---|---|---|
| chaîne dépendante `fmadd fmul fmsub fadd` | 221 ms | **132 ms (−40 %)** |
| sommets 4×4 en double | 935 ms | **336 ms (−64 %)** |

### 24.5 A/B en jeu

`QEMU_BIN=~/src/qemu-tbfp/btf/qemu-system-ppc tools/tcg/matab.sh ab-fp64 6 "f0:"
"f1:FPNATIVE64=1" nx,prey fen` (mêmes conditions) :

| cellule | éteint (médiane, min..max) | `x-fp-native64` | écart |
|---|---|---|---|
| Prey fenêtre | 48,1 (47,1..48,8) | 47,3 (46,7..47,3) | **−1,7 %** |
| Nexuiz ARB fenêtre | — | — | **+0,1 %** (rien) |

Le ~7 % de flottant double du profil de Nexuiz venait surtout de la **racine logicielle**
`___sqrt` de la libm (itérations de Newton en double) : depuis `POMPPCFsqrt` (même jour), la
libm prend `fsqrt` et ce double a disparu. Reste Prey (squelettes, `idSIMD_Generic`).

## 25. Les mêmes sur un hôte x86-64 : `tcg/0017` à `0019`

02/10/2026, PC Linux (i7-10700F, Ubuntu 24.04). Arbre `~/src/qemu11` (clone neuf de v11.1.2 par
`build_qemu_qfb.sh`, un commit par patch), binaire de référence intact ; preuves sur la VM de dev
(`disks/tiger-dev.raw`, 10.4.6, mono-cœur, single-user, `devloop.py`). Jusqu'ici `x-fp-native`,
`x-fp-native64` et `x-tb-fast` ne valaient que sur arm64 (ailleurs : `x-fp-flat`, un appel par
instruction, et l'horloge d'origine), et `vperm` n'avait son `tbl` NEON que là. Les deux postes
de développement (M4 et ce PC) ont désormais le même niveau.

### 25.1 `tcg/0017` : l'émetteur x86_64 de `INDEX_op_ppc_fp32`

- **Même op, même talon, même chemin lent** que l'arm64 (§22, §24) ; seul l'émetteur change
  (`tcg/x86_64/tcg-target.c.inc`, `tcg_out_ppc_fp32`). Opérandes VEX scalaires (`vmovq`,
  `vcvtsd2ss`/`vcvtss2sd`, `vaddss`/`vsubss`/`vmulss` et leurs formes `sd`) et **FMA3**
  (`vfmadd231`, `vfmsub231` : une seule rondeur ; `a·c − b` est exactement `a·c + (−b)`).
  `fnmadd`/`fnmsub` : `btc r, 63` **après** l'arrondi, zéro compris, comme l'arm64.
- **Tests par sauts** : chaque test du chemin court (porte du FPSCR, `fpi_zon`/`fpi_zon64` des
  opérandes, résultat infini, somme double minuscule, produit non nul ≤ FLT_MIN/DBL_MIN) est un
  `jcc rel32` vers le talon hors ligne (jusqu'à 12 par op, `fp_jmp[]` du label), au lieu du
  registre « bad » accumulé sans branchement de l'arm64 : x86 a deux fois moins de registres et
  des branches non prises quasi gratuites. Constantes 64 bits par `movabs` ; FPRF/FPCC par
  `cmov`.
- **Registres** : op `TCG_OPF_CALL_CLOBBER`, donc les GPR sauvés par l'appelant qui ne portent
  aucun argument (au moins 3 sur 9 en SysV), les deux sorties avant d'être écrites et XMM0-3
  servent de brouillon. Talon : arguments par la pile (`push`/`pop`, toute affectation
  convient), `rdi` = env, `r8d` = sélecteur, `r9` = adresse de retour, retour `rax:rdx`.
- **Disponibilité à l'exécution** : `TCG_TARGET_HAS_ppc_fp32` vaut `have_avx1 && FMA3`
  (`CPUINFO_FMA`, nouveau dans `util/cpuinfo-i386.c`) ; sans eux, `x-fp-native` retombe sur
  `x-fp-flat` comme sur tout autre hôte. D'où `TCG_TARGET_PPC_FP32_IMPL` (constante de build,
  posée aussi par l'arm64) pour les `#if` de `tcg/tcg.c`, où une valeur d'exécution vaudrait 0
  en silence. ABI Win64 exclue (pas assez de registres sauvés par l'appelant).
- **Vérificateur** : `helper_fpn_verify` ignore désormais `float_flag_input_denormal_used`
  (QEMU 10+), que softfloat lève sur un opérande dénormal de `fcmpu` et que la cible PowerPC ne
  lit jamais. Faux positif commun à l'arm64 et au chemin C (même `fpi_fcmpu`), apparu avec le
  passage à 11.1.2 : 5 462 « divergences » `fcmpu` au premier tour, résultat, CR et FPSCR
  identiques.

### 25.2 `tcg/0018` : `x-tb-fast` par le TSC

`get_clock()` = `(rdtsc · mult) >> 32 + K` (`qemu_raw_clock_tsc_ns()`, produit 128 bits). Exigé :
TSC invariant (CPUID 0x80000007 EDX[8]) **et** source d'horloge du noyau `tsc`
(`/sys/devices/system/clocksource/clocksource0/current_clocksource`) : le noyau l'a jugé
synchronisé entre les CPU et stable. Sinon refusé, avec message. `mult` (ns par tick en 32.32)
est étalonné une fois contre `CLOCK_MONOTONIC` : deux couples (TSC, horloge) à 20 ms d'écart,
chacun le plus serré de 32 lectures encadrées (~2 ppm) ; K comme sur arm64 (20 000 encadrements,
borne basse, résolution 1 ns sous Linux). Le calcul de `mftb` (§23.2) est inchangé.

### 25.3 `tcg/0019` : `vperm` par `pshufb`

`helper_VPERM_FAST` sur hôte x86-64 avec AVX (sondé à l'exécution) : `idx = ~c & 31`, un
`pshufb` dans chaque moitié (b pour idx < 16, a sinon, 4 bits bas), `pblendvb` sur
`idx > 15`. C portable sans AVX.

### 25.4 La preuve

| épreuve | résultat |
|---|---|
| `fptest` (simple, 2^18) et `fptest d 65536`, `x-fast-fp` seul contre `x-fp-native` + `x-fp-native64` | **identiques octet pour octet** (30 124 880 et 11 978 884 instructions, empreintes `e80ec8026301ef1d`, `fb3e6006e03c4f53`) |
| `x-fp-verify` pendant `fptest`, `fptest d` et leurs bancs | **486 541 466 opérations vérifiées, 0 divergence en arithmétique** (449 972 515 par le chemin court ; `fadd` à `fnmsub` en double, `fadds` à `fnmsubs`) ; `fcmpu` : 5 462 faux positifs de drapeau (§25.1), **0** une fois le vérificateur corrigé (40 760 262 vérifiés) |
| mutants de l'émetteur x86 (1 opérandes doubles infinis acceptés ; 2 somme double sous-normale acceptée ; 3 produit double = DBL_MIN accepté ; 4 `fnmadd` nié avant l'arrondi ; 5 opérande simple non représentable accepté) | **5/5 détectés** : 1-4 par `fptest d`, 5 par `fptest` simple (sortie différente), et par `x-fp-verify` (372 à 103 499 divergences) |
| `tbtest` sous `x-tb-verify` | **1 216 403 275 lectures, 0 écart positif**, écart −3 360..0 ticks (15 728 lectures d'origine retardées de plus de 1 µs) ; 0 recul, 24,98 MHz |
| bureau Tiger **SMP=2** (`qemu-system-ppc64`, toutes les options de `run_tiger.sh`) + Marble Blast Gold, sous `x-fp-verify` et `x-tb-verify` | **956 016 000 opérations vérifiées, 0 divergence** (913 697 704 par le chemin court) ; **1 345 858 821 lectures de la base de temps sur deux vCPU, 0 écart positif** ; rendu du jeu juste |
| `vpermproof.sh` (hôte, `helper_VPERM` contre la version AVX tirés tels quels de `int_helper.c`) | **50 662 144 cas, 0 divergence** (recouvrements compris) ; C portable (`VPERMPROOF_NOAVX=1`) : 5,66 M, 0 |
| `vfptest` invité, `x-vperm-fast`/`x-vfp-fast` éteints contre allumés | empreintes identiques |
| clone neuf par `build_qemu_qfb.sh` | 0017-0019 posés sans fuzz, 25/25 capacités, sources identiques à l'arbre de travail |

### 25.5 Gains

A/B entrelacé avant/après (`ab.sh`) : binaire « avant » = le même arbre sans 0017-0019 (donc
`x-fp-flat` en repli, horloge d'origine, `vperm` en C), « après » = avec ; mêmes options que
`run_tiger.sh` par défaut (`x-fast-fp x-fp-inline x-fp-native x-fp-native64 x-tb-fast x-vfp-fast
x-vperm-fast x-lfs-inline`), deux démarrages de VM par binaire, trois tours chacun, hôte au repos
(charge ≤ 1,5). Médianes des 6 tours (écart min..max ≤ 3 %) :

| banc invité | avant | après | écart |
|---|---|---|---|
| `fptest banc` chaîne simple `fmadds fmuls fmsubs fadds` | 287 ms | **180 ms** | **−37 %** |
| `fptest banc` sommets 4×4 simple | 1 462 ms | **1 078 ms** | **−26 %** |
| `fptest banc` `fcmpu` | 185 ms | **146 ms** | **−21 %** |
| `fptest banc-d` chaîne double | 271 ms | **125 ms** | **−54 %** |
| `fptest banc-d` sommets 4×4 double | 1 227 ms | **749 ms** | **−39 %** |
| `mftb` (`tbtest`) | 31,8 ns | **17,5 ns** | **−45 %** |
| `vfptest` banc `vperm` | 452 ms | **95 ms** | **−79 %** |
| `vfptest` banc `vmaddfp…` (non touché, témoin) | 766 ms | 764 ms | 0 |

Contre `x-fast-fp` seul (sans `x-fp-inline` ni `x-fp-flat`), le même jour : chaîne simple
460 → 185 ms, sommets double 1 970 → 754 ms.

### 25.6 A/B en jeu (03/10/2026)

`tools/tcg/matab.sh x86-<jeu> 3 "avant:QEMU_BIN=~/src/qemu-avant0017/build/qemu-system-ppc"
"apres:" <jeu> fen` : binaire « avant » = la référence moins 0017-0019 (mêmes sources qgpu, même
tablette), VM quotidienne du PC (10.4.6, SMP 2, backend GL de la RTX 4060 Ti), parties entrelacées
A B B A A B, chacune sur un QEMU relancé, hôte au repos (0 autre QEMU). Seuls trois jeux de la
matrice sont sur ce PC.

| cellule | avant (médiane, min..max) | après | écart |
|---|---|---|---|
| UT2004 fenêtre | 62,7 (61,4..63,3) | 60,0 (58,5..61,6) | **−4,3 %** |
| Marble Blast fenêtre | 21,8 (21,5..22,2) | 21,0 (20,4..21,0) | **−3,7 %** |
| Zenerchi fenêtre (menu) | 7,5 (7,4..7,6) | 7,0 (6,8..7,1) | **−6,7 %** |

Gain modeste et de même sens partout, comme sur le M4 (§23.5, §24.5) : le temps d'image n'en
récupère qu'une partie, le reste est ailleurs (plugin, géométrie, rendu).

---

## 26. Le flottant AltiVec sur hôte x86-64 : `tcg/0020`

03/10/2026, PC Linux (i7-10700F). Restait un écart entre les deux postes dans `x-vfp-fast`
(§9) : sur x86, `vfp_fma4` était coupé en dur (`ok = false`, « softfloat peut y forcer la FMA
logicielle ») ; `vmaddfp` et `vnmsubfp` repassaient donc toujours par quatre `float32_muladd`.
D'où le « témoin » inchangé du §25.5 (`vfptest` banc `vmaddfp…` 766 → 764 ms).

### 26.1 La conception

- **La crainte d'origine ne tenait pas.** `force_soft_fma` (`fpu/softfloat.c`) n'est posé que
  si le `fma()` de la libc est faux (glibc < 2.23) ; et même alors, ce n'est qu'une
  optimisation de softfloat : chemin dur et chemin logiciel donnent le même résultat et les
  mêmes drapeaux dans les conditions que `vfp_fma4` exige. Ce qui compte, c'est que notre FMA
  à nous soit juste : une instruction FMA3 l'est par définition (un seul arrondi).
- **Hôte x86-64 avec FMA3** (`CPUINFO_FMA`, sondé à l'exécution, ajouté par `tcg/0017`) :
  `vfp_fma4_fma3` fait les quatre voies d'un `vfmadd231ps`, **et les mêmes décisions par
  voie** que la version scalaire, prises sur les motifs binaires (`pcmpgtd`/`pcmpeqd`,
  `movmskps`) : entrée nulle ou normale ssi `|x| = 0` ou `0x00800000 ≤ |x| ≤ 0x7f7fffff` ;
  `|r| ≤ FLT_MIN` ssi `|r| ≤ 0x00800000` (`r` n'est jamais un NaN : entrées finies).
  `vnmsubfp` : addende nié avant (`pxor`), résultat nié après, comme `float32_chs`.
- **Hôte x86-64 avec AVX** : `vfp_add4_avx`, `vaddps`/`vsubps` et les mêmes tests, au lieu
  de la boucle scalaire (déjà juste sur x86, elle n'était pas vectorisée).
- **Sans FMA3** (ou i386) : FMA toujours au logiciel, comme avant (`fmaf()` serait l'émulation
  lente de la libm). Sans AVX : la boucle scalaire d'avant.
- Le tout reste **entre les marqueurs `vfp-fast`**, donc extrait tel quel par `vfpproof.sh`.

### 26.2 La preuve

| épreuve | résultat |
|---|---|
| `vfpproof.sh ~/src/qemu11 20000000` (le vrai softfloat de 11.1.2), chemin AVX + FMA3 | **648 454 144 vecteurs (2,59 G voies), 0 divergence** (résultats et `float_status` entier) ; **97 486 278 par le chemin rapide : exactement le compte du M4** (§9.3), donc les mêmes décisions voie par voie |
| même banc, `VFPPROOF_NOX86=1` (chemin scalaire, 2 M) | 72 454 144 vecteurs, 0 divergence, 0 FMA par le chemin rapide (comme avant) |
| `vfpproof-x86-mut.sh` : 9 mutants (seuil `< FLT_MIN`, `|r| ≤ FLT_MIN` admis en add, un seul zéro exempte, addende nul exempte en FMA, overflow oublié, dénormaux admis, infinis admis, résultat ou addende de `vnmsubfp` non niés) | **9/9 détectés** (6 à 777 618 divergences) |
| `vfptest` invité (VM de dev, 10.4.11, `-faltivec`, NJ 0 et 1, catalogue 40³ et 2^22 vecteurs par NJ, plus `vperm`), référence contre 0020, options par défaut de `run_tiger.sh` | **empreinte identique** `bc13e93c39e62602` aux six démarrages |
| clone de la référence (`patch --fuzz=0 --dry-run`) | 0020 posé sans fuzz après 0019 |

`vfpproof.sh` suit QEMU 10+ : softfloat y est compilé une seule fois (`libcommon.a.p/`, plus
`libqemu-ppc-softmmu.a.p/`), et un `float_status` sans motif de NaN par défaut fait échouer
une assertion : le banc pose désormais les règles NaN de `cpu_init.c` (`-DVFPPROOF_QEMU10`).

### 26.3 Gains

A/B entrelacé avant/après (A B B A A B), VM de dev mono-cœur relancée à chaque fois, hôte au
repos (charge ≤ 0,6 sauf le premier démarrage, 3,0 au lancement, retombée pendant le boot),
binaire « avant » = la référence `~/src/qemu` (0017-0019, sans 0020), `BANC=50000000` :

| banc invité | avant (3 tours) | après (3 tours) | écart |
|---|---|---|---|
| `vfptest` 50 M × (2 `vmaddfp` + `vaddfp` + `vsubfp`) | 4 054 ms (3 771..4 065) | **1 492 ms** (1 489..1 495) | **−63 %** (×2,7) |
| `vfptest` 50 M × 4 `vperm` (témoin, non touché) | 470 ms (468..470) | 468 ms (467..476) | 0 |

Le PC fait désormais ce banc en 1,49 s contre 1,31 s sur le M4 (§9.3) ; avant, 4,05 s. Attendu
en jeu : les jeux du PC (Marble Blast, Zenerchi, UT2004) font peu d'AltiVec flottant (§2.2) ;
c'est DOOM 3 (`idSIMD_AltiVec`, `vmaddfp` 4,3 % du temps sur le M4, §16) qui en profitera, quand
il sera sur ce PC.

---

## 27. `x-sr-tlb` validé sur l'hôte x86-64 (03/10/2026)

Code indépendant de l'hôte, mais sa preuve (§4) n'avait tourné que sur le M4. Binaire de
référence reconstruit (11.1.2, 0001-0020), `x-sr-tlb-verify=64`, toutes les options par défaut :

| passe | contrôles gardés vérifiés | entrées retraduites | divergences |
|---|---|---|---|
| VM de dev (10.4.11), **SMP=1**, démarrage + ~35 min de compilations, `vfptest`, lectures de fichiers | 6 576 128 | 44 420 706 | **0** |
| VM quotidienne (10.4.6), **SMP=2**, démarrage + Marble Blast fenêtre, deux parties (`matab.sh x86-srtlb`) | 2 235 392 (cpu 0 : 1 144 832 ; cpu 1 : 1 090 560) | 11 072 952 | **0** |

Marble Blast sous vérificateur : 20,9 et 20,4 ms/image (21,0 sans, §25.6). Aucune divergence
« fenêtre `tlbie` » de l'autre CPU cette fois (§4 : une sur le M4, licite).

---

## 28. DOOM 3 sur l'hôte x86-64 : le traducteur poste par poste (`tcg/0021` à `0024`)

03/10/2026, PC Linux (i7-10700F, AVX2/FMA3). DOOM 3 (VM 10.4.11) y tourne à **150,5 ms/image**
contre 56-61 sur le M4, limité par le G4 émulé. Étape 2 du chantier vitesse : ce que le
traducteur fait encore par helper, ou plus mal que sur arm64, pour ce que DOOM 3 exécute.
**Phase d'étude et d'écriture** : l'hôte était pris par la VM quotidienne et les A/B d'un autre
agent ; rien n'a tourné dans une VM, aucun `make` complet. Copie isolée `~/src/qemu-d3tcg`
(clone de `~/src/qemu11`, un commit par patch ; `target/ppc`, `tcg`, `accel/tcg`, `fpu`,
`include`, `util`, `host` identiques octet pour octet à la référence `~/src/qemu` avant les
patches). Chaque fichier touché a été compilé seul (`tools/tcg/cc1.py`, nouveau : la ligne de
`compile_commands.json` de l'arbre construit, les en-têtes des sources pris dans la copie,
`nice 19`), pour `ppc` et `ppc64` ; les quatre patches s'appliquent sans fuzz à la suite de 0020
et redonnent la copie à l'octet.

### 28.1 Inventaire statique

**Le code de DOOM 3** : l'exécutable PPC de la démo Mac 1.3 (`doom3macdemo.dmg`, extrait par
`7z`, Mach-O lu par un petit script, même moteur que le jeu installé), désassemblé : 645 030
instructions dans le moteur, 576 652 dans `gameppc.dylib`. `idSIMD_AltiVec` (70 fonctions,
10 424 instructions) : `vperm` 603, `vmaddfp` 293, `vsldoi` 145, `vaddfp` 127, `stvewx` 94,
`vmrghw` 76, `vmrglw` 61, `vsubfp` 48, `vnmsubfp` 21, `vrsqrtefp` 11, `vrefp` 6 ; déjà en ligne :
`lvx` 664, `stvx` 217, `lvsl`/`lvsr` 215, `vaddubm` 137, `vspltw` 134, `vsel` 126.
`NormalizeTangents` (un des trois postes du profil du 01/10) : `vperm` 50, `vmaddfp` 49, `vsldoi`
40, `vaddfp` 40, `stvewx` 36. Profil dynamique du M4 (§6 bis, 598 M instr./s) : `vperm` 1,0 %,
`vmaddfp` 0,5, `vmrghw` 0,4, `vmrglw` 0,3, `vaddfp` 0,2 % des instructions.

| instructions de DOOM 3 | arm64 (M4) | x86-64 (PC) avant ces patches |
|---|---|---|
| `vaddfp vsubfp vmaddfp vnmsubfp` | helper **sans drapeau** (globales réécrites et relues), 4 voies NEON dedans (0003) | idem, 4 voies AVX/FMA3 dedans (0020) ; banc 7,5 ns/instr. contre 2,3 pour `vperm` |
| `vperm` | helper `NO_RWG`, `tbl` (0004) | helper `NO_RWG`, `pshufb` (0019) |
| `vsldoi`, `vmrghw`, `vmrglw` | helper `NO_RWG`, boucle d'octets/mots | idem |
| `stvewx` (`stvebx`, `stvehx`) | helper **sans drapeau** | idem |
| `vrsqrtefp`, `vrefp`, `vcmp*fp`, `vctsxs`, `vcfsx` | helpers softfloat par voie | idem (rares dans le code chaud) |
| scalaire simple, double | op `ppc_fp32` en ligne (0014, 0016) | même op, émetteur VEX/FMA3 (0017) |
| `lfs`/`stfs` | ops entières (0002) | idem (code commun) |
| `frsp`, `fctiwz`, `frsqrte`, `fdivs` | helpers (2 041, 546, 8, 610 dans le moteur) | idem |
| `mftb` | `cntvct` (0015), 5,5 ns | TSC (0018), **17,5 ns** : `rdtsc` ~5,5 ns, puis un `div r64` par une variable (~3,3 ns, banc hôte) |
| `lmw`/`stmw` | helper (`lmw` sans drapeau) | idem |

- **`x-lmw-inline` n'est ni actif ni dans la série** : c'est l'essai `essais/0002` (§5.4), écrit
  pour 9.2, jamais reposé sur 11.1.2 ; il ne couvrait que les plages tenant dans une page et
  n'avait rien gagné sur le M4 (−1,3 % au banc : le coût est celui des accès). DOOM 3 en a peu :
  209 `lmw` et 210 `stmw` dans le moteur, 79/71 dans `gameppc` (GCC 3.3 sauve les registres un
  par un) ; les 5,4 % du profil viennent des bibliothèques d'Apple, de GLEngine, du plugin et du
  noyau. Pas de patch tant que le profil x86 ne le désigne pas.
- **`helper_lookup_tb_ptr`** : la sonde en ligne de `x-ret-inline` (0008) est faite d'ops TCG
  génériques, la même sur les deux hôtes ; restent les ratés du cache de sauts (§18). Rien de
  propre à x86 — sauf l'appel du helper lui-même, ci-dessous.
- **`helper_ldul_mmu`** (chargement lent, 2,2 % sur le M4) : ratés du TLB, accès à cheval sur
  deux pages, MMIO (registres du `qgpu`). Le chemin rapide x86 (`movbe` après la comparaison du
  TLB) vaut celui de l'arm64 ; à reprendre avec le profil x86.
- **Ce que le code x86 fait de plus — trouvé en route, sans doute le plus gros** : sous Linux, le
  tampon du JIT est posé par le noyau vers `0x7f…`, à ~35 Tio du texte PIE de QEMU
  (`0x55…-0x5f…`). Tous les journaux du PC disent « `x-jit-near : pas de place, noyau … AUTRE
  fenêtre de 4 Gio` » : `x-jit-near` (0006) fait `mmap(NULL)` et espère, ce qui marche sur macOS,
  jamais sous Linux. Alors `tcg_out_branch()` ne peut émettre aucun `call rel32` : **chaque appel
  de helper depuis le code généré** (helpers, chemins lents de `qemu_ld/st`, `lookup_tb_ptr`,
  talons de `ppc_fp32`) est un `call *[rip+pool]`, un chargement plus un saut indirect, et 8
  octets de constante par cible dans chaque bloc.
- Registres : 12-13 GPR allouables sur x86 contre ~25 sur arm64 ; les ops `ppc_fp32` (0017) font
  leurs tests par `jcc` vers le talon (branches non prises), choix déjà fait au §25.1.

### 28.2 Les quatre postes retenus

| patch | propriété (défaut éteint) | quoi | attendu (temps vCPU, d'après le profil du M4) |
|---|---|---|---|
| `0022-tcg-vfp-native` | `x-vfp-native` (`VFPNATIVE=1`) | `vaddfp vsubfp vmaddfp vnmsubfp` dans le code généré (op TCG nouvelle), helper hors ligne | AltiVec flottant 4,3-6,6 % ; ~5 M instr./s × ~4-5 ns gagnées : **2,5-4 %** |
| `0024-tcg-jit-rel32` | `x-jit-rel32` (accélérateur, `JITREL32=1`) | tampon du JIT à < 2 Gio du texte : appels directs | tous les appels de helpers (dizaines de millions par seconde) : **1-5 %**, inconnu avant mesure |
| `0021-ppc-vmx-inline` | `x-vmx-inline` (`VMXINLINE=1`) | `vsldoi`, `vmrghw`/`vmrglw`, `stve[bhw]x` en ops TCG | ~6 M instr./s × 1,5-3 ns : **~1-1,5 %** |
| `0023-ppc-tb-div` | (sous `x-tb-fast`) | `mftb` : `/ 40` constant au lieu d'un `div r64` | ~1 M `mftb`/s × 3,3 ns : **~0,3 %** |

Ensemble : de l'ordre de **4 à 10 % du temps vCPU**, à confirmer par le profil x86 de DOOM 3. Le
fil du jeu étant presque toujours occupé sur le PC, l'essentiel devrait se retrouver à l'image.

### 28.3 `tcg/0021` : `x-vmx-inline`

Code commun aux deux hôtes (parité automatique). `x-vmx-verify` : mode preuve.

- `vsldoi vD,vA,vB,sh` : avec w0..w3 = vA.haut, vA.bas, vB.haut, vB.bas, q = sh/8,
  s = 8·(sh mod 8) : vD = (w[q], w[q+1]) si s = 0, sinon chaque double mot est
  `extract2(w[i+1], w[i], 64 − s)` (`shld` sur x86, `extr` sur arm64). 2 à 3 chargements, 2 ops,
  2 rangements, au lieu d'un appel et d'une boucle de 16 octets.
- `vmrghw` : `rh = deposit(a.haut, b.haut >> 32, 0, 32)`, `rl = deposit(b.haut, a.haut, 32, 32)` ;
  `vmrglw` de même sur les doubles mots bas. Opérandes lus avant d'écrire vD.
- `stvebx/stvehx/stvewx` (mode grand-boutiste ; petit-boutiste : helper) : le double mot
  (`EA & 8 ? bas : haut`, `movcond`) décalé de `64 − 8·taille − 8·(EA & 7)`, puis **un `qemu_st` de
  même taille, même endianité, même `mmu_idx`, même adresse** que `cpu_st*_be_data_ra` : même
  faute, même DAR/DSISR, globales synchronisées comme pour tout accès ; le helper d'origine était
  un appel sans drapeau.
- `vmrghb/h`, `vmrglb/h` : absents de DOOM 3, laissés aux helpers (l'entrelacement d'octets
  coûterait ~30 ops).
- `x-vmx-verify` : opérandes copiés avant (`vmxv_in`), puis le **helper d'origine** refait
  l'instruction et compare (vD reçoit sa valeur en cas d'écart) ; `stve*x` : l'élément que
  l'expression du macro `STVE` aurait rangé contre la valeur rangée. Bilan `vmx-verify:`.

### 28.4 `tcg/0022` : `x-vfp-native`, l'op TCG `ppc_vfp`

Le chemin court de `x-vfp-fast` (§9, §26) sans appel. **Op nouvelle sans opérande TCG** : les AVR
et `vec_status` vivent dans `env`, hors des globales ; constantes : le genre, les décalages de
vD vA vB vC, le helper d'origine et la porte. `TCG_OPF_NOT_PRESENT | CALL_CLOBBER` : allouée par
`tcg_reg_alloc_ppc_vfp()` comme un appel `NO_RWG` (registres appelant-sauvés libérés, aucune
globale synchronisée), `la_cross_call` en vivacité, `remove_mem_copy_all` dans l'optimiseur (elle
écrit `env` dans son dos). Chemin lent : étiquette de `ldst_labels`, talon hors ligne qui appelle
`helper_vaddfp`… `helper_vnmsubfp` (env, &vD, &vA, &vB, &vC) et revient ; ces helpers ne lèvent
rien, aucune adresse de retour n'est nécessaire.

- **Les mêmes décisions que `vfp_add4_avx()`/`vfp_fma4_fma3()`**, sur les motifs, t = x << 1 :
  opérande nul ou normal ssi `t == 0` ou `t − 0x01000000 ≤u 0xfdffffff` (`vpsubd`, `vpminud`,
  `vpcmpeqd`) ; résultat gardé ssi `FLT_MIN < |r| < inf` (`t − 0x01000001 ≤u 0xfdfffffe`), ou
  exempté si les deux opérandes sont nuls (add/sub) ou le produit nul (FMA). **Une voie infinie
  part au helper** (qui lève overflow) : le chemin court n'écrit que vD, jamais `vec_status`.
  `vmaddfp` : `vfmadd231ps` ; `vnmsubfp` : `vfmsub231ps` (a·c − b, égal à `fmaf(a, c, −b)`, zéro
  compris) puis le signe inversé après l'arrondi. Un `vmovmskps` et un `jne` vers le talon.
- **La porte** : `vfp_can_use_fpu(&vec_status)` (`!no_hardfloat`, inexact posé, arrondi au plus
  proche, pas de re-biaisage). Le `float_status` de 11.1.2 est en champs de bits, 12 octets avec
  GCC sur x86_64, `no_hardfloat` à l'octet 8 : un mot de 64 bits ne suffit pas (premier jet,
  attrapé par la preuve hôte : porte impossible, aucun chemin court). `ppc_vfn_gate_init()` (au
  `realize`) pose chaque champ seul dans un `float_status` nul, relit ses octets et range son
  masque dans la première de deux fenêtres de 64 bits (les 8 premiers, les 8 derniers octets) qui
  le contient : ici `+0 : 0x03070010 / 0x10` et `+4 : 0x1_00000000 / 0`. Si un champ ne tient dans
  aucune, `x-vfp-native` reste éteint (message).
- `x-vfp-native` n'agit qu'avec `x-vfp-fast` et l'op disponible : x86_64 avec AVX et FMA3 (sondé
  à l'exécution, `TCG_TARGET_HAS_ppc_vfp`), aarch64. **Émetteur aarch64 écrit, pas compilé** (pas
  de chaîne arm64 sur le PC) : NEON 4S, mêmes tests en comparaisons non signées (`cmhs`/`cmhi`),
  `fmla` (b nié avant pour `vnmsubfp`, résultat nié après), tous les tests accumulés sans
  branchement (`umaxv` des voies ratées, ou avec la porte) puis un `cbnz` ; registres X9-X11 (pas
  TMP0 : `tcg_out_ld()` le prend pour un décalage au-delà de l'immédiat, et les AVR sont après
  `spr_cb[]`). Encodages vérifiés à la main contre les constantes `Iqrrr_e_*` du backend (ADD,
  SUB, CMHI, CMHS, AND, ORR, CMEQ0, NOT) et l'ARM ARM (FADD, FSUB, FMLA, FNEG, UMAXV, UMOV) :
  **à compiler et prouver sur le M4 avant tout usage**.
- `x-vfp-native-verify` : opérandes et `vec_status` sauvés avant l'op (`helper_vfn_save`), puis
  l'instruction refaite par **la boucle par voie de softfloat** (QEMU sans `x-vfp-fast`) : vD et
  tout le `float_status` doivent être égaux ; compte aussi les opérations que le chemin court
  aurait dû prendre. Bilan `vfp-native-verify:`.

### 28.5 `tcg/0024` : `x-jit-rel32` (Linux x86-64)

Propriété de l'accélérateur. Avant `x-jit-near`, `alloc_code_gen_buffer_anon()` demande la place
juste sous le texte (`__executable_start`), par l'indication de `mmap` (Linux la prend telle quelle
si elle est libre ; le résultat est vérifié), 64 Mio plus bas à chaque essai, le tampon réduit de
64 en 64 Mio jusqu'à 512 Mio, tant que tout le tampon reste à moins de 2 Gio − 16 Mio de `etext`.
QEMU imprime le texte, l'écart maximal et « appels des helpers directs (rel32) » ou
« INDIRECTS ». La boucle seule, dans un PIE de test : tampon de 1 Gio posé à 1 089 Mio sous la fin
du texte, trois lancements sur trois. Aucune sémantique ne change (seul l'encodage des appels) :
pas de vérificateur ; la preuve est que QEMU l'annonce et que les épreuves des §25-27 et de la
matrice redonnent les mêmes empreintes. Sans objet sur arm64 (macOS : 0006 ; la portée d'un `bl`
est de ±128 Mio, qu'un tampon de 512 Mio ne tiendrait pas) et sur les autres hôtes.

### 28.6 `tcg/0023` : `mftb` sans `div`

Sous `x-tb-fast`, `tbf_load()` divisait les ns par `tbf_div`, une variable : `div r64`, 35 à 88
cycles sur ce cœur. À 25 MHz (mac99), `tbf_div` vaut 40 : `ns / 40` est une multiplication et un
décalage (`mul`, `shr $5` dans l'objet compilé), même quotient pour tout `uint64_t`. Banc hôte
(`rdtsc` + produit 128 bits + division, 20 M lectures) : 8,83 → 5,53 ns, le `rdtsc` seul en coûte
5,51. Pas de propriété nouvelle (identité arithmétique, sous `x-tb-fast`) : `x-tb-verify` compare
toujours chaque lecture à la valeur d'origine ; l'A/B se joue binaire contre binaire.

### 28.7 Ce qui est prouvé, ce qui reste à prouver

Fait sans VM (un cœur, `taskset -c 15 nice -n 19`) :

| épreuve | résultat |
|---|---|
| `tools/tcg/vmxproof.sh ~/src/qemu-d3tcg 200000 mut` : helpers `vsldoi`, `vmrg[hl]w`, `STVE*` extraits tels quels contre le modèle op par op (le script vérifie que l'arbre émet bien ces 19 ops) | vsldoi 6 400 000 cas (16 décalages, recouvrements), vmrg 1 200 000, stve 9 600 000 (16 adresses × 3 tailles) : **0 divergence** ; **6 mutants sur 6 détectés** |
| `VFPPROOF_NATIVE=1 VFPPROOF_BUILT=~/src/qemu11 tools/tcg/vfpproof.sh ~/src/qemu-d3tcg 20000` : modèle instruction pour instruction de l'émetteur x86, porte extraite de `cpu_init.c`, contre la boucle softfloat du vrai `fpu_softfloat.c.o`, 8 états | 9 094 144 vecteurs (36,4 M voies), 905 792 par le chemin court : **0 divergence** ; **8 mutants sur 8 détectés** (borne d'opérande, b non testé, `vfmadd` pour `vnmsubfp`, exemption sur b au lieu de c, `FLT_MIN` admis, infini admis, signe non inversé, seconde fenêtre de la porte oubliée) |
| compilation isolée des fichiers touchés (`ppc`, `ppc64`, `libsystem`) | sans erreur ni avertissement ; aarch64 non compilé |

Ce sont des preuves du **modèle** ; l'émetteur réel (encodages, registres) ne sera prouvé qu'en
VM. À faire au feu vert, dans l'ordre :

1. **Construire** `~/src/qemu-d3tcg` (`configure` comme la référence) ; `-d out_asm` sur un
   démarrage court pour relire le code de `ppc_vfp` et les appels directs de `x-jit-rel32` ;
   `vfpproof.sh` complet (`N = 20 000 000`, comme §26.2) avec `VFPPROOF_NATIVE=1`.
2. **VM de dev** (`devloop.py`, 10.4.11) : `vfptest` (NJ 0/1, catalogue 40³, 2^22 vecteurs) et
   `vmxtest` (nouveau : `vsldoi` aux 16 décalages, `vmrg[hl]w`, recouvrements de registres par
   `asm`, `stve[bhw]x` aux 16 adresses, fautes sur une page en lecture seule) : **empreintes
   identiques** propriétés éteintes, allumées, allumées avec vérificateurs ; bilans `vmx-verify`
   et `vfp-native-verify` à 0 divergence ; `tbtest` sous `x-tb-verify` avec 0023.
3. **Mutants de l'émetteur x86** (à la manière de `fpnat-mut.sh`) : borne d'opérande, infini
   admis, `vfmadd231ps` pour `vnmsubfp`, seconde fenêtre de porte sautée ; pour 0021 un décalage
   de `stve` faux et un `extract2` décalé : chacun doit changer l'empreinte de `vfptest`/`vmxtest`
   et faire diverger le vérificateur.
4. **Partie DOOM 3 vérifiée** (`VMXINLINE=1 VFPNATIVE=1 VMXVERIFY=1 VFPNVERIFY=1 TBVERIFY=1`,
   VM quotidienne) : des centaines de millions d'opérations, 0 divergence, part du chemin court.
5. **Bancs** : `vfptest banc` (1 492 ms aujourd'hui, attendu ~500-700), `vmxtest banc`, `tbtest`.
6. **A/B DOOM 3** (`tools/tcg/matab.sh`, VM quotidienne, hôte au repos, parties entrelacées sur
   un QEMU relancé), un poste à la fois puis l'ensemble, binaire `qemu-d3tcg` des deux côtés :

        QEMU_BIN=~/src/qemu-d3tcg/build/qemu-system-ppc tools/tcg/matab.sh d3-rel32 6 \
            "r0:" "r1:JITREL32=1" d3 fen
        # de même "v0:" "v1:VFPNATIVE=1", "m0:" "m1:VMXINLINE=1",
        # puis "t0:" "t1:VMXINLINE=1 VFPNATIVE=1 JITREL32=1"

   0023 se lit au banc `tbtest` et binaire contre binaire (référence contre `qemu-d3tcg`, les trois
   propriétés éteintes). Puis la matrice du PC (mb, zen, ut, d3) sur la configuration retenue,
   avant toute mise par défaut. L'ordre des A/B suivra le profil x86 de DOOM 3.
7. **Parité** : sur le M4, compiler l'émetteur aarch64 de 0022 et refaire 2-5 avec des mutants
   NEON ; 0021 et 0023 y valent tels quels ; 0024 n'y a pas d'objet.

## 29. Validation du JIT sur M4 (05/10/2026)

L’émetteur NEON de 0022 est maintenant construit et vérifié sur QEMU 11.1.2,
avec correction du helper lent aarch64 (0028) et du zéro signé `vnmsubfp`.
Preuves hôte, huit mutants du modèle, instructions et invalidations en SMP=1/2 :
zéro divergence. Microbanc flottant : 1 336 → 362 ms (−73 %), sans mesure en jeu.
Les variantes de copie `lmw/stmw` (0025) et de hachage du cache (0026) n’apportent
aucun gain mesuré ; le lanceur les allume néanmoins sur macOS arm64 avec
`VFPNATIVE`, à la demande de l’utilisateur. 0027 corrige le placement RX sous
`split-wx` ; `hotblocks`/`jitblocks` permettent d’inspecter les blocs ARM émis.
Méthodes, limites et résultats : [rapport M4](jit-m4-2026-10-05.md).

---

## 33. Le flottant scalaire restant sur le PC : comparaisons et conversions natives (06/10/2026)

PC Linux x86-64 (i7-10700F). Copie isolée `~/src/qemu-fp` (= `~/src/qemu-d3tcg`, c'est-à-dire la
référence + 0021-0024, plus 0025-0028 posés par `build_qemu_qfb.sh`), binaire
`~/src/qemu-fp/build/qemu-system-ppc{,64}` ; l'arbre de référence `~/src/qemu` n'a pas été touché.
Patch `patches/tcg/0033-ppc-fp-native-cmp.patch`, propriétés `x-fp-native-cmp` et
`x-fp-native-cmp-verify` (éteintes par défaut), lanceur `FPNATIVECMP=1` / `FPNCMPVERIFY=1`.

### 33.1 Inventaire : ce qui passait encore par softfloat

Point de départ : « softfloat 3,3 % du temps vCPU » de DOOM 3 sur le PC
(`docs/vitesse-doom3-x86.md` §4), avec `x-fast-fp`, `x-fp-native` et `x-fp-native64` allumés.
Les trois relevés `perf` de la campagne x86 (DOOM 3 `bench/vitesse/d3-x86/profil-1/d3-fen`,
Marble Blast `bench/tcg/ab/x86-prof-mb/p1-1/mb-fen`, UT2004 `bench/tcg/ab/x86-prof-ut/p1-1/ut-fen`),
relus fonction par fonction (`perf report --sort sym`, fils `CPU n/TCG` seulement, parts du
temps vCPU), rangés par instruction d'origine :

| poste (part du temps vCPU) | DOOM 3 | Marble Blast | UT2004 |
|---|---|---|---|
| `frsp` (`helper_FRSP`, `float64_to_float32`, `helper_todouble`) | 0,32 % | 0,16 % | 0,12 % |
| `fctiw`/`fctiwz` (`helper_FCTIWZ`, `float64_to_int32*`) | 0,10 % | 0,12 % | 0,09 % |
| `fdivs`/`fdiv` (`helper_FDIV*`, `float64r32_div`, `float64_div`) | 0,14 % | 0,10 % | 0,06 % |
| `fsel` (`helper_FSEL`, pur) | 0,03 % | 0 | 0,06 % |
| `fcmpo` | 0,01 % | 0 | 0,01 % |
| FPRF et contrôle des helpers restants (`helper_compute_fprf_float64`, `helper_fprf_check_float64`, `do_float_check_status`) | 0,40 % | 0,41 % | 0,31 % |
| `float64_unpack_canonical` (frsp, fctiw, fdiv) | 0,39 % | 0,30 % | 0,22 % |
| `parts64_*` partagés (canonicalize, uncanon, float_to_sint, round_to_int, scalbn…) | 0,81 % | 0,55 % | 1,42 % |
| replis du scalaire déjà natif (`do_fcmpu`, talon, `fp32_flat`) | 0,19 % | 0,04 % | 0,14 % |
| `fsqrt`, `fres`, `frsqrte` | 0,02 % | 0,03 % | 0,03 % |
| **AltiVec** (`float32_compare_quiet` de `vcmpgtfp`/`vcmpgefp`, `vcfsx` = `int32_to_float32` + `float32_scalbn`, `float32_muladd` des replis de `vmaddfp`, `vmaxfp`, `vrefp`, `vrsqrtefp`) | **1,22 %** | 0,23 % | **1,36 %** |
| total flottant hors code généré | 3,63 % | 1,94 % | 3,82 % |

Lecture :

- **`fcmpu` n'y est plus** : il passe par l'op native depuis 0014/0017 (§22.5, §25.1) ; seuls
  ses replis (NaN, porte fermée) restent. Le `float32_compare_quiet` que le §4 citait en tête
  vient de **`vcmpgtfp`/`vcmpgefp` (AltiVec)**, pas du scalaire (la cible n'emploie
  `float32_compare*` que dans les comparaisons vectorielles) ; même chose pour la grosse part
  `parts64_scalbn`/`int32_to_float32` d'UT2004 (`vcfsx`). Hors du périmètre de ce patch.
- **Le scalaire qui restait en helpers** : `frsp` en tête, puis `fctiwz`, `fdivs`/`fdiv`, et
  leur FPRF/contrôle (deux appels de helper par instruction, `fastfp/0002`). Avec la part de
  `float64_unpack_canonical` et des `parts64_*` qui leur revient, **~1,5 % du temps vCPU sur
  DOOM 3, ~1,3 % sur Marble Blast, ~0,8 % sur UT2004**.
- `fsel` est déjà un helper pur (`NO_RWG_SE`) : rien à gagner (vérifié au banc, §33.5) ; `fcmpo`
  est absent des jeux (gcc émet `fcmpu`). Les deux sont faits quand même : pour `fcmpo` c'est la
  même op que `fcmpu`, pour `fsel` trois `movcond`.
- `fabs`, `fneg`, `fnabs`, `fmr`, `lfd`/`stfd` sont déjà des ops entières de TCG ; `lfs`/`stfs`
  aussi (0002). `fsqrt`/`fres`/`frsqrte` pèsent 0,02-0,03 % : laissés aux helpers.
- **Fréquences**. Le profil d'instructions du M4 (§15.1, 74 ms/image) donnait `frsp` à 0,7 M/s,
  soit ~50 000 par image ; même travail par image ici, à 138 ms/image : **~0,35 M `frsp`/s**
  sur le PC. Statiquement, le moteur de DOOM 3 contient 2 041 `frsp`, 546 `fctiwz`, 610 `fdivs`
  (§28.1). Le coût mesuré d'un `frsp` par les helpers est ~25 ns (banc, §33.5), ce qui redonne
  la part du profil (0,35 M/s × 25 ns ≈ 0,9 % d'un vCPU occupé à ~110 %). Les compteurs du
  vérificateur (`x-fp-native-cmp-verify`, une ligne par opération : vérifiés / par le chemin
  court / divergences) donnent les nombres exacts : sur Marble Blast dans la VM de dev,
  **`frsp` 0,22 M/s, `fctiwz` 0,20 M/s, `fctiw` 0,10 M/s, `fdivs` 31 000/s** (§33.4) ; DOOM 3 à
  relever pendant la partie vérifiée de la phase 2 (§33.7).

### 33.2 Conception

Même mécanique que `x-fp-native` (§22.5) : **mêmes porte, op, talon, vérificateur**, six
sélecteurs de plus pour `INDEX_op_ppc_fp32` (`internal.h`, `FPI_FRSP = 15` … `FPI_DDIV = 20`,
vérifiés à la compilation contre ceux de l'émetteur) :

| instruction | chemin court (porte : XX = 1, XE = OE = UE = 0, et RN = 00 sauf `fcmpo`/`fctiwz`) | sinon |
|---|---|---|
| `frsp` | frB nul, ou FLT_MIN ≤ \|frB\| < FLT_MAX + ½ ulp : `vcvtsd2ss` + `vcvtss2sd` ; FPRF du résultat, FI | talon |
| `fctiw`, `fctiwz` | frB double nul ou normal (`fpi_zon64`), `vcvtsd2si`/`vcvttsd2si` **64 bits** dont le résultat doit tenir dans un int32 (`movslq` + `cmp`) : c'est alors l'int32 étendu en signe que rendent les helpers ; FI ; FPRF intact | talon (NaN, infini, dénormal, VXCVI, saturation) |
| `fcmpo` | aucun NaN : exactement `fcmpu` (même code émis) | talon : `do_fcmpo` (VXVC, VXSNAN) |
| `fdivs`, `fdiv` | opérandes nuls ou normaux (`fpi_zon`/`fpi_zon64`), diviseur non nul, quotient nul (dividende nul) ou FLT_MIN (DBL_MIN) < \|q\| < ∞ : `vdivss`/`vdivsd` ; FPRF, FI | talon (ZX, VXZDZ, VXIDI, OX, UX, dénormaux) |
| `fsel` | trois `movcond` (pas de FPSCR) | — |

Pourquoi c'est exact (modèle C `fpi_frsp()`, `fpi_fcti()`, `fpi_fp32/64(FPI_DIVS)` du bloc
« fp-inline » de `fpu_helper.c`, extrait tel quel par `fpproof.sh`) :

- **`frsp`** : avec \|x\| ≥ FLT_MIN le simple arrondi est normal, la petitesse (testée avant
  l'arrondi sur PowerPC) n'est pas atteinte ; sous FLT_MAX + ½ ulp il est fini, pas de
  débordement (au-delà, l'arrondi au pair va à 2^128 : OX, au talon). Le FPSCR amorcé ne laisse
  alors que l'inexact (déjà posé) : FPRF du résultat, FI = 1, comme la séquence d'origine.
- **`fctiw(z)`** : le test « tient dans un int32 » sur la conversion 64 bits couvre aussi les
  bords (−2^31 − 0,5 arrondi vers zéro tient ; 2^31 − 0,5 arrondi au pair ne tient pas) ;
  NaN, infinis et \|x\| ≥ 2^63 donnent 2^63, jamais un int32. Les dénormaux vont au talon
  (softfloat y lève `input_denormal_used`).
- **`fdivs`** : sur des simples exacts, `float64r32_div` est la division simple correctement
  arrondie, ce que fait `vdivss` (le même argument que `x-fast-fp` pour son chemin hardfloat).
- MXCSR : arrondi au plus proche, ni DAZ ni FTZ (QEMU ne le touche pas ; l'émetteur 0017 le
  suppose déjà).

Traduction (`translate/fp-impl.c.inc`) : `do_fpnc()` (frB, frA pour la division) appelle
`gen_fp_native()` ; `fcmpo` réutilise `gen_fcmpu_native(…, FPI_CMPO)`. Chemin lent :
`fp32_flat()`/`fcmpu_flat()` font la séquence d'origine (`do_frsp`, `float64_to_int32*` +
`float_invalid_cvt`, `float64r32_div`/`float64_div` + `div_flags_handler`, `do_fcmpo`) avec
l'adresse de retour du code généré, frT écrit avant le contrôle, FPRF sauf pour `fctiw(z)`.
`helper_FCMPO` est réécrit en `do_fcmpo(…, GETPC())` sans changement de comportement.

**Porte de l'hôte** : `TCG_TARGET_HAS_ppc_fp_cmp` (`tcg/tcg-has.h`, 0 par défaut ; x86_64 :
`TCG_TARGET_HAS_ppc_fp32`, AVX + FMA3), `tcg_ppc_fp_cmp_supported()`. **Sur arm64 la propriété
ne change rien** (ni `fsel`) : l'émetteur aarch64 ne connaît pas ces sélecteurs, et aucune
chaîne aarch64 n'est disponible sur le PC pour en écrire un prouvé. Pendant NEON à faire sur le
M4 (`fcvt s,d`/`fcvt d,s`, `fcvtzs`/`fcvtns` vers x, `fdiv`, même test de plage int32),
prouvé là-bas par `fptest c` dans Tiger (empreinte de référence ci-dessous) et
`x-fp-native-cmp-verify` — `fpnatcmp-user.sh` demande linux-user, absent de macOS.

Vérificateur : `helper_fpn_verify` (§22.5) connaît les nouveaux sélecteurs (`fpi_short()`,
`fpi_gate_op()`, `do_fcmpo`, pas de FPRF pour `fctiw(z)`) ; `fsel` a le sien
(`helper_fpnc_fsel`, contre `helper_FSEL`). Il s'allume par `x-fp-native-cmp-verify` (pas
`x-fp-verify`) et écrit sur la même ligne `fp-native-verify:` (une colonne par opération,
`frsp` à `fsel`).

### 33.3 La preuve hôte

**Modèle et talon contre le vrai softfloat** (`tools/tcg/fpproof.sh ~/src/qemu-fp 20000000`,
journal `logs/fpproof-20M.log` du §33.8) : `fpproof.c` était resté à 9.2 (ni formes double, ni
objets de QEMU 11 : `fpu_softfloat.c.o` est dans `libcommon`, `helper_FCMPU`, motif du NaN par
défaut) ; remis à jour, puis étendu (`-DFPPROOF_NCMP`) : catalogue croisé (bords de `frsp` et de
`fctiw` ajoutés : FLT_MAX, FLT_MAX + ½ ulp et ses voisins, sous FLT_MIN, 2^-149, ±2^31 ± ¼, ½, 1,
2^63, 0,5, 2,5), puis aléatoire (entiers, demi- et quarts d'entier jusqu'à 2^33, voisins de
±2^31 et des bornes de `frsp`, diviseurs à mantisse courte), FPSCR en trois familles (porte
ouverte, au hasard, fermée d'un bit), MSR[FE] au hasard. Résultat, frT en mémoire, FPSCR entier,
drapeaux softfloat (sauf `input_denormal_used`, §25.1), exception levée et `exception_index` :

| `fpproof.sh … 20000000` | vecteurs | par le chemin court | divergences |
|---|---|---|---|
| tout (simple, double, 0033) | 461 749 260 | 297 198 078 | **0** |
| dont `frsp` / `fctiw` / `fctiwz` | 20 000 420 chacun | 14,8 M / 11,6 M / 12,6 M | **0** |
| dont `fcmpo` / `fdivs` / `fdiv` | 20 058 800 chacun | 17,6 M / 4,3 M / 16,8 M | **0** |
| talon `ppc_fp32_native_slow`, tous chemins | 461 749 260 (16,8 M exceptions levées) | — | **0** |
| `x-fp-flat`, tous chemins (ops d'origine) | 341 571 600 | — | **0** |

Contre-épreuves du modèle (`fpproof-mut.sh`, 200 000 vecteurs) : les 10 mutants d'origine et 7
nouveaux (borne basse ou haute de `frsp`, `fctiw` tronqué, 2^31 accepté, FI oublié, diviseur nul
accepté, RN ignoré) : **17 sur 17 détectés** (9 à 650 491 divergences).

**L'émetteur réel** : `tools/tcg/fpnatcmp-user.sh` construit le job `fptest` pour
`powerpc-linux-gnu` et le fait tourner sous un `qemu-ppc` **linux-user** construit depuis une
copie de l'arbre patché (`~/src/qemu-fpu`) : même traducteur, même op, même émetteur x86_64,
même talon que le binaire système. Trois choses y sont neutralisées, dans la copie seulement
(`tools/tcg/fpnatcmp-userhack.py`) : le code « système seulement » de 0008 (`x-ret-inline`, jamais allumé
ici) et des champs de 0001/0021-0025 rangés sous `!CONFIG_USER_ONLY`, et
**MSR[FE0] = MSR[FE1] = 0 comme sous Tiger** (linux-user les pose et force
`fp_exceptions_enabled()`). Le mode `d` y redonne l'empreinte de Tiger (`fb3e6006e03c4f53`,
§25.4) : le banc linux-user exécute bien les mêmes instructions de la même façon.

| `fpnatcmp-user.sh qemu-ppc 65536` | instructions | référence contre `x-fp-native-cmp` | sous `x-fp-native-cmp-verify` |
|---|---|---|---|
| `fptest c` (frsp fctiw fctiwz fcmpo fdivs fdiv fsel, 11 états du FPSCR) | 6 206 354 | **identiques** (`e6bba64145ea9bbf`) | identique ; 8 060 929 vérifiées (2 841 840 par le chemin court), **0 divergence** |
| `fptest` (simple) | 12 823 376 | identiques (`c2dffc9e43a020d8`) | identique ; `frsp` du programme : 0 divergence |
| `fptest d` | 11 978 884 | identiques (`fb3e6006e03c4f53`) | identique |

Contre-épreuve de l'émetteur (`tools/tcg/fpnatcmp-mut.sh`, un `qemu-ppc` par mutation,
`fptest c 16384`) : **9 mutants sur 9 détectés**, chacun par une sortie différente **et** par
le vérificateur — borne basse de `frsp` retirée (3 271 divergences), borne haute à 2^128
(1 817), `fctiw` tronqué (7 385), test de plage int32 retiré (39 779), FI oublié par `fctiw(z)`
(80 782), diviseur nul accepté (18), quotient non testé (7 976), NaN de frB accepté par
`fcmpo` (12 009), NaN oublié par `fsel` (13 990).

**Patch** : 0033 posé (`patch --fuzz=0`) sur l'instantané des douze fichiers d'avant redonne
l'arbre de travail à l'octet ; `build_qemu_qfb.sh` le pose (section « tcg/0033 », marqueurs) et
sonde la propriété (`check_opt x-fp-native-cmp`).

### 33.4 La preuve invitée

VM de dev : copie privée `disks/tiger-dev-fp.raw` de `tiger-dev.raw` (10.4.11, supprimée
ensuite), `devloop.py` depuis le worktree, **toutes les options de production** de
`run_tiger.sh` (`x-fast-fp x-sr-tlb x-lfs-inline x-vfp-fast x-vperm-fast x-fp-inline
x-ret-inline x-jc-idx x-icbi-sync x-msr-nobql x-fp-native x-tb-fast x-fp-native64
x-vmx-inline x-vfp-native`, `x-jit-near`, `x-jc-bits=14`), plus ou moins `x-fp-native-cmp`.
Le job `fptest` (`MODE=c`, 2^18 vecteurs aléatoires par état et opération) est compilé dans
Tiger par gcc 4.0 :

| binaire, mode | `fptest c` (21 345 170 instr.) | `fptest` simple (30 124 880) | `fptest d 65536` | vérificateur |
|---|---|---|---|---|
| ppc64, SMP=1, sans 0033 | `67cf96efa75f9286` | `e80ec8026301ef1d` | — | — |
| ppc64, SMP=1, `x-fp-native-cmp` + vérif. | `67cf96efa75f9286` | `e80ec8026301ef1d` | `fb3e6006e03c4f53` | 34 777 776 opérations vérifiées (12 011 755 par le chemin court), **0 divergence** |
| ppc64, SMP=2, `x-fp-native-cmp` + vérif. | `67cf96efa75f9286` | `e80ec8026301ef1d` | — | 34 744 722 vérifiées, **0 divergence** |
| ppc (32 bits), SMP=1, `x-fp-native-cmp` + vérif. | `67cf96efa75f9286` | — | — | 28 944 953 vérifiées, **0 divergence** |

**Identiques à l'octet** avec et sans la propriété, en SMP=1 et SMP=2 ; les empreintes simple
et double sont celles du §25.4 (inchangées depuis 0017), et celle du mode c est **la même que
sous `qemu-ppc` linux-user** (`fpnatcmp-user.sh … 262144`, `logs/user-262144.log`) : le banc
hôte du §33.3 voit exactement ce que voit Tiger.

**Bureau et jeu sous le vérificateur** (ppc64, SMP=2, `start --gui`) :

| | vérifiées | par le chemin court | divergences | détail (vérifiées / chemin court) |
|---|---|---|---|---|
| démarrage de Tiger jusqu'au bureau, puis arrêt | 359 007 | 313 974 | **0** | `frsp` 92 011 / 92 009, `fctiw` 44 377 / 52, `fctiwz` 181 237 / 180 757, `fdiv` 38 681, `fdivs` 2 697 |
| bureau + **Marble Blast Gold, 120 s** (`fpgames`, `GAMES=mb DUR=120`) | **70 271 934** | 67 037 278 (95,4 %) | **0** | `frsp` 26 851 765 / 26 843 032, `fctiwz` 24 564 589 / 24 564 036, `fctiw` 12 123 084 / 8 899 614, `fdivs` 3 728 571, `fdiv` 1 666 799, `fsel` 1 337 078, `fcmpo` 48 |

Soit, en jeu (Marble Blast dans la VM de dev, ~37 img/s) : **`frsp` ~0,22 M/s, `fctiwz` ~0,20 M/s,
`fctiw` ~0,10 M/s** (73 % par le chemin court : le reste sont des valeurs hors int32 ou un FPSCR
non amorcé), `fdivs` ~31 000/s, `fdiv` ~14 000/s, `fsel` ~11 000/s, `fcmpo` ~0. Ce sont les
fréquences de l'inventaire (§33.1) mesurées, et non plus estimées.

### 33.5 Gains

Banc hôte (`fptest banc-c 20000000` sous le `qemu-ppc` du §33.3, trois tours entrelacés,
médianes ; hôte peu chargé) :

| boucle (20 M itérations) | helpers | `x-fp-native-cmp` | écart | par instruction |
|---|---|---|---|---|
| `frsp` (débit) | 514 ms | **156 ms** | **−70 %** | −18 ns |
| `fctiwz` + `stfd` + `lwz` | 450 ms | **101 ms** | **−78 %** | −17 ns |
| `fdivs` (chaîne dépendante) | 277 ms | **208 ms** | −25 % | −3,5 ns |
| `fcmpo` + branchement | 402 ms | **265 ms** | −34 % | −7 ns |
| `fsel` | 79 ms | 78 ms | 0 | — (helper pur) |

Banc invité (`fptest banc-c 10000000` dans Tiger, ppc64 SMP=2, options de production,
trois passes par démarrage, deux démarrages par mode entrelacés ref/cmp/ref/cmp ; même ordre
de grandeur que le banc hôte) :

| boucle (10 M itérations) | helpers | `x-fp-native-cmp` | écart |
|---|---|---|---|
| `frsp` | 348-354 ms | **152-153 ms** | **−57 %** |
| `fctiwz` + `stfd` + `lwz` | 250-253 ms | **70 ms** | **−72 %** |
| `fdivs` (chaîne) | 159-161 ms | **115-117 ms** | −28 % |
| `fcmpo` + branchement | 238-239 ms | **163 ms** | −32 % |
| `fsel` | 141-144 ms | 136-140 ms | ~0 |
| témoin `fptest banc` (chaîne simple, sommets, `fcmpu`) | 202 / 1 247-1 265 / 164-168 ms | 209-212 / 1 278-1 300 / 169-171 ms | +2-3 % (bruit, non touchés) |

**Attendu en jeu** : le poste visé pèse ~1,5 % du temps vCPU de DOOM 3 (§33.1) et l'op native
en retire ~75 % : **~1 % du temps vCPU, soit ~1-1,5 ms/image sur 138** (moins sur Marble Blast et
UT2004, ~0,5-1 %). C'est sous la dispersion d'une matrice à trois parties : il faut six
parties par bras pour le voir.

### 33.6 Ce qui reste en helper, et pourquoi

- **L'AltiVec softfloat** (1,2 % sur DOOM 3, 1,4 % sur UT2004) : `vcmpgtfp`/`vcmpgefp`
  (`float32_compare_quiet`), `vcfsx`/`vctsxs` (`int32_to_float32` + `scalbn`), replis de
  `vmaddfp`, `vmaxfp`/`vminfp`, `vrefp`/`vrsqrtefp`. **Le plus gros poste restant**, hors de ce
  patch (AltiVec) : `vcmp*fp` et `vcfsx` en ops vectorielles seraient la suite logique.
- `fsqrt`, `fsqrts`, `fres`, `frsqrte` (0,02-0,03 %) : rares (la libm prend `fsqrt` avec le
  kext `POMPPCFsqrt` depuis le 01/10 ; 8 `frsqrte` dans DOOM 3).
- `fctiw` en arrondi autre qu'au plus proche, et tout opérande ou résultat hors des conditions
  du §33.2 : le talon, exact par construction.
- `mffs`, `mtfsf`, `mtfsb0/1`, `mcrfs` : rares, inchangés.

### 33.7 Phase 2 : l'A/B en jeu (VM quotidienne, hôte au repos)

Depuis le dépôt principal (après fusion, `FPNATIVECMP` existe dans `run_tiger.sh`), binaire
`~/src/qemu-fp/build/qemu-system-ppc` (qemu-bundle à côté, dans `build/`) :

    tools/tcg/matab.sh x86-fpcmp 6 "ref:QEMU_BIN=$HOME/src/qemu-fp/build/qemu-system-ppc" \
        "fp:QEMU_BIN=$HOME/src/qemu-fp/build/qemu-system-ppc FPNATIVECMP=1" d3 fen
    # une partie vérifiée (pas pour la vitesse) : bilan « fp-native-verify » du run_tiger.log,
    # colonnes frsp..fsel = les fréquences exactes de la partie, 0 divergence attendu
    tools/tcg/matab.sh x86-fpcmp-verif 1 \
        "v:QEMU_BIN=$HOME/src/qemu-fp/build/qemu-system-ppc FPNATIVECMP=1 FPNCMPVERIFY=1" d3 fen
    tools/tcg/matab.sh --restore

Même binaire des deux côtés : seul `x-fp-native-cmp` change. Attendu : DOOM 3 −0,7 à −1,5 %
(138 → ~136-137 ms/image) ; Marble Blast, UT2004 (`mb,ut fen`) dans le bruit.

### 33.8 Chemins

- Patch : `patches/tcg/0033-ppc-fp-native-cmp.patch` ; arbre `~/src/qemu-fp` (binaire
  `build/`, binaire d'avant 0033 dans `bin/base/`), copie linux-user `~/src/qemu-fpu`
  (`build-user/qemu-ppc`).
- Outils : `tools/tcg/fpproof.{c,sh}`, `fpproof-mut.sh`, `fpnatcmp-user.sh`,
  `fpnatcmp-mut.sh` ; job `tools/guest/jobs/fptest` (`MODE=c`, `BANCMODE=banc-c`).
- Journaux (non versionnés) : `bench/tcg/fpcmp/` du dépôt principal — `logs/fpproof-20M.log`,
  `logs/mut.log` (mutants de l'émetteur), `logs/user-262144.log` et `u2/` (banc linux-user),
  `logs/bench-user.txt`, `guest/` (journaux `devloop` et `qemu.log` de chaque démarrage :
  `v1-*`, `v2-*`, `b2*`, `desk-qemu.log`, `mb2-qemu.log`), `prof-*.txt` et `cats.py` (relevés
  `perf` du §33.1), `seq.sh`/`vm.sh`/`start2.py` (la campagne invitée).
- Banc linux-user : `tools/tcg/fpnatcmp-userhack.py` (la copie `~/src/qemu-fpu`).
