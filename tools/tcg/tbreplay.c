/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * tbreplay.c - fait retraduire par un qemu-ppc linux-user les blocs de code
 * invité d'un journal QEMU (« IN: » + OBJD-T, tools/tcg/tbreplay.py les
 * extrait), pour comparer le code x86 que deux binaires ou deux réglages
 * produisent pour EXACTEMENT les mêmes instructions (docs/tcg-g4.md
 * section 36 ; journal -d in_asm,out_asm, tools/tcg/tcgsize.py).
 *
 * Chaque bloc est recopié à son adresse d'origine (pages RWX fixes), tous
 * les GPR pointent au milieu d'une zone de données nulle, CTR = 1, LR = un
 * retour ; on saute au début du bloc. QEMU traduit le bloc entier avant de
 * l'exécuter : peu importe qu'il fasse ensuite une faute (SIGSEGV, SIGILL,
 * SIGBUS) ou boucle (SIGALRM), le gestionnaire revient ici par siglongjmp.
 *
 *   powerpc-linux-gnu-gcc -O1 -static -fno-stack-protector -o tbreplay.ppc tbreplay.c
 *   qemu-ppc -cpu g4,... -d in_asm,out_asm -D log tbreplay.ppc blocs.bin
 * Format de blocs.bin : suite de (u32 adresse, u32 n, n mots), gros-boutiste.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef unsigned int u32;

static sigjmp_buf jb;
static u32 saved_r2, saved_r13;

/* le bloc a pu écraser r2 (pointeur de TLS) et r13 : les remettre d'abord */
static void onsig(int s)
{
    __asm__ __volatile__("mr 2,%0\n\tmr 13,%1" : : "r"(saved_r2), "r"(saved_r13));
    (void)s;
    siglongjmp(jb, 1);
}

static char data[1 << 20] __attribute__((aligned(4096)));
static void ret_stub(void) { }

/* saute en addr avec r1..r31 = p, CTR = 1, LR = retour */
static void run_at(u32 addr, char *p, void (*ret)(void))
{
    __asm__ __volatile__(
        "mr 0,%0\n\t"
        "mtctr 0\n\t"           /* adresse du bloc */
        "mr 0,%2\n\t"
        "mtlr 0\n\t"
        "mr 3,%1\n\t" "mr 4,%1\n\t" "mr 5,%1\n\t" "mr 6,%1\n\t" "mr 7,%1\n\t"
        "mr 8,%1\n\t" "mr 9,%1\n\t" "mr 10,%1\n\t" "mr 11,%1\n\t" "mr 12,%1\n\t"
        "bctr\n\t"
        : : "r"(addr), "r"(p), "r"(ret)
        : "0", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "ctr", "lr",
          "memory");
}

int main(int argc, char **argv)
{
    FILE *f;
    u32 hdr[2], *w;
    long n = 0;
    struct sigaction sa;

    if (argc < 2 || !(f = fopen(argv[1], "rb"))) {
        fprintf(stderr, "tbreplay blocs.bin\n");
        return 2;
    }
    {
        /* le bloc a pu déplacer r1 : le gestionnaire a sa propre pile */
        stack_t ss;
        ss.ss_sp = malloc(1 << 16);
        ss.ss_size = 1 << 16;
        ss.ss_flags = 0;
        sigaltstack(&ss, 0);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onsig;
    sa.sa_flags = SA_NODEFER | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGFPE, &sa, 0);
    sigaction(SIGTRAP, &sa, 0);
    sigaction(SIGALRM, &sa, 0);
    __asm__ __volatile__("mr %0,2\n\tmr %1,13" : "=r"(saved_r2), "=r"(saved_r13));
    w = malloc(4 * 65536);
    while (fread(hdr, 4, 2, f) == 2) {
        /* volatile : relus après siglongjmp */
        volatile u32 a = hdr[0], k = hdr[1], pg = a & ~4095u,
                     end = (a + 4 * k + 4095) & ~4095u;
        if (k > 65536 || fread(w, 4, k, f) != k) {
            break;
        }
        if (mmap((void *)pg, end - pg, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0) == MAP_FAILED) {
            continue;
        }
        memcpy((void *)a, w, 4 * k);
        __builtin___clear_cache((char *)a, (char *)a + 4 * k);
        memset(data, 0, sizeof(data));
        if (getenv("TBREPLAY_V")) {
            fprintf(stderr, "bloc %08x (%u mots)\n", a, k);
        }
        if (sigsetjmp(jb, 1) == 0) {
            alarm(1);
            run_at(a, data + (1 << 19), ret_stub);
        }
        __asm__ __volatile__("mr 2,%0\n\tmr 13,%1" : : "r"(saved_r2), "r"(saved_r13));
        alarm(0);
        munmap((void *)pg, end - pg);
        n++;
    }
    printf("tbreplay : %ld blocs\n", n);
    return 0;
}
