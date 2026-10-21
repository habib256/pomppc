/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * tbedges.c — greffon TCG : profil des blocs ET de leurs enchaînements
 * (phase 0 du traducteur de second niveau, docs/plan-traducteur-rapide.md §5).
 *
 * Par bloc (clé : adresse virtuelle, adresse hôte de la première instruction
 * — donc la page physique, ce qui sépare deux processus au même pc —, nombre
 * d'instructions) : exécutions, instructions invité, mot de la dernière
 * instruction (classe de la sortie : bclr/bcctr/rfi/sc = indirecte).
 * Par arête (bloc précédent du même vCPU → bloc courant) : nombre de passages.
 * Toutes les transitions sont vues, chaînées (goto_tb), par le cache de sauts
 * ou par cpu_exec, interruptions comprises.
 *
 * Coût : un appel par bloc exécuté (≈ ×2-3 sur le vCPU) : mesure de FORME,
 * jamais de vitesse. Un greffon présent coupe aussi x-ret-inline.
 *
 *   -plugin tools/tcg/libtbedges.so,out=/tmp/e[,snap=/tmp/e.snap][,interval=30,keep=8]
 *
 * Sorties : <out>.blocks.csv (slot,pc,phys,insns,lastw,execs) et
 * <out>.edges.csv (src,dst,count) à la sortie de QEMU ; avec snap=FICHIER,
 * l'apparition de FICHIER écrit un instantané cumulé <out>.<n>.{blocks,edges}.csv
 * (tools/tcg/regions.py fait la différence de deux instantanés).
 * Construction : QEMU_SRC=~/src/qemu tools/tcg/build.sh tbedges
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

enum { LIMIT = 1 << 21, HBITS = 22, EBITS = 23, MAXV = 16 };

typedef struct {
    uint64_t pc, phys;
    uint32_t insns, lastw;
} Block;

typedef struct {
    uint64_t key;               /* (src + 1) << 32 | (dst + 1) ; 0 = vide */
    uint64_t n;
} Edge;

typedef struct {
    uint32_t last;              /* slot + 1 du bloc précédent, 0 = aucun */
    uint64_t lost;              /* arêtes perdues (table pleine) */
    uint64_t *execs;            /* [LIMIT] */
    Edge *edges;                /* [1 << EBITS] */
} Vcpu;

static Block blocks[LIMIT];
static uint32_t hidx[1u << HBITS];
static uint32_t used;
static uint64_t skipped;
static Vcpu vc[MAXV];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char *out, *snap;
static unsigned nsnap, interval, keep = 8;

static void on_exec(unsigned int vcpu, void *udata)
{
    uint32_t cur = (uint32_t)(uintptr_t)udata;      /* slot + 1 */
    Vcpu *v;
    if (vcpu >= MAXV) {
        return;
    }
    v = &vc[vcpu];
    v->execs[cur - 1]++;
    if (v->last && v->edges) {
        uint64_t key = (uint64_t)v->last << 32 | cur;
        uint64_t h = (key * 0x9E3779B97F4A7C15ULL) >> (64 - EBITS);
        for (unsigned probe = 0; probe < 64; probe++) {
            Edge *e = &v->edges[h];
            if (e->key == key) {
                e->n++;
                goto done;
            }
            if (!e->key) {
                e->n = 1;
                __atomic_store_n(&e->key, key, __ATOMIC_RELEASE);
                goto done;
            }
            h = (h + 1) & ((1u << EBITS) - 1);
        }
        v->lost++;
    }
done:
    v->last = cur;
}

static void translate(struct qemu_plugin_tb *tb, void *id)
{
    uint64_t pc = qemu_plugin_tb_vaddr(tb);
    size_t ni = qemu_plugin_tb_n_insns(tb);
    struct qemu_plugin_insn *first = qemu_plugin_tb_get_insn(tb, 0);
    struct qemu_plugin_insn *lasti = qemu_plugin_tb_get_insn(tb, ni - 1);
    uint64_t phys = (uint64_t)(uintptr_t)qemu_plugin_insn_haddr(first);
    uint8_t b[4] = { 0 };
    uint32_t lastw = 0, slot;
    (void)id;
    if (qemu_plugin_insn_data(lasti, b, 4) == 4) {
        lastw = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
    }
    pthread_mutex_lock(&lock);
    uint32_t h = (uint32_t)(((pc ^ (phys << 7)) * 0x9E3779B97F4A7C15ULL + ni)
                            >> (64 - HBITS));
    while (hidx[h]) {
        Block *k = &blocks[hidx[h] - 1];
        if (k->pc == pc && k->phys == phys && k->insns == ni) {
            break;
        }
        h = (h + 1) & ((1u << HBITS) - 1);
    }
    if (hidx[h]) {
        slot = hidx[h] - 1;
    } else if (used == LIMIT) {
        skipped++;
        pthread_mutex_unlock(&lock);
        return;
    } else {
        slot = used;
        blocks[slot] = (Block){ pc, phys, (uint32_t)ni, lastw };
        hidx[h] = slot + 1;
        __atomic_store_n(&used, used + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lock);
    qemu_plugin_register_vcpu_tb_exec_cb(tb, on_exec, QEMU_PLUGIN_CB_NO_REGS,
                                         (void *)(uintptr_t)(slot + 1));
}

static void dump(const char *base)
{
    char path[4096];
    uint32_t n = __atomic_load_n(&used, __ATOMIC_ACQUIRE);
    uint64_t lost = 0;
    FILE *f;

    snprintf(path, sizeof(path), "%s.blocks.csv", base);
    f = fopen(path, "w");
    if (!f) {
        perror(path);
        return;
    }
    for (int i = 0; i < MAXV; i++) {
        lost += vc[i].lost;
    }
    {
        struct timespec tw, tm;
        clock_gettime(CLOCK_REALTIME, &tw);
        clock_gettime(CLOCK_MONOTONIC, &tm);
        fprintf(f, "# skipped_translations=%" PRIu64 " lost_edges=%" PRIu64
                " t_wall=%.3f t_mono=%.3f\nslot,pc,phys,insns,lastw,execs\n",
                skipped, lost, tw.tv_sec + tw.tv_nsec * 1e-9,
                tm.tv_sec + tm.tv_nsec * 1e-9);
    }
    for (uint32_t s = 0; s < n; s++) {
        uint64_t e = 0;
        for (int i = 0; i < MAXV; i++) {
            if (vc[i].execs) {
                e += vc[i].execs[s];
            }
        }
        fprintf(f, "%u,0x%" PRIx64 ",0x%" PRIx64 ",%u,0x%08x,%" PRIu64 "\n", s,
                blocks[s].pc, blocks[s].phys, blocks[s].insns, blocks[s].lastw, e);
    }
    fclose(f);

    snprintf(path, sizeof(path), "%s.edges.csv", base);
    f = fopen(path, "w");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "src,dst,count\n");
    for (int i = 0; i < MAXV; i++) {
        if (!vc[i].edges) {
            continue;
        }
        for (uint32_t k = 0; k < (1u << EBITS); k++) {
            uint64_t key = __atomic_load_n(&vc[i].edges[k].key, __ATOMIC_ACQUIRE);
            if (key) {
                /* une ligne par vCPU : regions.py additionne */
                fprintf(f, "%u,%u,%" PRIu64 "\n", (uint32_t)(key >> 32) - 1,
                        (uint32_t)key - 1, vc[i].edges[k].n);
            }
        }
    }
    fclose(f);
}

static void *snap_loop(void *arg)
{
    (void)arg;
    for (;;) {
        if (!access(snap, F_OK)) {
            char base[4096];
            snprintf(base, sizeof(base), "%s.%u", out, ++nsnap);
            dump(base);
            unlink(snap);
        }
        usleep(100000);
    }
    return NULL;
}

/* interval=S : instantané cumulé toutes les S secondes, en anneau de keep=K
 * fichiers <out>.r<i> (i = n mod K) ; l'en-tête porte t_wall : regions.py
 * prend les deux qui encadrent la mesure (par ex. la fenêtre de matrice.py). */
static void *ring_loop(void *arg)
{
    (void)arg;
    for (unsigned n = 0;; n++) {
        char base[4096];
        sleep(interval);
        snprintf(base, sizeof(base), "%s.r%u", out, n % keep);
        dump(base);
    }
    return NULL;
}

static void finish(void *p)
{
    (void)p;
    dump(out);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    (void)info;
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "out=", 4)) {
            out = strdup(argv[i] + 4);
        } else if (!strncmp(argv[i], "interval=", 9)) {
            interval = atoi(argv[i] + 9);
        } else if (!strncmp(argv[i], "keep=", 5)) {
            keep = atoi(argv[i] + 5) > 0 ? atoi(argv[i] + 5) : 8;
        } else if (!strncmp(argv[i], "snap=", 5)) {
            snap = strdup(argv[i] + 5);
        } else {
            fprintf(stderr, "tbedges : option inconnue %s\n", argv[i]);
            return -1;
        }
    }
    if (!out) {
        fprintf(stderr, "tbedges : out=<préfixe> requis\n");
        return -1;
    }
    for (int i = 0; i < MAXV; i++) {
        vc[i].execs = calloc(LIMIT, sizeof(uint64_t));
        vc[i].edges = NULL;
    }
    /* tables d'arêtes pour les vCPU réels seulement (128 Mio chacune) */
    {
        int nv = info->system_emulation ? info->system.smp_vcpus : 1;
        if (nv > MAXV) {
            nv = MAXV;
        }
        for (int i = 0; i < nv; i++) {
            vc[i].edges = calloc(1u << EBITS, sizeof(Edge));
            if (!vc[i].edges || !vc[i].execs) {
                fprintf(stderr, "tbedges : mémoire\n");
                return -1;
            }
        }
    }
    qemu_plugin_register_vcpu_tb_trans_cb(id, translate, NULL);
    qemu_plugin_register_atexit_cb(id, finish, NULL);
    if (snap) {
        pthread_t t;
        pthread_create(&t, NULL, snap_loop, NULL);
        pthread_detach(t);
    }
    if (interval) {
        pthread_t t;
        pthread_create(&t, NULL, ring_loop, NULL);
        pthread_detach(t);
    }
    return 0;
}
