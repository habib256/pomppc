/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
#ifndef POMPPC_STPATCH_H
#define POMPPC_STPATCH_H

#include <stdint.h>
#include <string.h>
#include "qgpu_proto.h"

/* Optimisation du contrat logiciel STATE_BLOCK : source du calcul canonique,
 * qgpu_state_block_compute dans patches/qgpu/qgpu-core.c. Aucun nouvel opcode.
 * Les entrées sont les arguments du bloc, en mots déjà gros-boutistes lus
 * comme entiers, comme le décodeur (pas les octets du flux sur un hôte LE). */
#define POMPPC_STPATCH_WORDS (QGPU_SB_HDR + QGPU_SB_WORDS)
#define POMPPC_STPATCH_KEYS 12
#define STP_W1(off) (QGPU_SB_HDR + QGPU_SB_W0_N + ((off) - QGPU_SB_W1_OFF) / 4)
#define STP_W3(off) (QGPU_SB_HDR + QGPU_SB_W0_N + QGPU_SB_W1_N + QGPU_SB_W2_N + \
                    ((off) - QGPU_SB_W3_OFF) / 4)

typedef struct PomppcStatePatch {
    unsigned n, why;                    /* refus : 1 en-tête, 2 autre champ, 3 enum */
    uint32_t key[POMPPC_STPATCH_KEYS], val[POMPPC_STPATCH_KEYS];
} PomppcStatePatch;

static uint32_t stp_word(const uint32_t *a, unsigned off)
{
    return a[off >= QGPU_SB_W3_OFF ? STP_W3(off) : STP_W1(off)];
}

static uint32_t stp_u16(const uint32_t *a, unsigned off)
{
    return (stp_word(a, off) >> (8 * (2 - (off & 2)))) & 0xffff;
}

static int stp_op_valid(uint32_t op)
{
    return op == QGPU_SOP_ZERO || op == QGPU_SOP_INVERT || op == QGPU_SOP_KEEP ||
           op == QGPU_SOP_REPLACE || op == QGPU_SOP_INCR || op == QGPU_SOP_DECR ||
           op == QGPU_SOP_INCR_WRAP || op == QGPU_SOP_DECR_WRAP;
}

/* Même canonicité que le décodeur : quand le test est coupé, référence nulle,
 * ALWAYS, masque ff, KEEP ; write-mask et clear restent lus. Les mots de
 * stencil arrière 31a8..31bc ne sont PAS lus par STATE_BLOCK et ne produisent
 * donc aucune clé. Ils peuvent changer sans empêcher cette voie. */
static int stp_values(const uint32_t *a, uint32_t *v)
{
    uint32_t cull = stp_u16(a, 0x3176);
    int on = (a[0] & QGPU_SB_F_STENCIL) && a[3] && (stp_word(a, 0x31c0) & 1);
    v[0] = on != 0;
    v[1] = on ? stp_u16(a, 0x31a0) : 0x0207;
    v[2] = on ? stp_word(a, 0x319c) & 0xff : 0;
    v[3] = on ? stp_word(a, 0x3198) & 0xff : 0xff;
    v[4] = stp_word(a, 0x2e38) & 0xff;
    v[5] = on ? stp_u16(a, 0x31a2) : QGPU_SOP_KEEP;
    v[6] = on ? stp_u16(a, 0x31a4) : QGPU_SOP_KEEP;
    v[7] = on ? stp_u16(a, 0x31a6) : QGPU_SOP_KEEP;
    v[8] = stp_word(a, 0x2db4) & 0xff;
    v[9] = ((stp_word(a, 0x3178) >> 8) & 0xff) != 0;
    v[10] = cull == 0x0404 || cull == 0x0405 || cull == 0x0408 ? cull : 0x0405;
    v[11] = stp_u16(a, 0x3174) == 0x0900 ? 0x0900 : 0x0901;
    /* Une valeur de stencil que SET_STATE refuserait garde la voie complète,
       même si elle n'a pas changé : ne pas escamoter le refus du bloc. */
    return v[1] >= 0x0200 && v[1] <= 0x0207 &&
           stp_op_valid(v[5]) && stp_op_valid(v[6]) && stp_op_valid(v[7]);
}

/* 1 : les seules différences brutes sont celles ci-dessous ; les clés
 * produites sont triées comme sb_ranges et ne touchent aucune clé chaude.
 * 0 : même contrat que la voie d'avant, bloc entier obligatoire. */
static int pomppc_stpatch_make(const uint32_t *prev, const uint32_t *next,
                              PomppcStatePatch *p)
{
    static const uint32_t key[POMPPC_STPATCH_KEYS] = {
        QGPU_SK_STENCIL_TEST, QGPU_SK_STENCIL_FUNC, QGPU_SK_STENCIL_REF,
        QGPU_SK_STENCIL_VALUE_MASK, QGPU_SK_STENCIL_WRITE_MASK,
        QGPU_SK_STENCIL_OP_FAIL, QGPU_SK_STENCIL_OP_ZFAIL, QGPU_SK_STENCIL_OP_ZPASS,
        QGPU_SK_STENCIL_CLEAR, QGPU_SK_CULL_FACE, QGPU_SK_CULL_MODE, QGPU_SK_FRONT_FACE
    };
    uint32_t oldv[POMPPC_STPATCH_KEYS], newv[POMPPC_STPATCH_KEYS];
    unsigned i;
    p->n = 0; p->why = 0;
    if (!(prev[0] & QGPU_SB_F_VALID) || !(prev[0] & QGPU_SB_F_V7) ||
        (prev[0] & ~(uint32_t)QGPU_SB_F_ALL) || prev[1] > QGPU_MAX_SURF_DIM ||
        prev[2] > QGPU_MAX_SURF_DIM || prev[0] != next[0] || prev[1] != next[1] ||
        prev[2] != next[2] || prev[3] != next[3])
        { p->why = 1; return 0; }
    /* Cinq plages contiguës au lieu d'une boucle masquée sur 108 mots du
       G4. Entre elles : clear, write-mask, faces, stencil avant/arrière. */
    if (memcmp(prev + QGPU_SB_HDR, next + QGPU_SB_HDR,
               (STP_W1(0x2db4) - QGPU_SB_HDR) * 4) ||
        memcmp(prev + STP_W1(0x2db4) + 1, next + STP_W1(0x2db4) + 1,
               (STP_W1(0x2e38) - STP_W1(0x2db4) - 1) * 4) ||
        memcmp(prev + STP_W1(0x2e38) + 1, next + STP_W1(0x2e38) + 1,
               (STP_W3(0x3174) - STP_W1(0x2e38) - 1) * 4) ||
        ((prev[STP_W3(0x3178)] ^ next[STP_W3(0x3178)]) & 0xffff00ffu) ||
        memcmp(prev + STP_W3(0x3178) + 1, next + STP_W3(0x3178) + 1,
               (STP_W3(0x3198) - STP_W3(0x3178) - 1) * 4) ||
        memcmp(prev + STP_W3(0x31c0) + 1, next + STP_W3(0x31c0) + 1,
               (POMPPC_STPATCH_WORDS - STP_W3(0x31c0) - 1) * 4))
        { p->why = 2; return 0; }
    if (!stp_values(prev, oldv) || !stp_values(next, newv))
        { p->why = 3; return 0; }
    for (i = 0; i < POMPPC_STPATCH_KEYS; i++)
        if (oldv[i] != newv[i]) {
            p->key[p->n] = key[i];
            p->val[p->n++] = newv[i];
        }
    return 1;
}

/* Après une décision positive, seuls ces 15 mots peuvent avoir changé.
 * Le snapshot reste exact sans recopier les 432 octets du bloc entier. */
static void pomppc_stpatch_save(uint32_t *prev, const uint32_t *next)
{
    prev[STP_W1(0x2db4)] = next[STP_W1(0x2db4)];
    prev[STP_W1(0x2e38)] = next[STP_W1(0x2e38)];
    prev[STP_W3(0x3174)] = next[STP_W3(0x3174)];
    prev[STP_W3(0x3178)] = next[STP_W3(0x3178)];
    memcpy(prev + STP_W3(0x3198), next + STP_W3(0x3198),
           (STP_W3(0x31c0) - STP_W3(0x3198) + 1) * 4);
}

#undef STP_W1
#undef STP_W3
#endif
