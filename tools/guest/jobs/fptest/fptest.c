/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * fptest.c - equivalence du flottant scalaire simple precision entre les
 * helpers de QEMU et le chemin court de x-fp-inline
 * (patches/tcg/0007-ppc-fp-inline.patch, docs/tcg-g4.md section 15).
 *
 * Execute les VRAIES instructions dans l'invite Tiger : fadds fsubs fmuls
 * fmadds fmsubs fnmadds fnmsubs fcmpu, sur des operandes charges par lfs
 * (float32 : zeros, denormaux, normaux, limites, infinis, NaN silencieux et
 * signalants) et par lfd (doubles sans equivalent simple, denormaux et
 * exposants hors plage simple, NaN doubles), catalogue croise puis
 * aleatoire.  Chaque resultat, le FPSCR relu par mffs et le CR (fcmpu) sont
 * haches, dans onze etats du FPSCR :
 *   P0 FPSCR = 0 avant chaque instruction (XX efface : jamais amorce)
 *   P1 XX (amorce, arrondi au plus proche : le cas du chemin court)
 *   P2-P4 XX et RN = 1, 2, 3 (vers zero, +inf, -inf)
 *   P5 XX, VE, ZE (porte ouverte : VE/ZE ne la ferment pas)
 *   P6-P8 XX et XE, OE, UE (trappes armees ; MSR[FE] = 0 sous Tiger)
 *   P9 FPSCR laisse s'accumuler (le cas reel)
 *   P10 FPSCR au hasard avant chaque instruction
 * La sortie doit etre IDENTIQUE octet pour octet entre x-fp-inline=off et
 * =on (hors lignes « banc ») ; avec x-fp-verify=on, QEMU verifie en plus
 * chaque passage par le chemin court contre les helpers (stderr de QEMU).
 *
 *   fptest [NRAND]            (defaut 2^18 vecteurs aleatoires par op et etat)
 *   fptest banc N             bancs de N iterations
 *   fptest d [NRAND]          les formes double precision (fadd fsub fmul fmadd
 *                             fmsub fnmadd fnmsub) : x-fp-native64, tcg/0016,
 *                             docs/tcg-g4.md section 24 ; operandes doubles
 *                             (sous-normaux, voisins de DBL_MIN, debordements,
 *                             annulations exactes)
 *   fptest banc-d N           bancs double (chaine, sommets)
 *   fptest c [NRAND]          comparaisons et conversions : frsp fctiw fctiwz
 *                             fcmpo fdivs fdiv fsel (x-fp-native-cmp, tcg/0033,
 *                             docs/tcg-g4.md section 33) ; operandes simples,
 *                             doubles, entiers et demi-entiers, voisins de
 *                             +-2^31, de FLT_MAX + 1/2 ulp, de FLT_MIN ; CR
 *                             de fcmpo
 *   fptest banc-c N           bancs de ces instructions (latence et debit)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef unsigned int u32;
typedef unsigned long long u64;

static u64 fnv = 1469598103934665603ULL;
static void h(const void *p, size_t n)
{
    const unsigned char *c = p;
    while (n--) { fnv ^= *c++; fnv *= 1099511628211ULL; }
}

/* ---- FPSCR ---- */
static void setfpscr(u32 v)
{
    union { double d; u64 u; } x;
    x.u = v;
    __asm__ __volatile__("mtfsf 0xff,%0" : : "f"(x.d));
}
static u32 getfpscr(void)
{
    union { double d; u64 u; } x;
    __asm__ __volatile__("mffs %0" : "=f"(x.d));
    return (u32)x.u;
}

/* ---- operandes : bits exacts dans les registres flottants ---- */
static double ld_s(u32 bits)
{
    double d;
    __asm__ __volatile__("lfs %0,0(%1)" : "=f"(d) : "b"(&bits) : "memory");
    return d;
}
static double ld_d(u64 bits)
{
    double d;
    __asm__ __volatile__("lfd %0,0(%1)" : "=f"(d) : "b"(&bits) : "memory");
    return d;
}
static u64 bits_d(double d)
{
    u64 u;
    memcpy(&u, &d, 8);
    return u;
}

enum { ADD, SUB, MUL, MADD, MSUB, NMADD, NMSUB, CMPU,
       ADDD, SUBD, MULD, MADDD, MSUBD, NMADDD, NMSUBD, NOPS,
       /* mode c (tcg/0033) : hors des modes d'origine, empreintes inchangees */
       FRSP = NOPS, FCTIW, FCTIWZ, CMPO, DIVS, DIVD, FSEL, NOPSC };
static const char *names[NOPSC] = {
    "fadds", "fsubs", "fmuls", "fmadds", "fmsubs", "fnmadds", "fnmsubs", "fcmpu",
    "fadd", "fsub", "fmul", "fmadd", "fmsub", "fnmadd", "fnmsub",
    "frsp", "fctiw", "fctiwz", "fcmpo", "fdivs", "fdiv", "fsel"
};

/* une instruction ; r = resultat (ou CR pour fcmpu) */
static u64 run(int op, double a, double b, double c)
{
    double r = 0;
    u32 cr;
    switch (op) {
    case ADD:   __asm__ __volatile__("fadds %0,%1,%2" : "=f"(r) : "f"(a), "f"(b)); break;
    case SUB:   __asm__ __volatile__("fsubs %0,%1,%2" : "=f"(r) : "f"(a), "f"(b)); break;
    case MUL:   __asm__ __volatile__("fmuls %0,%1,%2" : "=f"(r) : "f"(a), "f"(c)); break;
    case MADD:  __asm__ __volatile__("fmadds %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(c), "f"(b)); break;
    case MSUB:  __asm__ __volatile__("fmsubs %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(c), "f"(b)); break;
    case NMADD: __asm__ __volatile__("fnmadds %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(c), "f"(b)); break;
    case NMSUB: __asm__ __volatile__("fnmsubs %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(c), "f"(b)); break;
    case ADDD:  __asm__ __volatile__("fadd %0,%1,%2" : "=f"(r) : "f"(a), "f"(b)); break;
    case SUBD:  __asm__ __volatile__("fsub %0,%1,%2" : "=f"(r) : "f"(a), "f"(b)); break;
    case MULD:  __asm__ __volatile__("fmul %0,%1,%2" : "=f"(r) : "f"(a), "f"(c)); break;
    case MADDD: __asm__ __volatile__("fmadd %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(c), "f"(b)); break;
    case MSUBD: __asm__ __volatile__("fmsub %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(c), "f"(b)); break;
    case NMADDD: __asm__ __volatile__("fnmadd %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(c), "f"(b)); break;
    case NMSUBD: __asm__ __volatile__("fnmsub %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(c), "f"(b)); break;
    case FRSP:  __asm__ __volatile__("frsp %0,%1" : "=f"(r) : "f"(b)); break;
    case FCTIW: __asm__ __volatile__("fctiw %0,%1" : "=f"(r) : "f"(b)); break;
    case FCTIWZ: __asm__ __volatile__("fctiwz %0,%1" : "=f"(r) : "f"(b)); break;
    case DIVS:  __asm__ __volatile__("fdivs %0,%1,%2" : "=f"(r) : "f"(a), "f"(b)); break;
    case DIVD:  __asm__ __volatile__("fdiv %0,%1,%2" : "=f"(r) : "f"(a), "f"(b)); break;
    case FSEL:  __asm__ __volatile__("fsel %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(c), "f"(b)); break;
    case CMPO:
        __asm__ __volatile__("fcmpo cr1,%1,%2\n\tmfcr %0" : "=r"(cr) : "f"(a), "f"(b) : "cr1");
        return (cr >> 24) & 0xf;
    default:
        __asm__ __volatile__("fcmpu cr1,%1,%2\n\tmfcr %0" : "=r"(cr) : "f"(a), "f"(b) : "cr1");
        return (cr >> 24) & 0xf;
    }
    return bits_d(r);
}

/* ---- catalogue ---- */
static const u32 cat_s[] = {
    0x00000000, 0x00000001, 0x00000002, 0x007fffff, 0x00400000,
    0x00800000, 0x00800001, 0x00ffffff, 0x01000000, 0x0c800000,
    0x1f800000, 0x3f800000, 0x3f800001, 0x3f7fffff, 0x3fc00000,
    0x3dcccccd, 0x4b7fffff, 0x4b800000, 0x4b800001, 0x33000000,
    0x33800000, 0x34000000, 0x5f800000, 0x7e800000, 0x7f000000,
    0x7f7fffff, 0x7f7ffffe, 0x7f800000, 0x7fc00000, 0x7fa00000,
    0x7f800001, 0x7fffffff, 0x20000000, 0x1f000000, 0x40490fdb,
};
static const u64 cat_d[] = {
    0x3fb999999999999aULL, 0x3fd5555555555555ULL, 0x01a56e1fc2f8f359ULL,
    0x7e37e43c8800759cULL, 0x0000000000000001ULL, 0x0010000000000000ULL,
    0x3ff0000000000001ULL, 0x47efffffe0000001ULL, 0x380fffffbfffffffULL,
    0x3800000000000000ULL, 0x3810000000000000ULL, 0x47e0000000000000ULL,
    0x47f0000000000000ULL, 0x3ff0000010000000ULL, 0x3ff0000020000000ULL,
    0x7ff4000000000000ULL, 0x7ff0000000000001ULL, 0x7ff8000000000000ULL,
};
#define NCS (sizeof(cat_s) / sizeof(cat_s[0]))
#define NCD (sizeof(cat_d) / sizeof(cat_d[0]))
#define NCAT (2 * (NCS + NCD))
static double cat[NCAT];

static void build_cat(void)
{
    unsigned i, k = 0;
    for (i = 0; i < NCS; i++) {
        cat[k++] = ld_s(cat_s[i]);
        cat[k++] = ld_s(cat_s[i] ^ 0x80000000u);
    }
    for (i = 0; i < NCD; i++) {
        cat[k++] = ld_d(cat_d[i]);
        cat[k++] = ld_d(cat_d[i] ^ 0x8000000000000000ULL);
    }
}

/* ---- aleatoire (xorshift64*) ---- */
static u64 rs = 0x9e3779b97f4a7c15ULL;
static u64 rnd(void)
{
    rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27;
    return rs * 2685821657736338717ULL;
}
static double rnd_operand(void)
{
    u64 x = rnd();
    u32 f;
    switch (x & 15) {
    case 0: return cat[(x >> 8) % NCAT];
    case 1: return ld_d(rnd());
    case 2: return ld_s((u32)(x >> 32));
    case 3: f = ((u32)(x >> 32) & 0x807fffff) | (u32)(0xf0 + ((x >> 8) & 0xf)) << 23;
            return ld_s(f);
    case 4: f = ((u32)(x >> 32) & 0x807fffff) | (u32)((x >> 8) & 0x1f) << 23;
            return ld_s(f);
    case 5: f = ((u32)(x >> 32) & 0xff800000) | ((u32)(x >> 8) & 0xfff) << 11;
            return ld_s(f);
    default:
            f = ((u32)(x >> 32) & 0x807fffff) | (u32)(0x60 + ((x >> 8) & 0x3f)) << 23;
            return ld_s(f);
    }
}

/*
 * Operandes doubles : exposant choisi pres des bords (sous-normaux, DBL_MIN,
 * produits qui sous-debordent ou debordent, sommes qui s'annulent), mantisse
 * au hasard, signe au hasard.
 */
static double rnd_operand_d(void)
{
    u64 x = rnd(), m = rnd() & 0x000fffffffffffffULL, sg = x & 0x8000000000000000ULL;
    unsigned e;
    switch (x & 15) {
    case 0: return cat[(x >> 8) % NCAT];
    case 1: return ld_d(rnd());                                  /* tout */
    case 2: return ld_d(sg | m);                                 /* sous-normal */
    case 3: return ld_d(sg | m >> ((x >> 8) & 31));              /* petit sous-normal */
    case 4: e = 1 + ((x >> 8) & 7); break;                       /* pres de DBL_MIN */
    case 5: e = 0x1ff + ((x >> 8) & 0x3ff); break;               /* produits minuscules */
    case 6: e = 0x5ff + ((x >> 8) & 0x1ff); break;               /* produits enormes */
    case 7: e = 0x7f6 + ((x >> 8) & 7); break;                   /* pres du max */
    case 8: return ld_d(sg | 0x7ff0000000000000ULL | (m & ((x >> 8) & 1 ? 0 : ~0ULL)));
    default: e = 0x3c0 + ((x >> 8) & 0x7f); break;               /* ordinaire */
    }
    return ld_d(sg | (u64)e << 52 | m);
}

/*
 * ---- mode c (tcg/0033) : catalogue elargi et operandes des conversions ----
 * (les modes d'origine gardent leur catalogue : leurs empreintes ne bougent pas)
 */
static const u64 cat_x[] = {
    0x41dfffffffc00000ULL, 0x41dfffffffe00000ULL, 0x41dffffffff00000ULL, /* 2^31 - 1, -0.5, -0.25 */
    0x41e0000000000000ULL, 0x41e0000000100000ULL,                        /* 2^31, 2^31 + 0.5 */
    0xc1e0000000080000ULL, 0xc1e0000000100000ULL, 0xc1e0000000200000ULL, /* -2^31 - 0.25, -0.5, -1 */
    0x43e0000000000000ULL, 0x3fe0000000000000ULL, 0x3ff8000000000000ULL, /* 2^63, 0.5, 1.5 */
    0x4004000000000000ULL, 0x3fdfffffffffffffULL, 0x3fe0000000000001ULL, /* 2.5, 0.5 -+ ulp */
    0x47efffffe0000000ULL, 0x47effffff0000000ULL, 0x47efffffefffffffULL, /* FLT_MAX, + 1/2 ulp, dessous */
    0x47effffff0000001ULL, 0x47effffff8000000ULL,
    0x380fffffffffffffULL, 0x380ffffff0000000ULL, 0x380ffffff0000001ULL, /* sous FLT_MIN */
    0x36a0000000000000ULL, 0x369fffffffffffffULL, 0x3690000000000000ULL, /* 2^-149, ..., 2^-150 */
    0x3e70000000000000ULL, 0x4170000000000000ULL, 0x4330000000000000ULL, /* 2^-24, 2^24, 2^52 */
};
#define NCX (sizeof(cat_x) / sizeof(cat_x[0]))
#define NCATC (NCAT + 2 * NCX)
static double catc[NCATC];

static void build_catc(void)
{
    unsigned i, k = NCAT;
    for (i = 0; i < NCAT; i++) {
        catc[i] = cat[i];
    }
    for (i = 0; i < NCX; i++) {
        catc[k++] = ld_d(cat_x[i]);
        catc[k++] = ld_d(cat_x[i] ^ 0x8000000000000000ULL);
    }
}

static double rnd_operand_c(void)
{
    u64 x = rnd(), k = rnd(), b;
    switch (x & 7) {
    case 0: return catc[(x >> 8) % NCATC];
    case 1: case 2: return rnd_operand();
    case 3: return rnd_operand_d();
    case 4: /* entier, demi-entier ou quart d'entier, de 0 a 2^33 */
        b = k >> (31 + (unsigned)((x >> 8) % 33));
        return ld_d(bits_d((double)(long long)b / (double)(1 << ((x >> 16) & 3))) |
                    ((x & 0x1000000) ? 0x8000000000000000ULL : 0));
    case 5: /* a quelques ulp de +-2^31 */
        b = ((x & 0x100) ? 0x41e0000000000000ULL : 0xc1e0000000000000ULL) +
            (k % 8192) - 4096;
        return ld_d(b);
    case 6: /* a quelques ulp de FLT_MAX + 1/2 ulp, ou de FLT_MIN */
        b = ((x & 0x100) ? 0x47effffff0000000ULL : 0x3810000000000000ULL) +
            (k % 65536) - 32768;
        return ld_d(b | (x & 0x8000000000000000ULL));
    default:
        return ld_s((u32)(x >> 32));
    }
}

/* ---- etats du FPSCR ---- */
#define XX 0x02000000u
static const u32 phase_fpscr[] = {
    0, XX, XX | 1, XX | 2, XX | 3, XX | 0x80 | 0x10, XX | 0x08, XX | 0x40, XX | 0x20,
};
#define NPH 11

static u32 fpscr_for(int ph)
{
    if (ph < 9) {
        return phase_fpscr[ph];
    }
    if (ph == 10) {
        /* au hasard, hors FEX/VX (calcules) et bits reserves */
        return (u32)rnd() & 0x9ffff0ffu & ~0x00000800u;
    }
    return 0;   /* P9 : ne sert pas */
}

static u64 phash;
static void rec(int ph, int op, double a, double b, double c, int show)
{
    u32 f0 = 0, f1;
    u64 r;
    unsigned char buf[24];

    if (ph != 9) {
        f0 = fpscr_for(ph);
        setfpscr(f0);
    }
    r = run(op, a, b, c);
    f1 = getfpscr();
    memcpy(buf, &r, 8);
    memcpy(buf + 8, &f1, 4);
    memcpy(buf + 12, &f0, 4);
    h(buf, 16);
    {
        int k;
        for (k = 0; k < 16; k++) { phash ^= buf[k]; phash *= 1099511628211ULL; }
    }
    if (show) {
        printf("  %s a=%016llx b=%016llx c=%016llx fpscr %08x -> %016llx %08x\n",
               names[op], bits_d(a), bits_d(b), bits_d(c), f0, r, f1);
    }
}

static double now_ms(void)
{
    struct timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec * 1000.0 + t.tv_usec / 1000.0;
}

/*
 * Bancs (n = iterations) : 1. chaine dependante fmadds/fmuls/fmsubs/fadds
 * (latence) ; 2. transformation de sommets 4x4 comme un jeu (lfs, 12 fmadds
 * + 4 fmuls, stfs ; 1024 sommets par passe) ; 3. fcmpu + branchement sur un
 * tableau (min/max). Le FPSCR est d'abord amorce (XX pose par une division
 * inexacte), comme dans un vrai programme.
 */
#define NV 1024
static float vin[NV][4], vout[NV][4], mat[16];

static void banc(long n)
{
    volatile float three = 3.0f;
    float x, y = 1.0001f, z = 0.9999f, w = 0.5f, mn, mx;
    double t0;
    long i, p, np;
    int j;

    setfpscr(0);
    x = 1.0f / three;                   /* inexact : pose XX */
    t0 = now_ms();
    for (i = 0; i < n; i++) {
        x = x * y + z;
        x = x * w;
        x = x * y - z;
        x = x + w;
    }
    printf("banc : chaine %ld x 4 %.0f ms (%g)\n", n, now_ms() - t0, (double)x);

    for (j = 0; j < 16; j++) {
        mat[j] = (float)(j + 1) / three;
    }
    for (j = 0; j < NV; j++) {
        vin[j][0] = j / three; vin[j][1] = 1.0f - j / three;
        vin[j][2] = j * 0.25f; vin[j][3] = 1.0f;
    }
    np = n / NV;
    t0 = now_ms();
    for (p = 0; p < np; p++) {
        for (j = 0; j < NV; j++) {
            float a = vin[j][0], b = vin[j][1], c = vin[j][2], d = vin[j][3];
            vout[j][0] = mat[0] * a + mat[4] * b + mat[8] * c + mat[12] * d;
            vout[j][1] = mat[1] * a + mat[5] * b + mat[9] * c + mat[13] * d;
            vout[j][2] = mat[2] * a + mat[6] * b + mat[10] * c + mat[14] * d;
            vout[j][3] = mat[3] * a + mat[7] * b + mat[11] * c + mat[15] * d;
        }
        mat[p & 15] += vout[p & (NV - 1)][p & 3] * 1e-9f;
    }
    printf("banc : sommets %ld x 16 %.0f ms (%g)\n", np * NV, now_ms() - t0,
           (double)vout[7][2]);

    t0 = now_ms();
    mn = 1e30f; mx = -1e30f;
    for (p = 0; p < np; p++) {
        for (j = 0; j < NV; j++) {
            float v = vout[j][p & 3];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        vout[p & (NV - 1)][0] += 1.0f;
    }
    printf("banc : fcmpu %ld x 2 %.0f ms (%g %g)\n", np * NV, now_ms() - t0,
           (double)mn, (double)mx);
}

static double vind[NV][4], voutd[NV][4], matd[16];

static void banc_d(long n)
{
    volatile double three = 3.0;
    double x, y = 1.0001, z = 0.9999, w = 0.5, t0;
    long i, p, np;
    int j;

    setfpscr(0);
    x = 1.0 / three;                    /* inexact : pose XX */
    t0 = now_ms();
    for (i = 0; i < n; i++) {
        x = x * y + z;
        x = x * w;
        x = x * y - z;
        x = x + w;
    }
    printf("banc-d : chaine %ld x 4 %.0f ms (%g)\n", n, now_ms() - t0, x);
    for (j = 0; j < 16; j++) {
        matd[j] = (j + 1) / three;
    }
    for (j = 0; j < NV; j++) {
        vind[j][0] = j / three; vind[j][1] = 1.0 - j / three;
        vind[j][2] = j * 0.25; vind[j][3] = 1.0;
    }
    np = n / NV;
    t0 = now_ms();
    for (p = 0; p < np; p++) {
        for (j = 0; j < NV; j++) {
            double a = vind[j][0], b = vind[j][1], c = vind[j][2], d = vind[j][3];
            voutd[j][0] = matd[0] * a + matd[4] * b + matd[8] * c + matd[12] * d;
            voutd[j][1] = matd[1] * a + matd[5] * b + matd[9] * c + matd[13] * d;
            voutd[j][2] = matd[2] * a + matd[6] * b + matd[10] * c + matd[14] * d;
            voutd[j][3] = matd[3] * a + matd[7] * b + matd[11] * c + matd[15] * d;
        }
        matd[p & 15] += voutd[p & (NV - 1)][p & 3] * 1e-9;
    }
    printf("banc-d : sommets %ld x 16 %.0f ms (%g)\n", np * NV, now_ms() - t0,
           voutd[7][2]);
}

/*
 * Bancs de mode c (tcg/0033) : chaque instruction en boucle, sur des
 * operandes qui prennent le chemin court (FPSCR amorce par une division
 * inexacte) ; frsp et fctiwz comme les produit le code compile (double ->
 * float, float -> int), fdivs, fcmpo + branchement, fsel.
 */
static void banc_c(long n)
{
    volatile double three = 3.0;
    double x = 1.0, t0, acc = 0, f = 0.0;
    float fs = 1.0f, q = 1.0f;
    long i, si = 0;
    u32 cr;

    setfpscr(0);
    x = 1.0 / three;                    /* inexact : pose XX */
    t0 = now_ms();
    for (i = 0; i < n; i++) {           /* frsp (debit) : x -> simple */
        double r;
        __asm__ __volatile__("frsp %0,%1" : "=f"(r) : "f"(x));
        acc += r;
        x += 0.0001;
    }
    printf("banc-c : frsp %ld %.0f ms (%g)\n", n, now_ms() - t0, acc);
    x = 1.0 / three;
    t0 = now_ms();
    for (i = 0; i < n; i++) {           /* fctiwz + stfd + lwz : (int)x */
        double r;
        union { double d; u64 u; } v;
        __asm__ __volatile__("fctiwz %0,%1" : "=f"(r) : "f"(x));
        v.d = r;
        si += (long)(u32)v.u;
        x += 0.37;
    }
    printf("banc-c : fctiwz %ld %.0f ms (%ld)\n", n, now_ms() - t0, si);
    t0 = now_ms();
    for (i = 0; i < n; i++) {           /* fdivs, chaine */
        __asm__ __volatile__("fdivs %0,%1,%2" : "=f"(q) : "f"((double)fs), "f"((double)1.0001f));
        fs = q + 0.5f;
    }
    printf("banc-c : fdivs %ld %.0f ms (%g)\n", n, now_ms() - t0, (double)fs);
    x = 0.25; acc = 0;
    t0 = now_ms();
    for (i = 0; i < n; i++) {           /* fcmpo + branchement */
        __asm__ __volatile__("fcmpo cr1,%1,%2\n\tmfcr %0" : "=r"(cr) : "f"(x), "f"(f) : "cr1");
        if (cr & 0x04000000) {
            acc += 1.0;
        }
        f += 0.5;
        if (f > 1.0) {
            f = -1.0;
        }
    }
    printf("banc-c : fcmpo %ld %.0f ms (%g)\n", n, now_ms() - t0, acc);
    f = -0.5; acc = 0;
    t0 = now_ms();
    for (i = 0; i < n; i++) {           /* fsel */
        double r;
        __asm__ __volatile__("fsel %0,%1,%2,%3" : "=f"(r) : "f"(f), "f"(1.0), "f"(2.0));
        acc += r;
        f = -f;
    }
    printf("banc-c : fsel %ld %.0f ms (%g)\n", n, now_ms() - t0, acc);
}

int main(int argc, char **argv)
{
    long nrand = 1L << 18;
    int ph, op;
    unsigned x, y, z;
    long i;
    u64 total = 0;

    int dbl = 0, cmode = 0, op0 = 0, op1 = CMPU + 1;

    if (argc > 2 && !strcmp(argv[1], "banc")) {
        banc(atol(argv[2]));
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "banc-d")) {
        banc_d(atol(argv[2]));
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "banc-c")) {
        banc_c(atol(argv[2]));
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "d")) {
        dbl = 1; op0 = ADDD; op1 = NOPS;
        argc--; argv++;
    } else if (argc > 1 && !strcmp(argv[1], "c")) {
        cmode = 1; op0 = FRSP; op1 = NOPSC;
        argc--; argv++;
    }
    if (argc > 1) {
        nrand = atol(argv[1]);
    }
    build_cat();
    build_catc();
    for (ph = 0; ph < NPH; ph++) {
        for (op = op0; op < op1; op++) {
            u64 n = 0;
            int bop = dbl ? op - ADDD : op;
            int fma = bop >= MADD && bop <= NMSUB;
            phash = 1469598103934665603ULL;
            rs = 0x9e3779b97f4a7c15ULL + ph * 131 + op;
            if (ph == 9) {
                setfpscr(0);
            }
            if (cmode) {
                /* catalogue croise elargi, puis aleatoire (a, b, c independants) */
                for (x = 0; x < NCATC; x++) {
                    if (op == FRSP || op == FCTIW || op == FCTIWZ) {
                        rec(ph, op, 0, catc[x], 0, 0);
                        n++;
                        continue;
                    }
                    for (y = 0; y < NCATC; y++) {
                        rec(ph, op, catc[x], catc[y], catc[(x * 7 + y * 3) % NCATC], 0);
                        n++;
                    }
                }
                for (i = 0; i < nrand; i++) {
                    double a = rnd_operand_c(), b = rnd_operand_c(), c = rnd_operand_c();
                    if ((i & 7) == 5 && op == DIVS) {
                        /* diviseur a mantisse courte : quotients pile sur un demi-ulp */
                        b = ld_s(0x3f800000 | ((u32)rnd() & 0x7ff) << 12);
                    }
                    rec(ph, op, a, b, c, i < 2 && ph == 1);
                    n++;
                }
                printf("P%-2d %-8s %8llu vecteurs  empreinte %016llx\n", ph, names[op],
                       n, phash);
                total += n;
                continue;
            }
            /* catalogue croise (les FMA : croisement complet a b c en P1 seulement) */
            for (x = 0; x < NCAT; x++) {
                for (y = 0; y < NCAT; y++) {
                    if (fma) {
                        unsigned zn = ph == 1 ? NCAT : 4;
                        for (z = 0; z < zn; z++) {
                            unsigned zz = ph == 1 ? z : (x * 7 + y * 3 + z * 11) % NCAT;
                            rec(ph, op, cat[x], cat[zz], cat[y], 0);
                            n++;
                        }
                    } else if (bop == MUL) {
                        rec(ph, op, cat[x], 0, cat[y], 0);
                        n++;
                    } else {
                        rec(ph, op, cat[x], cat[y], 0, 0);
                        n++;
                    }
                }
            }
            for (i = 0; i < nrand; i++) {
                double a, b, c;
                if (dbl) {
                    a = rnd_operand_d(); b = rnd_operand_d(); c = rnd_operand_d();
                } else {
                    a = rnd_operand(); b = rnd_operand(); c = rnd_operand();
                }
                if ((i & 7) == 3 && !dbl) {
                    /* annulation exacte ou presque */
                    float fa = (float)a, fc = (float)c, p;
                    p = op == ADD ? -fa : op == SUB ? fa : -(fa * fc);
                    b = (double)p;
                } else if ((i & 7) == 3) {
                    /* annulation exacte, voisine (1 ulp) ou produit arrondi oppose */
                    u64 k = rnd();
                    double p = bop == ADD ? -a : bop == SUB ? a : -(a * c);
                    b = (k & 3) == 0 ? p : ld_d(bits_d(p) + ((k & 4) ? 1 : -1));
                }
                rec(ph, op, a, b, c, i < 2 && ph == 1);
                n++;
            }
            printf("P%-2d %-8s %8llu vecteurs  empreinte %016llx\n", ph, names[op],
                   n, phash);
            total += n;
        }
    }
    printf("total %llu instructions ; empreinte %016llx\n", total, fnv);
    return 0;
}
