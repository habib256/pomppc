/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 VERHILLE Arnaud
 *
 * t3bench.S — micro-banc nu des régions compilées tcg/0049 (x-tier3) : ce
 * qu'une région gagne SUR SON PROPRE CODE quand elle tourne longtemps
 * (boucles), sans le coût d'entrée et de sortie. Chargé par
 * `-M mac99 -bios t3bench.bin` (mode réel, 1 vCPU), voir tools/tcg/t3bench.sh.
 *
 *  K1 : somme de contrôle d'un tampon de 4 Kio (lwz, add, rotlwi, xor, stw),
 *       boucle de 1024 tours, appelée N fois (bl/blr vers une autre page) ;
 *  K2 : boucle entière sans mémoire (addc/adde, mullw, rlwinm, cmpw, bc).
 *
 * Fin : 0xC0FFEE00 à 0x3000, la somme à 0x3004.
 */
        .text
        .org 0x100
        lis     %r1, 0x10
        lis     %r28, 0x20              /* tampon : 0x200000, 1024 mots */
        lis     %r20, 0x1234
        ori     %r20, %r20, 0x5678
        li      %r5, 1024
        mtctr   %r5
        mr      %r3, %r28
1:      stw     %r20, 0(%r3)
        rotlwi  %r20, %r20, 7
        addi    %r20, %r20, 0x3d
        addi    %r3, %r3, 4
        bdnz    1b
        li      %r22, 0
        li      %r23, 0
        /* K1 */
        lis     %r14, KITER@h
        ori     %r14, %r14, KITER@l
k1:     mr      %r3, %r28
        bl      cksum
        add     %r22, %r22, %r3
        addic.  %r14, %r14, -1
        bne     k1
        /* K2 */
        lis     %r14, K2ITER@h
        ori     %r14, %r14, K2ITER@l
        li      %r6, 1
        li      %r7, 3
k2:     addc    %r6, %r6, %r7
        adde    %r7, %r7, %r22
        mullw   %r8, %r6, %r7
        rlwinm  %r8, %r8, 5, 3, 27
        xor     %r23, %r23, %r8
        cmpw    %r6, %r7
        blt     2f
        addi    %r23, %r23, 1
2:      addic.  %r14, %r14, -1
        bne     k2
        xor     %r7, %r22, %r23
        li      %r3, 0x3000
        stw     %r7, 4(%r3)
        lis     %r6, 0xc0ff
        ori     %r6, %r6, 0xee00
        stw     %r6, 0(%r3)
9:      b       9b

        .org 0x2000                     /* une autre page */
cksum:  li      %r5, 1024
        mtctr   %r5
        li      %r4, 0
1:      lwz     %r6, 0(%r3)
        add     %r4, %r4, %r6
        rotlwi  %r4, %r4, 3
        xor     %r6, %r6, %r4
        stw     %r6, 0(%r3)
        addi    %r3, %r3, 4
        bdnz    1b
        mr      %r3, %r4
        blr
