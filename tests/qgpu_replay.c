/* qgpu_replay.c — rejoue en natif les soumissions vidées par le plugin
 * (POMPPC_GL_DUMP, guest/gldriver/pomppc_accel.c : dump_submit) sur le cœur et
 * un backend de l'hôte, et écrit une image PPM par SURF_PRESENT — ce que
 * l'invité aurait affiché. Pour reproduire hors VM un défaut de rendu vu dans
 * un jeu (traînées d'UT2004, 22/09/2026) avec des données EXACTES.
 *
 *   cc -std=gnu11 -O1 -pthread -I patches/qgpu tests/qgpu_replay.c \
 *      patches/qgpu/qgpu-core.c patches/qgpu/qgpu-soft.c patches/qgpu/qgpu-gl.c \
 *      $(pkg-config --libs egl gl) -lm -o qgpu_replay
 *   ./qgpu_replay <dossier des NNNNNN.bin> <préfixe des PPM> [soft|gl] [image_min image_max]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include "qgpu_proto.h"
#include "qgpu-core.h"

struct dump_hdr {
    uint32_t magic, frame, base, ncmd_bytes, vtx_off, vtx_len, idx_off, idx_len,
             arena_off, arena_len, reserved[6];
};

#define SHMEM (64u << 20)
#define VRAM  (32u << 20)

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void write_present(const uint8_t *vram, const uint32_t *a, const char *prefix,
                          unsigned n, uint32_t frame)
{
    /* [surf, off, stride, x, y, w, h, format] */
    uint32_t off = a[1], stride = a[2], w = a[5], h = a[6], fmt = a[7], x, y;
    char path[512];
    FILE *f;
    if (fmt != QGPU_PF_XRGB8888 || !w || !h || (uint64_t)off + (uint64_t)h * stride > VRAM)
        return;
    snprintf(path, sizeof(path), "%s-%04u-f%u.ppm", prefix, n, frame);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++) {
        const uint8_t *row = vram + off + (size_t)y * stride;
        for (x = 0; x < w; x++) {
            uint32_t p = qgpu_ld32(row + x * 4);
            uint8_t rgb[3] = { (p >> 16) & 255, (p >> 8) & 255, p & 255 };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "dump", *prefix = argc > 2 ? argv[2] : "replay";
    const char *backend = argc > 3 ? argv[3] : "soft";
    uint32_t fmin = argc > 4 ? (uint32_t)atoi(argv[4]) : 0, fmax = argc > 5 ? (uint32_t)atoi(argv[5]) : ~0u;
    uint8_t *shmem = calloc(SHMEM, 1), *vram = calloc(VRAM, 1);
    QgpuCore c;
    DIR *d;
    struct dirent *e;
    char **names = NULL;
    size_t nn = 0, i;
    unsigned npresent = 0, nerr = 0;

    if (!shmem || !vram || !qgpu_core_init(&c, backend, shmem, SHMEM)) {
        fprintf(stderr, "init impossible (backend %s)\n", backend);
        return 1;
    }
    qgpu_core_set_scanout(&c, vram, VRAM, NULL, NULL);
    d = opendir(dir);
    if (!d) { perror(dir); return 1; }
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".bin")) {
            names = realloc(names, (nn + 1) * sizeof(*names));
            names[nn++] = strdup(e->d_name);
        }
    }
    closedir(d);
    qsort(names, nn, sizeof(*names), cmp);
    for (i = 0; i < nn; i++) {
        char path[512];
        struct dump_hdr h;
        FILE *f;
        uint32_t st, k;
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        f = fopen(path, "rb");
        uint8_t raw[sizeof(h)];
        uint32_t *hf = (uint32_t *)&h, q;
        if (!f || fread(raw, sizeof(raw), 1, f) != 1) {
            fprintf(stderr, "%s : en-tête illisible\n", path);
            if (f) fclose(f);
            continue;
        }
        for (q = 0; q < sizeof(h) / 4; q++)      /* écrit par un PowerPC : grand-boutiste */
            hf[q] = qgpu_ld32(raw + q * 4);
        if (h.magic != 0x50514431u) {
            fprintf(stderr, "%s : en-tête invalide\n", path);
            if (f) fclose(f);
            continue;
        }
        if (h.frame > fmax) { fclose(f); break; }
        if ((uint64_t)h.base + h.arena_off + h.arena_len > SHMEM) {
            fprintf(stderr, "%s : hors fenêtre\n", path); fclose(f); continue;
        }
        if (fread(shmem + h.base, 1, h.ncmd_bytes, f) != h.ncmd_bytes ||
            fread(shmem + h.base + h.vtx_off, 1, h.vtx_len, f) != h.vtx_len ||
            fread(shmem + h.base + h.idx_off, 1, h.idx_len, f) != h.idx_len ||
            fread(shmem + h.base + h.arena_off, 1, h.arena_len, f) != h.arena_len) {
            fprintf(stderr, "%s : tronqué\n", path); fclose(f); continue;
        }
        fclose(f);
        st = qgpu_core_execute(&c, h.base, h.ncmd_bytes);
        if (st != QGPU_ST_OK) {
            nerr++;
            fprintf(stderr, "%s (image %u) : statut %u à la commande %u\n", names[i], h.frame, st, c.status_pc);
        }
        /* les présentations de cette soumission → une image chacune */
        for (k = 0; k + 1 < h.ncmd_bytes / 4; ) {
            uint32_t hdr = qgpu_ld32(shmem + h.base + k * 4);
            uint32_t op = QGPU_CMD_OP(hdr), len = QGPU_CMD_LEN(hdr);
            uint32_t a[QGPU_MAX_CMD_ARGS + 1], j;
            if (!len || k + len > h.ncmd_bytes / 4) break;
            if (getenv("QGPU_REPLAY_LIST") && h.frame == (uint32_t)atoi(getenv("QGPU_REPLAY_LIST")) &&
                (op == QGPU_OP_DRAW_RAW || op == QGPU_OP_DRAW_RAW_BUF)) {
                for (j = 0; j < len && j <= QGPU_MAX_CMD_ARGS; j++) a[j] = qgpu_ld32(shmem + h.base + (k + j) * 4);
                fprintf(stderr, "LIST image %u %s : mode %u count %u voff %x stride %u fmt %x ioff %x itype %u first %u nverts %u\n",
                        h.frame, op == QGPU_OP_DRAW_RAW ? "DRAW_RAW" : "DRAW_RAW_BUF",
                        a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
            }
            if (op == QGPU_OP_SURF_PRESENT && len == QGPU_LEN_SURF_PRESENT && h.frame >= fmin) {
                for (j = 0; j < len; j++) a[j] = qgpu_ld32(shmem + h.base + (k + j) * 4);
                write_present(vram, a + 1, prefix, npresent++, h.frame);
            }
            k += len;
        }
    }
    fprintf(stderr, "%zu soumissions, %u présentations écrites, %u en erreur\n", nn, npresent, nerr);
    qgpu_core_fini(&c);
    return 0;
}
