/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Compare the actual NEON RAM-copy functions with word-by-word references.
 * Every start register and byte alignment, 32/64-bit GPRs, and guard pages.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

#define HOST_BIG_ENDIAN 0
#ifndef TARGET_LONG_BITS
#define TARGET_LONG_BITS 64
#endif
#if TARGET_LONG_BITS == 64
typedef uint64_t target_ulong;
#else
typedef uint32_t target_ulong;
#endif
static uint32_t ldl_be_p(const void *p)
{
    uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v);
}
static void stl_be_p(void *p, uint32_t v)
{
    v = __builtin_bswap32(v); memcpy(p, &v, 4);
}
#include "lmw-vector.h"

static uint64_t rng = 0x1badc0de;
static uint64_t next(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;
}
static void check(uint8_t *p, unsigned reg)
{
    target_ulong got[32], ref[32], input[32];
    uint8_t before[160], after[160];
    size_t bytes = (32 - reg) * 4;
    for (unsigned i = 0; i < 32; i++) {
        got[i] = ref[i] = next(); input[i] = next();
    }
    for (size_t i = 0; i < bytes; i++) p[i] = next();
    for (unsigned i = reg; i < 32; i++) ref[i] = ldl_be_p(p + 4 * (i - reg));
    ppc_lmw_vector(got, p, reg);
    if (memcmp(got, ref, sizeof got)) abort();
    memset(before, 0xa5, sizeof before); memcpy(after, before, sizeof before);
    for (unsigned i = reg; i < 32; i++) stl_be_p(before + 16 + 4 * (i - reg), input[i]);
    ppc_stmw_vector(input, after + 16, reg);
    if (memcmp(before, after, sizeof before)) abort();
    ppc_stmw_vector(input, p, reg);
    for (unsigned i = reg; i < 32; i++) {
        if (ldl_be_p(p + 4 * (i - reg)) != (uint32_t)input[i]) abort();
    }
}
int main(void)
{
    _Alignas(16) uint8_t buf[160];
    long page = sysconf(_SC_PAGESIZE);
    uint8_t *m = mmap(NULL, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED || mprotect(m + page, page, PROT_READ | PROT_WRITE)) return 2;
    uint64_t cases = 0;
    for (unsigned round = 0; round < 1000; round++) {
        for (unsigned reg = 0; reg < 32; reg++) {
            for (unsigned off = 0; off < 32; off++) { check(buf + off, reg); cases++; }
        }
    }
    for (unsigned reg = 0; reg < 32; reg++) {
        check(m + page, reg);
        check(m + 2 * page - 4 * (32 - reg), reg); cases += 2;
    }
    munmap(m, 3 * page);
    printf("lmw/stmw %d bits: %llu cases, guard pages, zero differences\n",
           TARGET_LONG_BITS, (unsigned long long)cases);
    return 0;
}
