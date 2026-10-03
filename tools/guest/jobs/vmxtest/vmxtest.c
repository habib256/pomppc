/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * vmxtest.c - equivalence de vsldoi, vmrghw, vmrglw, stvebx, stvehx, stvewx
 * entre les helpers d'origine et leur traduction en ligne (propriete
 * x-vmx-inline, patches/tcg/0021-ppc-vmx-inline.patch, docs/tcg-g4.md
 * section 28).
 *
 * Execute les VRAIES instructions dans l'invite Tiger :
 *   1. vsldoi a chaque decalage (0..15), vmrghw, vmrglw sur N couples
 *      aleatoires, et les recouvrements vD = vA, vD = vB, vD = vA = vB ;
 *   2. stvebx/stvehx/stvewx a chaque adresse basse (0..15) d'un tampon,
 *      N vecteurs, le tampon entier relu (octets voisins intacts) ;
 *   3. les fautes : stvewx vers une page PROT_READ (signal, page intacte).
 * et hache les resultats. La sortie doit etre IDENTIQUE octet pour octet
 * entre x-vmx-inline=off et =on (empreinte FNV en derniere ligne).
 * Un banc (argument N) : N x (vsldoi + vmrghw + vmrglw + stvewx) dependants.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

typedef unsigned int u32;
typedef unsigned long long u64;
typedef union { vector unsigned int v; vector unsigned char c; u32 u[4]; } V;

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

static u32 hh[4];
static void acc(const V *r)
{
    int i;
    for (i = 0; i < 4; i++) {
        hh[i] = (hh[i] ^ r->u[i]) * 16777619u;
    }
}

static u64 st = 0x5eed;
static u32 rnd(void)
{
    st = st * 6364136223846793005ULL + 1442695040888963407ULL;
    return (u32)(st >> 32);
}
static void rv(V *v)
{
    int i;
    for (i = 0; i < 4; i++) v->u[i] = rnd();
}

/*
 * Chaque instruction par asm volatile (le compilateur ne la fusionne ni ne
 * l'elimine), y compris les recouvrements de registres vD = vA, vD = vB,
 * vD = vA = vB, qu'un intrinseque ne garantit pas.
 */
#define SLD(k) do {                                                          \
    asm volatile("vsldoi %0,%1,%2,%3" : "=v"(r.c) : "v"(a.c), "v"(b.c), "i"(k)); acc(&r); \
    x = a; asm volatile("vsldoi %0,%0,%1,%2" : "+v"(x.c) : "v"(b.c), "i"(k)); acc(&x);   \
    x = b; asm volatile("vsldoi %0,%1,%0,%2" : "+v"(x.c) : "v"(a.c), "i"(k)); acc(&x);   \
    x = a; asm volatile("vsldoi %0,%0,%0,%1" : "+v"(x.c) : "i"(k)); acc(&x);             \
} while (0)
#define MRG(op) do {                                                         \
    asm volatile(op " %0,%1,%2" : "=v"(r.v) : "v"(a.v), "v"(b.v)); acc(&r);  \
    x = a; asm volatile(op " %0,%0,%1" : "+v"(x.v) : "v"(b.v)); acc(&x);     \
    x = b; asm volatile(op " %0,%1,%0" : "+v"(x.v) : "v"(a.v)); acc(&x);     \
    x = a; asm volatile(op " %0,%0,%0" : "+v"(x.v)); acc(&x);                \
} while (0)

static sigjmp_buf jb;
static volatile int nsig;
static void onsig(int s)
{
    nsig++;
    siglongjmp(jb, 1);
}

int main(int argc, char **argv)
{
    char line[256];
    long nrand = getenv("NVEC") ? atol(getenv("NVEC")) : (1L << 20), n;
    long banc = argc > 1 ? atol(argv[1]) : 0;
    unsigned off, i;
    V a, b, r, x;
    struct timeval t0, t1;
    static unsigned char buf[64] __attribute__((aligned(16)));

    setvbuf(stdout, 0, _IONBF, 0);

    /* 1. permutations */
    memset(hh, 0, sizeof hh);
    for (n = 0; n < nrand; n++) {
        rv(&a);
        rv(&b);
        SLD(0); SLD(1); SLD(2); SLD(3); SLD(4); SLD(5); SLD(6); SLD(7);
        SLD(8); SLD(9); SLD(10); SLD(11); SLD(12); SLD(13); SLD(14); SLD(15);
        MRG("vmrghw");
        MRG("vmrglw");
    }
    snprintf(line, sizeof line, "vsldoi x 16, vmrghw, vmrglw : %ld couples, h=%08x%08x%08x%08x\n",
             nrand, hh[0], hh[1], hh[2], hh[3]);
    out(line);

    /* 2. rangements d'un element, chaque adresse basse ; tampon entier hache */
    memset(hh, 0, sizeof hh);
    for (n = 0; n < nrand / 4; n++) {
        rv(&a);
        for (off = 0; off < 16; off++) {
            memset(buf, 0x5a, sizeof buf);
            vec_ste(a.c, off, buf + 16);
            vec_ste((vector unsigned short)a.v, off, (unsigned short *)(buf + 32));
            vec_ste(a.v, off, (u32 *)(buf + 48));
            for (i = 0; i < 4; i++) {
                x.u[0] = ((u32 *)buf)[i * 4 + 0];
                x.u[1] = ((u32 *)buf)[i * 4 + 1];
                x.u[2] = ((u32 *)buf)[i * 4 + 2];
                x.u[3] = ((u32 *)buf)[i * 4 + 3];
                acc(&x);
            }
        }
    }
    snprintf(line, sizeof line, "stvebx/stvehx/stvewx x 16 adresses : %ld vecteurs, h=%08x%08x%08x%08x\n",
             nrand / 4, hh[0], hh[1], hh[2], hh[3]);
    out(line);

    /* 3. fautes : stve[bhw]x vers une page en lecture seule */
    {
        long pg = sysconf(_SC_PAGESIZE);
        unsigned char *p = mmap(0, pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        unsigned ok = 0;
        memset(p, 0xa5, pg);
        mprotect(p, pg, PROT_READ);
        signal(SIGBUS, onsig);
        signal(SIGSEGV, onsig);
        rv(&a);
        for (off = 0; off < 16; off++) {
            if (!sigsetjmp(jb, 1)) vec_ste(a.c, off, p + 64);
            if (!sigsetjmp(jb, 1)) vec_ste((vector unsigned short)a.v, off, (unsigned short *)(p + 128));
            if (!sigsetjmp(jb, 1)) vec_ste(a.v, off, (u32 *)(p + 256));
        }
        for (i = 0; i < pg; i++) ok += p[i] == 0xa5;
        snprintf(line, sizeof line, "fautes : %d signaux sur 48, page intacte %s\n",
                 nsig, ok == (unsigned)pg ? "oui" : "NON");
        out(line);
    }
    snprintf(line, sizeof line, "empreinte %016llx\n", fnv);
    fputs(line, stdout);

    if (banc > 0) {
        V p0, p1;
        rv(&p0);
        rv(&p1);
        gettimeofday(&t0, 0);
        for (n = 0; n < banc; n++) {
            p0.c = vec_sld(p0.c, p1.c, 4);
            p1.v = vec_mergeh(p1.v, p0.v);
            p0.v = vec_mergel(p0.v, p1.v);
            vec_ste(p0.v, (int)(n & 15), (u32 *)buf);
        }
        gettimeofday(&t1, 0);
        printf("banc : %ld x (vsldoi + vmrghw + vmrglw + stvewx) en %ld ms (%08x)\n", banc,
               (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000),
               p0.u[0] ^ ((u32 *)buf)[0]);
    }
    return 0;
}
