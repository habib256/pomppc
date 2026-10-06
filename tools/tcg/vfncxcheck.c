/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * vfncxcheck.c — le modèle de x-vfp-native-cmp (tcg/0034, x86_64) contre son
 * pendant NEON (tcg/0038, aarch64), docs/parite-arm64-0031-0035.md.
 *
 * Lancé par tools/tcg/vfncxcheck.sh, qui extrait TEL QUEL le bloc
 * « vfp-native-cmp » de target/ppc/int_helper.c (vfnc_x86() : branche SSE/AVX
 * sur x86-64, branche NEON sur aarch64) dans vfncxcheck-model.h.  Sans QEMU :
 * mêmes vecteurs (graine fixe, catalogue puis aléatoire), chaque sorte, chaque
 * uim, porte ouverte et fermée ; une empreinte FNV par (sorte, uim) de
 * (chemin court ou non, vD, SAT).  Les deux hôtes doivent sortir le MÊME
 * texte : le modèle x86 étant prouvé contre les helpers (vfpcmpproof.sh),
 * l'égalité étend la preuve au modèle NEON sur ces vecteurs.
 *
 * Usage : vfncxcheck N [sorte uim]   (avec sorte et uim : une ligne par
 * vecteur, pour trouver le premier écart entre deux hôtes)
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif
#ifndef HOST_BIG_ENDIAN
#define HOST_BIG_ENDIAN 0
#endif

#pragma GCC diagnostic ignored "-Wunused-function"
#include "vfncxcheck-model.h"

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void)
{
    rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27;
    return rs * 2685821657736338717ull;
}
static uint32_t mkf(uint32_t s, uint32_t e, uint32_t f)
{
    return s << 31 | (e & 0xff) << 23 | (f & 0x7fffff);
}

/* le catalogue de vfpcmpproof.c, plus les bords de saturation */
static const uint32_t FCAT[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00400000, 0x00800000,
    0x80800000, 0x00800001, 0x3f800000, 0xbf800000, 0x3f7fffff, 0x3f000000,
    0xbf000000, 0x3effffff, 0x4b800000, 0x4b7fffff, 0x7f7fffff, 0xff7fffff,
    0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001, 0x7fa00000,
    0x4f000000, 0x4effffff, 0xcf000000, 0xcf000001, 0x4f800000, 0x4f7fffff,
    0xbf7fffff, 0x30000000, 0x2f800000, 0x307fffff, 0xb0000000, 0x5f000000,
    0xdf000000, 0x40490fdb, 0x1f800000, 0x01000000, 0xbf800001, 0xceffffff,
    0x4f000001, 0x4f7fffff, 0x7effffff, 0xff7ffffe,
};
#define NFCAT (sizeof(FCAT) / sizeof(FCAT[0]))
static const uint32_t ICAT[] = {
    0, 1, 2, 3, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff, 0xfffffffe,
    0x00ffffff, 0x01000000, 0x01000001, 0x01000003, 0xff000001, 0xfefffffd,
    0x7ffffffe, 0x7fffff80, 0x7fffffc0, 0xffffff7f, 0x80000080, 0x55555555,
    0xaaaaaaab, 0x00010000, 0xffff0000, 0x0000ffff, 0x12345678, 0x00ffff80,
    0x0100007f, 0xffffff80, 0xffffff7f, 0x80000040,
};
#define NICAT (sizeof(ICAT) / sizeof(ICAT[0]))

static uint32_t flane(void)
{
    uint64_t r = rnd();
    uint32_t f = rnd() & 0x7fffff, s = r & 1, k = (r >> 8) % 100, e = r >> 16;

    if (k < 10) return FCAT[(r >> 24) % NFCAT];
    if (k < 45) return mkf(s, 100 + e % 60, f);             /* doux, 2^-27..2^32 */
    if (k < 60) return mkf(s, 1 + e % 254, f);
    if (k < 66) return mkf(s, 0, f | 1);                     /* dénormal */
    if (k < 70) return mkf(s, 0, 0);
    if (k < 74) return mkf(s, 0xff, 0);
    if (k < 78) return mkf(s, 0xff, f | 1);                  /* NaN */
    if (k < 90) return mkf(s, 150 + e % 12, f);              /* bords 2^24..2^34 */
    return mkf(s, 157 + e % 3, (r & 0x100) ? 0x7fffff - (f & 0xff) : f & 0xff);
}
static uint32_t ilane(void)
{
    uint64_t r = rnd();
    uint32_t k = r % 8;

    if (k < 2) return ICAT[(r >> 8) % NICAT];
    if (k < 4) return (uint32_t)rnd() >> ((r >> 8) % 32);
    if (k < 5) return (uint32_t)-(int32_t)((uint32_t)rnd() >> ((r >> 8) % 32));
    return rnd();
}

static const char *const names[VFNC_END] = {
    [VFNC_CMPEQ] = "vcmpeqfp", [VFNC_CMPGE] = "vcmpgefp",
    [VFNC_CMPGT] = "vcmpgtfp", [VFNC_CMPB] = "vcmpbfp",
    [VFNC_CFUX] = "vcfux", [VFNC_CFSX] = "vcfsx",
    [VFNC_CTUXS] = "vctuxs", [VFNC_CTSXS] = "vctsxs",
};

static uint64_t fnv = 0xcbf29ce484222325ull;
static void mix(uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        fnv = (fnv ^ ((v >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    }
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 100000;
    int dk = argc > 3 ? atoi(argv[2]) : -1, du = argc > 3 ? atoi(argv[3]) : -1;
    uint64_t total = 0, tshort = 0, all = 0xcbf29ce484222325ull;

    for (int kind = VFNC_CMPEQ; kind < VFNC_END; kind++) {
        bool conv = kind >= VFNC_CFUX, cf = kind == VFNC_CFUX ||
                                            kind == VFNC_CFSX;
        for (int uim = 0; uim < (conv ? 32 : 1); uim++) {
            uint64_t nshort = 0;
            long ncat = conv ? (cf ? NICAT : NFCAT) : NFCAT * NFCAT;

            rs = 0x9e3779b97f4a7c15ull ^ ((uint64_t)kind << 40) ^ uim;
            fnv = 0xcbf29ce484222325ull;
            for (long i = 0; i < ncat + n; i++) {
                uint32_t a[4], b[4], r[4] = { 0 };
                bool sat = false, ok, gate = cf ? (i & 1) : false;

                for (int l = 0; l < 4; l++) {
                    if (i < ncat) {     /* catalogue : la voie 0 le parcourt */
                        a[l] = l ? flane() : FCAT[(i / NFCAT) % NFCAT];
                        b[l] = l ? (cf ? ilane() : flane())
                                 : cf ? ICAT[i % NICAT] : FCAT[i % NFCAT];
                        if (cf) {
                            gate = true;
                        }
                    } else {
                        a[l] = flane();
                        b[l] = cf ? ilane() : flane();
                    }
                }
                ok = vfnc_x86(kind, uim, gate, a, b, r, &sat);
                nshort += ok;
                mix(ok | sat << 1);
                for (int l = 0; l < 4; l++) {
                    mix(ok ? r[l] : 0);
                }
                if (kind == dk && uim == du) {
                    printf("%ld a %08x %08x %08x %08x b %08x %08x %08x %08x"
                           " -> %d %08x %08x %08x %08x sat %d\n", i,
                           a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], ok,
                           ok ? r[0] : 0, ok ? r[1] : 0, ok ? r[2] : 0,
                           ok ? r[3] : 0, sat);
                }
            }
            if (dk < 0) {
                printf("%-8s uim %2d : %8ld vecteurs, %8llu chemin court,"
                       " empreinte %016llx\n", names[kind], uim, ncat + n,
                       (unsigned long long)nshort, (unsigned long long)fnv);
            }
            total += ncat + n;
            tshort += nshort;
            all = (all ^ fnv) * 0x100000001b3ull;
        }
    }
    if (dk < 0) {
        printf("total %llu vecteurs, %llu par le chemin court, empreinte"
               " %016llx\n", (unsigned long long)total,
               (unsigned long long)tshort, (unsigned long long)all);
    }
    return 0;
}
