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

/* Taille des surfaces créées à la volée (surface liée avant le vidage) :
   QGPU_REPLAY_SURF=LxH, sinon la plus grande zone présentée dans le vidage
   (x+w, y+h des SURF_PRESENT : fenêtre 640×480, plein écran 1024×768),
   sinon 800×600. */
static uint32_t surf_w = 800, surf_h = 600;

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
    static uint8_t tex_img_seen[QGPU_MAX_TEX];
    QgpuCore c;
    DIR *d;
    struct dirent *e;
    char **names = NULL;
    size_t nn = 0, i;
    unsigned npresent = 0, nerr = 0;
    static uint32_t sk[QGPU_SK_COUNT];             /* dernier SET_STATE vu par clé (suivi grossier) */

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
    if (getenv("QGPU_REPLAY_SURF") &&
        sscanf(getenv("QGPU_REPLAY_SURF"), "%ux%u", &surf_w, &surf_h) == 2) {
        /* imposé */
    } else {
        uint32_t mw = 0, mh = 0;
        for (i = 0; i < nn; i++) {
            char path[512];
            uint8_t raw[64], *cmd;
            uint32_t ncb, q;
            FILE *f;
            snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
            f = fopen(path, "rb");
            if (!f) continue;
            if (fread(raw, 64, 1, f) != 1 || qgpu_ld32(raw) != 0x50514431u ||
                (ncb = qgpu_ld32(raw + 12)) > SHMEM || !(cmd = malloc(ncb ? ncb : 1))) {
                fclose(f); continue;
            }
            if (fread(cmd, 1, ncb, f) == ncb)
                for (q = 0; q + 1 < ncb / 4; ) {
                    uint32_t hd = qgpu_ld32(cmd + q * 4), o = QGPU_CMD_OP(hd), l = QGPU_CMD_LEN(hd);
                    if (!l || q + l > ncb / 4) break;
                    if (o == QGPU_OP_SURF_PRESENT && l == QGPU_LEN_SURF_PRESENT) {
                        uint32_t x = qgpu_ld32(cmd + (q + 4) * 4), y = qgpu_ld32(cmd + (q + 5) * 4);
                        uint32_t w = qgpu_ld32(cmd + (q + 6) * 4), hh = qgpu_ld32(cmd + (q + 7) * 4);
                        if (x + w > mw && x + w <= 4096) mw = x + w;
                        if (y + hh > mh && y + hh <= 4096) mh = y + hh;
                    }
                    q += l;
                }
            free(cmd);
            fclose(f);
        }
        /* exactement la zone présentée : une surface plus haute décale tout
           (origine en bas, relecture comptée d'en haut — DOOM 3 640×480, 26/09) */
        if (mw && mh) { surf_w = mw; surf_h = mh; }
    }
    fprintf(stderr, "surfaces créées à la volée : %ux%u\n", surf_w, surf_h);
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
            static uint8_t prog_seen[256][QGPU_MAX_PROG];        /* v16, par contexte */
            static uint8_t buf_seen[QGPU_MAX_BUF];               /* v14 */
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
                /* TEX_CREATE (v3, sans cible) crée aussi : sans lui, un TEX_IMAGE3
                   qui suit faisait créer la texture par le prologue et le
                   TEX_CREATE du flux tombait en LIMIT (Marble Blast, 26/09) */
                if ((o == QGPU_OP_TEX_CREATE3 || o == QGPU_OP_TEX_CREATE) && id < QGPU_MAX_TEX)
                    tex_seen[id] = 1;
                /* texture détruite sans avoir été vue : le vidage autonome
                   (invalidate_mirrors) détruit ce que l'hôte avait ; on la crée
                   pour que le TEX_DESTROY ne soit pas un BAD_ARG */
                if (o == QGPU_OP_TEX_DESTROY && id < QGPU_MAX_TEX && np + 3 <= 4090) {
                    if (!tex_seen[id]) {
                        pre[np++] = QGPU_CMD_HDR(QGPU_OP_TEX_CREATE3, QGPU_LEN_TEX_CREATE3);
                        pre[np++] = id; pre[np++] = QGPU_TT_2D;
                    }
                    tex_seen[id] = 0;
                }
                /* v14 : tampon hôte créé avant le vidage (Prey, 23/09) : on le
                   crée à la taille maximale, sans quoi BUF_SUBDATA puis tout
                   le reste de la soumission sont jetés (BAD_ARG en cascade). */
                if (o == QGPU_OP_BUF_CREATE && id < QGPU_MAX_BUF) buf_seen[id] = 1;
                {
                    uint32_t bid = 0xffffffffu;
                    if (o == QGPU_OP_BUF_SUBDATA) bid = id;
                    if (o == QGPU_OP_DRAW_RAW_BUF) bid = qgpu_ld32(shmem + h.base + (q + 3) * 4);
                    if (bid < QGPU_MAX_BUF && !buf_seen[bid] && np + 3 <= 4090) {
                        pre[np++] = QGPU_CMD_HDR(QGPU_OP_BUF_CREATE, QGPU_LEN_BUF_CREATE);
                        pre[np++] = bid; pre[np++] = QGPU_MAX_BUF_SIZE;
                        buf_seen[bid] = 1;
                    }
                }
                /* v18 : DRAW_NATIVE cite son tampon d'indices et un tampon par
                   attribut (table à aoff, absolu dans BAR0) : même prologue */
                if (o == QGPU_OP_DRAW_NATIVE && l == QGPU_LEN_DRAW_NATIVE) {
                    uint32_t ib = qgpu_ld32(shmem + h.base + (q + 3) * 4);
                    uint32_t it = qgpu_ld32(shmem + h.base + (q + 5) * 4);
                    uint32_t na = qgpu_ld32(shmem + h.base + (q + 7) * 4);
                    uint32_t ao = qgpu_ld32(shmem + h.base + (q + 8) * 4), kk;
                    uint32_t bids[QGPU_NATIVE_MAX_ATTRS + 1], nbid = 0;
                    if (it != QGPU_IDX_NONE) bids[nbid++] = ib;
                    if (na <= QGPU_NATIVE_MAX_ATTRS &&
                        (uint64_t)ao + na * QGPU_NATIVE_DESC_WORDS * 4 <= SHMEM)
                        for (kk = 0; kk < na; kk++)
                            bids[nbid++] = qgpu_ld32(shmem + ao + (kk * QGPU_NATIVE_DESC_WORDS + 1) * 4);
                    for (kk = 0; kk < nbid; kk++) {
                        uint32_t bid = bids[kk];
                        if (bid < QGPU_MAX_BUF && !buf_seen[bid] && np + 3 <= 4090) {
                            pre[np++] = QGPU_CMD_HDR(QGPU_OP_BUF_CREATE, QGPU_LEN_BUF_CREATE);
                            pre[np++] = bid; pre[np++] = QGPU_MAX_BUF_SIZE;
                            buf_seen[bid] = 1;
                        }
                    }
                }
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
                /* v16 : programme créé avant le vidage — son texte n'est connu
                   que par un PROG_STRING ; on le crée alors, avec la cible lue
                   dans l'en-tête du texte. Un BIND/LOCAL sans texte reste
                   BAD_ARG : le rejeu ne peut pas inventer un programme. */
                if (o == QGPU_OP_PROG_CREATE && id < QGPU_MAX_PROG && bound_ctx < 256)
                    prog_seen[bound_ctx][id] = 1;
                if (o == QGPU_OP_PROG_STRING && l == QGPU_LEN_PROG_STRING && id < QGPU_MAX_PROG &&
                    bound_ctx < 256 && !prog_seen[bound_ctx][id] && np + 5 <= 4090) {
                    uint32_t off = qgpu_ld32(shmem + h.base + (q + 3) * 4);
                    uint32_t tgt = (off + 10 <= SHMEM && !memcmp(shmem + off, "!!ARBfp1.0", 10))
                                   ? QGPU_PT_FRAGMENT : QGPU_PT_VERTEX;
                    pre[np++] = QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX); pre[np++] = bound_ctx;
                    pre[np++] = QGPU_CMD_HDR(QGPU_OP_PROG_CREATE, QGPU_LEN_PROG_CREATE);
                    pre[np++] = id; pre[np++] = tgt;
                    prog_seen[bound_ctx][id] = 1;
                }
                if (o == QGPU_OP_CTX_BIND && id < 256) bound_ctx = id;
                if (o == QGPU_OP_SURF_BIND && bound_ctx < 256) ctx_has_surf[bound_ctx] = 1;
                if (o == QGPU_OP_SURF_BIND && id < 256 && !surf_seen[id] && np + 5 <= 60) {
                    pre[np++] = QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE); pre[np++] = id;
                    pre[np++] = surf_w; pre[np++] = surf_h;
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
                    pre[np++] = surf_w; pre[np++] = surf_h;
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
                    if (o == QGPU_OP_DRAW_RAW || o == QGPU_OP_DRAW_RAW_BUF ||
                        o == QGPU_OP_DRAW_NATIVE) {
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
                if (key < QGPU_SK_COUNT) sk[key] = val;
            }
            if (op == QGPU_OP_TEX_CREATE3 || op == QGPU_OP_TEX_IMAGE3) {
                uint32_t id = qgpu_ld32(shmem + h.base + (k + 1) * 4);
                if (id < QGPU_MAX_TEX) tex_img_seen[id] |= (op == QGPU_OP_TEX_IMAGE3) ? 2 : 1;
            }
            if (op == QGPU_OP_DRAW_RAW || op == QGPU_OP_DRAW_RAW_BUF ||
                op == QGPU_OP_DRAW_NATIVE || op == QGPU_OP_DRAW_TRIANGLES_TEXN || op == QGPU_OP_DRAW_TRIANGLES_SEC ||
                op == QGPU_OP_DRAW_TRIANGLES_TEX || op == QGPU_OP_DRAW_TRIANGLES_TEX2) {
                /* lot 11 : une texture liée dont aucune image n'est dans le
                   vidage rend le rejeu infidèle — le dire, une fois par texture */
                uint32_t en[QGPU_MAX_UNITS], bk[QGPU_MAX_UNITS], u;
                for (u = 0; u < QGPU_MAX_UNITS; u++) {
                    en[u] = QGPU_SK_UNIT(u) + QGPU_SK_U_ENABLE;
                    bk[u] = QGPU_SK_UNIT(u) + QGPU_SK_U_BIND;
                }
                for (u = 0; u < QGPU_MAX_UNITS; u++) {
                    uint32_t id = sk[bk[u]];
                    if (sk[en[u]] && id < QGPU_MAX_TEX && !(tex_img_seen[id] & 2) && !(tex_img_seen[id] & 4)) {
                        tex_img_seen[id] |= 4;
                        fprintf(stderr, "image %u : texture %u liée à l'unité %u sans image dans le vidage\n", h.frame, id, u);
                    }
                }
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
                if (op == QGPU_OP_DRAW_NATIVE && len == QGPU_LEN_DRAW_NATIVE) {
                    /* v18 : [mode, n, ibuf, ioff, itype, premier, nattr, aoff],
                       puis un descripteur par ligne */
                    uint32_t kk, na = a[7], ao = a[8];
                    fprintf(stderr, "LIST image %u DRAW_NATIVE : mode %u count %u ibuf %d ioff %u itype %u premier %u nattr %u aoff %x\n",
                            h.frame, a[1], a[2], (int)a[3], a[4], a[5], a[6], na, ao);
                    if (na <= QGPU_NATIVE_MAX_ATTRS &&
                        (uint64_t)ao + na * QGPU_NATIVE_DESC_WORDS * 4 <= SHMEM)
                        for (kk = 0; kk < na; kk++) {
                            const uint8_t *d = shmem + ao + kk * QGPU_NATIVE_DESC_WORDS * 4;
                            uint32_t sz = qgpu_ld32(d + 20);
                            fprintf(stderr, "   attr code %u buf %u off %u pas %u type %x taille %u%s\n",
                                    qgpu_ld32(d), qgpu_ld32(d + 4), qgpu_ld32(d + 8), qgpu_ld32(d + 12),
                                    qgpu_ld32(d + 16), sz & QGPU_NA_SIZE_MASK,
                                    (sz & QGPU_NA_NORMALIZED) ? " normalisé" : "");
                        }
                }
                if (op == QGPU_OP_DRAW_RAW || op == QGPU_OP_DRAW_RAW_BUF) {
                    static const char *fn[8] = { "REPLACE", "MODULATE", "ADD", "ADD_SIGNED", "INTERPOLATE", "SUBTRACT", "DOT3_RGB", "DOT3_RGBA" };
                    static const char *sn[8] = { "TEX", "CONST", "PRIM", "PREV", "TEX0", "TEX1", "TEX2", "TEX3" };
                    static const char *on[4] = { "c", "1-c", "a", "1-a" };
                    uint32_t u;
                    for (u = 0; u < QGPU_MAX_UNITS; u++) {
                        uint32_t cb = sk[QGPU_SK_COMBINE(u)], cs = sk[QGPU_SK_COMBINE_SRC(u)], i;
                        if (u >= 4 && !sk[QGPU_SK_UNIT(u) + QGPU_SK_U_ENABLE])
                            continue;
                        fprintf(stderr, "   u%u: %s(", u, fn[cb & 7]);
                        for (i = 0; i < 3; i++) { uint32_t v = (cs >> (5 * i)) & 0x1f; fprintf(stderr, "%s%s.%s", i ? "," : "", sn[v & 7], on[(v >> 3) & 3]); }
                        fprintf(stderr, ")x%u  A:%s(", 1u << ((cb >> 8) & 3), fn[(cb >> 4) & 7]);
                        for (i = 0; i < 3; i++) { uint32_t v = (cs >> (15 + 4 * i)) & 0xf; fprintf(stderr, "%s%s.%s", i ? "," : "", sn[v & 7], on[(v >> 3) & 1 ? 3 : 2]); }
                        fprintf(stderr, ")x%u  const %08x\n", 1u << ((cb >> 10) & 3),
                                sk[u == 0 ? QGPU_SK_TEX_ENV_COLOR : u == 1 ? QGPU_SK_TEX1_ENV_COLOR : u == 2 ? QGPU_SK_TEX2_ENV_COLOR : QGPU_SK_TEX3_ENV_COLOR]);
                    }
                }
                if ((op == QGPU_OP_DRAW_TRIANGLES_TEXN || op == QGPU_OP_DRAW_TRIANGLES_SEC) && len >= 4) {
                    /* chemin hérité : [nverts, off, nunits] ; sommets de 8 + 4n mots
                       (x y z f r g b a, puis s t r q par unité) — étendues (lot 11) */
                    uint32_t nv = a[1], off = a[2], nu = a[3], words = 8 + 4 * nu + (op == QGPU_OP_DRAW_TRIANGLES_SEC ? 3 : 0);
                    float mn[24], mx[24]; uint32_t v, w;
                    for (w = 0; w < 24; w++) { mn[w] = 1e30f; mx[w] = -1e30f; }
                    if (nv > 200000) nv = 200000;
                    for (v = 0; v < nv; v++) {
                        const uint8_t *vp = shmem + off + (size_t)v * words * 4;
                        if (off + ((size_t)v + 1) * words * 4 > SHMEM) break;
                        for (w = 0; w < words && w < 24; w++) {
                            float f = qgpu_u2f(qgpu_ld32(vp + w * 4));
                            if (f < mn[w]) mn[w] = f;
                            if (f > mx[w]) mx[w] = f;
                        }
                    }
                    fprintf(stderr, "LIST image %u %s : nverts %u nunits %u | x [%g,%g] y [%g,%g] z [%g,%g] | rgba [%g,%g] [%g,%g] [%g,%g] [%g,%g]",
                            h.frame, op == QGPU_OP_DRAW_TRIANGLES_TEXN ? "TEXN" : "SEC", a[1], nu,
                            mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], mn[4], mx[4], mn[5], mx[5], mn[6], mx[6], mn[7], mx[7]);
                    for (w = 0; w < nu && w < 4; w++)
                        fprintf(stderr, " | u%u s [%g,%g] t [%g,%g] r [%g,%g] q [%g,%g]", w,
                                mn[8 + 4 * w], mx[8 + 4 * w], mn[9 + 4 * w], mx[9 + 4 * w], mn[10 + 4 * w], mx[10 + 4 * w], mn[11 + 4 * w], mx[11 + 4 * w]);
                    fprintf(stderr, " | tex %u/%u %u/%u %u/%u %u/%u light %u blend %u %x/%x\n",
                            sk[QGPU_SK_TEXTURE], sk[QGPU_SK_TEX_BIND], sk[QGPU_SK_TEXTURE1], sk[QGPU_SK_TEX1_BIND],
                            sk[QGPU_SK_TEXTURE2], sk[QGPU_SK_TEX2_BIND], sk[QGPU_SK_TEXTURE3], sk[QGPU_SK_TEX3_BIND],
                            sk[QGPU_SK_LIGHTING], sk[QGPU_SK_BLEND], sk[QGPU_SK_BLEND_SRC_RGB], sk[QGPU_SK_BLEND_DST_RGB]);
                }
                if (op == QGPU_OP_SET_TEXGEN && len == QGPU_LEN_SET_TEXGEN)
                    fprintf(stderr, "LIST image %u SET_TEXGEN u%u coord %u actif %u mode %x\n",
                            h.frame, a[1], a[2], a[3], a[4]);
                /* v16 : programmes ARB */
                if (op == QGPU_OP_PROG_CREATE && len == QGPU_LEN_PROG_CREATE)
                    fprintf(stderr, "LIST image %u PROG_CREATE %u cible %x\n", h.frame, a[1], a[2]);
                if (op == QGPU_OP_PROG_STRING && len == QGPU_LEN_PROG_STRING) {
                    uint32_t n = a[2] < 40 ? a[2] : 40;
                    fprintf(stderr, "LIST image %u PROG_STRING %u len %u : %.*s%s\n", h.frame, a[1], a[2],
                            (int)n, a[3] + n <= SHMEM ? (const char *)shmem + a[3] : "", a[2] > 40 ? "…" : "");
                }
                if (op == QGPU_OP_PROG_BIND && len == QGPU_LEN_PROG_BIND)
                    fprintf(stderr, "LIST image %u PROG_BIND cible %x id %u\n", h.frame, a[1], a[2]);
                if ((op == QGPU_OP_PROG_ENV || op == QGPU_OP_PROG_LOCAL) && len == QGPU_LEN_PROG_PARAMS)
                    fprintf(stderr, "LIST image %u %s %x [%u..%u]\n", h.frame,
                            op == QGPU_OP_PROG_ENV ? "PROG_ENV cible" : "PROG_LOCAL id", a[1], a[2], a[2] + a[3]);
                if (op == QGPU_OP_SET_STATE && len == 3 &&
                    (a[1] == QGPU_SK_VERTEX_PROGRAM || a[1] == QGPU_SK_FRAGMENT_PROGRAM))
                    fprintf(stderr, "LIST image %u SET_STATE %s = %u\n", h.frame,
                            a[1] == QGPU_SK_VERTEX_PROGRAM ? "VERTEX_PROGRAM" : "FRAGMENT_PROGRAM", a[2]);
                if (op == QGPU_OP_DRAW_RAW && len == QGPU_LEN_DRAW_RAW) {
                    /* étendue des coordonnées de texture de chaque unité portée
                       par le format : 2D en [0,1] ou vecteurs 3D (lot 11) */
                    /* QGPU_CAP_GEN_SIZES : génériques à la taille déclarée par
                       la dernière clé QGPU_SK_GEN_SIZES vue (suivi grossier,
                       tous contextes confondus, comme sk[]) */
                    uint32_t gs = sk[QGPU_SK_GEN_SIZES];
                    uint32_t fmt = a[5], nv = a[9], words = QGPU_VF_WORDS_GS(fmt, gs), pas = a[4] ? a[4] : words;
                    uint32_t base = QGPU_VF_POS_COUNT(fmt) + ((fmt & QGPU_VF_NORMAL) ? 3 : 0) +
                                    ((fmt & QGPU_VF_COLOR) ? 4 : 0) + ((fmt & QGPU_VF_SEC_COLOR) ? 3 : 0) +
                                    ((fmt & QGPU_VF_FOG) ? 1 : 0);
                    uint32_t u, off = base;
                    if (nv > 200000) nv = 200000;
                    for (u = 0; u < QGPU_MAX_UNITS; u++) {       /* v17 : 8 unités */
                        float mn[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, mx[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
                        uint32_t v, c;
                        if (!(fmt & QGPU_VF_TEX(u))) continue;
                        for (v = 0; v < nv; v++) {
                            const uint8_t *vp = shmem + a[3] + ((size_t)v * pas + off) * 4;
                            if (a[3] + ((size_t)v * pas + off + 4) * 4 > SHMEM) break;
                            for (c = 0; c < 4; c++) {
                                float f = qgpu_u2f(qgpu_ld32(vp + c * 4));
                                if (f < mn[c]) mn[c] = f;
                                if (f > mx[c]) mx[c] = f;
                            }
                        }
                        fprintf(stderr, "   tc u%u : s [%g, %g] t [%g, %g] r [%g, %g] q [%g, %g]\n",
                                u, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], mn[3], mx[3]);
                        off += 4;
                    }
                    /* v16 : position (mots 0..) et attributs génériques, après les unités */
                    {
                        uint32_t k, o2 = 0, nc = QGPU_VF_POS_COUNT(fmt);
                        for (k = 0; k < 1 + QGPU_VF_GEN_MAX; k++) {
                            float mn[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, mx[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
                            uint32_t v, c, n4 = k ? (uint32_t)QGPU_GS_COUNT(gs, k - 1) : nc, at = k ? off : o2;
                            if (k && !(fmt & QGPU_VF_GEN(k - 1))) continue;
                            for (v = 0; v < nv; v++) {
                                const uint8_t *vp = shmem + a[3] + ((size_t)v * pas + at) * 4;
                                if (a[3] + ((size_t)v * pas + at + n4) * 4 > SHMEM) break;
                                for (c = 0; c < n4; c++) {
                                    float f = qgpu_u2f(qgpu_ld32(vp + c * 4));
                                    if (f < mn[c]) mn[c] = f;
                                    if (f > mx[c]) mx[c] = f;
                                }
                            }
                            if (k) { fprintf(stderr, "   gen %u (%u) : [%g, %g] [%g, %g] [%g, %g] [%g, %g]\n", k - 1,
                                             n4, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], mn[3], mx[3]); off += n4; }
                            else fprintf(stderr, "   pos (%u) : x [%g, %g] y [%g, %g] z [%g, %g] w [%g, %g]\n", nc,
                                         mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], mn[3], mx[3]);
                        }
                        /* QGPU_REPLAY_VERTS=n : les n premiers sommets, mot à mot (hexa et flottant) */
                        if (getenv("QGPU_REPLAY_VERTS")) {
                            uint32_t v, w, nvv = (uint32_t)atoi(getenv("QGPU_REPLAY_VERTS"));
                            for (v = 0; v < nvv && v < nv; v++) {
                                const uint8_t *vp = shmem + a[3] + (size_t)v * pas * 4;
                                fprintf(stderr, "   sommet %u :", v);
                                for (w = 0; w < words && w < 32; w++) {
                                    uint32_t u = qgpu_ld32(vp + w * 4);
                                    fprintf(stderr, " %08x(%g)", u, qgpu_u2f(u));
                                }
                                fprintf(stderr, "\n");
                            }
                        }
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
                uint32_t pdst = qgpu_ld32(shmem + h.base + (k + 2) * 4);
                uint32_t pstr = qgpu_ld32(shmem + h.base + (k + 3) * 4);
                uint32_t px = qgpu_ld32(shmem + h.base + (k + 4) * 4);
                uint32_t py = qgpu_ld32(shmem + h.base + (k + 5) * 4);
                uint32_t pw = qgpu_ld32(shmem + h.base + (k + 6) * 4);
                uint32_t ph = qgpu_ld32(shmem + h.base + (k + 7) * 4);
                uint32_t roff, pres[9];
                if (!pw || !ph || pw > 4096 || ph > 4096) { pw = 800; ph = 600; px = py = 0; }
                roff = SHMEM - 16384 - pw * ph * 4;
                rb[0] = QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER);
                rb[1] = surf; rb[2] = roff; rb[3] = pw * 4; rb[4] = px; rb[5] = py; rb[6] = pw; rb[7] = ph;
                for (j = 0; j < 8; j++) qgpu_st32(shmem + SHMEM - 8192 + j * 4, rb[j]);
                if (qgpu_core_execute(&c, SHMEM - 8192, 32) == QGPU_ST_OK) {
                    /* QGPU_REPLAY_PRESENTS=<fichier> : une ligne par image écrite,
                       « n image décalage_vram pas l h », pour situer l'image
                       sur l'écran de la VM (tools/matrice/) */
                    static FILE *pl;
                    if (!pl && getenv("QGPU_REPLAY_PRESENTS"))
                        pl = fopen(getenv("QGPU_REPLAY_PRESENTS"), "w");
                    if (pl) {
                        fprintf(pl, "%u %u %u %u %u %u\n", npresent, h.frame, pdst, pstr, pw, ph);
                        fflush(pl);
                    }
                    pres[0] = surf; pres[1] = roff; pres[2] = pw * 4; pres[3] = 0; pres[4] = 0;
                    pres[5] = pw; pres[6] = ph; pres[7] = QGPU_PF_XRGB8888;
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
