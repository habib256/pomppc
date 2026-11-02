/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/* Harnais des fonctions extraites de QEMU : relais, pools et restauration. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>

typedef uint32_t tcg_insn_unit;
typedef uint64_t tcg_target_ulong;
typedef struct TCGLabelQemuLdst {
    tcg_insn_unit *label_ptr[2];
    const tcg_insn_unit *raddr;
    struct TCGLabelQemuLdst *next;
} TCGLabelQemuLdst;
typedef struct TCGLabelPoolData {
    struct TCGLabelPoolData *next;
    tcg_insn_unit *label;
    intptr_t addend;
    int rtype;
    unsigned nlong;
    tcg_target_ulong data[1];
} TCGLabelPoolData;
typedef struct TCGContext {
    tcg_insn_unit *code_ptr;
    void *code_gen_ptr, *code_gen_highwater, *data_gen_ptr;
    void *code_cold_ptr, *code_cold_highwater;
    TCGLabelPoolData *pool_labels;
    struct { TCGLabelQemuLdst *first; } ldst_labels;
} TCGContext;

/* Les primitives de liste/écriture et le callback remplacé sont l'interface
 * du harnais ; les algorithmes de finalisation/relocation viennent du source. */
#define QSIMPLEQ_FOREACH(v, head, field) \
    for ((v) = (head)->first; (v); (v) = (v)->field)
#define QSIMPLEQ_EMPTY(head) (!(head)->first)
#define TCG_TARGET_COLD_VENEERS 1
#define unlikely(x) (x)
#define qatomic_set(p, v) (*(p) = (v))
#define tcg_ptr_byte_diff(a, b) ((char *)(a) - (char *)(b))
#define ROUND_UP(v, a) (((v) + (a) - 1) & ~((a) - 1))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define DIV_ROUND_UP(v, a) (((v) + (a) - 1) / (a))
#define MiB (1024u * 1024u)
static intptr_t rx_diff;
#define tcg_splitwx_to_rx(p) ((tcg_insn_unit *)((uintptr_t)(p) + rx_diff))
#define B 0x14000000u
#define BL 0x94000000u
#define tcg_out_insn(s, type, opcode, offset) \
    (*(s)->code_ptr++ = (opcode) | ((offset) & 0x03ffffffu))

static int64_t sextract64(uint64_t v, int start, int length)
{
    return (int64_t)(v << (64 - start - length)) >> (64 - length);
}
static uint32_t deposit32(uint32_t v, int start, int length, uint32_t field)
{
    uint32_t mask = ((UINT32_C(1) << length) - 1) << start;
    return (v & ~mask) | ((field << start) & mask);
}
#include "reloc.inc"
#include "regions.inc"

static unsigned flushes;
static size_t flushed_size;
static void flush_idcache_range(uintptr_t rx, uintptr_t rw, size_t size)
{
    assert(rx == rw); /* exécution native MAP_JIT, alias arithmétique testé à part */
    flushes++;
    flushed_size = size;
    sys_icache_invalidate((void *)rx, size);
}
static void tcg_out_nop_fill(tcg_insn_unit *p, int n)
{
    while (n-- > 0) { *p++ = 0xd503201f; }
}
static int fail_callback, fail_reloc;
static TCGLabelPoolData cold_pool;
static const uint64_t cold_value = UINT64_C(0xabcdef0123456789);
static bool patch_reloc(tcg_insn_unit *label, int type, uintptr_t v, intptr_t a)
{
    assert(type == 1 && a == 0);
    return !fail_reloc && reloc_pc19(label, (const tcg_insn_unit *)v);
}
static int tcg_out_ldst_finalize(TCGContext *s)
{
    TCGLabelQemuLdst *l = s->ldst_labels.first;
    assert(s->pool_labels == NULL && s->data_gen_ptr == NULL);
    if (!reloc_ldst(l->label_ptr[0], tcg_splitwx_to_rx(s->code_ptr))) {
        return -2;
    }
    memset(&cold_pool, 0, sizeof(cold_pool));
    cold_pool.nlong = 1;
    cold_pool.rtype = 1;
    cold_pool.data[0] = cold_value;
    cold_pool.label = s->code_ptr;
    s->pool_labels = &cold_pool;
    *s->code_ptr++ = 0x58000000; /* ldr x0, pool : véritable accès littéral */
    *s->code_ptr++ = 0xca010000; /* eor x0, x0, x1 : argument préservé */
    *s->code_ptr = 0x14000000;
    assert(reloc_pc26(s->code_ptr, l->raddr));
    s->code_ptr++;
    return fail_callback;
}
#include "finalize.inc"

typedef uint64_t (*Gate)(uint64_t, uint64_t);
int main(void)
{
    const size_t arena_size = 256 * MiB;
    const ptrdiff_t distances[] = {48 * MiB, 96 * MiB,
                                  -(ptrdiff_t)(48 * MiB), -(ptrdiff_t)(96 * MiB)};
    const uint64_t conditions[] = {0, 1, UINT64_C(1) << 40};
    unsigned cases = 0;
    uint8_t *arena = mmap(NULL, arena_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    assert(arena != MAP_FAILED);
    alarm(10);
    for (unsigned d = 0; d < 4; d++) {
        for (unsigned kind = 0; kind < 4; kind++) {
            uint32_t *hot = (uint32_t *)(arena + 128 * MiB);
            uint32_t *cold = (uint32_t *)((char *)hot + distances[d]);
            TCGLabelQemuLdst l = {.label_ptr = {hot + 2}, .raddr = hot + 4};
            TCGLabelPoolData hot_pool = {.label = hot + 20, .nlong = 1,
                                        .rtype = 1, .data = {0x8765432101234567}};
            TCGContext s = {.code_ptr = hot + 5, .code_gen_highwater = hot + 64,
                .code_cold_ptr = cold, .code_cold_highwater = cold + 64,
                .data_gen_ptr = hot + 24, .pool_labels = &hot_pool,
                .ldst_labels = {&l}};
            pthread_jit_write_protect_np(0);
            hot[0] = 0xd503245f; /* bti c */
            hot[1] = 0xf100001f; /* cmp x0, #0 */
            hot[2] = kind < 2 ? 0x54000000 | (kind ? 8 : 1) :
                     kind == 2 ? 0xb5000000 : 0x35000000; /* ne, hi, cbnz x/w */
            hot[3] = 0xaa0103e0; /* mov x0, x1 */
            hot[4] = 0xd65f03c0; /* ret, raddr du corps chaud */
            hot[20] = 0x58000001; /* référence du pool chaud, hors chemin exécuté */
            unsigned before = flushes;
            assert(tcg_out_cold_finalize(&s) == 0);
            assert(s.code_ptr == hot + 6 && s.code_cold_ptr > (void *)(cold + 2));
            assert(s.pool_labels == &hot_pool && s.data_gen_ptr == hot + 24);
            assert(s.code_gen_highwater == hot + 64 && l.raddr == hot + 4);
            assert(flushes == before + 1 && flushed_size >= 16);
            assert(tcg_out_pool_finalize(&s) == 0);
            assert(*(uint64_t *)s.data_gen_ptr == hot_pool.data[0]);
            sys_icache_invalidate(hot, 128);
            pthread_jit_write_protect_np(1);
            for (unsigned c = 0; c < 3; c++) {
                bool taken = kind == 3 ? (uint32_t)conditions[c] != 0 : conditions[c] != 0;
                uint64_t payload = UINT64_C(0x123456789abcdef0);
                uint64_t expected = taken ? cold_value ^ payload : payload;
                uint64_t got = ((Gate)hot)(conditions[c], payload);
                assert(got == expected);
                cases++;
            }
        }
    }
    pthread_jit_write_protect_np(0);
    for (int failure = 0; failure < 5; failure++) {
        uint32_t *hot = (uint32_t *)(arena + 128 * MiB);
        uint32_t *cold = hot + MiB;
        TCGLabelQemuLdst l = {.label_ptr = {hot}, .raddr = hot + 4};
        TCGLabelPoolData hot_pool = {0};
        TCGContext s = {.code_ptr = hot + 5, .code_gen_highwater = hot + 64,
            .code_cold_ptr = cold, .code_cold_highwater = cold + 64,
            .data_gen_ptr = hot + 24, .pool_labels = &hot_pool,
            .ldst_labels = {&l}};
        hot[0] = 0x54000001;
        unsigned before = flushes;
        fail_callback = failure == 0 ? -2 : failure == 1 ? -1 : 0;
        fail_reloc = failure == 2;
        if (failure == 3) { s.code_cold_highwater = cold + 1; }
        if (failure == 4) { s.code_gen_highwater = hot + 5; }
        int expected = failure == 0 || failure == 2 ? -2 : -1;
        assert(tcg_out_cold_finalize(&s) == expected);
        if (failure == 4) {
            assert(s.code_ptr == hot + 5 && s.pool_labels == &hot_pool);
            assert(s.code_gen_ptr == hot + 5);
            cases++;
            continue;
        }
        assert(s.code_ptr == hot + 6 && s.code_cold_ptr == cold);
        assert(s.pool_labels == &hot_pool && s.data_gen_ptr == hot + 24);
        assert(s.code_gen_highwater == hot + 64 && flushes == before);
        if (expected == -1) { assert(s.code_gen_ptr == hot + 64); }
        cases++;
    }
    fail_callback = 0;
    fail_reloc = 0;
    /* La différence RX/RW est appliquée à la source, pour imm19 et imm26. */
    uint32_t label;
    rx_diff = 16 * MiB;
    label = 0x14000000;
    assert(reloc_ldst(&label, (void *)((uintptr_t)&label + rx_diff + 96 * MiB)));
    assert(sextract64(label, 0, 26) * 4 == 96 * MiB);
    label = 0xb5000000;
    assert(reloc_ldst(&label, (void *)((uintptr_t)&label + rx_diff - 64)));
    assert(sextract64(label, 5, 19) * 4 == -64);
    rx_diff = 0;
    assert(tcg_cold_region_count(1024 * MiB, 1) == 16);
    assert(tcg_cold_region_count(65 * MiB, 1) == 2);
    assert(tcg_cold_region_count(1024 * MiB, 32) == 32);
    pthread_jit_write_protect_np(1);
    assert(munmap(arena, arena_size) == 0);
    printf("M4 : %u cas, relais et pools exécutés ; erreurs, alias RX/RW, SMP=1 couverts\n", cases);
    return 0;
}
