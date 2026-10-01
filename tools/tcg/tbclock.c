/* GPL3 - Copyleft VERHILLE Arnaud
 * tbclock.c — sur l'hôte (arm64 macOS) : ce que coûte la base de temps de QEMU
 * (clock_gettime(CLOCK_MONOTONIC) + muldiv64) et ce que coûterait sa lecture
 * directe (cntvct_el0), résolution et relation entre les horloges.
 *   cc -O2 -o tbclock tools/tcg/tbclock.c && ./tbclock */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <mach/mach_time.h>

static inline uint64_t cntvct(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
static inline uint64_t cntvct_isb(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v; }
static inline uint64_t cntfrq(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
static int64_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c) { return (unsigned __int128)a * b / c; }

#define BANC(nom, expr) do { uint64_t a = mach_absolute_time(); \
    for (int i = 0; i < N; i++) s += (expr); \
    uint64_t b = mach_absolute_time(); printf("%-36s %6.1f ns\n", nom, (b - a) * 125.0 / 3 / N); } while (0)

int main(void)
{
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    printf("cntfrq %llu Hz, mach timebase %u/%u\n", (unsigned long long)cntfrq(), tb.numer, tb.denom);
    const int N = 2000000; volatile int64_t s = 0;
    BANC("clock_gettime(MONOTONIC)", mono());
    BANC("+ muldiv64(…, 25 MHz, 1e9)", muldiv64(mono(), 25000000, 1000000000));
    BANC("cntvct_el0", cntvct());
    BANC("isb + cntvct_el0", cntvct_isb());
    BANC("cntvct * 25 / 24", cntvct() * 25 / 24);
    BANC("mach_continuous_time", mach_continuous_time());
    BANC("mach_absolute_time", mach_absolute_time());

    int64_t m0 = mono(), m1 = m0; int eq = 0;
    for (int i = 0; i < 1000000; i++) { m1 = mono(); if (m1 == m0) eq++; m0 = m1; }
    printf("MONOTONIC : %d/1000000 lectures égales à la précédente ; ns %% 1000 = %lld\n", eq, (long long)(m1 % 1000));
    for (int k = 0; k < 3; k++) {
        uint64_t c = cntvct(), ct = mach_continuous_time(), ab = mach_absolute_time();
        int64_t m = mono();
        printf("cntvct %llu  continuous %llu  absolute %llu  mono-continuous*125/3 = %lld ns\n",
               (unsigned long long)c, (unsigned long long)ct, (unsigned long long)ab,
               (long long)(m - (int64_t)(ct * 125 / 3)));
    }
    return 0;
}
