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

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
enum { LIMIT = 262144 };
static struct qemu_plugin_scoreboard *score;
static struct { uint64_t pc; size_t insns; } blocks[LIMIT];
static size_t used;
static uint64_t skipped;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char *out;

static void translate(struct qemu_plugin_tb *tb, void *userdata)
{
    (void)userdata;
    pthread_mutex_lock(&lock);
    if (used == LIMIT) {
        skipped++;
        pthread_mutex_unlock(&lock);
        return;
    }
    size_t slot = used++;
    blocks[slot].pc = qemu_plugin_tb_vaddr(tb);
    blocks[slot].insns = qemu_plugin_tb_n_insns(tb);
    pthread_mutex_unlock(&lock);
    qemu_plugin_u64 e = { .score = score, .offset = slot * sizeof(uint64_t) };
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(tb, QEMU_PLUGIN_INLINE_ADD_U64, e, 1);
}
static void finish(void *userdata)
{
    (void)userdata;
    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return; }
    fprintf(f, "# skipped_translations=%" PRIu64 "\npc,guest_insns,executions\n", skipped);
    for (size_t i = 0; i < used; i++) {
        qemu_plugin_u64 e = { .score = score, .offset = i * sizeof(uint64_t) };
        fprintf(f, "0x%" PRIx64 ",%zu,%" PRIu64 "\n", blocks[i].pc,
                blocks[i].insns, qemu_plugin_u64_sum(e));
    }
    fclose(f);
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
        } else {
            fprintf(stderr, "hotblocks: unknown option %s\n", argv[i]);
            free(out); return -1;
        }
    }
    score = qemu_plugin_scoreboard_new(LIMIT * sizeof(uint64_t));
    qemu_plugin_register_vcpu_tb_trans_cb(id, translate, NULL);
    qemu_plugin_register_atexit_cb(id, finish, NULL);
    return 0;
}
