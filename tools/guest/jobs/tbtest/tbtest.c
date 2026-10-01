/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 VERHILLE Arnaud
 * tbtest — coût et cohérence de la base de temps lue par mftb/mftbu.
 *   gcc -O2 -o tbtest tbtest.c && ./tbtest [N]
 * 1. ns par lecture (mftb seul, couple mftbu/mftb/mftbu comme le fait Tiger) ;
 * 2. monotonie : N lectures 64 bits successives, aucune ne doit reculer ;
 * 3. fréquence : ticks par seconde mesurés contre gettimeofday (attendu 25 MHz). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>

static inline uint32_t tbl(void) { uint32_t v; __asm__ volatile("mftb %0" : "=r"(v)); return v; }
static inline uint32_t tbu(void) { uint32_t v; __asm__ volatile("mftbu %0" : "=r"(v)); return v; }
static inline uint64_t tb64(void)
{
    uint32_t u, l, u2;
    do { u = tbu(); l = tbl(); u2 = tbu(); } while (u != u2);
    return ((uint64_t)u << 32) | l;
}
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec * 1e-6; }

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 2000000, i, recul = 0, egaux = 0;
    volatile uint32_t s = 0;
    double t0 = now();
    for (i = 0; i < n; i++) s += tbl();
    double t1 = now();
    uint64_t prev = tb64(), maxsaut = 0;
    for (i = 0; i < n; i++) {
        uint64_t v = tb64();
        if (v < prev) recul++;
        else if (v == prev) egaux++;
        else if (v - prev > maxsaut) maxsaut = v - prev;
        prev = v;
    }
    double t2 = now();
    uint64_t a = tb64(); double ta = now();
    while (now() - ta < 1.0) ;
    uint64_t b = tb64(); double tb = now();
    printf("mftb %.1f ns, tb64 %.1f ns ; reculs %ld, égaux %ld, plus grand saut %llu ; fréquence %.3f MHz\n",
           (t1 - t0) * 1e9 / n, (t2 - t1) * 1e9 / n, recul, egaux,
           (unsigned long long)maxsaut, (b - a) / (tb - ta) / 1e6);
    return recul != 0;
}
