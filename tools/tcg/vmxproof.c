/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * vmxproof.c — preuve hôte de patches/tcg/0021-ppc-vmx-inline.patch
 * (propriété x-vmx-inline, docs/tcg-g4.md §28).
 *
 * Les helpers d'origine — helper_vsldoi, VMRG_DO (vmrghw, vmrglw), STVE
 * (stvebx, stvehx, stvewx) — sont EXTRAITS TELS QUELS de l'arbre par
 * vmxproof.sh ; la traduction en ligne de vmx-impl.c.inc est écrite ici en
 * C, UNE LIGNE PAR OP TCG, dans le même ordre et avec les mêmes constantes
 * (vmxproof.sh vérifie que l'arbre contient bien ces ops).  Sémantique des
 * ops (tcg/README) : extract2(al, ah, ofs) = (ah:al) >> ofs ; deposit(a1,
 * a2, ofs, len) remplace les bits [ofs, ofs+len) de a1 par ceux de a2 ;
 * movcond, shr, subfi, qemu_st de 1, 2 ou 4 octets (les octets de poids
 * faible).  Doubleword « haut » d'un AVR = VsrD(0).
 *
 *   vsldoi : chaque sh (16) x N couples aléatoires, plus vD = vA = vB ;
 *   vmrghw, vmrglw : N couples aléatoires et vD = vA, vD = vB ;
 *   stve[bhw]x : chaque taille x chaque adresse basse (16) x N vecteurs.
 * Compilé avec -DMUT=k (1..6) : une mutation du modèle, qui doit diverger.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MUT
#define MUT 0
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define HOST_BIG_ENDIAN 1
#define VsrB(i) u8[i]
#define VsrH(i) u16[i]
#define VsrW(i) u32[i]
#define VsrD(i) u64[i]
#define HI_IDX 0
#define LO_IDX 1
#else
#define HOST_BIG_ENDIAN 0
#define VsrB(i) u8[15 - (i)]
#define VsrH(i) u16[7 - (i)]
#define VsrW(i) u32[3 - (i)]
#define VsrD(i) u64[1 - (i)]
#define HI_IDX 1
#define LO_IDX 0
#endif
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
typedef union {
    uint8_t u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
} ppc_avr_t;
typedef struct { int dummy; } CPUPPCState;
typedef uint64_t target_ulong;

/* STVE : l'accès capture la valeur rangée (big-endian : pas d'échange) */
static uint64_t stored;
#define ppc_env_is_little_endian(env) false
#define GETPC() 0
#define cpu_stb_data_ra(env, a, v, ra) (stored = (uint8_t)(v))
#define cpu_stw_be_data_ra(env, a, v, ra) (stored = (uint16_t)(v))
#define cpu_stl_be_data_ra(env, a, v, ra) (stored = (uint32_t)(v))
#define bswap16 __builtin_bswap16
#define bswap32 __builtin_bswap32

#include "vmxproof-helpers.h"   /* helper_vsldoi, helper_vmrg*, helper_STVE*X */

/* ---- les ops TCG ---- */
static uint64_t extract2(uint64_t al, uint64_t ah, unsigned ofs)
{
    return (al >> ofs) | (ah << (64 - ofs));    /* 0 < ofs < 64 */
}

static uint64_t deposit(uint64_t a1, uint64_t a2, unsigned ofs, unsigned len)
{
    uint64_t m = (len == 64 ? ~0ull : (1ull << len) - 1) << ofs;
    return (a1 & ~m) | ((a2 << ofs) & m);
}

static uint64_t get_avr64(const ppc_avr_t *v, bool high)
{
    return high ? v->VsrD(0) : v->VsrD(1);
}

/* ---- le modèle de vmx-impl.c.inc (x-vmx-inline) ---- */
static void model_vsldoi(ppc_avr_t *vd, const ppc_avr_t *va,
                         const ppc_avr_t *vb, int vsh)
{
    int q = vsh >> 3, s = (vsh & 7) * 8;
    int n = s ? q + 3 : q + 2;
    uint64_t w[4] = { 0 }, rh, rl;

#if MUT == 1
    s = (vsh & 3) * 8;                          /* mutation : sh mod 4 */
#endif
    for (int i = q; i < n; i++) {
        w[i] = get_avr64(i < 2 ? va : vb, !(i & 1));
    }
    if (s == 0) {
        rh = w[q];
        rl = w[q + 1];
    } else {
#if MUT == 2
        rh = extract2(w[q + 1], w[q], s);       /* mutation : décalage */
#else
        rh = extract2(w[q + 1], w[q], 64 - s);
#endif
        rl = extract2(w[q + 2], w[q + 1], 64 - s);
    }
    vd->VsrD(0) = rh;
    vd->VsrD(1) = rl;
}

static void model_vmrgw(ppc_avr_t *vd, const ppc_avr_t *va,
                        const ppc_avr_t *vb, bool high)
{
    uint64_t a, b, rh, rl;

    a = get_avr64(va, high);
    b = get_avr64(vb, high);
#if MUT == 3
    rh = b >> 32;
    rh = deposit(a, rh, 32, 32);                /* mutation : moitié */
#else
    rh = b >> 32;
    rh = deposit(a, rh, 0, 32);
#endif
#if MUT == 4
    vd->VsrD(0) = rh;                           /* mutation : écrit avant */
    rl = deposit(get_avr64(vb, high), get_avr64(va, high), 32, 32);
#else
    rl = deposit(b, a, 32, 32);
#endif
    vd->VsrD(0) = rh;
    vd->VsrD(1) = rl;
}

/* la valeur rangée par la traduction en ligne de stve[bhw]x (EA masqué) */
static uint64_t model_stve(const ppc_avr_t *vs, uint64_t ea, int size)
{
    uint64_t d, lo, t;

    d = get_avr64(vs, true);
    lo = get_avr64(vs, false);
    t = ea & 8;
    d = t != 0 ? lo : d;                        /* movcond NE */
    t = ea & 7;
    t = t << 3;
#if MUT == 5
    t = 64 - 8 * 4 - t;                         /* mutation : taille fixe */
#else
    t = (64 - 8 * size) - t;                    /* subfi */
#endif
#if MUT == 6
    t &= 31;                                    /* mutation : décalage < 32 */
#endif
    d = d >> t;
    return size == 1 ? (uint8_t)d : size == 2 ? (uint16_t)d : (uint32_t)d;
}

/* ---- le banc ---- */
static uint64_t st = 0x5eed;
static uint64_t rnd(void)
{
    uint64_t z = (st += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static void rv(ppc_avr_t *v) { v->u64[0] = rnd(); v->u64[1] = rnd(); }

static uint64_t cases[3], bad[3];
static void count(int k, bool ko, const char *what)
{
    cases[k]++;
    if (ko && bad[k]++ < 3) {
        fprintf(stderr, "✘ %s\n", what);
    }
}

int main(int argc, char **argv)
{
    uint64_t n = argc > 1 ? strtoull(argv[1], 0, 0) : 1000000;
    CPUPPCState env;
    ppc_avr_t a, b, r1, r2, x;

    for (uint64_t i = 0; i < n; i++) {
        rv(&a);
        rv(&b);
        for (int sh = 0; sh < 16; sh++) {
            helper_vsldoi(&r1, &a, &b, sh);
            model_vsldoi(&r2, &a, &b, sh);
            count(0, memcmp(&r1, &r2, 16), "vsldoi");
            /* vD = vA = vB */
            x = a;
            helper_vsldoi(&r1, &x, &x, sh);
            model_vsldoi(&x, &x, &x, sh);
            count(0, memcmp(&r1, &x, 16), "vsldoi vD = vA = vB");
        }
        for (int h = 0; h < 2; h++) {
            if (h) {
                helper_vmrghw(&r1, &a, &b);
            } else {
                helper_vmrglw(&r1, &a, &b);
            }
            model_vmrgw(&r2, &a, &b, h);
            count(1, memcmp(&r1, &r2, 16), "vmrg");
            x = a;                              /* vD = vA */
            model_vmrgw(&x, &x, &b, h);
            count(1, memcmp(&r1, &x, 16), "vmrg vD = vA");
            x = b;                              /* vD = vB */
            model_vmrgw(&x, &a, &x, h);
            count(1, memcmp(&r1, &x, 16), "vmrg vD = vB");
        }
        for (int lo = 0; lo < 16; lo++) {
            uint64_t ea = (rnd() & ~15ull) | lo;
            /* EA masqué comme le fait do_ldst_ve_X / do_stve_X */
            helper_STVEBX(&env, &a, ea);
            count(2, stored != model_stve(&a, ea, 1), "stvebx");
            helper_STVEHX(&env, &a, ea & ~1ull);
            count(2, stored != model_stve(&a, ea & ~1ull, 2), "stvehx");
            helper_STVEWX(&env, &a, ea & ~3ull);
            count(2, stored != model_stve(&a, ea & ~3ull, 4), "stvewx");
        }
    }
    printf("vsldoi : %llu cas, %llu divergences\n",
           (unsigned long long)cases[0], (unsigned long long)bad[0]);
    printf("vmrghw/vmrglw : %llu cas, %llu divergences\n",
           (unsigned long long)cases[1], (unsigned long long)bad[1]);
    printf("stvebx/stvehx/stvewx : %llu cas, %llu divergences\n",
           (unsigned long long)cases[2], (unsigned long long)bad[2]);
    return bad[0] + bad[1] + bad[2] ? 1 : 0;
}
