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
    /* [surf, off, stride, x, y, w, h, format] ; `vram` = tampon où lire */
    uint32_t off = a[1], stride = a[2], w = a[5], h = a[6], fmt = a[7], x, y;
    char path[512];
    FILE *f;
    if (fmt != QGPU_PF_XRGB8888 || !w || !h)
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
    static uint32_t sk[128];             /* dernier SET_STATE vu par clé (suivi grossier) */

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
    uint32_t ndraw_frame = 0, ndraw_last_frame = 0xffffffffu;
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
        /* Vidage pris en cours de partie : contexte et surface ont été créés
           avant. On les crée à la volée au premier BIND d'un identifiant
           inconnu (surface 800×600 xRGB + profondeur + stencil), dans un petit
           flux synthétique exécuté avant la soumission. */
        {
            static uint8_t ctx_seen[256], surf_seen[256], ctx_has_surf[256], tex_seen[QGPU_MAX_TEX];
            static uint32_t last_present_surf = 1;
            uint32_t pre[4096], np = 0, q, bound_ctx = 256;
            /* la surface présentée dans CETTE soumission, si on la voit */
            for (q = 0; q + 1 < h.ncmd_bytes / 4; ) {
                uint32_t hd = qgpu_ld32(shmem + h.base + q * 4);
                uint32_t o = QGPU_CMD_OP(hd), l = QGPU_CMD_LEN(hd);
                if (!l || q + l > h.ncmd_bytes / 4) break;
                if (o == QGPU_OP_SURF_PRESENT && l == QGPU_LEN_SURF_PRESENT)
                    last_present_surf = qgpu_ld32(shmem + h.base + (q + 1) * 4);
                q += l;
            }
            for (q = 0; q + 1 < h.ncmd_bytes / 4; ) {
                uint32_t hd = qgpu_ld32(shmem + h.base + q * 4);
                uint32_t o = QGPU_CMD_OP(hd), l = QGPU_CMD_LEN(hd), id;
                if (!l || q + l > h.ncmd_bytes / 4) break;
                id = qgpu_ld32(shmem + h.base + (q + 1) * 4);
                if (o == QGPU_OP_CTX_CREATE && id < 256) ctx_seen[id] = 1;
                if (o == QGPU_OP_SURF_CREATE && id < 256) surf_seen[id] = 1;
                if (o == QGPU_OP_CTX_BIND && id < 256 && !ctx_seen[id] && np + 2 <= 60) {
                    pre[np++] = QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX); pre[np++] = id; ctx_seen[id] = 1;
                }
                if (o == QGPU_OP_TEX_CREATE3 && id < QGPU_MAX_TEX) tex_seen[id] = 1;
                /* texture créée avant le vidage : la créer ici, avec la cible
                   déduite de TEX_IMAGE3 (face de cube → cube), 2D sinon */
                if ((o == QGPU_OP_TEX_IMAGE3 || o == QGPU_OP_TEX_PARAM) && id < QGPU_MAX_TEX &&
                    !tex_seen[id] && np + 3 <= 4090) {
                    uint32_t tgt = QGPU_TT_2D;
                    if (o == QGPU_OP_TEX_IMAGE3) {
                        uint32_t it = qgpu_ld32(shmem + h.base + (q + 2) * 4);
                        tgt = (it >= QGPU_TT_CUBE_FACE(0) && it <= QGPU_TT_CUBE_FACE(5)) ? QGPU_TT_CUBE_MAP : it;
                    }
                    pre[np++] = QGPU_CMD_HDR(QGPU_OP_TEX_CREATE3, QGPU_LEN_TEX_CREATE3); pre[np++] = id; pre[np++] = tgt;
                    tex_seen[id] = 1;
                }
                if (o == QGPU_OP_CTX_BIND && id < 256) bound_ctx = id;
                if (o == QGPU_OP_SURF_BIND && bound_ctx < 256) ctx_has_surf[bound_ctx] = 1;
                if (o == QGPU_OP_SURF_BIND && id < 256 && !surf_seen[id] && np + 5 <= 60) {
                    pre[np++] = QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE); pre[np++] = id;
                    pre[np++] = 800; pre[np++] = 600;
                    pre[np++] = QGPU_FMT_XRGB8888 | QGPU_FMT_FLAG_DEPTH | QGPU_FMT_FLAG_STENCIL;
                    surf_seen[id] = 1;
                }
                q += l;
            }
            /* contexte lié sans surface dans le vidage : la surface était liée
               avant. On la crée (id de la présentation) et on la lie NOUS-MÊMES
               dans le prologue, après un CTX_BIND du même contexte. */
            if (bound_ctx < 256 && (!ctx_has_surf[bound_ctx] ||
                                    (last_present_surf < 256 && !surf_seen[last_present_surf])) &&
                np + 9 <= 4090) {
                uint32_t sid = last_present_surf < 256 ? last_present_surf : 1;
                if (!surf_seen[sid]) {
                    pre[np++] = QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE); pre[np++] = sid;
                    pre[np++] = 800; pre[np++] = 600;
                    pre[np++] = QGPU_FMT_XRGB8888 | QGPU_FMT_FLAG_DEPTH | QGPU_FMT_FLAG_STENCIL;
                    surf_seen[sid] = 1;
                }
                pre[np++] = QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX); pre[np++] = bound_ctx;
                pre[np++] = QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF); pre[np++] = sid;
                ctx_has_surf[bound_ctx] = 1;
            }
            if (np) {
                uint32_t poff = SHMEM - 16384;
                for (q = 0; q < np; q++) qgpu_st32(shmem + poff + q * 4, pre[q]);
                st = qgpu_core_execute(&c, poff, np * 4);
                if (st != QGPU_ST_OK || getenv("QGPU_REPLAY_DEBUG"))
                    fprintf(stderr, "prologue %s (%u mots, ctx %u, surf présentée %u) : statut %u pc %u\n",
                            names[i], np, bound_ctx, last_present_surf, st, c.status_pc);
            }
        }
        /* QGPU_REPLAY_SETKEY="clé=val,clé=val" (hexa) : expérience — chaque
           SET_STATE de ces clés est réécrit sur place avant l'exécution (et
           injecté en tête de soumission si la clé n'y figure pas), pour
           isoler la contribution d'une unité, d'un combineur, d'une constante. */
        if (getenv("QGPU_REPLAY_SETKEY")) {
            const char *e = getenv("QGPU_REPLAY_SETKEY");
            uint32_t q, kk, vv;
            while (*e) {
                if (sscanf(e, "%x=%x", &kk, &vv) == 2) {
                    int found = 0;
                    for (q = 0; q + 1 < h.ncmd_bytes / 4; ) {
                        uint32_t hd = qgpu_ld32(shmem + h.base + q * 4);
                        uint32_t o = QGPU_CMD_OP(hd), l = QGPU_CMD_LEN(hd);
                        if (!l || q + l > h.ncmd_bytes / 4) break;
                        if (o == QGPU_OP_SET_STATE && l == 3 && qgpu_ld32(shmem + h.base + (q + 1) * 4) == kk) {
                            qgpu_st32(shmem + h.base + (q + 2) * 4, vv); found = 1;
                        }
                        q += l;
                    }
                    if (!found) {
                        uint32_t inj[3] = { QGPU_CMD_HDR(QGPU_OP_SET_STATE, 3), kk, vv };
                        for (q = 0; q < 3; q++) qgpu_st32(shmem + SHMEM - 8192 + q * 4, inj[q]);
                        qgpu_core_execute(&c, SHMEM - 8192, 12);
                    }
                }
                while (*e && *e != ',') e++;
                if (*e == ',') e++;
            }
        }
        /* Expériences d'éclairage : QGPU_REPLAY_NOATT=1 → atténuation (1,0,0) sur
           chaque SET_LIGHT ; QGPU_REPLAY_AMB=1 → ambiante du modèle à 1. */
        if (getenv("QGPU_REPLAY_NOATT") || getenv("QGPU_REPLAY_AMB")) {
            uint32_t q;
            for (q = 0; q + 1 < h.ncmd_bytes / 4; ) {
                uint32_t hd = qgpu_ld32(shmem + h.base + q * 4);
                uint32_t o = QGPU_CMD_OP(hd), l = QGPU_CMD_LEN(hd);
                if (!l || q + l > h.ncmd_bytes / 4) break;
                if (getenv("QGPU_REPLAY_NOATT") && o == QGPU_OP_SET_LIGHT && l == QGPU_LEN_SET_LIGHT) {
                    qgpu_st32(shmem + h.base + (q + 24) * 4, qgpu_f2u(1.0f));
                    qgpu_st32(shmem + h.base + (q + 25) * 4, 0);
                    qgpu_st32(shmem + h.base + (q + 26) * 4, 0);
                }
                if (getenv("QGPU_REPLAY_AMB") && o == QGPU_OP_SET_LIGHT_MODEL && l == QGPU_LEN_SET_LIGHT_MODEL) {
                    uint32_t j2;
                    for (j2 = 1; j2 <= 3; j2++) qgpu_st32(shmem + h.base + (q + j2) * 4, qgpu_f2u(1.0f));
                }
                q += l;
            }
        }
        /* QGPU_REPLAY_SKIPDRAW="image:i-j" : expérience — les dessins d'indice
           i..j (DRAW_RAW/DRAW_RAW_BUF comptés depuis le début de l'image) sont
           sautés ; la soumission est exécutée par tronçons autour d'eux. */
        st = QGPU_ST_OK;
        if (h.frame != ndraw_last_frame) { ndraw_last_frame = h.frame; ndraw_frame = 0; }
        {
            unsigned sf = 0, si = 0, sj = 0; int skipping = 0;
            if (getenv("QGPU_REPLAY_SKIPDRAW") &&
                sscanf(getenv("QGPU_REPLAY_SKIPDRAW"), "%u:%u-%u", &sf, &si, &sj) == 3 && h.frame == sf)
                skipping = 1;
            if (!skipping) {
                st = qgpu_core_execute(&c, h.base, h.ncmd_bytes);
            } else {
                uint32_t q, seg = 0;
                for (q = 0; q + 1 < h.ncmd_bytes / 4 && st == QGPU_ST_OK; ) {
                    uint32_t hd = qgpu_ld32(shmem + h.base + q * 4);
                    uint32_t o = QGPU_CMD_OP(hd), l = QGPU_CMD_LEN(hd);
                    if (!l || q + l > h.ncmd_bytes / 4) break;
                    if (o == QGPU_OP_DRAW_RAW || o == QGPU_OP_DRAW_RAW_BUF) {
                        if (ndraw_frame >= si && ndraw_frame <= sj) {
                            if (q > seg) st = qgpu_core_execute(&c, h.base + seg * 4, (q - seg) * 4);
                            seg = q + l;
                        }
                        ndraw_frame++;
                    }
                    q += l;
                }
                if (st == QGPU_ST_OK && q > seg) st = qgpu_core_execute(&c, h.base + seg * 4, (q - seg) * 4);
            }
        }
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
            if (op == QGPU_OP_SET_STATE && len == 3) {
                uint32_t key = qgpu_ld32(shmem + h.base + (k + 1) * 4);
                uint32_t val = qgpu_ld32(shmem + h.base + (k + 2) * 4);
                if (key < 128) sk[key] = val;
            }
            if (getenv("QGPU_REPLAY_LIST") && h.frame == (uint32_t)atoi(getenv("QGPU_REPLAY_LIST"))) {
                for (j = 0; j < len && j <= QGPU_MAX_CMD_ARGS; j++) a[j] = qgpu_ld32(shmem + h.base + (k + j) * 4);
                if (op == QGPU_OP_SET_LIGHT && len == QGPU_LEN_SET_LIGHT) {
                    float f[26]; uint32_t q2;
                    for (q2 = 0; q2 < 25; q2++) f[q2] = qgpu_u2f(a[q2 + 2]);
                    fprintf(stderr, "LIST image %u SET_LIGHT %u actif %u : amb %.2f %.2f %.2f diff %.2f %.2f %.2f spec %.2f %.2f %.2f pos %.1f %.1f %.1f %.1f coupure %.0f att %g %g %g\n",
                            h.frame, a[1], a[2], f[1], f[2], f[3], f[5], f[6], f[7], f[9], f[10], f[11], f[13], f[14], f[15], f[16],
                            f[21], f[22], f[23], f[24]);
                }
                if (op == QGPU_OP_SET_MATERIAL && len == QGPU_LEN_SET_MATERIAL)
                    fprintf(stderr, "LIST image %u SET_MATERIAL face %x : amb %.2f %.2f %.2f diff %.2f %.2f %.2f %.2f emis %.2f %.2f %.2f\n",
                            h.frame, a[1], qgpu_u2f(a[2]), qgpu_u2f(a[3]), qgpu_u2f(a[4]), qgpu_u2f(a[6]), qgpu_u2f(a[7]), qgpu_u2f(a[8]), qgpu_u2f(a[9]),
                            qgpu_u2f(a[14]), qgpu_u2f(a[15]), qgpu_u2f(a[16]));
                if (op == QGPU_OP_SET_LIGHT_MODEL && len == QGPU_LEN_SET_LIGHT_MODEL)
                    fprintf(stderr, "LIST image %u LIGHT_MODEL ambiante %.2f %.2f %.2f %.2f\n", h.frame,
                            qgpu_u2f(a[1]), qgpu_u2f(a[2]), qgpu_u2f(a[3]), qgpu_u2f(a[4]));
                if (op == QGPU_OP_TEX_IMAGE3)
                    fprintf(stderr, "LIST image %u TEX_IMAGE3 : tex %u cible %x niveau %u %ux%ux%u base %x fmt %x type %x\n",
                            h.frame, a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
                if (op == QGPU_OP_DRAW_RAW || op == QGPU_OP_DRAW_RAW_BUF) {
                    static const char *fn[8] = { "REPLACE", "MODULATE", "ADD", "ADD_SIGNED", "INTERPOLATE", "SUBTRACT", "DOT3_RGB", "DOT3_RGBA" };
                    static const char *sn[8] = { "TEX", "CONST", "PRIM", "PREV", "TEX0", "TEX1", "TEX2", "TEX3" };
                    static const char *on[4] = { "c", "1-c", "a", "1-a" };
                    uint32_t u;
                    for (u = 0; u < 4; u++) {
                        uint32_t cb = sk[QGPU_SK_COMBINE0 + u], cs = sk[QGPU_SK_COMBINE_SRC0 + u], i;
                        fprintf(stderr, "   u%u: %s(", u, fn[cb & 7]);
                        for (i = 0; i < 3; i++) { uint32_t v = (cs >> (5 * i)) & 0x1f; fprintf(stderr, "%s%s.%s", i ? "," : "", sn[v & 7], on[(v >> 3) & 3]); }
                        fprintf(stderr, ")x%u  A:%s(", 1u << ((cb >> 8) & 3), fn[(cb >> 4) & 7]);
                        for (i = 0; i < 3; i++) { uint32_t v = (cs >> (15 + 4 * i)) & 0xf; fprintf(stderr, "%s%s.%s", i ? "," : "", sn[v & 7], on[(v >> 3) & 1 ? 3 : 2]); }
                        fprintf(stderr, ")x%u  const %08x\n", 1u << ((cb >> 10) & 3),
                                sk[u == 0 ? QGPU_SK_TEX_ENV_COLOR : u == 1 ? QGPU_SK_TEX1_ENV_COLOR : u == 2 ? QGPU_SK_TEX2_ENV_COLOR : QGPU_SK_TEX3_ENV_COLOR]);
                    }
                }
                if (op == QGPU_OP_DRAW_RAW || op == QGPU_OP_DRAW_RAW_BUF)
                    fprintf(stderr, "LIST image %u %s : mode %u count %u fmt %x nverts %u | tex u0 %u/%u env %x u1 %u/%u env %x u2 %u/%u u3 %u/%u | comb0 %x src0 %x | light %u blend %u alpha %u colmat %u/%x\n",
                            h.frame, op == QGPU_OP_DRAW_RAW ? "DRAW_RAW" : "DRAW_RAW_BUF",
                            a[1], a[2], a[5], a[9],
                            sk[QGPU_SK_TEXTURE], sk[QGPU_SK_TEX_BIND], sk[QGPU_SK_TEX_ENV_MODE],
                            sk[QGPU_SK_TEXTURE1], sk[QGPU_SK_TEX1_BIND], sk[QGPU_SK_TEX1_ENV_MODE],
                            sk[QGPU_SK_TEXTURE2], sk[QGPU_SK_TEX2_BIND], sk[QGPU_SK_TEXTURE3], sk[QGPU_SK_TEX3_BIND],
                            sk[QGPU_SK_COMBINE0], sk[QGPU_SK_COMBINE_SRC0],
                            sk[QGPU_SK_LIGHTING], sk[QGPU_SK_BLEND], sk[QGPU_SK_ALPHA_TEST], sk[QGPU_SK_COLOR_MATERIAL], sk[QGPU_SK_COLOR_MAT_MODE]);
            }
            if (op == QGPU_OP_SURF_PRESENT && len == QGPU_LEN_SURF_PRESENT && h.frame >= fmin) {
                /* relecture de la surface par le cœur (SURF_READBACK dans une
                   zone de travail), puis PPM : ne dépend pas du scanout */
                uint32_t rb[9], surf = qgpu_ld32(shmem + h.base + (k + 1) * 4);
                uint32_t roff = SHMEM - 16384 - 800 * 600 * 4, pres[9];
                rb[0] = QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER);
                rb[1] = surf; rb[2] = roff; rb[3] = 800 * 4; rb[4] = 0; rb[5] = 0; rb[6] = 800; rb[7] = 600;
                for (j = 0; j < 8; j++) qgpu_st32(shmem + SHMEM - 8192 + j * 4, rb[j]);
                if (qgpu_core_execute(&c, SHMEM - 8192, 32) == QGPU_ST_OK) {
                    pres[0] = surf; pres[1] = roff; pres[2] = 800 * 4; pres[3] = 0; pres[4] = 0;
                    pres[5] = 800; pres[6] = 600; pres[7] = QGPU_PF_XRGB8888;
                    write_present(shmem, pres, prefix, npresent++, h.frame);
                } else {
                    fprintf(stderr, "relecture impossible (image %u, surface %u)\n", h.frame, surf);
                }
            }
            k += len;
        }
    }
    fprintf(stderr, "%zu soumissions, %u présentations écrites, %u en erreur\n", nn, npresent, nerr);
    qgpu_core_fini(&c);
    return 0;
}
