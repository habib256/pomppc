/*
 * vfpproof.c — preuve d'équivalence de patches/tcg/0003-ppc-vfp-fast.patch
 * (propriété x-vfp-fast, docs/tcg-g4.md §9).
 *
 * Se lie au VRAI objet softfloat du binaire (fpu_softfloat.c.o) et compare,
 * vecteur par vecteur (4 voies), ce que fait le helper patché :
 *
 *     if (vfp_add4 / vfp_fma4 (chemin rapide) réussit) → ses résultats
 *     sinon → la boucle d'origine (float32_add / sub / muladd par voie)
 *
 * à la boucle d'origine seule, sur deux float_status identiques au départ :
 * les 4 résultats au bit près ET le float_status d'arrivée (drapeaux) doivent
 * être égaux. Les fonctions rapides sont EXTRAITES TELLES QUELLES de
 * target/ppc/int_helper.c par vfpproof.sh (entre les marqueurs « vfp-fast »).
 *
 * États : no_hardfloat 0/1 × inexact déjà posé ou non × NJ (flush_to_zero et
 * flush_inputs_to_zero) 0/1 — les 8 combinaisons, arrondi au plus proche
 * (AltiVec n'a que celui-là), règle NaN « ab » comme cpu_init.c.
 * Opérations : vaddfp, vsubfp, vmaddfp, vnmsubfp.
 * Entrées : (1) catalogue de 64 valeurs limites, croisé en entier sur une voie
 * (64^2 pour add/sub, 64^3 pour les FMA) avec les autres voies normales ;
 * (2) vecteurs aléatoires dont chaque voie tire une classe (normal « doux »,
 * exposants extrêmes pour les débordements, annulations exactes a*c = -b,
 * zéros signés, dénormaux, infinis, NaN calmes et signalants).
 *
 * Usage : vfpproof [vecteurs aléatoires par (op, état)] [graine]
 */
#include "qemu/osdep.h"
#include "fpu/softfloat.h"
#include <float.h>
#include <math.h>
#include <pthread.h>

#include "vfpproof-fast.h"    /* vfp_can_use_fpu, vfp_zon, vfp_add4, vfp_fma4 */

enum { OP_ADD, OP_SUB, OP_MADD, OP_NMSUB, NOPS };
static const char *opname[NOPS] = { "vaddfp", "vsubfp", "vmaddfp", "vnmsubfp" };

static float_status mkstatus(int cfg)
{
    float_status s;
    memset(&s, 0, sizeof s);
    set_float_rounding_mode(float_round_nearest_even, &s);
    set_float_2nan_prop_rule(float_2nan_prop_ab, &s);
    s.no_hardfloat = cfg & 1;
    if (cfg & 2) {
        s.float_exception_flags = float_flag_inexact;
    }
    set_flush_to_zero(!!(cfg & 4), &s);
    set_flush_inputs_to_zero(!!(cfg & 4), &s);
    return s;
}

/* la boucle d'origine des helpers (int_helper.c, VARITHFP / VARITHFPFMA) */
static void ref(int op, float_status *s, uint32_t *r, const uint32_t *a,
                const uint32_t *b, const uint32_t *c)
{
    uint32_t t[4];
    for (int i = 0; i < 4; i++) {
        switch (op) {
        case OP_ADD:   t[i] = float32_add(a[i], b[i], s); break;
        case OP_SUB:   t[i] = float32_sub(a[i], b[i], s); break;
        case OP_MADD:  t[i] = float32_muladd(a[i], c[i], b[i], 0, s); break;
        default:       t[i] = float32_muladd(a[i], c[i], b[i],
                                  float_muladd_negate_result |
                                  float_muladd_negate_c, s); break;
        }
    }
    memcpy(r, t, sizeof t);
}

/* le helper patché */
static bool patched(int op, float_status *s, uint32_t *r, const uint32_t *a,
                    const uint32_t *b, const uint32_t *c)
{
    bool fast;
    switch (op) {
    case OP_ADD:   fast = vfp_add4(s, r, a, b, false); break;
    case OP_SUB:   fast = vfp_add4(s, r, a, b, true); break;
    case OP_MADD:  fast = vfp_fma4(s, r, a, b, c, 0); break;
    default:       fast = vfp_fma4(s, r, a, b, c, float_muladd_negate_result |
                                   float_muladd_negate_c); break;
    }
    if (!fast) {
        ref(op, s, r, a, b, c);
    }
    return fast;
}

typedef struct {
    int op, cfg;
    uint64_t n, seed;
    uint64_t cases, fast, bad;
} Job;

static bool one(Job *j, const uint32_t *a, const uint32_t *b, const uint32_t *c)
{
    float_status s1 = mkstatus(j->cfg), s2 = s1;
    uint32_t r1[4], r2[4];
    j->fast += patched(j->op, &s1, r1, a, b, c);
    ref(j->op, &s2, r2, a, b, c);
    j->cases++;
    if (memcmp(r1, r2, sizeof r1) || memcmp(&s1, &s2, sizeof s1)) {
        if (j->bad++ < 3) {
            fprintf(stderr, "✘ %s état %d : a=%08x %08x %08x %08x b=%08x %08x %08x %08x"
                    " c=%08x %08x %08x %08x → %08x %08x %08x %08x / %08x %08x %08x %08x"
                    " drapeaux %x / %x\n", opname[j->op], j->cfg,
                    a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], c[0], c[1], c[2], c[3],
                    r1[0], r1[1], r1[2], r1[3], r2[0], r2[1], r2[2], r2[3],
                    s1.float_exception_flags, s2.float_exception_flags);
        }
        return false;
    }
    return true;
}

static uint64_t rnd(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static uint32_t mkf(uint32_t sign, uint32_t exp, uint32_t frac)
{
    return sign << 31 | (exp & 0xff) << 23 | (frac & 0x7fffff);
}

/* une voie : une classe tirée au hasard */
static uint32_t lane(uint64_t *s)
{
    uint64_t r = rnd(s);
    uint32_t sg = r & 1, fr = (uint32_t)(r >> 8) & 0x7fffff, k = (r >> 40) % 100;
    if (k < 55) {
        return mkf(sg, 112 + (uint32_t)(r >> 32) % 32, fr);      /* doux */
    } else if (k < 70) {
        return mkf(sg, 1 + (uint32_t)(r >> 32) % 254, fr);       /* tout normal */
    } else if (k < 76) {
        return mkf(sg, 1 + (uint32_t)(r >> 32) % 6, fr);         /* près de FLT_MIN */
    } else if (k < 82) {
        return mkf(sg, 249 + (uint32_t)(r >> 32) % 6, fr);       /* près de FLT_MAX */
    } else if (k < 88) {
        return sg << 31;                                          /* ±0 */
    } else if (k < 92) {
        return mkf(sg, 0, fr | 1);                                /* dénormal */
    } else if (k < 95) {
        return mkf(sg, 0xff, 0);                                  /* ±inf */
    } else if (k < 98) {
        return mkf(sg, 0xff, fr | 0x400000);                      /* qNaN */
    }
    return mkf(sg, 0xff, (fr & 0x3fffff) | 1);                    /* sNaN */
}

static const uint32_t CAT[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007fffff, 0x807fffff,
    0x00400000, 0x00800000, 0x80800000, 0x00800001, 0x00ffffff, 0x01000000,
    0x3f800000, 0xbf800000, 0x3f800001, 0x3f7fffff, 0x40000000, 0xc0000000,
    0x3f000000, 0x3effffff, 0x33800000, 0x33000000, 0x34000000, 0x4b800000,
    0x4b7fffff, 0x4b800001, 0x7f7fffff, 0xff7fffff, 0x7f7ffffe, 0x7f000000,
    0x7e800000, 0x1f800000, 0x1f000000, 0x20000000, 0x5f800000, 0x5f000000,
    0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001, 0x7fbfffff,
    0x3eaaaaab, 0xbeaaaaab, 0x40490fdb, 0x3dcccccd, 0x42c80000, 0xc2c80000,
    0x2d000000, 0x2c800000, 0x52000000, 0x51800000, 0x00000002, 0x00200000,
    0x3fc00000, 0xbfc00000, 0x3fffffff, 0x3f800002, 0x0d800000, 0x71800000,
    0x7f7ffff0, 0x00800002, 0x3a800000, 0x45800000,
};
#define NCAT (sizeof CAT / sizeof CAT[0])

static void *run(void *arg)
{
    Job *j = arg;
    uint32_t a[4], b[4], c[4];
    uint64_t s = j->seed;
    bool fma = j->op >= OP_MADD;

    /* (1) catalogue croisé, voie 0 ; voies 1..3 normales */
    for (size_t x = 0; x < NCAT; x++) {
        for (size_t y = 0; y < NCAT; y++) {
            for (size_t z = 0; z < (fma ? NCAT : 1); z++) {
                for (int i = 1; i < 4; i++) {
                    a[i] = mkf(0, 120 + i, 0x123456 * i);
                    b[i] = mkf(1, 118 + i, 0x654321 * i);
                    c[i] = mkf(0, 126, 0x0f0f0f * i);
                }
                a[0] = CAT[x]; b[0] = CAT[y]; c[0] = CAT[z];
                one(j, a, b, c);
                if (fma) {
                    /* annulation exacte au voisinage : b = -(a*c) arrondi */
                    float p = (float)((double)*(float *)&CAT[x] * *(float *)&CAT[z]);
                    uint32_t pb;
                    memcpy(&pb, &p, 4);
                    b[0] = pb ^ 0x80000000;
                    one(j, a, b, c);
                }
            }
        }
    }
    /* (2) aléatoire */
    for (uint64_t n = 0; n < j->n; n++) {
        /* un vecteur sur deux : voies « douces » (normales d'exposant 112..143,
         * ou zéros), pour que le chemin rapide soit souvent pris */
        bool soft = rnd(&s) & 1;
        for (int i = 0; i < 4; i++) {
            a[i] = lane(&s); b[i] = lane(&s); c[i] = lane(&s);
            if (soft) {
                uint64_t r = rnd(&s);
                a[i] = (r & 15) ? mkf(r >> 4 & 1, 112 + (r >> 8) % 32, r >> 16) : 0;
                b[i] = (r >> 40 & 15) ? mkf(r >> 5 & 1, 112 + (r >> 44) % 32, r >> 20) : 0x80000000;
                c[i] = mkf(r >> 6 & 1, 112 + (r >> 50) % 32, r >> 30);
            }
            if (fma && (rnd(&s) & 7) == 0) {
                float p = vfp_f(a[i]) * vfp_f(c[i]);
                b[i] = vfp_u(-p);                   /* annulation (presque) exacte */
            } else if (!fma && (rnd(&s) & 7) == 0) {
                b[i] = a[i] ^ ((rnd(&s) & 1) ? 0x80000000 : 1);
            }
        }
        one(j, a, b, c);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    uint64_t n = argc > 1 ? strtoull(argv[1], 0, 0) : 2000000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], 0, 0) : 0x5eed;
    Job jobs[NOPS * 8];
    pthread_t th[NOPS * 8];
    uint64_t tc = 0, tf = 0, tb = 0;

    for (int op = 0; op < NOPS; op++) {
        for (int cfg = 0; cfg < 8; cfg++) {
            Job *j = &jobs[op * 8 + cfg];
            memset(j, 0, sizeof *j);
            j->op = op; j->cfg = cfg; j->n = n;
            j->seed = seed * 131 + op * 8 + cfg;
            pthread_create(&th[op * 8 + cfg], NULL, run, j);
        }
    }
    for (int k = 0; k < NOPS * 8; k++) {
        pthread_join(th[k], NULL);
    }
    for (int op = 0; op < NOPS; op++) {
        uint64_t c = 0, f = 0, b = 0, fprimed = 0;
        for (int cfg = 0; cfg < 8; cfg++) {
            Job *j = &jobs[op * 8 + cfg];
            c += j->cases; f += j->fast; b += j->bad;
            if (cfg == 2 || cfg == 6) {
                fprimed += j->fast;
            }
            /* chemin rapide impossible sans hardfloat ou sans amorçage */
            if ((cfg & 1 || !(cfg & 2)) && j->fast) {
                fprintf(stderr, "✘ %s état %d : chemin rapide pris hors conditions\n",
                        opname[op], cfg);
                b++;
            }
        }
        printf("%-9s : %" PRIu64 " vecteurs, %" PRIu64 " par le chemin rapide"
               " (%" PRIu64 " dans les états amorcés), %" PRIu64 " divergences\n",
               opname[op], c, f, fprimed, b);
        tc += c; tf += f; tb += b;
    }
    printf("total : %" PRIu64 " vecteurs (%" PRIu64 " voies), %" PRIu64
           " rapides, %" PRIu64 " divergences — %s\n", tc, tc * 4, tf, tb,
           tb ? "ÉCHEC" : "OK : résultats et drapeaux identiques au helper d'origine");
    return tb != 0;
}
