/*
 * lfsproof.c — preuve d'équivalence de patches/tcg/0002-ppc-lfs-inline.patch
 * (propriété x-lfs-inline, docs/tcg-g4.md §8).
 *
 * Compare, sur l'hôte, les helpers de QEMU (helper_todouble / helper_tosingle,
 * EXTRAITS TELS QUELS de target/ppc/fpu_helper.c par lfsproof.sh) au modèle C
 * des ops TCG que le patch émet : une ligne C par op, même ordre, mêmes
 * largeurs (tout en 64 bits), mêmes sémantiques de TCG (clzi avec la valeur
 * par défaut pour 0, umax/umin non signés, movcond, deposit) ; aucun décalage
 * hors de [0, 63], donc aucun comportement « non spécifié » de TCG en jeu.
 *
 *   lfs  (DOUBLE)  : les 2^32 motifs de float32, tous.
 *   stfs (SINGLE)  : les 2^32 mots hauts d'un float64 (signe, exposant, 20 bits
 *                    hauts de fraction : tout ce qui décide du chemin) croisés
 *                    avec 8 mots bas (dont un aléatoire par mot haut), soit
 *                    34 359 738 368 cas ; puis, pour chaque exposant et chaque
 *                    signe, les fractions 1<<b, (1<<b)-1, ~0>>b, b = 0..52.
 *
 * En plus, un contrôle de vraisemblance du helper lui-même : pour tout float32
 * non NaN, helper_todouble(x) == la conversion float -> double du FPU hôte.
 *
 * Imprime le nombre de cas et de divergences ; code de sortie 1 si divergence.
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ce dont les helpers extraits ont besoin (sémantique de QEMU) ---- */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
static inline uint32_t extract32(uint32_t v, int s, int l)
{
    return (v >> s) & (~0U >> (32 - l));
}
static inline uint64_t extract64(uint64_t v, int s, int l)
{
    return (v >> s) & (~0ULL >> (64 - l));
}
static inline int clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }

#include "lfsproof-helpers.h"   /* helper_todouble, helper_tosingle */

/* ---- modèle des ops TCG (gen_todouble_inline / gen_tosingle_inline) ---- */
static inline uint64_t op_clzi(uint64_t a, uint64_t dflt)
{
    return a ? (uint64_t)__builtin_clzll(a) : dflt;
}
static inline uint64_t op_umax(uint64_t a, uint64_t b) { return a > b ? a : b; }
static inline uint64_t op_umin(uint64_t a, uint64_t b) { return a < b ? a : b; }
static inline uint64_t op_deposit(uint64_t a, uint64_t b, int pos, int len)
{
    uint64_t m = (~0ULL >> (64 - len)) << pos;
    return (a & ~m) | ((b << pos) & m);
}

static uint64_t model_todouble(uint32_t in)
{
    uint64_t u = in;                                  /* ld_i64 MO_UL */
    uint64_t abs, sign, k, m, e, d;
    abs = u & 0x7fffffff;                             /* andi */
    sign = u ^ abs;                                   /* xor */
    sign = sign << 32;                                /* shli */
    k = op_clzi(abs, 64);                             /* clzi */
    k = op_umax(k, 40);                               /* umax */
    m = k - 11;                                       /* subi */
    if (m > 63) { abort(); }                          /* (jamais : [29, 53]) */
    m = abs << m;                                     /* shl */
    e = 936 - k;                                      /* subfi */
    e = e << 52;                                      /* shli */
    e = abs >= 0x7f800000 ? 0x700ULL << 52 : e;       /* movcond GEU */
    e = abs == 0 ? 0 : e;                             /* movcond EQ */
    d = m + e;                                        /* add */
    d = d | sign;                                     /* or */
    return d;
}

static uint32_t model_tosingle(uint64_t x)
{
    uint64_t exp, hi, t, lo, sh, d;
    exp = extract64(x, 52, 11);                       /* extract */
    hi = x >> 29;                                     /* shri */
    t = x >> 62;                                      /* shri */
    hi = op_deposit(hi, t, 30, 2);                    /* deposit */
    lo = x & ((1ULL << 52) - 1);                      /* andi */
    lo = lo | (1ULL << 52);                           /* ori */
    sh = 926 - exp;                                   /* subfi (mod 2^64) */
    sh = op_umin(sh, 63);                             /* umin */
    lo = lo >> sh;                                    /* shr */
    t = x >> 32;                                      /* shri */
    t = t & 0x80000000;                               /* andi */
    lo = lo | t;                                      /* or */
    d = exp > 896 ? hi : lo;                          /* movcond GTU */
    return (uint32_t)d;                               /* st_i64 MO_UL */
}

/* ---- balayages parallèles ---- */
#define NTH 16
typedef struct {
    int id;
    uint64_t cases, bad, hostbad, hostcmp;
    uint64_t first_bad;
    bool have_bad;
} Job;

static uint64_t splitmix(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void *run_lfs(void *arg)
{
    Job *j = arg;
    uint64_t lo = (1ULL << 32) / NTH * j->id, hi = lo + (1ULL << 32) / NTH;
    for (uint64_t v = lo; v < hi; v++) {
        uint32_t f = (uint32_t)v;
        uint64_t a = helper_todouble(f), b = model_todouble(f);
        j->cases++;
        if (a != b) {
            if (!j->have_bad) { j->first_bad = v; j->have_bad = true; }
            j->bad++;
        }
        if ((f & 0x7f800000) != 0x7f800000 || !(f & 0x007fffff)) {
            float x; double y; uint64_t yb;
            memcpy(&x, &f, 4);
            y = x;
            memcpy(&yb, &y, 8);
            j->hostcmp++;
            j->hostbad += yb != a;
        }
    }
    return NULL;
}

static const uint32_t LOWS[7] = {
    0x00000000, 0xffffffff, 0x20000000, 0xe0000000, 0x1fffffff,
    0x80000001, 0x5a5a5a5a,
};

static void *run_stfs(void *arg)
{
    Job *j = arg;
    uint64_t seed = 0x1234 + j->id;
    uint64_t lo = (1ULL << 32) / NTH * j->id, hi = lo + (1ULL << 32) / NTH;
    for (uint64_t h = lo; h < hi; h++) {
        for (int i = 0; i < 8; i++) {
            uint64_t low = i < 7 ? LOWS[i] : (uint32_t)splitmix(&seed);
            uint64_t x = (h << 32) | low;
            uint32_t a = helper_tosingle(x), b = model_tosingle(x);
            j->cases++;
            if (a != b) {
                if (!j->have_bad) { j->first_bad = x; j->have_bad = true; }
                j->bad++;
            }
        }
    }
    return NULL;
}

static uint64_t edge_stfs(uint64_t *cases)
{
    uint64_t bad = 0;
    for (uint64_t s = 0; s < 2; s++) {
        for (uint64_t e = 0; e < 2048; e++) {
            for (int b = 0; b <= 52; b++) {
                uint64_t fr[3] = {
                    (1ULL << b) & ((1ULL << 52) - 1),
                    (1ULL << b) - 1,
                    ((1ULL << 52) - 1) >> b,
                };
                for (int i = 0; i < 3; i++) {
                    uint64_t x = s << 63 | e << 52 | fr[i];
                    (*cases)++;
                    if (helper_tosingle(x) != model_tosingle(x)) {
                        if (!bad) {
                            fprintf(stderr, "stfs bord: %016" PRIx64 "\n", x);
                        }
                        bad++;
                    }
                }
            }
        }
    }
    return bad;
}

static void sweep(void *(*fn)(void *), const char *name, uint64_t *c,
                  uint64_t *b, uint64_t *hc, uint64_t *hb)
{
    pthread_t th[NTH];
    Job jobs[NTH];
    memset(jobs, 0, sizeof jobs);
    for (int i = 0; i < NTH; i++) {
        jobs[i].id = i;
        pthread_create(&th[i], NULL, fn, &jobs[i]);
    }
    *c = *b = *hc = *hb = 0;
    for (int i = 0; i < NTH; i++) {
        pthread_join(th[i], NULL);
        *c += jobs[i].cases;
        *b += jobs[i].bad;
        *hc += jobs[i].hostcmp;
        *hb += jobs[i].hostbad;
        if (jobs[i].have_bad && *b == jobs[i].bad) {
            fprintf(stderr, "%s: première divergence %016" PRIx64 "\n", name,
                    jobs[i].first_bad);
        }
    }
}

int main(void)
{
    uint64_t c, b, hc, hb, ec = 0, eb;
    int rc = 0;

    sweep(run_lfs, "lfs", &c, &b, &hc, &hb);
    printf("lfs  (todouble) : %" PRIu64 " motifs float32, %" PRIu64
           " divergences\n", c, b);
    printf("     contrôle helper = FPU hôte (non NaN) : %" PRIu64 " cas, %"
           PRIu64 " écarts\n", hc, hb);
    rc |= b != 0 || c != (1ULL << 32) || hb != 0;

    sweep(run_stfs, "stfs", &c, &b, &hc, &hb);
    printf("stfs (tosingle) : %" PRIu64 " float64 (2^32 mots hauts x 8 mots"
           " bas), %" PRIu64 " divergences\n", c, b);
    rc |= b != 0 || c != (8ULL << 32);

    eb = edge_stfs(&ec);
    printf("stfs (tosingle) : %" PRIu64 " cas limites (signe x exposant x"
           " fraction), %" PRIu64 " divergences\n", ec, eb);
    rc |= eb != 0;

    printf(rc ? "ÉCHEC\n" : "OK : modèle des ops == helpers, au bit près\n");
    return rc;
}
