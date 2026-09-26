/*
 * fpproof.c — preuve hôte de patches/tcg/0007-ppc-fp-inline.patch (x-fp-inline).
 * Lancé par tools/tcg/fpproof.sh, qui le compile avec les -I/-D de
 * target/ppc/fpu_helper.c de l'arbre et le lie aux VRAIS objets de l'arbre
 * construit : target_ppc_fpu_helper.c.o (les helpers d'origine ET
 * helper_fp32_fast, le calcul du chemin court), target_ppc_cpu.c.o
 * (ppc_store_fpscr) et fpu_softfloat.c.o.  Le bloc « fp-inline » de
 * fpu_helper.c (portes, FPRF, fcmpu) est extrait TEL QUEL dans
 * fpproof-fast.h (helper_fp32_fast y est renommé model_fp32_fast).
 *
 * Pour chaque vecteur (opération, opérandes, FPSCR de départ) :
 *   référence = la séquence d'origine du code généré, en mode x-fast-fp :
 *               reset_fpstatus, helper_FxxxS, fprf_check_float64
 *               (fcmpu : reset_fpstatus, helper_fcmpu, float_check_status)
 *   chemin court = ce que le code généré de x-fp-inline fait quand sa porte
 *               est ouverte : fpi_gate(), r = helper_fp32_fast() != FPI_FAIL,
 *               frT = r, FPSCR = fpi_fpscr_arith(), drapeaux = inexact.
 * Quand le chemin court est pris : résultat, FPSCR entier, drapeaux
 * softfloat, exception levée ou non et exception_index doivent être ÉGAUX
 * au bit près.  Quand il ne l'est pas, le code généré exécute la séquence
 * d'origine elle-même (rien à comparer) ; on compte les deux.
 *
 * Usage : fpproof N [graine]  (N vecteurs aléatoires par opération et par
 * famille de FPSCR ; plus le catalogue croisé).
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "exec/helper-proto.h"
#include "fpu/softfloat.h"
#include <float.h>
#include <math.h>
#include <setjmp.h>

#define helper_fp32_fast model_fp32_fast
#include "fpproof-fast.h"
#undef helper_fp32_fast
uint64_t helper_fp32_fast(uint64_t a, uint64_t b, uint64_t c, uint32_t op);

/* --- ce que target_ppc_cpu.c.o et fpu_helper.c.o attendent ------------- */
bool tcg_allowed = true;
int qemu_loglevel;
void qemu_log(const char *fmt, ...) { }
void hreg_compute_hflags(CPUPPCState *env) { }
int hreg_store_msr(CPUPPCState *env, target_ulong value, int alter_hv)
{ return 0; }
void ppc_maybe_interrupt(CPUPPCState *env) { }
int cpu_breakpoint_insert(CPUState *cpu, vaddr pc, int flags,
                          CPUBreakpoint **bp) { return 0; }
void cpu_breakpoint_remove_by_ref(CPUState *cpu, CPUBreakpoint *bp) { }
int cpu_watchpoint_insert(CPUState *cpu, vaddr addr, vaddr len, int flags,
                          CPUWatchpoint **wp) { return 0; }
void cpu_watchpoint_remove_by_ref(CPUState *cpu, CPUWatchpoint *wp) { }
ObjectClass *object_get_class(Object *obj) { abort(); }
ObjectClass *object_class_dynamic_cast_assert(ObjectClass *c, const char *t,
                                         const char *f, int l, const char *fn)
{ abort(); }

static jmp_buf jb;
static int raised, raised_code;
void raise_exception_err_ra(CPUPPCState *env, uint32_t exception,
                            uint32_t error_code, uintptr_t raddr)
{
    raised = exception;
    raised_code = error_code;
    longjmp(jb, 1);
}

/* --- état ---------------------------------------------------------------- */
static PowerPCCPU *cpu;
static CPUPPCState *env;

static void env_setup(uint64_t fpscr, uint64_t msr)
{
    memset(&env->fp_status, 0, sizeof(env->fp_status));
    set_float_detect_tininess(float_tininess_before_rounding, &env->fp_status);
    set_float_2nan_prop_rule(float_2nan_prop_ab, &env->fp_status);
    env->fast_fp = true;
    env->fp_status.no_hardfloat = false;
    env->fp_prime_mask = float_flag_inexact;
    env->msr = msr;
    ppc_store_fpscr(env, fpscr);
    /* un reste d'un vecteur précédent dans les drapeaux, comme en vrai */
    env->fp_status.float_exception_flags = float_flag_overflow;
    cpu->parent_obj.exception_index = -1;
    env->error_code = 0;
    raised = 0;
}

typedef struct {
    uint64_t r, fpscr;
    int flags, excp, raised;
    uint32_t cr;
} Out;

static void ref_arith(int op, uint64_t a, uint64_t b, uint64_t c, Out *o)
{
    uint64_t r = 0;

    if (setjmp(jb) == 0) {
        helper_reset_fpstatus(env);
        switch (op) {
        case FPI_ADD: r = helper_FADDS(env, a, b); break;
        case FPI_SUB: r = helper_FSUBS(env, a, b); break;
        case FPI_MUL: r = helper_FMULS(env, a, c); break;
        case FPI_MADD: r = helper_FMADDS(env, a, c, b); break;
        case FPI_MSUB: r = helper_FMSUBS(env, a, c, b); break;
        case FPI_NMADD: r = helper_FNMADDS(env, a, c, b); break;
        default: r = helper_FNMSUBS(env, a, c, b); break;
        }
        helper_fprf_check_float64(env, r);
    }
    o->r = r;
    o->fpscr = env->fpscr;
    o->flags = get_float_exception_flags(&env->fp_status);
    o->excp = cpu->parent_obj.exception_index;
    o->raised = raised;
}

static void ref_fcmpu(uint64_t a, uint64_t b, Out *o)
{
    env->crf[3] = 0xf;
    if (setjmp(jb) == 0) {
        helper_reset_fpstatus(env);
        helper_fcmpu(env, a, b, 3);
        helper_float_check_status(env);
    }
    o->cr = env->crf[3];
    o->fpscr = env->fpscr;
    o->flags = get_float_exception_flags(&env->fp_status);
    o->excp = cpu->parent_obj.exception_index;
    o->raised = raised;
}

/* --- vecteurs ------------------------------------------------------------ */
static uint64_t rs[4];
static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
static uint64_t rnd(void)
{
    uint64_t r = rotl(rs[1] * 5, 7) * 9, t = rs[1] << 17;
    rs[2] ^= rs[0]; rs[3] ^= rs[1]; rs[1] ^= rs[2]; rs[0] ^= rs[3];
    rs[2] ^= t; rs[3] = rotl(rs[3], 45);
    return r;
}

static uint64_t d_of_f(uint32_t f)            /* DOUBLE() de l'ISA */
{
    return helper_todouble(f);
}
static uint64_t d_bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static uint64_t cat[256];
static int ncat;

static void cat_add(uint64_t x)
{
    cat[ncat++] = x;
    cat[ncat++] = x ^ (1ull << 63);
}

static void build_catalog(void)
{
    static const uint32_t f32[] = {
        0x00000000, 0x00000001, 0x00000002, 0x007fffff, 0x00400000,
        0x00800000, 0x00800001, 0x00ffffff, 0x01000000, 0x0c800000,
        0x1f800000, 0x3f800000, 0x3f800001, 0x3f7fffff, 0x3fc00000,
        0x3dcccccd, 0x4b7fffff, 0x4b800000, 0x4b800001, 0x33000000,
        0x33800000, 0x34000000, 0x5f800000, 0x7e800000, 0x7f000000,
        0x7f7fffff, 0x7f7ffffe, 0x7f800000, 0x7fc00000, 0x7fa00000,
        0x7f800001, 0x7fffffff, 0x20000000, 0x1f000000, 0x40490fdb,
    };
    static const double f64[] = {
        0.1, 1.0 / 3.0, 1e-300, 1e300, 4.9406564584124654e-324,
        2.2250738585072014e-308, 1.0000000000000002, 3.4028235677973366e38,
        1.1754942807573643e-38, 1.401298464324817e-45 / 2,
    };
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(f32); i++) {
        cat_add(d_of_f(f32[i]));
    }
    for (i = 0; i < ARRAY_SIZE(f64); i++) {
        cat_add(d_bits(f64[i]));
    }
    /* exposants juste hors de la plage normale simple, bits bas posés */
    cat_add(0x3800000000000000ull);                /* 2^-127 */
    cat_add(0x3810000000000000ull);                /* 2^-126 = FLT_MIN */
    cat_add(0x47e0000000000000ull);                /* 2^127 */
    cat_add(0x47f0000000000000ull);                /* 2^128 */
    cat_add(0x3ff0000010000000ull);                /* 1 + 2^-24 */
    cat_add(0x3ff0000020000000ull);                /* 1 + 2^-23 (simple) */
    cat_add(0x7ff4000000000000ull);                /* sNaN double */
    cat_add(0x7ff0000000000001ull);                /* sNaN minimal */
}

/* un float32 normal (ou zéro) élargi, avec des familles utiles */
static uint64_t rnd_operand(void)
{
    uint64_t x = rnd();
    uint32_t f;

    switch (x & 15) {
    case 0:                                     /* catalogue */
        return cat[(x >> 8) % ncat];
    case 1:                                     /* double quelconque */
        return rnd();
    case 2:                                     /* float32 quelconque */
        return d_of_f((uint32_t)(x >> 32));
    case 3:                                     /* près du débordement */
        f = (uint32_t)(x >> 32) & 0x807fffff;
        f |= (0xf0u + ((x >> 8) & 0xf)) << 23;
        return d_of_f(f);
    case 4:                                     /* près du dénormal */
        f = (uint32_t)(x >> 32) & 0x807fffff;
        f |= ((x >> 8) & 0x1f) << 23;
        return d_of_f(f);
    case 5:                                     /* mantisse courte (demi-ulp) */
        f = (uint32_t)(x >> 32) & 0xff800000;
        f |= ((uint32_t)(x >> 8) & 0xfff) << 11;
        return d_of_f(f);
    case 6:                                     /* float32 à un bit près */
        return d_of_f(((uint32_t)(x >> 32) & 0xbfffffff) | 0x3c000000) ^
               (1ull << ((x >> 8) % 29));
    default:                                    /* float32 normal « doux » */
        f = (uint32_t)(x >> 32) & 0x807fffff;
        f |= (0x60u + ((x >> 8) & 0x3f)) << 23;
        return d_of_f(f);
    }
}

/* FPSCR de départ : famille 0 = porte ouverte (XX, reste au hasard hors
 * porte), 1 = au hasard complet, 2 = porte fermée d'un seul bit (RN,
 * XE, OE, UE, ou XX effacé), avec trappes MSR[FE] au hasard. */
static uint64_t rnd_fpscr(int fam, uint64_t *msr)
{
    uint64_t f = rnd() & FPSCR_MTFS_MASK & 0xffffffffull;
    static const uint64_t closers[] = { 1, 2, 3, FP_XE, FP_OE, FP_UE, 0 };

    *msr = (rnd() & 1) ? (1ull << MSR_FE0) | (1ull << MSR_FE1) : 0;
    switch (fam) {
    case 0:
        return (f & ~(uint64_t)FPI_GATE_MASK) | FP_XX;
    case 1:
        return f;
    default: {
        uint64_t k = closers[rnd() % ARRAY_SIZE(closers)];
        f = (f & ~(uint64_t)FPI_GATE_MASK) | FP_XX;
        return k ? f | k : f & ~(uint64_t)FP_XX;
    }
    }
}

static uint64_t n_fast[FPI_NOPS], n_slow[FPI_NOPS], n_bad[FPI_NOPS];
static uint64_t n_modelbad;
static bool use_model;
static const char *names[FPI_NOPS] = {
    "fadds", "fsubs", "fmuls", "fmadds", "fmsubs", "fnmadds", "fnmsubs",
    "fcmpu",
};

static void check_arith(int op, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t fpscr, uint64_t msr)
{
    Out ref;
    uint64_t r = helper_fp32_fast(a, b, c, op);
    uint64_t rm = model_fp32_fast(a, b, c, op);

    if (use_model) {
        r = rm;                     /* contre-épreuve : modèle muté */
    } else if (r != rm) {
        n_modelbad++;
    }
    env_setup(fpscr, msr);
    fpscr = env->fpscr;             /* tel que ppc_store_fpscr() l'a rangé */
    if (!fpi_gate(fpscr) || r == FPI_FAIL) {
        n_slow[op]++;
        return;
    }
    n_fast[op]++;
    ref_arith(op, a, b, c, &ref);
    if (ref.r != r || ref.fpscr != fpi_fpscr_arith(fpscr, r) ||
        ref.flags != float_flag_inexact || ref.excp != -1 || ref.raised) {
        if (n_bad[op]++ < 20) {
            printf("DIVERGENCE %s a=%016" PRIx64 " b=%016" PRIx64 " c=%016"
                   PRIx64 " fpscr=%08" PRIx64 " : court %016" PRIx64
                   " %08" PRIx64 " / réf %016" PRIx64 " %08" PRIx64
                   " drapeaux %x excp %d levée %d\n", names[op], a, b, c,
                   fpscr, r, fpi_fpscr_arith(fpscr, r), ref.r, ref.fpscr,
                   ref.flags, ref.excp, ref.raised);
        }
    }
}

static void check_fcmpu(uint64_t a, uint64_t b, uint64_t fpscr, uint64_t msr)
{
    Out ref;
    uint32_t cr;

    env_setup(fpscr, msr);
    fpscr = env->fpscr;
    cr = fpi_fcmpu(fpscr, a, b);
    if (cr == 0) {
        n_slow[FPI_CMPU]++;
        return;
    }
    n_fast[FPI_CMPU]++;
    ref_fcmpu(a, b, &ref);
    if (ref.cr != cr || ref.fpscr != fpi_fpscr_fcmpu(fpscr, cr) ||
        ref.flags != float_flag_inexact || ref.excp != -1 || ref.raised) {
        if (n_bad[FPI_CMPU]++ < 20) {
            printf("DIVERGENCE fcmpu a=%016" PRIx64 " b=%016" PRIx64
                   " fpscr=%08" PRIx64 " : court %x / réf %x %08" PRIx64
                   " drapeaux %x\n", a, b, fpscr, cr, ref.cr, ref.fpscr,
                   ref.flags);
        }
    }
}

static void check_any(int op, uint64_t a, uint64_t b, uint64_t c,
                      uint64_t fpscr, uint64_t msr)
{
    if (op == FPI_CMPU) {
        check_fcmpu(a, b, fpscr, msr);
    } else {
        check_arith(op, a, b, c, fpscr, msr);
    }
}

int main(int argc, char **argv)
{
    uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 0) : 1000000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x5eed;
    uint64_t i, tot_fast = 0, tot = 0, tot_bad = 0, msr;
    int op, fam, x, y, z;

    rs[0] = seed; rs[1] = seed * 0x9e3779b97f4a7c15ull; rs[2] = ~seed;
    rs[3] = seed ^ 0xdeadbeefcafef00dull;
    cpu = g_malloc0(sizeof(*cpu));
    env = &cpu->env;
    build_catalog();
    use_model = getenv("FPPROOF_MODEL") != NULL;

    /* catalogue croisé : porte ouverte, et chaque façon de la fermer */
    for (op = 0; op < FPI_NOPS; op++) {
        for (x = 0; x < ncat; x++) {
            for (y = 0; y < ncat; y++) {
                int zn = (op >= FPI_MADD && op <= FPI_NMSUB) ? ncat : 1;
                for (z = 0; z < zn; z++) {
                    for (fam = 0; fam < 3; fam++) {
                        uint64_t f = rnd_fpscr(fam, &msr);
                        if (op == FPI_MUL || zn > 1) {
                            check_any(op, cat[x], cat[z], cat[y], f, msr);
                        } else {
                            check_any(op, cat[x], cat[y], 0, f, msr);
                        }
                    }
                }
            }
        }
    }
    /* aléatoire */
    for (op = 0; op < FPI_NOPS; op++) {
        for (i = 0; i < n; i++) {
            uint64_t a = rnd_operand(), b = rnd_operand(), c = rnd_operand();
            uint64_t f = rnd_fpscr(i % 8 == 7 ? 1 + (i & 8 ? 1 : 0) : 0, &msr);
            if (op == FPI_MUL || op >= FPI_MADD) {
                /* produit pile sur un demi-ulp de temps en temps */
                if ((i & 3) == 1) {
                    c = d_of_f(0x3f800000 | ((uint32_t)rnd() & 0x7ff) << 12);
                }
            }
            if ((i & 7) == 3 && op != FPI_CMPU) {
                /* annulation exacte ou presque : b = -(a*c) arrondi */
                float fa, fc, p;
                double da, dc;
                memcpy(&da, &a, 8); memcpy(&dc, &c, 8);
                fa = da; fc = dc; p = -(fa * fc);
                if (op == FPI_ADD || op == FPI_SUB) {
                    p = op == FPI_ADD ? -fa : fa;
                }
                b = d_bits(p) ^ ((i & 16) ? 1ull << 29 : 0);
            }
            check_any(op, a, b, c, f, msr);
        }
    }
    for (op = 0; op < FPI_NOPS; op++) {
        printf("%-8s court %12" PRIu64 "  helpers %12" PRIu64
               "  divergences %" PRIu64 "\n", names[op], n_fast[op],
               n_slow[op], n_bad[op]);
        tot_fast += n_fast[op];
        tot += n_fast[op] + n_slow[op];
        tot_bad += n_bad[op];
    }
    printf("total %" PRIu64 " vecteurs, %" PRIu64 " par le chemin court, %"
           PRIu64 " divergences ; modèle != objet : %" PRIu64 "\n",
           tot, tot_fast, tot_bad, n_modelbad);
    return tot_bad || n_modelbad;
}
