/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * vfpcmpproof.c — preuve hôte de patches/tcg/0034-ppc-vfp-native-cmp.patch
 * (x-vfp-native-cmp, docs/tcg-g4.md §34).  Lancé par tools/tcg/vfpcmpproof.sh,
 * qui extrait TELS QUELS de target/ppc/int_helper.c de l'arbre :
 *   - les helpers d'origine (VCF, VCMPFP_DO/vcmpbfp, VCT, SATCVT, set_vscr_sat)
 *     dans vfpcmpproof-ref.h ;
 *   - le modèle de l'émetteur x86_64 (bloc « vfp-native-cmp », vfnc_x86()) et
 *     vfp_can_use_fpu() dans vfpcmpproof-model.h ;
 * et compile contre le VRAI objet softfloat de l'arbre construit.
 *
 * Pour chaque vecteur (instruction, uim, vA, vB, état de vec_status : NJ 0/1,
 * inexact posé ou non, no_hardfloat 0/1, drapeaux résiduels) : la séquence
 * d'origine (le helper Rc pour les comparaisons) depuis cet état, et le modèle.
 * Quand le modèle prend le chemin court : vD, VSCR[SAT], vec_status entier
 * (le chemin court n'en touche rien) et CR6 (calculé depuis vD comme le code
 * généré, gen_vfnc_cr6()) doivent être ÉGAUX au bit près.
 *
 * Usage : vfpcmpproof N [graine]  (N vecteurs aléatoires par instruction, uim
 * et état ; plus le catalogue croisé).  VFPCMPPROOF_MUT=k : mutant k du modèle.
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "fpu/softfloat.h"
/* le modèle : SSE/AVX sur x86-64, NEON sur aarch64 (tcg/0038) */
#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "vfpcmpproof-ref.h"
#include "vfpcmpproof-model.h"

static int mut;

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

static const uint32_t FCAT[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00400000, 0x00800000,
    0x80800000, 0x00800001, 0x3f800000, 0xbf800000, 0x3f7fffff, 0x3f000000,
    0xbf000000, 0x3effffff, 0x4b800000, 0x4b7fffff, 0x7f7fffff, 0xff7fffff,
    0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001, 0x7fa00000,
    0x4f000000, 0x4effffff, 0xcf000000, 0xcf000001, 0x4f800000, 0x4f7fffff,
    0xbf7fffff, 0x30000000, 0x2f800000, 0x307fffff, 0xb0000000, 0x5f000000,
    0xdf000000, 0x40490fdb, 0x1f800000, 0x01000000,
};
#define NFCAT (sizeof(FCAT) / sizeof(FCAT[0]))
static const uint32_t ICAT[] = {
    0, 1, 2, 3, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff, 0xfffffffe,
    0x00ffffff, 0x01000000, 0x01000001, 0x01000003, 0xff000001, 0xfefffffd,
    0x7ffffffe, 0x7fffff80, 0x7fffffc0, 0xffffff7f, 0x80000080, 0x55555555,
    0xaaaaaaab, 0x00010000, 0xffff0000, 0x0000ffff, 0x12345678, 0x00ffff80,
    0x0100007f,
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
static uint64_t n_fast[VFNC_END], n_slow[VFNC_END], n_bad[VFNC_END];

static CPUPPCState *env;

/* état de vec_status k : NJ, inexact posé, no_hardfloat, résidus */
static void vs_setup(float_status *s, int k)
{
    memset(s, 0, sizeof(*s));
    set_float_rounding_mode(float_round_nearest_even, s);
    set_float_detect_tininess(float_tininess_before_rounding, s);
    set_float_2nan_prop_rule(float_2nan_prop_ab, s);
    set_float_3nan_prop_rule(float_3nan_prop_acb, s);
    set_float_infzeronan_rule(float_infzeronan_dnan_never, s);
    set_float_default_nan_pattern(0b01000000, s);
    set_flush_to_zero(k & 1, s);
    set_flush_inputs_to_zero(k & 1, s);
    s->no_hardfloat = (k >> 2) & 1;
    set_float_exception_flags(((k & 2) ? float_flag_inexact : 0) |
                              ((k & 8) ? float_flag_overflow : 0), s);
}

/* CR6 de gen_vfnc_cr6() (code généré), depuis vD */
static uint32_t cr6_of(const ppc_avr_t *r, int kind)
{
    uint64_t lo = r->u64[0], hi = r->u64[1];
    uint32_t c = ((lo | hi) == 0) << 1;

    if (kind != VFNC_CMPB) {
        c |= ((lo & hi) == UINT64_MAX) << 3;
    }
    return c;
}

static void check(int kind, int uim, int k, const uint32_t *pa,
                  const uint32_t *pb)
{
    ppc_avr_t a, b, ref, fast;
    float_status before;
    bool sat, ok;
    uint32_t cr6;

    memcpy(a.u32, pa, 16);
    memcpy(b.u32, pb, 16);
    vs_setup(&env->vec_status, k);
    before = env->vec_status;
    env->vscr_sat.u64[0] = env->vscr_sat.u64[1] = 0;
    env->crf[6] = 0xf;
    switch (kind) {
    case VFNC_CMPEQ: helper_vcmpeqfp_dot(env, &ref, &a, &b); break;
    case VFNC_CMPGE: helper_vcmpgefp_dot(env, &ref, &a, &b); break;
    case VFNC_CMPGT: helper_vcmpgtfp_dot(env, &ref, &a, &b); break;
    case VFNC_CMPB: helper_vcmpbfp_dot(env, &ref, &a, &b); break;
    case VFNC_CFUX: helper_vcfux(env, &ref, &b, uim); break;
    case VFNC_CFSX: helper_vcfsx(env, &ref, &b, uim); break;
    case VFNC_CTUXS: helper_vctuxs(env, &ref, &b, uim); break;
    default: helper_vctsxs(env, &ref, &b, uim); break;
    }
    if (!vfnc_x86(kind, uim, vfp_can_use_fpu(&before), a.u32, b.u32,
                  fast.u32, &sat)) {
        n_slow[kind]++;
        return;
    }
    if (mut == 1 && kind == VFNC_CTSXS) {
        fast.u32[0] ^= fast.u32[0] == 0x7fffffff ? 1 : 0;      /* max - 1 */
    }
    if (mut == 2) {
        sat = false;                                           /* SAT oublié */
    }
    n_fast[kind]++;
    cr6 = kind <= VFNC_CMPB ? cr6_of(&fast, kind) : 0xf;
    ok = !memcmp(&fast, &ref, 16) &&
         sat == ((env->vscr_sat.u64[0] | env->vscr_sat.u64[1]) != 0) &&
         !memcmp(&before, &env->vec_status, sizeof(before)) &&
         cr6 == env->crf[6];
    if (!ok && n_bad[kind]++ < 20) {
        printf("DIVERGENCE %s uim %d état %d a=%08x %08x %08x %08x b=%08x %08x"
               " %08x %08x : modèle %08x %08x %08x %08x sat %d cr6 %x / réf"
               " %08x %08x %08x %08x sat %d cr6 %x drapeaux %x->%x\n",
               names[kind], uim, k, pa[0], pa[1], pa[2], pa[3], pb[0], pb[1],
               pb[2], pb[3], fast.u32[0], fast.u32[1], fast.u32[2],
               fast.u32[3], sat, cr6, ref.u32[0], ref.u32[1], ref.u32[2],
               ref.u32[3], (env->vscr_sat.u64[0] | env->vscr_sat.u64[1]) != 0,
               env->crf[6], get_float_exception_flags(&before),
               get_float_exception_flags(&env->vec_status));
    }
}

int main(int argc, char **argv)
{
    uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 0) : 100000;
    uint64_t i, tot = 0, totf = 0, totb = 0;
    uint32_t a[4], b[4];
    int kind, uim, k, x, y, l;

    rs = argc > 2 ? strtoull(argv[2], NULL, 0) : rs;
    mut = getenv("VFPCMPPROOF_MUT") ? atoi(getenv("VFPCMPPROOF_MUT")) : 0;
    env = g_malloc0(sizeof(CPUPPCState));
    for (kind = VFNC_CMPEQ; kind < VFNC_END; kind++) {
        bool cmp = kind <= VFNC_CMPB, cf = kind == VFNC_CFUX || kind == VFNC_CFSX;
        for (uim = 0; uim < (cmp ? 1 : 32); uim++) {
            for (k = 0; k < 16; k++) {
                /* catalogue croisé sur la voie 0 (comparaisons) ou toutes */
                for (x = 0; x < (cf ? (int)NICAT : (int)NFCAT); x++) {
                    for (y = 0; y < (cmp ? (int)NFCAT : 1); y++) {
                        for (l = 0; l < 4; l++) {
                            a[l] = l ? flane() : FCAT[x % NFCAT];
                            b[l] = cf ? ICAT[(x + 7 * l) % NICAT]
                                 : cmp ? (l ? (rnd() & 1 ? a[l] : flane()) : FCAT[y])
                                 : FCAT[(x + 5 * l) % NFCAT];
                        }
                        check(kind, uim, k, a, b);
                        if (cmp) {          /* quatre voies égales : « toutes » */
                            memcpy(b + 1, a + 1, 12);
                            check(kind, uim, k, a, b);
                        }
                    }
                }
                for (i = 0; i < n; i++) {
                    for (l = 0; l < 4; l++) {
                        a[l] = flane();
                        b[l] = cf ? ilane() : flane();
                        if (cmp && (rnd() & 7) == 0) {
                            b[l] = a[l] ^ ((rnd() & 1) ? 0x80000000u : 0);
                        }
                        if (!cmp && !cf && (rnd() & 3) == 0) {
                            /* bords de la saturation, à 2^-uim près */
                            static const uint32_t e[] = { 158, 159, 127, 126 };
                            b[l] = mkf(rnd() & 1, e[rnd() % 4] - uim,
                                       (rnd() & 1) ? 0 : (rnd() & 1) ? 0x7fffff
                                                     : rnd() & 3);
                        }
                    }
                    check(kind, uim, k, a, b);
                }
            }
        }
    }
    for (kind = VFNC_CMPEQ; kind < VFNC_END; kind++) {
        printf("%-9s court %12" PRIu64 "  helpers %12" PRIu64
               "  divergences %" PRIu64 "\n", names[kind], n_fast[kind],
               n_slow[kind], n_bad[kind]);
        tot += n_fast[kind] + n_slow[kind];
        totf += n_fast[kind];
        totb += n_bad[kind];
    }
    printf("total %" PRIu64 " vecteurs, %" PRIu64 " par le chemin court, %"
           PRIu64 " divergences\n", tot, totf, totb);
    return totb != 0;
}
