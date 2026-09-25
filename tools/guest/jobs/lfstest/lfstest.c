/*
 * lfstest.c - equivalence de lfs/stfs entre les helpers de QEMU (todouble /
 * tosingle) et la traduction en ligne (propriete x-lfs-inline,
 * patches/tcg/0002-ppc-lfs-inline.patch, docs/tcg-g4.md section 8).
 *
 * Execute les VRAIES instructions dans l'invite Tiger :
 *   1. lfs puis stfd, pour les 2^32 motifs de float32 (TOUS, en deux
 *      processus) : le double obtenu est compare a une reference entiere
 *      (le code du helper recopie, compile par le GCC de l'invite) et haché ;
 *   2. lfd puis stfs, pour 2^32 float64 : chaque mot haut (signe, exposant,
 *      20 bits hauts de fraction) avec un mot bas derive du mot haut ; puis
 *      tous les exposants x signe x fractions 1<<b, (1<<b)-1, ~0>>b ;
 *   3. les formes indexees et a mise a jour (lfsx lfsu lfsux stfsx stfsu
 *      stfsux) sur 2^22 motifs chacune, adresse de mise a jour relue ;
 *   4. fautes : lfs depuis une page PROT_NONE, stfs vers une page PROT_READ
 *      (le signal, et la memoire qui ne doit pas changer) ;
 *   5. un banc (argument ou BANC=N) : N paires lfs/stfs, temps en ms.
 * Les parties 1 a 4 doivent donner 0 divergence avec la reference ET une
 * sortie IDENTIQUE octet pour octet entre x-lfs-inline=off et =on (empreinte
 * FNV en derniere ligne).  LFSBITS=n (defaut 32) reduit les parties 1 et 2 a
 * 2^n motifs repartis sur tout l'espace (pas de 2^(32-n)).
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

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

/* ---- reference : helper_todouble / helper_tosingle de QEMU 9.2 ---- */
static u64 ref_todouble(u32 arg)
{
    u32 abs_arg = arg & 0x7fffffff;
    u64 ret;
    if (abs_arg >= 0x00800000) {
        if (((arg >> 23) & 0xff) == 0xff) {
            ret  = (u64)(arg >> 31) << 63;
            ret |= (u64)0x7ff << 52;
            ret |= (u64)(arg & 0x7fffff) << 29;
        } else {
            ret  = (u64)(arg >> 30) << 62;
            ret |= ((((arg >> 30) & 1) ^ 1) * (u64)7) << 59;
            ret |= (u64)(arg & 0x3fffffff) << 29;
        }
    } else {
        ret = (u64)(arg >> 31) << 63;
        if (abs_arg != 0) {
            int shift = __builtin_clz(abs_arg) - 8;
            int exp = -126 - shift + 1023 - 1;
            ret |= (u64)exp << 52;
            ret += (u64)abs_arg << (52 - 23 + shift);
        }
    }
    return ret;
}
static u32 ref_tosingle(u64 arg)
{
    int exp = (int)((arg >> 52) & 0x7ff);
    u32 ret;
    if (exp > 896) {
        ret  = (u32)((arg >> 62) & 3) << 30;
        ret |= (u32)((arg >> 29) & 0x3fffffff);
    } else {
        ret = (u32)(arg >> 63) << 31;
        if (exp >= 874) {
            ret |= (u32)(((1ULL << 52) | (arg & ((1ULL << 52) - 1))) >> (896 + 30 - exp));
        }
    }
    return ret;
}

/* ---- les vraies instructions ---- */
static u32 B[8] __attribute__((aligned(16)));

static u64 hw_lfs(u32 v)
{
    __asm__ volatile("stw %1,0(%0)\n\tlfs f0,0(%0)\n\tstfd f0,8(%0)"
                     : : "b"(B), "r"(v) : "fr0", "memory");
    return ((u64)B[2] << 32) | B[3];
}
static u32 hw_stfs(u64 x)
{
    __asm__ volatile("stw %1,0(%0)\n\tstw %2,4(%0)\n\tlfd f0,0(%0)\n\tstfs f0,8(%0)"
                     : : "b"(B), "r"((u32)(x >> 32)), "r"((u32)x) : "fr0", "memory");
    return B[2];
}

static u32 mix(u32 x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

typedef struct { u64 n, bad; u32 h1, h2; } Res;

static void acc(Res *r, u32 a, u32 b)
{
    r->h1 = (r->h1 ^ a) * 16777619u;
    r->h2 = (r->h2 ^ b) * 16777619u;
}

/* part = 0..1 : moitie de l'espace ; step = 2^(32-bits) */
static void sweep_lfs(Res *r, int part, u32 step)
{
    u64 v, lo = part ? 0x80000000ULL : 0, hi = lo + 0x80000000ULL;
    for (v = lo; v < hi; v += step) {
        u64 d = hw_lfs((u32)v), e = ref_todouble((u32)v);
        r->n++;
        if (d != e) {
            if (r->bad++ < 4) {
                fprintf(stderr, "lfs %08x : %016llx attendu %016llx\n", (u32)v, d, e);
            }
        }
        acc(r, (u32)(d >> 32), (u32)d);
    }
}
static void sweep_stfs(Res *r, int part, u32 step)
{
    u64 v, lo = part ? 0x80000000ULL : 0, hi = lo + 0x80000000ULL;
    for (v = lo; v < hi; v += step) {
        u64 x = (v << 32) | mix((u32)v);
        u32 s = hw_stfs(x), e = ref_tosingle(x);
        r->n++;
        if (s != e) {
            if (r->bad++ < 4) {
                fprintf(stderr, "stfs %016llx : %08x attendu %08x\n", x, s, e);
            }
        }
        acc(r, s, (u32)v);
    }
}

/* deux processus, un par moitie ; resultats par un tube */
static void par(void (*fn)(Res *, int, u32), u32 step, Res *tot)
{
    int fd[2], i;
    Res r[2];
    pid_t pid;
    pipe(fd);
    pid = fork();
    if (pid == 0) {
        Res c;
        memset(&c, 0, sizeof c);
        c.h1 = c.h2 = 2166136261u;
        fn(&c, 1, step);
        write(fd[1], &c, sizeof c);
        _exit(0);
    }
    memset(&r[0], 0, sizeof r[0]);
    r[0].h1 = r[0].h2 = 2166136261u;
    fn(&r[0], 0, step);
    read(fd[0], &r[1], sizeof r[1]);
    waitpid(pid, 0, 0);
    close(fd[0]); close(fd[1]);
    memset(tot, 0, sizeof *tot);
    for (i = 0; i < 2; i++) {
        tot->n += r[i].n; tot->bad += r[i].bad;
    }
    tot->h1 = r[0].h1 ^ (r[1].h1 * 31); tot->h2 = r[0].h2 ^ (r[1].h2 * 31);
}

/* ---- formes indexees et a mise a jour ---- */
static u32 M[64] __attribute__((aligned(16)));

static void forms(char *line, size_t sz, u32 n)
{
    u32 i, bad = 0, hh = 2166136261u;
    for (i = 0; i < n; i++) {
        u32 v = mix(i * 0x9e3779b9u);
        u64 x = ((u64)mix(i) << 32) | mix(~i);
        u32 *p, *q;
        u64 d;
        M[0] = v; M[1] = v ^ 0x80000000u; M[2] = v ^ 0x00400000u;
        /* lfsx : EA = rA + rB */
        __asm__ volatile("lfsx f0,%0,%1\n\tstfd f0,32(%0)"
                         : : "b"(M), "r"(0) : "fr0", "memory");
        d = ((u64)M[8] << 32) | M[9];
        bad += d != ref_todouble(v); hh = (hh ^ M[8] ^ M[9]) * 16777619u;
        /* lfsu : rA += 4, EA = rA */
        p = M;
        __asm__ volatile("lfsu f0,4(%0)\n\tstfd f0,40(%1)"
                         : "+b"(p) : "b"(M) : "fr0", "memory");
        d = ((u64)M[10] << 32) | M[11];
        bad += d != ref_todouble(v ^ 0x80000000u) || p != M + 1;
        hh = (hh ^ M[10] ^ M[11] ^ (u32)(p - M)) * 16777619u;
        /* lfsux : rA += rB */
        p = M;
        __asm__ volatile("lfsux f0,%0,%2\n\tstfd f0,48(%1)"
                         : "+b"(p) : "b"(M), "r"(8) : "fr0", "memory");
        d = ((u64)M[12] << 32) | M[13];
        bad += d != ref_todouble(v ^ 0x00400000u) || p != M + 2;
        hh = (hh ^ M[12] ^ M[13] ^ (u32)(p - M)) * 16777619u;
        /* stfsx / stfsu / stfsux d'un double quelconque */
        M[4] = (u32)(x >> 32); M[5] = (u32)x;
        q = M + 16;
        __asm__ volatile("lfd f0,16(%1)\n\tstfsx f0,%1,%2\n\tstfsu f0,4(%0)\n\t"
                         "stfsux f0,%0,%3"
                         : "+b"(q) : "b"(M), "r"(96), "r"(8) : "fr0", "memory");
        bad += M[24] != ref_tosingle(x) || M[17] != ref_tosingle(x)
               || M[19] != ref_tosingle(x) || q != M + 19;
        hh = (hh ^ M[24] ^ M[17] ^ M[19] ^ (u32)(q - M)) * 16777619u;
    }
    snprintf(line, sz, "formes x/u/ux : %u motifs x 6 instructions, %u divergences, h=%08x\n",
             n, bad, hh);
}

static sigjmp_buf jb;
static void onfault(int s) { siglongjmp(jb, s); }

int main(int argc, char **argv)
{
    char line[256];
    Res r;
    int bits = getenv("LFSBITS") ? atoi(getenv("LFSBITS")) : 32;
    u32 step = bits >= 32 ? 1 : 1u << (32 - bits);
    u64 ec = 0, eb = 0, s, e;
    int b, i, sig;
    u32 eh = 2166136261u;
    long P = getpagesize();
    unsigned char *pg;
    struct timeval t0, t1;

    setvbuf(stdout, 0, _IONBF, 0);
    /* 0. quelques valeurs en clair */
    {
        static const u32 V[] = {
            0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007fffff, 0x00400000,
            0x00800000, 0x3f800000, 0xbf800000, 0x7f7fffff, 0x7f800000, 0xff800000,
            0x7fc00000, 0x7f800001, 0xffbfffff, 0x3eaaaaab,
        };
        for (i = 0; i < (int)(sizeof V / sizeof V[0]); i++) {
            u64 d = hw_lfs(V[i]);
            snprintf(line, sizeof line, "lfs  %08x -> %016llx  stfs -> %08x\n", V[i], d,
                     hw_stfs(d));
            out(line);
        }
    }
    /* 1. lfs sur 2^bits motifs */
    gettimeofday(&t0, 0);
    par(sweep_lfs, step, &r);
    gettimeofday(&t1, 0);
    snprintf(line, sizeof line, "lfs  : %llu motifs float32, %llu divergences, h=%08x%08x\n",
             r.n, r.bad, r.h1, r.h2);
    out(line);
    fprintf(stderr, "(lfs : %ld s)\n", (long)(t1.tv_sec - t0.tv_sec));
    /* 2. stfs sur 2^bits mots hauts, puis les cas limites */
    gettimeofday(&t0, 0);
    par(sweep_stfs, step, &r);
    gettimeofday(&t1, 0);
    snprintf(line, sizeof line, "stfs : %llu float64, %llu divergences, h=%08x%08x\n",
             r.n, r.bad, r.h1, r.h2);
    out(line);
    fprintf(stderr, "(stfs : %ld s)\n", (long)(t1.tv_sec - t0.tv_sec));
    for (s = 0; s < 2; s++) {
        for (e = 0; e < 2048; e++) {
            for (b = 0; b <= 52; b++) {
                u64 fr[3], x;
                fr[0] = (1ULL << b) & ((1ULL << 52) - 1);
                fr[1] = (1ULL << b) - 1;
                fr[2] = ((1ULL << 52) - 1) >> b;
                for (i = 0; i < 3; i++) {
                    u32 got;
                    x = s << 63 | e << 52 | fr[i];
                    got = hw_stfs(x);
                    ec++;
                    eb += got != ref_tosingle(x);
                    eh = (eh ^ got) * 16777619u;
                }
            }
        }
    }
    snprintf(line, sizeof line, "stfs : %llu cas limites, %llu divergences, h=%08x\n", ec, eb, eh);
    out(line);
    /* 3. formes */
    forms(line, sizeof line, 1u << 22);
    out(line);
    /* 4. fautes */
    pg = mmap(0, 2 * P, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    signal(SIGBUS, onfault);
    signal(SIGSEGV, onfault);
    memset(pg, 0x5a, 2 * P);
    mprotect(pg + P, P, PROT_NONE);
    if ((sig = sigsetjmp(jb, 1)) == 0) {
        __asm__ volatile("lfs f0,0(%0)" : : "b"(pg + P) : "fr0", "memory");
    }
    snprintf(line, sizeof line, "faute lfs : signal %d\n", sig);
    out(line);
    mprotect(pg + P, P, PROT_READ);
    {
        static double src = 3.25;
        if ((sig = sigsetjmp(jb, 1)) == 0) {
            __asm__ volatile("lfd f0,0(%0)\n\tstfs f0,0(%1)"
                             : : "b"(&src), "b"(pg + P) : "fr0", "memory");
        }
        snprintf(line, sizeof line, "faute stfs : signal %d, memoire apres %02x%02x%02x%02x\n",
                 sig, pg[P], pg[P + 1], pg[P + 2], pg[P + 3]);
        out(line);
    }
    snprintf(line, sizeof line, "empreinte %016llx\n", fnv);
    fputs(line, stdout);
    /* 5. banc */
    {
        long n = argc > 1 ? atol(argv[1]) : 0, k;
        if (n > 0) {
            static float fv[8] = { 1.5f, -2.25f, 3.0e10f, 1.0e-3f, 0, 7, 8, 9 };
            gettimeofday(&t0, 0);
            for (k = 0; k < n; k += 4) {
                __asm__ volatile("lfs f0,0(%0)\n\tstfs f0,16(%0)\n\t"
                                 "lfs f1,4(%0)\n\tstfs f1,20(%0)\n\t"
                                 "lfs f2,8(%0)\n\tstfs f2,24(%0)\n\t"
                                 "lfs f3,12(%0)\n\tstfs f3,28(%0)"
                                 : : "b"(fv) : "fr0", "fr1", "fr2", "fr3", "memory");
            }
            gettimeofday(&t1, 0);
            printf("banc : %ld paires lfs/stfs en %ld ms\n", n,
                   (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000);
        }
    }
    return 0;
}
