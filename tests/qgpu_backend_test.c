/*
 * qgpu_backend_test.c — les RÈGLES DE RASTÉRISATION que les deux backends
 * doivent tenir (lot 8 du bug hunt du 22/09/2026 : S1, S2, S3).
 *
 * Le harnais qgpu_core_test.c compare soft et GL sur des pixels choisis « à
 * l'intérieur des primitives, jamais sur une arête » : par construction, il
 * ne peut donc pas voir ce qui se passe SUR les arêtes. C'est précisément là
 * que vivent les trois findings :
 *
 *   S1 — règle du haut-gauche : un pixel posé sur une arête partagée n'est
 *        rastérisé qu'UNE fois. Mesure directe : un quad mélangé en ONE/ONE
 *        doit donner 0x40 partout, y compris sur sa diagonale, et non 0x80.
 *        Lignes et points passent par deux triangles : même mesure.
 *   S2 — lignes et points du chemin brut sont TEXTURÉS, comme en OpenGL.
 *   S3 — un w minuscule (1e-40, vu en vrai) ne doit ni escamoter la primitive
 *        ni la faire exploser : l'image doit être celle de la même scène sans
 *        le facteur d'échelle.
 *
 * Se compile et se lie exactement comme tests/qgpu_core_test.c :
 *   cc -std=gnu11 -O1 -pthread -I patches/qgpu tests/qgpu_backend_test.c \
 *      patches/qgpu/qgpu-{core,soft,gl}.c [-framework OpenGL | $(pkg-config --libs egl gl)] \
 *      -lm -o qgpu_backend_test && ./qgpu_backend_test
 *
 * Comme dans le device (v9), le backend naît sur un thread et tout le reste
 * se fait sur un autre.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qgpu-core.h"

#define SHMEM_SIZE   (1u << 20)
#define CMD_OFF      0x1000u
#define VTX_OFF      0x4000u
#define TEX_OFF      0x8000u
#define RB_OFF       0x10000u
#define W            64u
#define H            64u
#define STRIDE       (W * 4)
#define SURF         1u

static int failures;

#define CHECK(cond, ...) do { if (cond) { printf("  ok   " __VA_ARGS__); } \
    else { printf("  FAIL " __VA_ARGS__); failures++; } putchar('\n'); } while (0)

typedef struct { uint8_t *base; uint32_t off, start; } Emit;

static void emit(Emit *e, uint32_t v)
{
    qgpu_st32(e->base + e->off, v);
    e->off += 4;
}
static void emitf(Emit *e, float f) { emit(e, qgpu_f2u(f)); }

static uint32_t px(const uint8_t *shmem, uint32_t x, uint32_t y)
{
    return qgpu_ld32(shmem + RB_OFF + y * STRIDE + x * 4) & 0xFFFFFF;
}

static void state(Emit *e, uint32_t key, uint32_t val)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_SET_STATE, QGPU_LEN_SET_STATE));
    emit(e, key); emit(e, val);
}

static void statef(Emit *e, uint32_t key, float val)
{
    state(e, key, qgpu_f2u(val));
}

static void clear_cmd(Emit *e, uint32_t argb)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR));
    emit(e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH); emit(e, argb); emitf(e, 1.0f);
}

static void readback_cmd(Emit *e)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
    emit(e, SURF); emit(e, RB_OFF); emit(e, STRIDE); emit(e, 0); emit(e, 0);
    emit(e, W); emit(e, H);
}

/* Sommet du chemin existant : x y z w r g b a. */
static void vertex(Emit *e, float x, float y, float r, float g, float b)
{
    emitf(e, x); emitf(e, y); emitf(e, 0.0f); emitf(e, 1.0f);
    emitf(e, r); emitf(e, g); emitf(e, b); emitf(e, 1.0f);
}

static void draw(Emit *e, uint32_t op, uint32_t n)
{
    emit(e, QGPU_CMD_HDR(op, QGPU_LEN_DRAW));
    emit(e, n); emit(e, VTX_OFF);
}

/* [mode, n, voff, pas serré, format, ioff, itype, premier, nverts] */
static void draw_raw(Emit *e, uint32_t mode, uint32_t count, uint32_t fmt,
                     uint32_t nverts)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_DRAW_RAW, QGPU_LEN_DRAW_RAW));
    emit(e, mode); emit(e, count); emit(e, VTX_OFF); emit(e, 0); emit(e, fmt);
    emit(e, 0); emit(e, QGPU_IDX_NONE); emit(e, 0); emit(e, nverts);
}

static void set_matrix(Emit *e, uint32_t which, const float *m)
{
    int i;
    emit(e, QGPU_CMD_HDR(QGPU_OP_SET_MATRIX, QGPU_LEN_SET_MATRIX));
    emit(e, which);
    for (i = 0; i < 16; i++) {
        emitf(e, m[i]);
    }
}

/* glOrtho(0, W, H, 0, 0, −1) en ordre colonne : le chemin brut dessine alors
   dans les mêmes pixels que le chemin existant (y vers le bas). `k` met TOUTE
   la matrice à l'échelle : l'image projetée est la même, mais w le devient
   aussi — c'est le sujet de S3. */
static void mat_ortho_px(float *m, float k)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = k * 2.0f / (float)W;
    m[5] = k * -2.0f / (float)H;
    m[10] = k * 2.0f;
    m[12] = k * -1.0f;
    m[13] = k * 1.0f;
    m[14] = k * -1.0f;
    m[15] = k;
}

static void mat_identity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* Le pixel le plus clair de la surface, et le nombre de pixels qui ne valent
   ni le fond ni `want` : un double passage de mélange ONE/ONE se voit tout
   de suite dans l'un ou l'autre. */
static uint32_t brightest(const uint8_t *shmem, uint32_t *nbad, uint32_t want,
                          uint32_t bg)
{
    uint32_t x, y, mx = 0;
    *nbad = 0;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint32_t p = px(shmem, x, y);
            if (p > mx) {
                mx = p;
            }
            if (p != want && p != bg) {
                (*nbad)++;
            }
        }
    }
    return mx;
}

static uint32_t snap[W * H];

static void take_snap(const uint8_t *shmem)
{
    memcpy(snap, shmem + RB_OFF, sizeof(snap));
}

/* Comparaison MOT À MOT de la fenêtre de relecture, telle qu'elle est écrite
   (take_snap la recopie sans conversion : on compare donc les mêmes octets). */
static uint32_t snap_diff(const uint8_t *shmem)
{
    uint32_t i, n = 0, v;
    for (i = 0; i < W * H; i++) {
        memcpy(&v, shmem + RB_OFF + i * 4, sizeof(v));
        if (snap[i] != v) {
            n++;
        }
    }
    return n;
}

/* Une texture 1×1 d'un rouge franc, filtrée au plus proche : n'importe quelle
   coordonnée rend le même texel, donc le test ne dépend ni du filtrage ni de
   la façon dont chaque backend interpole sur un point ou une ligne. */
static void make_tex(Emit *e, Emit *t, uint32_t id)
{
    t->off = t->start = TEX_OFF;
    emit(t, 0xFFFF0000u);
    emit(e, QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX)); emit(e, id);
    emit(e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(e, id); emit(e, 0); emit(e, 1); emit(e, 1); emit(e, 0x1908); emit(e, TEX_OFF);
    emit(e, QGPU_CMD_HDR(QGPU_OP_TEX_PARAM, QGPU_LEN_TEX_PARAM));
    emit(e, id); emit(e, QGPU_TP_MIN_FILTER); emit(e, 0x2600);
    emit(e, QGPU_CMD_HDR(QGPU_OP_TEX_PARAM, QGPU_LEN_TEX_PARAM));
    emit(e, id); emit(e, QGPU_TP_MAG_FILTER); emit(e, 0x2600);
}

#define VF_P2C  ((uint32_t)(QGPU_VF_POS(2) | QGPU_VF_COLOR))
#define VF_P2CT ((uint32_t)(QGPU_VF_POS(2) | QGPU_VF_COLOR | QGPU_VF_TEX0))

/* ══════════════════════════ S1 : règle du haut-gauche ═════════════════════ */

static void run_s1(QgpuCore *c, uint8_t *shmem)
{
    Emit e = { shmem, CMD_OFF, CMD_OFF }, v = { shmem, VTX_OFF, VTX_OFF };
    uint32_t st, nbad, mx;

    /* Mélange ONE/ONE : chaque passage AJOUTE. Un quart de gris (0x40) passé
       une fois donne 0x40, passé deux fois 0x80 — la mesure est donc un
       COMPTEUR de passages, pixel par pixel. */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_BLEND, 1);
    state(&e, QGPU_SK_BLEND_SRC_RGB, 1);            /* GL_ONE */
    state(&e, QGPU_SK_BLEND_DST_RGB, 1);
    state(&e, QGPU_SK_BLEND_SRC_A, 1);
    state(&e, QGPU_SK_BLEND_DST_A, 1);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "(S1) mélange ONE/ONE posé (st %u)", st);

    /* (a) LE CAS DU RAPPORT : un quad en deux triangles, diagonale (0,0) →
       (64,64). Les centres de pixel x + 0,5 == y + 0,5 tombent EXACTEMENT sur
       elle. Avant la règle du haut-gauche, ils étaient rastérisés deux fois. */
    v.off = v.start = VTX_OFF;
    vertex(&v, 0, 0, 0.25f, 0.25f, 0.25f);
    vertex(&v, 64, 0, 0.25f, 0.25f, 0.25f);
    vertex(&v, 64, 64, 0.25f, 0.25f, 0.25f);
    vertex(&v, 0, 0, 0.25f, 0.25f, 0.25f);
    vertex(&v, 64, 64, 0.25f, 0.25f, 0.25f);
    vertex(&v, 0, 64, 0.25f, 0.25f, 0.25f);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, 0xFF000000);
    draw(&e, QGPU_OP_DRAW_TRIANGLES, 6);
    readback_cmd(&e);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    mx = brightest(shmem, &nbad, 0x404040, 0x404040);
    CHECK(st == QGPU_ST_OK && mx == 0x404040 && nbad == 0 &&
          px(shmem, 32, 32) == 0x404040 && px(shmem, 10, 40) == 0x404040,
          "(S1) diagonale d'un quad ONE/ONE : (32,32) = %06x, maximum %06x, "
          "%u pixel(s) hors 0x404040 (st %u)", px(shmem, 32, 32), mx, nbad, st);

    /* (b) Même mesure sur un quad du CHEMIN BRUT, dont la découpe et
       l'assemblage passent par le même rasteriseur. */
    {
        float m[16], mv[16];
        mat_ortho_px(m, 1.0f);
        mat_identity(mv);
        v.off = v.start = VTX_OFF;
        emitf(&v, 0); emitf(&v, 0); emitf(&v, .25f); emitf(&v, .25f); emitf(&v, .25f); emitf(&v, 1);
        emitf(&v, 64); emitf(&v, 0); emitf(&v, .25f); emitf(&v, .25f); emitf(&v, .25f); emitf(&v, 1);
        emitf(&v, 64); emitf(&v, 64); emitf(&v, .25f); emitf(&v, .25f); emitf(&v, .25f); emitf(&v, 1);
        emitf(&v, 0); emitf(&v, 64); emitf(&v, .25f); emitf(&v, .25f); emitf(&v, .25f); emitf(&v, 1);
        e.off = e.start = CMD_OFF;
        set_matrix(&e, QGPU_MTX_PROJECTION, m);
        set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
        clear_cmd(&e, 0xFF000000);
        draw_raw(&e, QGPU_PRIM_MODE_QUADS, 4, VF_P2C, 4);
        readback_cmd(&e);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        mx = brightest(shmem, &nbad, 0x404040, 0x404040);
        CHECK(st == QGPU_ST_OK && mx == 0x404040 && nbad == 0,
              "(S1) quad du chemin brut : maximum %06x, %u pixel(s) hors "
              "0x404040 (st %u)", mx, nbad, st);
    }

    /* (c) LIGNE LARGE. Le backend de référence la trace en deux triangles :
       sa diagonale était doublée sur toute la longueur du segment. Le segment
       est choisi pour que cette diagonale passe par des CENTRES de pixel —
       une ligne de 8 de large et de 8 de long, posée sur des demi-entiers,
       donne au parallélogramme une diagonale de pente 1 qui traverse (8,8),
       (9,9)… (15,15). */
    v.off = v.start = VTX_OFF;
    vertex(&v, 8.5f, 12.5f, 0.25f, 0.25f, 0.25f);
    vertex(&v, 16.5f, 12.5f, 0.25f, 0.25f, 0.25f);
    e.off = e.start = CMD_OFF;
    statef(&e, QGPU_SK_LINE_WIDTH, 8.0f);
    clear_cmd(&e, 0xFF000000);
    draw(&e, QGPU_OP_DRAW_LINES, 2);
    readback_cmd(&e);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    mx = brightest(shmem, &nbad, 0x404040, 0x000000);
    CHECK(st == QGPU_ST_OK && mx == 0x404040 && nbad == 0 &&
          px(shmem, 12, 12) == 0x404040,
          "(S1) ligne large ONE/ONE : (12,12) = %06x, maximum %06x, %u pixel(s) "
          "ni fond ni 0x404040 (st %u)", px(shmem, 12, 12), mx, nbad, st);

    /* (d) POINT : même décomposition en deux triangles. */
    v.off = v.start = VTX_OFF;
    vertex(&v, 32, 32, 0.25f, 0.25f, 0.25f);
    e.off = e.start = CMD_OFF;
    statef(&e, QGPU_SK_LINE_WIDTH, 1.0f);
    statef(&e, QGPU_SK_POINT_SIZE, 12.0f);
    clear_cmd(&e, 0xFF000000);
    draw(&e, QGPU_OP_DRAW_POINTS, 1);
    readback_cmd(&e);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    mx = brightest(shmem, &nbad, 0x404040, 0x000000);
    CHECK(st == QGPU_ST_OK && mx == 0x404040 && nbad == 0 &&
          px(shmem, 30, 30) == 0x404040,
          "(S1) point ONE/ONE : (30,30) = %06x, maximum %06x, %u pixel(s) ni "
          "fond ni 0x404040 (st %u)", px(shmem, 30, 30), mx, nbad, st);

    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_BLEND, 0);
    statef(&e, QGPU_SK_POINT_SIZE, 1.0f);
    qgpu_core_execute(c, CMD_OFF, e.off - e.start);
}

/* ══════════ S2 : lignes et points du chemin brut, texturés ════════════════ */

static void run_s2(QgpuCore *c, uint8_t *shmem)
{
    Emit e = { shmem, CMD_OFF, CMD_OFF }, v = { shmem, VTX_OFF, VTX_OFF };
    Emit t = { shmem, TEX_OFF, TEX_OFF };
    float m[16], mv[16];
    uint32_t st;

    mat_ortho_px(m, 1.0f);
    mat_identity(mv);
    e.off = e.start = CMD_OFF;
    make_tex(&e, &t, 7);
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    state(&e, QGPU_SK_TEXTURE, 1);
    state(&e, QGPU_SK_TEX_BIND, 7);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x1E01);              /* REPLACE */
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "(S2) texture rouge 1×1, REPLACE (st %u)", st);

    /* Sommets BLEUS, texture ROUGE : sous REPLACE, ce qui sort doit être la
       texture. Le backend OpenGL lie ses unités quelle que soit la primitive ;
       le backend de référence traçait lignes et points sans texture. */
    v.off = v.start = VTX_OFF;
    emitf(&v, 8); emitf(&v, 32); emitf(&v, 0); emitf(&v, 0); emitf(&v, 1); emitf(&v, 1);
    emitf(&v, 0.5f); emitf(&v, 0.5f); emitf(&v, 0); emitf(&v, 1);
    emitf(&v, 56); emitf(&v, 32); emitf(&v, 0); emitf(&v, 0); emitf(&v, 1); emitf(&v, 1);
    emitf(&v, 0.5f); emitf(&v, 0.5f); emitf(&v, 0); emitf(&v, 1);
    e.off = e.start = CMD_OFF;
    statef(&e, QGPU_SK_LINE_WIDTH, 5.0f);
    clear_cmd(&e, 0xFF000000);
    draw_raw(&e, QGPU_PRIM_MODE_LINES, 2, VF_P2CT, 2);
    readback_cmd(&e);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0xFF0000,
          "(S2) ligne brute texturée : (32,32) = %06x, attendu ff0000 (st %u)",
          px(shmem, 32, 32), st);

    v.off = v.start = VTX_OFF;
    emitf(&v, 32); emitf(&v, 32); emitf(&v, 0); emitf(&v, 0); emitf(&v, 1); emitf(&v, 1);
    emitf(&v, 0.5f); emitf(&v, 0.5f); emitf(&v, 0); emitf(&v, 1);
    e.off = e.start = CMD_OFF;
    statef(&e, QGPU_SK_LINE_WIDTH, 1.0f);
    statef(&e, QGPU_SK_POINT_SIZE, 10.0f);
    clear_cmd(&e, 0xFF000000);
    draw_raw(&e, QGPU_PRIM_MODE_POINTS, 1, VF_P2CT, 1);
    readback_cmd(&e);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0xFF0000,
          "(S2) point brut texturé : (32,32) = %06x, attendu ff0000 (st %u)",
          px(shmem, 32, 32), st);

    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_TEXTURE, 0);
    statef(&e, QGPU_SK_POINT_SIZE, 1.0f);
    qgpu_core_execute(c, CMD_OFF, e.off - e.start);
}

/* ═══════════ S3 : un w minuscule, et la division perspective ══════════════ */

static void run_s3(QgpuCore *c, uint8_t *shmem, const char *backend)
{
    Emit e = { shmem, CMD_OFF, CMD_OFF }, v = { shmem, VTX_OFF, VTX_OFF };
    float m[16], mv[16];
    uint32_t st, n;
    int i;
    /* Sommets volontairement hors des demi-pixels : aucune arête ne passe par
       un centre de pixel, donc la couverture ne tient qu'à la géométrie. */
    static const float tri[3][2] = { { 8.3f, 6.7f }, { 55.7f, 14.3f }, { 20.1f, 57.9f } };

    mat_identity(mv);
    v.off = v.start = VTX_OFF;
    for (i = 0; i < 3; i++) {
        emitf(&v, tri[i][0]); emitf(&v, tri[i][1]);
        emitf(&v, 0); emitf(&v, 1); emitf(&v, 0); emitf(&v, 1);
    }

    mat_ortho_px(m, 1.0f);
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    clear_cmd(&e, 0xFF000000);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 3);
    readback_cmd(&e);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    take_snap(shmem);
    CHECK(st == QGPU_ST_OK && px(shmem, 25, 25) == 0x00FF00 &&
          px(shmem, 2, 2) == 0x000000,
          "(S3) référence, w = 1 : dedans %06x, dehors %06x (st %u)",
          px(shmem, 25, 25), px(shmem, 2, 2), st);

    /* LE CAS. Toute la projection mise à l'échelle 1e-40 : les rapports x/w,
       y/w, z/w sont INCHANGÉS, donc l'image doit l'être aussi. Mais w vaut
       maintenant 1e-40, et « 1/w » déborde en binary32 : le sommet partait à
       l'infini, puis (int)ceilf(inf) — triangle disparu sur x86-64, triangle
       couvrant tout le ciseau sur aarch64. */
    mat_ortho_px(m, 1e-40f);
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    clear_cmd(&e, 0xFF000000);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 3);
    readback_cmd(&e);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    n = snap_diff(shmem);
    if (!strcmp(backend, "soft")) {
        CHECK(st == QGPU_ST_OK && n == 0,
              "(S3) w = 1e-40 : même image qu'à w = 1 (%u pixel(s) d'écart, "
              "dedans %06x, st %u)", n, px(shmem, 25, 25), st);
    } else {
        /* Sur le GPU hôte, la division perspective est câblée (souvent un
           RCP 32 bits) : on ne peut pas exiger le pixel près, seulement que
           la commande passe et que le triangle n'ait pas MANGÉ l'écran. */
        uint32_t x, y, full = 0;
        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                if (px(shmem, x, y) != 0x000000) {
                    full++;
                }
            }
        }
        CHECK(st == QGPU_ST_OK && full < W * H,
              "(S3) w = 1e-40 : commande acceptée, %u pixel(s) peints sur %u "
              "(écart au w = 1 : %u) (st %u)", full, W * H, n, st);
    }

    mat_ortho_px(m, 1.0f);
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    qgpu_core_execute(c, CMD_OFF, e.off - e.start);
}

typedef struct { QgpuCore *c; uint8_t *shmem; const char *name; } BackendRun;

static void *run_body(void *arg)
{
    BackendRun *r = arg;
    QgpuCore *c = r->c;
    uint8_t *shmem = r->shmem;
    Emit e = { shmem, CMD_OFF, CMD_OFF };
    uint32_t st;

    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, SURF); emit(&e, W); emit(&e, H); emit(&e, QGPU_FMT_XRGB8888);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, SURF);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "contexte et surface %ux%u (st %u)", W, H, st);

    run_s1(c, shmem);
    run_s2(c, shmem);
    run_s3(c, shmem, r->name);

    qgpu_core_reset(c);
    qgpu_core_fini(c);
    return NULL;
}

static void run_backend(const char *name)
{
    uint8_t *shmem = calloc(1, SHMEM_SIZE);
    static QgpuCore c;                  /* ~10 Mio (32 contextes par client, 26/09) : hors de la pile */
    BackendRun r = { &c, shmem, name };
    pthread_t th;

    printf("== backend %s\n", name);
    if (!shmem) {
        CHECK(0, "mémoire partagée du test");
        return;
    }
    if (!qgpu_core_init(&c, name, shmem, SHMEM_SIZE)) {
        printf("  –    indisponible sur cet hôte (ignoré)\n");
        free(shmem);
        return;
    }
    CHECK(!strcmp(c.be->name, name), "backend actif : %s", c.be->name);
    if (pthread_create(&th, NULL, run_body, &r) != 0) {
        CHECK(0, "thread de rendu du test");
        qgpu_core_fini(&c);
    } else {
        pthread_join(th, NULL);
    }
    free(shmem);
}

int main(void)
{
    run_backend("soft");
    run_backend("gl");
    printf("%s (%d échec(s))\n", failures ? "ÉCHEC" : "OK", failures);
    return failures ? 1 : 0;
}
