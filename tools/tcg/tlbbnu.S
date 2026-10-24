/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * tlbbnu.S — x-tlb-base (patches/tcg/0052, docs/tcg-g4.md §41.3) vu du mode superviseur,
 * sans système : ce que l'espace utilisateur de Tiger ne peut pas faire (tools/tcg/tlbbnu.sh
 * le charge en RAM à 0x10000 par -device loader et lit les résultats par le moniteur).
 *
 * Mémoire physique : HTAB 64 Kio à 0x100000, page A 128 Kio à 0x200000 (mot i = 0xA000_0000
 * + i), page B à 0x300000 (0xB000_0000 + i), résultats à 0x400000 (fin : 0x600dcafe à
 * 0x400ffc). Instructions en mode réel (IR = 0), données traduites (DR = 1) :
 *   DBAT1 : 0 - 8 Mio à l'identique (code, HTAB, A, B, résultats)
 *   DBAT0 : EA 0x20000000 (128 Kio) -> A, puis -> B par mtspr DBAT0L + DBAT0U (helper)
 *   SR1 = VSID 0x123, PTE : EA 0x10005000 -> page A, puis -> page B (stw, tlbie, sync)
 *   DBAT3 : EA 0x30000000 -> A ; DBAT2 : EA 0x30020000 -> 0xf2800000 (registre d'adresse
 *           de configuration PCI d'Uni-North, MMIO, I|G) : pages voisines RAM / MMIO.
 * Avant T1, T2 et T4, un bloc à part lit la base : le premier chargement du bloc testé trouve
 * le TLB rempli (succès : la base est posée, les suivants sont couverts).
 * T1 (helper au milieu d'un bloc) : lwz 0(r3) par DBAT0 -> A[0] ; mtspr DBAT0L (B),
 *    mtspr DBAT0U (valeur changée : helper_store_dbatu vide les pages du BAT, en ligne,
 *    sans étiquette) ; lwz 4(r3), lwz 8(r3) -> B[1], B[2]. Une base qui survivrait à
 *    l'appel lirait A.
 * T2 (PTE, tlbie, sync) : lwz 0(r3) -> A[0] ; stw PTE -> B ; tlbie ; sync ; lwz 4(r3) -> B[1].
 * T3 (MMIO) : stw au registre, puis lwz, lwz, lhz, lbz sur la même base (le premier passe
 *    par le chemin lent : rien n'est couvert).
 * T4 (RAM puis MMIO voisin) : base 0x3001fff8 : lwz 0, lwz 4 (A, couvert), lwz 8 (page
 *    MMIO : hors de la page vérifiée, chemin complet), lwz -4096 (A).
 * T6 (poison) : une boucle de 4 tours, le bloc t6 (lwz 0, lwz 4 sur EA 0x20000000) puis le
 *    bloc t6b (court : peu de registres de l'hôte touchés) qui met DBAT0 sur B (au premier
 *    tour DBAT0U change : le TLB de la page est vidé) ; au deuxième tour le premier
 *    chargement de t6 passe par le chemin lent, et le second ne doit pas réutiliser la page et
 *    l'addend du tour précédent (A), restés dans les registres préservés de l'hôte.
 * T7 (poison, dans un bloc) : paire 1 (lwz 0, lwz 4 : base posée, sur A), mtspr DBAT0L/U
 *    (-> B, vidage : la base meurt), paire 2 : son premier passe par le chemin lent ; les
 *    temporaires libérés de la paire 1 sont repris par la paire 2, dans les mêmes registres
 *    de l'hôte : sans le poison, le second lirait A.
 * T8 (étiquette sans appel) : paire (lwz 0, lwz 4), stwcx. (en SMP=1, le traducteur ne pose
 *    qu'un branchement et une étiquette, aucun helper), lwz 8 : la base doit être morte à
 *    l'étiquette (ses temporaires EBB n'y survivent pas).
 * T5 (bord de page, RAM des deux côtés, pages physiques non contiguës) : EA 0x10005000 -> B
 *    (depuis T2), EA 0x10006000 -> A + 0x3000 ; lwz 0xff8 (premier), lwz 0xffc et lbz 0xfff
 *    (couverts), lwz 0xffd et lhz 0xfff (à cheval), lwz 0x1000 (page suivante).
 */
    .text
    .globl _start
_start:
    lis     30, 0x40                /* résultats */
    /* A et B : 32768 mots chacun */
    lis     10, 0x20
    lis     11, 0x30
    li      12, 0
    lis     13, 1                   /* 65536 / 2 = 32768 mots */
    srwi    13, 13, 1
    mtctr   13
1:  slwi    14, 12, 2
    addis   15, 12, 0xa000
    stwx    15, 10, 14
    addis   15, 12, 0xb000
    stwx    15, 11, 14
    addi    12, 12, 1
    bdnz    1b
    /* HTAB à zéro (64 Kio) */
    lis     10, 0x10
    li      12, 0
    lis     13, 1
    srwi    13, 13, 2
    mtctr   13
2:  stw     12, 0(10)
    addi    10, 10, 4
    bdnz    2b
    /* PTE de EA 0x10005000 : PTEG 0x100000 + (0x123 ^ 5) << 6 */
    lis     9, 0x10
    ori     9, 9, 0x4980
    lis     12, 0x8000
    ori     12, 12, 0x9180          /* V | VSID 0x123 << 7 | API 0 */
    stw     12, 0(9)
    lis     12, 0x20
    ori     12, 12, 2               /* RPN A | PP = 2 */
    stw     12, 4(9)
    /* PTE de EA 0x10006000 -> A + 0x3000 : PTEG 0x100000 + (0x123 ^ 6) << 6 */
    lis     9, 0x10
    ori     9, 9, 0x4940
    lis     12, 0x8000
    ori     12, 12, 0x9180
    stw     12, 0(9)
    lis     12, 0x20
    ori     12, 12, 0x3002
    stw     12, 4(9)
    sync
    lis     12, 0x10                /* SDR1 = 0x100000, HTABMASK 0 */
    mtspr   25, 12
    li      12, 0x123
    mtsr    1, 12
    /* DBAT1 : 0 - 8 Mio identique */
    li      12, 2
    mtspr   539, 12                 /* DBAT1L : BRPN 0, PP 2 */
    li      12, 0xfe
    mtspr   538, 12                 /* DBAT1U : BEPI 0, BL 8 Mio, Vs */
    /* DBAT0 : 0x20000000 -> A */
    lis     12, 0x20
    ori     12, 12, 2
    mtspr   537, 12
    lis     12, 0x2000
    ori     12, 12, 2
    mtspr   536, 12
    /* DBAT3 : 0x30000000 -> A ; DBAT2 : 0x30020000 -> 0xf2800000 I|G */
    lis     12, 0x20
    ori     12, 12, 2
    mtspr   543, 12
    lis     12, 0x3000
    ori     12, 12, 2
    mtspr   542, 12
    lis     12, 0xf280
    ori     12, 12, 0x2a
    mtspr   541, 12
    lis     12, 0x3002
    ori     12, 12, 2
    mtspr   540, 12
    /* MSR : DR, ME */
    li      12, 0x1010
    mtmsr   12
    isync
    /* valeurs des tests, avant les blocs */
    lis     21, 0x30
    ori     21, 21, 2               /* DBAT0L -> B */
    lis     22, 0x2000
    ori     22, 22, 3               /* DBAT0U : Vs | Vp (changé) */
    lis     23, 0x30
    ori     23, 23, 2               /* PTE -> B */
    lis     24, 0x8000
    ori     24, 24, 0x0814          /* adresse de configuration écrite */
    lis     9, 0x10
    ori     9, 9, 0x4980
    lis     3, 0x2000               /* bloc à part : remplit le TLB pour t1 */
    lwz     0, 0(3)
    b       t1
    .balign 64
t1: lis     3, 0x2000
    lwz     5, 0(3)
    mtspr   537, 21
    mtspr   536, 22
    lwz     6, 4(3)
    lwz     7, 8(3)
    stw     5, 0(30)
    stw     6, 4(30)
    stw     7, 8(30)
    lis     3, 0x1000
    ori     3, 3, 0x5000
    b       w2
    .balign 64
w2: lwz     0, 0(3)                 /* bloc à part : remplit le TLB pour t2 */
    b       t2
    .balign 64
t2: lis     3, 0x1000
    ori     3, 3, 0x5000
    lwz     5, 0(3)
    stw     23, 4(9)
    tlbie   3
    sync
    lwz     6, 4(3)
    lwz     7, 8(3)
    stw     5, 12(30)
    stw     6, 16(30)
    stw     7, 20(30)
    b       t3
    .balign 64
t3: lis     3, 0x3002
    stw     24, 0(3)
    lwz     5, 0(3)
    lwz     6, 0(3)
    lhz     7, 2(3)
    lbz     8, 1(3)
    stw     5, 24(30)
    stw     6, 28(30)
    stw     7, 32(30)
    stw     8, 36(30)
    lis     4, 0x3002
    addi    4, 4, -8
    b       w4
    .balign 64
w4: lwz     0, 0(4)                 /* bloc à part : remplit le TLB pour t4 */
    b       t4
    .balign 64
t4: lis     4, 0x3002
    addi    4, 4, -8
    lwz     5, 0(4)
    lwz     6, 4(4)
    lwz     7, 8(4)
    lwz     8, -4096(4)
    stw     5, 40(30)
    stw     6, 44(30)
    stw     7, 48(30)
    stw     8, 52(30)
    lis     3, 0x1000
    ori     3, 3, 0x5000
    b       w5
    .balign 64
w5: lwz     0, 0(3)                 /* bloc à part : les deux pages dans le TLB */
    lwz     0, 0x1000(3)
    b       t5
    .balign 64
t5: lwz     5, 0xff8(3)             /* premier : page 0x10005000 (-> B depuis T2) */
    lwz     6, 0xffc(3)             /* couvert, dernier mot */
    lwz     7, 0xffd(3)             /* à cheval 3 + 1 : chemin complet */
    lhz     8, 0xfff(3)             /* à cheval 1 + 1 */
    lbz     10, 0xfff(3)            /* couvert, dernier octet */
    lwz     11, 0x1000(3)           /* page suivante (-> A + 0x3000) */
    stw     5, 56(30)
    stw     6, 60(30)
    stw     7, 64(30)
    stw     8, 68(30)
    stw     10, 72(30)
    stw     11, 76(30)
    /* T6 : DBAT0 -> A (EA 0x20000000), boucle de 4 tours */
    lis     21, 0x20
    ori     21, 21, 2               /* DBAT0L -> A */
    lis     22, 0x30
    ori     22, 22, 2               /* DBAT0L -> B */
    lis     23, 0x2000
    ori     23, 23, 2               /* DBAT0U : Vs */
    lis     24, 0x2000
    ori     24, 24, 3               /* DBAT0U : Vs | Vp */
    mtspr   537, 21
    mtspr   536, 23
    lis     3, 0x2000
    addi    29, 30, 80              /* résultats de T6 : 80(r30) */
    li      12, 4
    mtctr   12
    b       t6
    .balign 64
t6: lwz     5, 0(3)                 /* premier : raté du TLB (vidé par t6b) */
    lwz     6, 4(3)                 /* couvert : le poison doit l'envoyer au chemin complet */
    stw     5, 0(29)
    stw     6, 4(29)
    addi    29, 29, 8
    b       t6b
    .balign 64
t6b:mtspr   537, 22                 /* B ; DBAT0U changé au premier tour seulement */
    mtspr   536, 24
    bdnz    t6
    /* T7 : DBAT0 -> A (U = Vs), puis un bloc à part remplit le TLB */
    mtspr   537, 21
    mtspr   536, 23
    b       w7
    .balign 64
w7: lwz     0, 0(3)
    b       t7
    .balign 64
t7: lwz     5, 0(3)                 /* paire 1 sur A : base posée (registres de l'hôte) */
    lwz     6, 4(3)
    mtspr   537, 22                 /* -> B, DBAT0U changé : TLB de la page vidé, base tuée */
    mtspr   536, 24
    lwz     7, 0(3)                 /* paire 2 : premier par le chemin lent (raté) */
    lwz     8, 4(3)                 /* couvert : ni la page ni l'addend de la paire 1 */
    stw     5, 112(30)
    stw     6, 116(30)
    stw     7, 120(30)
    stw     8, 124(30)
    lis     10, 0x40
    ori     10, 10, 0x800           /* mot de travail de stwcx. (0x400800) */
    b       w8
    .balign 64
w8: lwz     0, 0(3)                 /* bloc à part : remplit le TLB pour t8 */
    b       t8
    .balign 64
t8: lwz     5, 0(3)                 /* paire : base posée (B, depuis T7) */
    lwz     6, 4(3)
    stwcx.  9, 0, 10                /* sans réservation : échoue ; un branchement et une
                                       étiquette TCG, sans appel (SMP=1 : pas d'atomique) */
    lwz     7, 8(3)                 /* après l'étiquette : nouvelle base */
    stw     5, 128(30)
    stw     6, 132(30)
    stw     7, 136(30)
    b       fin
    .balign 64
fin:
    lis     12, 0x600d
    ori     12, 12, 0xcafe
    stw     12, 0xffc(30)
    sync
3:  b       3b
