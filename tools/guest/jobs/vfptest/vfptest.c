/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * vfptest.c - equivalence de vaddfp/vsubfp/vmaddfp/vnmsubfp entre les helpers
 * d'origine et le chemin rapide a 4 voies (propriete x-vfp-fast,
 * patches/tcg/0003-ppc-vfp-fast.patch, docs/tcg-g4.md section 9).
 *
 * Execute les VRAIES instructions AltiVec dans l'invite Tiger, VSCR[NJ] a 0
 * puis a 1, sur :
 *   1. un catalogue de 40 valeurs limites croise sur une voie (40^2 pour
 *      add/sub, 40^3 pour les FMA), les trois autres voies normales ;
 *   2. N vecteurs aleatoires (defaut 2^22) : voies normales, extremes,
 *      zeros, denormaux, infinis, NaN, annulations exactes ;
 * et hache les resultats. La sortie doit etre IDENTIQUE octet pour octet
 * entre x-vfp-fast=off et =on (empreinte FNV en derniere ligne).
 * Plus vperm (patches/tcg/0004, x-vperm-fast) : chaque octet de controle a
 * chaque position, et des vecteurs aleatoires (dont r = a = c).
 * Un banc (BANC=N) : N x (2 vmaddfp + vaddfp + vsubfp) dependants, puis
 * N x 4 vperm dependants, temps en ms.
 * Mode c (vfptest c [BANC], patches/tcg/0034, x-vfp-native-cmp) : voir plus bas.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#ifndef __APPLE__
#include <altivec.h>      /* powerpc-linux-gnu-gcc -maltivec (tools/tcg/vfpcmp-user.sh) */
#endif

typedef unsigned int u32;
typedef unsigned long long u64;

static u64 fnv = 1469598103934665603ULL;
static void h(const void *p, size_t n)
{
    const unsigned char *c = p;
    while (n--) { fnv ^= *c++; fnv *= 1099511628211ULL; }
}
static void out(const char *s)
{
    fputs(s, stdout);
    h(s, strlen(s));
}

typedef union { vector float v; u32 u[4]; } V;

static u32 hh[4];
static void acc(const V *r)
{
    int i;
    for (i = 0; i < 4; i++) {
        hh[i] = (hh[i] ^ r->u[i]) * 16777619u;
    }
}

static void run4(const V *a, const V *b, const V *c)
{
    V r;
    r.v = vec_add(a->v, b->v); acc(&r);
    r.v = vec_sub(a->v, b->v); acc(&r);
    r.v = vec_madd(a->v, c->v, b->v); acc(&r);
    r.v = vec_nmsub(a->v, c->v, b->v); acc(&r);
}

static u64 st = 0x5eed;
static u32 rnd(void)
{
    st = st * 6364136223846793005ULL + 1442695040888963407ULL;
    return (u32)(st >> 32);
}
static u32 mkf(u32 s, u32 e, u32 f) { return s << 31 | (e & 0xff) << 23 | (f & 0x7fffff); }
static u32 lane(void)
{
    u32 r = rnd(), f = rnd() & 0x7fffff, s = r & 1, k = (r >> 8) % 100, e = r >> 16;
    if (k < 55) return mkf(s, 112 + e % 32, f);
    if (k < 70) return mkf(s, 1 + e % 254, f);
    if (k < 76) return mkf(s, 1 + e % 6, f);
    if (k < 82) return mkf(s, 249 + e % 6, f);
    if (k < 88) return s << 31;
    if (k < 92) return mkf(s, 0, f | 1);
    if (k < 95) return mkf(s, 0xff, 0);
    if (k < 98) return mkf(s, 0xff, f | 0x400000);
    return mkf(s, 0xff, (f & 0x3fffff) | 1);
}

static const u32 CAT[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x80800000,
    0x00800001, 0x01000000, 0x3f800000, 0xbf800000, 0x3f800001, 0x3f7fffff,
    0x3f000000, 0x33800000, 0x33000000, 0x4b800000, 0x4b7fffff, 0x7f7fffff,
    0xff7fffff, 0x7f000000, 0x1f800000, 0x1f000000, 0x20000000, 0x5f800000,
    0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001, 0x3eaaaaab, 0xbeaaaaab,
    0x40490fdb, 0x3dcccccd, 0x2d000000, 0x52000000, 0x00200000, 0x3fc00000,
    0x3fffffff, 0x0d800000, 0x71800000, 0x7f7ffff0,
};
#define NCAT (sizeof CAT / sizeof CAT[0])

/*
 * ---- mode c (patches/tcg/0034, x-vfp-native-cmp, docs/tcg-g4.md §34) ----
 * vcmpeqfp vcmpgefp vcmpgtfp vcmpbfp et leurs formes Rc (CR6 relu par mfcr),
 * vcfux vcfsx vctuxs vctsxs pour chaque uim 0..31 (VSCR[SAT] relu par
 * mfvscr, remis a 0 avant chaque conversion), VSCR[NJ] a 0 puis a 1 ;
 * catalogue croise sur une voie puis vecteurs aleatoires. Empreinte FNV.
 *   vfptest c [BANC]
 */
typedef union { vector unsigned int v; vector float f; u32 u[4]; } W;

#define U32X(M) M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7) M(8) M(9) M(10) \
    M(11) M(12) M(13) M(14) M(15) M(16) M(17) M(18) M(19) M(20) M(21)   \
    M(22) M(23) M(24) M(25) M(26) M(27) M(28) M(29) M(30) M(31)

static void cvt(int op, int uim, const W *b, W *r)
{
    switch (op * 32 + uim) {
#define CFU(k) case 0 * 32 + k: __asm__ __volatile__("vcfux %0,%1,%2" : "=v"(r->v) : "v"(b->v), "i"(k)); break;
#define CFS(k) case 1 * 32 + k: __asm__ __volatile__("vcfsx %0,%1,%2" : "=v"(r->v) : "v"(b->v), "i"(k)); break;
#define CTU(k) case 2 * 32 + k: __asm__ __volatile__("vctuxs %0,%1,%2" : "=v"(r->v) : "v"(b->v), "i"(k)); break;
#define CTS(k) case 3 * 32 + k: __asm__ __volatile__("vctsxs %0,%1,%2" : "=v"(r->v) : "v"(b->v), "i"(k)); break;
    U32X(CFU) U32X(CFS) U32X(CTU) U32X(CTS)
    }
}

/* op 0..3 : eq ge gt b ; dot : forme Rc, CR6 rendu */
static u32 cmpf(int op, int dot, const W *a, const W *b, W *r)
{
    u32 cr = 0;
    switch (op * 2 + dot) {
    case 0: __asm__ __volatile__("vcmpeqfp %0,%1,%2" : "=v"(r->v) : "v"(a->v), "v"(b->v)); break;
    case 1: __asm__ __volatile__("vcmpeqfp. %0,%2,%3\n\tmfcr %1" : "=v"(r->v), "=r"(cr) : "v"(a->v), "v"(b->v) : "cr6"); break;
    case 2: __asm__ __volatile__("vcmpgefp %0,%1,%2" : "=v"(r->v) : "v"(a->v), "v"(b->v)); break;
    case 3: __asm__ __volatile__("vcmpgefp. %0,%2,%3\n\tmfcr %1" : "=v"(r->v), "=r"(cr) : "v"(a->v), "v"(b->v) : "cr6"); break;
    case 4: __asm__ __volatile__("vcmpgtfp %0,%1,%2" : "=v"(r->v) : "v"(a->v), "v"(b->v)); break;
    case 5: __asm__ __volatile__("vcmpgtfp. %0,%2,%3\n\tmfcr %1" : "=v"(r->v), "=r"(cr) : "v"(a->v), "v"(b->v) : "cr6"); break;
    case 6: __asm__ __volatile__("vcmpbfp %0,%1,%2" : "=v"(r->v) : "v"(a->v), "v"(b->v)); break;
    default: __asm__ __volatile__("vcmpbfp. %0,%2,%3\n\tmfcr %1" : "=v"(r->v), "=r"(cr) : "v"(a->v), "v"(b->v) : "cr6"); break;
    }
    return (cr >> 4) & 0xf;
}

static void setvscr(u32 x)
{
    W m;
    m.u[0] = m.u[1] = m.u[2] = 0; m.u[3] = x;
    vec_mtvscr((vector unsigned short)m.v);
}
static u32 getvscr(void)
{
    W m;
    m.v = (vector unsigned int)vec_mfvscr();
    return m.u[3];
}

static const u32 ICAT[] = {
    0, 1, 2, 3, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff, 0xfffffffe,
    0x00ffffff, 0x01000000, 0x01000001, 0x01000003, 0xff000001, 0xfefffffd,
    0x7ffffffe, 0x7fffff80, 0x7fffffc0, 0xffffff7f, 0x80000080, 0x55555555,
    0xaaaaaaab, 0x00010000, 0xffff0000, 0x0000ffff, 0x12345678,
};
#define NICAT (sizeof ICAT / sizeof ICAT[0])
/* bords de la saturation : +-2^31, 2^32, -1, a uim pres */
static const u32 FCAT2[] = {
    0x4f000000, 0x4effffff, 0xcf000000, 0xcf000001, 0x4f800000, 0x4f7fffff,
    0xbf800000, 0xbf7fffff, 0xbf000000, 0x80000000, 0x3f7fffff, 0x3effffff,
    0x30000000, 0x2f800000, 0x307fffff, 0x30800000, 0xb0000000, 0xb0800000,
    0x5f000000, 0xdf000000, 0x7f7fffff, 0xff7fffff,
};
#define NFCAT2 (sizeof FCAT2 / sizeof FCAT2[0])

static u32 ilane(void)
{
    u32 r = rnd(), k = r % 8;
    if (k < 2) return ICAT[(r >> 8) % NICAT];
    if (k < 4) return rnd() >> ((r >> 8) % 32);
    if (k < 5) return (u32)-(int)(rnd() >> ((r >> 8) % 32));
    return rnd();
}
static u32 flane_c(void)
{
    u32 r = rnd(), k = r % 8;
    if (k < 1) return FCAT2[(r >> 8) % NFCAT2];
    if (k < 2) return CAT[(r >> 8) % NCAT];
    if (k < 3) return mkf(r >> 31, 126 + (r >> 8) % 34, rnd());   /* 0.5 .. 2^33 */
    if (k < 4) return mkf(r >> 31, 158 - (r >> 8) % 32, (rnd() & 0x7fffff) | (r & 1 ? 0x7fff00 : 0));
    return lane();
}

static void recc(const W *r, u32 x)
{
    acc((const V *)r);
    hh[0] = (hh[0] ^ x) * 16777619u;
}

static void mode_c(long nrand)
{
    char line[256];
    unsigned nj, x, y, i, op, uim, dot;
    long n;
    W a, b, r;

    for (nj = 0; nj < 2; nj++) {
        u32 vs = nj ? 0x00010000 : 0;
        /* comparaisons */
        for (op = 0; op < 4; op++) {
            memset(hh, 0, sizeof hh);
            setvscr(vs);
            for (x = 0; x < NCAT + NFCAT2; x++) {
                for (y = 0; y < NCAT + NFCAT2; y++) {
                    for (i = 1; i < 4; i++) {
                        a.u[i] = mkf(i & 1, 120 + i, 0x123456 * i);
                        b.u[i] = mkf(0, 118 + i * 2, 0x654321 * i);
                    }
                    a.u[0] = x < NCAT ? CAT[x] : FCAT2[x - NCAT];
                    b.u[0] = y < NCAT ? CAT[y] : FCAT2[y - NCAT];
                    for (dot = 0; dot < 2; dot++) {
                        u32 cr = cmpf(op, dot, &a, &b, &r);
                        recc(&r, cr);
                        /* les quatre voies egales : CR6 « toutes » */
                        b.u[1] = a.u[1]; b.u[2] = a.u[2]; b.u[3] = a.u[3];
                        cr = cmpf(op, dot, &a, &b, &r);
                        recc(&r, cr);
                    }
                }
            }
            for (n = 0; n < nrand; n++) {
                for (i = 0; i < 4; i++) {
                    a.u[i] = flane_c(); b.u[i] = flane_c();
                    if ((rnd() & 7) == 0) b.u[i] = a.u[i] ^ ((rnd() & 1) ? 0x80000000u : 0);
                }
                for (dot = 0; dot < 2; dot++) {
                    u32 cr = cmpf(op, dot, &a, &b, &r);
                    recc(&r, cr);
                }
            }
            snprintf(line, sizeof line, "NJ=%u %s : %ld, h=%08x%08x%08x%08x\n", nj,
                     (const char *[]){ "vcmpeqfp", "vcmpgefp", "vcmpgtfp", "vcmpbfp" }[op],
                     (long)((NCAT + NFCAT2) * (NCAT + NFCAT2) * 4 + nrand * 2),
                     hh[0], hh[1], hh[2], hh[3]);
            out(line);
        }
        /* conversions, chaque uim ; VSCR[SAT] remis a 0 avant chacune */
        for (op = 0; op < 4; op++) {
            memset(hh, 0, sizeof hh);
            for (uim = 0; uim < 32; uim++) {
                unsigned nc = op < 2 ? NICAT : NCAT + NFCAT2;
                for (x = 0; x < nc; x++) {
                    for (i = 0; i < 4; i++) {
                        b.u[i] = op < 2 ? ICAT[(x + i * 7) % NICAT]
                               : (x + i * 5) % nc < NCAT ? CAT[(x + i * 5) % nc]
                               : FCAT2[(x + i * 5) % nc - NCAT];
                    }
                    setvscr(vs);
                    cvt(op, uim, &b, &r);
                    recc(&r, getvscr());
                }
                for (n = 0; n < nrand / 32; n++) {
                    for (i = 0; i < 4; i++) {
                        b.u[i] = op < 2 ? ilane() : flane_c();
                    }
                    setvscr(vs);
                    cvt(op, uim, &b, &r);
                    recc(&r, getvscr());
                }
            }
            snprintf(line, sizeof line, "NJ=%u %s : 32 uim, h=%08x%08x%08x%08x\n", nj,
                     (const char *[]){ "vcfux", "vcfsx", "vctuxs", "vctsxs" }[op],
                     hh[0], hh[1], hh[2], hh[3]);
            out(line);
        }
    }
    snprintf(line, sizeof line, "empreinte %016llx\n", fnv);
    fputs(line, stdout);
}

/* bancs de mode c : n x 4 instructions de chaque sorte, dependantes */
static void banc_c(long banc)
{
    struct timeval t0, t1;
    W a, b, k, r;
    long n;
    int i;
    for (i = 0; i < 4; i++) {
        a.u[i] = mkf(0, 127, i * 0x1000); b.u[i] = mkf(0, 127, 0x400000);
        k.u[i] = 3 * i + 1;
    }
    setvscr(0);
#define BANC(nom, corps)                                                     \
    gettimeofday(&t0, 0);                                                    \
    for (n = 0; n < banc; n++) { corps }                                     \
    gettimeofday(&t1, 0);                                                    \
    printf("banc-c : %ld x 4 %s en %ld ms (%08x)\n", banc, nom,              \
           (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000), \
           r.u[0] ^ r.u[3]);
    BANC("vcmpgtfp + vsel",
         r.v = (vector unsigned int)vec_cmpgt(a.f, b.f); a.f = vec_sel(a.f, b.f, r.v);
         r.v = (vector unsigned int)vec_cmpgt(b.f, a.f); b.f = vec_sel(b.f, a.f, r.v);
         r.v = (vector unsigned int)vec_cmpge(a.f, b.f); a.f = vec_sel(b.f, a.f, r.v);
         r.v = (vector unsigned int)vec_cmpeq(b.f, a.f); b.f = vec_sel(a.f, b.f, r.v);)
    BANC("vcmpbfp.",
         if (vec_all_in(a.f, b.f)) k.u[0]++; a.f = vec_sld(a.f, a.f, 4);
         if (vec_any_gt(a.f, b.f)) k.u[1]++; b.f = vec_sld(b.f, b.f, 8);
         if (vec_all_in(b.f, a.f)) k.u[2]++; a.f = vec_sld(a.f, a.f, 12);
         if (vec_any_ge(b.f, a.f)) k.u[3]++; b.f = vec_sld(b.f, b.f, 4);
         r = k;)
    BANC("vcfsx + vctsxs",
         r.f = vec_ctf((vector signed int)k.v, 3); k.v = (vector unsigned int)vec_cts(r.f, 2);
         r.f = vec_ctf((vector signed int)k.v, 1); k.v = (vector unsigned int)vec_cts(r.f, 0);)
    BANC("vcfux + vctuxs",
         r.f = vec_ctf(k.v, 2); k.v = vec_ctu(r.f, 2);
         r.f = vec_ctf(k.v, 0); k.v = vec_ctu(r.f, 1);)
}

/*
 * ---- mode k (patches/tcg/0053, x-vfp-known, docs/tcg-g4.md §42) ----
 * Les modes ci-dessus n'executent qu'une operation flottante AltiVec par
 * bloc de traduction, operandes venus de lvx. Celui-ci ecrit dans un tampon
 * executable des suites aleatoires de 8 a 48 instructions (un bloc, sauf
 * b .+4 ou page franchie) sur v1-v8 : vaddfp vsubfp vmaddfp vnmsubfp
 * (producteurs et consommateurs de x-vfp-known) ; vmrghw vmrglw vspltw
 * vsldoi vor vxor vspltis[bhw] (propagateurs, ou non selon leurs
 * operandes) ; vperm vmaxfp vminfp vrefp vrsqrtefp vrfi[nzpm] (helpers de
 * la liste blanche : vD seul perdu) ; vcmp*fp(.), vcf*, vct* (autres sortes
 * de l'op) ; lvx, stvx, vadduwm vand vslw (entiers), mtvscr (NJ), mfvscr,
 * fadds/fmuls (flottant scalaire), b .+4. Entrees : normales a exposants
 * larges (debordements et sous-debordements en chaine), zeros, voisines de
 * FLT_MIN, et le catalogue (denormaux, infinis, NaN) ; v9/v10 controles de
 * vperm (mots entiers ou octets quelconques), v11 une valeur de VSCR.
 * Sortie : v1-v8, VSCR, CR6 et la zone des stvx hachés.
 *   vfptest k [NPROG [GRAINE]]   phases NJ=0, NJ=1, NJ au hasard
 *   vfptest kx [NPROG [GRAINE]]  valeurs exactes (porte fermee : inexact
 *                                jamais pose par l'arithmetique), a lancer
 *                                dans un processus neuf
 *   vfptest kp [N [GRAINE]]      bloc fixe : propagation, voies exemptees (plus bas)
 */
#include <sys/mman.h>
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
static u32 *kbuf;
static unsigned kn;
static int kexact;
static void kput(u32 w) { kbuf[kn++] = w; }
#define KVA(d, a, b, c, xo) ((u32)4 << 26 | (u32)(d) << 21 | (u32)(a) << 16 | (u32)(b) << 11 | (u32)(c) << 6 | (xo))
#define KVX(d, a, b, xo) ((u32)4 << 26 | (u32)(d) << 21 | (u32)(a) << 16 | (u32)(b) << 11 | (xo))
#define KVR(d, a, b, rc, xo) ((u32)4 << 26 | (u32)(d) << 21 | (u32)(a) << 16 | (u32)(b) << 11 | (u32)(rc) << 10 | (xo))
#define KX31(s, a, b, xo) ((u32)31 << 26 | (u32)(s) << 21 | (u32)(a) << 16 | (u32)(b) << 11 | (u32)(xo) << 1)
#define KLI(r, v) ((u32)14 << 26 | (u32)(r) << 21 | ((v) & 0xffff))
/*
 * VRSAVE (SPR 256) : le noyau de Mac OS X ne sauve et ne rend, a la preemption, que les VR
 * qu'il marque ; le code genere s'en sert sans que gcc le sache : tout marque le temps du
 * bloc (r8 garde l'ancienne valeur). Sans cela une preemption au milieu du programme
 * perd des VR et l'empreinte varie d'une execution a l'autre (vu le 24/10 dans Tiger).
 */
#define KMFVRSAVE(r) ((u32)31 << 26 | (u32)(r) << 21 | 8u << 11 | 339u << 1)
#define KMTVRSAVE(r) ((u32)31 << 26 | (u32)(r) << 21 | 8u << 11 | 467u << 1)
static void kvrsave_on(void)
{
    kput(KMFVRSAVE(8));
    kput(KLI(9, -1));
    kput(KMTVRSAVE(9));
}

static unsigned kreg(void) { return 1 + rnd() % 8; }

static void kinsn(void)
{
    u32 x = rnd(), w = x % 1000, d = kreg(), a = kreg(), b = kreg(), c = kreg();
    static const u32 xfp[4] = { 10, 74, 46, 47 };       /* vaddfp vsubfp vmaddfp vnmsubfp */
    static const u32 xsp[3] = { 780, 844, 908 };        /* vspltisb vspltish vspltisw */
    static const u32 xcmp[4] = { 198, 454, 710, 966 };  /* vcmpeqfp vcmpgefp vcmpgtfp vcmpbfp */
    static const u32 xcv[4] = { 778, 842, 906, 970 };   /* vcfux vcfsx vctuxs vctsxs */

    if (kexact && w >= 640 && w < 900) {
        w = x % 640;            /* kx : rien qui pose inexact dans vec_status */
    }
    if (w < 420) {
        u32 k = (x >> 10) & 3;
        if ((x >> 12) % 4 == 0) {
            a = b;              /* operandes egaux */
        }
        kput(k < 2 ? KVX(d, a, b, xfp[k]) : KVA(d, a, b, c, xfp[k]));
    } else if (w < 600) {
        u32 k = (x >> 10) % 9;
        switch (k) {
        case 0: kput(KVX(d, a, b, 140)); break;                 /* vmrghw */
        case 1: kput(KVX(d, a, b, 396)); break;                 /* vmrglw */
        case 2: kput(KVX(d, (x >> 14) & 3, b, 652)); break;     /* vspltw */
        case 3: kput(KVA(d, a, b, (x >> 14) & 1 ? (x >> 15) & 15 : (x >> 15) & 12, 44)); break; /* vsldoi */
        case 4: kput(KVX(d, a, a, 1156)); break;                /* vor = vmr */
        case 5: kput(KVX(d, a, b, 1156)); break;                /* vor */
        case 6: kput(KVX(d, a, (x >> 14) & 1 ? a : b, 1220)); break; /* vxor */
        case 7: kput(KVX(d, (x >> 14) & 1 ? 0 : (x >> 16) & 31, 0,
                         xsp[(x >> 22) % 3])); break; /* vspltis* */
        default: kput(KVX(d, a, b, 1220)); break;
        }
    } else if (w < 640) {
        kput(KVA(d, a, b, (x >> 10) % 3 == 0 ? c : 9 + ((x >> 12) & 1), 43)); /* vperm */
    } else if (w < 700) {
        static const u32 xh[8] = { 1034, 1098, 266, 330, 522, 586, 650, 714 };
        u32 k = (x >> 10) & 7;  /* vmaxfp vminfp vrefp vrsqrtefp vrfi[nzpm] */
        kput(KVX(d, k < 2 ? a : 0, b, xh[k]));
    } else if (w < 780) {
        u32 k = (x >> 10) & 7;
        if (k < 4) {
            kput(KVR(d, a, b, (x >> 13) & 1, xcmp[k]));
        } else {
            kput(KVX(d, (x >> 13) & 31, b, xcv[k - 4]));
        }
    } else if (w < 850) {
        if ((x >> 10) & 1) {
            kput(KLI(6, 16 * (11 + (x >> 11) % 5)));            /* lvx d, in[11..15] */
            kput(KX31(d, 3, 6, 103));
        } else {
            kput(KLI(6, 160 + 16 * ((x >> 11) % 16)));          /* stvx a, out[10..25] */
            kput(KX31(a, 4, 6, 231));
        }
    } else if (w < 900) {
        static const u32 xi[4] = { 128, 1028, 388, 1152 };     /* vadduwm vand vslw vsubuwm */
        kput(KVX(d, a, b, xi[(x >> 10) & 3]));
    } else if (w < 930) {
        if ((x >> 10) & 1) {
            kput(KVX(0, 0, 11, 1604));                          /* mtvscr v11 */
        } else {
            kput(KVX(d, 0, 0, 1540));                           /* mfvscr d */
        }
    } else if (w < 970) {
        kput((x >> 10) & 1 ? (u32)59 << 26 | 1 << 21 | 1 << 16 | 2 << 11 | 21 << 1
                           : (u32)59 << 26 | 3 << 21 | 1 << 16 | 2 << 6 | 25 << 1);
    } else if (w < 978) {
        kput(0x48000004);                                       /* b .+4 */
    } else {
        kput((u32)14 << 26 | 7 << 21 | 7 << 16 | (x >> 16)); /* addi r7,r7 : une globale sale avant une sortie de bloc */
    }
}

static void kgen(unsigned len)
{
    unsigned i;
    kn = 0;
    kvrsave_on();
    kput(KLI(7, 0));                                            /* li r7, 0 */
    for (i = 1; i <= 11; i++) {
        kput(KLI(6, 16 * (i - 1)));
        kput(KX31(i, 3, 6, 103));                               /* lvx vi, in[i - 1] */
    }
    for (i = 0; i < len; i++) {
        kinsn();
    }
    for (i = 1; i <= 8; i++) {
        kput(KLI(6, 16 * (i - 1)));
        kput(KX31(i, 4, 6, 231));                               /* stvx vi, out[i - 1] */
    }
    kput(KVX(0, 0, 0, 1540));                                   /* mfvscr v0 */
    kput(KLI(6, 128));
    kput(KX31(0, 4, 6, 231));
    kput(KX31(5, 0, 0, 19));                                    /* mfcr r5 */
    kput((u32)36 << 26 | 5 << 21 | 4 << 16 | 144);              /* stw r5, 144(r4) */
    kput((u32)36 << 26 | 7 << 21 | 4 << 16 | 148);              /* stw r7, 148(r4) */
    kput(KMTVRSAVE(8));
    kput(0x4e800020);
    for (i = 0; i < kn; i += 8) {
        __asm__ __volatile__("dcbst 0,%0" : : "r"(kbuf + i) : "memory");
    }
    __asm__ __volatile__("sync" : : : "memory");
    for (i = 0; i < kn; i += 8) {
        __asm__ __volatile__("icbi 0,%0" : : "r"(kbuf + i) : "memory");
    }
    __asm__ __volatile__("sync\n\tisync" : : : "memory");
}

static u32 klane(void)
{
    u32 r = rnd(), k = r % 100;
    if (kexact) {
        static const u32 ex[8] = { 0x00000000, 0x80000000, 0x3f800000, 0xbf800000,
                                   0x40000000, 0x3f000000, 0xc0400000, 0x3fc00000 };
        return ex[(r >> 8) & 7];
    }
    if (k < 40) return mkf(r >> 31, 0x7f - 64 + (rnd() % 129), rnd());
    if (k < 52) return mkf(r >> 31, 1 + (r >> 8) % 3, rnd());   /* voisines de FLT_MIN */
    if (k < 64) return (r >> 31) << 31;                         /* zeros : voies exemptees */
    if (k < 74) return mkf(r >> 31, 200 + (r >> 8) % 55, rnd()); /* grands : debordements */
    if (k < 80) return mkf(r >> 31, 0, rnd() | 1);              /* denormaux */
    if (k < 88) return CAT[(r >> 8) % NCAT];
    if (k < 92) return lane();
    return mkf(r >> 31, 1 + (r >> 8) % 254, rnd());
}

static void kmode(long nprog, unsigned seed)
{
    static W kin[16], kout[26];
    void (*fn)(W *, W *);
    long p;
    int ph, v, i;
    unsigned long n = 0;
    u64 phash;
    char line[256];

    kbuf = mmap(0, 65536, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (kbuf == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    fn = (void (*)(W *, W *))(void *)kbuf;
    for (ph = 0; ph < 3; ph++) {
        phash = 1469598103934665603ULL;
        st = 0x9e3779b97f4a7c15ULL + ph * 977 + seed * 0x100000001b3ULL;
        for (p = 0; p < nprog; p++) {
            kgen(8 + rnd() % 41);
            for (v = 0; v < 8; v++) {
                u32 nj = ph == 2 ? rnd() & 1 : ph;
                for (i = 0; i < 8; i++) {
                    kin[i].u[0] = klane(); kin[i].u[1] = klane();
                    kin[i].u[2] = klane(); kin[i].u[3] = klane();
                }
                for (i = 11; i < 16; i++) {
                    kin[i].u[0] = klane(); kin[i].u[1] = klane();
                    kin[i].u[2] = klane(); kin[i].u[3] = klane();
                }
                for (i = 0; i < 4; i++) {
                    u32 s = rnd();      /* v9 : mots entiers de a ou b ; v10 : octets */
                    u32 wd = (s & 7) * 4;
                    kin[8].u[i] = (wd << 24 | (wd + 1) << 16 | (wd + 2) << 8 | (wd + 3));
                    kin[9].u[i] = rnd();
                }
                kin[10].u[0] = kin[10].u[1] = kin[10].u[2] = 0;
                kin[10].u[3] = rnd() & 1 ? 0x00010000 : 0;
                memset(kout, 0x5a, sizeof(kout));
                setvscr(nj ? 0x00010000 : 0);
                fn(kin, kout);
                kout[9].u[0] &= 0x000000f0;     /* CR6 */
                kout[9].u[2] = kout[9].u[3] = 0;        /* u[1] : r7 */
                h(kout, sizeof(kout));
                for (i = 0; i < (int)sizeof(kout); i++) {
                    phash ^= ((unsigned char *)kout)[i];
                    phash *= 1099511628211ULL;
                }
                n++;
            }
        }
        snprintf(line, sizeof line, "K%d %8lu executions  empreinte %016llx\n", ph, n, phash);
        fputs(line, stdout);
    }
    printf("total %lu programmes ; empreinte %016llx\n", n, fnv);
}

/*
 * ---- mode kp (patches/tcg/0053) : propagation et voies exemptees, bloc fixe ----
 * Deux producteurs (vmaddfp) dont le second deborde souvent (voies infinies, ou NaN
 * d'entree), fusionnes (vmrghw, vmrglw, vsldoi 8, vspltw) puis consommes par des vmaddfp
 * dont l'autre facteur a des voies nulles : un produit nul exempte la voie du test de
 * resultat, si bien qu'un infini ou un NaN pris a tort pour connu (producteur oublie)
 * donnerait le NaN de l'hote au lieu de celui de softfloat. NJ 0 puis 1.
 *   vfptest kp [N [GRAINE]]
 */
static void kpmode(long n, unsigned seed)
{
    static W kin[16], kout[26];
    void (*fn)(W *, W *);
    long p;
    int i, j, nj;
    unsigned uim, cons;
    u64 phash;
    char line[256];

    kbuf = mmap(0, 65536, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (kbuf == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    fn = (void (*)(W *, W *))(void *)kbuf;
    for (nj = 0; nj < 2; nj++) {
        phash = 1469598103934665603ULL;
        st = 0x51ed270b27e9ULL + nj * 131 + seed * 0x100000001b3ULL;
        for (p = 0; p < n; p++) {
            if (p % 64 == 0) {                  /* un bloc par 64 jeux d'entrees */
                uim = rnd() & 3;
                kn = 0;
                kvrsave_on();
                for (i = 1; i <= 7; i++) {
                    kput(KLI(6, 16 * (i - 1)));
                    kput(KX31(i == 7 ? 18 : i, 3, 6, 103));     /* v1-v6, v18 */
                }
                /* v13-v17 a zero : un consommateur non tire ne rangerait sinon que le
                   reste du code C (non deterministe dans Tiger, hors de VRSAVE) */
                for (i = 13; i <= 17; i++) {
                    kput(KVX(i, i, i, 1220));                   /* vxor vi,vi,vi */
                }
                kput(KVA(7, 1, 3, 2, 46));                      /* vmaddfp v7,v1,v2,v3 */
                kput(KVA(8, 4, 6, 5, 46));                      /* vmaddfp v8,v4,v5,v6 */
                kput(KVX(9, 7, 8, 140));                        /* vmrghw v9,v7,v8 */
                kput(KVX(10, 8, 7, 396));                       /* vmrglw v10,v8,v7 */
                kput(KVA(11, 7, 8, 8, 44));                     /* vsldoi v11,v7,v8,8 */
                kput(KVX(12, uim, 8, 652));                     /* vspltw v12,v8,uim */
                /* consommateurs tires au hasard : un bloc sur quatre ne lit v8 qu'en
                   seconde source (v9, v11), ou son producteur doit encore etre oblige */
                cons = rnd() & 3 ? rnd() | 1 : 5;
                kput(KVA(13, 18, 3, 9, 46));                    /* vmaddfp v13,v18,v9,v3 */
                if (cons & 2) kput(KVA(14, 10, 6, 18, 46));     /* vmaddfp v14,v10,v18,v6 */
                if (cons & 4) kput(KVA(15, 18, 6, 11, 47));     /* vnmsubfp v15,v18,v11,v6 */
                if (cons & 8) kput(KVA(16, 12, 3, 18, 46));     /* vmaddfp v16,v12,v18,v3 */
                if (cons & 16) kput(KVX(17, 9, 10, 10));        /* vaddfp v17,v9,v10 */
                for (i = 7; i <= 17; i++) {
                    kput(KLI(6, 16 * (i - 7)));
                    kput(KX31(i, 4, 6, 231));
                }
                kput(KVX(0, 0, 0, 1540));
                kput(KLI(6, 176));
                kput(KX31(0, 4, 6, 231));
                kput(KMTVRSAVE(8));
                kput(0x4e800020);
                for (i = 0; i < (int)kn; i += 8) {
                    __asm__ __volatile__("dcbst 0,%0" : : "r"(kbuf + i) : "memory");
                }
                __asm__ __volatile__("sync" : : : "memory");
                for (i = 0; i < (int)kn; i += 8) {
                    __asm__ __volatile__("icbi 0,%0" : : "r"(kbuf + i) : "memory");
                }
                __asm__ __volatile__("sync\n\tisync" : : : "memory");
            }
            for (j = 0; j < 4; j++) {
                u32 r = rnd();
                kin[0].u[j] = mkf(r & 1, 100 + rnd() % 50, rnd());          /* v1 */
                kin[1].u[j] = mkf(r >> 1 & 1, 100 + rnd() % 50, rnd());     /* v2 */
                kin[2].u[j] = mkf(r >> 2 & 1, 100 + rnd() % 50, rnd());     /* v3 */
                kin[3].u[j] = mkf(r >> 3 & 1, (r >> 8) & 1 ? 230 + rnd() % 25 : 120 + rnd() % 20, rnd());
                kin[4].u[j] = mkf(r >> 4 & 1, (r >> 9) & 1 ? 230 + rnd() % 25 : 120 + rnd() % 20, rnd());
                kin[5].u[j] = (r >> 10) % 8 == 0 ? CAT[(r >> 16) % NCAT] : mkf(r >> 5 & 1, 120 + rnd() % 20, rnd());
                kin[6].u[j] = (r >> 13) & 1 ? (r >> 14 & 1) << 31 : mkf(r >> 6 & 1, 120 + rnd() % 20, rnd());
                if ((r >> 20) % 16 == 0) {
                    kin[3].u[j] = CAT[(r >> 24) % NCAT];        /* inf, NaN, denormaux */
                }
            }
            memset(kout, 0x5a, sizeof(kout));
            setvscr(nj ? 0x00010000 : 0);
            fn(kin, kout);
            h(kout, sizeof(kout));
            for (i = 0; i < (int)sizeof(kout); i++) {
                phash ^= ((unsigned char *)kout)[i];
                phash *= 1099511628211ULL;
            }
        }
        snprintf(line, sizeof line, "P%d %8ld executions  empreinte %016llx\n", nj, n, phash);
        fputs(line, stdout);
    }
    printf("total ; empreinte %016llx\n", fnv);
}

int main(int argc, char **argv)
{
    char line[256];
    long nrand = getenv("NVEC") ? atol(getenv("NVEC")) : (1L << 22), n;
    long banc = argc > 1 ? atol(argv[1]) : 0;
    unsigned nj, x, y, z, i;
    V a, b, c;
    struct timeval t0, t1;

    setvbuf(stdout, 0, _IONBF, 0);
    if (argc > 1 && (!strcmp(argv[1], "k") || !strcmp(argv[1], "kx"))) {
        kexact = argv[1][1] == 'x';
        kmode(argc > 2 ? atol(argv[2]) : 4096, argc > 3 ? atoi(argv[3]) : 0);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "kp")) {
        kpmode(argc > 2 ? atol(argv[2]) : 65536, argc > 3 ? atoi(argv[3]) : 0);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "c")) {
        banc = argc > 2 ? atol(argv[2]) : 0;
        if (banc > 0) {
            banc_c(banc);
        } else {
            mode_c(nrand);
        }
        return 0;
    }
    for (nj = 0; nj < 2; nj++) {
        vector unsigned short vs = (vector unsigned short)vec_splat_u32(0);
        V m;
        m.u[0] = m.u[1] = m.u[2] = 0; m.u[3] = nj ? 0x00010000 : 0;
        vs = (vector unsigned short)m.v;
        vec_mtvscr(vs);
        memset(hh, 0, sizeof hh);
        for (x = 0; x < NCAT; x++) {
            for (y = 0; y < NCAT; y++) {
                for (z = 0; z < NCAT; z++) {
                    for (i = 1; i < 4; i++) {
                        a.u[i] = mkf(0, 120 + i, 0x123456 * i);
                        b.u[i] = mkf(1, 118 + i, 0x654321 * i);
                        c.u[i] = mkf(0, 126, 0x0f0f0f * i);
                    }
                    a.u[0] = CAT[x]; b.u[0] = CAT[y]; c.u[0] = CAT[z];
                    run4(&a, &b, &c);
                }
            }
        }
        snprintf(line, sizeof line, "NJ=%u catalogue : %u triplets, h=%08x%08x%08x%08x\n",
                 nj, (unsigned)(NCAT * NCAT * NCAT), hh[0], hh[1], hh[2], hh[3]);
        out(line);
        memset(hh, 0, sizeof hh);
        gettimeofday(&t0, 0);
        for (n = 0; n < nrand; n++) {
            for (i = 0; i < 4; i++) {
                a.u[i] = lane(); b.u[i] = lane(); c.u[i] = lane();
                if ((rnd() & 7) == 0) {
                    float p = *(float *)&a.u[i] * *(float *)&c.u[i];
                    p = -p;
                    b.u[i] = *(u32 *)&p;
                } else if ((rnd() & 7) == 0) {
                    b.u[i] = a.u[i] ^ ((rnd() & 1) ? 0x80000000u : 1u);
                }
            }
            run4(&a, &b, &c);
        }
        gettimeofday(&t1, 0);
        snprintf(line, sizeof line, "NJ=%u aleatoire : %ld vecteurs x 4 instructions, h=%08x%08x%08x%08x\n",
                 nj, nrand, hh[0], hh[1], hh[2], hh[3]);
        out(line);
        fprintf(stderr, "(NJ=%u : %ld s)\n", nj, (long)(t1.tv_sec - t0.tv_sec));
    }
    /* vperm (patches/tcg/0004, x-vperm-fast) : chaque octet de controle a
     * chaque position, puis des vecteurs aleatoires */
    {
        typedef union { vector unsigned char v; u32 u[4]; unsigned char b[16]; } B;
        B pa, pb, pc, pr;
        unsigned pos, val, k;
        memset(hh, 0, sizeof hh);
        for (pos = 0; pos < 16; pos++) {
            for (val = 0; val < 256; val++) {
                for (k = 0; k < 4; k++) {
                    for (i = 0; i < 4; i++) {
                        pa.u[i] = rnd(); pb.u[i] = rnd(); pc.u[i] = rnd();
                    }
                    pc.b[pos] = val;
                    pr.v = vec_perm(pa.v, pb.v, pc.v);
                    acc((V *)&pr);
                }
            }
        }
        for (n = 0; n < nrand; n++) {
            for (i = 0; i < 4; i++) {
                pa.u[i] = rnd(); pb.u[i] = rnd(); pc.u[i] = rnd();
            }
            pr.v = vec_perm(pa.v, pb.v, pc.v);
            acc((V *)&pr);
            pa.v = vec_perm(pa.v, pb.v, pa.v);     /* r = a = c */
            acc((V *)&pa);
        }
        snprintf(line, sizeof line, "vperm : %u + %ld x 2 vecteurs, h=%08x%08x%08x%08x\n",
                 16 * 256 * 4, nrand, hh[0], hh[1], hh[2], hh[3]);
        out(line);
    }
    snprintf(line, sizeof line, "empreinte %016llx\n", fnv);
    fputs(line, stdout);
    if (banc > 0) {
        vector unsigned char p0 = vec_lvsl(3, (unsigned char *)0);
        vector unsigned char x0 = (vector unsigned char)vec_splat_u8(7);
        vector unsigned char x1 = (vector unsigned char)vec_splat_u8(9);
        typedef union { vector unsigned char v; u32 u[4]; } B;
        B rr;
        gettimeofday(&t0, 0);
        for (n = 0; n < banc; n++) {
            x0 = vec_perm(x0, x1, p0);
            x1 = vec_perm(x1, x0, p0);
            x0 = vec_perm(x0, x1, p0);
            x1 = vec_perm(x1, x0, p0);
        }
        gettimeofday(&t1, 0);
        rr.v = vec_xor(x0, x1);
        printf("banc : %ld x 4 vperm en %ld ms (%08x)\n", banc,
               (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000),
               rr.u[0]);
    }
    if (banc > 0) {
        vector float va = (vector float){ 0.5f, 0.25f, 0.75f, 0.125f };
        vector float vb = (vector float){ 1.0f, 2.0f, 3.0f, 4.0f };
        vector float vc = (vector float){ 0.5f, 0.75f, 0.25f, 0.625f };
        vector float acc1 = va, acc2 = vb;
        V r;
        vec_mtvscr((vector unsigned short)vec_splat_u32(0));
        gettimeofday(&t0, 0);
        for (n = 0; n < banc; n++) {
            acc1 = vec_madd(acc1, vc, vb);
            acc2 = vec_add(acc2, va);
            acc1 = vec_madd(acc1, va, vc);
            acc2 = vec_sub(acc2, vc);
        }
        gettimeofday(&t1, 0);
        r.v = vec_add(acc1, acc2);
        printf("banc : %ld x (2 vmaddfp + vaddfp + vsubfp) en %ld ms (%08x)\n", banc,
               (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000),
               r.u[0]);
    }
    return 0;
}
