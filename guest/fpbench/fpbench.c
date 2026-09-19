/* fpbench.c — banc de vitesse du flottant PowerPC sous QEMU (G4 7400, Tiger).
 *
 * Un noyau par ligne :
 *
 *     nom  secondes  itérations  somme_de_contrôle
 *
 * Les itérations sont FIXES (pas d'auto-calibrage à l'exécution) pour que les
 * secondes de deux modes soient directement comparables et que la somme de
 * contrôle, qui porte sur les bits des résultats, prouve que les deux modes
 * calculent la MÊME chose. Le noyau `entier-temoin` ne fait aucun flottant :
 * il ne doit pas bouger d'un mode à l'autre, et sert de mesure de la charge de
 * l'hôte.
 *
 * Usage :
 *     fpbench [POURCENT]     POURCENT = échelle des itérations (défaut 100)
 *     fpbench --calibrer     mesure court et propose les itérations pour ~2 s
 *
 * Le chemin rapide du patch QEMU ne s'arme que lorsque FPSCR[XX] vaut déjà 1 :
 * c'est le cas dès la première opération inexacte, donc en pratique tout de
 * suite. Les lignes « # fpscr… » le donnent à voir.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mach/mach_time.h>

typedef unsigned long long u64;
typedef unsigned int u32;
typedef union { double d; u64 u; } du_t;
typedef union { float f; u32 u; } fu_t;

/* ----------------------------------------------------------- chronomètre */
static mach_timebase_info_data_t g_tb;

static u64 tick(void) { return mach_absolute_time(); }

/* millisecondes écoulées, en ENTIERS (aucun flottant : le chronomètre ne doit
 * pas perturber le FPSCR du noyau qu'il encadre) */
static u64 ms_since(u64 t0)
{
    u64 dt = tick() - t0;
    return dt * g_tb.numer / g_tb.denom / 1000000ULL;
}

static u64 us_since(u64 t0)
{
    u64 dt = tick() - t0;
    return dt * g_tb.numer / g_tb.denom / 1000ULL;
}

/* ------------------------------------------------------------- FPSCR ----- */
static unsigned fpscr_get(void)
{
    du_t x;
    __asm__ __volatile__("mffs %0" : "=f"(x.d));
    return (unsigned)(x.u & 0xFFFFFFFFULL);
}

/* --------------------------------------------------------------- hachage */
#define FNV_OFF 14695981039346656037ULL
#define FNV_PRM 1099511628211ULL

static u64 hmix(u64 h, u64 v) { h ^= v; h *= FNV_PRM; h ^= h >> 29; return h; }
static u64 dbits(double d) { du_t x; x.d = d; return x.u; }
static u64 fbits(float f) { fu_t x; x.f = f; return (u64)x.u; }

/* --------------------------------------------------------------- données */
#define N 1024
static float  fa[N], fb[N], fsrc[N], fdst[N], ftrig[N + 32], fbuf[N];
static double da[N], db[N];
static float  ffta[512], fftw[512];
static double mA[16], mB[16], mC[16];

static u32 g_rs = 0x2545F491u;
static u32 xr(void) { g_rs ^= g_rs << 13; g_rs ^= g_rs >> 17; g_rs ^= g_rs << 5; return g_rs; }

/* un réel dans [-2, 2) sans jamais d'infini ni de NaN */
static double uni(void) { return (double)(int)(xr() & 0xFFFFF) / 262144.0 - 2.0; }

static void init_data(void)
{
    int i;
    g_rs = 0x2545F491u;
    for (i = 0; i < N; i++) {
        fa[i]   = (float)uni();
        fb[i]   = (float)uni();
        fsrc[i] = (float)uni();
        fdst[i] = 0.0f;
        fbuf[i] = 0.0f;
        da[i]   = uni();
        db[i]   = uni() + 3.0;          /* jamais nul : les divisions restent saines */
    }
    for (i = 0; i < N + 32; i++)
        ftrig[i] = (float)(0.25 + (double)(i % 97) / 400.0);
    for (i = 0; i < 512; i++) {
        ffta[i] = (float)uni();
        fftw[i] = (float)(0.3 + (double)(i % 61) / 200.0);
    }
    for (i = 0; i < 16; i++) { mA[i] = uni() * 0.25; mB[i] = uni() * 0.25; mC[i] = 0.0; }
}

/* ============================ les noyaux ================================= */
/* Chacun rend une somme de contrôle : un hachage des BITS des résultats.
 * Les données restent bornées (pas de dérive vers l'infini ni vers les
 * dénormaux, qui fausseraient le temps sans rien prouver de plus). */

/* 1. témoin ENTIER — aucun flottant : doit être identique d'un mode à l'autre */
static u64 k_entier(long n)
{
    u32 a = 1u, b = 2166136261u;
    long i;
    for (i = 0; i < n; i++) {
        a = a * 1664525u + 1013904223u;
        b ^= a >> 13;
        b = (b << 5) - b + (u32)i;
        b ^= a >> 7;
        a += b;
    }
    return ((u64)a << 32) | b;
}

/* 2. papillons MDCT en float — ce que fait mdct_butterfly_generic de libvorbis */
static void bfly_generic(float *x, int points, int step, const float *T)
{
    float *x1 = x + points - 8;
    float *x2 = x + (points >> 1) - 8;
    float r0, r1;
    do {
        r0 = x1[6] - x2[6]; r1 = x1[7] - x2[7];
        x1[6] += x2[6];     x1[7] += x2[7];
        x2[6] = r1 * T[1] - r0 * T[0];
        x2[7] = r0 * T[1] + r1 * T[0];

        r0 = x1[4] - x2[4]; r1 = x2[5] - x1[5];
        x1[4] += x2[4];     x1[5] += x2[5];
        x2[4] = r1 * T[5] - r0 * T[4];
        x2[5] = r0 * T[5] + r1 * T[4];

        r0 = x1[2] - x2[2]; r1 = x1[3] - x2[3];
        x1[2] += x2[2];     x1[3] += x2[3];
        x2[2] = r1 * T[9] - r0 * T[8];
        x2[3] = r0 * T[9] + r1 * T[8];

        r0 = x1[0] - x2[0]; r1 = x2[1] - x1[1];
        x1[0] += x2[0];     x1[1] += x2[1];
        x2[0] = r1 * T[13] - r0 * T[12];
        x2[1] = r0 * T[13] + r1 * T[12];

        T += step;
        x1 -= 8; x2 -= 8;
    } while (x2 >= x);
}

static u64 k_mdct(long n)
{
    long i;
    int j;
    u64 h = FNV_OFF;
    for (i = 0; i < n; i++) {
        for (j = 0; j < 512; j++) fbuf[j] = fsrc[j] * 0.5f;   /* recharge bornée */
        bfly_generic(fbuf, 512, 2, ftrig);
        bfly_generic(fbuf, 256, 4, ftrig);
        if ((i & 63) == 0) h = hmix(h, fbits(fbuf[3]) ^ (fbits(fbuf[300]) << 1));
    }
    for (j = 0; j < 512; j += 37) h = hmix(h, fbits(fbuf[j]));
    return h;
}

/* 3. FFT inverse en float (radix 2, 128 points complexes) + rotation MDCT */
static u64 k_fft(long n)
{
    long it;
    int i, j, m, k;
    u64 h = FNV_OFF;
    for (it = 0; it < n; it++) {
        for (i = 0; i < 256; i++) ffta[i] = fftw[i] * 0.5f + ffta[i] * 0.25f;
        /* inversion de bits */
        for (i = 1, j = 0; i < 128; i++) {
            int bit = 128 >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) {
                float tr = ffta[2 * i], ti = ffta[2 * i + 1];
                ffta[2 * i] = ffta[2 * j]; ffta[2 * i + 1] = ffta[2 * j + 1];
                ffta[2 * j] = tr; ffta[2 * j + 1] = ti;
            }
        }
        for (m = 2; m <= 128; m <<= 1) {
            float wr = fftw[m], wi = fftw[m + 1];
            for (k = 0; k < 128; k += m) {
                float cr = 1.0f, ci = 0.0f;
                for (i = 0; i < m / 2; i++) {
                    int a = 2 * (k + i), b = 2 * (k + i + m / 2);
                    float tr = ffta[b] * cr - ffta[b + 1] * ci;
                    float ti = ffta[b] * ci + ffta[b + 1] * cr;
                    ffta[b]     = ffta[a] - tr;
                    ffta[b + 1] = ffta[a + 1] - ti;
                    ffta[a]     = ffta[a] + tr;
                    ffta[a + 1] = ffta[a + 1] + ti;
                    { float nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr; }
                }
            }
        }
        /* post-rotation façon MDCT + normalisation 1/N */
        for (i = 0; i < 128; i++) {
            float ar = ffta[2 * i] * 0.0078125f, ai = ffta[2 * i + 1] * 0.0078125f;
            ffta[2 * i]     = ar * fftw[i] - ai * fftw[i + 1];
            ffta[2 * i + 1] = ar * fftw[i + 1] + ai * fftw[i];
        }
        if ((it & 63) == 0) h = hmix(h, fbits(ffta[5]) ^ (fbits(ffta[201]) << 1));
    }
    for (i = 0; i < 256; i += 17) h = hmix(h, fbits(ffta[i]));
    return h;
}

/* 4. produit de matrices 4×4 en double (moteur 3D) */
static void mat4(double *r, const double *a, const double *b)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r[4 * i + j] = a[4 * i + 0] * b[0 * 4 + j] + a[4 * i + 1] * b[1 * 4 + j]
                         + a[4 * i + 2] * b[2 * 4 + j] + a[4 * i + 3] * b[3 * 4 + j];
}

static u64 k_mat4(long n)
{
    long i;
    int j;
    u64 h = FNV_OFF;
    double acc = 0.0;
    for (i = 0; i < n; i++) {
        mA[0] = 0.125 + (double)(int)(i & 7) * 0.0009765625;   /* empêche le hissage */
        mat4(mC, mA, mB);
        acc += mC[(int)(i & 15)] * 1e-6;
        if ((i & 255) == 0) h = hmix(h, dbits(acc));
    }
    for (j = 0; j < 16; j++) h = hmix(h, dbits(mC[j]));
    return hmix(h, dbits(acc));
}

/* 5. Horner en double (polynôme de degré 15) */
static u64 k_horner(long n)
{
    long i;
    u64 h = FNV_OFF;
    double acc = 0.0;
    for (i = 0; i < n; i++) {
        double x = da[(int)(i & (N - 1))] * 0.25;
        double p = db[0];
        int k;
        for (k = 1; k < 16; k++) p = p * x + db[k];
        acc += p * 1e-7;
        if ((i & 255) == 0) h = hmix(h, dbits(acc) ^ dbits(p));
    }
    return hmix(h, dbits(acc));
}

/* 6. divisions en double */
static u64 k_divd(long n)
{
    long i;
    u64 h = FNV_OFF;
    double acc = 0.0;
    for (i = 0; i < n; i++) {
        int k = (int)(i & (N - 1));
        double a = da[k], b = db[k];
        acc += (a / b + b / (a + 4.0) + (a + 1.5) / (b + 0.5) + 1.0 / b) * 1e-7;
        if ((i & 255) == 0) h = hmix(h, dbits(acc));
    }
    return hmix(h, dbits(acc));
}

/* 7. divisions en float (fdivs) */
static u64 k_divf(long n)
{
    long i;
    u64 h = FNV_OFF;
    float acc = 0.0f;
    for (i = 0; i < n; i++) {
        int k = (int)(i & (N - 1));
        float a = fa[k], b = fb[k] + 3.0f;
        acc += (a / b + b / (a + 4.0f) + (a + 1.5f) / (b + 0.5f) + 1.0f / b) * 1e-6f;
        if ((i & 255) == 0) h = hmix(h, fbits(acc));
    }
    return hmix(h, fbits(acc));
}

/* 8. fmadd en double (gcc fusionne mul+add sur PowerPC) */
static u64 k_fmaddd(long n)
{
    long i;
    u64 h = FNV_OFF;
    double s0 = 0.5, s1 = 0.25, s2 = -0.5, s3 = 0.75;
    for (i = 0; i < n; i++) {
        int k = (int)(i & (N - 1));
        double a = da[k], b = db[k] * 0.125;
        s0 = s0 * 0.5 + a * b;
        s1 = s1 * 0.5 + b * a + 0.125;
        s2 = s2 * 0.5 - a * b;
        s3 = s3 * 0.5 + (a + b) * 0.25;
        if ((i & 255) == 0) h = hmix(h, dbits(s0) ^ dbits(s3));
    }
    return hmix(hmix(h, dbits(s1)), dbits(s2) ^ dbits(s0));
}

/* 9. fmadds en float */
static u64 k_fmaddf(long n)
{
    long i;
    u64 h = FNV_OFF;
    float s0 = 0.5f, s1 = 0.25f, s2 = -0.5f, s3 = 0.75f;
    for (i = 0; i < n; i++) {
        int k = (int)(i & (N - 1));
        float a = fa[k], b = fb[k] * 0.125f;
        s0 = s0 * 0.5f + a * b;
        s1 = s1 * 0.5f + b * a + 0.125f;
        s2 = s2 * 0.5f - a * b;
        s3 = s3 * 0.5f + (a + b) * 0.25f;
        if ((i & 255) == 0) h = hmix(h, fbits(s0) ^ fbits(s3));
    }
    return hmix(hmix(h, fbits(s1)), fbits(s2) ^ fbits(s0));
}

/* 10. conversions flottant ↔ entier (fctiwz/stfiwx et la ruse 0x43300000) */
static u64 k_conv(long n)
{
    long i;
    u64 h = FNV_OFF;
    int acc = 0;
    double dacc = 0.0;
    for (i = 0; i < n; i++) {
        int k = (int)(i & (N - 1));
        double x = da[k] * 1000.0 + (double)(int)(i & 1023);
        int  ix = (int)x;
        unsigned ux = (unsigned)(x < 0.0 ? -x : x);
        acc += ix ^ (int)ux;
        dacc += ((double)ix + (double)ux) * 1e-7;
        if ((i & 255) == 0) h = hmix(h, (u64)(u32)acc ^ dbits(dacc));
    }
    return hmix(hmix(h, (u64)(u32)acc), dbits(dacc));
}

/* 11. recopie lfs/stfs — instructions imposées par l'assembleur en ligne */
static u64 k_lfsstfs(long n)
{
    long i;
    int j;
    u64 h = FNV_OFF;
    for (i = 0; i < n; i++) {
        for (j = 0; j < 256; j++) {
            double d;
            __asm__ __volatile__("lfs %0,%1" : "=f"(d) : "m"(fsrc[j]));
            __asm__ __volatile__("stfs %1,%0" : "=m"(fdst[255 - j]) : "f"(d));
        }
        if ((i & 255) == 0) h = hmix(h, fbits(fdst[7]));
    }
    for (j = 0; j < 256; j += 29) h = hmix(h, fbits(fdst[j]));
    return h;
}

/* 12. AltiVec : vec_madd (vmaddfp) */
#ifdef __ALTIVEC__
#ifndef __APPLE_ALTIVEC__
#include <altivec.h>
#endif
#define HAVE_AV 1
static vector float VA[64], VB[64], VC[64];

static void init_av(void)
{
    int i, k;
    union { vector float v; float f[4]; } u;
    for (i = 0; i < 64; i++) {
        for (k = 0; k < 4; k++) u.f[k] = (float)uni();
        VA[i] = u.v;
        for (k = 0; k < 4; k++) u.f[k] = (float)uni();
        VB[i] = u.v;
        for (k = 0; k < 4; k++) u.f[k] = 0.5f;
        VC[i] = u.v;
    }
}

static u64 k_vmadd(long n)
{
    long i;
    int j, k;
    u64 h = FNV_OFF;
    union { vector float v; u32 w[4]; } u;
    union { vector float v; float f[4]; } hu;
    vector float half;
    hu.f[0] = hu.f[1] = hu.f[2] = hu.f[3] = 0.5f;
    half = hu.v;
    for (i = 0; i < n; i++) {
        for (j = 0; j < 64; j++) {
            /* point fixe : C ← C*0,5 + A  (borné, toujours inexact) */
            VC[j] = vec_madd(VC[j], half, VA[j]);
            VC[j] = vec_madd(VC[j], half, VB[j]);
        }
        if ((i & 255) == 0) { u.v = VC[3]; h = hmix(h, ((u64)u.w[0] << 32) | u.w[3]); }
    }
    for (j = 0; j < 64; j += 7) {
        u.v = VC[j];
        for (k = 0; k < 4; k++) h = hmix(h, (u64)u.w[k]);
    }
    return h;
}
#else
#define HAVE_AV 0
static void init_av(void) {}
static u64 k_vmadd(long n) { (void)n; return 0; }
#endif

/* 13. mélange libm : sin, cos, sqrt, pow */
static u64 k_libm(long n)
{
    long i;
    u64 h = FNV_OFF;
    double acc = 0.0;
    for (i = 0; i < n; i++) {
        int k = (int)(i & (N - 1));
        double x = da[k], y = db[k];
        acc += (sin(x) + cos(x) + sqrt(y) + pow(y, 1.3)) * 1e-7;
        if ((i & 63) == 0) h = hmix(h, dbits(acc));
    }
    return hmix(h, dbits(acc));
}

/* ========================================================== ordonnancement */
typedef struct { const char *nom; u64 (*fn)(long); long it; } kern_t;

/* Itérations calibrées pour ~2 s chacune sur le QEMU 9.2 patché POMPPC en mode
 * exact (~/src/qemu, hôte i7-10700F, 19/09/2026). `fpbench --calibrer` les
 * recalcule si l'hôte ou le binaire changent beaucoup. */
static kern_t K[] = {
    { "entier-temoin",   k_entier, 400000000L },
    { "mdct-papillons",  k_mdct,       26000L },
    { "fft-inverse",     k_fft,         9000L },
    { "mat4-double",     k_mat4,     1400000L },
    { "horner-double",   k_horner,   4800000L },
    { "div-double",      k_divd,     6300000L },
    { "div-float",       k_divf,     4800000L },
    { "fmadd-double",    k_fmaddd,   9600000L },
    { "fmadd-float",     k_fmaddf,   6400000L },
    { "conv-fp-entier",  k_conv,     9900000L },
    { "lfs-stfs",        k_lfsstfs,  1400000L },
    { "altivec-vmadd",   k_vmadd,     248000L },
    { "libm-melange",    k_libm,      357000L }
};
#define NK ((int)(sizeof(K) / sizeof(K[0])))

int main(int argc, char **argv)
{
    int i, calib = 0;
    long pct = 100;

    mach_timebase_info(&g_tb);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--calibrer")) calib = 1;
        else { pct = atol(argv[i]); if (pct < 1) pct = 1; }
    }
    init_data();
    init_av();

    printf("# fpbench v1 — flottant PowerPC sous QEMU (G4 7400)\n");
    printf("# echelle=%ld%% altivec=%d timebase=%u/%u\n",
           pct, HAVE_AV, (unsigned)g_tb.numer, (unsigned)g_tb.denom);
    printf("# fpscr_a_l_entree=%08x\n", fpscr_get());

    if (calib) {
        printf("# calibrage : iterations proposees pour ~2,0 s\n");
        printf("# %-16s %8s %12s %12s\n", "noyau", "ms", "iter_test", "iter_2s");
        for (i = 0; i < NK; i++) {
            long it = K[i].it / 20;
            u64 t0, us;
            if (it < 1) it = 1;
            t0 = tick();
            K[i].fn(it);
            us = us_since(t0);
            if (us < 1) us = 1;
            printf("# %-16s %8lu %12ld %12ld\n", K[i].nom,
                   (unsigned long)(us / 1000), it,
                   (long)((double)it * 2000000.0 / (double)us));
        }
        printf("# fin\n");
        return 0;
    }

    printf("%-16s %10s %12s %18s\n", "noyau", "secondes", "iterations", "somme_de_controle");
    for (i = 0; i < NK; i++) {
        long it = (long)((double)K[i].it * (double)pct / 100.0);
        u64 t0, ms, sum;
        unsigned fp;
        if (it < 1) it = 1;
        t0 = tick();
        sum = K[i].fn(it);
        ms = ms_since(t0);
        fp = fpscr_get();
        printf("%-16s %6lu.%03lu %12ld   %016llx\n", K[i].nom,
               (unsigned long)(ms / 1000), (unsigned long)(ms % 1000), it,
               (unsigned long long)sum);
        fflush(stdout);
        if (i == 0) printf("# fpscr_apres_%s=%08x\n", K[i].nom, fp);
    }
    printf("# fpscr_a_la_sortie=%08x\n", fpscr_get());
    printf("# fin\n");
    return 0;
}
