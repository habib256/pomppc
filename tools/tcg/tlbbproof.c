// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
/*
 * tlbbproof.c — épreuve exhaustive du test de couverture de x-tlb-base
 * (patches/tcg/0052, docs/tcg-g4.md §41.3), modèles C des deux émetteurs.
 *
 * qemu_ld_tlbb0 (premier chargement d'une base), sur un succès du TLB :
 *   x86_64  : L1 = (u32)(ea0 + s - 1) & 0xfffff000 (lea 32 bits, and) ; succès ssi
 *             L1 == comparateur de l'entrée d'indice idx(ea0) ; negp = -(u64)L1 (mov, neg)
 *   aarch64 : TMP2 = (u32)(ea0 + s - 1) & 0xfffff000 (add w, and w) ; negp = 0 - TMP2
 *   chemin lent : negp = 0x1000 (mov r32, imm : zéro-étendu)
 * qemu_ld_tlbb1 (chargement couvert) :
 *   x86_64  : t = negp + (u64)(u32)ea (lea 64 bits ; l'adresse 32 bits est zéro-étendue
 *             dans son registre) ; cmp t, 0x1000 - s (imm32 étendu) ; ja = hors ligne
 *   aarch64 : t = negp + uxtw(ea) (add x, x, w, uxtw) ; subs xzr, t, #(0x1000 - s) ;
 *             b.hi = hors ligne
 * Attendu : accès direct ssi [ea, ea + s - 1] tient dans la page vérifiée (page(ea) ==
 * page0 et (ea & 0xfff) + s <= 0x1000), jamais après le chemin lent.
 *
 *   cc -O2 -o tlbbproof tools/tcg/tlbbproof.c && ./tlbbproof [nb-pages-aléatoires]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const unsigned SIZES[] = { 1, 2, 4, 8 };
static uint64_t nchk, nbad, nacc;

/* le TLB du modèle : l'entrée d'indice idx(a) tient la page de a (le meilleur cas pour un
 * faux succès : tout comparateur qui ne vaut pas L1 donne le chemin lent) */
static int tlb_hit(uint32_t ea0, unsigned s, uint32_t *l1)
{
    *l1 = (uint32_t)(ea0 + s - 1) & 0xfffff000u;
    return *l1 == (ea0 & 0xfffff000u);
}

static uint64_t negp_x86(uint32_t l1) { return -(uint64_t)l1; }
static uint64_t negp_a64(uint32_t tmp2) { return (uint64_t)0 - (uint64_t)tmp2; }

static int cover_x86(uint64_t negp, uint32_t ea, unsigned s)
{
    uint64_t t = negp + (uint64_t)ea;                   /* lea L0, [negp + addr] */
    uint64_t k = (uint64_t)(int64_t)(int32_t)(0x1000 - s); /* imm32 étendu */
    return !(t > k);                                    /* ja : non signé */
}

static int cover_a64(uint64_t negp, uint32_t ea, unsigned s)
{
    uint64_t t = negp + (uint64_t)(uint32_t)ea;         /* add x16, negp, waddr, uxtw */
    return !(t > (uint64_t)(0x1000 - s));               /* subs ; b.hi */
}

static int expect(uint32_t page0, uint32_t ea, unsigned s)
{
    return (ea & 0xfffff000u) == page0 && (ea & 0xfff) + s <= 0x1000;
}

static void check(uint64_t nx, uint64_t na, int poison, uint32_t page0, uint32_t ea, unsigned s)
{
    int e = poison ? 0 : expect(page0, ea, s);
    int x = cover_x86(nx, ea, s), a = cover_a64(na, ea, s);

    nchk++;
    nacc += e;
    if (x != e || a != e) {
        if (nbad++ < 20) {
            printf("ÉCART page0 %08x ea %08x taille %u poison %d : attendu %d x86 %d a64 %d\n",
                   page0, ea, s, poison, e, x, a);
        }
    }
}

/* tous les ea0 de la fenêtre comme premier accès, et pour chacun la page qu'il pose */
static void first_window(uint32_t lo, uint32_t n)
{
    for (unsigned k = 0; k < 4; k++) {
        unsigned s = SIZES[k];
        for (uint32_t i = 0; i < n; i++) {
            uint32_t ea0 = lo + i, l1;
            nchk++;
            if (tlb_hit(ea0, s, &l1)) {
                /* succès : la page posée est celle de ea0, et l'accès ne chevauche pas */
                if (l1 != (ea0 & 0xfffff000u) || (ea0 & 0xfff) + s > 0x1000 ||
                    negp_x86(l1) != -(uint64_t)(ea0 & 0xfffff000u) ||
                    negp_a64(l1) != negp_x86(l1)) {
                    if (nbad++ < 20) printf("ÉCART premier ea0 %08x taille %u\n", ea0, s);
                }
            } else if ((ea0 & 0xfff) + s <= 0x1000) {
                if (nbad++ < 20) printf("ÉCART premier ea0 %08x taille %u : raté\n", ea0, s);
            }
        }
    }
}

static void window(uint32_t page0, int64_t lo, int64_t hi)
{
    uint64_t nx = negp_x86(page0), na = negp_a64(page0);

    for (unsigned k = 0; k < 4; k++) {
        for (int64_t e = lo; e < hi; e++) {
            check(nx, na, 0, page0, (uint32_t)e, SIZES[k]);
        }
    }
}

static void full(uint32_t page0, int poison)
{
    uint64_t nx = poison ? 0x1000 : negp_x86(page0), na = poison ? 0x1000 : negp_a64(page0);

    for (unsigned k = 0; k < 4; k++) {
        uint32_t ea = 0;
        do {
            check(nx, na, poison, page0, ea, SIZES[k]);
        } while (++ea != 0);
    }
}

int main(int argc, char **argv)
{
    unsigned nrand = argc > 1 ? atoi(argv[1]) : 4096;
    static const uint32_t FIXED[] = { 0x00000000, 0x00001000, 0x7ffff000, 0x80000000,
                                      0xffffe000, 0xfffff000 };

    /* 1. premier accès : toutes les adresses au voisinage de bords de page */
    first_window(0xfffff000u - 0x2000, 0x4000);          /* bord 0, le haut de l'espace */
    first_window(0x7fffe000u, 0x4000);
    first_window(0x0001e000u, 0x4000);
    printf("premier accès : %llu cas, %llu écarts\n",
           (unsigned long long)nchk, (unsigned long long)nbad);
    /* 2. couverture : page posée, ea sur tout l'espace 32 bits (pages fixes) et poison */
    for (unsigned i = 0; i < sizeof(FIXED) / sizeof(FIXED[0]); i++) {
        full(FIXED[i], 0);
    }
    full(0, 1);
    printf("+ couverture, 6 pages x 2^32 adresses x 4 tailles et poison x 2^32 x 4 : "
           "%llu cas, %llu écarts\n", (unsigned long long)nchk, (unsigned long long)nbad);
    /* 3. pages tirées au hasard : ea de page0 - 0x3000 à page0 + 0x4000 */
    srand(52);
    for (unsigned i = 0; i < nrand; i++) {
        uint32_t page0 = ((uint32_t)rand() << 12) ^ ((uint32_t)rand() << 24);
        page0 &= 0xfffff000u;
        window(page0, (int64_t)page0 - 0x3000, (int64_t)page0 + 0x4000);
    }
    printf("+ %u pages aléatoires x 0x7000 adresses x 4 tailles : %llu cas, dont %llu accès "
           "directs ; %llu écarts\n", nrand, (unsigned long long)nchk,
           (unsigned long long)nacc, (unsigned long long)nbad);
    return nbad != 0;
}
