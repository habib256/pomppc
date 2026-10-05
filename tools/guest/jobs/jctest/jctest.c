/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Indirect branches at several code strides; also invalidate a warm block.
 * Synthetic benchmark: its time is not a game-performance estimate.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/time.h>
typedef unsigned (*fn)(unsigned);
static void flush(void *p, size_t n)
{
    char *a = (char *)((uintptr_t)p & ~31UL), *end = (char *)p + n, *q;
    for (q = a; q < end; q += 32) __asm__ volatile("dcbst 0,%0" :: "r"(q) : "memory");
    __asm__ volatile("sync" ::: "memory");
    for (q = a; q < end; q += 32) __asm__ volatile("icbi 0,%0" :: "r"(q) : "memory");
    __asm__ volatile("sync\n\tisync" ::: "memory");
}
static double now(void)
{
    struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec * 1e-6;
}
int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? strtoul(argv[1], NULL, 0) : 2000000;
    unsigned strides[] = {16, 64, 256};
    unsigned s, i, rep;
    for (s = 0; s < 3; s++) {
        size_t size = strides[s] * 1024;
        uint8_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
        fn calls[1024];
        if (p == MAP_FAILED) { perror("mmap"); return 2; }
        for (i = 0; i < 1024; i++) {
            uint32_t *code = (uint32_t *)(p + i * strides[s]);
            code[0] = 0x38630001; /* addi r3,r3,1 */
            code[1] = 0x4e800020; /* blr */
            calls[i] = (fn)code;
        }
        flush(p, size);
        for (i = 0; i < 1024; i++) if (calls[i](10) != 11) return 1;
        for (rep = 0; rep < 3; rep++) {
            unsigned value = 0;
            double t = now();
            for (i = 0; i < n; i++) value = calls[(i * 29) & 1023](value);
            printf("jc stride=%u trial=%u calls=%u ns=%.1f result=%u\n",
                   strides[s], rep, n, (now() - t) * 1e9 / n, value);
            if (value != n) return 1;
        }
        *(uint32_t *)p = 0x38630002; /* addi r3,r3,2 */
        flush(p, 8);
        if (calls[0](10) != 12) { fprintf(stderr, "stale TB\n"); return 1; }
        munmap(p, size);
    }
    puts("indirect calls and warm-TB invalidation: OK");
    return 0;
}
