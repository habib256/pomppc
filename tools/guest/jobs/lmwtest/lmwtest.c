/*
 * lmwtest.c - equivalence de lmw/stmw entre le helper de QEMU et la traduction
 * en ligne (propriete x-lmw-inline, patches/tcg/0002, docs/tcg-g4.md).
 *
 * Execute les VRAIES instructions dans l'invite Tiger et imprime tout ce qui
 * est observable :
 *   1. lmw rN (N = 13..31) depuis chaque decalage d'une liste qui couvre le cas
 *      "dans une page" (chemin en ligne) et "a cheval sur deux pages" (chemin
 *      helper), decalages non alignes compris ; les registres charges sont
 *      relus par des stw un a un (pas par stmw) ;
 *   2. stmw rN vers les memes decalages, registres charges par des lwz un a un ;
 *      la zone autour est relue (debordements) ;
 *   3. faute a mi-chemin : stmw/lmw a cheval sur une page protegee (PROT_READ
 *      pour stmw, PROT_NONE pour lmw) : SIGBUS/SIGSEGV attrape, puis on regarde
 *      si la premiere page a ete ecrite (stmw) ou si des registres ont change
 *      (lmw) avant la faute ;
 *   4. un banc : N paires stmw/lmw de 19 registres (prologue et epilogue
 *      d'une fonction ; N = argv[1], 20 millions par defaut), temps en ms.
 * La sortie des parties 1 a 3 doit etre IDENTIQUE octet pour octet entre
 * x-lmw-inline=off et =on (empreinte FNV en derniere ligne).
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

static unsigned long long fnv = 1469598103934665603ULL;
static void h(const void *p, size_t n)
{
    const unsigned char *c = p;
    while (n--) { fnv ^= *c++; fnv *= 1099511628211ULL; }
}
static void out(const char *fmt, unsigned a, unsigned b, unsigned c)
{
    char buf[128];
    int n = snprintf(buf, sizeof buf, fmt, a, b, c);
    fputs(buf, stdout);
    h(buf, n);
}

#define CLOB "r13","r14","r15","r16","r17","r18","r19","r20","r21","r22", \
             "r23","r24","r25","r26","r27","r28","r29","r30","r31"

/* registres r13..r31 -> dst[0..18], un stw par registre */
#define STW_ALL(d) \
    "stw r13,0(" d ")\n\tstw r14,4(" d ")\n\tstw r15,8(" d ")\n\tstw r16,12(" d ")\n\t" \
    "stw r17,16(" d ")\n\tstw r18,20(" d ")\n\tstw r19,24(" d ")\n\tstw r20,28(" d ")\n\t" \
    "stw r21,32(" d ")\n\tstw r22,36(" d ")\n\tstw r23,40(" d ")\n\tstw r24,44(" d ")\n\t" \
    "stw r25,48(" d ")\n\tstw r26,52(" d ")\n\tstw r27,56(" d ")\n\tstw r28,60(" d ")\n\t" \
    "stw r29,64(" d ")\n\tstw r30,68(" d ")\n\tstw r31,72(" d ")\n\t"
#define LWZ_ALL(s) \
    "lwz r13,0(" s ")\n\tlwz r14,4(" s ")\n\tlwz r15,8(" s ")\n\tlwz r16,12(" s ")\n\t" \
    "lwz r17,16(" s ")\n\tlwz r18,20(" s ")\n\tlwz r19,24(" s ")\n\tlwz r20,28(" s ")\n\t" \
    "lwz r21,32(" s ")\n\tlwz r22,36(" s ")\n\tlwz r23,40(" s ")\n\tlwz r24,44(" s ")\n\t" \
    "lwz r25,48(" s ")\n\tlwz r26,52(" s ")\n\tlwz r27,56(" s ")\n\tlwz r28,60(" s ")\n\t" \
    "lwz r29,64(" s ")\n\tlwz r30,68(" s ")\n\tlwz r31,72(" s ")\n\t"

/* pre : registres = pre[], puis lmw N depuis src, puis tous relus dans dst */
#define DEF(N) \
static void lmw_##N(const void *src, unsigned *dst, const unsigned *pre) { \
    __asm__ volatile(LWZ_ALL("%2") "lmw r" #N ",0(%0)\n\t" STW_ALL("%1") \
                     :: "b"(src), "b"(dst), "b"(pre) : CLOB, "memory"); } \
static void stmw_##N(void *dst, const unsigned *src) { \
    __asm__ volatile(LWZ_ALL("%1") "stmw r" #N ",0(%0)\n\t" \
                     :: "b"(dst), "b"(src) : CLOB, "memory"); }
DEF(13) DEF(14) DEF(15) DEF(16) DEF(17) DEF(18) DEF(19) DEF(20) DEF(21) DEF(22)
DEF(23) DEF(24) DEF(25) DEF(26) DEF(27) DEF(28) DEF(29) DEF(30) DEF(31)
typedef void (*lmw_f)(const void *, unsigned *, const unsigned *);
typedef void (*stmw_f)(void *, const unsigned *);
static lmw_f LMW[] = { lmw_13, lmw_14, lmw_15, lmw_16, lmw_17, lmw_18, lmw_19, lmw_20,
    lmw_21, lmw_22, lmw_23, lmw_24, lmw_25, lmw_26, lmw_27, lmw_28, lmw_29, lmw_30, lmw_31 };
static stmw_f STMW[] = { stmw_13, stmw_14, stmw_15, stmw_16, stmw_17, stmw_18, stmw_19,
    stmw_20, stmw_21, stmw_22, stmw_23, stmw_24, stmw_25, stmw_26, stmw_27, stmw_28,
    stmw_29, stmw_30, stmw_31 };

static sigjmp_buf jb;
static void onfault(int sig) { siglongjmp(jb, sig); }

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 20000000;
    long P = getpagesize();
    unsigned char *buf = mmap(0, 3 * P, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    unsigned pre[19], got[19], src[19];
    int i, N, k;
    struct timeval t0, t1;

    for (i = 0; i < 19; i++) {
        pre[i] = 0xdead0000u + i;
        src[i] = 0x11110000u * (i + 1) + 0x0102u * i;
    }
    for (N = 13; N <= 31; N++) {
        int nb = (32 - N) * 4;
        long offs[] = { 0, 1, 2, 3, 4, 100, P - nb - 4, P - nb - 1, P - nb, P - nb + 1,
                        P - nb + 2, P - nb + 4, P - 8, P - 4, P - 2, P - 1 };
        for (k = 0; k < (int)(sizeof offs / sizeof offs[0]); k++) {
            long o = offs[k];
            if (o < 0) continue;
            /* 1. lmw */
            for (i = 0; i < 3 * P; i++) buf[i] = (unsigned char)(i * 7 + N);
            LMW[N - 13](buf + P / 2 + o, got, pre);
            out("lmw %u off %u :", N, (unsigned)o, 0);
            for (i = 0; i < 19; i++) out(" %08x", got[i], 0, 0);
            out("\n", 0, 0, 0);
            /* 2. stmw */
            memset(buf, 0xa5, 3 * P);
            STMW[N - 13](buf + P / 2 + o, src);
            out("stmw %u off %u :", N, (unsigned)o, 0);
            h(buf, 3 * P);
            for (i = 0; i < nb + 8; i++) {
                if (buf[P / 2 + o - 4 + i] != 0xa5) out(" %02x", buf[P / 2 + o - 4 + i], 0, 0);
                else out(" ..", 0, 0, 0);
            }
            out("\n", 0, 0, 0);
        }
    }
    /* 3. fautes a mi-chemin : page 2 protegee, acces a cheval sur 1|2 */
    signal(SIGBUS, onfault);
    signal(SIGSEGV, onfault);
    for (N = 13; N <= 31; N += 3) {
        int nb = (32 - N) * 4, sig;
        for (i = 0; i < 3 * P; i++) buf[i] = 0x5a;
        mprotect(buf + P, P, PROT_READ);
        if ((sig = sigsetjmp(jb, 1)) == 0) {
            STMW[N - 13](buf + P - nb + 8, src);
            out("faute stmw %u : PAS DE FAUTE\n", N, 0, 0);
        } else {
            int w = 0;
            for (i = 0; i < P; i++) w += buf[i] != 0x5a;
            out("faute stmw %u : signal %u, octets ecrits avant la faute %u\n", N, sig, w);
        }
        mprotect(buf + P, P, PROT_NONE);
        memset(got, 0, sizeof got);
        if ((sig = sigsetjmp(jb, 1)) == 0) {
            LMW[N - 13](buf + P - nb + 8, got, pre);
            out("faute lmw %u : PAS DE FAUTE\n", N, 0, 0);
        } else {
            out("faute lmw %u : signal %u\n", N, sig, 0);
        }
        mprotect(buf + P, P, PROT_READ | PROT_WRITE);
    }
    printf("empreinte %016llx\n", fnv);
    /* 4. banc (hors empreinte) */
    gettimeofday(&t0, 0);
    for (i = 0; i < iters; i++) {
        STMW[0](buf + 256, src);
        LMW[0](buf + 256, got, pre);
    }
    gettimeofday(&t1, 0);
    printf("banc %ld x (stmw+lmw de 19 registres) : %ld ms\n", iters,
           (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000));
    return 0;
}
