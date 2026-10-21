/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 VERHILLE Arnaud
 *
 * t2proof.S — banc nu (sans OpenBIOS) du second niveau tcg/0048 (x-tier2,
 * x-tier2-verify), chargé par `-M mac99 -bios t2proof.bin` (image brute à
 * 0xfff00000, départ à 0xfff00100, mode réel). Voir tools/tcg/t2proof.sh.
 *
 * Il reproduit les deux défauts de la partie vérifiée de Tiger du 21/10
 * (docs/plan-traducteur-rapide.md §9.5) :
 *  1. un accès que le journal ne voit pas (lswx/stswx : helpers à cpu_ld/st,
 *     comme l'en-tête du bcopy de la page commune à 0xffff88dc) dans une
 *     trace qui fait glisser un tampon sur lui-même : avant la correction,
 *     le rejeu relisait la mémoire déjà modifiée (« registres : r6 ») et
 *     son stswx RÉÉCRIVAIT la mémoire avec les registres de l'entrée
 *     (corruption de l'invité : somme fausse) ;
 *  2. un bloc droit de 144 instructions et 144 accès (comme la copie d'état
 *     de thread du noyau à 0xa66cc), un second, et un petit bloc : la copie
 *     journalisée, à court de temporaires TCG, était coupée en deux par
 *     tb_gen_code et finissait au milieu du bloc (« écriture en trop ») ;
 *     maintenant une trace coupée est raccourcie à ses blocs entiers.
 *
 * Fin : 0xC0FFEE00 à 0x3000, la somme à 0x3004, puis boucle d'attente.
 */
        .text
        .org 0x100
_start:
        lis     %r1, 0x10               /* pile (inutilisée) */
        lis     %r28, 0x20              /* buf  : 0x200000, 72 octets glissants */
        lis     %r30, 0x21              /* src  : 0x210000, 48 mots */
        lis     %r31, 0x22              /* dst  : 0x220000, 224 mots */
        li      %r2, 0
        lis     %r20, 0x1234
        ori     %r20, %r20, 0x5678
        li      %r21, 1
        /* initialisation : buf et src remplis d'un motif */
        li      %r5, 18
        mtctr   %r5
        mr      %r3, %r28
1:      stw     %r20, 0(%r3)
        rotlwi  %r20, %r20, 7
        addi    %r20, %r20, 0x3d
        addi    %r3, %r3, 4
        bdnz    1b
        li      %r5, 48
        mtctr   %r5
        mr      %r3, %r30
1:      stw     %r20, 0(%r3)
        rotlwi  %r20, %r20, 11
        xori    %r20, %r20, 0x5a5a
        addi    %r3, %r3, 4
        bdnz    1b

        li      %r5, 0x4000             /* phase A : 16384 tours */
        mtctr   %r5
top:
        /* 1. glissement de buf de 8 octets vers le bas : 7 octets par
         *    lswx/stswx (helpers), puis 14 mots par lwz/stw */
        addi    %r4, %r28, 8
        mr      %r3, %r28
        li      %r11, 7
        mtxer   %r11
        lswx    %r6, 0, %r4
        stswx   %r6, 0, %r3
        stw     %r21, 8(%r28)           /* réécrit la source du lswx : un rejeu
                                         * qui relit la mémoire voit autre chose */
        add     %r4, %r4, %r11
        add     %r3, %r3, %r11
        cmpwi   %r11, 0
        beq     9f                      /* jamais pris : fin de bloc */
        li      %r12, 14
2:      lwz     %r6, 0(%r4)
        stw     %r6, 0(%r3)
        addi    %r4, %r4, 4
        addi    %r3, %r3, 4
        addic.  %r12, %r12, -1
        bne     2b
        /* un octet de queue, et 8 octets neufs en fin de tampon */
        lbz     %r6, 0(%r4)
        stb     %r6, 0(%r3)
        stw     %r20, 64(%r28)
        stw     %r21, 68(%r28)
        rotlwi  %r20, %r20, 5
        xor     %r20, %r20, %r21
        addi    %r21, %r21, 3
        cmpwi   %r28, 0
        beq     9f                      /* jamais pris */
        bdnz    top

        li      %r5, 0x4000             /* phase B : 16384 tours */
        mtctr   %r5
topb:
        /* 2. bloc droit de 144 instructions / 144 accès (noyau 0xa66cc) */
        .set    i, 0
        .rept   48
        lwz     %r3, i*4(%r30)
        stw     %r2, i*8(%r31)
        stw     %r3, i*8+4(%r31)
        .set    i, i+1
        .endr
        cmpwi   %r30, 0
        beq     9f                      /* jamais pris : fin du long bloc */
        /* un second long bloc : la trace (~300 instructions) dépasse les
         * temporaires de TCG, tb_gen_code la coupe ; t2_form la raccourcit
         * au premier bloc */
        .set    i, 0
        .rept   48
        lwz     %r3, i*4(%r30)
        stw     %r3, 0x200+i*8(%r31)
        stw     %r2, 0x204+i*8(%r31)
        .set    i, i+1
        .endr
        cmpwi   %r31, 0
        beq     9f                      /* jamais pris */
        /* petit bloc qui le suit : src évolue */
        lwz     %r5, 0(%r30)
        add     %r5, %r5, %r21
        stw     %r5, 0(%r30)
        lwz     %r5, 100(%r30)
        xor     %r5, %r5, %r20
        stw     %r5, 100(%r30)
        addi    %r2, %r2, 1
        bdnz    topb

        /* somme : buf (72 o) et dst (224 mots) */
        li      %r7, 0
        li      %r5, 18
        mtctr   %r5
        mr      %r3, %r28
1:      lwz     %r6, 0(%r3)
        rotlwi  %r7, %r7, 3
        xor     %r7, %r7, %r6
        addi    %r3, %r3, 4
        bdnz    1b
        li      %r5, 224
        mtctr   %r5
        mr      %r3, %r31
1:      lwz     %r6, 0(%r3)
        rotlwi  %r7, %r7, 3
        xor     %r7, %r7, %r6
        addi    %r3, %r3, 4
        bdnz    1b
        li      %r3, 0x3000
        stw     %r7, 4(%r3)
        lis     %r6, 0xc0ff
        ori     %r6, %r6, 0xee00
        stw     %r6, 0(%r3)
9:      b       9b
