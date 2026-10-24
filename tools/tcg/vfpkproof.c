// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
/*
 * vfpkproof.c — preuve hôte des lemmes de patches/tcg/0053 (x-vfp-known, docs/tcg-g4.md §42),
 * sur les 2^32 mots :
 *
 *  L1  le test d'opérande de l'émetteur x86_64 (t = x << 1 ; t == 0 ou t - 0x01000000 <=u
 *      0xfdffffff, pvf_zon()) et celui de l'émetteur aarch64 (t == 0 ou 0x01000000 <=u t <u
 *      0xff000000) acceptent exactement les mots que ppc_vfpk_check() tient pour connus
 *      (vfp_zon() de int_helper.c : exposant 1..254, ou ±0) : un opérande connu passerait le
 *      test omis ;
 *  L2  un mot qui passe le test de résultat (x86_64 : t - 0x01000001 <=u 0xfdfffffe ;
 *      aarch64 : 0x01000000 <u t <u 0xff000000) est vfp_zon() : le chemin court produit des
 *      voies connues ;
 *  L3  voies exemptées : vaddfp/vsubfp de deux zéros donnent un zéro ; vmaddfp/vnmsubfp d'un
 *      produit nul (a ou c zéro, l'autre nul ou normal) et de b nul ou normal donnent un mot
 *      vfp_zon(), pour tout b (2^32), a = ±0 et c parmi des nuls et normaux de bord, par le
 *      FPU de l'hôte (FMA3 : vfmadd/vfmsub, comme l'émetteur, puis la négation de vnmsubfp).
 *
 *   cc -O2 -mavx -mfma -o vfpkproof tools/tcg/vfpkproof.c && ./vfpkproof
 */
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int vfp_zon(uint32_t x)          /* int_helper.c, tel quel */
{
    uint32_t e = (x >> 23) & 0xff;
    return (e != 0 && e != 0xff) || !(x & 0x7fffffff);
}

static int op_x86(uint32_t x)
{
    uint32_t t = x << 1;
    return t == 0 || t - 0x01000000u <= 0xfdffffffu;
}

static int op_a64(uint32_t x)
{
    uint32_t t = x << 1;
    return t == 0 || (t >= 0x01000000u && t < 0xff000000u);
}

static int res_x86(uint32_t x)
{
    uint32_t t = x << 1;
    return t - 0x01000001u <= 0xfdfffffeu;
}

static int res_a64(uint32_t x)
{
    uint32_t t = x << 1;
    return t > 0x01000000u && t < 0xff000000u;
}

static float f_of(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }
static uint32_t u_of(float f) { uint32_t x; memcpy(&x, &f, 4); return x; }

int main(void)
{
    uint64_t bad1 = 0, bad2 = 0, nzon = 0, nres = 0, bad3 = 0, n3 = 0;
    uint32_t x = 0;
    static const uint32_t cs[] = { 0x00000000, 0x80000000, 0x3f800000, 0xbf800000,
                                   0x00800000, 0x80800000, 0x7f7fffff, 0xff7fffff,
                                   0x3f7fffff, 0x0d800000 };

    do {
        int z = vfp_zon(x);
        nzon += z;
        if (op_x86(x) != z || op_a64(x) != z) {
            if (bad1++ < 5) printf("L1 écart %08x\n", x);
        }
        if ((res_x86(x) && !z) || (res_a64(x) && !z) || res_x86(x) != res_a64(x)) {
            if (bad2++ < 5) printf("L2 écart %08x\n", x);
        }
        nres += res_x86(x);
    } while (++x != 0);
    printf("L1 : 2^32 mots, %llu nuls ou normaux, test x86_64 = test aarch64 = vfp_zon : %llu écarts\n",
           (unsigned long long)nzon, (unsigned long long)bad1);
    printf("L2 : %llu mots passent le test de résultat (x86_64 = aarch64), tous vfp_zon : %llu écarts\n",
           (unsigned long long)nres, (unsigned long long)bad2);

    /* L3 : sommes de zéros */
    for (int i = 0; i < 4; i++) {
        float a = f_of(i & 1 ? 0x80000000 : 0), b = f_of(i & 2 ? 0x80000000 : 0);
        n3 += 2;
        bad3 += !vfp_zon(u_of(a + b)) + !vfp_zon(u_of(a - b));
    }
    /* L3 : produit nul, b quelconque nul ou normal (2^32 b, 4 voies à la fois) */
    for (int ai = 0; ai < 2; ai++) {
        for (unsigned ci = 0; ci < sizeof(cs) / sizeof(cs[0]); ci++) {
            for (int swap = 0; swap < 2; swap++) {
                __m128 va = _mm_castsi128_ps(_mm_set1_epi32(swap ? cs[ci] : (ai ? 0x80000000 : 0)));
                __m128 vc = _mm_castsi128_ps(_mm_set1_epi32(swap ? (ai ? 0x80000000 : 0) : cs[ci]));
                uint32_t b = 0;
                if (swap && !(cs[ci] & 0x7fffffff)) {
                    continue;
                }
                do {
                    uint32_t bb[4] = { b, b + 1, b + 2, b + 3 }, r1[4], r2[4];
                    __m128 vb = _mm_castsi128_ps(_mm_loadu_si128((const __m128i *)bb));
                    __m128 m = _mm_fmadd_ps(va, vc, vb);
                    __m128 s = _mm_xor_ps(_mm_fmsub_ps(va, vc, vb),
                                          _mm_castsi128_ps(_mm_set1_epi32(0x80000000)));
                    _mm_storeu_si128((__m128i *)r1, _mm_castps_si128(m));
                    _mm_storeu_si128((__m128i *)r2, _mm_castps_si128(s));
                    for (int k = 0; k < 4; k++) {
                        if (!vfp_zon(bb[k])) {
                            continue;
                        }
                        n3 += 2;
                        if (!vfp_zon(r1[k]) || !vfp_zon(r2[k])) {
                            if (bad3++ < 5) printf("L3 écart b %08x\n", bb[k]);
                        }
                    }
                    b += 4;
                } while (b != 0);
            }
        }
    }
    printf("L3 : %llu voies exemptées (sommes de zéros, produits nuls par FMA3), toutes vfp_zon : %llu écarts\n",
           (unsigned long long)n3, (unsigned long long)bad3);
    return bad1 || bad2 || bad3;
}
