/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * dcbztest.c - dcbz et lmw/stmw courts dans l'invite : preuve de
 * x-dcbz-inline (patches/tcg/0033) et banc de x-lmw-inline (patches/tcg/0032),
 * docs/tcg-g4.md section 32.
 *   1. dcbz a chaque decalage 0..63 d'un tampon a motif : la ligne de 32
 *      octets qui contient EA est mise a zero, rien autour ; EA aligne ou non,
 *      forme rA = 0 et rA != 0 ;
 *   2. dcbz sur une page en lecture seule : faute (adresse rendue, page
 *      intacte) ; sur une page PROT_NONE ;
 *   3. reservation : lwarx X ; dcbz sur la ligne de X ; stwcx. X doit echouer ;
 *      dcbz sur une autre ligne : stwcx. reussit ;
 *   4. lmw/stmw de r24..r31 a r31 (1 a 8 registres, la forme en ligne) a tous
 *      les decalages d'une fin de page (dans la page / a cheval) ;
 * empreinte FNV identique avec et sans les proprietes.  Puis les bancs.
 *   dcbztest [banc N]
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * Deux empreintes : tout, et tout sauf la partie 3. Le helper_dcbz d'origine
 * est declare TCG_CALL_NO_WG mais ecrit env->reserve_addr (une globale TCG) :
 * si la reservation est encore dans un registre de l'hote, le stwcx. qui suit
 * la voit intacte -- selon l'allocation des registres. La forme en ligne la
 * retire toujours, comme le helper le veut (docs/tcg-g4.md section 32).
 */
static unsigned long long fnv = 1469598103934665603ULL;
static unsigned long long fnv2 = 1469598103934665603ULL;
static int in_resv;
static void h(const void *p, size_t n)
{
    const unsigned char *c = p;
    while (n--) {
        fnv ^= *c; fnv *= 1099511628211ULL;
        if (!in_resv) { fnv2 ^= *c; fnv2 *= 1099511628211ULL; }
        c++;
    }
}
static void out(const char *fmt, unsigned long a, unsigned long b, unsigned long c)
{
    char buf[256];
    int n = snprintf(buf, sizeof buf, fmt, a, b, c);
    fputs(buf, stdout);
    h(buf, n);
}

static sigjmp_buf jb;
static volatile unsigned long fault_addr;
static void onfault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    fault_addr = (unsigned long)si->si_addr;
    siglongjmp(jb, sig);
}

static inline void dcbz_ra(void *base, long off)
{
    __asm__ volatile("dcbz %0,%1" :: "b"(base), "r"(off) : "memory");
}
static inline void dcbz_0(void *p)
{
    __asm__ volatile("dcbz 0,%0" :: "r"(p) : "memory");
}

/* stwcx. apres lwarx et un dcbz : 1 si le stwcx. a reussi */
static int resv(volatile unsigned *x, void *z)
{
    unsigned ok, v;
    __asm__ volatile("lwarx %1,0,%2\n\t"
                     "dcbz 0,%3\n\t"
                     "stwcx. %1,0,%2\n\t"
                     "mfcr %0\n\t"
                     : "=&r"(ok), "=&r"(v) : "r"(x), "r"(z) : "cr0", "memory");
    return (ok >> 29) & 1;
}

#define CLOB "r24","r25","r26","r27","r28","r29","r30","r31"
#define LWZ8(s) "lwz r24,0(" s ")\n\tlwz r25,4(" s ")\n\tlwz r26,8(" s ")\n\tlwz r27,12(" s ")\n\t" \
                "lwz r28,16(" s ")\n\tlwz r29,20(" s ")\n\tlwz r30,24(" s ")\n\tlwz r31,28(" s ")\n\t"
#define STW8(d) "stw r24,0(" d ")\n\tstw r25,4(" d ")\n\tstw r26,8(" d ")\n\tstw r27,12(" d ")\n\t" \
                "stw r28,16(" d ")\n\tstw r29,20(" d ")\n\tstw r30,24(" d ")\n\tstw r31,28(" d ")\n\t"
#define DEF(N) \
static void lmw_##N(const void *src, unsigned *dst, const unsigned *pre) { \
    __asm__ volatile(LWZ8("%2") "lmw r" #N ",0(%0)\n\t" STW8("%1") \
                     :: "b"(src), "b"(dst), "b"(pre) : CLOB, "memory"); } \
static void stmw_##N(void *dst, const unsigned *src) { \
    __asm__ volatile(LWZ8("%1") "stmw r" #N ",0(%0)\n\t" \
                     :: "b"(dst), "b"(src) : CLOB, "memory"); }
DEF(24) DEF(25) DEF(26) DEF(27) DEF(28) DEF(29) DEF(30) DEF(31)
typedef void (*lmw_f)(const void *, unsigned *, const unsigned *);
typedef void (*stmw_f)(void *, const unsigned *);
static lmw_f LMW[] = { lmw_24, lmw_25, lmw_26, lmw_27, lmw_28, lmw_29, lmw_30, lmw_31 };
static stmw_f STMW[] = { stmw_24, stmw_25, stmw_26, stmw_27, stmw_28, stmw_29, stmw_30, stmw_31 };

static long ms_since(struct timeval *t0)
{
    struct timeval t1;
    gettimeofday(&t1, 0);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_usec - t0->tv_usec) / 1000;
}

static void bench(long n, unsigned char *buf)
{
    struct timeval t0;
    long i, k;
    unsigned pre[8], got[8], src[8];
    for (i = 0; i < 8; i++) { pre[i] = i; src[i] = 0x1111 * i; }
    gettimeofday(&t0, 0);
    for (i = 0; i < n; i++)
        for (k = 0; k < 4096; k += 32) dcbz_0(buf + k);
    printf("banc dcbz %ld x 128 lignes : %ld ms\n", n, ms_since(&t0));
    gettimeofday(&t0, 0);
    for (i = 0; i < n * 64; i++) {
        STMW[4](buf + 256, src);   /* stmw r28 : 4 registres, epilogue type */
        LMW[4](buf + 256, got, pre);
    }
    printf("banc lmw/stmw r28 %ld paires : %ld ms\n", n * 64, ms_since(&t0));
}

int main(int argc, char **argv)
{
    long P = getpagesize();
    unsigned char *buf = mmap(0, 3 * P, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    struct sigaction sa;
    int i, k, sig;
    unsigned pre[8], got[8], src[8];

    if (argc > 2 && !strcmp(argv[1], "banc")) {
        bench(atol(argv[2]), buf);
        return 0;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = onfault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    /* 1. dcbz a chaque decalage */
    for (k = 0; k < 64; k++) {
        for (i = 0; i < 3 * P; i++) buf[i] = (unsigned char)(i * 13 + 1);
        if (k & 1) dcbz_ra(buf + P, k); else dcbz_0(buf + P + k);
        out("dcbz %lu :", k, 0, 0);
        h(buf, 3 * P);
        for (i = P - 32; i < P + 128; i++) {
            if (buf[i] != (unsigned char)(i * 13 + 1)) out(" %lx", i - P, 0, 0);
        }
        out("\n", 0, 0, 0);
    }
    /* 2. fautes */
    for (k = 0; k < 64; k += 7) {
        for (i = 0; i < 3 * P; i++) buf[i] = 0x5a;
        mprotect(buf + P, P, k & 1 ? PROT_NONE : PROT_READ);
        if ((sig = sigsetjmp(jb, 1)) == 0) {
            dcbz_0(buf + P + 1000 + k);
            out("faute dcbz %lu : PAS DE FAUTE\n", k, 0, 0);
        } else {
            out("faute dcbz %lu : signal %lu a +%lx", k, sig, fault_addr - (unsigned long)buf);
            mprotect(buf + P, P, PROT_READ);
            for (i = 0; i < P && buf[P + i] == 0x5a; i++) ;
            out(", page intacte %lu\n", i == P, 0, 0);
        }
        mprotect(buf + P, P, PROT_READ | PROT_WRITE);
    }
    /* 3. reservation */
    in_resv = 1;
    for (k = 0; k < 64; k += 4) {
        volatile unsigned *x = (volatile unsigned *)(buf + 512 + 32);
        out("resv %lu : stwcx %lu\n", k, resv(x, buf + 512 + k), 0);
    }
    in_resv = 0;
    /* 4. lmw/stmw courts */
    for (i = 0; i < 8; i++) { pre[i] = 0xdead0000u + i; src[i] = 0x01020304u * (i + 1); }
    for (k = 0; k < 8; k++) {
        int nb = (8 - k) * 4;
        long offs[] = { 0, 2, P - nb - 4, P - nb, P - nb + 2, P - nb + 4, P - 4, P - 2 };
        unsigned o;
        for (o = 0; o < sizeof offs / sizeof offs[0]; o++) {
            for (i = 0; i < 3 * P; i++) buf[i] = (unsigned char)(i * 5 + k);
            LMW[k](buf + offs[o], got, pre);
            out("lmw r%lu +%lx :", 24 + k, offs[o], 0);
            for (i = 0; i < 8; i++) out(" %08lx", got[i], 0, 0);
            out("\n", 0, 0, 0);
            memset(buf, 0xa5, 3 * P);
            STMW[k](buf + offs[o], src);
            h(buf, 3 * P);
            out("stmw r%lu +%lx\n", 24 + k, offs[o], 0);
        }
        /* faute a cheval : page 2 protegee */
        memset(buf, 0x5a, 3 * P);
        mprotect(buf + P, P, PROT_READ);
        if ((sig = sigsetjmp(jb, 1)) == 0) {
            STMW[k](buf + P - nb + 4, src);
            out("faute stmw r%lu : PAS DE FAUTE\n", 24 + k, 0, 0);
        } else {
            int w = 0;
            for (i = 0; i < P; i++) w += buf[i] != 0x5a;
            out("faute stmw r%lu : signal %lu, octets ecrits avant %lu\n", 24 + k, sig, w);
        }
        mprotect(buf + P, P, PROT_READ | PROT_WRITE);
    }
    printf("empreinte %016llx\n", fnv);
    printf("empreinte hors reservation %016llx\n", fnv2);
    bench(50000, buf);
    return 0;
}
