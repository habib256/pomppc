/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Per-translation execution counts. Diagnostic only: enabling a plugin
 * disables POMPPC's inline indirect-exit path, so do not use this run for A/B.
 * Pair with -d out_asm -D jit.log and jitblocks.py for emitted-code costs.
 */
#include <qemu-plugin.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
/* 21/10 : une case par (adresse, taille) — une retraduction (ou le même code
 * à la même adresse dans un autre processus) reprend sa case ; sans cela le
 * démarrage de Tiger épuisait les 262 144 cases. */
enum { LIMIT = 1 << 21, HBITS = 22 };
static uint32_t hidx[1 << HBITS];       /* slot + 1, 0 = vide */
static struct qemu_plugin_scoreboard *score;
static struct { uint64_t pc; size_t insns; } blocks[LIMIT];
static size_t used;
static uint64_t skipped;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char *out;
static char *snap;              /* snap=FICHIER : instantané à la demande */
static unsigned nsnap;

static void translate(struct qemu_plugin_tb *tb, void *userdata)
{
    (void)userdata;
    pthread_mutex_lock(&lock);
    if (used == LIMIT) {
        skipped++;
        pthread_mutex_unlock(&lock);
        return;
    }
    uint64_t pc = qemu_plugin_tb_vaddr(tb);
    size_t ni = qemu_plugin_tb_n_insns(tb), slot;
    uint32_t h = (uint32_t)((pc * 0x9E3779B97F4A7C15ULL + ni) >> (64 - HBITS));
    while (hidx[h] && (blocks[hidx[h] - 1].pc != pc || blocks[hidx[h] - 1].insns != ni))
        h = (h + 1) & ((1u << HBITS) - 1);
    if (hidx[h]) {
        slot = hidx[h] - 1;
    } else {
        slot = used++;
        blocks[slot].pc = pc;
        blocks[slot].insns = ni;
        hidx[h] = (uint32_t)slot + 1;
    }
    pthread_mutex_unlock(&lock);
    qemu_plugin_u64 e = { .score = score, .offset = slot * sizeof(uint64_t) };
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(tb, QEMU_PLUGIN_INLINE_ADD_U64, e, 1);
}
static void dump(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    fprintf(f, "# skipped_translations=%" PRIu64 "\npc,guest_insns,executions\n", skipped);
    for (size_t i = 0; i < used; i++) {
        qemu_plugin_u64 e = { .score = score, .offset = i * sizeof(uint64_t) };
        fprintf(f, "0x%" PRIx64 ",%zu,%" PRIu64 "\n", blocks[i].pc,
                blocks[i].insns, qemu_plugin_u64_sum(e));
    }
    fclose(f);
}
/* snap=FICHIER : tant que QEMU tourne, l'apparition de FICHIER (touch côté
 * hôte) écrit un instantané cumulé <out>.<n> (n = 1, 2…) puis retire FICHIER.
 * La différence de deux instantanés encadre une mesure (lecture des compteurs
 * sans arrêter les vCPU : à quelques blocs près). */
static void *snap_loop(void *arg)
{
    (void)arg;
    for (;;) {
        if (!access(snap, F_OK)) {
            char path[4096];
            pthread_mutex_lock(&lock);
            snprintf(path, sizeof(path), "%s.%u", out, ++nsnap);
            dump(path);
            pthread_mutex_unlock(&lock);
            unlink(snap);
        }
        usleep(100000);
    }
    return NULL;
}
static void finish(void *userdata)
{
    (void)userdata;
    dump(out);
    qemu_plugin_scoreboard_free(score);
    free(out);
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
    const qemu_info_t *info, int argc, char **argv)
{
    (void)info;
    out = strdup("hotblocks.csv");
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "out=", 4)) {
            free(out); out = strdup(argv[i] + 4);
        } else if (!strncmp(argv[i], "snap=", 5)) {
            snap = strdup(argv[i] + 5);
        } else {
            fprintf(stderr, "hotblocks: unknown option %s\n", argv[i]);
            free(out); return -1;
        }
    }
    score = qemu_plugin_scoreboard_new(LIMIT * sizeof(uint64_t));
    qemu_plugin_register_vcpu_tb_trans_cb(id, translate, NULL);
    qemu_plugin_register_atexit_cb(id, finish, NULL);
    if (snap) {
        pthread_t t;
        pthread_create(&t, NULL, snap_loop, NULL);
        pthread_detach(t);
    }
    return 0;
}
