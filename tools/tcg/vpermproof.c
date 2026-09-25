/*
 * vpermproof.c — preuve d'équivalence de patches/tcg/0004-ppc-vperm-fast.patch
 * (propriété x-vperm-fast, docs/tcg-g4.md §10).
 *
 * helper_VPERM (la boucle d'origine) et helper_VPERM_FAST (une consultation de
 * table ; un seul tbl NEON sur un hôte arm64), EXTRAITS TELS QUELS de
 * target/ppc/int_helper.c par vpermproof.sh, sont comparés sur l'hôte :
 *   1. chaque octet de contrôle (256 valeurs) à chaque position (16), avec
 *      64 couples (a, b) aléatoires et les 15 autres octets aléatoires ;
 *   2. des vecteurs entièrement aléatoires ;
 *   3. le recouvrement : r = a, r = b, r = c, a = b = c = r.
 * Les 16 octets du résultat doivent être égaux.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define HOST_BIG_ENDIAN 1
#define VsrB(i) u8[i]
#else
#define HOST_BIG_ENDIAN 0
#define VsrB(i) u8[15 - (i)]
#endif
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
typedef union { uint8_t u8[16]; uint64_t u64[2]; } ppc_avr_t;

#include "vpermproof-helpers.h"   /* helper_VPERM, helper_VPERM_FAST */

static uint64_t st = 0x5eed;
static uint64_t rnd(void)
{
    uint64_t z = (st += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static void rv(ppc_avr_t *v) { v->u64[0] = rnd(); v->u64[1] = rnd(); }

static uint64_t cases, bad;
static void cmp(ppc_avr_t a, ppc_avr_t b, ppc_avr_t c)
{
    ppc_avr_t r1, r2;
    helper_VPERM(&r1, &a, &b, &c);
    helper_VPERM_FAST(&r2, &a, &b, &c);
    cases++;
    if (memcmp(&r1, &r2, 16)) {
        if (bad++ < 3) {
            fprintf(stderr, "✘ c=%016llx%016llx\n", (unsigned long long)c.u64[1],
                    (unsigned long long)c.u64[0]);
        }
    }
}

int main(int argc, char **argv)
{
    uint64_t n = argc > 1 ? strtoull(argv[1], 0, 0) : 50000000;
    ppc_avr_t a, b, c;
    uint64_t alias = 0, abad = 0;

    for (int pos = 0; pos < 16; pos++) {
        for (int v = 0; v < 256; v++) {
            for (int k = 0; k < 64; k++) {
                rv(&a); rv(&b); rv(&c);
                c.u8[pos] = v;
                cmp(a, b, c);
            }
        }
    }
    printf("contrôle octet par octet : %llu cas (16 positions x 256 valeurs x 64)\n",
           (unsigned long long)cases);
    for (uint64_t i = 0; i < n; i++) {
        rv(&a); rv(&b); rv(&c);
        cmp(a, b, c);
    }
    for (int k = 0; k < 100000; k++) {
        ppc_avr_t r1, r2, x, y, z;
        rv(&a); rv(&b); rv(&c);
        for (int m = 0; m < 4; m++) {
            x = a; y = b; z = c;
            helper_VPERM(&r1, &a, &b, &c);
            switch (m) {
            case 0: helper_VPERM_FAST(&x, &x, &y, &z); r2 = x; break;
            case 1: helper_VPERM_FAST(&y, &x, &y, &z); r2 = y; break;
            case 2: helper_VPERM_FAST(&z, &x, &y, &z); r2 = z; break;
            default:
                x = c;
                helper_VPERM(&r1, &c, &c, &c);
                helper_VPERM_FAST(&x, &x, &x, &x); r2 = x; break;
            }
            alias++;
            abad += memcmp(&r1, &r2, 16) != 0;
        }
    }
    printf("aléatoire : %llu vecteurs ; recouvrement r=a/b/c : %llu cas, %llu divergences\n",
           (unsigned long long)n, (unsigned long long)alias, (unsigned long long)abad);
    printf("total : %llu cas, %llu divergences — %s\n", (unsigned long long)(cases + alias),
           (unsigned long long)(bad + abad), bad + abad ? "ÉCHEC" : "OK : mêmes octets");
    return bad + abad != 0;
}
