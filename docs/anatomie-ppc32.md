# Anatomie du code x86 émis pour le G4 sur la cible 32 bits (MTTCG), 23/10/2026

PC Linux x86-64 (i7-10700F, Skylake), QEMU 11.1.2 + série `tcg/0001-0050`. Objectif de fond :
DOOM 3 à 50 ms/image (87,6 depuis `PPC32MT=1`, `docs/ppc32-mttcg.md` §13). Le code généré fait
~84 % du temps vCPU de DOOM 3, sans point chaud (`docs/vitesse-doom3-x86.md` §14) ; l'ancienne
anatomie (`docs/plan-traducteur-rapide.md` §8.2) était faite sur `qemu-system-ppc64`, avant
0045-0050, et pondérée par le temps. Elle est refaite ici sur la cible de production
(`qemu-system-ppc` en MTTCG, SMP=2), pondérée par les **instructions hôte réellement
retirées**, poste par poste et classe par classe, avec la lecture du code émis réel.

Résumé :

- t2bench (gltest `d3inter`, le tiers « plugin » d'une image de DOOM 3) : **11,5 instructions
  x86 par instruction invité**, **54 par bloc exécuté**, 4,7 instructions invité par bloc ;
  le code généré fait 55 % des instructions du processus (helpers 26 %, pilote NVIDIA 13 %).
  `mtbench` (calcul entier, noyau, gcc) : 10,2 / 58,6 / 5,7, code généré 55 %.
- Postes (t2bench, part des instructions du code généré) : **TLB 21,9 %** (9 instructions
  par accès, toujours), sauts et sorties 18,7 % (dont 6,8 % de recherche en ligne du cache de
  sauts aux `blr`/`bctr`, 38 instructions chacune), rangements/lectures de `env` 14,8 %,
  **tête de bloc 9,4 %** (5 instructions par bloc), calcul du CR 8,8 %, adresse effective
  3,5 %, accès eux-mêmes 2,4 %, **inversion d'octets 0** (tout est en `movbe`), calcul 13,9 %.
- Un seul levier dépasse 3 % des instructions du code généré tout en restant prouvable : **la
  tête de bloc** (−7,5 % sur t2bench, −5,5 % sur mtbench). Fait : `tcg/0051` (`x-tb-head`,
  éteint par défaut, `TBHEAD=3` dans `run_tiger.sh`), §6. Les autres sont chiffrés au §5,
  tous sous 3 %.
- `tcg/0051` prouvé (épreuve nue et mutants, vérificateur dédié : 27,7 M E/S, 0 divergence ;
  suite, `plugmd5`, `gpu`, `atomtest`, vérificateurs, empreintes identiques) et mesuré en
  SMP=2 (4 contre 4 démarrages entrelacés) : **−5,0 % / −3,1 % d'instructions hôte** du
  processus (t2bench / mtbench, comme prévu), **−2,0 % / −2,2 % de cycles**, mtbench −2,3 %
  de temps, t2bench inchangé. Modeste : sur DOOM 3, −1 à −2 % au mieux, à confirmer en jeu
  (commandes au §7, rien n'a tourné sur la VM quotidienne).

## 1. Conditions

- Binaire : `~/src/qemu-anat/build-mt/qemu-system-ppc` (copie de `~/src/qemu-ppc32mt`, série
  0001-0050, `-O3 -march=native` sans LTO ni PGO ; le code émis par TCG ne dépend pas de la
  façon dont QEMU est compilé). `~/src/qemu`, son `build-fast` et la VM quotidienne n'ont pas
  été touchés.
- Options : celles de `run_tiger.sh` en `PPC32MT=1` sur Linux x86-64 (`-cpu g4,x-fast-fp,…,
  x-pteg-map` ; `-accel tcg,thread=multi,x-jit-near,x-jit-rel32,x-jc-bits=14,x-jc-word,
  x-jc-2way`), SMP=2, MTTCG — la ligne exacte est dans `tools/tcg/ppc32mt-bench.sh`.
- VM de dev : `disks/tiger-dev-anat.raw` (copie de `tiger-dev-ppc32.raw`, 10.4.11,
  single-user), `taskset -c 6,7,14,15`, la VM quotidienne tournant à côté (les comptes
  d'instructions n'en dépendent pas ; les temps, si).
- Charges : `t2bench` (gltest `d3inter`, plugin + GLEngine + kext, 150 images, 2 passes,
  image `7ff741d6…`) — analysé sur ses **22 dernières secondes** (les passes, sans la
  compilation du kext et du plugin) ; `mtbench` (regbench, 4 `gcc -O2`, vfptest, lmwtest,
  `par2`, atomtest) — en entier : c'est le « banc entier ».

## 2. Méthode

`tools/tcg/codeanat32.py JOURNAL PERF.DATA` (nouveau ; `codeanat.py` reste pour l'ancien
format) :

1. le journal `-d in_asm,out_asm` du démarrage (OBJD-T/OBJD-H : QEMU sans capstone ; les
   adresses hôte sont imprimées par `disas()`) donne, pour chaque bloc, le code x86 de chaque
   instruction invité, désassemblé par capstone. Le bloc IN d'un bloc OUT est apparié **par
   traduction** (dernier IN au même pc avant le OUT) : deux processus de l'invité ont du code
   différent aux mêmes adresses, et deux vCPU entrelacent leurs journaux ;
2. `perf record -e instructions:upp,cycles:upp` (PEBS) pendant le job
   (`PERFREC=… tools/tcg/ppc32mt-bench.sh`) ; chaque échantillon est rangé dans l'instruction
   x86 exacte qui l'a pris ;
3. **répartition uniforme dans le bloc** : les exécutions d'un bloc sont estimées par
   (échantillons du bloc) / (longueur de son chemin exécuté, sans chemins lents ni talons de
   sortie non chaînés), puis chaque instruction du chemin compte une fois par exécution. Le
   glissement et les biais d'attribution de PEBS (qui donnent par exemple 6,5 % des
   échantillons à la première instruction d'un bloc et presque rien aux deux suivantes)
   disparaissent ; seuls restent ceux d'un bloc à l'autre. Les tableaux « par IP » sont
   aussi dans les relevés ;
4. `perf record -b` (LBR) : les branchements pris qui arrivent au début d'un bloc, rangés
   selon l'instruction de départ (`jmp rel32` d'un `goto_tb` vers un pc plus haut ou plus
   bas, `jmp reg` d'une recherche en ligne, boucle principale) — `--lbr`.

Postes, par instruction x86 (détail dans l'en-tête de l'outil) : `entete` (vérification
d'interruption et stores de `can_do_io`), `ae` (adresse effective), `tlb` (chemin rapide du
TLB, `jne` compris), `acces`, `bswap` (en plus de l'accès), `env-ld`/`env-st` (registres
invité dans `CPUPPCState`, détaillés par registre), `cr`, `xer`, `bc-test`, `saut` (sorties
directes), `sortie-ind` (recherche en ligne du cache de sauts), `appel`, `barriere`, `neg`,
`calcul`, `lent`.

Commande (un démarrage de la VM de dev, ~7 min ; le journal fait ~650 Mo) :

    TAG=anat PERFREC=instructions:upp,cycles:upp PERFPER=1000003 \
      QEXTRA="-d in_asm,out_asm -D $PWD/bench/ppc32mt/anat/asm.log" \
      DEVDISK=disks/tiger-dev-anat.raw QEMU_BIN=~/src/qemu-anat/build-mt/qemu-system-ppc \
      SMP=2 PPC32MT=1 taskset -c 6,7,14,15 tools/tcg/ppc32mt-bench.sh anat t2bench mtbench
    python3 tools/tcg/codeanat32.py bench/ppc32mt/anat/asm.log bench/ppc32mt/anat/t2bench.data \
      --last 22 --top 25 --dump-top 40
    # entrées de bloc : PERFREC=cycles:u PERFOPT=-b, puis --event cycles --lbr

Relevés (non versionnés) : `bench/ppc32mt/anat-p32-pebs/` (PEBS : `t2bench-anat.txt`,
`mtbench-anat.txt`, code des 40 blocs les plus lourds), `anat-p32-lbr/` (LBR),
`anat-p32/` (premier essai sans PEBS, glissement visible).

Limites : le chemin exécuté d'un bloc compte les deux `nop ; jmp` d'un `bc` (un seul est
pris : +1 instruction par `bc`) ; le classement par poste est fait sur des motifs (la
séquence du TLB commence à `mov rdi, reg ; shr`, etc.) vérifiés à la main sur les blocs les
plus lourds (§4), mais le code des helpers en ligne (`lmw`/`stmw` vectoriels, `dcbz`) est
rangé surtout en `calcul`.

## 3. Résultats

### 3.1 Vue d'ensemble

| | t2bench (passes) | mtbench |
|---|---|---|
| code généré, part des instructions du processus | 55,1 % | 55,5 % |
| … helpers et reste de QEMU / pilote GL / libc | 26,4 / 12,6+3,4 / 1,9 % | 43,4 / — / 0,6 % |
| instructions x86 par instruction invité | **11,5** | **10,2** |
| instructions x86 par bloc exécuté | 54,0 | 58,6 |
| instructions invité par bloc exécuté | 4,7 | 5,7 |

(L'ancien « ~22 par instruction invité » de DOOM 3 était pondéré par le temps, sur ppc64 et
sur des blocs plutôt flottants : les deux chiffres ne se comparent pas.)

### 3.2 Postes (part des instructions du code généré ; par instruction invité)

| poste | t2bench | | mtbench | |
|---|---|---|---|---|
| tête de bloc (`entete`) | **9,4 %** | 1,08 | **8,7 %** | 0,89 |
| … dont vérification d'interruption (3 instr. par bloc) | 5,6 % | | 5,1 % | |
| … dont stores de `can_do_io` (2 par bloc) | 3,9 % | | 3,6 % | |
| adresse effective (`ae`) | 3,5 % | 0,40 | 3,2 % | 0,33 |
| chemin rapide du TLB | **21,9 %** | 2,52 | **21,7 %** | 2,22 |
| accès à la mémoire de l'invité | 2,4 % | 0,28 | 2,4 % | 0,25 |
| inversion d'octets en plus de l'accès | 0,0 % | 0,00 | 0,0 % | 0,00 |
| lectures de `env` | 6,5 % | 0,75 | 6,2 % | 0,63 |
| rangements dans `env` | 8,3 % | 0,95 | 9,2 % | 0,94 |
| calcul du CR | 8,8 % | 1,01 | 5,0 % | 0,51 |
| calcul de XER[CA] | 0,3 % | 0,04 | 0,8 % | 0,08 |
| test de condition des `bc` | 2,4 % | 0,27 | 1,5 % | 0,16 |
| sorties directes (`goto_tb`, `exit_tb`) | 11,9 % | 1,36 | 9,5 % | 0,97 |
| sorties indirectes (recherche en ligne) | 6,8 % | 0,78 | 11,2 % | 1,14 |
| appels de helpers | 1,6 % | 0,18 | 2,2 % | 0,22 |
| autres accès à `CPUState` (`neg`) | 2,2 % | 0,26 | 1,9 % | 0,19 |
| calcul (le travail de l'instruction) | 13,9 % | 1,59 | 16,3 % | 1,66 |

Registres de `env` (t2bench) : rangements de GPR 4,7 %, lectures de GPR 3,9 %, rangements du
CR 1,2 %, **rangement d'`access_type` 1,1 %** (un par bloc qui charge, §5.6), lecture de
XER[SO] 1,0 % (une par `cmp`), `nip` 0,4 %, `hflags` 0,4 % (recherche en ligne), le reste
< 0,4 %.

### 3.3 Par classe d'instruction invité (t2bench ; instructions x86 par instruction invité)

| classe | part des instr. invité | part du code généré | x86/invité | dont |
|---|---|---|---|---|
| entier simple | 30,8 % | 9,0 % | **3,4** | calcul 1,1, env 1,4, tête 0,9 |
| chargement | 16,3 % | 20,6 % | 14,5 | **TLB 9,0**, accès 1,0, ae 0,8, env 2,1, tête 1,6 |
| `bc` | 15,0 % | 11,4 % | 8,7 | sorties 5,9, test 1,8 |
| `cmp` | 9,5 % | 11,1 % | **13,5** | **CR 8,9**, env 2,5, tête 2,1 |
| rangement | 7,1 % | 7,8 % | 12,5 | **TLB 9,0**, accès 1,0, ae 1,0 |
| `mfspr`/`mtspr`/`mfcr`/cr-logique | 6,0 % | 2,7 % | 5,3 | |
| `b` | 3,3 % | 4,2 % | 14,6 | sortie (souvent hors page : recherche en ligne) 9,7 |
| forme « . » (`and.`, `rlwinm.`…) | 2,7 % | 3,5 % | 14,8 | CR 6,0 |
| `blr`/`bctr` | 2,1 % | 7,9 % | **43,7** | **recherche en ligne 37,6** |
| flottant (ld/st) / VMX (ld/st) | 1,6 / 1,4 % | 3,8 / 2,0 % | 26,5 / 16,2 | TLB 9,8 / 9,0 |
| `lmw`/`stmw` | 1,3 % | 11,7 % | 103,6 | copie vectorielle en ligne (0025/0039) |
| flottant | 0,5 % | 2,0 % | 43,2 | |
| `addc`/`adde`/`srawi`… | 0,8 % | 0,5 % | 7,0 | XER[CA] 4,4 |

mtbench a la même forme (chargement 13,7 ; `cmp` 13,7 dont CR 8,9 ; `blr`/`bctr` 43,6 dont
37,4 ; entier 2,6), avec plus d'entier pur (40 % des instructions invité) et de sorties
indirectes (13 % du code généré).

### 3.4 Entrées de bloc (LBR)

| entrée | t2bench | mtbench |
|---|---|---|
| `goto_tb` vers un pc **plus haut** | **64,7 %** | **37,4 %** |
| `goto_tb` vers un pc plus bas ou égal (boucles) | 12,8 % | 32,9 % |
| saut indirect après recherche en ligne, `blr`/`bctr` | 8,6 % | 17,0 % |
| saut indirect après recherche en ligne, `b`/`bl` hors page | 8,0 % | 8,0 % |
| boucle principale (`cpu_exec`), helper | 5,9 % | 4,6 % |

(406 612 et 3 248 147 branchements vers un début de bloc.)

## 4. Le code émis réel, et ce qu'il a en trop

### 4.1 Chargement (`lwz r7,0(r3)`, bloc 0x90131280 de t2bench, le plus lourd)

    mov  ebx, [rbp+0xc]          ; env-ld : r3 (première lecture dans le bloc)
    mov  rdi, rbx                ; TLB 1
    shr  rdi, 7                  ; TLB 2   index = adresse >> (12 − 5)
    and  edi, [rbp-0x40]         ; TLB 3   masque (taille dynamique du TLB)
    add  rdi, [rbp-0x38]         ; TLB 4   table
    lea  esi, [rbx+3]            ; TLB 5   dernier octet : accès non aligné à cheval
    and  esi, 0xfffff000         ; TLB 6
    cmp  esi, [rdi]              ; TLB 7   comparateur addr_read
    jne  lent                    ; TLB 8
    mov  rdi, [rdi+0x18]         ; TLB 9   addend
    movbe r12d, [rbx+rdi]        ; accès ET inversion d'octets
    mov  [rbp+0x1c], r12d        ; env-st : r7, parce que l'accès suivant peut fauter

Minimal pour un accès natif : 1 instruction. Ici 12 : **9 pour le TLB** (sur 9 à 10 pour
n'importe quel TLB logiciel à taille variable : le `lea`/`and` du dernier octet est le prix
des accès non alignés permis par PowerPC ; un TLB de taille fixe changerait `and edi,[mem]` en
`and edi,imm`, même nombre d'instructions), **0 pour l'inversion** (`movbe` direct, Skylake ;
seuls `lhz` et `lha` ajoutent un `movzx`/`movsx` après un `movbe` 16 bits, 0,0 %), 1 lecture
de `env` (premier usage de r3 dans le bloc ; les blocs font 4,7 instructions) et 1 rangement
dans `env` : TCG range tout global modifié avant chaque accès qui peut fauter
(`TCG_OPF_SIDE_EFFECTS`). Seuls 0,46 % du code (t2bench, 1,66 % sur mtbench) sont des
rangements **redondants** (le même registre rangé de nouveau plus loin dans le bloc sans
appel ni sortie entre les deux) : les autres seraient seulement déplacés à la fin du bloc.

`stw` : même chose, comparateur `addr_write` (`cmp esi,[rdi+8]`), `movbe [r+rdi], reg`.
L'adresse effective `rA+d` est un `add`/`lea` (0,4 par instruction invité), sans `ext32u`
(la cible 32 bits ne le demande plus, `docs/ppc32-mttcg.md` §1).

### 4.2 `cmplwi` + `bc` (bloc 0xa0d80)

    mov  ebx, 8                  ; CR 1   constante LT
    mov  r13d, 2                 ; CR 2   constante EQ
    mov  r14d, r13d              ; CR 3   copie (la sortie du movcond doit être la 2e valeur)
    cmp  r12d, 0x10              ; CR 4
    cmovb r14d, ebx              ; CR 5
    mov  r13d, 4                 ; CR 6   constante GT
    cmp  r12d, 0x10              ; CR 7   la MÊME comparaison, refaite
    cmova r14d, r13d             ; CR 8
    mov  r15d, [rbp+0x12c]       ; env-ld : XER[SO]
    or   r15d, r14d              ; CR 9
    mov  [rbp+0x108], r15d       ; env-st : cr0
    …                            ; trois instructions entières
    mov  byte [rbp-0x10], 1      ; tête : can_do_io = 1 avant la dernière instruction
    and  r15d, ebx               ; bc-test (ebx vaut encore 8)
    test r15d, r15d              ; bc-test
    jne  …                       ; sortie, puis nop ; jmp (goto_tb) ×2

`gen_op_cmp` (`target/ppc/translate.c`) fait deux `movcond` : x86 n'a pas de `cmov` à
immédiat (trois constantes en registre), chaque `movcond` refait son `cmp`, et la première
sortie est une copie. Minimal exact : `mov t,2 ; mov a,8 ; mov b,4 ; cmp ; cmovl t,a ;
cmovg t,b ; or t,so ; mov [crf],t` = 8 (11 aujourd'hui avec la lecture de SO et le
rangement). Le `bc` teste le bit par `and`+`test` (l'optimiseur ne fusionne pas
`brcond(and(x,c),0)` en `TSTNE`) au lieu d'un `test r15d,8`. Un `cmp`+`jcc` fusionné ne
gagnerait rien de plus : le champ du CR doit être rangé de toute façon (il est visible), et
`test r,imm ; jcc` fait déjà deux instructions. Écart total : 2 à 3 instructions par
`cmp`+`bc` sur ~15.

### 4.3 Tête de bloc (tous les blocs)

    mov  ebx, [rbp-0x14]         ; icount_decr.u32 (demande de sortie, interruption)
    test ebx, ebx
    jl   sortie                  ; vers « exit_tb TB_EXIT_REQUESTED »
    mov  byte [rbp-0x10], 0      ; can_do_io = 0 (bloc de plus d'une instruction)
    …
    mov  byte [rbp-0x10], 1      ; can_do_io = 1 avant la dernière instruction

5 instructions par bloc, 9,4 % du code généré à 4,7 instructions invité par bloc. Les deux
stores ne servent qu'à `io_prepare()` (`accel/tcg/cputlb.c`) : une E/S faite par une
instruction qui n'est pas la dernière de son bloc fait recompiler le bloc pour qu'elle le
devienne (amont, depuis 8.2, même sans icount). La vérification d'interruption, elle, est
faite à **chaque** entrée de bloc, y compris quand un `goto_tb` enchaîne vers un bloc plus
loin dans la même page (65 % des entrées sur t2bench).

### 4.4 Sortie indirecte (`blr`, bloc 0x901311cc)

38 instructions : `lr & ~3`, rangement de `nip`, lecture de `hflags`, **12 pour le hachage**
(`tb_jmp_cache_hash_func` avec `x-jc-word`, refait en ops TCG), 2 déversements sur la pile
(`[rsp+0x80]` : les temporaires vivent à travers des `brcond`), puis 4 comparaisons
(entrée vide, pc, drapeaux, `cflags`), la liste des points d'arrêt, `can_do_io`, et
`jmp [tb->tc.ptr]`. 6,8 % du code généré sur t2bench, 11,2 % sur mtbench.

## 5. Leviers chiffrés

Gain attendu en part des instructions du code généré (t2bench / mtbench) ; le code généré
fait ~55 % des instructions du processus sur ces bancs, ~84 % du temps vCPU sur DOOM 3.

| levier | mécanisme | t2bench | mtbench | prouvable | état |
|---|---|---|---|---|---|
| **tête de bloc** (A) | plus de stores de `can_do_io` : la valeur est déduite de l'adresse de retour dans `io_prepare` | **−3,9 %** | **−3,6 %** | oui, exact (vérificateur dédié) | **fait, `tcg/0051`** |
| **tête de bloc** (B) | `goto_tb` vers un pc plus haut chaîné après la vérification d'interruption | **−3,6 %** (65 % × 5,6 %) | **−1,9 %** (37 % × 5,1 %) | oui (épreuve nue, mutant) ; interruptions vues au plus une chaîne avant plus tard | **fait, `tcg/0051`** |
| A + B | | **−7,5 %** | **−5,5 %** | | |
| TLB même page | accès à la même base non modifiée, même page, même genre : comparaison de page au lieu des 9 instructions | ≤ −2,5 % (3,7 % des instructions du TLB sont candidates, ~2/3 gagnés) | ≤ −2,9 % | difficile (addend gardé dans un registre réservé, chemin lent qui l'écrase, MMIO, `TLB_NOTDIRTY`) | non |
| recherche en ligne allégée | hachage simple, `cflags` et points d'arrêt hors du chemin chaud, sans déversement | ~−2,4 % (≈ 13 sur 38) | ~−4 % | oui (x-ret-verify) mais touche le hachage partagé avec le C (`tb_jmp_cache_hash_func`) et 0008/0011/0026/0043 | non (≥ 3 % sur mtbench seul ; voir §8) |
| `cmp` / `bc` | `movi` + `movcond` en place (−1 par `cmp`), `brcond TSTNE` (−1 par `bc` sur le CR) | −2,1 % | −1,3 % | oui (équivalence) | non |
| `access_type` paresseux | ne plus ranger `env->access_type` (servi seulement aux segments « direct-store » T=1) | −1,1 % | −0,9 % | moyen (décodage de l'instruction fautive au défaut) | non |
| rangements différés | rangements de globaux dans le chemin lent des accès au lieu d'avant l'accès | −0,5 % | −1,7 % | difficile (vivacité, allocateur) | non |
| tête en `cmp [mem],0 ; jl` | −1 instruction par bloc non sauté | −1,9 % | −1,7 % | trivial, mais **même nombre d'uops** (pas de fusion mémoire+immédiat) | non |
| inversion d'octets | — | 0 | 0 | | déjà `movbe` |

Le seul poste qui dépasse 3 % et reste exact et localisé est la tête de bloc : c'est aussi
la seule instruction de TCG que **tout** bloc exécute, quel que soit son contenu ; sur DOOM 3,
dont les blocs sont plus longs et plus flottants, la part sera plus petite (à mesurer en jeu,
§7). Le TLB (22 %) n'a pas de levier local : 9 instructions par accès est le minimum d'un
TLB logiciel ; seule une région compilée qui sort les vérifications des boucles le réduirait
(`docs/plan-traducteur-rapide.md` §8.3).

## 6. `tcg/0051` : `x-tb-head` (tête de bloc allégée)

### 6.1 Conception

Propriété de l'accélérateur `-accel tcg,x-tb-head=N` (0 à 7, fixée avant le premier CPU,
forcée à 0 sous icount), **éteinte par défaut** ; lanceur `TBHEAD=N` (`run_tiger.sh`, sondé
par `qemu_tcg_has_prop`), capacité `x-tb-head` dans `scripts/build_qemu_qfb.sh`. Code
commun seulement (`accel/tcg/translator.c`, `translate-all.c`, `cpu-exec.c`, `cputlb.c`,
`tcg-all.c`, `tcg/tcg.c`, deux en-têtes) : **aucun émetteur touché**, le même patch vaut pour
arm64.

- **bit 1 (`TB_HEAD_CDIO`)** : `set_can_do_io()` n'émet plus rien. `io_prepare()` appelle
  `cpu_io_last_insn(cpu, retaddr)` : bloc par `tcg_tb_lookup(retaddr)`, puis « dernière
  instruction » si `retaddr − GETPC_ADJ − tc.ptr ≥ tb->last_insn_off` (nouveau champ :
  début du code de la dernière instruction, `gen_insn_end_off[icount−2]`, 0 pour un bloc
  d'une instruction) — exactement la valeur que les deux stores auraient laissée (`false`
  avant la première instruction, `true` à partir de la fin de l'avant-dernière, `true` hors de
  tout bloc). Coût : une recherche d'arbre par E/S (MMIO), contre deux stores par bloc.
- **bit 2 (`TB_HEAD_FWD`)** : `gen_tb_start` note le `brcond` de la vérification ;
  `tcg_gen_code` relève l'adresse qui le suit (fin d'un bloc de base TCG : rien n'est vivant
  dans les registres, comme à `tc.ptr`) dans `tb->head_skip` ; `tb_add_jump` chaîne un
  `goto_tb` vers `tc.ptr + head_skip` quand le bloc visé a un pc **strictement plus haut**
  que le bloc source. Tout cycle de blocs chaînés contient une arête vers un pc plus bas ou
  égal, qui vérifie ; les entrées indirectes (recherche en ligne, `lookup_and_goto_ptr`) et la
  boucle principale entrent à `tc.ptr`. Une interruption ou une demande de sortie est donc
  vue au plus une chaîne de blocs « vers l'avant » plus tard (bornée par le nombre de blocs
  de pc croissants) ; les interruptions externes et du décrémenteur sont asynchrones, l'amont
  les prend déjà seulement aux frontières de bloc.
- **bit 4 (`TB_HEAD_VERIFY`, épreuve)** : les stores restent ; à chaque E/S, la valeur
  déduite est comparée à la valeur rangée (bilan « tb-head : … vérification : N identiques,
  M DIVERGENCES » à la sortie, les 8 premières divergences détaillées).
- Bilan à la sortie dès que la propriété est posée : E/S par la dernière instruction / au
  milieu (recompilées) / hors bloc, chaînages vers l'avant / l'arrière.
- Mutants (`POMPPC_TBHEAD_MUTANT`) : 1 = toute arête chaînée après la vérification ; 2 = la
  règle de `can_do_io` à l'envers.

### 6.2 Preuves

**Épreuve nue `tools/tcg/tbhproof.sh QEMU_BIN`** (`tools/tcg/tbhproof.S`, mac99 sans
OpenBIOS, mode réel, 1 vCPU, en `thread=single` et `thread=multi`) : boucle sur elle-même
(`b .`), cycle A → B → C → A (deux arêtes vers l'avant, une vers l'arrière) et lectures
d'E/S (UniNorth) au milieu et en fin de bloc, chacun pendant 100 interruptions du
décrémenteur.

| passe | single | multi |
|---|---|---|
| ref (`x-tb-head=0`) | finit, 300 interruptions | finit, 300 |
| tete (`=3`) | finit, 300 ; 16 chaînages vers l'avant, 4 vers l'arrière ; 315 + 315 E/S (milieu → recompilées, puis dernière) | finit, 300 ; idem |
| verif (`=7`) | finit ; **1 116 E/S comparées, 0 divergence** | 1 164, 0 |
| mutant 1 (toute arête après la vérification) | — | **ne finit jamais** (boucle 1 sourde au décrémenteur) : attrapé |
| mutant 2 (règle de `can_do_io` à l'envers, sous `=5`) | — | **1 366 divergences** : attrapé |

Verdict : **CONFORME**. (Sur le cycle A → B → C, `=3` fait ~1,8 fois plus de tours que
`=0` entre deux interruptions : la boucle est faite presque seulement de têtes de bloc.)

**OpenBIOS SMP** (2 vCPU, MTTCG, options de production, 10 s) : `x-tb-head=5`
(vérification) **8 879 238 E/S comparées, 0 divergence** ; mutant 2 : 8 692 398 divergences.

**Tiger, VM de dev, SMP=2 MTTCG, options de production** (`bench/ppc32mt/tbh-*`, au niveau
de `docs/tcg-g4.md` §30.5 et `docs/ppc32-mttcg.md` §4.1) :

| épreuve | démarrage vérifié : `x-tb-head=7` + `x-vfp-native-verify`, `x-vmx-verify`, `x-ret-verify`, `x-tb-verify` | démarrage `x-tb-head=3` |
|---|---|---|
| `vfptest` / `vmxtest` / `lmwtest` | `bc13e93c39e62602` / `c24e3d31d479381b` (48 fautes sur 48, page intacte) / `2575eafce78adf66` | identiques |
| `smctest` A-F | `7308784105b8fd59`, 0 erreur | identique |
| `tbtest` / `jctest` | 0 recul / OK | 0 recul / OK |
| `atomtest` (2 fils × 4 000 000) | A, B, C exacts | exacts |
| `plugmd5` | **69 scènes, 0 écart** | 69, 0 écart |
| `gpu` | 0 échec | 0 échec |
| `t2bench` | image `7ff741d6…` (3 passes) | `7ff741d6…` |
| **`can_do_io` déduit contre rangé** | **27 717 886 E/S comparées, 0 divergence** (13 858 943 au milieu d'un bloc, recompilées, autant par la dernière instruction) | — |
| chaînages | 565 893 vers l'avant (après la vérification), 54 310 vers l'arrière | |
| `x-vfp-native-verify` | 34 693 754 opérations, 0 divergence | |
| `x-vmx-verify` | 302 103 961 + 50 396 437, 0 + 0 | |
| `x-ret-verify` | 8,83 G en ligne + 302 M en boucle + 216 M par le helper ; **14 « sans traduction », 0 autre**, aux mêmes 6 `pc` que le témoin du §4.1 de `docs/ppc32-mttcg.md` (BootX 0x5601cb0, 0x5610664, 0x561171c ; OpenBIOS 0xfff0d044, 0xfff16f0c, 0xfff1ccb4 : la classe du §35 de `docs/tcg-g4.md`) ; 175 courses | |
| `x-tb-verify` | 383 817 986 lectures, 0 positif | |

**Bureau** (`tools/endurance/devdemarrages.py --nom tbh3-bureau -n 8 --froid-tous 4`,
`x-tb-head=3`, SMP=2 MTTCG, options de production) : **8 démarrages sur 8** (2 à froid, 6 par
`shutdown -r`) jusqu'au bureau complet (WindowServer, Finder, Dock, SystemUIServer),
`hw.ncpu` = 2, « Processors active: 0 1 » ; bureau à 124 s à froid, 77 s après un
redémarrage (comme en `x-tb-head=0`, `docs/ppc32-mttcg.md` §10.2).

Les empreintes sont celles du témoin `x-tb-head=0` (`docs/ppc32-mttcg.md` §4.1 ; `plugmd5`
compte 69 scènes depuis l'ajout de scènes au job). En passant : la VM de dev fait ~14 millions
d'E/S **au milieu d'un bloc** par démarrage (chacune fait sortir et recompiler un bloc d'une
instruction, comportement amont) : presque toutes sont des lectures PIO du registre de données
IDE — une piste à part (§8).

### 6.3 Mesure : A/B entrelacé sur la VM de dev, SMP=2

Même binaire (`~/src/qemu-anat/build-mt`), mêmes options de production, seule
`x-tb-head` change ; `tools/tcg/ppc32mt-bench.sh … mtbench t2bench` (`perf stat` sur tout le
processus QEMU), `taskset -c 6,7,14,15`, la VM quotidienne tournant à côté. Ordre
`h0 h3 h1 | h1 h3 h0 | h3 h0 | h0 h3` (23/10, 22:55 → 23:55) ; tous les démarrages ont leur
tampon de JIT dans la même fenêtre de 4 Gio que le texte ; images de t2bench identiques
(`7ff741d6…`), `atomtest` exact partout.

| SMP=2, médianes | h0 (`=0`, 4 démarrages) | **h3 (`=3`, 4)** | h1 (`=1` : `can_do_io` seul, 2) |
|---|---|---|---|
| `mtbench`, instructions hôte | 1 876 G | **1 817 G (−3,1 %)** | 1 833 G (−2,3 %) |
| `mtbench`, cycles hôte | 871 G | **851 G (−2,2 %)** | 873 G (+0,2 %) |
| `mtbench`, somme des noyaux | 42 304 ms | **41 343 ms (−2,3 %)** | 42 211 ms (−0,2 %) |
| … `call` / `sys` / `ctx` / `gcc` / `vfp` | 439 / 2 634 / 2 248 / 4 923 / 12 047 | 418 / 2 550 / 2 130 / 4 765 / 11 480 (−4,7 / −3,2 / −5,3 / −3,2 / −4,7 %) | |
| `t2bench`, instructions hôte | 805 G | **764 G (−5,0 %)** | 783 G (−2,7 %) |
| `t2bench`, cycles hôte | 408 G | **400 G (−2,0 %)** | 406 G (−0,6 %) |
| `t2bench`, temps (médiane des passes) | 8,05 s | 8,03 s (−0,3 %) | 8,02 s |

Détail par démarrage : cycles de `mtbench` h0 890 / 866 / 875 / 865 G, h3 876 / 850 / 848 /
852 G ; de `t2bench` h0 426 / 408 / 404 / 409 G, h3 408 / 401 / 397 / 399 G.

Lecture :

- Les instructions baissent **exactement comme prévu** par l'anatomie (t2bench : −7,5 % du
  code généré × 55 % = −4,1 % prévu, −5,0 % mesuré ; mtbench : −5,5 % × 55,5 % = −3,1 %
  prévu, −3,1 % mesuré).
- Les cycles baissent moins (−2 %) : les deux stores de `can_do_io` (bit 1) ne coûtaient
  presque rien en temps (h1 : −2,5 % d'instructions, cycles inchangés) ; le gain de temps vient
  du saut de la vérification aux entrées vers l'avant (bit 2), qui raccourcit la chaîne
  dépendante d'entrée de bloc.
- Le temps de t2bench ne bouge pas (8,0 s dans les trois bras, à ±4 % d'un démarrage à
  l'autre) : ses passes attendent aussi le GPU et le fil du plugin ; `mtbench` (calcul) gagne
  2,3 %.
- Gain attendu sur DOOM 3 : −1 à −2 % de temps d'image au mieux (le code généré y fait 84 %
  du vCPU, mais avec des blocs plus longs) ; **à confirmer en jeu** (§7) — en dessous du bruit
  d'une partie si c'est −1 %.

## 7. A/B en jeu (pour la session principale, VM quotidienne)

**Rien n'a été lancé sur la VM quotidienne** : à faire avec l'accord de l'utilisateur, hôte au
repos. `tools/tcg/matab.sh` lance le `run_tiger.sh` du dépôt principal, qui ne connaît pas
`TBHEAD` tant que cette branche n'est pas fusionnée ; mais il transmet `TCG_OPTS` tel quel à
l'accélérateur, et `PPC32MT=1` y est déjà le défaut sur Linux x86-64 : les deux bras prennent
**le même binaire** (`~/src/qemu-anat/build-mt`, série 0001-0051, `-O3 -march=native` sans
LTO ni PGO) et ne diffèrent que par la propriété.

    B=$HOME/src/qemu-anat/build-mt/qemu-system-ppc
    cd ~/src/pomppc
    # 1. fenêtre, 4 jeux, 3 parties par bras et par jeu, entrelacées
    for j in d3 ut mb zen; do
      MATAB_AVANT="python3 tools/tcg/chargehote.py attendre bench/tcg/ab/tbh-$j/charge-releve.txt" \
      tools/tcg/matab.sh tbh-$j 3 "h0:QEMU_BIN=$B TCG_OPTS=x-tb-head=0" \
                                  "h3:QEMU_BIN=$B TCG_OPTS=x-tb-head=3" $j fen
    done
    for j in d3 ut mb zen; do python3 tools/tcg/matabsum.py bench/tcg/ab/tbh-$j; done
    # 2. plein écran (règle du projet), au moins d3 et ut
    for j in d3 ut; do
      tools/tcg/matab.sh tbh-$j-pe 2 "h0:QEMU_BIN=$B TCG_OPTS=x-tb-head=0" \
                                     "h3:QEMU_BIN=$B TCG_OPTS=x-tb-head=3" $j pe
    done
    # 3. une partie DOOM 3 vérifiée (pas pour la vitesse) : bilan « tb-head : … 0 DIVERGENCES »
    #    et les vérificateurs habituels dans run_tiger.log
    tools/tcg/matab.sh tbh-d3-verif 1 \
      "v:QEMU_BIN=$B TCG_OPTS=x-tb-head=7 CPU_OPTS=x-vfp-native-verify=on,x-vmx-verify=on,x-ret-verify=on,x-tb-verify=on" d3 fen
    tools/tcg/matab.sh --restore

Contrôles dans chaque `run_tiger.log` : le chemin `…/qemu-anat/build-mt/qemu-system-ppc`, la
ligne `tb-head: x-tb-head=3 ; …` à la sortie (bras h3 seulement ; chaînages vers l'avant non
nuls), pas d'avertissement. Métrique : temps d'image (CPU de QEMU,
`docs/vitesse-doom3-x86.md`). Une régression d'image, un gel ou un son haché arrête tout.
Attendu sur DOOM 3 : moins que sur t2bench (blocs plus longs et plus flottants, la tête y
pèse moins) — de l'ordre de −1 à −3 % de temps d'image ; c'est la partie qui décide.

**Après fusion** (et reconstruction de `~/src/qemu`, voir §8) : `"h0:"` contre `"h3:TBHEAD=3"`
sur le binaire rapide reconstruit (`QEMU_FAST=1`), le profil PGO repris avec `TBHEAD=3`.

## 8. Parité arm64, fusion, suite

- **Parité arm64** (règle du projet) : le patch ne touche que du code commun (aucun
  émetteur) ; la tête de bloc arm64 est la même (`ldur w, [x19,#-0x14]` ; `tbnz`/`b.lt`), le
  `goto_tb` arm64 se repatche vers n'importe quelle adresse du tampon (branchement direct ou
  `ldr`+`br`), donc `tc.ptr + head_skip` marche aussi. À construire et prouver sur le M4 au
  même niveau : `tools/tcg/tbhproof.sh` (il faut l'assembleur `powerpc-linux-gnu-as` ; sinon
  assembler `tbhproof.S` sur le PC et copier `tbhproof.bin`), la suite, les vérificateurs avec
  `x-tb-head=7`, `plugmd5`, `atomtest`, puis l'A/B en jeu. Le défaut, s'il change, change sur
  les deux hôtes.
- **Fusion** : ajouter 0051 à la série change l'empreinte de la série
  (`pomppc_serie_hash`) : `scripts/qemu_fast.sh` écartera le `build-fast` actuel tant qu'il
  n'est pas reconstruit — fusionner seulement avec une reconstruction de `~/src/qemu` (`build/`
  et `build-fast/`, VM quotidienne arrêtée), comme pour 0049-0050. Le patch s'applique sur
  l'état 0001-0050 (`patch --dry-run` sur une copie de `~/src/qemu-ppc32mt`) ; la chaîne
  complète `scripts/build_qemu_qfb.sh` sur un arbre neuf n'a pas été rejouée.
- **Défaut** : seulement si les jeux confirment (§7) ; `TBHEAD=3` d'abord en option.
- **Pistes suivantes, par ordre de gain** :
  1. *E/S au milieu d'un bloc* : ~13-14 millions par démarrage de la VM de dev (démarrage +
     mtbench + t2bench), et presque **toutes** les E/S tombent au milieu d'un bloc (chacune
     fait sortir du bloc par `cpu_io_recompile` / `cpu_loop_exit_noexc`, puis exécuter un
     bloc d'une instruction). Trace `-trace enable=memory_region_ops_*` d'un démarrage +
     t2bench (`bench/ppc32mt/tbh-iotr/`) : 13,06 M accès, dont **12,04 M lectures 16 bits du
     registre de données IDE** (`pmac-ide`, 0x80020000 : le disque de la VM de dev est lu en
     **PIO**, 2 octets par accès), 0,39 M du registre d'état IDE, 0,13 M OpenPIC, 0,08 M
     fw_cfg. C'est propre à la VM de dev (single-user, agent de devloop) tant qu'on ne l'a
     pas vu en jeu ; s'il y apparaît (chargements de niveau), deux voies : faire passer le
     pilote ATA en DMA (dbdma), ou déclarer le registre de données IDE sans recompilation
     (son interruption de fin de transfert est asynchrone). Hors du code émis ; à mesurer en
     jeu avant tout (`TBHEAD=1` donne le compte à la sortie, ligne `tb-head:`) ;
  2. recherche en ligne allégée aux `blr`/`bctr` (§5 : ~13 instructions sur 38, −2,4 % à
     −4 %) ;
  3. `cmp`/`bc` (−1 à −2 %) et `access_type` paresseux (−1 %) ;
  4. le TLB (22 %) ne baisse qu'avec une région compilée (`docs/plan-traducteur-rapide.md`).

## 9. Fichiers et relevés

- `patches/tcg/0051-tcg-tb-head.patch` (appliqué après 0050 par `scripts/build_qemu_qfb.sh`,
  marqueurs vérifiés, capacité `x-tb-head`), `run_tiger.sh` (`TBHEAD=N`, éteint par défaut).
- `tools/tcg/codeanat32.py` (anatomie pondérée par PEBS, répartition dans le bloc, leviers,
  LBR), `tools/tcg/ppc32mt-bench.sh` (`PERFREC`, `PERFPER`, `PERFOPT` : `perf record`
  pendant chaque job, défauts inchangés), `tools/tcg/tbhproof.S` et `tbhproof.sh` (épreuve
  nue, mutants).
- Arbre QEMU : `~/src/qemu-anat` (copie de `~/src/qemu-ppc32mt` + 0051), `build-mt/`
  (`-O3 -march=native`) ; `~/src/qemu-anat-orig` = l'état 0001-0050, pour refaire le patch
  (`diff -u` fichier par fichier). Disque : `disks/tiger-dev-anat.raw`.
- Relevés (non versionnés, `bench/ppc32mt/` du worktree) : `anat-p32-pebs/`, `anat-p32-lbr/`,
  `anat-p32/` (anatomie) ; `tbh-verif/` (démarrage vérifié), `tbh-ref3/` (empreintes en
  `x-tb-head=3`), `tbh-ab-h{0,1,3}-*` (A/B), `tbh-io/` (piles LBR, faussées par les
  `longjmp`), `tbh-iotr/` (trace des E/S) ; `bench/endurance-dev/tbh3-bureau/` (bureau).
