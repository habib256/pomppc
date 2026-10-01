/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * fprun.c — greffon TCG : longueur des « suites » de flottant scalaire que le
 * G4 émulé exécute (docs/tcg-g4.md §22).
 *
 * Question : si le flottant scalaire simple (fadds fsubs fmuls fmadds fmsubs
 * fnmadds fnmsubs, fcmpu) était traduit sans branchement par instruction,
 * avec une seule vérification par suite, combien d'instructions flottantes
 * une suite contiendrait-elle, pondérée par l'exécution ?  Trois définitions
 * de la suite, de la plus stricte à la plus large :
 *
 *   s1  instructions flottantes consécutives (fmr/fneg/fabs/fnabs admis) ;
 *   s2  … séparées seulement par du calcul entier (ni accès mémoire, ni
 *       branchement, ni sc/rfi/isync/mtmsr/mtspr) ;
 *   s3  … séparées aussi par des LECTURES en mémoire (lfs, lwz… : une suite
 *       ne s'arrête qu'aux écritures, aux branchements et au système).
 *
 * Chaque suite compte k instructions flottantes « candidates » (Rc = 0) ;
 * une par suite ajoute 1 à l'histogramme[k] de sa définition (compteur en
 * ligne sur sa première instruction, par vCPU).  Un fil écrit l'instantané
 * cumulé toutes les `interval` secondes : « T <s> » puis « <déf> <k> <n> »
 * (k plafonné à 63), plus « op <clé> <n> » : le nombre d'exécutions de
 * chaque candidate (0..7 = fadds…fcmpu).  tools/tcg/fprun.py résume.
 *
 *   qemu-system-ppc64 … -plugin tools/tcg/libfprun.dylib,out=/tmp/run.txt,interval=10
 *
 * Construction : QEMU_SRC=~/src/qemu-fpnat tools/tcg/build.sh fprun
 * GPL-2.0-or-later (s'appuie sur l'API de greffons de QEMU).
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define NDEF 3
#define NK   64
#define NOPS 8

typedef struct {
    uint64_t h[NDEF][NK];
    uint64_t op[NOPS];
} Counts;

static struct qemu_plugin_scoreboard *sb;
static char *out_path;
static unsigned interval = 10;
static struct timespec t0;

enum { C_FP, C_MOVE, C_ALU, C_LOAD, C_STORE, C_STOP };

/* candidate : 0..6 = fadds fsubs fmuls fmadds fmsubs fnmadds fnmsubs, 7 = fcmpu, -1 sinon */
static int candidate(uint32_t w)
{
    unsigned op = w >> 26, rc = w & 1;
    if (op == 59 && !rc) {
        switch ((w >> 1) & 0x1f) {
        case 21: return 0;  /* fadds */
        case 20: return 1;  /* fsubs */
        case 25: return 2;  /* fmuls */
        case 29: return 3;  /* fmadds */
        case 28: return 4;  /* fmsubs */
        case 31: return 5;  /* fnmadds */
        case 30: return 6;  /* fnmsubs */
        }
    }
    if (op == 63 && ((w >> 1) & 0x3ff) == 0) {
        return 7;           /* fcmpu */
    }
    return -1;
}

static int classify(uint32_t w)
{
    unsigned op = w >> 26, xo = (w >> 1) & 0x3ff;
    if (candidate(w) >= 0) {
        return C_FP;
    }
    if (op == 63 && !(w & 1) && (xo == 72 || xo == 40 || xo == 264 || xo == 136)) {
        return C_MOVE;      /* fmr fneg fabs fnabs */
    }
    if (op >= 32 && op <= 55) {
        /* lwz..lhau, lmw, lfs..lfdu : lectures ; stw.., stmw, stfs.. : écritures */
        static const unsigned char st[] = { 36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55 };
        for (unsigned i = 0; i < sizeof(st); i++) {
            if (op == st[i]) {
                return C_STORE;
            }
        }
        return C_LOAD;
    }
    if (op == 31) {
        switch (xo) {
        /* lectures indexées (entier, flottant, AltiVec, lwarx) */
        case 23: case 55: case 87: case 119: case 279: case 311: case 343: case 375:
        case 534: case 790: case 535: case 567: case 599: case 631: case 20:
        case 7: case 39: case 71: case 103: case 359: case 6: case 38:
        case 533: case 597:
            return C_LOAD;
        /* écritures indexées, stwcx., dcbz, dcbst/icbi… (effets) */
        case 151: case 183: case 215: case 247: case 407: case 439: case 662:
        case 918: case 663: case 695: case 727: case 759: case 983: case 150:
        case 135: case 167: case 199: case 231: case 487: case 1014: case 661:
        case 725: case 54: case 982: case 86: case 470:
            return C_STORE;
        case 146: case 210: case 242: case 306: case 370: case 467: case 566:
        case 598: case 854: case 4:
            return C_STOP;  /* mtmsr mtsr mtsrin tlbie tlbia mtspr tlbsync sync eieio tw */
        }
        return C_ALU;
    }
    if (op == 16 || op == 17 || op == 18 || op == 19 || op == 3 || op == 2) {
        return C_STOP;      /* branchements, sc, rfi/isync, twi */
    }
    if (op == 59 || op == 63) {
        return C_STOP;      /* autre flottant (helper, peut toucher le FPSCR) */
    }
    return C_ALU;           /* entier, AltiVec */
}

/* une définition : breaks[c] = 1 si la classe c termine la suite */
static const unsigned char breaks[NDEF][6] = {
    /*        FP MOVE ALU LOAD STORE STOP */
    /* s1 */ { 0, 0,   1,  1,   1,    1 },
    /* s2 */ { 0, 0,   0,  1,   1,    1 },
    /* s3 */ { 0, 0,   0,  0,   1,    1 },
};

static void add_inline(struct qemu_plugin_insn *insn, size_t off)
{
    qemu_plugin_u64 e = { .score = sb, .offset = off };
    qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
        insn, QEMU_PLUGIN_INLINE_ADD_U64, e, 1);
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    struct qemu_plugin_insn *first[NDEF] = { NULL };
    unsigned k[NDEF] = { 0 };

    for (size_t i = 0; i <= n; i++) {
        struct qemu_plugin_insn *insn = NULL;
        int c = C_STOP;
        uint32_t w = 0;
        if (i < n) {
            uint8_t b[4] = { 0 };
            insn = qemu_plugin_tb_get_insn(tb, i);
            if (qemu_plugin_insn_data(insn, b, 4) == 4) {
                w = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
                c = classify(w);
            }
        }
        if (c == C_FP) {
            add_inline(insn, offsetof(Counts, op) + candidate(w) * 8);
        }
        for (int d = 0; d < NDEF; d++) {
            if (c == C_FP) {
                if (!k[d]) {
                    first[d] = insn;
                }
                k[d]++;
            } else if (breaks[d][c] && k[d]) {
                unsigned kk = k[d] < NK ? k[d] : NK - 1;
                add_inline(first[d], offsetof(Counts, h) + (d * NK + kk) * 8);
                k[d] = 0;
            }
        }
    }
}

static void dump(FILE *f)
{
    struct timespec t;
    int nv = qemu_plugin_num_vcpus();
    clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(f, "T %.3f\n", (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9);
    for (unsigned i = 0; i < sizeof(Counts) / 8; i++) {
        uint64_t s = 0;
        qemu_plugin_u64 e = { .score = sb, .offset = i * 8 };
        for (int v = 0; v < nv; v++) {
            s += qemu_plugin_u64_get(e, v);
        }
        if (!s) {
            continue;
        }
        if (i < NDEF * NK) {
            fprintf(f, "s%u %u %" PRIu64 "\n", i / NK + 1, i % NK, s);
        } else {
            fprintf(f, "op %u %" PRIu64 "\n", i - NDEF * NK, s);
        }
    }
    fflush(f);
}

static void *dumper(void *arg)
{
    for (;;) {
        sleep(interval);
        dump(arg);
    }
    return NULL;
}

static FILE *outf;

static void at_exit(qemu_plugin_id_t id, void *p)
{
    if (outf) {
        dump(outf);
        fprintf(outf, "END\n");
        fflush(outf);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "out=", 4)) {
            out_path = strdup(argv[i] + 4);
        } else if (!strncmp(argv[i], "interval=", 9)) {
            interval = atoi(argv[i] + 9);
        } else {
            fprintf(stderr, "fprun : option inconnue %s\n", argv[i]);
            return -1;
        }
    }
    if (!out_path || !(outf = fopen(out_path, "w"))) {
        fprintf(stderr, "fprun : out=<fichier> requis\n");
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    sb = qemu_plugin_scoreboard_new(sizeof(Counts));
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    if (interval) {
        pthread_t th;
        pthread_create(&th, NULL, dumper, outf);
        pthread_detach(th);
    }
    return 0;
}
