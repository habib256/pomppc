/* GPL3 - Copyleft VERHILLE Arnaud
 * sqrttest — la racine de la libm de Tiger contre l'instruction fsqrt.
 *
 * La libm de Tiger (`_sqrt`, libSystem) lit _cpu_capabilities dans la commpage
 * (0xFFFF8020) : bit kHasFsqrt (0x20000000, G5) levé → `fsqrt` ; sinon la
 * racine logicielle `___sqrt`. Ce banc compare les deux, à l'octet (résultat et
 * FPSCR), sur des valeurs choisies (zéros, infinis, NaN, dénormaux, négatifs,
 * carrés exacts, voisins) puis aléatoires, et chronomètre chacune.
 *
 *   gcc -O2 -o sqrttest sqrttest.c && ./sqrttest [N]
 * Sortie : nombre d'écarts (attendu 0), ns par appel des deux chemins,
 * valeur actuelle de _cpu_capabilities. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

extern double __sqrt(double);           /* ___sqrt : la racine logicielle */

static inline double hw_sqrt(double x) { double r; __asm__ volatile("fsqrt %0,%1" : "=f"(r) : "f"(x)); return r; }
static inline double rd_fpscr(void) { double f; __asm__ volatile("mffs %0" : "=f"(f)); return f; }
static inline void clr_fpscr(void) { __asm__ volatile("mtfsf 255,%0" :: "f"(0.0)); }

static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double dbl(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static long ecarts, cas;

static void un(double x)
{
    double a, b, fa, fb;
    clr_fpscr(); a = __sqrt(x); fa = rd_fpscr();
    clr_fpscr(); b = hw_sqrt(x); fb = rd_fpscr();
    cas++;
    /* NaN : même classe suffit (la charge utile n'est pas garantie pareille) */
    int nan_a = a != a, nan_b = b != b;
    if ((nan_a && nan_b) ? 0 : bits(a) != bits(b)) {
        if (ecarts++ < 20)
            printf("écart x=%016llx : ___sqrt %016llx, fsqrt %016llx\n",
                   (unsigned long long)bits(x), (unsigned long long)bits(a), (unsigned long long)bits(b));
    }
    if ((uint32_t)bits(fa) != (uint32_t)bits(fb) && ecarts < 40) {
        /* drapeaux : on les compte à part, la libm ne promet pas les mêmes */
        static int vus;
        if (vus++ < 10)
            printf("FPSCR x=%016llx : ___sqrt %08x, fsqrt %08x\n", (unsigned long long)bits(x),
                   (uint32_t)bits(fa), (uint32_t)bits(fb));
    }
}

static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec * 1e-6; }

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 2000000, i;
    static const uint64_t speciaux[] = {
        0, 0x8000000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
        0x7ff8000000000000ULL, 0x7ff4000000000000ULL, 1, 0x000fffffffffffffULL,
        0x0010000000000000ULL, 0x3ff0000000000000ULL, 0xbff0000000000000ULL,
        0x4010000000000000ULL, 0x7fefffffffffffffULL, 0x8000000000000001ULL,
    };
    for (i = 0; i < (long)(sizeof speciaux / sizeof *speciaux); i++)
        un(dbl(speciaux[i]));
    for (i = 1; i < 100000; i++) {          /* carrés exacts et leurs voisins */
        double c = (double)i * (double)i;
        un(c); un(dbl(bits(c) + 1)); un(dbl(bits(c) - 1));
    }
    for (i = 0; i < n; i++) {
        uint64_t u = next();
        un(dbl(u));                                         /* tous signes, NaN, dénormaux */
        un(dbl((u & 0x000fffffffffffffULL) | 0x3fe0000000000000ULL | ((u >> 12) & 0x0010000000000000ULL)));
    }
    printf("cas %ld, écarts %ld\n", cas, ecarts);

    volatile double s = 0;
    double t0 = now();
    for (i = 0; i < 2000000; i++) s += __sqrt(1.0 + i);
    double t1 = now();
    for (i = 0; i < 2000000; i++) s += hw_sqrt(1.0 + i);
    double t2 = now();
    printf("___sqrt %.1f ns/appel, fsqrt %.1f ns/appel\n", (t1 - t0) * 500.0, (t2 - t1) * 500.0);
    printf("_cpu_capabilities = %08x (kHasFsqrt %s)\n", *(volatile uint32_t *)0xffff8020,
           (*(volatile uint32_t *)0xffff8020 & 0x20000000) ? "levé" : "baissé");
    return ecarts != 0;
}
