# Le G4 sur la cible 32 bits en MTTCG (`tcg/0049`, `tcg/0050`, 23/10/2026)

PC Linux x86-64 (i7-10700F), QEMU 11.1.2. Objectif de fond : DOOM 3 à 50 ms/image (~92
aujourd'hui), où le code généré fait 84 % du temps vCPU (`docs/vitesse-doom3-x86.md` §14). Ce
levier ne touche à aucun émetteur : il fait exécuter au G4 le code que QEMU génère pour un
processeur **32 bits**, au lieu de celui du 64 bits, et il retire au binaire 64 bits un compteur
de PMU qui n'existe pas sur un 7447.

**Tout est éteint par défaut.** `PPC32MT=1` et `PMUREAL=1` dans `run_tiger.sh` ; rien ne change
tant qu'on ne les pose pas. Rien n'est fusionné : branche du worktree `agent-af8215e75c4376073`.

## 1. Constat

Avec SMP ≥ 2, `run_tiger.sh` lance `qemu-system-ppc64`, **seulement** parce que QEMU ne déclare
MTTCG que pour la cible 64 bits. Le G4 est un processeur 32 bits : la cible 64 bits l'émule
« en mode étroit », et le paie.

Mesure statique de la session principale (OpenBIOS seul, 2 811 blocs communs) : **ppc64 émet
11,4 % d'instructions x86 de plus** (12,11 contre 10,87 par instruction invitée). Causes :

- `ext32u` sur chaque adresse effective (`NARROW_MODE`) ;
- `movslq` et comparaisons 64 bits pour `cmpi` (64 octets contre 52) ;
- à **chaque sortie de bloc**, `PMC5 += num_insns` en mémoire (`env+0x3bd8`,
  `pmu_count_insns`, `target/ppc/translate.c`) : `pmu_update_summaries`
  (`target/ppc/power8-pmu.c`) lit `SPR_POWER_MMCR0`, qu'un 7447 n'a pas (il vaut donc 0 et
  non sa valeur de reset `MMCR0[FC]`), conclut « FC56 = 0, PMC5 compte » et pose
  `HFLAGS_INSN_CNT`. `bc` passe de 84 à 125 octets.

Refait ici sur les binaires de ce travail (`~/src/qemu-ppc32mt/build-mt`, OpenBIOS 10 s,
options de production ; commande au §3.1) : 2 963 blocs communs, 20 243 instructions invitées :

| binaire | instructions x86 / instruction invitée | écart |
|---|---|---|
| `qemu-system-ppc` | **9,81** | référence |
| `qemu-system-ppc64` + `x-pmu-real` | 10,53 | +7,3 % |
| `qemu-system-ppc64` (tel quel) | 10,87 | +10,8 % |

## 2. Phase 0 : le plafond, mesuré avant de construire

VM de dev (`disks/tiger-dev-ppc32.raw`, copie de `tiger-dev-plugin.raw`, 10.4.11,
single-user), SMP=1, **toutes les options de production** de `run_tiger.sh` (Linux x86-64 :
`x-fast-fp` … `x-pteg-map`, `x-jit-near`, `x-jit-rel32`, `x-jc-bits=14`, `x-jc-word`,
`x-jc-2way`), même binaire de base des deux côtés (`~/src/qemu-ppc32mt/build-base`, série du
21/10 inchangée, `-O3 -march=native` sans LTO). Pilote : `tools/tcg/ppc32mt-bench.sh` ;
`perf stat -e instructions:u,cycles:u -p <qemu>` pendant chaque job (processus entier,
`perf_event_paranoid=1`). Deux démarrages par binaire, ordre ppc, ppc64, ppc64, ppc.

Bancs :

- **`mtbench`** (nouveau job, `tools/guest/jobs/mtbench/`) : `regbench` un tour (noyaux `call`,
  `mem`, `fp`, `sys`, `ctx`, `copy`), 4 `gcc -O2 -faltivec -c`, `vfptest 10000000`,
  `lmwtest 5000000`, deux `regbench` simultanés (`par2`) ; trois passes, médianes ; puis
  `atomtest` (§4.2) ;
- **`t2bench`** : gltest `d3inter` (plugin + GLEngine + kext), 150 images, 3 passes
  (`docs/plan-traducteur-rapide.md` §9.3).

| SMP=1 | ppc (2 démarrages) | ppc64 (2) | écart ppc64 |
|---|---|---|---|
| `mtbench`, somme des noyaux (ms) | 47 935 | 51 542 | **+7,5 %** |
| … `call` / `sys` / `ctx` / `gcc` / `vfp` | 423 / 2 314 / 3 545 / 4 140 / 10 680 | 456 / 2 575 / 3 992 / 4 505 / 11 252 | +7,9 / +11,3 / +12,6 / +8,8 / +5,4 % |
| `mtbench`, instructions hôte | 1 763 G | 1 880 G | **+6,6 %** |
| `mtbench`, cycles hôte | 757 G | 812 G | +7,2 % |
| `t2bench`, instructions hôte | 730 G | 783 G | **+7,3 %** |
| `t2bench`, cycles hôte | 346 G | 370 G | +6,6 % |
| `t2bench`, temps (médiane des passes) | 7,01 / 7,87 s | 7,85 / 7,80 s | +5,2 % (bruité) |

Images de `t2bench` identiques partout (`7ff741d6…`). Le temps de `t2bench` du second
démarrage ppc (7,87 s) est dans le régime lent bien connu des démarrages (le tampon du JIT
tombé dans une « AUTRE fenêtre de 4 Gio », `docs/tcg-g4.md` §14) ; ses instructions et ses
cycles, eux, ne bougent pas (730,6 G, 352 G).

**Porte : l'écart dépasse 3 % de loin (6,6-7,5 %). On construit.** Le chiffre statique
d'OpenBIOS (+11 %) se dilue en dynamique dans les helpers, la MMU et le noyau, qui coûtent
pareil des deux côtés.

## 3. Conception

### 3.1 `tcg/0049` : `x-pmu-real` (propriété de CPU, éteinte par défaut)

Le modèle de PMU POWER8 (`power8-pmu.c`) ne compte que si le CPU a les registres du PMU
Book3S : test `env->spr_cb[SPR_POWER_MMCR0].name` dans `pmu_update_summaries` (même idiome que
`env->spr_cb[SPR_LPCR].name` dans `cpu_init.c`). Pourquoi pas `insns_flags2 & PPC2_ISA207S` :
le 970, le POWER5+ et le POWER7 ont eux aussi `MMCR0`/`PMC5` (registrés par
`init_proc_book3s_common`, reset `MMCR0[FC]`) ; ISA207S leur aurait retiré un comptage visible.
Le test retenu ne change **rien de visible** pour aucun CPU : sans `MMCR0` Book3S, le `PMC5`
compté (SPR 0x317) n'est pas lisible par l'invité (le 7447 a ses propres `MMCR0`/`PMC1-6`, SPR
952-959, simples registres sous QEMU). `HFLAGS_INSN_CNT` n'est plus posé, `pmu_count_insns`
n'émet plus rien.

- Sans effet sur `qemu-system-ppc` (tout le code PMU y est sous `TARGET_PPC64`).
- Propriété (`x-pmu-real=on`) plutôt que changement inconditionnel : A/B avec le même binaire
  sur la VM quotidienne, et la règle du dépôt (éteint par défaut). Lanceur : `PMUREAL=1`,
  sondé comme `DIRTYSKIP`.
- Preuve que le code des blocs perd le compteur (OpenBIOS seul, 10 s, même binaire
  `build-mt/qemu-system-ppc64`, options de production, `-d in_asm,out_asm`, puis
  `tools/tcg/pmc5count.py` : motif `mov env+OFF,%r ; add $n,%r ; mov %r,env+OFF`) :
  **sans** la propriété, `env+0x3bd8` (PMC5) mis à jour 1 730 fois dans **1 270 des 2 963
  blocs** ; **avec**, 0 (les 85 blocs restants portent d'autres compteurs de l'invité, les
  mêmes que sur ppc). Code statique : −3,3 %.

### 3.2 `tcg/0050` : MTTCG pour la cible 32 bits

QEMU 11.1.2 déclare MTTCG par cible dans `TCGCPUOps.mttcg_supported`
(`accel/tcg/tcg-all.c`, `tcg_init_machine`) ; le PowerPC y mettait
`TARGET_LONG_BITS == 64` (`target/ppc/cpu_init.c`). Le patch met `true`. Ce qu'on a vérifié de ce
qui supposait ppc64 :

- **`lwarx`/`stwcx.`** : `gen_load_locked` note adresse, longueur et valeur ;
  `gen_conditional_store` fait `tcg_gen_atomic_cmpxchg_tl` **dans tous les cas**, sans
  `#ifdef` : en 32 bits, un `cmpxchg` 32 bits (hôte x86-64 : `lock cmpxchg`). Les atomiques
  POWER9 (`lwat`/`stwat`) passent par `gen_serialize`/`exit_atomic` sous `CF_PARALLEL`, comme en
  64 bits (et n'existent pas sur un G4).
- **Ordre mémoire** : `TCG_GUEST_DEFAULT_MO` est devenu `guest_default_memory_order`, **0 pour
  les deux cibles** (tout ordre est une barrière explicite : `sync`, `lwsync`, `eieio`, `isync`
  → `tcg_gen_mb`). Rien à changer.
- **MMU et TLB** : le 7447 prend `mmu-hash32.c` et le cas `POWERPC_MMU_32B` de
  `ppc_tlb_invalidate_one`/`check_tlb_flush` dans les deux binaires (les `#ifdef TARGET_PPC64`
  n'y entourent que les MMU 64 bits) ; diffusion du `tlbie` (`TLB_NEED_GLOBAL_FLUSH`,
  `x-sr-tlb`, classes de `x-tlb-precise`) sans condition de cible.
- **mac99** (`hw/ppc/mac_newworld.c`) : seul `#ifdef TARGET_PPC64` = le câblage d'un 970 ;
  réveil du second CPU (GPIO 4 de KeyLargo, `cpu_kick_work` par `async_run_on_cpu`), OpenPIC,
  `max_cpus = 2`, `FW_CFG_NB_CPUS` : identiques. `default_cpu_type` diffère (7400 contre 970)
  mais le lanceur passe `-cpu g4`.
- **Nos patches** (série appliquée, `git diff` de `~/src/qemu-ppc32mt`) : un seul `#ifdef
  TARGET_PPC64` (`fpu_helper.c`, troncature de `nip` hors mode 64 bits, `fpk_exit`), et des
  `TARGET_LONG_BITS == 64` dans `lmw-vector.h` (NEON/SSE2, les deux largeurs de GPR codées) et
  `fp-impl.c.inc` (`FPK_TL_ARG`, les deux). Aucune des branches MTTCG de `x-tb-fast`,
  `x-sr-tlb`, `x-jc-*`, `x-tlb-precise`, `x-msr-nobql`, `x-icbi-sync` ne dépend de la cible
  (elles testent `qemu_tcg_mttcg_enabled()`, `CF_PARALLEL` ou `CPU_NEXT(first_cpu)`). Tous
  ont déjà leurs preuves SMP=1 sur `qemu-system-ppc` (`docs/tcg-g4.md` §30.5).
- **qgpu** : périphérique PCI, mémoire physique, aucun code dépendant de la cible
  (`grep TARGET_` vide).
- **`-smp 1`** : avec le patch, « auto » choisirait MTTCG même pour un vCPU. Pour ne rien changer
  par défaut, `run_tiger.sh` et `devloop.py` disent désormais `thread=single` en SMP=1
  (identique à aujourd'hui sur tout binaire existant).

### 3.3 Lanceur, sondes, construction

- `scripts/caps.sh` : `qemu_ppc32_mttcg_ok <bin> <machine> <n>` — démarre figé avec
  `-smp n -accel tcg,thread=multi` **et** sans « Guest not yet converted to MTTCG » (un
  `qemu-system-ppc` sans le patch démarre quand même, en prévenant). Témoins : `build-base` et
  `~/src/qemu/build` → 1, `build-mt` → 0. Jamais `-device help`.
- `run_tiger.sh` : `PPC32MT=1` (avec SMP ≥ 2) prend `$QEMU_BIN` en `tcg,thread=multi` si la
  sonde passe (bannière « SMP 2 cœurs (MTTCG, ppc 32 bits) »), sinon avertit et garde ppc64.
  `PMUREAL=1` → `x-pmu-real=on`, sondé. `scripts/qemu_fast.sh` : avec `PPC32MT=1`, le binaire
  rapide est jugé sur `qemu-system-ppc` et sa déclaration MTTCG.
- Ligne de commande de la mesure statique :
  `qemu-system-ppc{,64} -M mac99,via=pmu -smp 1 -cpu g4,<production>[,x-pmu-real=on]
  -accel tcg,thread=single,x-jit-near=on,x-jc-bits=14,x-jc-2way=on -m 768
  -bios patches/smp-mac99/openbios-smp-screamer.elf -display none -nic none
  -d in_asm,out_asm -D <journal>` (`timeout 10`).
- `tools/guest/devloop.py` : `PPC32MT=1` garde `qemu-system-ppc` en SMP>1 ; `thread=single` en
  SMP=1.
- `scripts/build_qemu_qfb.sh` : 0049 et 0050 après 0048, marqueurs vérifiés ; capacités
  `x-pmu-real` (sur ppc64) et `mttcg-ppc32` (sur ppc) consignées (optionnelles).

## 4. Preuves en SMP=2 sur `qemu-system-ppc` (MTTCG)

### 4.1 Tableau (au niveau de `docs/tcg-g4.md` §30.5)

`build-mt`, VM de dev, single-user, toutes les options de production ; démarrage de
**référence**, puis démarrage **vérifié** (`x-vfp-native-verify`, `x-vmx-verify`,
`x-ret-verify`, `x-tb-verify` en plus) ; témoin : le même démarrage vérifié sur
`qemu-system-ppc64` du même arbre. Job `suite` = `jit-suite` (`vfptest`, `vmxtest`,
`lmwtest`, `tbtest`, `smctest`, `jctest`) + `atomtest`.

| | SMP=2 `qemu-system-ppc` MTTCG | SMP=2 `qemu-system-ppc64` (témoin) |
|---|---|---|
| `vfptest` | `bc13e93c39e62602` (référence = vérifié) | identique |
| `vmxtest` | `c24e3d31d479381b` ; fautes 48/48, page intacte | identique |
| `lmwtest` | `2575eafce78adf66` ; faute `lmw 31` signal 10 | identique |
| `smctest` A-F | `7308784105b8fd59`, 0 erreur | identique |
| `jctest` | appels indirects et invalidation à chaud OK | OK |
| `tbtest` | 0 recul | 0 recul |
| `atomtest` (§4.2) | A, B, C exacts (8 000 000) | exacts |
| `x-vfp-native-verify` | 34 066 432 opérations, 8 573 259 prédites courtes, **0 divergence** | idem, **0** |
| `x-vmx-verify` | 301 989 888 + 50 331 648, **0 + 0** | idem, **0 + 0** |
| `x-ret-verify` | 3,06 G en ligne + 46 M en boucle + 79 M par le helper ; **14 « sans traduction », 0 autre** ; 149 courses (`smctest`) | 3,12 G + 47 M + 44 M ; **les mêmes 14** ; 123 courses |
| `x-tb-verify` | 799 831 939 lectures, 0 positif | 983 702 123, 0 positif |

Empreintes = celles du §30.5 (et du M4). Les 14 « sans traduction » sont tous au démarrage, sur
cpu 0, `mmu_idx` 1, `pc` dans BootX (`0x5601cb0`, `0x5610664`, `0x561171c`) ou OpenBIOS
(`0xfff0d044`, `0xfff16f0c`, `0xfff1ccb4`) : la classe expliquée au `docs/tcg-g4.md` §35
(OpenBIOS évince une PTE sans `tlbie` de sa page, `x-tlb-precise` garde l'entrée du cache de
sauts), règle de lecture du §35.2. **Le témoin ppc64 donne exactement les mêmes 14 cas aux
mêmes `pc`** : rien n'est propre à la cible 32 bits.

### 4.2 Contention `lwarx`/`stwcx.` (`atomtest`)

`tools/guest/jobs/mtbench/atomtest.c` : deux fils (deux vCPU), 4 000 000 d'incréments chacun :
A, une boucle `lwarx`/`addi`/`stwcx.`/`bne-` sur un mot partagé ; B, un verrou tournant
`lwarx`/`stwcx.` + `isync` à l'acquisition, `sync` + `stw 0` à la libération, autour d'un
incrément **ordinaire** (vérifie aussi l'ordre mémoire) ; C, deux mots voisins de la même ligne,
un par fil. Résultat exigé : 8 000 000 partout.

| | A (lwarx) | B (verrou) | C (voisins) |
|---|---|---|---|
| ppc MTTCG, référence | exact, 0,339 s | exact, 0,882 s | exact, 0,208 s |
| ppc MTTCG, vérifié | exact, 0,431 s | exact, 1,410 s | exact, 0,243 s |
| ppc64, vérifié | exact | exact | exact |
| (SMP=1, ppc, 2 × 2 000 000) | exact, 0,027 s | exact, 0,219 s | exact, 0,021 s |

Le facteur ~12 entre SMP=1 et SMP=2 sur A est la vraie contention (la ligne et la réservation
font des allers-retours entre deux cœurs hôte, chaque `stwcx.` est un `lock cmpxchg`) : la
preuve que les deux fils tournaient bien en parallèle.

### 4.3 Bureau, GPU, plugin

| épreuve (SMP=2, `qemu-system-ppc` MTTCG) | résultat |
|---|---|
| bureau (`devloop.py start --gui`) | Finder, Dock, SystemUIServer, WindowServer ; `hostinfo` : « 2 processors are physically available », « Processors active: 0 1 » ; capture `bench/ppc32mt/bureau-p32/bureau.png` (non versionné) ; arrêt propre (`shutdown -h now`, volume démonté) |
| job `gpu` (single-user) | `VERDICT : 0 échec(s)` |
| job `plugmd5` (**single-user**, comme il se doit : en session graphique il donne un faux vert) | `VERDICT : 66 scènes, 0 écart(s)` ; **les 66 md5 identiques** à ceux du même job sur `qemu-system-ppc64` (même binaire, SMP=2) |
| `t2bench` (§5) | image `7ff741d6…` identique sur les trois bras |

En passant : `plugmd5` a coûté 742 G instructions hôte sur ppc contre 823 G sur ppc64 (−9,8 %,
un démarrage chacun). Aucun avertissement « Guest not yet converted to MTTCG » dans les
journaux de QEMU des démarrages ppc.

## 5. Banc SMP=2 : ppc64 contre ppc en MTTCG

Même VM de dev, même binaire `build-mt` (0049 + 0050), mêmes options de production, SMP=2
(`hw.ncpu` = 2 dans l'invité) ; trois bras entrelacés `p64 p32 p64pmu | p64pmu p32 p64` :
`qemu-system-ppc64` tel quel, `qemu-system-ppc` en MTTCG (`PPC32MT=1`), `qemu-system-ppc64` avec
`x-pmu-real=on`. Un démarrage par bras et par tour a eu son tampon de JIT dans « l'AUTRE
fenêtre de 4 Gio » (`b2-*-a`) : réparti.

| SMP=2 (médianes de 2 démarrages) | ppc64 | **ppc MTTCG** | ppc64 + `x-pmu-real` |
|---|---|---|---|
| `mtbench`, somme des noyaux (ms) | 39 907 | **38 162 (−4,4 %)** | 39 993 (+0,2 %) |
| … `sys` / `ctx` / `gcc` / `par2` | 2 596 / 1 993 / 4 730 / 13 684 | 2 359 / 1 829 / 4 326 / 12 945 (−9,1 / −8,2 / −8,5 / −5,4 %) | 2 566 / 2 017 / 4 620 / 13 826 |
| … `call` / `mem` / `fp` / `vfp` / `lmw` | 452 / 3 620 / 412 / 11 218 / 959 | 440 / 3 573 / 419 / 11 080 / 960 | 451 / 3 629 / 408 / 11 287 / 950 |
| `mtbench`, instructions hôte | 1 982 G | **1 880 G (−5,2 %)** | 1 957 G (−1,3 %) |
| `mtbench`, cycles hôte | 873 G | **831 G (−4,8 %)** | 872 G (−0,1 %) |
| `t2bench`, temps (s) | 7,82 | **7,37 (−5,8 %)** | 7,72 (−1,3 %) |
| `t2bench`, instructions hôte | 834 G | **777 G (−6,9 %)** | 807 G (−3,2 %) |
| `t2bench`, cycles hôte | 395 G | **366 G (−7,4 %)** | 385 G (−2,4 %) |

Images de `t2bench` identiques dans les trois bras ; `atomtest` exact partout.

Lecture :

- **ppc en MTTCG bat ppc64 en SMP=2 d'autant qu'en SMP=1** (−5 à −7 % d'instructions et de
  cycles hôte) : le prix de MTTCG lui-même (barrières, `cmpxchg`, TLB synchronisés, BQL) est le
  même pour les deux cibles, il ne mange pas le gain.
- Le PMU seul (`x-pmu-real`) vaut **−1,3 à −3,2 % d'instructions**, −0 à −2,4 % de cycles :
  plus sur `t2bench` (des blocs courts, du code de bibliothèque et de pilote, où le `PMC5 +=`
  pèse plus par instruction) que sur le calcul pur. Le temps de `mtbench` ne bouge pas aux
  erreurs de mesure près. C'est à peu près la moitié de l'écart `t2bench` ppc/ppc64 ; l'autre
  moitié est `NARROW_MODE` et la largeur des registres.
- `sys`, `ctx` et `gcc` (noyau, appels système, changements de contexte : beaucoup de petits
  blocs, de `rfi`, d'accès à la MMU) gagnent le plus ; le flottant et l'AltiVec (`fp`, `vfp`,
  `lmw`), dont le code est fait de helpers et d'ops natives identiques des deux côtés, presque
  rien.
- `par2` (deux copies simultanées) : 13,0 s en SMP=2 contre 21,6 s en SMP=1 : les deux vCPU
  travaillent vraiment en parallèle sur `qemu-system-ppc`.

## 6. Gains attendus

Sur le processeur émulé, à charge de DOOM 3 (beaucoup de noyau, de pilote et de
bibliothèques, comme `t2bench`) : **−5 à −7 % de cycles hôte** pour le code du G4, soit
×1,05-1,08 de vitesse. Amplifié par les tics de DOOM 3 (`docs/vitesse-doom3-x86.md` §3.1 :
10 % de vitesse donnent 12,9 % de temps d'image), on peut attendre **−6 à −9 % de temps
d'image** sur DOOM 3, soit de ~92 à ~84-86 ms ; moins sur Marble Blast et Zenerchi (fil de jeu
moins dense en code invité). **À confirmer en jeu** (§7) : la VM de dev ne joue pas les jeux.

`x-pmu-real` seul (si l'on reste sur ppc64) : −1 à −3 % d'instructions, ~−1 à −2 % de temps
d'image attendu, sous le bruit d'une partie.

## 7. A/B en jeu sur la VM quotidienne (session principale)

`tools/tcg/matab.sh` lance le `run_tiger.sh` du **dépôt principal**. Tant que cette branche
n'est pas fusionnée, il ne connaît ni `PPC32MT` ni `PMUREAL` : on passe par un dossier de liens
où **`qemu-system-ppc64` est le binaire 32 bits** (le lanceur d'aujourd'hui prend
`${QEMU_BIN}64` en SMP=2, avec `thread=multi` ; le binaire patché ne prévient plus, ses
capacités sont sondées comme d'habitude ; seule la bannière dit encore « ppc64 ») :

    ~/src/qemu-ppc32mt/p32comme64/qemu-system-ppc   -> ../build-mt/qemu-system-ppc
    ~/src/qemu-ppc32mt/p32comme64/qemu-system-ppc64 -> ../build-mt/qemu-system-ppc   (sic)

(déjà créé ; `ln -sf` pour le refaire). Les trois bras tournent sur **le même arbre et la même
compilation** (`build-mt`, `-O3 -march=native`, sans LTO ni PGO) : seule la cible change.
`x-pmu-real` passe par `CPU_OPTS`, que le lanceur actuel transmet tel quel.

    B=$HOME/src/qemu-ppc32mt
    cd ~/src/pomppc
    # 1. A/B entrelacé, 4 jeux, fenêtre (3 parties par bras et par jeu)
    for j in mb zen ut d3; do
      tools/tcg/matab.sh p32mt-$j 3 \
        "p64:QEMU_BIN=$B/build-mt/qemu-system-ppc" \
        "p32:QEMU_BIN=$B/p32comme64/qemu-system-ppc" \
        "pmu:QEMU_BIN=$B/build-mt/qemu-system-ppc CPU_OPTS=x-pmu-real=on" \
        $j fen
    done
    python3 tools/tcg/matabsum.py bench/tcg/ab/p32mt-d3      # (idem mb, zen, ut)
    # 2. plein écran (règle du projet : preuves en fenêtre ET en plein écran), au moins d3 et ut
    tools/tcg/matab.sh p32mt-d3-pe 2 "p64:QEMU_BIN=$B/build-mt/qemu-system-ppc" \
        "p32:QEMU_BIN=$B/p32comme64/qemu-system-ppc" d3 pe
    # 3. une partie DOOM 3 sous les vérificateurs (pas pour la vitesse : bilans dans run_tiger.log)
    tools/tcg/matab.sh p32mt-d3-verif 1 \
        "v:QEMU_BIN=$B/p32comme64/qemu-system-ppc CPU_OPTS=x-vfp-native-verify=on,x-vmx-verify=on,x-ret-verify=on,x-tb-verify=on" d3 fen
    tools/tcg/matab.sh --restore

(Modes de la matrice : `fen` fenêtre, `pe` plein écran, `tools/matrice/jeu.py`.)
Vérifier dans chaque `run_tiger.log` du bras `p32` : chemin du binaire
`…/p32comme64/qemu-system-ppc64`, **pas** d'avertissement « Guest not yet converted to MTTCG »,
et, dans l'invité, `sysctl hw.ncpu` = 2. Lire le temps d'image (métrique : CPU de QEMU,
`docs/vitesse-doom3-x86.md`) ; une régression d'image ou un gel sur un bras arrête tout.

**Après fusion**, la même chose sans dossier de liens :

    tools/tcg/matab.sh p32mt-d3 3 "p64:QEMU_BIN=$B/build-mt/qemu-system-ppc" \
        "p32:QEMU_BIN=$B/build-mt/qemu-system-ppc PPC32MT=1" \
        "pmu:QEMU_BIN=$B/build-mt/qemu-system-ppc PMUREAL=1" d3 fen
    # et, sur le binaire rapide reconstruit (§8) : "fast:QEMU_FAST=1 PPC32MT=1"

## 8. Binaire rapide (PGO) et parité arm64

**Binaire rapide.** `build-fast/` (LTO + PGO, `docs/binaire-rapide-x86.md`) est construit pour
les **deux** cibles par `QEMU_FAST=1 ./scripts/build_qemu_qfb.sh` ; avec 0049-0050 dans la
série, rien d'autre à faire pour la construction (les deux patches sont dans l'empreinte de la
série : l'ancien `build-fast` sera écarté par `qemu_fast.sh` jusqu'à reconstruction). Mais le
**profil** : `tools/tcg/pgo-train.sh` joue la matrice avec `run_tiger.sh` par défaut, donc
**SMP=2 sur `qemu-system-ppc64`** — les objets propres à la cible 32 bits (`target/ppc/*` et
tout ce que meson compile par cible pour `qemu-system-ppc`) n'ont aujourd'hui de profil que par
les rares parties SMP=1, et seraient compilés **sans profil** (`-Wno-missing-profile`). Si
`PPC32MT=1` devient le défaut :

1. entraîner avec le bras `PPC32MT=1` : `pgo-train.sh` passe `"train:QEMU_BIN=$FAST"` à
   `matab.sh` ; ajouter `PPC32MT=1` à ce bras (après fusion ; avant fusion, un dossier de liens
   comme au §7, côté `build-fast/`) — ou faire deux parties, une par cible, si l'on garde les
   deux usages ;
2. reconstruire (`--etapes use`) puis refaire l'A/B du §7 en `fast:QEMU_FAST=1 PPC32MT=1` contre
   `QEMU_FAST=1` seul : le gain doit se retrouver sur le binaire quotidien.

Sans profil 32 bits, l'A/B serait biaisé contre ppc (−8 % au lieu de −13 % pour PGO, §5.5 de
`docs/vitesse-doom3-x86.md`) : ne pas conclure sur un `build-fast` entraîné en ppc64 seul.

**Parité arm64 (règle du projet).** Les deux patches ne touchent **aucun émetteur** :
`power8-pmu.c`, `cpu.h`, `cpu_init.c`, code commun au frontal. Le même patch vaut pour le M4 ;
il faut l'y **construire et prouver au même niveau** : `mtbench` SMP=1 (porte) et SMP=2
(`PPC32MT=1`), la suite et les vérificateurs (`tools/tcg/ppc32mt-bench.sh` est écrit pour le PC :
`perf` → rien sous macOS, `SO`… à adapter, ou `tools/tcg/jit-m4-proof.sh` avec `PPC32MT=1`, que
`devloop.py` comprend), `atomtest` (sur arm64, `stwcx.` devient `casal`/LL-SC : l'épreuve de
contention y compte plus encore), puis l'A/B en jeu sur la VM du M4. Le défaut, s'il change,
change sur les deux hôtes en même temps.

## 9. Ce qui reste

1. **L'A/B en jeu** sur la VM quotidienne (§7), les 4 jeux, fenêtre et plein écran, et une
   partie DOOM 3 vérifiée. Marble Blast n'a pas été joué sur la VM de dev (jobs `games`/
   `fpgames` non lancés : la VM de dev n'a pas servi aux jeux ici).
2. ~~Le binaire rapide entraîné sur la cible 32 bits~~ : fait le 23/10 (§11),
   `build-fast-mt`, −5 à −9 % contre le binaire rapide quotidien sur la VM de dev ; reste
   l'A/B en jeu des binaires rapides (§12).
3. **Parité arm64** : construire et prouver 0049-0050 sur le M4 (§8).
4. ~~Endurance~~ : faite le 23/10 sur la VM de dev (§10) : 52 démarrages sur 52 (40
   `shutdown -r`), 1 h de charge sans erreur. Reste une longue session de jeu en `PPC32MT=1`
   sur la VM quotidienne (l'A/B des §7 et §12 en tient lieu en partie).
5. Si les jeux confirment : `PPC32MT=1` par défaut (et `x-pmu-real` devient sans objet en
   SMP=2 ; il reste utile au seul cas ppc64). Les deux binaires resteraient construits : le
   ppc64 sert encore de témoin.
6. Hors de ce levier, ce que la cible 32 bits laisse : le reste de l'écart statique est le
   coût propre des helpers et de la MMU, identique des deux côtés — le traducteur de second
   niveau (`docs/plan-traducteur-rapide.md`) reste le grand levier sur le code généré.

Risques :

- **MTTCG 32 bits n'a jamais tourné en amont** : nos épreuves (suite, vérificateurs, `atomtest`,
  bureau, GPU, plugin) couvrent les mécanismes connus, pas toutes les courses possibles d'une
  longue partie. `x-ret-verify` montre 149 courses attendues (`smctest`) comme sur ppc64.
- Les `pc` « sans traduction » de `x-ret-verify` (§4.1) : la classe connue du §35 de
  `docs/tcg-g4.md`, identique sur ppc64 ; toute autre divergence en jeu reste à instruire.
- La bannière du lanceur actuel annoncera « ppc64 » pendant l'A/B par dossier de liens : c'est
  le chemin du binaire qui fait foi.
- Un `build-fast` pris avec un profil PGO ppc64 seul fausserait la comparaison (§8).
- Le profil de `build-fast-mt` vient des bancs de la VM de dev, pas des jeux (§11.2) : en jeu,
  il peut valoir moins que celui de `build-fast` ; ne pas l'adopter sans l'A/B du §12.
- `devdemarrages.py` ne voit que ce que voit l'agent (processus, `hostinfo`) : un bureau gelé
  avec un agent vivant passerait pour sain si les processus restent présents.

Journaux (non versionnés) : `bench/ppc32mt/` du worktree — `g1-*` (porte SMP=1), `p2-ref`,
`p2-ver`, `p2-ver64` (preuves), `b2-*` (banc SMP=2), `g2-p32`, `g2-p64` (`gpu`, `plugmd5`),
`bureau-p32`. Résumé : `python3 tools/tcg/ppc32mtsum.py --ref b2-p64 --cmp b2-p32 bench/ppc32mt/b2-*`.

Fichiers : `patches/tcg/0049-ppc-pmu-real.patch`, `patches/tcg/0050-ppc-mttcg-32.patch`,
`scripts/build_qemu_qfb.sh`, `scripts/caps.sh` (`qemu_ppc32_mttcg_ok`), `scripts/qemu_fast.sh`,
`run_tiger.sh` (`PPC32MT`, `PMUREAL`, `thread=single` en SMP=1), `tools/guest/devloop.py`,
`tools/guest/jobs/mtbench/` (`job.sh`, `atomtest.c`), `tools/tcg/ppc32mt-bench.sh`,
`tools/tcg/ppc32mtsum.py`, `tools/tcg/pmc5count.py`. Arbre QEMU : `~/src/qemu-ppc32mt`
(`build-base` = série du 21/10, `build-mt` = + 0049-0050, `-O3 -march=native` sans LTO ;
`p32comme64/` = dossier de liens du §7).

## 10. Endurance de MTTCG 32 bits (VM de dev, 23/10)

`build-mt/qemu-system-ppc`, SMP=2, `PPC32MT=1` (`tcg,thread=multi`), toutes les options de
production (`-cpu g4,x-fast-fp=on,…,x-pteg-map=on`, `x-jit-near`, `x-jit-rel32`, `x-jc-bits=14`,
`x-jc-word`, `x-jc-2way`), disque `disks/tiger-dev-ppc32.raw` (10.4.11, bureau par ouverture
de session automatique), sans son (`SND=none`), `-display none`. Tout dans l'enclos de l'A/B en
cours sur la VM quotidienne : `taskset -c 6,7,14,15 nice -n 19` (deux cœurs physiques).

### 10.1 Pourquoi pas `endurance.py`, et `tools/endurance/devdemarrages.py`

`tools/endurance/endurance.py` interroge l'invité par **ssh** avec la clé du banc
(`.run/cmr/id_rsa`). Essai `essai-p32` (un démarrage à froid, recouvrement qcow2 posé sur le
raw de dev : `endurance.py` sait désormais le faire, voir plus bas) : bureau intact à l'écran,
vCPU au repos (`machine_idle`), sshd répond (`SSH-1.99-OpenSSH_4.5`) mais refuse la clé
(code 255) : le disque de dev n'a pas la clé du banc dans `~tiger/.ssh/authorized_keys`. Ce
« gel » est un faux positif de l'outil, pas de l'invité ; il n'est pas compté. On n'a pas
modifié le compte de l'invité.

D'où `tools/endurance/devdemarrages.py` : même logique de cycles, mais l'invité est interrogé
par **l'agent de devloop** (StartupItem `POMPPCAgent`, boîtes aux lettres sur le disque) :

- départ à froid (`devloop.py start --gui`) tous les `--froid-tous` cycles, sinon
  redémarrage dans le même QEMU (job de l'agent : `shutdown -r now` en tâche de fond) — le
  chemin du gel AppleUSBOHCI (`docs/endurance.md` §6) ;
- sondes de 45 s (un identifiant de job neuf à chaque essai : l'agent fraîchement démarré
  ignore le job resté dans la boîte) jusqu'au bureau complet — `WindowServer`, `Finder`,
  `Dock`, `SystemUIServer` —, `hw.ncpu` = 2 et `hostinfo` « Processors active: 0 1 » ; une
  seconde sonde `--repos` (20) s plus tard ;
- moniteur HMP ajouté par `QEMU_EXTRA` : panique vue par `panicstr` ou un vCPU dans `panic()`
  (symboles `bench/endurance/invite-10.4.11`), collecte d'incident de `endurance.py`
  (registres, RAM, écran, `kpanic.py`) ; gel = pas de bureau au bout de `--delai` (600) s.

`endurance.py` a gagné, sans rien changer à ses défauts : `ENDURANCE_BANC` et
`ENDURANCE_RUNDIR` (journaux et instances hors du `.run/` du dépôt principal), une base
`.raw` (recouvrement `-F raw`) et, pour une base inscriptible, un `flock` **partagé** sur
`<base>.lock` pendant la campagne (devloop prend ce verrou en exclusif : il ne peut plus écrire
la base sous les recouvrements).

### 10.2 Démarrages répétés : 52 sur 52

| campagne | départs | résultat | durée (médiane / max) | bureau atteint (médiane / max) |
|---|---|---|---|---|
| `essai` | 1 froid + 1 reboot | 2 ok | 112 / 99 s | 88 / 75 s |
| `p32-a` (12:24-13:54) | 10 froids + 40 reboots | **50 ok** | froid 132 / 142 s ; reboot 101 / 115 s | froid 110 / 118 s ; reboot 77 / 91 s |

À chaque cycle : 2 processeurs (`hw.ncpu` 2, « Processors active: 0 1 »), bureau complet,
aucun `panic.log`, rien dans `dmesg` sur `USBOHCI`, `panic` ou `LockTimeOut`. **0 gel, 0
panique sur 52 démarrages** (IC 95 % de Wilson : 0-6,9 %). Témoin ppc64 : le PC a 2 paniques et
6 « pas de ssh » sur 923 démarrages en ppc64 (`docs/endurance.md` §8.6, ≈ 0,9 %), et le gel
AppleUSBOHCI est corrigé depuis le 29/09 (`patches/openpic/0001`, 0 sur 253). Aucun gel n'étant
apparu, le témoin ppc64 sur ce disque n'a pas été rejoué (règle de la mission : seulement si le
gel apparaît). 52 démarrages ne mesurent pas un taux de 1 % : ils excluent un défaut grossier
propre à MTTCG 32 bits (≥ 6 %), pas un défaut rare.

Journaux (non versionnés) : `bench/endurance-dev/{essai,p32-a}/` (`cycles.jsonl`,
`rapport.md`), `bench/endurance-dev/p32-a.out`.

### 10.3 Longue charge : 1 h, 0 erreur

Job `mtcharge` (`tools/guest/jobs/mtcharge/`, `tools/tcg/ppc32mt-bench.sh charge-p32
mtcharge`, single-user, SMP=2 MTTCG, 14:06-15:07) : pendant 3 600 s, en même temps, deux fils
de `gcc -O2 -faltivec -c` (chaque objet comparé au premier par md5), `atomtest` en boucle
(`lwarx`/`stwcx.`, verrou + `sync`, voisins ; 2 × 1 000 000 incréments, total exact exigé) et
la scène gltest `d3inter` (plugin + GLEngine + kext, 60 images toutes les 20 s, md5 de l'image
comparé au premier).

| fil | tours | erreurs |
|---|---|---|
| `gcc` (regbench.c) | 3 617 | 0 |
| `gcc` (vfptest.c) | 807 | 0 |
| `atomtest` | 7 578 (×3 épreuves) | 0 |
| gltest `d3inter` | 118 | 0 (md5 `587c064f…` constant) |

Charge de l'invité ~4,0 tout du long (relevés toutes les 10 min), QEMU à ~180 % d'un cœur hôte ;
7 361 s de CPU, 54,9 T instructions hôte. Arrêt propre (`halt`, volume démonté) ensuite.
L'agent de ce disque écrit l'en-tête à trois champs (code de sortie non rendu) : le verdict est
la ligne `charge bilan OK` du job. Journal : `bench/ppc32mt/charge-p32/`.

**Verdict endurance : critère tenu** (0 gel ou panique, rien de plus que le témoin ppc64).

## 11. Binaire rapide (LTO + PGO) de la cible 32 bits : `build-fast-mt`

### 11.1 Construction et profil

`~/src/qemu-ppc32mt/build-fast-mt/` (même arbre que `build-mt`, série 0049-0050 comprise,
`serie=70b6801aa3c0ec49`), `QEMU_OPT=native,nohard,lto,pgo` (`-O3` par `-Doptimization=3`,
`-march=native` = skylake, LTO, PGO) ; profil `~/src/qemu-ppc32mt/pgo-fast-mt/` (1 255 `.gcda`,
`pomppc-profil.txt`). `build-mt` et `build-base` n'ont pas été touchés (le script recopie
seulement `screamer.c`, `qfb-pci.c` et `qgpu-*.c` dans les sources, contenu identique).

    W="taskset -c 6,7,14,15 nice -n 19"
    export QEMU_SRC=~/src/qemu-ppc32mt QEMU_FAST_BUILD=build-fast-mt PGO_WRAP="$W" \
           PGO_MEM=20G PGO_SWAP=2G LTO_JOBS=4 JOBS=4 \
           DEVDISK=~/src/pomppc/disks/tiger-dev-ppc32.raw TIMEOUT=5400
    $W tools/tcg/pgo-train.sh --etapes gen                               # 14 min, 1,7 Gio max
    $W tools/tcg/pgo-train.sh --etapes jouer --banc dev --cible ppc32mt  # 30 min
    $W tools/tcg/pgo-train.sh --etapes use                               # 13 min, 1,3 Gio max

Ce qui a changé dans les outils (défauts inchangés) :

- `scripts/build_qemu_qfb.sh` : `QEMU_FAST_BUILD` (dossier du binaire rapide, défaut
  `build-fast`) ; profil par défaut `<arbre>/pgo-<nom sans « build- »>`.
- `tools/tcg/pgo-train.sh` : `QEMU_FAST_BUILD`, `PGO_WRAP` (préfixe de tout ce qui est lancé),
  `PGO_SWAP` ; **`--cible ppc64|ppc32mt|les-deux`** pour la matrice (bras `PPC32MT=1` ; les
  `.gcda` des deux cibles s'additionnent) ; **`--banc dev`** : la partie d'entraînement se
  joue sur une VM de dev (`DEVDISK`, refus de `tiger.qcow2`) : un bureau (`devloop start
  --gui`, `PGO_GUI` = 120 s, arrêt propre), puis un démarrage single-user avec `PGO_JOBS`
  (défaut `plugmd5 gpu suite mtbench t2bench`) par `tools/tcg/ppc32mt-bench.sh`, SMP=2,
  options de production, pour chaque cible demandée.

Partie d'entraînement (`bench/ppc32mt/pgo-train-20261023-1533-ppc32mt/`), binaire
instrumenté, SMP=2 MTTCG : bureau (1 256 `.gcda` déjà au premier arrêt : QEMU quitte de
lui-même sur `shutdown -h`), `plugmd5` **66 scènes, 0 écart**, `gpu` **0 échec**, `suite`
(`atomtest` OK), `mtbench`, `t2bench` (image `7ff741d6…`). L'instrumenté est ~2,5 fois plus lent
(`plugmd5` : 340 s, 1 082 G instructions contre 742 G).

**Le profil couvre la cible lancée** : `target/ppc/*` et tout ce que meson compile pour
`qemu-system-ppc` ont leur profil ; le `qemu-system-ppc64` de `build-fast-mt` n'a que celui du
code commun (pour lui, `--cible les-deux`).

### 11.2 Mesure : p64 rapide actuel contre p32 rapide

VM de dev, single-user, SMP=2, options de production, `tools/tcg/ppc32mt-bench.sh … mtbench
t2bench`, trois bras entrelacés `f64 f32 m32 | m32 f32 f64` (16:18-17:30) :

- **f64** : `~/src/qemu/build-fast/qemu-system-ppc64`, le binaire rapide quotidien (LTO+PGO
  entraîné sur les 4 jeux, série `19f435fc…` du 21/10, lu seulement), MTTCG ppc64 ;
- **f32** : `build-fast-mt/qemu-system-ppc`, `PPC32MT=1` ;
- **m32** : `build-mt/qemu-system-ppc`, `PPC32MT=1` (`-O3 -march=native`, sans LTO ni PGO).

| SMP=2 (médianes de 2 démarrages) | f64 (rapide actuel) | **f32 (p32 rapide)** | m32 (`build-mt`) |
|---|---|---|---|
| `mtbench`, somme des noyaux (ms) | 36 200 | **34 290 (−5,3 %)** | 38 333 (+5,9 %) |
| … `sys` / `ctx` / `gcc` / `par2` | 2 282 / 1 723 / 4 632 / 11 553 | 2 055 / 1 518 / 4 232 / 10 921 (−9,9 / −11,9 / −8,6 / −5,5 %) | 2 397 / 1 882 / 4 436 / 13 053 |
| … `call` / `mem` / `vfp` / `lmw` | 455 / 2 719 / 11 218 / 957 | 421 / 2 678 / 10 871 / 959 | 427 / 3 612 / 10 927 / 958 |
| `mtbench`, instructions hôte | 1 742 G | **1 625 G (−6,7 %)** | 1 867 G |
| `mtbench`, cycles hôte | 777 G | **739 G (−4,9 %)** | 841 G |
| `t2bench`, temps (médiane des passes) | 7,62 s | **6,91 s (−9,3 %)** | 7,37 s |
| `t2bench`, instructions hôte | 788 G | **733 G (−6,9 %)** | 774 G |
| `t2bench`, cycles hôte | 385 G | **354 G (−8,0 %)** | 378 G |

Images de `t2bench` identiques dans les trois bras (`7ff741d6…`), `atomtest` exact partout.
Gain du binaire rapide sur la cible 32 bits (f32 contre m32) : −10,5 % de temps `mtbench`,
−12 % de cycles ; −6,3 % de temps `t2bench`.

Lecture et réserves :

- **f32 bat le binaire rapide quotidien de 5 à 9 %** de temps, 5 à 8 % de cycles hôte : le gain
  de la cible 32 bits du §5 (−5 à −7 %) se retrouve entier sur les binaires rapides.
- **Biais en faveur de f32** : son profil a été pris sur `mtbench` et `t2bench` eux-mêmes (plus
  `plugmd5`, `gpu`, `suite` et un bureau), celui de f64 sur les jeux. Sur ces bancs, une part de
  l'écart vient de là ; c'est pour cela que **le verdict est l'A/B en jeu** (§12), où le biais
  joue dans l'autre sens (f64 a été entraîné sur les jeux mêmes).
- f64 est sur la série du 21/10 (sans 0048-0050) ; les options de production sont les mêmes.
- Le `mem` de m32 (3 612 ms contre ~2 700) est le prix du binaire sans LTO/PGO, pas de la cible.

Journaux : `bench/ppc32mt/bf-{f64,f32,m32}-{a,b}/` ; résumé :
`python3 tools/tcg/ppc32mtsum.py --ref bf-f64 --cmp bf-f32 bench/ppc32mt/bf-f64-* bench/ppc32mt/bf-f32-*`.

## 12. A/B en jeu des binaires rapides (pour la session principale)

`matab.sh` lance le `run_tiger.sh` du dépôt principal, qui ne connaît pas `PPC32MT` tant que
cette branche n'est pas fusionnée : même astuce qu'au §7, un dossier de liens où
`qemu-system-ppc64` **est** le binaire 32 bits rapide (déjà créé) :

    ~/src/qemu-ppc32mt/p32rapide/qemu-system-ppc   -> ~/src/qemu-ppc32mt/build-fast-mt/qemu-system-ppc
    ~/src/qemu-ppc32mt/p32rapide/qemu-system-ppc64 -> ~/src/qemu-ppc32mt/build-fast-mt/qemu-system-ppc  (sic)

Le bras `f64` impose le binaire rapide quotidien par `QEMU_BIN` (sinon `matab.sh` part en
`QEMU_FAST=0`, la référence) ; en SMP=2 le lanceur prend son `…64`.

    F64=$HOME/src/qemu/build-fast/qemu-system-ppc
    F32=$HOME/src/qemu-ppc32mt/p32rapide/qemu-system-ppc
    cd ~/src/pomppc
    # 1. fenêtre, 4 jeux, 3 parties par bras et par jeu, entrelacées
    for j in mb zen ut d3; do
      tools/tcg/matab.sh p32fast-$j 3 "f64:QEMU_BIN=$F64" "f32:QEMU_BIN=$F32" $j fen
    done
    for j in mb zen ut d3; do python3 tools/tcg/matabsum.py bench/tcg/ab/p32fast-$j; done
    # 2. plein écran (règle du projet), au moins d3 et ut
    for j in d3 ut; do
      tools/tcg/matab.sh p32fast-$j-pe 2 "f64:QEMU_BIN=$F64" "f32:QEMU_BIN=$F32" $j pe
    done
    # 3. une partie DOOM 3 sous les vérificateurs (bilans dans run_tiger.log, pas pour la vitesse)
    tools/tcg/matab.sh p32fast-d3-verif 1 \
        "v:QEMU_BIN=$F32 CPU_OPTS=x-vfp-native-verify=on,x-vmx-verify=on,x-ret-verify=on,x-tb-verify=on" d3 fen
    tools/tcg/matab.sh --restore

Contrôles dans chaque `run_tiger.log` du bras `f32` : `binaire : …/p32rapide/qemu-system-ppc64`,
pas d'avertissement « Guest not yet converted to MTTCG », `hw.ncpu` = 2 dans l'invité ; la
bannière dit « ppc64 » (c'est le chemin qui fait foi). Métrique : CPU de QEMU, temps d'image
(`docs/vitesse-doom3-x86.md`). Une régression d'image ou un gel arrête tout.

**Après fusion**, sans dossier de liens :
`"f64:QEMU_BIN=$F64" "f32:QEMU_BIN=$HOME/src/qemu-ppc32mt/build-fast-mt/qemu-system-ppc PPC32MT=1"`.
Si `PPC32MT=1` devient le défaut, reconstruire `~/src/qemu/build-fast` avec 0049-0050 et un
profil `--cible ppc32mt` (ou `les-deux`) entraîné **sur les jeux** (`--banc matrice`, VM
quotidienne, hôte au repos), puis refaire cet A/B contre lui.

## 13. A/B en jeu sur la VM quotidienne, binaires rapides (23/10, 16 h 55 - 19 h)

`bench/ppc32mt-2310/chaine-fast.sh`, hôte au repos (le premier A/B `p32mt-*`, joué avec la VM de
l'agent d'endurance à côté, est **écarté** : dispersion 8-24 %). f64 = `~/src/qemu/build-fast`
(ppc64, binaire quotidien), f32 = `~/src/qemu-ppc32mt/build-fast-mt` (ppc 32 bits en MTTCG, PGO
entraîné sur la cible 32 bits) ; SMP=2, 3 parties par bras en fenêtre, 2 en plein écran.

| jeu (ms/image, médianes) | f64 | f32 | écart |
|---|---|---|---|
| DOOM 3, fenêtre | 94,9 | **87,6** | **−7,7 %** (les 3 parties f32 sous les 3 f64) |
| UT2004, fenêtre | 42,7 | **40,5** | −5,1 % |
| Marble Blast, fenêtre | 10,0 | **9,3** | −7,0 % |
| Zen, fenêtre | 5,1 | **4,8** | −5,9 % |
| DOOM 3, plein écran | 92,8 | **89,0** | −4,1 % |
| UT2004, plein écran | 46,6 | **40,8** | −12,4 % |

Dispersion 0,7-5,9 % par bras. Partie DOOM 3 vérifiée sur f32 (`x-vfp-native-verify`,
`x-vmx-verify`, `x-ret-verify`, `x-tb-verify`, 202,7 ms/image) : 2 094 757 866 opérations VFP,
2 324 819 440 permutations et 159 726 832 rangements VMX, 376 193 171 lectures de la base de
temps, **0 divergence** ; `x-ret-verify` : 8,1 G blocs en ligne, 0 divergence, et **un** cas
« sans traduction » au démarrage (`pc 0x5605d88`, BootX), la classe connue du §35 de `tcg-g4.md`.

**Verdict : le G4 sur la cible 32 bits gagne sur les quatre jeux, en fenêtre comme en plein
écran.** Fusionné éteint (`PPC32MT=1`). Pour le défaut sur Linux x86-64 : reconstruire la
référence `~/src/qemu` avec 0049-0050 (VM quotidienne arrêtée), entraîner `build-fast` sur les
jeux (`pgo-train.sh --banc matrice --cible ppc32mt`), puis `PPC32MT` à 1 par défaut ; parité
arm64 sur le M4 (construction, `atomtest`, preuves du §30.5) avant de l'allumer là-bas.
