/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 VERHILLE Arnaud
 *
 * t3proof.S — banc nu (sans OpenBIOS) des régions compilées tcg/0049
 * (x-tier3, x-tier2-verify), chargé par `-M mac99 -bios t3proof.bin` (image
 * brute à 0xfff00000, départ à 0xfff00100, mode réel). Voir
 * tools/tcg/t3proof.sh et docs/plan-traducteur-rapide.md §10.
 *
 * Ce qu'une région change par rapport aux TB et qu'il faut éprouver :
 *  A. boucles imbriquées (arêtes arrière internes), chaîne de retenue
 *     addc/adde, mulhwu, rotlw, divwu, sraw (helper dont la sortie est un
 *     registre invité, défaut trouvé sur Tiger), et un appel bl vers une fonction
 *     d'une AUTRE page qui revient par blr (retour prédit, contrôle de page) ;
 *  B. accès mémoire dans une boucle, et une exception précise (tweq) dans la
 *     boucle une fois sur 4 : le gestionnaire (0x700, préfixe 0xfff00000)
 *     compte et saute l'instruction (SRR0 + 4) — un SRR0 faux (pc mal
 *     restauré depuis une région) dérègle la somme ;
 *  C. code modifié : une fonction copiée en RAM, dont l'immédiat d'un addi
 *     change à chaque tour (stw ; dcbst ; sync ; icbi ; isync), appelée par
 *     bctrl : la région qui la contient doit mourir à chaque écriture ;
 *  D. lswx/stswx (accès hors journal) dans une boucle qui fait glisser un
 *     tampon sur lui-même (défaut du 21/10, §9.5).
 *
 * Fin : 0xC0FFEE00 à 0x3000, la somme à 0x3004, le nombre de pièges à
 * 0x3008, puis boucle d'attente.
 */
        .text
        .org 0x100
        b       main

        .org 0x700                      /* programme (piège) */
        mfsrr0  %r29
        addi    %r29, %r29, 4
        mtsrr0  %r29
        addi    %r25, %r25, 1
        rfi

        .org 0x1000
main:
        mfmsr   %r3
        ori     %r3, %r3, 0x40          /* MSR[IP] : vecteurs à 0xfff00000 */
        mtmsr   %r3
        isync
        lis     %r1, 0x10
        lis     %r28, 0x20              /* buf : 0x200000, 256 mots */
        li      %r25, 0
        li      %r22, 0
        li      %r23, 0
        lis     %r20, 0x1234
        ori     %r20, %r20, 0x5678
        li      %r21, 7
        /* buf rempli d'un motif */
        li      %r5, 256
        mtctr   %r5
        mr      %r3, %r28
1:      stw     %r20, 0(%r3)
        rotlwi  %r20, %r20, 7
        addi    %r20, %r20, 0x3d
        addi    %r3, %r3, 4
        bdnz    1b

        /* A. calcul en boucles imbriquées, appel d'une autre page */
        li      %r5, 0x3000
        mtctr   %r5
pa:     li      %r12, 7
pa2:    addc    %r22, %r22, %r20
        adde    %r23, %r23, %r21
        mulhwu  %r24, %r20, %r21
        xor     %r22, %r22, %r24
        rotlw   %r20, %r20, %r21
        addi    %r21, %r21, 3
        divwu   %r26, %r20, %r21
        add     %r23, %r23, %r26
        sraw    %r27, %r22, %r12        /* helper à sortie globale (+ XER[CA]) */
        addze   %r27, %r27
        xor     %r23, %r23, %r27
        addic.  %r12, %r12, -1
        bne     pa2
        mr      %r3, %r22
        bl      farf
        add     %r23, %r23, %r3
        bdnz    pa

        /* B. mémoire en boucle, un piège sur 4 */
        li      %r5, 0x2000
        mtctr   %r5
        li      %r6, 0
pb:     mulli   %r7, %r6, 37
        andi.   %r7, %r7, 255
        slwi    %r7, %r7, 2
        lwzx    %r8, %r28, %r7
        mulli   %r8, %r8, 3
        add     %r8, %r8, %r6
        stwx    %r8, %r28, %r7
        andi.   %r10, %r6, 3
        bne     1f
        tweq    %r10, %r10              /* piège : r25++, on saute le tweq */
        addi    %r22, %r22, 1           /* exécuté après le piège */
1:      addi    %r6, %r6, 1
        bdnz    pb

        /* C. code modifié : smcf copiée à 0x7000, son addi change */
        bl      3f                      /* adresse de smcf sans relocation */
3:      mflr    %r3
        addi    %r3, %r3, smcf - 3b - 4
        li      %r4, 0x6ffc
        li      %r5, (smce - smcf) / 4
        mtctr   %r5
1:      lwzu    %r6, 4(%r3)
        stwu    %r6, 4(%r4)
        bdnz    1b
        li      %r5, (smce - smcf) / 4
        mtctr   %r5
        li      %r4, 0x7000
1:      dcbst   0, %r4
        sync
        icbi    0, %r4
        addi    %r4, %r4, 4
        bdnz    1b
        sync
        isync
        li      %r14, 0x40              /* tours */
        li      %r15, 0
pc:     andi.   %r6, %r15, 0x7fff
        oris    %r6, %r6, 0x3863        /* addi r3, r3, imm */
        li      %r4, 0x7000 + (smci - smcf)
        stw     %r6, 0(%r4)
        dcbst   0, %r4
        sync
        icbi    0, %r4
        isync
        li      %r3, 0
        li      %r4, 0x7000
        mtctr   %r4
        bctrl
        add     %r22, %r22, %r3
        addi    %r15, %r15, 3
        addic.  %r14, %r14, -1
        bne     pc

        /* D. glissement de 8 octets par lswx/stswx puis lwz/stw */
        li      %r5, 0x1000
        mtctr   %r5
pd:     addi    %r4, %r28, 8
        mr      %r3, %r28
        li      %r11, 7
        mtxer   %r11
        lswx    %r6, 0, %r4
        stswx   %r6, 0, %r3
        stw     %r21, 8(%r28)
        add     %r4, %r4, %r11
        add     %r3, %r3, %r11
        li      %r12, 14
2:      lwz     %r6, 0(%r4)
        stw     %r6, 0(%r3)
        addi    %r4, %r4, 4
        addi    %r3, %r3, 4
        addic.  %r12, %r12, -1
        bne     2b
        stw     %r20, 64(%r28)
        rotlwi  %r20, %r20, 5
        xor     %r20, %r20, %r21
        addi    %r21, %r21, 3
        bdnz    pd

        /* somme : buf, r22, r23, r25 */
        li      %r7, 0
        li      %r5, 256
        mtctr   %r5
        mr      %r3, %r28
1:      lwz     %r6, 0(%r3)
        rotlwi  %r7, %r7, 3
        xor     %r7, %r7, %r6
        addi    %r3, %r3, 4
        bdnz    1b
        xor     %r7, %r7, %r22
        rotlwi  %r7, %r7, 7
        xor     %r7, %r7, %r23
        rotlwi  %r7, %r7, 7
        xor     %r7, %r7, %r25
        li      %r3, 0x3000
        stw     %r7, 4(%r3)
        stw     %r25, 8(%r3)
        lis     %r6, 0xc0ff
        ori     %r6, %r6, 0xee00
        stw     %r6, 0(%r3)
9:      b       9b

        /* fonction copiée en RAM (C) */
smcf:   li      %r9, 100
        mtctr   %r9
smcl:
smci:   addi    %r3, %r3, 1             /* immédiat réécrit à chaque tour */
        xori    %r3, %r3, 0x55
        bdnz    smcl
        blr
smce:

        .org 0x2000                     /* une autre page */
farf:   rotlwi  %r3, %r3, 9
        xor     %r3, %r3, %r21
        addi    %r3, %r3, 0x77
        blr
