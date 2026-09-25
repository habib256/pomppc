/*
 * ppcmix.c — greffon TCG : ce que le G4 émulé exécute VRAIMENT, par opcode.
 *
 * Compte chaque instruction PowerPC exécutée, rangée par clé d'opcode
 * (opcode primaire + code étendu : AltiVec VX/VA/VC, X-form 31, 19, flottant
 * 59/63 ; mtspr et mfspr par numéro de SPR, rangés sous les primaires 0 et 1
 * que le G4 n'emploie pas), en deux banques : « commpage » (pc >= 0xffff8000 : memcpy, bzero,
 * gettimeofday… de Tiger, écrits en AltiVec) et « reste ». Compteurs en ligne
 * (inline, par vCPU) : aucun appel de fonction par instruction exécutée.
 *
 * Un fil écrit l'instantané cumulé toutes les `interval` secondes dans
 * `out` (une ligne « T <s> <tbs> » puis « <banque> <clé> <compte> ») : la
 * différence de deux instantanés donne une fenêtre fixe de la scène.
 * tools/tcg/ppcmix.py nomme les clés et classe chaque instruction (helper ou
 * code TCG en ligne, d'après le relevé de docs/tcg-g4.md).
 *
 *   qemu-system-ppc … -plugin tools/tcg/libppcmix.dylib,out=/tmp/mix.txt,interval=10
 *
 * Construction : tools/tcg/build.sh (QEMU_SRC=~/src/qemu-tcg par défaut).
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

#define NKEYS  (64 * 2048)      /* primaire (6 bits) << 11 | sous-code (11 bits) */
#define NBANKS 2                /* 0 = reste, 1 = commpage */

typedef struct {
    uint64_t c[NBANKS][NKEYS];
} Counts;

static struct qemu_plugin_scoreboard *sb;
static char *out_path;
static unsigned interval = 10;
static uint64_t n_tbs;          /* traductions vues (atomique) */
static struct timespec t0;

/* Codes VC (comparaisons AltiVec, bit Rc = 0x400) du G4. */
static const int vc_codes[] = { 6, 70, 134, 198, 454, 518, 582, 646, 710,
                                774, 838, 902, 966 };

static unsigned key_of(uint32_t w)
{
    unsigned op = w >> 26, sub = 0;
    switch (op) {
    case 4: {                                   /* AltiVec */
        unsigned lo6 = w & 0x3f;
        if ((lo6 & 0x30) == 0x20) {             /* VA-form : 32..47 */
            sub = 0x400 | lo6;                  /* hors plage des VX (pairs, < 0x7ff) */
            break;
        }
        for (unsigned i = 0; i < sizeof(vc_codes) / sizeof(vc_codes[0]); i++) {
            if ((w & 0x3ff) == (unsigned)vc_codes[i]) {
                sub = w & 0x3ff;                /* point (Rc) confondu */
                return op << 11 | sub;
            }
        }
        sub = w & 0x7ff;
        break;
    }
    case 19:
        sub = (w >> 1) & 0x3ff;
        break;
    case 31:
        sub = (w >> 1) & 0x3ff;
        if (sub == 467 || sub == 339) {         /* mtspr / mfspr : par SPR */
            unsigned spr = ((w >> 16) & 0x1f) | ((w >> 6) & 0x3e0);
            return (sub == 467 ? 0u : 1u) << 11 | spr;   /* primaires 0 et 1 : libres sur G4 */
        }
        break;
    case 59:
        sub = (w >> 1) & 0x1f;
        break;
    case 63: {
        unsigned xo5 = (w >> 1) & 0x1f;
        sub = xo5 >= 18 ? 0x400 | xo5 : (w >> 1) & 0x3ff;
        break;
    }
    default:
        sub = 0;
    }
    return op << 11 | sub;
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    __atomic_add_fetch(&n_tbs, 1, __ATOMIC_RELAXED);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint8_t b[4] = { 0 };
        if (qemu_plugin_insn_data(insn, b, 4) != 4) {
            continue;
        }
        uint32_t w = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
        uint64_t pc = qemu_plugin_insn_vaddr(insn);
        unsigned bank = (pc & 0xffffffffu) >= 0xffff8000u ? 1 : 0;
        size_t off = ((size_t)bank * NKEYS + key_of(w)) * sizeof(uint64_t);
        qemu_plugin_u64 e = { .score = sb, .offset = off };
        qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
            insn, QEMU_PLUGIN_INLINE_ADD_U64, e, 1);
    }
}

static void dump(FILE *f)
{
    struct timespec t;
    int nv = qemu_plugin_num_vcpus();
    clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(f, "T %.3f %" PRIu64 " %d\n",
            (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9,
            __atomic_load_n(&n_tbs, __ATOMIC_RELAXED), nv);
    for (unsigned b = 0; b < NBANKS; b++) {
        for (unsigned k = 0; k < NKEYS; k++) {
            uint64_t s = 0;
            qemu_plugin_u64 e = { .score = sb,
                                  .offset = ((size_t)b * NKEYS + k) * 8 };
            for (int v = 0; v < nv; v++) {
                s += qemu_plugin_u64_get(e, v);
            }
            if (s) {
                fprintf(f, "%u %u %" PRIu64 "\n", b, k, s);
            }
        }
    }
    fflush(f);
}

static void *dumper(void *arg)
{
    FILE *f = arg;
    for (;;) {
        sleep(interval);
        dump(f);
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
            fprintf(stderr, "ppcmix : option inconnue %s\n", argv[i]);
            return -1;
        }
    }
    if (!out_path) {
        fprintf(stderr, "ppcmix : out=<fichier> requis\n");
        return -1;
    }
    outf = fopen(out_path, "w");
    if (!outf) {
        perror(out_path);
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
