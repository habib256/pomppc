/* GPL3 - Copyleft VERHILLE Arnaud */
/*
 * fpnatbench.c — borne hôte du flottant scalaire « natif » (docs/tcg-g4.md §22).
 *
 * Mesure, sur le processeur hôte, le coût par opération PowerPC simple
 * précision de trois façons de faire ce que fait aujourd'hui le chemin court
 * de x-fp-inline (tcg/0007), avec les FPR rangés en mémoire comme des doubles
 * (un tableau « env », relu et réécrit à chaque opération comme le fait le
 * code généré) :
 *
 *   appel   l'appel de helper_fp32_fast (copie exacte, non en ligne), puis le
 *           test FPI_FAIL et FPRF/FI en entier : ce que fait le code généré ;
 *   natif   la même chose en ligne : tests d'opérandes en entier, calcul
 *           float32 du FPU hôte, FPRF, un seul test « raté » cumulé : ce que
 *           ferait une op TCG native dont les opérandes arrivent en GPR ;
 *   vide    rien que les lectures/écritures d'« env » et FPRF (le plancher) ;
 *   registre la suite entière (le motif) en registres de l'hôte : FPR lus au
 *           début, calcul natif, un seul test cumulé et les écritures à la
 *           fin (ce que donneraient des FPR tenus en registres le temps d'un
 *           bloc, sans branchement par instruction).
 *
 * Deux motifs, ceux de `fptest banc` : une chaîne dépendante (fmadds fmuls
 * fmsubs fadds) et une transformation 4×4 de sommets (12 fmadds + 4 fmuls
 * par sommet, opérandes indépendants).  Sortie : ns par opération.
 *
 *   cc -O2 -o /tmp/fpnatbench tools/tcg/fpnatbench.c && /tmp/fpnatbench
 */
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { FPI_ADD, FPI_SUB, FPI_MUL, FPI_MADD, FPI_MSUB, FPI_NMADD, FPI_NMSUB };
#define FPI_FAIL 0x7ff8000000000001ull

static inline int fpi_zon(uint64_t x)
{
    uint64_t frac = x & ((1ull << 52) - 1);
    int exp = (x >> 52) & 0x7ff;
    if (exp == 0) {
        return frac == 0;
    }
    return (frac & ((1ull << 29) - 1)) == 0 && exp >= 0x381 && exp <= 0x47e;
}

static inline float f32(uint64_t x)
{
    double d;
    memcpy(&d, &x, 8);
    return (float)d;
}

static inline uint64_t f64(float f)
{
    double d = f;
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

/* copie de helper_fp32_fast (fpu_helper.c, tcg/0007) */
__attribute__((noinline)) uint64_t helper_fp32_fast(uint64_t a, uint64_t b,
                                                    uint64_t c, uint32_t op)
{
    float fa, fb, fc, r;

    switch (op) {
    case FPI_ADD:
    case FPI_SUB:
        if ((uint64_t)!fpi_zon(a) || (uint64_t)!fpi_zon(b)) {
            return FPI_FAIL;
        }
        fa = f32(a);
        fb = f32(b);
        r = op == FPI_ADD ? fa + fb : fa - fb;
        if (isinf(r)) {
            return FPI_FAIL;
        }
        return f64(r);
    case FPI_MUL:
        if ((uint64_t)!fpi_zon(a) || (uint64_t)!fpi_zon(c)) {
            return FPI_FAIL;
        }
        fa = f32(a);
        fc = f32(c);
        r = fa * fc;
        if (fa != 0 && fc != 0 && (isinf(r) || fabsf(r) <= FLT_MIN)) {
            return FPI_FAIL;
        }
        return f64(r);
    default:
        if ((uint64_t)!fpi_zon(a) || (uint64_t)!fpi_zon(b) || (uint64_t)!fpi_zon(c)) {
            return FPI_FAIL;
        }
        fa = f32(a);
        fb = f32(b);
        fc = f32(c);
        if (op == FPI_MSUB || op == FPI_NMSUB) {
            fb = -fb;
        }
        r = fmaf(fa, fc, fb);
        if (fa != 0 && fc != 0 && (isinf(r) || fabsf(r) <= FLT_MIN)) {
            return FPI_FAIL;
        }
        if (op == FPI_NMADD || op == FPI_NMSUB) {
            r = -r;
        }
        return f64(r);
    }
}

/* même calcul, en ligne, op connue à la traduction ; *bad cumule les ratés */
static inline __attribute__((always_inline))
uint64_t nat(uint64_t a, uint64_t b, uint64_t c, int op, uint64_t *bad)
{
    float r;
    if (op == FPI_ADD || op == FPI_SUB) {
        *bad |= (uint64_t)!fpi_zon(a) | (uint64_t)!fpi_zon(b);
        r = op == FPI_ADD ? f32(a) + f32(b) : f32(a) - f32(b);
        *bad |= isinf(r);
    } else if (op == FPI_MUL) {
        *bad |= (uint64_t)!fpi_zon(a) | (uint64_t)!fpi_zon(c);
        r = f32(a) * f32(c);
        *bad |= (a << 1) && (c << 1) && (isinf(r) || fabsf(r) <= FLT_MIN);
    } else {
        float fb = f32(b);
        *bad |= (uint64_t)!fpi_zon(a) | (uint64_t)!fpi_zon(b) | (uint64_t)!fpi_zon(c);
        r = fmaf(f32(a), f32(c), op == FPI_MSUB ? -fb : fb);
        *bad |= (a << 1) && (c << 1) && (isinf(r) || fabsf(r) <= FLT_MIN);
    }
    return f64(r);
}

static inline uint64_t fprf(uint64_t fpscr, uint64_t r)
{
    uint64_t neg = r >> 63;
    uint64_t f = (r << 1) == 0 ? (neg << 4) | 2 : 4ull << neg;
    return (fpscr & ~0x1f000ull & ~(1ull << 17)) | f << 12 | 1ull << 17;
}

/* « env » : 32 FPR, FPSCR, drapeaux ; volatile = relu/écrit comme par TCG */
static volatile uint64_t fpr[32];
static volatile uint64_t fpscr;
static volatile uint16_t flags;
static volatile uint64_t fails;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* un pas de chaque sorte, frT = op(frA, frB, frC) */
#define STEP_CALL(op, t, a, b, cc) do {                                  \
        uint64_t r_ = helper_fp32_fast(fpr[a], fpr[b], fpr[cc], op);     \
        uint64_t s_ = fpscr;                                             \
        if (((s_ & 0x200006bull) != 0x2000000ull) | (r_ == FPI_FAIL)) { \
            fails++;                                                     \
        } else {                                                         \
            fpr[t] = r_;                                                 \
            fpscr = fprf(s_, r_);                                        \
            flags = 32;                                                  \
        }                                                                \
    } while (0)
#define STEP_NAT(op, t, a, b, cc) do {                                   \
        uint64_t bad_ = 0;                                               \
        uint64_t r_ = nat(fpr[a], fpr[b], fpr[cc], op, &bad_);           \
        uint64_t s_ = fpscr;                                             \
        if (bad_ | ((s_ & 0x200006bull) != 0x2000000ull)) {             \
            fails++;                                                     \
        } else {                                                         \
            fpr[t] = r_;                                                 \
            fpscr = fprf(s_, r_);                                        \
            flags = 32;                                                  \
        }                                                                \
    } while (0)
#define STEP_VIDE(op, t, a, b, cc) do {                                  \
        uint64_t r_ = fpr[a];                                            \
        (void)fpr[b]; (void)fpr[cc];                                     \
        fpr[t] = r_;                                                     \
        fpscr = fprf(fpscr, r_);                                         \
        flags = 32;                                                      \
    } while (0)

/* « suite en registres » : les FPR de la suite dans des variables locales
 * (registres de l'hôte), un seul test et une seule écriture à la fin */
#define STEP_REG(op, t, a, b, cc) do {                                   \
        R[t] = nat(R[a], R[b], R[cc], op, &bad);                         \
        last = R[t];                                                     \
    } while (0)
#define RUN_REG(PAT, STEP) do {                                          \
        uint64_t R[32], bad = 0, last = 0, s_ = fpscr;                   \
        for (int j_ = 0; j_ < 24; j_++) {                                \
            R[j_] = fpr[j_];                                             \
        }                                                                \
        PAT(STEP);                                                       \
        if (bad | ((s_ & 0x200006bull) != 0x2000000ull)) {               \
            fails++;                                                     \
        } else {                                                         \
            for (int j_ = 0; j_ < 24; j_++) {                            \
                fpr[j_] = R[j_];                                         \
            }                                                            \
            fpscr = fprf(s_, last);                                      \
            flags = 32;                                                  \
        }                                                                \
    } while (0)

#define CHAIN(STEP) do {                                                 \
        STEP(FPI_MADD, 1, 1, 3, 2);   /* x = x*y + z */                  \
        STEP(FPI_MUL, 1, 1, 0, 4);    /* x = x*w */                      \
        STEP(FPI_MSUB, 1, 1, 3, 2);   /* x = x*y - z */                  \
        STEP(FPI_ADD, 1, 1, 4, 0);    /* x = x + w */                    \
    } while (0)

/* sommet : fpr 16..19 = a b c d, 0..15 = matrice (déjà en FPR), 20..23 = sortie */
#define ROW(STEP, o, m0, m1, m2, m3) do {                                \
        STEP(FPI_MUL, 20 + o, m3, 0, 19);                                \
        STEP(FPI_MADD, 20 + o, m2, 20 + o, 18);                          \
        STEP(FPI_MADD, 20 + o, m1, 20 + o, 17);                          \
        STEP(FPI_MADD, 20 + o, m0, 20 + o, 16);                          \
    } while (0)
#define VERTEX(STEP) do {                                                \
        ROW(STEP, 0, 0, 4, 8, 12); ROW(STEP, 1, 1, 5, 9, 13);            \
        ROW(STEP, 2, 2, 6, 10, 14); ROW(STEP, 3, 3, 7, 11, 15);          \
    } while (0)

static void init(void)
{
    for (int i = 0; i < 32; i++) {
        fpr[i] = f64((float)(i + 1) / 3.0f);
    }
    fpr[1] = f64(1.0f / 3.0f);
    fpr[3] = f64(1.0001f);
    fpr[2] = f64(0.9999f);
    fpr[4] = f64(0.5f);
    fpscr = 0x2000000;
    fails = 0;
}

#define BENCH(name, BODY, nops) do {                                     \
        init();                                                          \
        double t0 = now();                                               \
        for (long i = 0; i < n; i++) {                                   \
            BODY;                                                        \
        }                                                                \
        double dt = now() - t0;                                          \
        printf("%-8s %-26s %6.2f ns/op (%ld ratés)\n", name, #BODY,       \
               dt * 1e9 / (n * (double)(nops)), (long)fails);            \
    } while (0)

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 20000000;
    for (int k = 0; k < 2; k++) {
        BENCH("appel", CHAIN(STEP_CALL), 4);
        BENCH("natif", CHAIN(STEP_NAT), 4);
        BENCH("vide", CHAIN(STEP_VIDE), 4);
        BENCH("registre", RUN_REG(CHAIN, STEP_REG), 4);
        BENCH("appel", VERTEX(STEP_CALL), 16);
        BENCH("natif", VERTEX(STEP_NAT), 16);
        BENCH("vide", VERTEX(STEP_VIDE), 16);
        BENCH("registre", RUN_REG(VERTEX, STEP_REG), 16);
    }
    return 0;
}
