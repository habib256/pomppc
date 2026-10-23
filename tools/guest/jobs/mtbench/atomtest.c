/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 VERHILLE Arnaud
 *
 * atomtest — contention lwarx/stwcx. entre deux fils (docs/ppc32-mttcg.md).
 *
 * Deux fils (sur deux vCPU en SMP=2) incrémentent N fois chacun :
 *   A. un compteur partagé, par une boucle lwarx/addi/stwcx./bne- (atomique) ;
 *   B. un compteur ordinaire protégé par un verrou tournant lwarx/stwcx.
 *      (acquisition + isync, libération sync + stw 0) : vérifie aussi l'ordre
 *      mémoire (les écritures de la section critique visibles avant le verrou
 *      rendu) ;
 *   C. deux compteurs voisins dans la même ligne de cache, un par fil
 *      (réservations qui se touchent sans porter sur le même mot).
 * Le résultat doit être exact : A = B = 2N, C = N + N. Un défaut d'atomicité
 * (stwcx. réussi sur une réservation perdue) perd des incréments.
 *
 *   atomtest [N] [fils]     (défaut 4000000, 2)
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static volatile int shared_a;
static volatile int lockw;
static volatile int plain_b;
static volatile int pair_c[8] __attribute__((aligned(32)));
static int N = 4000000;
static int NT = 2;
static volatile int go;

static inline void atom_inc(volatile int *p)
{
    int t;
    __asm__ volatile("1: lwarx %0,0,%1\n"
                     "   addi %0,%0,1\n"
                     "   stwcx. %0,0,%1\n"
                     "   bne- 1b"
                     : "=&b"(t) : "r"(p) : "cc", "memory");
}

static inline void lock(volatile int *l)
{
    int t;
    __asm__ volatile("1: lwarx %0,0,%1\n"
                     "   cmpwi %0,0\n"
                     "   bne- 1b\n"
                     "   li %0,1\n"
                     "   stwcx. %0,0,%1\n"
                     "   bne- 1b\n"
                     "   isync"
                     : "=&r"(t) : "r"(l) : "cc", "memory");
}

static inline void unlock(volatile int *l)
{
    __asm__ volatile("sync" ::: "memory");
    *l = 0;
}

static double now(void)
{
    struct timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec + t.tv_usec / 1e6;
}

static void *run_a(void *arg)
{
    int i;
    (void)arg;
    while (!go) { }
    for (i = 0; i < N; i++) {
        atom_inc(&shared_a);
    }
    return 0;
}

static void *run_b(void *arg)
{
    int i;
    (void)arg;
    while (!go) { }
    for (i = 0; i < N; i++) {
        lock(&lockw);
        plain_b = plain_b + 1;
        unlock(&lockw);
    }
    return 0;
}

static void *run_c(void *arg)
{
    int i, k = (int)(long)arg;
    while (!go) { }
    for (i = 0; i < N; i++) {
        atom_inc(&pair_c[k]);
    }
    return 0;
}

static int phase(const char *name, void *(*fn)(void *), int expect_each)
{
    pthread_t th[8];
    double t0, t1;
    int k, bad = 0, sum = 0;

    go = 0;
    for (k = 0; k < NT; k++) {
        pthread_create(&th[k], 0, fn, (void *)(long)k);
    }
    t0 = now();
    go = 1;
    for (k = 0; k < NT; k++) {
        pthread_join(th[k], 0);
    }
    t1 = now();
    if (fn == run_a) {
        sum = shared_a;
        bad = sum != NT * expect_each;
    } else if (fn == run_b) {
        sum = plain_b;
        bad = sum != NT * expect_each;
    } else {
        for (k = 0; k < NT; k++) {
            sum += pair_c[k];
            bad |= pair_c[k] != expect_each;
        }
    }
    printf("atom %s fils=%d N=%d total=%d attendu=%d %s %.3f s\n", name, NT, N,
           sum, NT * expect_each, bad ? "FAUX" : "exact", t1 - t0);
    return bad;
}

int main(int argc, char **argv)
{
    int bad = 0;
    if (argc > 1) {
        N = atoi(argv[1]);
    }
    if (argc > 2) {
        NT = atoi(argv[2]);
    }
    if (NT < 1 || NT > 8) {
        NT = 2;
    }
    bad |= phase("A-lwarx", run_a, N);
    bad |= phase("B-verrou", run_b, N);
    bad |= phase("C-voisins", run_c, N);
    printf("atom bilan %s\n", bad ? "ECHEC" : "OK");
    return bad;
}
