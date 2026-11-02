/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/* Exécuter le mécanisme proposé sur le silicium M4, hors de QEMU. */
#if !defined(__APPLE__) || !defined(__aarch64__)
#error "Cette preuve doit être compilée pour macOS arm64."
#endif
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>

typedef uint32_t tcg_insn_unit;
#define tcg_splitwx_to_rx(p) (p) /* MAP_JIT : même adresse RW/RX dans ce processus. */

/* [D] QEMU v11.1.2 include/qemu/bitops.h : mêmes champs signés et dépôts. */
static int64_t sextract64(uint64_t v, int start, int length)
{
    return (int64_t)(v << (64 - start - length)) >> (64 - length);
}

static uint32_t deposit32(uint32_t v, int start, int length, uint32_t field)
{
    uint32_t mask = ((UINT32_C(1) << length) - 1) << start;
    return (v & ~mask) | ((field << start) & mask);
}

#include "coldbranch-target.inc"

typedef uint64_t (*Gate)(uint64_t condition, uint64_t payload);

int main(void)
{
    const size_t mib = 1024 * 1024;
    const size_t arena_size = 256 * mib;
    const ptrdiff_t distances[] = {48 * 1024 * 1024, 96 * 1024 * 1024,
                                  -48 * 1024 * 1024, -96 * 1024 * 1024};
    const uint64_t conditions[] = {0, 1, UINT64_C(1) << 40};
    unsigned cases = 0;
    uint8_t *arena = mmap(NULL, arena_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    assert(arena != MAP_FAILED);
    alarm(5);
    for (unsigned d = 0; d < sizeof(distances) / sizeof(distances[0]); d++) {
        uint32_t *hot = (uint32_t *)(arena + 128 * mib);
        uint32_t *cold = (uint32_t *)((uint8_t *)hot + distances[d]);
        for (unsigned kind = 0; kind < 3; kind++) {
            unsigned branch;
            pthread_jit_write_protect_np(0);
            /* [D] QEMU v11.1.2 tcg/aarch64/tcg-target.c.inc : encodages.
             * B.ne, B.hi ou CBNZ vers le lent deviennent la condition inverse
             * sautant le B long. Le registre de lien n'est pas modifié. */
            hot[0] = 0xd503245f;         /* bti c : entrée depuis le C */
            if (kind < 2) {
                hot[1] = 0xf100001f;     /* cmp x0, #0 */
                hot[2] = 0x54000000 | (kind == 0 ? 0 : 9); /* b.eq / b.ls */
                assert(reloc_pc19(hot + 2, hot + 4));
                branch = 3;
            } else {
                hot[1] = 0xb4000000;     /* cbz x0 : inverse de cbnz */
                assert(reloc_pc19(hot + 1, hot + 3));
                branch = 2;
            }
            hot[branch] = 0x14000000;    /* b, surtout pas bl */
            assert(!reloc_pc19(hot + branch, cold)); /* ancienne portée insuffisante */
            assert(reloc_pc26(hot + branch, cold));
            hot[branch + 1] = 0x11002c20; /* add w0, w1, #11 */
            hot[branch + 2] = 0xd65f03c0; /* ret */
            cold[0] = 0xd503249f;        /* bti j */
            cold[1] = 0x11005820;        /* add w0, w1, #22 */
            cold[2] = 0xd65f03c0;        /* ret */
            sys_icache_invalidate(hot, 32);
            sys_icache_invalidate(cold, 16);
            pthread_jit_write_protect_np(1);
            for (unsigned c = 0; c < sizeof(conditions) / sizeof(conditions[0]); c++) {
                uint64_t expected = 77 + (conditions[c] ? 22 : 11);
                uint64_t got = ((Gate)hot)(conditions[c], 77);
                if (got != expected) {
                    fprintf(stderr, "porte %u distance %td condition %llu : %llu != %llu\n",
                            kind, distances[d], (unsigned long long)conditions[c],
                            (unsigned long long)got, (unsigned long long)expected);
                    return 1;
                }
                cases++;
            }
        }
    }
    assert(munmap(arena, arena_size) == 0);
    printf("M4 : %u portes exécutées, conditions vraies/fausses, argument et retour préservés\n", cases);
    return 0;
}
