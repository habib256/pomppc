/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 VERHILLE Arnaud
 *
 * tbhproof.S — banc nu (sans OpenBIOS) de la tête de bloc allégée tcg/0051
 * (x-tb-head), chargé par `-M mac99 -bios tbhproof.bin` (image brute à
 * 0xfff00000, départ à 0xfff00100, mode réel, vecteurs à 0xfff00000 par
 * MSR[IP]). Voir tools/tcg/tbhproof.sh et docs/anatomie-ppc32.md.
 *
 *  1. boucle sur elle-même (`b .`) : le décrémenteur doit l'interrompre ;
 *  2. cycle de trois blocs A → B → C → A (deux arêtes VERS L'AVANT, chaînées
 *     après la vérification d'interruption avec x-tb-head=2, une vers
 *     l'arrière, qui vérifie) : le décrémenteur doit l'interrompre aussi ;
 *     le mutant POMPPC_TBHEAD_MUTANT=1 (toute arête chaînée après la
 *     vérification) ne sort plus jamais de la boucle 1 ;
 *  3. lectures d'un registre d'E/S (UniNorth, 0xf8000000) au milieu d'un
 *     bloc et en fin de bloc : chemin io_prepare / cpu_io_recompile, que
 *     x-tb-head=1 (can_do_io déduit) et le mode de vérification (+4)
 *     exercent ; elles comptent à part.
 *
 * Chaque phase dure NPH interruptions du décrémenteur (période DECP).
 * Fin : 0xC0FFEE00 à 0x3000, le nombre d'interruptions à 0x3004 (doit valoir
 * 3 * NPH), les compteurs des blocs A, B, C à 0x3010.., puis attente.
 */
        .set    NPH, 100
        .set    DECP, 100
        .set    MSR_IP, 0x40
        .set    MSR_EE, 0x8000
        .text
_base:
        .org 0x100
_start:
        li      %r3, MSR_IP
        mtmsr   %r3
        isync
        li      %r0, 0
        stw     %r0, 0x3000(0)
        stw     %r0, 0x3004(0)
        stw     %r0, 0x3008(0)
        stw     %r0, 0x3020(0)
        li      %r5, 0
        li      %r6, 0
        li      %r7, 0
        li      %r8, 0
        lis     %r9, 0xf800             /* registre d'E/S (UniNorth) */
        li      %r3, DECP
        mtdec   %r3
        li      %r3, MSR_IP
        ori     %r3, %r3, MSR_EE
        mtmsr   %r3                    /* fin de bloc : interruptions permises */
        isync
phase1:
        b       phase1                  /* arête vers lui-même : vérifie */

        .org 0x200                      /* machine check : noté, puis fin */
        lis     %r3, 0xdead
        stw     %r3, 0x3020(0)
        lis     %r4, 0xfff0
        ori     %r4, %r4, fin - _base
        mtsrr0  %r4
        rfi
        .org 0x300                      /* DSI : noté, puis fin */
        lis     %r3, 0xd51
        stw     %r3, 0x3020(0)
        lis     %r4, 0xfff0
        ori     %r4, %r4, fin - _base
        mtsrr0  %r4
        rfi

        .org 0x400
phase2:
blocA:  addi    %r5, %r5, 1
        b       blocB                   /* vers l'avant */
        .org 0x480
blocB:  addi    %r6, %r6, 1
        b       blocC                   /* vers l'avant */
        .org 0x500
blocC:  addi    %r7, %r7, 1
        b       blocA                   /* vers l'arrière : vérifie */

        .org 0x600
phase3:
        lwz     %r10, 0(%r9)            /* E/S au milieu du bloc */
        addi    %r8, %r8, 1
        add     %r11, %r10, %r8
        lwz     %r12, 4(%r9)            /* E/S au milieu */
        cmpwi   %r8, 0
        lwz     %r10, 8(%r9)            /* E/S en dernière instruction ? (non : le b suit) */
        b       phase3

        .org 0x700
fin:
        li      %r3, MSR_IP
        mtmsr   %r3
        isync
        lwz     %r3, 0x3008(0)
        stw     %r3, 0x3004(0)
        stw     %r5, 0x3010(0)
        stw     %r6, 0x3014(0)
        stw     %r7, 0x3018(0)
        stw     %r8, 0x301c(0)
        lis     %r3, 0xc0ff
        ori     %r3, %r3, 0xee00
        stw     %r3, 0x3000(0)
1:      b       1b

        .org 0x900                      /* décrémenteur */
dec:
        mtsprg  0, %r3
        mtsprg  1, %r4
        mfcr    %r4
        mtsprg  2, %r4
        lwz     %r3, 0x3008(0)
        addi    %r3, %r3, 1
        stw     %r3, 0x3008(0)
        li      %r4, DECP
        mtdec   %r4
        cmpwi   %r3, NPH
        bne     1f
        lis     %r4, 0xfff0
        ori     %r4, %r4, phase2 - _base
        mtsrr0  %r4
        b       9f
1:      cmpwi   %r3, 2 * NPH
        bne     2f
        lis     %r4, 0xfff0
        ori     %r4, %r4, phase3 - _base
        mtsrr0  %r4
        b       9f
2:      cmpwi   %r3, 3 * NPH
        bne     9f
        lis     %r4, 0xfff0
        ori     %r4, %r4, fin - _base
        mtsrr0  %r4
        mfsrr1  %r4                     /* fin sans interruptions : MSR[EE] = 0 */
        rlwinm  %r4, %r4, 0, 17, 15
        mtsrr1  %r4
9:      mfsprg  %r4, 2
        mtcr    %r4
        mfsprg  %r4, 1
        mfsprg  %r3, 0
        rfi

        .org 0xa00
        .long   0
