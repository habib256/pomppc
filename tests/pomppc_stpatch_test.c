/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/* Le helper du plugin contre le VRAI décodeur, pas une seconde conversion. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "qgpu-core.h"
#include "../guest/gldriver/pomppc_stpatch.h"

static unsigned cases, takes, noops;
static uint32_t rng = 0x5632a419u;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static unsigned idx(unsigned off)
{
    if (off >= QGPU_SB_W3_OFF)
        return QGPU_SB_HDR + QGPU_SB_W0_N + QGPU_SB_W1_N + QGPU_SB_W2_N +
               (off - QGPU_SB_W3_OFF) / 4;
    return QGPU_SB_HDR + QGPU_SB_W0_N + (off - QGPU_SB_W1_OFF) / 4;
}

static void put16(uint32_t *a, unsigned off, uint32_t x)
{
    unsigned sh = 8 * (2 - (off & 2));
    a[idx(off)] = (a[idx(off)] & ~(0xffffu << sh)) | ((x & 0xffff) << sh);
}

static void known(uint32_t *a, unsigned step)
{
    static const uint32_t ops[] = { QGPU_SOP_ZERO, QGPU_SOP_KEEP, QGPU_SOP_REPLACE,
        QGPU_SOP_INCR, QGPU_SOP_DECR, QGPU_SOP_INVERT, QGPU_SOP_INCR_WRAP, QGPU_SOP_DECR_WRAP };
    put16(a, 0x3174, step % 3 == 0 ? 0x900 : step % 3 == 1 ? 0x901 : 0xffff);
    put16(a, 0x3176, step % 4 == 0 ? 0x404 : step % 4 == 1 ? 0x405 : step % 4 == 2 ? 0x408 : 0xffff);
    a[idx(0x3178)] = (a[idx(0x3178)] & ~0xff00u) | ((rnd() & 0xff) << 8);
    a[idx(0x2db4)] = rnd(); a[idx(0x2e38)] = rnd();
    a[idx(0x3198)] = rnd(); a[idx(0x319c)] = rnd();
    put16(a, 0x31a0, 0x200 + step % 8);
    put16(a, 0x31a2, ops[(step >> 3) % 8]);
    put16(a, 0x31a4, ops[(step >> 6) % 8]);
    put16(a, 0x31a6, ops[(step >> 9) % 8]);
    a[idx(0x31c0)] = step % 2 ? 0x80000001u : 0x80000000u;
    a[idx(0x31a8)] = rnd(); a[idx(0x31ac)] = rnd();
    a[idx(0x31b0)] = rnd(); a[idx(0x31b4)] = rnd();
    a[idx(0x31b8)] = rnd(); a[idx(0x31bc)] = rnd();
}

static int compare(const uint32_t *a, const uint32_t *b)
{
    QgpuState cur;
    uint32_t old[QGPU_SK_COUNT], ref[QGPU_SK_COUNT], got[QGPU_SK_COUNT];
    uint32_t saved[POMPPC_STPATCH_WORDS];
    PomppcStatePatch p;
    unsigned i;
    int take = pomppc_stpatch_make(a, b, &p);
    cases++;
    if (!take) return 0;
    takes++; noops += p.n == 0;
    memcpy(saved,a,sizeof(saved));
    pomppc_stpatch_save(saved,b);
    assert(!memcmp(saved,b,sizeof(saved)));
    qgpu_state_init(&cur);
    for (i = 0; i < QGPU_SK_COUNT; i++) cur.v[i] ^= rnd(); /* valeurs conservées adverses */
    qgpu_state_block_compute(a, &cur, old);
    memcpy(cur.v, old, sizeof(old));
    qgpu_state_block_compute(b, &cur, ref);
    memcpy(got, old, sizeof(got));
    for (i = 0; i < p.n; i++) {
        assert(p.key[i] > 0 && p.key[i] < QGPU_SK_COUNT);
        assert(i == 0 || p.key[i - 1] < p.key[i]);
        got[p.key[i]] = p.val[i];
    }
    if (memcmp(got, ref, sizeof(ref))) {
        for (i = 0; i < QGPU_SK_COUNT; i++)
            if (got[i] != ref[i])
                fprintf(stderr, "case %u key %u got %08x expected %08x\n", cases, i, got[i], ref[i]);
        assert(0);
    }
    return 1;
}

int main(void)
{
    uint32_t a[POMPPC_STPATCH_WORDS], b[POMPPC_STPATCH_WORDS];
    unsigned i, j, cap;
    memset(a, 0, sizeof(a));
    a[0] = QGPU_SB_F_VALID | QGPU_SB_F_RAW | QGPU_SB_F_V7 | QGPU_SB_F_V8 | QGPU_SB_F_STENCIL;
    a[1] = 640; a[2] = 480; a[3] = 8;
    known(a, 4095);
    memcpy(b, a, sizeof(b));
    assert(compare(a, b));
    /* Chaque bit brut à tour de rôle : accepté = exactement le vrai calcul,
       les changements étrangers (dont octets voisins du cull) sont refusés. */
    for (i = 0; i < POMPPC_STPATCH_WORDS; i++)
        for (j = 0; j < 32; j++) {
            memcpy(b, a, sizeof(b)); b[i] ^= 1u << j;
            compare(a, b);
        }
    memcpy(b, a, sizeof(b)); b[idx(0x3178)] ^= 0x01000000u; assert(!compare(a, b));
    memcpy(b, a, sizeof(b)); b[idx(0x3178)] ^= 1u; assert(!compare(a, b));
    memcpy(b, a, sizeof(b)); b[idx(0x31b8)] ^= 0xfedcba98u;
    b[idx(0x31bc)] ^= 0x12345678u;
    { PomppcStatePatch p; assert(pomppc_stpatch_make(a,b,&p)); assert(!p.n); }
    memcpy(b, a, sizeof(b)); put16(b,0x31a0,0xffff); assert(!compare(a,b));
    memcpy(b, a, sizeof(b)); put16(b,0x31a4,0xffff); assert(!compare(a,b));
    memcpy(b, a, sizeof(b)); b[0] ^= QGPU_SB_F_RAW; assert(!compare(a,b));
    memcpy(b, a, sizeof(b)); b[1]++; assert(!compare(a,b));
    memcpy(b, a, sizeof(b)); b[3] = 0; assert(!compare(a,b));
    a[0] &= ~QGPU_SB_F_VALID; memcpy(b,a,sizeof(b)); assert(!compare(a,b));
    a[0] |= QGPU_SB_F_VALID; a[0] &= ~QGPU_SB_F_V7;
    memcpy(b,a,sizeof(b)); assert(!compare(a,b));
    a[0] |= QGPU_SB_F_V7; a[1] = QGPU_MAX_SURF_DIM+1;
    memcpy(b,a,sizeof(b)); assert(!compare(a,b));
    /* Présence/absence de stencil, tous drapeaux de capacités, test coupé ou
       allumé, masques >8 bits, faces non canoniques et état extérieur NaN. */
    for (cap = 0; cap < 64; cap++) {
        for (j = 0; j < 2048; j++) {
            for (i = 0; i < POMPPC_STPATCH_WORDS; i++) a[i] = rnd();
            a[0] = QGPU_SB_F_VALID | QGPU_SB_F_V7 | ((cap & 1) ? QGPU_SB_F_RAW : 0) |
                   ((cap & 2) ? QGPU_SB_F_STENCIL : 0) | ((cap & 4) ? QGPU_SB_F_V8 : 0) |
                   ((cap & 8) ? QGPU_SB_F_TEX14 : 0) | ((cap & 16) ? QGPU_SB_F_PROG : 0) |
                   ((cap & 32) ? QGPU_SB_F_UNITS8 | QGPU_SB_F_GLSL : 0);
            a[1] = j % 4097; a[2] = (j * 43) % 4097; a[3] = j % 3 ? 8 : 0;
            known(a, j);
            memcpy(b, a, sizeof(b)); known(b, j ^ 1023);
            assert(compare(a,b));
        }
    }
    printf("STPATCH : %u transitions, %u deltas exacts, %u sans clé ; oracle qgpu_state_block_compute\n",
           cases, takes, noops);
    return 0;
}
