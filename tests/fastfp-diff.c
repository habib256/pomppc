/*
 * fastfp-diff.c — test différentiel HÔTE du mode « flottant rapide » (x-fast-fp).
 *
 * Ce programme n'a pas besoin d'un invité : il se lie directement à l'objet
 * softfloat de QEMU (celui que le binaire embarque réellement) et compare,
 * opération par opération :
 *
 *   • le résultat et les drapeaux obtenus avec un float_status EXACT
 *     (no_hardfloat = 1, drapeaux à zéro — le comportement d'aujourd'hui) ;
 *   • le résultat et les drapeaux obtenus avec un float_status AMORCÉ
 *     (no_hardfloat = 0, float_flag_inexact déjà posé — ce que
 *     helper_reset_fpstatus() fait en mode rapide).
 *
 * Invariant vérifié : les RÉSULTATS sont identiques au bit près, et les
 * drapeaux aussi *une fois float_flag_inexact masqué* — l'inexact est
 * justement l'unique information que le mode rapide sacrifie (FPSCR[XX] est
 * collant et déjà posé quand l'amorçage s'enclenche ; FPSCR[FI] devient 1).
 *
 * Compilation : voir tests/run-all.sh (section « flottant rapide »). Il faut
 * l'arbre QEMU patché, pour ses en-têtes et pour fpu_softfloat.c.o :
 *
 *   cc -O2 -Wall $(flags -I/-iquote de l'arbre) tests/fastfp-diff.c \
 *      <build>/libqemu-ppc-softmmu.a.p/fpu_softfloat.c.o -lm -o fastfp-diff
 *
 * Usage : ./fastfp-diff [itérations] [graine]
 */
#include "qemu/osdep.h"
#include "fpu/softfloat.h"

static int failures;
static unsigned long long fast_eligible, total_cases;

/* --- générateur pseudo-aléatoire reproductible (xorshift64*) --- */
static uint64_t rng_state = 0x243F6A8885A308D3ULL;

static uint64_t rnd64(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static uint32_t rnd32(void)
{
    return (uint32_t)(rnd64() >> 32);
}

/* --- fabrication d'un float_status --- */
static float_status mkstatus(bool primed, FloatRoundMode rm, bool ftz)
{
    float_status s;

    memset(&s, 0, sizeof(s));
    set_float_detect_tininess(float_tininess_before_rounding, &s);
    set_float_2nan_prop_rule(float_2nan_prop_ab, &s);
    set_float_rounding_mode(rm, &s);
    s.flush_to_zero = ftz;
    s.flush_inputs_to_zero = ftz;
    /*
     * primed = ce que voit softfloat en mode rapide : hardfloat autorisé ET
     * inexact déjà posé. Sinon : exactement l'état d'aujourd'hui pour PPC.
     */
    s.no_hardfloat = !primed;
    s.float_exception_flags = primed ? float_flag_inexact : 0;
    return s;
}

/* --- le prédicat du chemin rapide, recopié pour mesurer la couverture --- */
static bool is_f32_zon(float64 a)
{
    uint64_t frac = a & ((1ULL << 52) - 1);
    int exp = (int)((a >> 52) & 0x7ff);

    if (exp == 0) {
        return frac == 0;
    }
    return (frac & ((1ULL << 29) - 1)) == 0 && exp >= 0x381 && exp <= 0x47e;
}

static void report(const char *op, const char *args,
                   uint64_t re, uint64_t rf, unsigned fe, unsigned ff)
{
    if (failures < 20) {
        printf("  ✘ %s(%s)\n     exact = %016llx fl=%04x\n"
               "     rapide= %016llx fl=%04x\n",
               op, args, (unsigned long long)re, fe,
               (unsigned long long)rf, ff);
    }
    failures++;
}

/* Les drapeaux comparés : tout sauf l'inexact (posé d'office en mode amorcé). */
#define CMP_FLAGS(s) ((unsigned)((s).float_exception_flags & ~float_flag_inexact))

/* --- comparateurs --- */
typedef float64 (*f64r32_op2)(float64, float64, float_status *);
typedef float64 (*f64r32_op1)(float64, float_status *);

static void chk2_64r32(const char *op, f64r32_op2 fn, float64 a, float64 b,
                       FloatRoundMode rm)
{
    float_status se = mkstatus(false, rm, false);
    float_status sf = mkstatus(true, rm, false);
    float64 re = fn(a, b, &se);
    float64 rf = fn(a, b, &sf);

    total_cases++;
    if (is_f32_zon(a) && is_f32_zon(b)) {
        fast_eligible++;
    }
    if (re != rf || CMP_FLAGS(se) != CMP_FLAGS(sf)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%016llx,%016llx",
                 (unsigned long long)a, (unsigned long long)b);
        report(op, buf, re, rf, CMP_FLAGS(se), CMP_FLAGS(sf));
    }
}

static void chk1_64r32(const char *op, f64r32_op1 fn, float64 a,
                       FloatRoundMode rm)
{
    float_status se = mkstatus(false, rm, false);
    float_status sf = mkstatus(true, rm, false);
    float64 re = fn(a, &se);
    float64 rf = fn(a, &sf);

    total_cases++;
    if (is_f32_zon(a)) {
        fast_eligible++;
    }
    if (re != rf || CMP_FLAGS(se) != CMP_FLAGS(sf)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)a);
        report(op, buf, re, rf, CMP_FLAGS(se), CMP_FLAGS(sf));
    }
}

static void chk3_64r32(const char *op, float64 a, float64 b, float64 c,
                       int flags, FloatRoundMode rm)
{
    float_status se = mkstatus(false, rm, false);
    float_status sf = mkstatus(true, rm, false);
    float64 re = float64r32_muladd(a, b, c, flags, &se);
    float64 rf = float64r32_muladd(a, b, c, flags, &sf);

    total_cases++;
    if (is_f32_zon(a) && is_f32_zon(b) && is_f32_zon(c)) {
        fast_eligible++;
    }
    if (re != rf || CMP_FLAGS(se) != CMP_FLAGS(sf)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%016llx,%016llx,%016llx fl=%d",
                 (unsigned long long)a, (unsigned long long)b,
                 (unsigned long long)c, flags);
        report(op, buf, re, rf, CMP_FLAGS(se), CMP_FLAGS(sf));
    }
}

/* --- double précision et AltiVec (float64_xxx et float32_xxx) --- */
static void chk2_64(const char *op,
                    float64 (*fn)(float64, float64, float_status *),
                    float64 a, float64 b)
{
    float_status se = mkstatus(false, float_round_nearest_even, false);
    float_status sf = mkstatus(true, float_round_nearest_even, false);
    float64 re = fn(a, b, &se);
    float64 rf = fn(a, b, &sf);

    total_cases++;
    if (re != rf || CMP_FLAGS(se) != CMP_FLAGS(sf)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%016llx,%016llx",
                 (unsigned long long)a, (unsigned long long)b);
        report(op, buf, re, rf, CMP_FLAGS(se), CMP_FLAGS(sf));
    }
}

static void chk2_32(const char *op,
                    float32 (*fn)(float32, float32, float_status *),
                    float32 a, float32 b, bool ftz)
{
    float_status se = mkstatus(false, float_round_nearest_even, ftz);
    float_status sf = mkstatus(true, float_round_nearest_even, ftz);
    float32 re = fn(a, b, &se);
    float32 rf = fn(a, b, &sf);

    total_cases++;
    if (re != rf || CMP_FLAGS(se) != CMP_FLAGS(sf)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%08x,%08x%s",
                 (unsigned)a, (unsigned)b, ftz ? " NJ" : "");
        report(op, buf, re, rf, CMP_FLAGS(se), CMP_FLAGS(sf));
    }
}

static void chk3_32(const char *op, float32 a, float32 b, float32 c,
                    int flags, bool ftz)
{
    float_status se = mkstatus(false, float_round_nearest_even, ftz);
    float_status sf = mkstatus(true, float_round_nearest_even, ftz);
    float32 re = float32_muladd(a, b, c, flags, &se);
    float32 rf = float32_muladd(a, b, c, flags, &sf);

    total_cases++;
    if (re != rf || CMP_FLAGS(se) != CMP_FLAGS(sf)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%08x,%08x,%08x fl=%d", (unsigned)a,
                 (unsigned)b, (unsigned)c, flags);
        report(op, buf, re, rf, CMP_FLAGS(se), CMP_FLAGS(sf));
    }
}

/* --- catalogue de cas limites (motifs binaires float64) --- */
static const uint64_t catalog64[] = {
    0x0000000000000000ULL, /* +0                                   */
    0x8000000000000000ULL, /* -0                                   */
    0x3ff0000000000000ULL, /* 1.0                                  */
    0xbff0000000000000ULL, /* -1.0                                 */
    0x4000000000000000ULL, /* 2.0                                  */
    0x3fe0000000000000ULL, /* 0.5                                  */
    0x3ff0000000000001ULL, /* 1 + 2^-52 : pas représentable en simple */
    0x3ff0000020000000ULL, /* 1 + 2^-27 : simple exact             */
    0x3810000000000000ULL, /* FLT_MIN                              */
    0xb810000000000000ULL, /* -FLT_MIN                             */
    0x380fffffc0000000ULL, /* plus grand dénormal simple           */
    0x36a0000000000000ULL, /* FLT_TRUE_MIN (2^-149)                */
    0x47efffffe0000000ULL, /* FLT_MAX                              */
    0xc7efffffe0000000ULL, /* -FLT_MAX                             */
    0x47f0000000000000ULL, /* 2^128 : déborde en simple            */
    0x0010000000000000ULL, /* DBL_MIN                              */
    0x0000000000000001ULL, /* dénormal double minimal              */
    0x7fefffffffffffffULL, /* DBL_MAX                              */
    0x7ff0000000000000ULL, /* +inf                                 */
    0xfff0000000000000ULL, /* -inf                                 */
    0x7ff8000000000000ULL, /* qNaN                                 */
    0xfff8000000000000ULL, /* -qNaN                                */
    0x7ff0000000000001ULL, /* sNaN (charge utile minimale)         */
    0x7ff4000000000000ULL, /* sNaN                                 */
    0x4170000000000000ULL, /* 2^24                                 */
    0x416fffffe0000000ULL, /* 2^24 - 1                             */
    0x4170000010000000ULL, /* 2^24 + 1 : pas représentable en simple */
    0x3e70000000000000ULL, /* 2^-24                                */
    0x3e60000000000000ULL, /* 2^-25 : le demi-ulp de 1.0 en simple */
    0x3e50000000000000ULL, /* 2^-26                                */
    0xbe60000000000000ULL, /* -2^-25                               */
    0x7e37e43c8800759cULL, /* 1e300                                */
    0x01a56e1fc2f8f359ULL, /* 1e-300                               */
};
#define NCAT (sizeof(catalog64) / sizeof(catalog64[0]))

/* --- générateurs --- */

/* un float32 normal (exposant 1..254) élargi en float64, exactement */
static float64 gen_f32_normal(void)
{
    union { uint32_t u; float f; } c;
    union { uint64_t u; double d; } r;

    c.u = (rnd32() & 0x807fffffU) | ((1 + (rnd32() % 254)) << 23);
    r.d = c.f;
    return r.u;
}

/* un float32 quelconque (y compris dénormal/zéro), élargi ; NaN/inf exclus */
static float64 gen_f32_any(void)
{
    union { uint32_t u; float f; } c;
    union { uint64_t u; double d; } r;

    c.u = rnd32();
    if (((c.u >> 23) & 0xff) == 0xff) {
        c.u &= ~(1u << 30);              /* évite inf/NaN : la conversion */
    }                                    /* hôte silencierait un sNaN     */
    r.d = c.f;
    return r.u;
}

static float64 gen_f64_any(void)
{
    return rnd64();
}

/* float64 « presque simple » : mantisse basse à quelques bits près */
static float64 gen_f64_near_f32(void)
{
    float64 v = gen_f32_normal();
    return v ^ (1ULL << (rnd32() % 29));
}

static float64 gen_value(void)
{
    switch (rnd32() % 8) {
    case 0: case 1: case 2: case 3: return gen_f32_normal();
    case 4:                         return gen_f32_any();
    case 5:                         return gen_f64_near_f32();
    case 6:                         return gen_f64_any();
    default:                        return catalog64[rnd32() % NCAT];
    }
}

/*
 * Paire conçue pour tomber sur un demi-ulp en simple précision : b vaut à peu
 * près 2^-24 ou 2^-25 fois a, c'est-à-dire le bit de garde de l'addition.
 */
static void gen_tie_pair(float64 *pa, float64 *pb)
{
    union { uint32_t u; float f; } a, b;
    union { uint64_t u; double d; } ra, rb;
    int ea = 40 + (int)(rnd32() % 170);   /* exposant à distance des bords */
    int shift = 23 + (int)(rnd32() % 4);
    int eb = ea - shift;
    uint32_t fa = rnd32() & 0x7fffff;
    uint32_t fb = (rnd32() % 4 == 0) ? 0 : (rnd32() & 0x7fffff);

    a.u = ((rnd32() & 1u) << 31) | ((uint32_t)ea << 23) | fa;
    b.u = ((rnd32() & 1u) << 31) | ((uint32_t)eb << 23) | fb;
    ra.d = a.f;
    rb.d = b.f;
    *pa = ra.u;
    *pb = rb.u;
}

/*
 * Paire conçue pour que le produit tombe sur un demi-ulp : mantisses courtes,
 * dont le produit tient sur 25 bits pile.
 */
static void gen_mul_tie_pair(float64 *pa, float64 *pb)
{
    union { uint32_t u; float f; } a, b;
    union { uint64_t u; double d; } ra, rb;
    int ea = 60 + (int)(rnd32() % 130);
    int eb = 60 + (int)(rnd32() % 130);
    int ka = 8 + (int)(rnd32() % 16);
    int kb = 8 + (int)(rnd32() % 16);

    a.u = ((rnd32() & 1u) << 31) | ((uint32_t)ea << 23) |
          (rnd32() & (0x7fffffu << ka) & 0x7fffffu);
    b.u = ((rnd32() & 1u) << 31) | ((uint32_t)eb << 23) |
          (rnd32() & (0x7fffffu << kb) & 0x7fffffu);
    ra.d = a.f;
    rb.d = b.f;
    *pa = ra.u;
    *pb = rb.u;
}

static const int madd_flags[] = {
    0,
    float_muladd_negate_c,
    float_muladd_negate_result,
    float_muladd_negate_c | float_muladd_negate_result,
    float_muladd_negate_product,
    float_muladd_negate_product | float_muladd_negate_c,
};
#define NMF (sizeof(madd_flags) / sizeof(madd_flags[0]))

static const FloatRoundMode rmodes[] = {
    float_round_nearest_even,
    float_round_to_zero,
    float_round_up,
    float_round_down,
};

/* --- passes --- */

static void pass_catalog(void)
{
    size_t i, j, k;

    for (i = 0; i < NCAT; i++) {
        for (j = 0; j < NCAT; j++) {
            size_t r;
            for (r = 0; r < sizeof(rmodes) / sizeof(rmodes[0]); r++) {
                chk2_64r32("float64r32_add", float64r32_add,
                           catalog64[i], catalog64[j], rmodes[r]);
                chk2_64r32("float64r32_sub", float64r32_sub,
                           catalog64[i], catalog64[j], rmodes[r]);
                chk2_64r32("float64r32_mul", float64r32_mul,
                           catalog64[i], catalog64[j], rmodes[r]);
                chk2_64r32("float64r32_div", float64r32_div,
                           catalog64[i], catalog64[j], rmodes[r]);
            }
            for (k = 0; k < NCAT; k++) {
                size_t f;
                for (f = 0; f < NMF; f++) {
                    chk3_64r32("float64r32_muladd", catalog64[i], catalog64[j],
                               catalog64[k], madd_flags[f],
                               float_round_nearest_even);
                }
            }
        }
        chk1_64r32("float64r32_sqrt", float64r32_sqrt, catalog64[i],
                   float_round_nearest_even);
    }
}

static void pass_random(unsigned long iterations)
{
    unsigned long n;

    for (n = 0; n < iterations; n++) {
        float64 a = gen_value(), b = gen_value(), c = gen_value();
        FloatRoundMode rm = rmodes[rnd32() % 4];

        chk2_64r32("float64r32_add", float64r32_add, a, b, rm);
        chk2_64r32("float64r32_sub", float64r32_sub, a, b, rm);
        chk2_64r32("float64r32_mul", float64r32_mul, a, b, rm);
        chk2_64r32("float64r32_div", float64r32_div, a, b, rm);
        chk1_64r32("float64r32_sqrt", float64r32_sqrt, a, rm);
        chk3_64r32("float64r32_muladd", a, b, c, madd_flags[rnd32() % NMF], rm);

        /* double précision : fadd/fsub/fmul/fdiv/fsqrt */
        chk2_64("float64_add", float64_add, a, b);
        chk2_64("float64_sub", float64_sub, a, b);
        chk2_64("float64_mul", float64_mul, a, b);
        chk2_64("float64_div", float64_div, a, b);
        {
            float_status se = mkstatus(false, float_round_nearest_even, false);
            float_status sf = mkstatus(true, float_round_nearest_even, false);
            float64 re = float64_sqrt(a, &se);
            float64 rf = float64_sqrt(a, &sf);
            total_cases++;
            if (re != rf || CMP_FLAGS(se) != CMP_FLAGS(sf)) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)a);
                report("float64_sqrt", buf, re, rf,
                       CMP_FLAGS(se), CMP_FLAGS(sf));
            }
        }
        {
            float_status se = mkstatus(false, float_round_nearest_even, false);
            float_status sf = mkstatus(true, float_round_nearest_even, false);
            int fl = madd_flags[rnd32() % NMF];
            float64 re = float64_muladd(a, b, c, fl, &se);
            float64 rf = float64_muladd(a, b, c, fl, &sf);
            total_cases++;
            if (re != rf || CMP_FLAGS(se) != CMP_FLAGS(sf)) {
                char buf[80];
                snprintf(buf, sizeof(buf), "%016llx,%016llx,%016llx fl=%d",
                         (unsigned long long)a, (unsigned long long)b,
                         (unsigned long long)c, fl);
                report("float64_muladd", buf, re, rf,
                       CMP_FLAGS(se), CMP_FLAGS(sf));
            }
        }

        /* AltiVec : float32_*, avec et sans NJ (flush-to-zero) */
        {
            float32 x = rnd32(), y = rnd32(), z = rnd32();
            bool ftz = (n & 1) != 0;

            chk2_32("float32_add", float32_add, x, y, ftz);
            chk2_32("float32_sub", float32_sub, x, y, ftz);
            chk2_32("float32_mul", float32_mul, x, y, ftz);
            chk2_32("float32_div", float32_div, x, y, ftz);
            chk3_32("float32_muladd", x, y, z, madd_flags[rnd32() % NMF], ftz);
            {
                float_status se = mkstatus(false, float_round_nearest_even, ftz);
                float_status sf = mkstatus(true, float_round_nearest_even, ftz);
                float32 re = float32_sqrt(x, &se);
                float32 rf = float32_sqrt(x, &sf);
                total_cases++;
                if (re != rf || CMP_FLAGS(se) != CMP_FLAGS(sf)) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%08x", (unsigned)x);
                    report("float32_sqrt", buf, re, rf,
                           CMP_FLAGS(se), CMP_FLAGS(sf));
                }
            }
        }
    }
}

static void pass_ties(unsigned long iterations)
{
    unsigned long n;

    for (n = 0; n < iterations; n++) {
        float64 a, b, c;

        gen_tie_pair(&a, &b);
        chk2_64r32("float64r32_add(tie)", float64r32_add, a, b,
                   float_round_nearest_even);
        chk2_64r32("float64r32_sub(tie)", float64r32_sub, a, b,
                   float_round_nearest_even);

        gen_mul_tie_pair(&a, &b);
        chk2_64r32("float64r32_mul(tie)", float64r32_mul, a, b,
                   float_round_nearest_even);
        chk2_64r32("float64r32_div(tie)", float64r32_div, a, b,
                   float_round_nearest_even);
        chk1_64r32("float64r32_sqrt(tie)", float64r32_sqrt, a,
                   float_round_nearest_even);

        /* a*b + c avec c voisin de -(a*b) : annulations et demi-ulp */
        gen_mul_tie_pair(&a, &b);
        c = gen_f32_normal();
        chk3_64r32("float64r32_muladd(tie)", a, b, c,
                   madd_flags[rnd32() % NMF], float_round_nearest_even);
    }
}

/*
 * Petit chronométrage : il ne prouve pas l'exactitude, il prouve que le
 * chemin rapide est RÉELLEMENT emprunté (sans quoi tout ce qui précède
 * comparerait le logiciel à lui-même).
 */
static bool pass_timing(void)
{
    /* Jeu de valeurs petit (résident en cache) : on chronomètre le calcul. */
    enum { NV = 4096, N = 8000000 };
    float64 *va = malloc(NV * sizeof(float64));
    float_status se, sf;
    struct timespec t0, t1, t2;
    double d_exact, d_fast;
    float64 acc;
    int i;

    if (!va) {
        return true;
    }
    for (i = 0; i < NV; i++) {
        va[i] = gen_f32_normal();
    }

    se = mkstatus(false, float_round_nearest_even, false);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    acc = 0x3ff0000000000000ULL;
    for (i = 0; i < N; i++) {
        acc = float64r32_mul(va[i & (NV - 1)], va[(i * 7 + 3) & (NV - 1)], &se);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    sf = mkstatus(true, float_round_nearest_even, false);
    for (i = 0; i < N; i++) {
        acc = float64r32_mul(va[i & (NV - 1)], va[(i * 7 + 3) & (NV - 1)], &sf);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    (void)acc;

    d_exact = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    d_fast  = (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec) / 1e9;
    free(va);

    printf("  float64r32_mul : logiciel %.3f s, rapide %.3f s (x%.2f) "
           "sur %d appels\n", d_exact, d_fast, d_exact / d_fast, N);
    if (d_fast >= d_exact) {
        printf("  ✘ le chemin rapide n'est pas plus rapide : il n'est "
               "probablement pas emprunté\n");
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    unsigned long iterations = (argc > 1) ? strtoul(argv[1], NULL, 0) : 300000;
    bool timing_ok;

    if (argc > 2) {
        rng_state = strtoull(argv[2], NULL, 0);
    }

    printf("fastfp-diff : exact (no_hardfloat) vs amorcé (inexact posé)\n");
    pass_catalog();
    pass_random(iterations);
    pass_ties(iterations);
    timing_ok = pass_timing();

    printf("  %llu cas comparés, dont %llu éligibles au chemin rapide "
           "(%.1f %%)\n", total_cases, fast_eligible,
           total_cases ? 100.0 * fast_eligible / total_cases : 0.0);
    if (failures) {
        printf("  ✘ %d divergence(s)\n", failures);
        return 1;
    }
    if (!timing_ok) {
        return 1;
    }
    printf("  ✔ aucune divergence\n");
    return 0;
}
