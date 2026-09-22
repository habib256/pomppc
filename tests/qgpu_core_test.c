/*
 * qgpu_core_test.c — test natif (hôte) du cœur qgpu et de ses backends.
 *
 * Exécute le même flux de commandes que tests/qgpu_smoke.py et que
 * guest/qgpu-test, mais sans QEMU ni invité : on lie directement
 * patches/qgpu/qgpu-core.c + qgpu-soft.c (+ qgpu-gl.c). Le backend logiciel
 * est la vérité terrain ; si le backend GL démarre sur cet hôte, il doit
 * produire les mêmes pixels aux points testés (intérieur des primitives et
 * fond — jamais sur une arête, où les règles de rastérisation diffèrent).
 *
 *   cc -pthread -I patches/qgpu tests/qgpu_core_test.c patches/qgpu/qgpu-{core,soft,gl}.c \
 *      [-framework OpenGL | -lEGL -lGL] -o qgpu_core_test && ./qgpu_core_test
 *
 * Comme dans le device (v9), le backend est initialisé sur un thread et tout le
 * reste — exécution, reset, libération — se fait sur UN AUTRE : c'est ce qui a
 * révélé qu'un contexte EGL laissé courant par l'initialisation ne peut plus
 * être pris par le thread de rendu (EGL_BAD_ACCESS sur le pilote NVIDIA ; CGL,
 * lui, ne l'interdit pas).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qgpu-core.h"

#define SHMEM_SIZE   (1u << 20)
#define CMD_OFF      0x1000u
#define VTX_OFF      0x4000u
#define RB_OFF       0x10000u
#define W            64u
#define H            64u
#define STRIDE       (W * 4)

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

static void vertex(Emit *e, float x, float y, float r, float g, float b)
{
    emitf(e, x); emitf(e, y); emitf(e, 0.0f); emitf(e, 1.0f);
    emitf(e, r); emitf(e, g); emitf(e, b); emitf(e, 1.0f);
}

/* RGB seulement : depuis la v2 l'octet haut est l'alpha. */
static uint32_t px(const uint8_t *shmem, uint32_t x, uint32_t y)
{
    return qgpu_ld32(shmem + RB_OFF + y * STRIDE + x * 4) & 0xFFFFFF;
}

static uint32_t pxa(const uint8_t *shmem, uint32_t x, uint32_t y)
{
    return qgpu_ld32(shmem + RB_OFF + y * STRIDE + x * 4);
}

static void vertexz(Emit *e, float x, float y, float z, float r, float g, float b, float a)
{
    emitf(e, x); emitf(e, y); emitf(e, z); emitf(e, 1.0f);
    emitf(e, r); emitf(e, g); emitf(e, b); emitf(e, a);
}

static void state(Emit *e, uint32_t key, uint32_t val)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_SET_STATE, QGPU_LEN_SET_STATE));
    emit(e, key); emit(e, val);
}

static void readback_cmd(Emit *e, uint32_t surf)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
    emit(e, surf); emit(e, RB_OFF); emit(e, STRIDE); emit(e, 0); emit(e, 0);
    emit(e, W); emit(e, H);
}

static void clear_cmd(Emit *e, uint32_t mask, uint32_t argb, float depth)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR));
    emit(e, mask); emit(e, argb); emitf(e, depth);
}

static void draw_cmd(Emit *e, uint32_t n)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW));
    emit(e, n); emit(e, VTX_OFF);
}

static void vertext(Emit *e, float x, float y, float r, float g, float b, float a,
                    float s, float t, float q)
{
    emitf(e, x); emitf(e, y); emitf(e, 0.0f); emitf(e, 1.0f);
    emitf(e, r); emitf(e, g); emitf(e, b); emitf(e, a);
    emitf(e, s * q); emitf(e, t * q); emitf(e, 0.0f); emitf(e, q);
}

/* Quad plein écran texturé (2 triangles), coordonnées 0..smax/0..tmax. */
static void tex_quad(Emit *v, float r, float g, float b, float a, float smax, float tmax)
{
    vertext(v, 0, 0, r, g, b, a, 0, 0, 1);
    vertext(v, 64, 0, r, g, b, a, smax, 0, 1);
    vertext(v, 64, 64, r, g, b, a, smax, tmax, 1);
    vertext(v, 0, 0, r, g, b, a, 0, 0, 1);
    vertext(v, 64, 64, r, g, b, a, smax, tmax, 1);
    vertext(v, 0, 64, r, g, b, a, 0, tmax, 1);
}

static void tparam(Emit *e, uint32_t tex, uint32_t key, uint32_t val)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_TEX_PARAM, QGPU_LEN_TEX_PARAM));
    emit(e, tex); emit(e, key); emit(e, val);
}

#define TEX_OFF 0x8000u

/* Tests v3 : textures, sur la surface 2. */
static void run_v3(QgpuCore *c, uint8_t *shmem)
{
    Emit e, v, t;
    uint32_t st;
    /* 2×2 : rouge, vert / bleu, blanc semi-transparent */
    static const uint32_t texels[4] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0x80FFFFFF };

    t.base = shmem; t.off = t.start = TEX_OFF;
    emit(&t, texels[0]); emit(&t, texels[1]); emit(&t, texels[2]); emit(&t, texels[3]);
    v.base = shmem; v.off = v.start = VTX_OFF;
    tex_quad(&v, 1, 1, 1, 1, 1, 1);
    e.base = shmem; e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX)); emit(&e, 5);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(&e, 5); emit(&e, 0); emit(&e, 2); emit(&e, 2); emit(&e, 0x1908); emit(&e, TEX_OFF);
    tparam(&e, 5, QGPU_TP_MIN_FILTER, 0x2600);
    tparam(&e, 5, QGPU_TP_MAG_FILTER, 0x2600);
    state(&e, QGPU_SK_TEXTURE, 1);
    state(&e, QGPU_SK_TEX_BIND, 5);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x1E01);          /* REPLACE */
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF000000, 1.0f);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_LEN_DRAW)); emit(&e, 6); emit(&e, VTX_OFF);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 10) == 0xFF0000 && px(shmem, 50, 10) == 0x00FF00 &&
          px(shmem, 10, 50) == 0x0000FF && px(shmem, 50, 50) == 0xFFFFFF,
          "REPLACE plus proche : %06x %06x %06x %06x (st %u)", px(shmem, 10, 10),
          px(shmem, 50, 10), px(shmem, 10, 50), px(shmem, 50, 50), st);

    /* MODULATE par un gris 50 % */
    v.off = v.start = VTX_OFF;
    tex_quad(&v, 0.5f, 0.5f, 0.5f, 1, 1, 1);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x2100);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_LEN_DRAW)); emit(&e, 6); emit(&e, VTX_OFF);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && (px(shmem, 10, 10) >> 16) > 120 && (px(shmem, 10, 10) >> 16) < 135 &&
          (px(shmem, 10, 10) & 0xFFFF) == 0,
          "MODULATE : %06x", px(shmem, 10, 10));

    /* REPEAT sur 2×2 : s de 0 à 2 → le motif se répète */
    v.off = v.start = VTX_OFF;
    tex_quad(&v, 1, 1, 1, 1, 2, 2);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x1E01);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_LEN_DRAW)); emit(&e, 6); emit(&e, VTX_OFF);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 5, 5) == 0xFF0000 && px(shmem, 37, 5) == 0xFF0000 &&
          px(shmem, 21, 5) == 0x00FF00, "REPEAT : %06x %06x %06x", px(shmem, 5, 5),
          px(shmem, 21, 5), px(shmem, 37, 5));

    /* CLAMP_TO_EDGE : au-delà de 1, la dernière colonne */
    e.off = e.start = CMD_OFF;
    tparam(&e, 5, QGPU_TP_WRAP_S, 0x812F);
    tparam(&e, 5, QGPU_TP_WRAP_T, 0x812F);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_LEN_DRAW)); emit(&e, 6); emit(&e, VTX_OFF);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 60, 5) == 0x00FF00 && px(shmem, 60, 60) == 0xFFFFFF,
          "CLAMP_TO_EDGE : %06x %06x", px(shmem, 60, 5), px(shmem, 60, 60));

    /* LINEAR : au centre, moyenne des 4 texels */
    v.off = v.start = VTX_OFF;
    tex_quad(&v, 1, 1, 1, 1, 1, 1);
    e.off = e.start = CMD_OFF;
    tparam(&e, 5, QGPU_TP_MAG_FILTER, 0x2601);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_LEN_DRAW)); emit(&e, 6); emit(&e, VTX_OFF);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    {
        uint32_t p = px(shmem, 32, 32);
        int r = p >> 16, g = (p >> 8) & 255, b = p & 255;
        CHECK(st == QGPU_ST_OK && r > 90 && r < 150 && g > 90 && g < 150 && b > 90 && b < 150,
              "LINEAR au centre : %06x", p);
    }

    /* coordonnées projectives : q = 2 partout, s/q identiques → même image */
    v.off = v.start = VTX_OFF;
    vertext(&v, 0, 0, 1, 1, 1, 1, 0, 0, 2);
    vertext(&v, 64, 0, 1, 1, 1, 1, 1, 0, 2);
    vertext(&v, 64, 64, 1, 1, 1, 1, 1, 1, 2);
    e.off = e.start = CMD_OFF;
    tparam(&e, 5, QGPU_TP_MAG_FILTER, 0x2600);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_LEN_DRAW)); emit(&e, 3); emit(&e, VTX_OFF);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 50, 10) == 0x00FF00 && px(shmem, 58, 50) == 0xFFFFFF,
          "q ≠ 1 : %06x %06x", px(shmem, 50, 10), px(shmem, 58, 50));

    /* LUMINANCE : L = R, en MODULATE sur du jaune */
    t.off = t.start = TEX_OFF;
    emit(&t, 0xFF808080);
    v.off = v.start = VTX_OFF;
    tex_quad(&v, 1, 1, 0, 1, 1, 1);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX)); emit(&e, 6);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(&e, 6); emit(&e, 0); emit(&e, 1); emit(&e, 1); emit(&e, 0x1909); emit(&e, TEX_OFF);
    tparam(&e, 6, QGPU_TP_MIN_FILTER, 0x2600);
    state(&e, QGPU_SK_TEX_BIND, 6);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x2100);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_LEN_DRAW)); emit(&e, 6); emit(&e, VTX_OFF);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    {
        uint32_t p = px(shmem, 32, 32);
        CHECK(st == QGPU_ST_OK && (p >> 16) > 120 && (p >> 16) < 136 && ((p >> 8) & 255) > 120 &&
              ((p >> 8) & 255) < 136 && (p & 255) == 0, "LUMINANCE × jaune : %06x", p);
    }

    /* texture incomplète (filtre mipmap par défaut, un seul niveau) : pas de texturage */
    t.off = t.start = TEX_OFF;
    emit(&t, 0xFF00FF00);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX)); emit(&e, 7);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(&e, 7); emit(&e, 0); emit(&e, 2); emit(&e, 2); emit(&e, 0x1908); emit(&e, TEX_OFF);
    state(&e, QGPU_SK_TEX_BIND, 7);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_LEN_DRAW)); emit(&e, 6); emit(&e, VTX_OFF);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0xFFFF00,
          "texture incomplète ignorée : %06x", px(shmem, 32, 32));

    /* mipmaps complets : réduction forte → dernier niveau (1×1 bleu) */
    t.off = t.start = TEX_OFF;
    emit(&t, 0xFF0000FF);
    v.off = v.start = VTX_OFF;
    tex_quad(&v, 1, 1, 1, 1, 64, 64);          /* 128 texels sur 64 px : λ = 1 */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(&e, 7); emit(&e, 1); emit(&e, 1); emit(&e, 1); emit(&e, 0x1908); emit(&e, TEX_OFF);
    tparam(&e, 7, QGPU_TP_MIN_FILTER, 0x2700);  /* NEAREST_MIPMAP_NEAREST */
    tparam(&e, 7, QGPU_TP_MAG_FILTER, 0x2600);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x1E01);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_LEN_DRAW)); emit(&e, 6); emit(&e, VTX_OFF);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0x0000FF,
          "mipmap : niveau 1 en réduction : %06x", px(shmem, 32, 32));

    /* validations */
    e.off = e.start = CMD_OFF;
    tparam(&e, 7, QGPU_TP_MAG_FILTER, 0x2700);  /* un filtre mipmap en agrandissement */
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "filtre d'agrandissement mipmap refusé : st %u", st);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(&e, 7); emit(&e, 0); emit(&e, 2048); emit(&e, 2048); emit(&e, 0x1908); emit(&e, SHMEM_SIZE - 16);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OOB, "texels hors fenêtre : st %u", st);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(&e, 7); emit(&e, 0); emit(&e, 1); emit(&e, 1); emit(&e, 0x1234); emit(&e, TEX_OFF);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "format de base inconnu refusé : st %u", st);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_TEX_BIND, QGPU_MAX_TEX);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "identifiant de texture hors bornes refusé : st %u", st);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_DESTROY, QGPU_LEN_TEX)); emit(&e, 5);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_DESTROY, QGPU_LEN_TEX)); emit(&e, 5);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG && c->status_pc == 2, "double destruction : st %u pc %u", st, c->status_pc);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_TEXTURE, 0);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
}

static void vert8(Emit *e, float x, float y, float z, float f, float r, float g, float b, float a)
{
    emitf(e, x); emitf(e, y); emitf(e, z); emitf(e, f);
    emitf(e, r); emitf(e, g); emitf(e, b); emitf(e, a);
}

static void draw_op(Emit *e, uint32_t op, uint32_t n)
{
    emit(e, QGPU_CMD_HDR(op, QGPU_LEN_DRAW));
    emit(e, n); emit(e, VTX_OFF);
}

/* Tests v4 : brouillard, 2e unité, lignes, points, décalage de polygone. */
static void run_v4(QgpuCore *c, uint8_t *shmem)
{
    Emit e, v, t;
    uint32_t st, i;

    /* brouillard : rouge, f = 0.5, brouillard bleu → violet à 50 % */
    v.base = shmem; v.off = v.start = VTX_OFF;
    vert8(&v, 0, 0, 0, 0.5f, 1, 0, 0, 1); vert8(&v, 64, 0, 0, 0.5f, 1, 0, 0, 1);
    vert8(&v, 0, 64, 0, 0.5f, 1, 0, 0, 1);
    e.base = shmem; e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_FOG, 1);
    state(&e, QGPU_SK_FOG_COLOR, 0xFF0000FF);
    draw_op(&e, QGPU_OP_DRAW_TRIANGLES, 3);
    state(&e, QGPU_SK_FOG, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    {
        uint32_t p = px(shmem, 8, 8), r = p >> 16, b = p & 255;
        CHECK(st == QGPU_ST_OK && r > 120 && r < 135 && b > 120 && b < 135,
              "brouillard f = 0.5 : %06x (st %u)", p, st);
    }

    /* deux unités : unité 0 blanche REPLACE, unité 1 1×1 (0.5, 1, 0) MODULATE */
    t.base = shmem; t.off = t.start = TEX_OFF;
    emit(&t, 0xFFFFFFFF); emit(&t, 0xFF80FF00);
    v.off = v.start = VTX_OFF;
    for (i = 0; i < 3; i++) {
        float xy[3][2] = { {0, 0}, {64, 0}, {0, 64} };
        vert8(&v, xy[i][0], xy[i][1], 0, 1, 1, 1, 1, 1);
        emitf(&v, 0.5f); emitf(&v, 0.5f); emitf(&v, 0); emitf(&v, 1);
        emitf(&v, 0.5f); emitf(&v, 0.5f); emitf(&v, 0); emitf(&v, 1);
    }
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX)); emit(&e, 10);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(&e, 10); emit(&e, 0); emit(&e, 1); emit(&e, 1); emit(&e, 0x1908); emit(&e, TEX_OFF);
    tparam(&e, 10, QGPU_TP_MIN_FILTER, 0x2600);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX)); emit(&e, 11);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(&e, 11); emit(&e, 0); emit(&e, 1); emit(&e, 1); emit(&e, 0x1907); emit(&e, TEX_OFF + 4);
    tparam(&e, 11, QGPU_TP_MIN_FILTER, 0x2600);
    state(&e, QGPU_SK_TEXTURE, 1); state(&e, QGPU_SK_TEX_BIND, 10);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x1E01);
    state(&e, QGPU_SK_TEXTURE1, 1); state(&e, QGPU_SK_TEX1_BIND, 11);
    state(&e, QGPU_SK_TEX1_ENV_MODE, 0x2100);
    draw_op(&e, QGPU_OP_DRAW_TRIANGLES_TEX2, 3);
    state(&e, QGPU_SK_TEXTURE, 0); state(&e, QGPU_SK_TEXTURE1, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    {
        uint32_t p = px(shmem, 8, 8);
        CHECK(st == QGPU_ST_OK && (p >> 16) > 120 && (p >> 16) < 136 && ((p >> 8) & 255) == 255 &&
              (p & 255) == 0, "deux unités (blanc × (0.5,1,0)) : %06x (st %u)", p, st);
    }

    /* lignes : horizontale y = 20.5, largeur 3 ; verticale x = 50.5, largeur 1 */
    v.off = v.start = VTX_OFF;
    vert8(&v, 4, 20.5f, 0, 1, 0, 1, 0, 1); vert8(&v, 40, 20.5f, 0, 1, 0, 1, 0, 1);
    vert8(&v, 50.5f, 30, 0, 1, 1, 1, 0, 1); vert8(&v, 50.5f, 60, 0, 1, 1, 1, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_LINE_WIDTH, qgpu_f2u(3.0f));
    draw_op(&e, QGPU_OP_DRAW_LINES, 2);
    state(&e, QGPU_SK_LINE_WIDTH, qgpu_f2u(1.0f));
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_LINES, QGPU_LEN_DRAW)); emit(&e, 2); emit(&e, VTX_OFF + 16 * 4);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 20, 20) == 0x00FF00 && px(shmem, 20, 21) == 0x00FF00 &&
          px(shmem, 20, 19) == 0x00FF00 && px(shmem, 20, 23) == 0 && px(shmem, 20, 17) == 0,
          "ligne de 3 px : %06x %06x %06x | %06x %06x (st %u)", px(shmem, 20, 19), px(shmem, 20, 20),
          px(shmem, 20, 21), px(shmem, 20, 17), px(shmem, 20, 23), st);
    CHECK(px(shmem, 50, 45) == 0xFFFF00 && px(shmem, 49, 45) == 0 && px(shmem, 51, 45) == 0,
          "ligne de 1 px : %06x | %06x %06x", px(shmem, 50, 45), px(shmem, 49, 45), px(shmem, 51, 45));

    /* points de 4 px centrés sur (10, 50) */
    v.off = v.start = VTX_OFF;
    vert8(&v, 10, 50, 0, 1, 1, 0, 1, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_POINT_SIZE, qgpu_f2u(4.0f));
    draw_op(&e, QGPU_OP_DRAW_POINTS, 1);
    state(&e, QGPU_SK_POINT_SIZE, qgpu_f2u(1.0f));
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 48) == 0xFF00FF && px(shmem, 11, 51) == 0xFF00FF &&
          px(shmem, 7, 50) == 0 && px(shmem, 12, 50) == 0,
          "point de 4 px : %06x %06x | %06x %06x", px(shmem, 8, 48), px(shmem, 11, 51),
          px(shmem, 7, 50), px(shmem, 12, 50));

    /* décalage de polygone : deux triangles coplanaires, le second (vert,
       décalé vers l'avant) passe le test LESS. */
    v.off = v.start = VTX_OFF;
    vert8(&v, 0, 0, 0.5f, 1, 1, 0, 0, 1); vert8(&v, 64, 0, 0.5f, 1, 1, 0, 0, 1); vert8(&v, 0, 64, 0.5f, 1, 1, 0, 0, 1);
    vert8(&v, 0, 0, 0.5f, 1, 0, 1, 0, 1); vert8(&v, 64, 0, 0.5f, 1, 0, 1, 0, 1); vert8(&v, 0, 64, 0.5f, 1, 0, 1, 0, 1);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_WRITE, 1);     /* fermé par un test v2 : l'effacement l'exige */
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_DEPTH_TEST, 1);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW)); emit(&e, 3); emit(&e, VTX_OFF);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW)); emit(&e, 3); emit(&e, VTX_OFF + 24 * 4);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0xFF0000, "sans décalage, le second échoue : %06x", px(shmem, 8, 8));
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_POLY_OFFSET, 1);
    state(&e, QGPU_SK_POLY_FACTOR, qgpu_f2u(0.0f));
    state(&e, QGPU_SK_POLY_UNITS, qgpu_f2u(-4.0f));
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW)); emit(&e, 3); emit(&e, VTX_OFF + 24 * 4);
    state(&e, QGPU_SK_POLY_OFFSET, 0);
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0x00FF00, "avec décalage, il passe : %06x", px(shmem, 8, 8));

    /* validations */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_LINE_WIDTH, qgpu_f2u(0.0f));
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "largeur de ligne nulle refusée : st %u", st);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_LINES, QGPU_LEN_DRAW)); emit(&e, 3); emit(&e, VTX_OFF);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "nombre impair de sommets de ligne refusé : st %u", st);
}

/* Tests v5 : quatre unités, GL_COMBINE. */
static int near8(uint32_t a, uint32_t b)
{
    int d = (int)a - (int)b;
    return d >= -2 && d <= 2;
}

static int near_rgb(uint32_t p, uint32_t want)
{
    p &= 0xFFFFFF;
    return near8(p >> 16, want >> 16) && near8((p >> 8) & 255, (want >> 8) & 255) &&
           near8(p & 255, want & 255);
}

static void texn_op(Emit *e, uint32_t n, uint32_t units)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEXN, QGPU_LEN_DRAW_N));
    emit(e, n); emit(e, VTX_OFF); emit(e, units);
}

static void tex1x1(Emit *e, uint32_t id, uint32_t fmt, uint32_t off)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX)); emit(e, id);
    emit(e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(e, id); emit(e, 0); emit(e, 1); emit(e, 1); emit(e, fmt); emit(e, off);
    tparam(e, id, QGPU_TP_MIN_FILTER, 0x2600);
    tparam(e, id, QGPU_TP_MAG_FILTER, 0x2600);
}

/* triangle plein écran, `units` unités de coordonnées (0.5, 0.5) */
static void tri_units(Emit *v, uint32_t units, float r, float g, float b, float a)
{
    static const float xy[3][2] = { {0, 0}, {64, 0}, {0, 64} };
    uint32_t i, u;
    v->off = v->start = VTX_OFF;
    for (i = 0; i < 3; i++) {
        vert8(v, xy[i][0], xy[i][1], 0, 1, r, g, b, a);
        for (u = 0; u < units; u++) {
            emitf(v, 0.5f); emitf(v, 0.5f); emitf(v, 0); emitf(v, 1);
        }
    }
}

static void run_v5(QgpuCore *c, uint8_t *shmem)
{
    Emit e, v, t;
    uint32_t st, p;

    t.base = shmem; t.off = t.start = TEX_OFF;
    emit(&t, 0x80803FFF);      /* 20 : RGBA (0.5, 0.25, 1), alpha 0.5 */
    emit(&t, 0xFF80FF00);      /* 21 : RGB (0.5, 1, 0) */
    emit(&t, 0xFFFF8080);      /* 22 : RGB (1, 0.5, 0.5) */
    v.base = shmem;
    e.base = shmem; e.off = e.start = CMD_OFF;
    tex1x1(&e, 20, 0x1908, TEX_OFF);
    tex1x1(&e, 21, 0x1907, TEX_OFF + 4);
    tex1x1(&e, 22, 0x1907, TEX_OFF + 8);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v5 : textures 1×1 (st %u)", st);

    /* ADD_SIGNED(texture, primaire) : (0.5+1-0.5, 0.25+0.5-0.5, 1+0-0.5) */
    tri_units(&v, 1, 1, 0.5f, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_TEXTURE, 1); state(&e, QGPU_SK_TEX_BIND, 20);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x8570);
    state(&e, QGPU_SK_COMBINE0, QGPU_COMBINE(QGPU_CB_ADD_SIGNED, QGPU_CB_REPLACE, 0, 0));
    state(&e, QGPU_SK_COMBINE_SRC0,
          QGPU_COMBINE_SRC_RGB(0, QGPU_CS_TEXTURE, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_RGB(1, QGPU_CS_PRIMARY, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_A(0, QGPU_CS_PRIMARY, QGPU_CA_ALPHA));
    texn_op(&e, 3, 1);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = pxa(shmem, 8, 8);
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0xFF4080) && (p >> 24) == 255,
          "COMBINE ADD_SIGNED : %08x (st %u)", p, st);

    /* INTERPOLATE(texture, constante, 1 − alpha texture) ×2 */
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_TEX_ENV_COLOR, 0xFF000000);
    state(&e, QGPU_SK_COMBINE0, QGPU_COMBINE(QGPU_CB_INTERPOLATE, QGPU_CB_MODULATE, 1, 0));
    state(&e, QGPU_SK_COMBINE_SRC0,
          QGPU_COMBINE_SRC_RGB(0, QGPU_CS_TEXTURE, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_RGB(1, QGPU_CS_CONSTANT, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_RGB(2, QGPU_CS_TEXTURE, QGPU_CO_ONE_MINUS_ALPHA) |
          QGPU_COMBINE_SRC_A(0, QGPU_CS_TEXTURE, QGPU_CA_ALPHA) |
          QGPU_COMBINE_SRC_A(1, QGPU_CS_PRIMARY, QGPU_CA_ONE_MINUS_ALPHA));
    texn_op(&e, 3, 1);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = pxa(shmem, 8, 8);
    /* a2 = 0.5 : rgb = (0.25, 0.125, 0.5) × 2 ; alpha = 0.5 × (1 − 1) = 0 */
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0x8040FF) && (p >> 24) <= 2,
          "COMBINE INTERPOLATE, échelle 2 : %08x (st %u)", p, st);

    /* DOT3_RGB(texture, primaire) : 4 × (0.25 + 0 + 0) = 1 */
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_TEX_BIND, 22);
    state(&e, QGPU_SK_COMBINE0, QGPU_COMBINE(QGPU_CB_DOT3_RGB, QGPU_CB_MODULATE, 0, 0));
    state(&e, QGPU_SK_COMBINE_SRC0, QGPU_COMBINE_SRC_RGB(0, QGPU_CS_TEXTURE, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_RGB(1, QGPU_CS_PRIMARY, QGPU_CO_COLOR));
    tri_units(&v, 1, 1, 0.5f, 0.5f, 1);
    texn_op(&e, 3, 1);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 8, 8);
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0xFFFFFF), "COMBINE DOT3_RGB : %06x (st %u)", p, st);

    /* unités 2 et 3 seules : MODULATE (0.5, 1, 0), puis SUBTRACT(précédent, constante) */
    tri_units(&v, 4, 1, 1, 1, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_TEXTURE, 0);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x2100);
    state(&e, QGPU_SK_COMBINE0, QGPU_COMBINE_DEFAULT);
    state(&e, QGPU_SK_COMBINE_SRC0, QGPU_COMBINE_SRC_DEFAULT);
    state(&e, QGPU_SK_TEXTURE2, 1); state(&e, QGPU_SK_TEX2_BIND, 21);
    state(&e, QGPU_SK_TEXTURE3, 1); state(&e, QGPU_SK_TEX3_BIND, 21);
    state(&e, QGPU_SK_TEX3_ENV_MODE, 0x8570);
    state(&e, QGPU_SK_TEX3_ENV_COLOR, 0xFF400000);
    state(&e, QGPU_SK_COMBINE0 + 3, QGPU_COMBINE(QGPU_CB_SUBTRACT, QGPU_CB_REPLACE, 0, 0));
    state(&e, QGPU_SK_COMBINE_SRC0 + 3,
          QGPU_COMBINE_SRC_RGB(0, QGPU_CS_PREVIOUS, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_RGB(1, QGPU_CS_CONSTANT, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_A(0, QGPU_CS_PREVIOUS, QGPU_CA_ALPHA));
    texn_op(&e, 3, 4);
    state(&e, QGPU_SK_TEXTURE2, 0); state(&e, QGPU_SK_TEXTURE3, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 8, 8);
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0x40FF00), "unités 2 et 3 : %06x (st %u)", p, st);

    /* validations */
    e.off = e.start = CMD_OFF;
    texn_op(&e, 3, 5);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "5 unités refusées : st %u", st);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_COMBINE0, QGPU_COMBINE(QGPU_CB_MODULATE, QGPU_CB_MODULATE, 3, 0));
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "échelle 8 refusée : st %u", st);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_COMBINE0, QGPU_COMBINE(QGPU_CB_MODULATE, QGPU_CB_DOT3_RGB, 0, 0));
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "DOT3 en alpha refusé : st %u", st);
}

/* ───────────────────────────── v6 : stencil ─────────────────────────────
 *
 * Sur une NOUVELLE surface couleur + profondeur + stencil (id 3). Comme
 * ailleurs dans ce fichier, aucun point testé n'est sur une arête : les règles
 * de remplissage diffèrent entre le rasteriseur logiciel et le GPU hôte.
 * Points de référence : (8,8) dans le quart haut-gauche, (16,32) hors de ce
 * quart mais dans le triangle plein, (56,56) dans le quart bas-droit. */
static uint32_t sten(const uint8_t *shmem, uint32_t x, uint32_t y)
{
    return qgpu_ld32(shmem + RB_OFF + y * STRIDE + x * 4);
}

static void sten_xfer(Emit *e, uint32_t op, uint32_t surf)
{
    emit(e, QGPU_CMD_HDR(op, QGPU_LEN_SURF_XFER));
    emit(e, surf); emit(e, RB_OFF); emit(e, STRIDE); emit(e, 0); emit(e, 0);
    emit(e, W); emit(e, H);
}

static void sop(Emit *e, uint32_t fail, uint32_t zfail, uint32_t zpass)
{
    state(e, QGPU_SK_STENCIL_OP_FAIL, fail);
    state(e, QGPU_SK_STENCIL_OP_ZFAIL, zfail);
    state(e, QGPU_SK_STENCIL_OP_ZPASS, zpass);
}

/* Triangle rectangle de côté `side` au coin haut-gauche, puis au coin bas-droit. */
static void tri_tl(Emit *v, float side, float z, float r, float g, float b)
{
    vertexz(v, 0, 0, z, r, g, b, 1);
    vertexz(v, side, 0, z, r, g, b, 1);
    vertexz(v, 0, side, z, r, g, b, 1);
}

static void tri_br(Emit *v, float side, float z, float r, float g, float b)
{
    vertexz(v, W, H, z, r, g, b, 1);
    vertexz(v, W - side, H, z, r, g, b, 1);
    vertexz(v, W, H - side, z, r, g, b, 1);
}

/* Le second triangle du tampon de sommets : 3 sommets de 8 mots. */
#define VTX2 (VTX_OFF + 24 * 4)

static void draw_second(Emit *e)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW));
    emit(e, 3); emit(e, VTX2);
}

static void run_v6(QgpuCore *c, uint8_t *shmem)
{
    Emit e, v;
    uint32_t st, i;

    e.base = shmem; e.off = e.start = CMD_OFF;
    v.base = shmem;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 3); emit(&e, W); emit(&e, H);
    emit(&e, QGPU_FMT_XRGB8888 | QGPU_FMT_FLAG_DEPTH | QGPU_FMT_FLAG_STENCIL);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v6 : surface couleur + profondeur + stencil (st %u)", st);

    /* (a) masquage classique : un quart marque le stencil à 1 (couleur fermée),
       puis un triangle plein ne peint que là où le stencil vaut 1. */
    v.off = v.start = VTX_OFF;
    tri_tl(&v, 32, 0, 1, 1, 1);
    v.off = v.start = VTX2;
    tri_tl(&v, 64, 0, 1, 1, 1);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    state(&e, QGPU_SK_DEPTH_WRITE, 1);
    state(&e, QGPU_SK_STENCIL_CLEAR, 0);
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH | QGPU_CLEAR_STENCIL, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_STENCIL_TEST, 1);
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0207);          /* ALWAYS */
    state(&e, QGPU_SK_STENCIL_REF, 1);
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_REPLACE);
    state(&e, QGPU_SK_COLOR_MASK, 0x0);
    draw_cmd(&e, 3);
    state(&e, QGPU_SK_COLOR_MASK, 0xF);
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0202);          /* EQUAL */
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_KEEP);
    draw_second(&e);
    readback_cmd(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0xFFFFFF && px(shmem, 16, 32) == 0 &&
          px(shmem, 50, 50) == 0,
          "masquage EQUAL 1 : %06x (dedans) %06x (hors du masque) %06x (fond) (st %u)",
          px(shmem, 8, 8), px(shmem, 16, 32), px(shmem, 50, 50), st);

    /* (b) INCR deux fois (stencil = 2 dans le quart), puis LESS 1 et GREATER 1. */
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_STENCIL, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0207);
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_INCR);
    state(&e, QGPU_SK_COLOR_MASK, 0x0);
    draw_cmd(&e, 3);
    draw_cmd(&e, 3);
    state(&e, QGPU_SK_COLOR_MASK, 0xF);
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_KEEP);
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0201);          /* LESS : 1 < 2 dedans */
    draw_second(&e);
    readback_cmd(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0xFFFFFF && px(shmem, 16, 32) == 0,
          "INCR ×2 puis LESS 1 : %06x %06x (st %u)",
          px(shmem, 8, 8), px(shmem, 16, 32), st);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0204);          /* GREATER : 1 > 0 hors du quart */
    draw_second(&e);
    readback_cmd(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0 && px(shmem, 16, 32) == 0xFFFFFF,
          "GREATER 1 : le complément : %06x %06x (st %u)",
          px(shmem, 8, 8), px(shmem, 16, 32), st);

    /* (b bis) saturation de INCR/DECR, bouclage des variantes _WRAP, INVERT.
       Le triangle plein est toujours au second emplacement de sommets. */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_STENCIL_CLEAR, 0);
    clear_cmd(&e, QGPU_CLEAR_STENCIL, 0, 1.0f);
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0207);
    state(&e, QGPU_SK_COLOR_MASK, 0x0);
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_DECR);
    draw_second(&e);                                  /* 0 − 1 sature à 0 */
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_DECR_WRAP);
    draw_second(&e);                                  /* 0 − 1 boucle à 255 */
    sten_xfer(&e, QGPU_OP_STENCIL_READBACK, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && sten(shmem, 8, 8) == 255 && sten(shmem, 16, 32) == 255,
          "DECR sature puis DECR_WRAP boucle : %u %u (st %u)",
          sten(shmem, 8, 8), sten(shmem, 16, 32), st);
    e.off = e.start = CMD_OFF;
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_INCR);
    draw_second(&e);                                  /* 255 + 1 sature à 255 */
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_INCR_WRAP);
    draw_second(&e);                                  /* 255 + 1 boucle à 0 */
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_INVERT);
    draw_second(&e);                                  /* ~0 = 255 */
    state(&e, QGPU_SK_COLOR_MASK, 0xF);
    sten_xfer(&e, QGPU_OP_STENCIL_READBACK, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && sten(shmem, 8, 8) == 255 && sten(shmem, 16, 32) == 255,
          "INCR sature, INCR_WRAP boucle, INVERT : %u %u (st %u)",
          sten(shmem, 8, 8), sten(shmem, 16, 32), st);

    /* (c) zfail contre zpass, profondeur active : le quart haut-gauche est
       derrière le fond effacé (échec LESS → zfail = INCR), le quart bas-droit
       devant (zpass = REPLACE par REF = 5). */
    v.off = v.start = VTX_OFF;
    tri_tl(&v, 32, 0.8f, 1, 0, 0);
    tri_br(&v, 32, 0.2f, 0, 1, 0);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_STENCIL_CLEAR, 0);
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH | QGPU_CLEAR_STENCIL, 0xFF000000, 0.5f);
    state(&e, QGPU_SK_DEPTH_TEST, 1);
    state(&e, QGPU_SK_DEPTH_FUNC, 0x0201);            /* LESS */
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0207);
    state(&e, QGPU_SK_STENCIL_REF, 5);
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_INCR, QGPU_SOP_REPLACE);
    state(&e, QGPU_SK_COLOR_MASK, 0x0);
    draw_cmd(&e, 3);
    draw_second(&e);
    state(&e, QGPU_SK_COLOR_MASK, 0xF);
    sten_xfer(&e, QGPU_OP_STENCIL_READBACK, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && sten(shmem, 8, 8) == 1 && sten(shmem, 56, 56) == 5 &&
          sten(shmem, 16, 32) == 0,
          "zfail = INCR, zpass = REPLACE : %u %u %u (st %u)",
          sten(shmem, 8, 8), sten(shmem, 56, 56), sten(shmem, 16, 32), st);

    /* (f) STENCIL_UPLOAD : 3 partout, relu tel quel, et le test le voit. */
    for (i = 0; i < W * H; i++) {
        qgpu_st32(shmem + RB_OFF + i * 4, 3);
    }
    e.off = e.start = CMD_OFF;
    sten_xfer(&e, QGPU_OP_STENCIL_UPLOAD, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    /* deux soumissions : la zone de relecture est aussi celle de l'envoi, on la
       salit entre les deux pour ne rien relire de rémanent. */
    for (i = 0; i < W * H; i++) {
        qgpu_st32(shmem + RB_OFF + i * 4, 0xDEADBEEF);
    }
    e.off = e.start = CMD_OFF;
    sten_xfer(&e, QGPU_OP_STENCIL_READBACK, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && sten(shmem, 8, 8) == 3 && sten(shmem, 56, 56) == 3 &&
          sten(shmem, 16, 32) == 3,
          "STENCIL_UPLOAD puis relecture : %u %u %u (st %u)",
          sten(shmem, 8, 8), sten(shmem, 56, 56), sten(shmem, 16, 32), st);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0202);          /* EQUAL 3 */
    state(&e, QGPU_SK_STENCIL_REF, 3);
    v.off = v.start = VTX2;
    tri_tl(&v, 64, 0, 0, 1, 1);
    draw_second(&e);
    readback_cmd(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0x00FFFF && px(shmem, 16, 32) == 0x00FFFF,
          "le stencil envoyé pilote le test : %06x %06x (st %u)",
          px(shmem, 8, 8), px(shmem, 16, 32), st);

    /* (d) masque d'écriture : REPLACE de 0xFF à travers 0x0F ne pose que 0x0F. */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_STENCIL_CLEAR, 0);
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_STENCIL, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_STENCIL_WRITE_MASK, 0x0F);
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0207);
    state(&e, QGPU_SK_STENCIL_REF, 0xFF);
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_REPLACE);
    state(&e, QGPU_SK_COLOR_MASK, 0x0);
    draw_second(&e);
    state(&e, QGPU_SK_COLOR_MASK, 0xF);
    state(&e, QGPU_SK_STENCIL_WRITE_MASK, 0xFF);
    sten_xfer(&e, QGPU_OP_STENCIL_READBACK, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && sten(shmem, 8, 8) == 0x0F && sten(shmem, 16, 32) == 0x0F &&
          sten(shmem, 50, 50) == 0,
          "masque d'écriture 0x0F : %02x %02x %02x (st %u)",
          sten(shmem, 8, 8), sten(shmem, 16, 32), sten(shmem, 50, 50), st);

    /* (d bis) masque de valeur : EQUAL 0xFF passe à travers 0x0F, pas à travers 0xFF. */
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_KEEP);
    state(&e, QGPU_SK_STENCIL_VALUE_MASK, 0x0F);
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0202);
    draw_second(&e);
    readback_cmd(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0x00FFFF,
          "masque de valeur 0x0F : EQUAL 0xFF passe : %06x (st %u)", px(shmem, 8, 8), st);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_STENCIL_VALUE_MASK, 0xFF);
    draw_second(&e);
    readback_cmd(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0,
          "masque de valeur 0xFF : EQUAL 0xFF échoue : %06x (st %u)", px(shmem, 8, 8), st);

    /* (e) effacement à une valeur non nulle, limité par les ciseaux. */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_STENCIL_CLEAR, 0);
    clear_cmd(&e, QGPU_CLEAR_STENCIL, 0, 1.0f);
    state(&e, QGPU_SK_STENCIL_CLEAR, 0x42);
    state(&e, QGPU_SK_SCISSOR, 1);
    state(&e, QGPU_SK_SCISSOR_X, 0); state(&e, QGPU_SK_SCISSOR_Y, 0);
    state(&e, QGPU_SK_SCISSOR_W, 8); state(&e, QGPU_SK_SCISSOR_H, 8);
    clear_cmd(&e, QGPU_CLEAR_STENCIL, 0, 1.0f);
    state(&e, QGPU_SK_SCISSOR, 0);
    sten_xfer(&e, QGPU_OP_STENCIL_READBACK, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && sten(shmem, 2, 2) == 0x42 && sten(shmem, 20, 20) == 0,
          "effacement du stencil à 0x42 sous ciseaux : %02x %02x (st %u)",
          sten(shmem, 2, 2), sten(shmem, 20, 20), st);
    /* et il respecte le masque d'écriture, comme glClear */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_STENCIL_WRITE_MASK, 0xF0);
    state(&e, QGPU_SK_STENCIL_CLEAR, 0x0F);
    clear_cmd(&e, QGPU_CLEAR_STENCIL, 0, 1.0f);
    state(&e, QGPU_SK_STENCIL_WRITE_MASK, 0xFF);
    sten_xfer(&e, QGPU_OP_STENCIL_READBACK, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && sten(shmem, 2, 2) == 0x02 && sten(shmem, 20, 20) == 0,
          "effacement à travers le masque 0xF0 : %02x %02x (st %u)",
          sten(shmem, 2, 2), sten(shmem, 20, 20), st);

    /* La profondeur doit continuer à marcher sur un tampon combiné : c'est le
       chemin que le backend GL a dû refaire (DEPTH24_STENCIL8). */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_STENCIL_TEST, 0);
    state(&e, QGPU_SK_DEPTH_WRITE, 1);
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_DEPTH_TEST, 1);
    v.off = v.start = VTX2;
    tri_tl(&v, 64, 0.25f, 1, 0, 0);
    draw_second(&e);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 3); emit(&e, RB_OFF); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0);
    emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    {
        float d = qgpu_u2f(qgpu_ld32(shmem + RB_OFF + 8 * STRIDE + 8 * 4));
        float bg = qgpu_u2f(qgpu_ld32(shmem + RB_OFF + 50 * STRIDE + 50 * 4));
        CHECK(st == QGPU_ST_OK && d > 0.24f && d < 0.26f && bg > 0.999f,
              "profondeur sur surface avec stencil : %g (triangle) %g (fond)", d, bg);
    }
    for (i = 0; i < W * H; i++) {
        qgpu_st32(shmem + RB_OFF + i * 4, qgpu_f2u(0.1f));
    }
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_UPLOAD, QGPU_LEN_SURF_XFER));
    emit(&e, 3); emit(&e, RB_OFF); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0);
    emit(&e, W); emit(&e, H);
    v.off = v.start = VTX2;
    tri_tl(&v, 64, 0.15f, 0, 1, 0);
    draw_second(&e);
    readback_cmd(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0xFF0000,
          "DEPTH_UPLOAD sur tampon combiné respecté : %06x (st %u)", px(shmem, 8, 8), st);
    /* et il n'a pas écrasé le stencil (0x02 depuis l'effacement masqué) */
    e.off = e.start = CMD_OFF;
    sten_xfer(&e, QGPU_OP_STENCIL_READBACK, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && sten(shmem, 2, 2) == 0x02,
          "DEPTH_UPLOAD préserve le stencil : %02x (st %u)", sten(shmem, 2, 2), st);
    /* et réciproquement : STENCIL_UPLOAD ne doit pas toucher la profondeur
       (0,1 partout depuis le DEPTH_UPLOAD ci-dessus). */
    for (i = 0; i < W * H; i++) {
        qgpu_st32(shmem + RB_OFF + i * 4, 0x55);
    }
    e.off = e.start = CMD_OFF;
    sten_xfer(&e, QGPU_OP_STENCIL_UPLOAD, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 3); emit(&e, RB_OFF); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0);
    emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    {
        float d = qgpu_u2f(qgpu_ld32(shmem + RB_OFF + 8 * STRIDE + 8 * 4));
        CHECK(st == QGPU_ST_OK && d > 0.09f && d < 0.11f,
              "STENCIL_UPLOAD préserve la profondeur : %g (st %u)", d, st);
    }

    /* (g) validations */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_STENCIL_FUNC, 0x1234);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "fonction de stencil invalide refusée : st %u", st);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_STENCIL_OP_ZPASS, 0x1234);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "opération de stencil invalide refusée : st %u", st);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_STENCIL_REF, 256);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "REF hors de 0..255 refusé : st %u", st);
    e.off = e.start = CMD_OFF;
    sten_xfer(&e, QGPU_OP_STENCIL_READBACK, 2);       /* surface 2 : profondeur seule */
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "relecture de stencil sans tampon refusée : st %u", st);
    e.off = e.start = CMD_OFF;
    sten_xfer(&e, QGPU_OP_STENCIL_UPLOAD, 1);         /* surface 1 : couleur seule */
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "envoi de stencil sans tampon refusé : st %u", st);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 4); emit(&e, W); emit(&e, H);
    emit(&e, QGPU_FMT_XRGB8888 | QGPU_FMT_FLAG_STENCIL);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "stencil sans profondeur refusé : st %u", st);

    /* état rendu au repos pour la suite */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    state(&e, QGPU_SK_STENCIL_TEST, 0);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
}

/* ═══════════════════════ v7 : géométrie brute ═══════════════════════════════
 *
 * Tout passe par DRAW_RAW : c'est l'hôte qui transforme, éclaire, découpe et
 * aplatit. Deux règles de lecture :
 *   - aucun point testé n'est sur une arête (les règles de remplissage
 *     diffèrent entre le rasteriseur de référence et le GPU) ;
 *   - la projection « pixels » mat_ortho_px est glOrtho(0, W, H, 0, 0, −1) :
 *     un sommet (x, y, z) y donne le pixel (x, y) et la profondeur fenêtre z,
 *     exactement comme le chemin DRAW_TRIANGLES. C'est elle qui rend les deux
 *     chemins comparables, et c'est celle que le plugin invité utilisera.
 */
#define IDX_OFF 0xC000u

static void mat_identity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* glOrtho, en ordre colonne. */
static void mat_ortho(float *m, float l, float r, float b, float t, float n, float f)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / (r - l);
    m[5] = 2.0f / (t - b);
    m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    m[15] = 1.0f;
}

static void mat_ortho_px(float *m)
{
    mat_ortho(m, 0.0f, (float)W, (float)H, 0.0f, 0.0f, -1.0f);
}

/* glFrustum, en ordre colonne. */
static void mat_frustum(float *m, float l, float r, float b, float t, float n, float f)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f * n / (r - l);
    m[5] = 2.0f * n / (t - b);
    m[8] = (r + l) / (r - l);
    m[9] = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -2.0f * f * n / (f - n);
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

static void set_current(Emit *e, uint32_t what, float x, float y, float z, float w)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_SET_CURRENT, QGPU_LEN_SET_CURRENT));
    emit(e, what); emitf(e, x); emitf(e, y); emitf(e, z); emitf(e, w);
}

static void set_light(Emit *e, uint32_t i, uint32_t on, const float *amb,
                      const float *dif, const float *spec, const float *pos,
                      const float *sdir, float sexp, float scut,
                      float a0, float a1, float a2)
{
    int k;
    emit(e, QGPU_CMD_HDR(QGPU_OP_SET_LIGHT, QGPU_LEN_SET_LIGHT));
    emit(e, i); emit(e, on);
    for (k = 0; k < 4; k++) { emitf(e, amb[k]); }
    for (k = 0; k < 4; k++) { emitf(e, dif[k]); }
    for (k = 0; k < 4; k++) { emitf(e, spec[k]); }
    for (k = 0; k < 4; k++) { emitf(e, pos[k]); }
    for (k = 0; k < 3; k++) { emitf(e, sdir[k]); }
    emitf(e, sexp); emitf(e, scut);
    emitf(e, a0); emitf(e, a1); emitf(e, a2);
}

static void set_material(Emit *e, uint32_t face, const float *amb, const float *dif,
                         const float *spec, const float *emi, float shin)
{
    int k;
    emit(e, QGPU_CMD_HDR(QGPU_OP_SET_MATERIAL, QGPU_LEN_SET_MATERIAL));
    emit(e, face);
    for (k = 0; k < 4; k++) { emitf(e, amb[k]); }
    for (k = 0; k < 4; k++) { emitf(e, dif[k]); }
    for (k = 0; k < 4; k++) { emitf(e, spec[k]); }
    for (k = 0; k < 4; k++) { emitf(e, emi[k]); }
    emitf(e, shin);
}

static void set_light_model(Emit *e, float r, float g, float b, float a)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_SET_LIGHT_MODEL, QGPU_LEN_SET_LIGHT_MODEL));
    emitf(e, r); emitf(e, g); emitf(e, b); emitf(e, a);
}

static void set_texgen(Emit *e, uint32_t unit, uint32_t coord, uint32_t on,
                       uint32_t mode, const float *obj, const float *eye)
{
    int k;
    emit(e, QGPU_CMD_HDR(QGPU_OP_SET_TEXGEN, QGPU_LEN_SET_TEXGEN));
    emit(e, unit); emit(e, coord); emit(e, on); emit(e, mode);
    for (k = 0; k < 4; k++) { emitf(e, obj[k]); }
    for (k = 0; k < 4; k++) { emitf(e, eye[k]); }
}

static void set_clip_plane(Emit *e, uint32_t i, uint32_t on,
                           float a, float b, float c, float d)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_SET_CLIP_PLANE, QGPU_LEN_SET_CLIP_PLANE));
    emit(e, i); emit(e, on);
    emitf(e, a); emitf(e, b); emitf(e, c); emitf(e, d);
}

/* [mode, n, voff, pas serré, format, ioff, itype, premier, nverts] */
static void draw_raw(Emit *e, uint32_t mode, uint32_t count, uint32_t fmt,
                     uint32_t nverts, uint32_t itype, uint32_t ioff)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_DRAW_RAW, QGPU_LEN_DRAW_RAW));
    emit(e, mode); emit(e, count); emit(e, VTX_OFF); emit(e, 0); emit(e, fmt);
    emit(e, ioff); emit(e, itype); emit(e, 0); emit(e, nverts);
}

/* Sommets bruts, format par format. */
static void rv2(Emit *v, float x, float y) { emitf(v, x); emitf(v, y); }

static void rv2c(Emit *v, float x, float y, float r, float g, float b, float a)
{
    emitf(v, x); emitf(v, y); emitf(v, r); emitf(v, g); emitf(v, b); emitf(v, a);
}

static void rv3c(Emit *v, float x, float y, float z,
                 float r, float g, float b, float a)
{
    emitf(v, x); emitf(v, y); emitf(v, z);
    emitf(v, r); emitf(v, g); emitf(v, b); emitf(v, a);
}

/* POS4 : la seule forme où le sommet porte un w, donc la seule qui peut
   demander une division perspective par ~0 (H4). */
static void rv4c(Emit *v, float x, float y, float z, float w,
                 float r, float g, float b, float a)
{
    emitf(v, x); emitf(v, y); emitf(v, z); emitf(v, w);
    emitf(v, r); emitf(v, g); emitf(v, b); emitf(v, a);
}

/* POS3 + normale : l'ordre du format est position, normale, couleur… */
static void rv3n(Emit *v, float x, float y, float z, float nx, float ny, float nz)
{
    emitf(v, x); emitf(v, y); emitf(v, z);
    emitf(v, nx); emitf(v, ny); emitf(v, nz);
}

static uint32_t snap[W * H];

static void take_snap(const uint8_t *shmem)
{
    memcpy(snap, shmem + RB_OFF, sizeof(snap));
}

static int same_snap(const uint8_t *shmem)
{
    return memcmp(snap, shmem + RB_OFF, sizeof(snap)) == 0;
}

#define VF_P2   ((uint32_t)QGPU_VF_POS(2))
#define VF_P3   ((uint32_t)QGPU_VF_POS(3))
#define VF_P4   ((uint32_t)QGPU_VF_POS(4))
#define VF_P2C  (VF_P2 | QGPU_VF_COLOR)
#define VF_P3C  (VF_P3 | QGPU_VF_COLOR)
#define VF_P4C  (VF_P4 | QGPU_VF_COLOR)

static void run_v7(QgpuCore *c, uint8_t *shmem)
{
    Emit e, v, t;
    float m[16], mv[16];
    uint32_t st, p, i;
    static const float zero4[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    static const float black4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    static const float white4[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const float red4[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    static const float green4[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    static const float dir_z[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
    static const float nospot[3] = { 0.0f, 0.0f, -1.0f };

    e.base = shmem; v.base = shmem; t.base = shmem;
    mat_ortho_px(m);
    mat_identity(mv);

    /* Contexte neuf : état GL et état géométrique aux valeurs initiales. */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(&e, 1);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 1);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 2);
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v7 : contexte neuf et projection en pixels (st %u)", st);

    /* (a) ÉQUIVALENCE DES CHEMINS ET SENS DE L'IMAGE. Le même triangle
       asymétrique par DRAW_TRIANGLES (pixels) et par DRAW_RAW (ortho
       équivalente) : mêmes pixels, donc même sens — (30,8) est dedans,
       (8,30) dehors, ce qui ne serait pas vrai si l'image était retournée. */
    v.off = v.start = VTX_OFF;
    vertex(&v, 4, 4, 1, 0, 0); vertex(&v, 60, 4, 1, 0, 0); vertex(&v, 4, 20, 1, 0, 0);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF0000FF, 1.0f);
    draw_cmd(&e, 3);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0xFF0000 &&
          px(shmem, 8, 10) == 0xFF0000 && px(shmem, 8, 30) == 0x0000FF &&
          px(shmem, 50, 16) == 0x0000FF,
          "(a) chemin pixels : %06x %06x %06x %06x (st %u)", px(shmem, 30, 8),
          px(shmem, 8, 10), px(shmem, 8, 30), px(shmem, 50, 16), st);
    v.off = v.start = VTX_OFF;
    rv2c(&v, 4, 4, 1, 0, 0, 1); rv2c(&v, 60, 4, 1, 0, 0, 1); rv2c(&v, 4, 20, 1, 0, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF0000FF, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 3, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0xFF0000 &&
          px(shmem, 8, 10) == 0xFF0000 && px(shmem, 8, 30) == 0x0000FF &&
          px(shmem, 50, 16) == 0x0000FF,
          "(a) chemin brut, MÊME image et MÊME sens : %06x %06x %06x %06x (st %u)",
          px(shmem, 30, 8), px(shmem, 8, 10), px(shmem, 8, 30), px(shmem, 50, 16), st);

    /* (b) MODÈLE-VUE ET PROJECTION. Translation. */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 0, 0, 0, 1, 0, 1); rv2c(&v, 16, 0, 0, 1, 0, 1);
    rv2c(&v, 16, 16, 0, 1, 0, 1); rv2c(&v, 0, 16, 0, 1, 0, 1);
    mat_identity(mv); mv[12] = 20.0f; mv[13] = 10.0f;
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_QUADS, 4, VF_P2C, 4, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 24, 14) == 0x00FF00 &&
          px(shmem, 34, 24) == 0x00FF00 && px(shmem, 10, 10) == 0 &&
          px(shmem, 40, 30) == 0,
          "(b) translation (20,10) : %06x %06x | %06x %06x (st %u)",
          px(shmem, 24, 14), px(shmem, 34, 24), px(shmem, 10, 10),
          px(shmem, 40, 30), st);

    /* Rotation de 90° autour de z, puis translation : le +x objet part vers le
       +y écran. Le triangle (0,0)(24,0)(0,8) atterrit en (32,20)(32,44)(24,20). */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 0, 0, 0, 1, 1, 1); rv2c(&v, 24, 0, 0, 1, 1, 1); rv2c(&v, 0, 8, 0, 1, 1, 1);
    mat_identity(mv);
    mv[0] = 0.0f; mv[1] = 1.0f; mv[4] = -1.0f; mv[5] = 0.0f;
    mv[12] = 32.0f; mv[13] = 20.0f;
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 3, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 24) == 0x00FFFF &&
          px(shmem, 40, 22) == 0,
          "(b) rotation 90° : dedans %06x, hors (sans rotation il serait dedans) %06x",
          px(shmem, 30, 24), px(shmem, 40, 22));

    /* Perspective : glFrustum(±1, ±1, 1, 100). Un quad de côté 2 à z = −2 se
       projette sur la moitié centrale de l'écran (16..48 en x comme en y). */
    v.off = v.start = VTX_OFF;
    rv3c(&v, -1, -1, -2, 1, 1, 0, 1); rv3c(&v, 1, -1, -2, 1, 1, 0, 1);
    rv3c(&v, 1, 1, -2, 1, 1, 0, 1); rv3c(&v, -1, 1, -2, 1, 1, 0, 1);
    mat_frustum(m, -1, 1, -1, 1, 1, 100);
    mat_identity(mv);
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_QUADS, 4, VF_P3C, 4, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0xFFFF00 &&
          px(shmem, 20, 44) == 0xFFFF00 && px(shmem, 10, 32) == 0 &&
          px(shmem, 32, 56) == 0,
          "(b) perspective : %06x %06x | %06x %06x",
          px(shmem, 32, 32), px(shmem, 20, 44), px(shmem, 10, 32), px(shmem, 32, 56));

    /* (h bis) DÉCOUPE PAR LE PLAN PROCHE : un triangle dont un sommet est
       DERRIÈRE l'œil (z = +2). Il doit devenir le trapèze (16,48) (48,48)
       (56,56) (8,56), calculé à la main. */
    v.off = v.start = VTX_OFF;
    rv3c(&v, -1, -1, -2, 0, 1, 1, 1); rv3c(&v, 1, -1, -2, 0, 1, 1, 1);
    rv3c(&v, 0, 0, 2, 0, 1, 1, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3C, 3, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 32, 52) == 0x00FFFF &&
          px(shmem, 32, 40) == 0 && px(shmem, 32, 60) == 0,
          "(h) découpe par le plan proche : %06x (dedans) %06x %06x (dehors) (st %u)",
          px(shmem, 32, 52), px(shmem, 32, 40), px(shmem, 32, 60), st);

    /* (c) LES DIX MODES, non indexés puis indexés en u16 et u32. */
    mat_ortho_px(m);
    mat_identity(mv);
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    {
        /* Tableaux d'indices identité, en u16 et en u32 : l'image doit être
           la même que sans indices, au pixel près. */
        static const struct {
            uint32_t mode, n;
            const char *name;
        } modes[10] = {
            { QGPU_PRIM_MODE_POINTS,         2, "POINTS" },
            { QGPU_PRIM_MODE_LINES,          4, "LINES" },
            { QGPU_PRIM_MODE_LINE_LOOP,      4, "LINE_LOOP" },
            { QGPU_PRIM_MODE_LINE_STRIP,     4, "LINE_STRIP" },
            { QGPU_PRIM_MODE_TRIANGLES,      3, "TRIANGLES" },
            { QGPU_PRIM_MODE_TRIANGLE_STRIP, 4, "TRIANGLE_STRIP" },
            { QGPU_PRIM_MODE_TRIANGLE_FAN,   4, "TRIANGLE_FAN" },
            { QGPU_PRIM_MODE_QUADS,          4, "QUADS" },
            { QGPU_PRIM_MODE_QUAD_STRIP,     6, "QUAD_STRIP" },
            { QGPU_PRIM_MODE_POLYGON,        5, "POLYGON" }
        };
        uint32_t k;
        int all_indexed_ok = 1;

        for (k = 0; k < 10; k++) {
            v.off = v.start = VTX_OFF;
            switch (modes[k].mode) {
            case QGPU_PRIM_MODE_POINTS:
                rv2c(&v, 10, 10, 1, 0, 1, 1); rv2c(&v, 50, 50, 1, 0, 1, 1);
                break;
            case QGPU_PRIM_MODE_LINES:
                rv2c(&v, 4, 10.5f, 0, 1, 0, 1); rv2c(&v, 60, 10.5f, 0, 1, 0, 1);
                rv2c(&v, 4, 40.5f, 0, 1, 0, 1); rv2c(&v, 60, 40.5f, 0, 1, 0, 1);
                break;
            case QGPU_PRIM_MODE_LINE_LOOP:
            case QGPU_PRIM_MODE_LINE_STRIP:
                rv2c(&v, 4.5f, 20.5f, 0, 1, 0, 1); rv2c(&v, 40.5f, 20.5f, 0, 1, 0, 1);
                rv2c(&v, 40.5f, 50.5f, 0, 1, 0, 1); rv2c(&v, 4.5f, 50.5f, 0, 1, 0, 1);
                break;
            case QGPU_PRIM_MODE_TRIANGLES:
                rv2c(&v, 4, 4, 1, 1, 0, 1); rv2c(&v, 60, 4, 1, 1, 0, 1);
                rv2c(&v, 4, 60, 1, 1, 0, 1);
                break;
            case QGPU_PRIM_MODE_TRIANGLE_STRIP:
                rv2c(&v, 8, 8, 0, 1, 1, 1); rv2c(&v, 8, 56, 0, 1, 1, 1);
                rv2c(&v, 56, 8, 0, 1, 1, 1); rv2c(&v, 56, 56, 0, 1, 1, 1);
                break;
            case QGPU_PRIM_MODE_TRIANGLE_FAN:
                rv2c(&v, 32, 32, 1, 1, 1, 1); rv2c(&v, 8, 8, 1, 1, 1, 1);
                rv2c(&v, 56, 8, 1, 1, 1, 1); rv2c(&v, 56, 56, 1, 1, 1, 1);
                break;
            case QGPU_PRIM_MODE_QUADS:
                rv2c(&v, 8, 8, 1, 0, 0, 1); rv2c(&v, 56, 8, 1, 0, 0, 1);
                rv2c(&v, 56, 56, 1, 0, 0, 1); rv2c(&v, 8, 56, 1, 0, 0, 1);
                break;
            case QGPU_PRIM_MODE_QUAD_STRIP:
                rv2c(&v, 8, 8, 0, 0, 1, 1); rv2c(&v, 8, 56, 0, 0, 1, 1);
                rv2c(&v, 32, 8, 0, 0, 1, 1); rv2c(&v, 32, 56, 0, 0, 1, 1);
                rv2c(&v, 56, 8, 0, 0, 1, 1); rv2c(&v, 56, 56, 0, 0, 1, 1);
                break;
            default:                                     /* POLYGON : pentagone */
                rv2c(&v, 32, 6, 1, 1, 1, 1); rv2c(&v, 58, 26, 1, 1, 1, 1);
                rv2c(&v, 48, 58, 1, 1, 1, 1); rv2c(&v, 16, 58, 1, 1, 1, 1);
                rv2c(&v, 6, 26, 1, 1, 1, 1);
                break;
            }
            e.off = e.start = CMD_OFF;
            clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
            state(&e, QGPU_SK_POINT_SIZE, qgpu_f2u(4.0f));
            draw_raw(&e, modes[k].mode, modes[k].n, VF_P2C, modes[k].n,
                     QGPU_IDX_NONE, 0);
            state(&e, QGPU_SK_POINT_SIZE, qgpu_f2u(1.0f));
            readback_cmd(&e, 2);
            st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
            take_snap(shmem);
            switch (modes[k].mode) {
            case QGPU_PRIM_MODE_POINTS:
                CHECK(st == QGPU_ST_OK && px(shmem, 9, 9) == 0xFF00FF &&
                      px(shmem, 51, 51) == 0xFF00FF && px(shmem, 30, 30) == 0,
                      "(c) POINTS : %06x %06x %06x", px(shmem, 9, 9),
                      px(shmem, 51, 51), px(shmem, 30, 30));
                break;
            case QGPU_PRIM_MODE_LINES:
                CHECK(st == QGPU_ST_OK && px(shmem, 30, 10) == 0x00FF00 &&
                      px(shmem, 30, 40) == 0x00FF00 && px(shmem, 30, 25) == 0,
                      "(c) LINES : %06x %06x %06x", px(shmem, 30, 10),
                      px(shmem, 30, 40), px(shmem, 30, 25));
                break;
            case QGPU_PRIM_MODE_LINE_STRIP:
                CHECK(st == QGPU_ST_OK && px(shmem, 20, 20) == 0x00FF00 &&
                      px(shmem, 40, 35) == 0x00FF00 && px(shmem, 4, 35) == 0,
                      "(c) LINE_STRIP (3 côtés) : %06x %06x | %06x",
                      px(shmem, 20, 20), px(shmem, 40, 35), px(shmem, 4, 35));
                break;
            case QGPU_PRIM_MODE_LINE_LOOP:
                CHECK(st == QGPU_ST_OK && px(shmem, 20, 20) == 0x00FF00 &&
                      px(shmem, 4, 35) == 0x00FF00,
                      "(c) LINE_LOOP (le 4e côté ferme) : %06x %06x",
                      px(shmem, 20, 20), px(shmem, 4, 35));
                break;
            case QGPU_PRIM_MODE_TRIANGLES:
                CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0xFFFF00 &&
                      px(shmem, 50, 50) == 0,
                      "(c) TRIANGLES : %06x %06x", px(shmem, 8, 8), px(shmem, 50, 50));
                break;
            case QGPU_PRIM_MODE_TRIANGLE_STRIP:
                CHECK(st == QGPU_ST_OK && px(shmem, 16, 16) == 0x00FFFF &&
                      px(shmem, 48, 48) == 0x00FFFF && px(shmem, 4, 4) == 0,
                      "(c) TRIANGLE_STRIP : %06x %06x %06x", px(shmem, 16, 16),
                      px(shmem, 48, 48), px(shmem, 4, 4));
                break;
            case QGPU_PRIM_MODE_TRIANGLE_FAN:
                CHECK(st == QGPU_ST_OK && px(shmem, 32, 20) == 0xFFFFFF &&
                      px(shmem, 48, 32) == 0xFFFFFF && px(shmem, 12, 50) == 0,
                      "(c) TRIANGLE_FAN : %06x %06x %06x", px(shmem, 32, 20),
                      px(shmem, 48, 32), px(shmem, 12, 50));
                break;
            case QGPU_PRIM_MODE_QUADS:
                CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0xFF0000 &&
                      px(shmem, 4, 4) == 0,
                      "(c) QUADS : %06x %06x", px(shmem, 32, 32), px(shmem, 4, 4));
                break;
            case QGPU_PRIM_MODE_QUAD_STRIP:
                CHECK(st == QGPU_ST_OK && px(shmem, 20, 32) == 0x0000FF &&
                      px(shmem, 44, 32) == 0x0000FF && px(shmem, 4, 4) == 0,
                      "(c) QUAD_STRIP : %06x %06x %06x", px(shmem, 20, 32),
                      px(shmem, 44, 32), px(shmem, 4, 4));
                break;
            default:
                CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0xFFFFFF &&
                      px(shmem, 2, 2) == 0,
                      "(c) POLYGON : %06x %06x", px(shmem, 32, 32), px(shmem, 2, 2));
                break;
            }
            /* mêmes sommets, indices identité : image identique */
            for (i = 0; i < modes[k].n; i++) {
                qgpu_st32(shmem + IDX_OFF + i * 2, 0);
                shmem[IDX_OFF + i * 2] = (uint8_t)(i >> 8);
                shmem[IDX_OFF + i * 2 + 1] = (uint8_t)i;
                qgpu_st32(shmem + IDX_OFF + 0x400 + i * 4, i);
            }
            e.off = e.start = CMD_OFF;
            clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
            state(&e, QGPU_SK_POINT_SIZE, qgpu_f2u(4.0f));
            draw_raw(&e, modes[k].mode, modes[k].n, VF_P2C, modes[k].n,
                     QGPU_IDX_U16, IDX_OFF);
            state(&e, QGPU_SK_POINT_SIZE, qgpu_f2u(1.0f));
            readback_cmd(&e, 2);
            st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
            if (st != QGPU_ST_OK || !same_snap(shmem)) {
                all_indexed_ok = 0;
                printf("       (u16 diffère pour %s, st %u)\n", modes[k].name, st);
            }
            e.off = e.start = CMD_OFF;
            clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
            state(&e, QGPU_SK_POINT_SIZE, qgpu_f2u(4.0f));
            draw_raw(&e, modes[k].mode, modes[k].n, VF_P2C, modes[k].n,
                     QGPU_IDX_U32, IDX_OFF + 0x400);
            state(&e, QGPU_SK_POINT_SIZE, qgpu_f2u(1.0f));
            readback_cmd(&e, 2);
            st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
            if (st != QGPU_ST_OK || !same_snap(shmem)) {
                all_indexed_ok = 0;
                printf("       (u32 diffère pour %s, st %u)\n", modes[k].name, st);
            }
        }
        CHECK(all_indexed_ok, "(c) indices u16 et u32 : image identique dans les 10 modes");
    }

    /* (d) OMBRAGE PLAT : sommet provoquant. TRIANGLES → le 3e sommet ;
       TRIANGLE_STRIP → le sommet i+2 de chaque triangle ; QUADS → le 4e. */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 4, 4, 1, 0, 0, 1); rv2c(&v, 60, 4, 0, 1, 0, 1); rv2c(&v, 4, 60, 0, 0, 1, 1);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_SHADE_MODEL, 0x1D00);              /* GL_FLAT */
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 3, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0x0000FF &&
          px(shmem, 40, 8) == 0x0000FF,
          "(d) TRIANGLES plat : couleur du 3e sommet : %06x %06x",
          px(shmem, 8, 8), px(shmem, 40, 8));
    v.off = v.start = VTX_OFF;
    rv2c(&v, 8, 8, 1, 0, 0, 1); rv2c(&v, 8, 56, 0, 1, 0, 1);
    rv2c(&v, 56, 8, 0, 0, 1, 1); rv2c(&v, 56, 56, 1, 1, 1, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLE_STRIP, 4, VF_P2C, 4, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 16, 16) == 0x0000FF &&
          px(shmem, 48, 48) == 0xFFFFFF,
          "(d) TRIANGLE_STRIP plat : sommets 2 puis 3 : %06x %06x",
          px(shmem, 16, 16), px(shmem, 48, 48));
    v.off = v.start = VTX_OFF;
    rv2c(&v, 8, 8, 1, 0, 0, 1); rv2c(&v, 56, 8, 0, 1, 0, 1);
    rv2c(&v, 56, 56, 0, 0, 1, 1); rv2c(&v, 8, 56, 1, 1, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_QUADS, 4, VF_P2C, 4, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_SHADE_MODEL, 0x1D01);              /* GL_SMOOTH */
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 20, 20) == 0xFFFF00 &&
          px(shmem, 44, 44) == 0xFFFF00,
          "(d) QUADS plat : couleur du 4e sommet : %06x %06x",
          px(shmem, 20, 20), px(shmem, 44, 44));

    /* (e) ÉLIMINATION DES FACES. En coordonnées fenêtre GL (y vers le haut),
       (4,4)(60,4)(4,60) en pixels tourne dans le sens horaire : c'est la face
       ARRIÈRE quand l'avant est GL_CCW. L'ordre inverse est l'avant. */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 4, 4, 1, 0, 0, 1); rv2c(&v, 60, 4, 1, 0, 0, 1); rv2c(&v, 4, 60, 1, 0, 0, 1);
    rv2c(&v, 4, 4, 0, 1, 0, 1); rv2c(&v, 4, 60, 0, 1, 0, 1); rv2c(&v, 60, 4, 0, 1, 0, 1);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_CULL_FACE, 1);
    state(&e, QGPU_SK_CULL_MODE, 0x0405);                /* GL_BACK */
    state(&e, QGPU_SK_FRONT_FACE, 0x0901);               /* GL_CCW */
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 6, VF_P2C, 6, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0x00FF00,
          "(e) CULL_BACK + CCW : seule la face avant (verte) reste : %06x",
          px(shmem, 8, 8));
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_FRONT_FACE, 0x0900);               /* GL_CW */
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 6, VF_P2C, 6, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0xFF0000,
          "(e) FRONT_FACE inversé : l'autre reste : %06x", px(shmem, 8, 8));
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_CULL_MODE, 0x0408);                /* GL_FRONT_AND_BACK */
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 6, VF_P2C, 6, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_CULL_FACE, 0);
    state(&e, QGPU_SK_CULL_MODE, 0x0405);
    state(&e, QGPU_SK_FRONT_FACE, 0x0901);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0,
          "(e) FRONT_AND_BACK : plus rien : %06x", px(shmem, 8, 8));

    /* (f) ÉCLAIRAGE. Ombrage PLAT partout : le triangle prend la couleur du
       sommet provoquant, qu'on calcule à la main — plus d'interpolation, donc
       plus d'écart de rastérisation entre les deux backends.
       Triangle (60,4) (4,60) (0,0) : le provoquant est l'origine de l'œil,
       et (20,20) est bien à l'intérieur. */
    v.off = v.start = VTX_OFF;
    rv3n(&v, 60, 4, 0, 0, 0.6f, 0.8f); rv3n(&v, 4, 60, 0, 0, 0.6f, 0.8f);
    rv3n(&v, 0, 0, 0, 0, 0.6f, 0.8f);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_SHADE_MODEL, 0x1D00);
    state(&e, QGPU_SK_LIGHTING, 1);
    set_light_model(&e, 0, 0, 0, 1);
    set_material(&e, 0x0408, black4, red4, black4, black4, 0.0f);
    set_light(&e, 0, 1, black4, white4, black4, dir_z, nospot, 0.0f, 180.0f,
              1.0f, 0.0f, 0.0f);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3 | QGPU_VF_NORMAL, 3,
             QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 20, 20);
    /* n·VP = (0, 0.6, 0.8)·(0, 0, 1) = 0.8 ; 0.8 × 255 = 204 */
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0xCC0000),
          "(f) diffuse directionnelle sur face inclinée (204 attendu) : %06x (st %u)",
          p, st);

    /* Normalisation : modèle-vue diag(1,1,0.5) → l'inverse-transposée est
       diag(1,1,2), la normale devient (0, 0.6, 1.6) (n·VP = 1.6, saturé à 255)
       et, normalisée, 1.6/√2.92 = 0.9363 → 239. */
    mat_identity(mv);
    mv[10] = 0.5f;
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3 | QGPU_VF_NORMAL, 3,
             QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 20, 20);
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0xFF0000),
          "(f) sans NORMALIZE, l'échelle sature (255) : %06x", p);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_NORMALIZE, 1);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3 | QGPU_VF_NORMAL, 3,
             QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_NORMALIZE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 20, 20);
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0xEF0000),
          "(f) avec NORMALIZE (239 attendu) : %06x", p);
    mat_identity(mv);
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);

    /* Lumière ponctuelle avec atténuation : à (0,0,10), sommet en (0,0,0),
       d = 10, atténuation 1/(1 + 0.01×100) = 0.5 → 128. Normale (0,0,1). */
    v.off = v.start = VTX_OFF;
    rv3n(&v, 60, 4, 0, 0, 0, 1); rv3n(&v, 4, 60, 0, 0, 0, 1); rv3n(&v, 0, 0, 0, 0, 0, 1);
    e.off = e.start = CMD_OFF;
    set_material(&e, 0x0408, black4, white4, black4, black4, 0.0f);
    {
        float pos[4] = { 0.0f, 0.0f, 10.0f, 1.0f };
        set_light(&e, 0, 1, black4, white4, black4, pos, nospot, 0.0f, 180.0f,
                  1.0f, 0.0f, 0.01f);
    }
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3 | QGPU_VF_NORMAL, 3,
             QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 20, 20);
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0x808080),
          "(f) ponctuelle, atténuation quadratique (128 attendu) : %06x (st %u)", p, st);

    /* Spot : axe à 45° du sommet, coupure 60° (donc dedans), exposant 2 :
       cos45^2 = 0.5 → 128. Puis coupure 30° : dehors → noir. */
    e.off = e.start = CMD_OFF;
    {
        float pos[4] = { 0.0f, 0.0f, 10.0f, 1.0f };
        float sd[3] = { 0.0f, -0.70710678f, -0.70710678f };
        set_light(&e, 0, 1, black4, white4, black4, pos, sd, 2.0f, 60.0f,
                  1.0f, 0.0f, 0.0f);
    }
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3 | QGPU_VF_NORMAL, 3,
             QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 20, 20);
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0x808080),
          "(f) spot à 45°, exposant 2 (128 attendu) : %06x", p);
    e.off = e.start = CMD_OFF;
    {
        float pos[4] = { 0.0f, 0.0f, 10.0f, 1.0f };
        float sd[3] = { 0.0f, -0.70710678f, -0.70710678f };
        set_light(&e, 0, 1, black4, white4, black4, pos, sd, 2.0f, 30.0f,
                  1.0f, 0.0f, 0.0f);
    }
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3 | QGPU_VF_NORMAL, 3,
             QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 20, 20) == 0,
          "(f) spot hors du cône de 30° : noir : %06x", px(shmem, 20, 20));

    /* Spéculaire, observateur à l'infini : n = (0, 0.6, 0.8), VP = h = (0,0,1),
       n·h = 0.8, brillance 4 → 0.8⁴ = 0.4096 → 104. */
    v.off = v.start = VTX_OFF;
    rv3n(&v, 60, 4, 0, 0, 0.6f, 0.8f); rv3n(&v, 4, 60, 0, 0, 0.6f, 0.8f);
    rv3n(&v, 0, 0, 0, 0, 0.6f, 0.8f);
    e.off = e.start = CMD_OFF;
    set_material(&e, 0x0408, black4, black4, white4, black4, 4.0f);
    set_light(&e, 0, 1, black4, black4, white4, dir_z, nospot, 0.0f, 180.0f,
              1.0f, 0.0f, 0.0f);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3 | QGPU_VF_NORMAL, 3,
             QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 20, 20);
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0x686868),
          "(f) spéculaire, brillance 4 (104 attendu) : %06x (st %u)", p, st);

    /* Observateur LOCAL : sommet provoquant en (40,40,0), donc VPe =
       −(40,40,0) normalisé ; h normalisé = (−0.5, −0.5, 0.7071), n = (0,0,1),
       n·h = 0.7071, brillance 2 → 0.5 → 128. */
    v.off = v.start = VTX_OFF;
    rv3n(&v, 0, 0, 0, 0, 0, 1); rv3n(&v, 60, 0, 0, 0, 0, 1); rv3n(&v, 40, 40, 0, 0, 0, 1);
    e.off = e.start = CMD_OFF;
    set_material(&e, 0x0408, black4, black4, white4, black4, 2.0f);
    state(&e, QGPU_SK_LOCAL_VIEWER, 1);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3 | QGPU_VF_NORMAL, 3,
             QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_LOCAL_VIEWER, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 30, 10);
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0x808080),
          "(f) observateur local, brillance 2 (128 attendu) : %06x", p);

    /* Color material : la couleur du sommet remplace la diffuse. */
    v.off = v.start = VTX_OFF;
    emitf(&v, 60); emitf(&v, 4); emitf(&v, 0); emitf(&v, 0); emitf(&v, 0); emitf(&v, 1);
    emitf(&v, 0.5f); emitf(&v, 0); emitf(&v, 0); emitf(&v, 1);
    emitf(&v, 4); emitf(&v, 60); emitf(&v, 0); emitf(&v, 0); emitf(&v, 0); emitf(&v, 1);
    emitf(&v, 0.5f); emitf(&v, 0); emitf(&v, 0); emitf(&v, 1);
    emitf(&v, 0); emitf(&v, 0); emitf(&v, 0); emitf(&v, 0); emitf(&v, 0); emitf(&v, 1);
    emitf(&v, 0.5f); emitf(&v, 0); emitf(&v, 0); emitf(&v, 1);
    e.off = e.start = CMD_OFF;
    set_material(&e, 0x0408, black4, white4, black4, black4, 0.0f);
    set_light(&e, 0, 1, black4, white4, black4, dir_z, nospot, 0.0f, 180.0f,
              1.0f, 0.0f, 0.0f);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3,
             VF_P3 | QGPU_VF_NORMAL | QGPU_VF_COLOR, 3, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && near_rgb(px(shmem, 20, 20), 0xFFFFFF),
          "(f) sans COLOR_MATERIAL, la diffuse du matériau gagne : %06x",
          px(shmem, 20, 20));
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_COLOR_MATERIAL, 1);
    state(&e, QGPU_SK_COLOR_MAT_MODE, 0x1602);           /* AMBIENT_AND_DIFFUSE */
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3,
             VF_P3 | QGPU_VF_NORMAL | QGPU_VF_COLOR, 3, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_COLOR_MATERIAL, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && near_rgb(px(shmem, 20, 20), 0x800000),
          "(f) COLOR_MATERIAL : la couleur du sommet (128,0,0) : %06x",
          px(shmem, 20, 20));

    /* Deux faces : matériau avant rouge, arrière vert ; la normale du triangle
       arrière est retournée pour qu'il s'éclaire aussi. */
    v.off = v.start = VTX_OFF;
    rv3n(&v, 4, 4, 0, 0, 0, 1); rv3n(&v, 4, 60, 0, 0, 0, 1); rv3n(&v, 60, 4, 0, 0, 0, 1);
    rv3n(&v, 4, 4, 0, 0, 0, -1); rv3n(&v, 60, 4, 0, 0, 0, -1); rv3n(&v, 4, 60, 0, 0, 0, -1);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_TWO_SIDE, 1);
    set_material(&e, 0x0404, black4, red4, black4, black4, 0.0f);
    set_material(&e, 0x0405, black4, green4, black4, black4, 0.0f);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3 | QGPU_VF_NORMAL, 6,
             QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && near_rgb(px(shmem, 8, 8), 0xFF0000),
          "(f) deux faces : la face avant est rouge : %06x", px(shmem, 8, 8));
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_RAW, QGPU_LEN_DRAW_RAW));
    emit(&e, QGPU_PRIM_MODE_TRIANGLES); emit(&e, 3); emit(&e, VTX_OFF);
    emit(&e, 0); emit(&e, VF_P3 | QGPU_VF_NORMAL); emit(&e, 0);
    emit(&e, QGPU_IDX_NONE); emit(&e, 3); emit(&e, 6);
    state(&e, QGPU_SK_TWO_SIDE, 0);
    state(&e, QGPU_SK_LIGHTING, 0);
    state(&e, QGPU_SK_SHADE_MODEL, 0x1D01);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && near_rgb(px(shmem, 8, 8), 0x00FF00),
          "(f) deux faces : la face arrière est verte : %06x", px(shmem, 8, 8));

    /* (g) TEXGEN et matrice de texture, sur une texture 2×2 à couleurs franches. */
    t.off = t.start = TEX_OFF;
    emit(&t, 0xFFFF0000); emit(&t, 0xFF00FF00);       /* rouge, vert */
    emit(&t, 0xFF0000FF); emit(&t, 0xFFFFFFFF);       /* bleu, blanc */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX)); emit(&e, 33);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(&e, 33); emit(&e, 0); emit(&e, 2); emit(&e, 2); emit(&e, 0x1908);
    emit(&e, TEX_OFF);
    tparam(&e, 33, QGPU_TP_MIN_FILTER, 0x2600);
    tparam(&e, 33, QGPU_TP_MAG_FILTER, 0x2600);
    tparam(&e, 33, QGPU_TP_WRAP_S, 0x812F);
    tparam(&e, 33, QGPU_TP_WRAP_T, 0x812F);
    state(&e, QGPU_SK_TEXTURE, 1);
    state(&e, QGPU_SK_TEX_BIND, 33);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x1E01);          /* REPLACE */
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "(g) texture 2×2 à zones franches (st %u)", st);

    v.off = v.start = VTX_OFF;
    rv2(&v, 0, 0); rv2(&v, 0, 64); rv2(&v, 64, 0); rv2(&v, 64, 64);
    e.off = e.start = CMD_OFF;
    {
        float ps[4] = { 1.0f / 64.0f, 0.0f, 0.0f, 0.0f };
        float pt[4] = { 0.0f, 1.0f / 64.0f, 0.0f, 0.0f };
        set_texgen(&e, 0, QGPU_TG_S, 1, QGPU_TG_OBJECT_LINEAR, ps, ps);
        set_texgen(&e, 0, QGPU_TG_T, 1, QGPU_TG_OBJECT_LINEAR, pt, pt);
    }
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLE_STRIP, 4, VF_P2, 4, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 10) == 0xFF0000 &&
          px(shmem, 50, 10) == 0x00FF00 && px(shmem, 10, 50) == 0x0000FF &&
          px(shmem, 50, 50) == 0xFFFFFF,
          "(g) OBJECT_LINEAR : %06x %06x %06x %06x (st %u)", px(shmem, 10, 10),
          px(shmem, 50, 10), px(shmem, 10, 50), px(shmem, 50, 50), st);

    /* Matrice de texture : translation de 0,5 en s → tout glisse d'une colonne. */
    mat_identity(m);
    m[12] = 0.5f;
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_TEXTURE0, m);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLE_STRIP, 4, VF_P2, 4, QGPU_IDX_NONE, 0);
    mat_identity(m);
    set_matrix(&e, QGPU_MTX_TEXTURE0, m);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 10) == 0x00FF00 &&
          px(shmem, 10, 50) == 0xFFFFFF,
          "(g) matrice de texture (+0,5 en s) : %06x %06x",
          px(shmem, 10, 10), px(shmem, 10, 50));

    /* EYE_LINEAR : modèle-vue qui translate de −16 en x, géométrie décalée de
       +16 pour rester à l'écran. En x = 24 à l'écran, l'œil vaut 24 → s =
       0,375, donc la colonne 0 : avec OBJECT_LINEAR on aurait eu la colonne 1. */
    v.off = v.start = VTX_OFF;
    rv2(&v, 16, 0); rv2(&v, 16, 64); rv2(&v, 80, 0); rv2(&v, 80, 64);
    mat_identity(mv);
    mv[12] = -16.0f;
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    {
        float ps[4] = { 1.0f / 64.0f, 0.0f, 0.0f, 0.0f };
        float pt[4] = { 0.0f, 1.0f / 64.0f, 0.0f, 0.0f };
        set_texgen(&e, 0, QGPU_TG_S, 1, QGPU_TG_EYE_LINEAR, ps, ps);
        set_texgen(&e, 0, QGPU_TG_T, 1, QGPU_TG_EYE_LINEAR, pt, pt);
    }
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLE_STRIP, 4, VF_P2, 4, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 24, 10) == 0xFF0000 &&
          px(shmem, 40, 10) == 0x00FF00,
          "(g) EYE_LINEAR : %06x (colonne 0) %06x (colonne 1) (st %u)",
          px(shmem, 24, 10), px(shmem, 40, 10), st);

    /* SPHERE_MAP : projection centrée sur l'œil (l'origine est au milieu de
       l'écran), quad de l'œil (−32,−32) à (32,32). Aux coins, u est à 45° :
       s et t valent 0,25 ou 0,75 — un quadrant par texel. */
    mat_ortho(m, -32.0f, 32.0f, 32.0f, -32.0f, 0.0f, -1.0f);
    mat_identity(mv);
    v.off = v.start = VTX_OFF;
    rv2(&v, -32, -32); rv2(&v, -32, 32); rv2(&v, 32, -32); rv2(&v, 32, 32);
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    {
        float z4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        set_texgen(&e, 0, QGPU_TG_S, 1, QGPU_TG_SPHERE_MAP, z4, z4);
        set_texgen(&e, 0, QGPU_TG_T, 1, QGPU_TG_SPHERE_MAP, z4, z4);
    }
    set_current(&e, QGPU_CUR_NORMAL, 0, 0, 1, 0);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLE_STRIP, 4, VF_P2, 4, QGPU_IDX_NONE, 0);
    {
        float z4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        set_texgen(&e, 0, QGPU_TG_S, 0, QGPU_TG_EYE_LINEAR, z4, z4);
        set_texgen(&e, 0, QGPU_TG_T, 0, QGPU_TG_EYE_LINEAR, z4, z4);
    }
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 10) == 0xFF0000 &&
          px(shmem, 50, 10) == 0x00FF00 && px(shmem, 10, 50) == 0x0000FF &&
          px(shmem, 50, 50) == 0xFFFFFF,
          "(g) SPHERE_MAP : %06x %06x %06x %06x (st %u)", px(shmem, 10, 10),
          px(shmem, 50, 10), px(shmem, 10, 50), px(shmem, 50, 50), st);

    /* (k) MULTITEXTURE BRUTE sur deux unités, la seconde en GL_COMBINE :
       blanc REPLACE, puis MODULATE(texture, précédent) avec (0.5, 1, 0). */
    t.off = t.start = TEX_OFF;
    emit(&t, 0xFFFFFFFF); emit(&t, 0xFF80FF00);
    mat_ortho_px(m);
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    tex1x1(&e, 30, 0x1908, TEX_OFF);
    tex1x1(&e, 31, 0x1907, TEX_OFF + 4);
    state(&e, QGPU_SK_TEXTURE, 1); state(&e, QGPU_SK_TEX_BIND, 30);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x1E01);
    state(&e, QGPU_SK_TEXTURE1, 1); state(&e, QGPU_SK_TEX1_BIND, 31);
    state(&e, QGPU_SK_TEX1_ENV_MODE, 0x8570);          /* GL_COMBINE */
    state(&e, QGPU_SK_COMBINE0 + 1, QGPU_COMBINE(QGPU_CB_MODULATE, QGPU_CB_MODULATE, 0, 0));
    state(&e, QGPU_SK_COMBINE_SRC0 + 1,
          QGPU_COMBINE_SRC_RGB(0, QGPU_CS_TEXTURE, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_RGB(1, QGPU_CS_PREVIOUS, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_A(0, QGPU_CS_TEXTURE, QGPU_CA_ALPHA) |
          QGPU_COMBINE_SRC_A(1, QGPU_CS_PREVIOUS, QGPU_CA_ALPHA));
    v.off = v.start = VTX_OFF;
    for (i = 0; i < 3; i++) {
        static const float xy[3][2] = { { 0, 0 }, { 64, 0 }, { 0, 64 } };
        emitf(&v, xy[i][0]); emitf(&v, xy[i][1]);
        emitf(&v, 0.5f); emitf(&v, 0.5f); emitf(&v, 0.0f); emitf(&v, 1.0f);
        emitf(&v, 0.5f); emitf(&v, 0.5f); emitf(&v, 0.0f); emitf(&v, 1.0f);
    }
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3,
             VF_P2 | QGPU_VF_TEX(0) | QGPU_VF_TEX(1), 3, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_TEXTURE, 0); state(&e, QGPU_SK_TEXTURE1, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 8, 8);
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0x80FF00),
          "(k) deux unités brutes, GL_COMBINE : %06x (st %u)", p, st);

    /* (j) VALEURS COURANTES : un format sans couleur prend SET_CURRENT. */
    v.off = v.start = VTX_OFF;
    rv2(&v, 4, 4); rv2(&v, 60, 4); rv2(&v, 4, 60);
    e.off = e.start = CMD_OFF;
    set_current(&e, QGPU_CUR_COLOR, 0.0f, 1.0f, 1.0f, 1.0f);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2, 3, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0x00FFFF,
          "(j) couleur courante pour un format sans couleur : %06x", px(shmem, 8, 8));
    /* Coordonnée de texture courante : (0.75, 0.25) désigne le texel vert. */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_TEXTURE, 1); state(&e, QGPU_SK_TEX_BIND, 33);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x1E01);
    set_current(&e, QGPU_CUR_TEXCOORD0, 0.75f, 0.25f, 0.0f, 1.0f);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2, 3, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_TEXTURE, 0);
    set_current(&e, QGPU_CUR_TEXCOORD0, 0.0f, 0.0f, 0.0f, 1.0f);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0x00FF00,
          "(j) coordonnée de texture courante : %06x", px(shmem, 8, 8));

    /* (h) PLAN DE DÉCOUPE UTILISATEUR : garde x <= 32 (en coordonnées œil). */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 0, 0, 1, 1, 0, 1); rv2c(&v, 0, 64, 1, 1, 0, 1);
    rv2c(&v, 64, 0, 1, 1, 0, 1); rv2c(&v, 64, 64, 1, 1, 0, 1);
    e.off = e.start = CMD_OFF;
    set_clip_plane(&e, 0, 1, -1.0f, 0.0f, 0.0f, 32.0f);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLE_STRIP, 4, VF_P2C, 4, QGPU_IDX_NONE, 0);
    set_clip_plane(&e, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 20, 20) == 0xFFFF00 &&
          px(shmem, 45, 20) == 0,
          "(h) plan de découpe x <= 32 : %06x (gardé) %06x (coupé) (st %u)",
          px(shmem, 20, 20), px(shmem, 45, 20), st);

    /* (i) BROUILLARD CALCULÉ PAR L'HÔTE. Projection ortho de z œil ∈ [0, −20],
       modèle-vue qui pose le quad à z œil = −10 : la distance vaut 10 partout,
       donc le facteur est constant et les deux backends peuvent s'accorder. */
    mat_ortho(m, 0.0f, (float)W, (float)H, 0.0f, 0.0f, 20.0f);
    mat_identity(mv);
    mv[14] = -10.0f;
    v.off = v.start = VTX_OFF;
    rv2c(&v, 0, 0, 1, 0, 0, 1); rv2c(&v, 0, 64, 1, 0, 0, 1);
    rv2c(&v, 64, 0, 1, 0, 0, 1); rv2c(&v, 64, 64, 1, 0, 0, 1);
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    state(&e, QGPU_SK_FOG, 1);
    state(&e, QGPU_SK_FOG_COLOR, 0xFF0000FF);
    state(&e, QGPU_SK_FOG_MODE, QGPU_FOG_LINEAR);
    state(&e, QGPU_SK_FOG_START, qgpu_f2u(0.0f));
    state(&e, QGPU_SK_FOG_END, qgpu_f2u(20.0f));
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLE_STRIP, 4, VF_P2C, 4, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 20, 20);
    /* f = (20 − 10)/20 = 0,5 → moitié rouge, moitié bleu */
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0x800080),
          "(i) brouillard LINEAR (128,0,128 attendu) : %06x (st %u)", p, st);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_FOG_MODE, QGPU_FOG_EXP);
    state(&e, QGPU_SK_FOG_DENSITY, qgpu_f2u(0.05f));
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLE_STRIP, 4, VF_P2C, 4, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_FOG, 0);
    state(&e, QGPU_SK_FOG_MODE, QGPU_FOG_VERTEX);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 20, 20);
    /* f = e^(−0,05 × 10) = 0,6065 → (155, 0, 100) */
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0x9B0064),
          "(i) brouillard EXP (155,0,100 attendu) : %06x", p);

    /* VIEWPORT. Il est en repère OpenGL : origine EN BAS à gauche. Un viewport
       (0, 0, 32, 32) occupe donc le quart BAS-gauche de la surface — c'est la
       preuve que l'hôte rapporte bien le viewport GL au bas de l'image. */
    mat_ortho_px(m);
    mat_identity(mv);
    v.off = v.start = VTX_OFF;
    rv2c(&v, 0, 0, 1, 0, 1, 1); rv2c(&v, 0, 64, 1, 0, 1, 1);
    rv2c(&v, 64, 0, 1, 0, 1, 1); rv2c(&v, 64, 64, 1, 0, 1, 1);
    e.off = e.start = CMD_OFF;
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_VIEWPORT, QGPU_LEN_VIEWPORT));
    emit(&e, 0); emit(&e, 0); emit(&e, 32); emit(&e, 32);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLE_STRIP, 4, VF_P2C, 4, QGPU_IDX_NONE, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_VIEWPORT, QGPU_LEN_VIEWPORT));
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 50) == 0xFF00FF &&
          px(shmem, 10, 10) == 0 && px(shmem, 50, 50) == 0 &&
          px(shmem, 50, 10) == 0,
          "viewport (0,0,32,32) = quart BAS-gauche : %06x | %06x %06x %06x (st %u)",
          px(shmem, 10, 50), px(shmem, 10, 10), px(shmem, 50, 50),
          px(shmem, 50, 10), st);

    /* DEPTH_RANGE : z objet 0 donne zd = −1, donc la borne « proche ». */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_RANGE, QGPU_LEN_DEPTH_RANGE));
    emitf(&e, 0.5f); emitf(&e, 1.0f);
    state(&e, QGPU_SK_DEPTH_WRITE, 1);
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF000000, 1.0f);
    /* le test doit être actif pour que la profondeur s'écrive, comme en GL */
    state(&e, QGPU_SK_DEPTH_TEST, 1);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLE_STRIP, 4, VF_P2C, 4, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_RANGE, QGPU_LEN_DEPTH_RANGE));
    emitf(&e, 0.0f); emitf(&e, 1.0f);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 2); emit(&e, RB_OFF); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0);
    emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    {
        float d = qgpu_u2f(qgpu_ld32(shmem + RB_OFF + 20 * STRIDE + 20 * 4));
        CHECK(st == QGPU_ST_OK && d > 0.49f && d < 0.51f,
              "DEPTH_RANGE (0,5 ; 1) : profondeur fenêtre %g (0,5 attendu)", d);
    }

    /* COULEUR SECONDAIRE : ajoutée à la primaire. 0,25 + 0,5 = 0,75 → 191. */
    v.off = v.start = VTX_OFF;
    for (i = 0; i < 3; i++) {
        static const float xy[3][2] = { { 4, 4 }, { 60, 4 }, { 4, 60 } };
        emitf(&v, xy[i][0]); emitf(&v, xy[i][1]);
        emitf(&v, 0.25f); emitf(&v, 0.0f); emitf(&v, 0.0f); emitf(&v, 1.0f);
        emitf(&v, 0.5f); emitf(&v, 0.0f); emitf(&v, 0.0f);
    }
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C | QGPU_VF_SEC_COLOR, 3,
             QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 8, 8);
    CHECK(st == QGPU_ST_OK && near_rgb(p, 0xBF0000),
          "couleur secondaire ajoutée (191 attendu) : %06x (st %u)", p, st);

    /* (l) STENCIL ET PROFONDEUR avec DRAW_RAW, sur la surface 3. */
    mat_ortho_px(m);
    mat_identity(mv);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 3);
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    v.off = v.start = VTX_OFF;
    rv3c(&v, 0, 0, 0, 1, 1, 1, 1); rv3c(&v, 32, 0, 0, 1, 1, 1, 1);
    rv3c(&v, 0, 32, 0, 1, 1, 1, 1);
    rv3c(&v, 0, 0, 0, 0, 1, 1, 1); rv3c(&v, 64, 0, 0, 0, 1, 1, 1);
    rv3c(&v, 0, 64, 0, 0, 1, 1, 1);
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    state(&e, QGPU_SK_DEPTH_WRITE, 1);
    state(&e, QGPU_SK_STENCIL_CLEAR, 0);
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH | QGPU_CLEAR_STENCIL,
              0xFF000000, 1.0f);
    state(&e, QGPU_SK_STENCIL_TEST, 1);
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0207);             /* ALWAYS */
    state(&e, QGPU_SK_STENCIL_REF, 1);
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_REPLACE);
    state(&e, QGPU_SK_COLOR_MASK, 0x0);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3C, 6, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_COLOR_MASK, 0xF);
    state(&e, QGPU_SK_STENCIL_FUNC, 0x0202);             /* EQUAL */
    sop(&e, QGPU_SOP_KEEP, QGPU_SOP_KEEP, QGPU_SOP_KEEP);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_RAW, QGPU_LEN_DRAW_RAW));
    emit(&e, QGPU_PRIM_MODE_TRIANGLES); emit(&e, 3); emit(&e, VTX_OFF);
    emit(&e, 0); emit(&e, VF_P3C); emit(&e, 0); emit(&e, QGPU_IDX_NONE);
    emit(&e, 3); emit(&e, 6);
    state(&e, QGPU_SK_STENCIL_TEST, 0);
    readback_cmd(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0x00FFFF &&
          px(shmem, 16, 32) == 0 && px(shmem, 50, 50) == 0,
          "(l) stencil avec DRAW_RAW : %06x %06x %06x (st %u)",
          px(shmem, 8, 8), px(shmem, 16, 32), px(shmem, 50, 50), st);
    v.off = v.start = VTX_OFF;
    rv3c(&v, 0, 0, 0.2f, 1, 0, 0, 1); rv3c(&v, 64, 0, 0.2f, 1, 0, 0, 1);
    rv3c(&v, 0, 64, 0.2f, 1, 0, 0, 1);
    rv3c(&v, 0, 0, 0.5f, 0, 1, 0, 1); rv3c(&v, 64, 0, 0.5f, 0, 1, 0, 1);
    rv3c(&v, 0, 64, 0.5f, 0, 1, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_DEPTH_TEST, 1);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 6, VF_P3C, 6, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    readback_cmd(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 16, 16) == 0xFF0000,
          "(l) profondeur avec DRAW_RAW : le plus proche gagne : %06x",
          px(shmem, 16, 16));

    /* (m) VALIDATIONS. Toutes dans le cœur, aucune dans un backend. */
    e.off = e.start = CMD_OFF;
    qgpu_st32(shmem + IDX_OFF, 0x00000005);              /* indices u16 0 puis 5 */
    qgpu_st32(shmem + IDX_OFF + 4, 0x00000000);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 3, QGPU_IDX_U16, IDX_OFF);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(m) indice hors bornes : st %u", st);
    e.off = e.start = CMD_OFF;
    draw_raw(&e, 10, 3, VF_P2C, 3, QGPU_IDX_NONE, 0);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(m) mode inconnu : st %u", st);
    e.off = e.start = CMD_OFF;
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, 0x400, 3, QGPU_IDX_NONE, 0);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(m) bit de format inconnu : st %u", st);
    e.off = e.start = CMD_OFF;
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, 3, 3, QGPU_IDX_NONE, 0);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(m) position à 5 composantes : st %u", st);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_RAW, QGPU_LEN_DRAW_RAW));
    emit(&e, QGPU_PRIM_MODE_TRIANGLES); emit(&e, 3); emit(&e, VTX_OFF);
    emit(&e, 3); emit(&e, VF_P2C); emit(&e, 0); emit(&e, QGPU_IDX_NONE);
    emit(&e, 0); emit(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(m) pas plus petit que le format : st %u", st);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_RAW, QGPU_LEN_DRAW_RAW));
    emit(&e, QGPU_PRIM_MODE_TRIANGLES); emit(&e, 3); emit(&e, SHMEM_SIZE - 8);
    emit(&e, 0); emit(&e, VF_P2C); emit(&e, 0); emit(&e, QGPU_IDX_NONE);
    emit(&e, 0); emit(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OOB, "(m) sommets hors de la fenêtre : st %u", st);
    e.off = e.start = CMD_OFF;
    mat_identity(m);
    m[5] = 0.0f / 0.0f;
    set_matrix(&e, QGPU_MTX_MODELVIEW, m);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(m) matrice avec un NaN : st %u", st);
    e.off = e.start = CMD_OFF;
    set_light(&e, QGPU_MAX_LIGHTS, 1, black4, white4, black4, dir_z, nospot,
              0.0f, 180.0f, 1.0f, 0.0f, 0.0f);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(m) lumière 8 : st %u", st);
    e.off = e.start = CMD_OFF;
    set_clip_plane(&e, QGPU_MAX_CLIP_PLANES, 1, 1.0f, 0.0f, 0.0f, 0.0f);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(m) plan de découpe 6 : st %u", st);
    e.off = e.start = CMD_OFF;
    set_texgen(&e, QGPU_MAX_UNITS, QGPU_TG_S, 1, QGPU_TG_EYE_LINEAR, zero4, zero4);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(m) unité de texgen 4 : st %u", st);
    e.off = e.start = CMD_OFF;
    set_light(&e, 0, 1, black4, white4, black4, dir_z, nospot, 0.0f, 120.0f,
              1.0f, 0.0f, 0.0f);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(m) angle de coupure 120° : st %u", st);

    /* (m bis) H4 — UN SOMMET MALSAIN NE COÛTE PLUS L'IMAGE. Le cœur refusait
       le dessin (BAD_ARG) et la boucle d'exécution abandonnait le reste de la
       soumission : l'effacement, la présentation et l'état qui suivaient
       étaient perdus. Trois cas, tous mesurés par le bug hunt. */
    mat_ortho_px(m);
    mat_identity(mv);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 2);
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    state(&e, QGPU_SK_STENCIL_TEST, 0);
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "(m bis) surface 2 et projection en pixels (st %u)", st);

    /* 1. Quatre sommets déclarés, trois cités : le quatrième est un NaN de
       bout en bout (queue jamais écrite d'un tampon surdimensionné). Il n'est
       plus SCANNÉ du tout, et le triangle sort entier. */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 4, 4, 1, 0, 0, 1); rv2c(&v, 60, 4, 1, 0, 0, 1); rv2c(&v, 4, 20, 1, 0, 0, 1);
    for (i = 0; i < 6; i++) {
        emit(&v, 0x7FC00000u);
    }
    for (i = 0; i < 3; i++) {
        qgpu_st16(shmem + IDX_OFF + i * 2, (uint16_t)i);
    }
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF0000FF, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 4, QGPU_IDX_U16, IDX_OFF);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0xFF0000 && px(shmem, 8, 30) == 0x0000FF,
          "(m bis) sommet NaN jamais cité par les indices : %06x %06x (st %u)",
          px(shmem, 30, 8), px(shmem, 8, 30), st);

    /* 1 bis. Indices ÉPARS sur un gros tampon : trois sommets cités aux rangs
       0, 150 et 299, tout le reste rempli de NaN. Ni la conversion ni la garde
       ne doivent regarder les 297 autres. */
    for (i = 0; i < 300 * 6; i++) {
        qgpu_st32(shmem + VTX_OFF + i * 4, 0x7FC00000u);
    }
    v.off = v.start = VTX_OFF; rv2c(&v, 4, 4, 1, 0, 0, 1);
    v.off = v.start = VTX_OFF + 150 * 6 * 4; rv2c(&v, 60, 4, 1, 0, 0, 1);
    v.off = v.start = VTX_OFF + 299 * 6 * 4; rv2c(&v, 4, 20, 1, 0, 0, 1);
    qgpu_st16(shmem + IDX_OFF, 0);
    qgpu_st16(shmem + IDX_OFF + 2, 150);
    qgpu_st16(shmem + IDX_OFF + 4, 299);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF0000FF, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 300, QGPU_IDX_U16, IDX_OFF);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0xFF0000 && px(shmem, 8, 30) == 0x0000FF,
          "(m bis) indices épars dans un tampon de 300 sommets NaN : %06x %06x (st %u)",
          px(shmem, 30, 8), px(shmem, 8, 30), st);

    /* 1 ter. `premier` non nul (glDrawArrays) : deux sommets NaN avant le
       triangle, jamais lus. */
    for (i = 0; i < 5 * 6; i++) {
        qgpu_st32(shmem + VTX_OFF + i * 4, 0x7FC00000u);
    }
    v.off = v.start = VTX_OFF + 2 * 6 * 4;
    rv2c(&v, 4, 4, 1, 0, 0, 1); rv2c(&v, 60, 4, 1, 0, 0, 1); rv2c(&v, 4, 20, 1, 0, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF0000FF, 1.0f);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_RAW, QGPU_LEN_DRAW_RAW));
    emit(&e, QGPU_PRIM_MODE_TRIANGLES); emit(&e, 3); emit(&e, VTX_OFF);
    emit(&e, 0); emit(&e, VF_P2C); emit(&e, 0); emit(&e, QGPU_IDX_NONE);
    emit(&e, 2); emit(&e, 5);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0xFF0000 && px(shmem, 8, 30) == 0x0000FF,
          "(m bis) `premier` = 2, les sommets 0 et 1 sont NaN et ignorés : %06x %06x (st %u)",
          px(shmem, 30, 8), px(shmem, 8, 30), st);

    /* 2. Le NaN est cette fois dans un sommet CITÉ : il est assaini, et
       surtout le CLEAR puis le READBACK qui SUIVENT le dessin s'exécutent. */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 4, 4, 1, 0, 0, 1); rv2c(&v, 60, 4, 1, 0, 0, 1); rv2c(&v, 4, 20, 1, 0, 0, 1);
    qgpu_st32(shmem + VTX_OFF + 4, 0x7FC00000u);         /* y du premier sommet */
    qgpu_st32(shmem + VTX_OFF + 24, 0x7F800000u);        /* x du deuxième : +inf */
    e.off = e.start = CMD_OFF;
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 3, QGPU_IDX_NONE, 0);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF204060, 1.0f);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 30) == 0x204060 && px(shmem, 2, 2) == 0x204060,
          "(m bis) NaN/infini dans un sommet cité : CLEAR et READBACK suivants exécutés :"
          " %06x %06x (st %u)", px(shmem, 30, 30), px(shmem, 2, 2), st);

    /* 3. Position à quatre composantes avec w = 0 : la division perspective en
       ferait un triangle infini (le « ciel » de Colin McRae). Le dessin est
       jeté, la soumission continue — le READBACK le prouve en remplaçant le
       fond 204060 précédent par du noir. */
    v.off = v.start = VTX_OFF;
    rv4c(&v, 4, 4, 0, 1, 1, 1, 0, 1); rv4c(&v, 60, 4, 0, 1, 1, 1, 0, 1);
    rv4c(&v, 4, 20, 0, 0.0f, 1, 1, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P4C, 3, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0x000000 && px(shmem, 2, 2) == 0x000000,
          "(m bis) position à w ≈ 0 : primitive jetée, flux poursuivi : %06x %06x (st %u)",
          px(shmem, 30, 8), px(shmem, 2, 2), st);

    /* 4. Le même dessin avec w = 1 sort bien : la garde ne mange pas les
       formats POS4 légitimes. */
    v.off = v.start = VTX_OFF;
    rv4c(&v, 4, 4, 0, 1, 1, 1, 0, 1); rv4c(&v, 60, 4, 0, 1, 1, 1, 0, 1);
    rv4c(&v, 4, 20, 0, 1, 1, 1, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P4C, 3, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0xFFFF00,
          "(m bis) POS4 avec w = 1 : dessiné : %06x (st %u)", px(shmem, 30, 8), st);

    /* état rendu au repos et contexte 0 repris pour la suite */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_DESTROY, QGPU_LEN_CTX)); emit(&e, 1);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 1);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v7 : retour au contexte 0 (st %u)", st);
}

/* ═════════════════════ v8 : la fin du pipeline fixe ═════════════════════════
 *
 * Même discipline que pour les versions précédentes : aucun point testé n'est
 * sur une arête de primitive, et les valeurs qui passent par un arrondi sont
 * comparées à ±2/255. Les tests de mode de polygone tracent leurs arêtes avec
 * une largeur de ligne de 3 et sondent le MILIEU d'une arête horizontale ou
 * verticale posée sur des centres de pixels : la ligne couvre alors trois
 * rangées franches, et le point sondé est au centre de celle du milieu.
 */
#define QRES_OFF 0x18000u

static void poly_stipple(Emit *e, const uint32_t *rows)
{
    int i;
    emit(e, QGPU_CMD_HDR(QGPU_OP_SET_POLYGON_STIPPLE, QGPU_LEN_SET_POLYGON_STIPPLE));
    for (i = 0; i < 32; i++) {
        emit(e, rows[i]);
    }
}

static void query_op(Emit *e, uint32_t op, uint32_t id)
{
    emit(e, QGPU_CMD_HDR(op, QGPU_LEN_QUERY));
    emit(e, id);
}

static void query_result(Emit *e, uint32_t id, uint32_t off)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_QUERY_RESULT, QGPU_LEN_QUERY_RESULT));
    emit(e, id); emit(e, off);
}

static uint32_t qword(const uint8_t *shmem, uint32_t off, int k)
{
    return qgpu_ld32(shmem + off + 4 * k);
}

/* Triangle rectangle posé sur des centres de pixels : arête du haut en
   y = 10,5 (de x = 10,5 à 50,5), arête de gauche en x = 10,5. Dans l'ordre de
   sommets donné, son aire à l'écran est HORAIRE : c'est une face ARRIÈRE. */
static void raw_tri_bl(Emit *v, float r, float g, float b)
{
    rv2c(v, 10.5f, 10.5f, r, g, b, 1);
    rv2c(v, 50.5f, 10.5f, r, g, b, 1);
    rv2c(v, 10.5f, 50.5f, r, g, b, 1);
}

static void run_v8(QgpuCore *c, uint8_t *shmem)
{
    Emit e, v;
    float m[16], mv[16];
    uint32_t st, p, i;
    uint32_t rows[32];

    e.base = shmem; v.base = shmem;
    mat_ortho_px(m);
    mat_identity(mv);

    /* Contexte neuf : tout l'état v8 est aux valeurs initiales d'OpenGL. */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(&e, 2);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 2);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 2);
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v8 : contexte neuf sur la surface 2 (st %u)", st);

    /* ── (a) MÉLANGE À COULEUR CONSTANTE ────────────────────────────────────
       Source blanche, destination noire, facteur de destination ZERO : le
       pixel obtenu EST le facteur source, donc la constante elle-même. La
       constante vaut 0x404080C0 — alpha 0x40, pour que CONSTANT_ALPHA (0x40)
       et ONE_MINUS_CONSTANT_ALPHA (0xBF) ne se confondent pas. */
    v.off = v.start = VTX_OFF;
    tri_tl(&v, 64, 0, 1, 1, 1);
    for (i = 0; i < 4; i++) {
        static const uint32_t fac[4] = {
            QGPU_BF_CONSTANT_COLOR, QGPU_BF_ONE_MINUS_CONSTANT_COLOR,
            QGPU_BF_CONSTANT_ALPHA, QGPU_BF_ONE_MINUS_CONSTANT_ALPHA
        };
        static const uint32_t want[4] = { 0x4080C0, 0xBF7F3F, 0x404040, 0xBFBFBF };
        static const char *nom[4] = { "CONSTANT_COLOR", "ONE_MINUS_CONSTANT_COLOR",
                                      "CONSTANT_ALPHA", "ONE_MINUS_CONSTANT_ALPHA" };
        e.off = e.start = CMD_OFF;
        clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
        state(&e, QGPU_SK_BLEND, 1);
        state(&e, QGPU_SK_BLEND_COLOR, 0x404080C0);
        state(&e, QGPU_SK_BLEND_SRC_RGB, fac[i]);
        state(&e, QGPU_SK_BLEND_SRC_A, fac[i]);
        state(&e, QGPU_SK_BLEND_DST_RGB, 0);          /* GL_ZERO */
        state(&e, QGPU_SK_BLEND_DST_A, 0);
        draw_cmd(&e, 3);
        readback_cmd(&e, 2);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        p = px(shmem, 8, 8);
        CHECK(st == QGPU_ST_OK && near_rgb(p, want[i]),
              "(a) facteur %s : %06x (attendu %06x, st %u)", nom[i], p, want[i], st);
    }

    /* ── (b) ÉQUATIONS MINIMUM ET MAXIMUM ───────────────────────────────────
       Les deux facteurs sont mis à ZERO : avec FUNC_ADD le pixel serait noir.
       S'il ne l'est pas, c'est bien que MIN et MAX LES IGNORENT. */
    v.off = v.start = VTX_OFF;
    tri_tl(&v, 64, 0, 48 / 255.0f, 224 / 255.0f, 80 / 255.0f);
    for (i = 0; i < 2; i++) {
        uint32_t eq = i ? QGPU_BEQ_MAX : QGPU_BEQ_MIN;
        uint32_t want = i ? 0xB0E050u : 0x301050u;
        e.off = e.start = CMD_OFF;
        clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFFB01050, 1.0f);
        state(&e, QGPU_SK_BLEND, 1);
        state(&e, QGPU_SK_BLEND_SRC_RGB, 0); state(&e, QGPU_SK_BLEND_DST_RGB, 0);
        state(&e, QGPU_SK_BLEND_SRC_A, 0);   state(&e, QGPU_SK_BLEND_DST_A, 0);
        state(&e, QGPU_SK_BLEND_EQ_RGB, eq);
        state(&e, QGPU_SK_BLEND_EQ_A, eq);
        draw_cmd(&e, 3);
        readback_cmd(&e, 2);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        p = px(shmem, 8, 8);
        CHECK(st == QGPU_ST_OK && near_rgb(p, want),
              "(b) équation %s, facteurs ignorés : %06x (attendu %06x, st %u)",
              i ? "GL_MAX" : "GL_MIN", p, want, st);
    }
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_BLEND, 0);
    state(&e, QGPU_SK_BLEND_EQ_RGB, QGPU_BEQ_ADD);
    state(&e, QGPU_SK_BLEND_EQ_A, QGPU_BEQ_ADD);
    state(&e, QGPU_SK_BLEND_SRC_RGB, 1); state(&e, QGPU_SK_BLEND_SRC_A, 1);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);

    /* ── (c) OPÉRATIONS LOGIQUES ────────────────────────────────────────────
       Source jaune 0xFFFF00 sur destination verte 0x00FF00 : chacune des six
       opérations sondées donne une couleur distincte. */
    v.off = v.start = VTX_OFF;
    tri_tl(&v, 64, 0, 1, 1, 0);
    {
        static const struct { uint32_t op, want; const char *nom; } lops[6] = {
            { QGPU_LO_XOR,           0xFF0000, "XOR" },
            { QGPU_LO_INVERT,        0xFF00FF, "INVERT" },
            { QGPU_LO_COPY_INVERTED, 0x0000FF, "COPY_INVERTED" },
            { QGPU_LO_AND,           0x00FF00, "AND" },
            { QGPU_LO_OR_REVERSE,    0xFFFFFF, "OR_REVERSE" },
            { QGPU_LO_CLEAR,         0x000000, "CLEAR" }
        };
        for (i = 0; i < 6; i++) {
            e.off = e.start = CMD_OFF;
            clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF00FF00, 1.0f);
            state(&e, QGPU_SK_LOGIC_OP, 1);
            state(&e, QGPU_SK_LOGIC_OP_MODE, lops[i].op);
            draw_cmd(&e, 3);
            readback_cmd(&e, 2);
            st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
            p = px(shmem, 8, 8);
            CHECK(st == QGPU_ST_OK && p == lops[i].want,
                  "(c) opération logique %s : %06x (attendu %06x, st %u)",
                  lops[i].nom, p, lops[i].want, st);
        }
    }
    /* Le mélange est IGNORÉ quand l'opération logique est active : avec
       ZERO/ZERO il donnerait du noir, et GL_COPY doit rendre la source. */
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF00FF00, 1.0f);
    state(&e, QGPU_SK_BLEND, 1);
    state(&e, QGPU_SK_BLEND_SRC_RGB, 0); state(&e, QGPU_SK_BLEND_DST_RGB, 0);
    state(&e, QGPU_SK_BLEND_SRC_A, 0);   state(&e, QGPU_SK_BLEND_DST_A, 0);
    state(&e, QGPU_SK_LOGIC_OP_MODE, QGPU_LO_COPY);
    draw_cmd(&e, 3);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 8, 8);
    CHECK(st == QGPU_ST_OK && p == 0xFFFF00,
          "(c) le mélange est ignoré sous opération logique : %06x (st %u)", p, st);
    /* … et le masque de couleur, lui, est respecté : INVERT à travers le seul
       canal rouge donne ~0x00 en rouge, et le vert et le bleu du fond. */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_BLEND, 0);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF00FF00, 1.0f);
    state(&e, QGPU_SK_LOGIC_OP_MODE, QGPU_LO_INVERT);
    state(&e, QGPU_SK_COLOR_MASK, 0x1);
    draw_cmd(&e, 3);
    state(&e, QGPU_SK_COLOR_MASK, 0xF);
    state(&e, QGPU_SK_LOGIC_OP, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = px(shmem, 8, 8);
    CHECK(st == QGPU_ST_OK && p == 0xFFFF00,
          "(c) opération logique sous masque de couleur : %06x (st %u)", p, st);

    /* ── (d) MODES DE POLYGONE ──────────────────────────────────────────────
       Triangle rectangle de DRAW_RAW, arêtes sur des centres de pixels. */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_LINE_WIDTH, qgpu_f2u(3.0f));
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);

    v.off = v.start = VTX_OFF;
    raw_tri_bl(&v, 0, 1, 0);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_POLYGON_MODE_FRONT, QGPU_POLY_LINE);
    state(&e, QGPU_SK_POLYGON_MODE_BACK, QGPU_POLY_LINE);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 3, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 10) == 0x00FF00 &&
          px(shmem, 10, 30) == 0x00FF00 && px(shmem, 25, 25) == 0 &&
          px(shmem, 30, 15) == 0,
          "(d) mode GL_LINE : arêtes %06x %06x, intérieur %06x %06x (st %u)",
          px(shmem, 30, 10), px(shmem, 10, 30), px(shmem, 25, 25),
          px(shmem, 30, 15), st);

    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_POINT_SIZE, qgpu_f2u(3.0f));
    state(&e, QGPU_SK_POLYGON_MODE_FRONT, QGPU_POLY_POINT);
    state(&e, QGPU_SK_POLYGON_MODE_BACK, QGPU_POLY_POINT);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 3, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_POINT_SIZE, qgpu_f2u(1.0f));
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 10) == 0x00FF00 &&
          px(shmem, 30, 10) == 0 && px(shmem, 25, 25) == 0,
          "(d) mode GL_POINT : sommet %06x, arête %06x, intérieur %06x (st %u)",
          px(shmem, 10, 10), px(shmem, 30, 10), px(shmem, 25, 25), st);

    /* Faces avant et arrière avec des modes DIFFÉRENTS. Le triangle ci-dessus
       est une face ARRIÈRE (son aire à l'écran est horaire) : avec
       avant = FILL et arrière = LINE il doit rester creux, et l'inverse le
       remplit. C'est aussi ce qui vérifie que les deux backends s'accordent
       sur le sens des faces. */
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_POLYGON_MODE_FRONT, QGPU_POLY_FILL);
    state(&e, QGPU_SK_POLYGON_MODE_BACK, QGPU_POLY_LINE);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 3, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 25, 25) == 0 && px(shmem, 30, 10) == 0x00FF00,
          "(d) face arrière en GL_LINE : intérieur %06x, arête %06x (st %u)",
          px(shmem, 25, 25), px(shmem, 30, 10), st);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_POLYGON_MODE_FRONT, QGPU_POLY_LINE);
    state(&e, QGPU_SK_POLYGON_MODE_BACK, QGPU_POLY_FILL);
    draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2C, 3, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 25, 25) == 0x00FF00,
          "(d) la même face en GL_FILL quand les modes sont échangés : %06x (st %u)",
          px(shmem, 25, 25), st);

    /* CONTOUR SEUL d'un GL_QUADS : la diagonale de la décomposition en deux
       triangles ne doit PAS être tracée. Elle passerait exactement par le
       centre du pixel (30,30). */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 10.5f, 10.5f, 1, 0, 1, 1); rv2c(&v, 50.5f, 10.5f, 1, 0, 1, 1);
    rv2c(&v, 50.5f, 50.5f, 1, 0, 1, 1); rv2c(&v, 10.5f, 50.5f, 1, 0, 1, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_POLYGON_MODE_FRONT, QGPU_POLY_LINE);
    state(&e, QGPU_SK_POLYGON_MODE_BACK, QGPU_POLY_LINE);
    draw_raw(&e, QGPU_PRIM_MODE_QUADS, 4, VF_P2C, 4, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 10) == 0xFF00FF &&
          px(shmem, 10, 30) == 0xFF00FF && px(shmem, 30, 50) == 0xFF00FF &&
          px(shmem, 30, 30) == 0 && px(shmem, 25, 35) == 0,
          "(d) GL_QUADS en GL_LINE : contour %06x %06x %06x, diagonale %06x, "
          "intérieur %06x (st %u)", px(shmem, 30, 10), px(shmem, 10, 30),
          px(shmem, 30, 50), px(shmem, 30, 30), px(shmem, 25, 35), st);

    /* MODE DE POLYGONE SUR LES ANCIENS OPCODES. Mêmes sommets, en pixels de
       surface, par DRAW_TRIANGLES : le mode s'applique aussi, et le sens des
       faces y est le même (ce triangle reste une face arrière). */
    v.off = v.start = VTX_OFF;
    vertexz(&v, 10.5f, 10.5f, 0, 0, 1, 1, 1);
    vertexz(&v, 50.5f, 10.5f, 0, 0, 1, 1, 1);
    vertexz(&v, 10.5f, 50.5f, 0, 0, 1, 1, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_POLYGON_MODE_FRONT, QGPU_POLY_FILL);
    state(&e, QGPU_SK_POLYGON_MODE_BACK, QGPU_POLY_LINE);
    draw_cmd(&e, 3);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 10) == 0x00FFFF && px(shmem, 25, 25) == 0,
          "(d) DRAW_TRIANGLES en GL_LINE (face arrière) : arête %06x, "
          "intérieur %06x (st %u)", px(shmem, 30, 10), px(shmem, 25, 25), st);

    /* DÉCALAGE DE POLYGONE POUR LES LIGNES. Le triangle plein rouge est posé à
       z = 0,5 ; le même triangle en fil vert, à la même profondeur, échoue au
       test LESS — sauf s'il est décalé vers l'avant. Sonde : (30,11), à
       l'intérieur du triangle plein ET sous l'arête large de 3. */
    v.off = v.start = VTX_OFF;
    rv3c(&v, 10.5f, 10.5f, 0.5f, 1, 0, 0, 1);
    rv3c(&v, 50.5f, 10.5f, 0.5f, 1, 0, 0, 1);
    rv3c(&v, 10.5f, 50.5f, 0.5f, 1, 0, 0, 1);
    rv3c(&v, 10.5f, 10.5f, 0.5f, 0, 1, 0, 1);
    rv3c(&v, 50.5f, 10.5f, 0.5f, 0, 1, 0, 1);
    rv3c(&v, 10.5f, 50.5f, 0.5f, 0, 1, 0, 1);
    for (i = 0; i < 2; i++) {
        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_DEPTH_WRITE, 1);
        clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF000000, 1.0f);
        state(&e, QGPU_SK_DEPTH_TEST, 1);
        state(&e, QGPU_SK_DEPTH_FUNC, 0x0201);            /* LESS */
        state(&e, QGPU_SK_POLYGON_MODE_FRONT, QGPU_POLY_FILL);
        state(&e, QGPU_SK_POLYGON_MODE_BACK, QGPU_POLY_FILL);
        draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P3C, 6, QGPU_IDX_NONE, 0);
        state(&e, QGPU_SK_POLYGON_MODE_FRONT, QGPU_POLY_LINE);
        state(&e, QGPU_SK_POLYGON_MODE_BACK, QGPU_POLY_LINE);
        state(&e, QGPU_SK_POLY_FACTOR, qgpu_f2u(0.0f));
        state(&e, QGPU_SK_POLY_UNITS, qgpu_f2u(-64.0f));
        state(&e, QGPU_SK_POLY_OFFSET_LINE, i);
        emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_RAW, QGPU_LEN_DRAW_RAW));
        emit(&e, QGPU_PRIM_MODE_TRIANGLES); emit(&e, 3); emit(&e, VTX_OFF);
        emit(&e, 0); emit(&e, VF_P3C); emit(&e, 0); emit(&e, QGPU_IDX_NONE);
        emit(&e, 3); emit(&e, 6);
        state(&e, QGPU_SK_POLY_OFFSET_LINE, 0);
        state(&e, QGPU_SK_DEPTH_TEST, 0);
        readback_cmd(&e, 2);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        p = px(shmem, 30, 11);
        CHECK(st == QGPU_ST_OK && p == (i ? 0x00FF00u : 0xFF0000u),
              "(d) décalage de ligne %s : %06x (attendu %06x, st %u)",
              i ? "actif" : "inactif", p, i ? 0x00FF00u : 0xFF0000u, st);
    }
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_POLYGON_MODE_FRONT, QGPU_POLY_FILL);
    state(&e, QGPU_SK_POLYGON_MODE_BACK, QGPU_POLY_FILL);
    state(&e, QGPU_SK_POLY_UNITS, qgpu_f2u(0.0f));
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);

    /* ── (e) POINTILLÉ DE LIGNE ─────────────────────────────────────────────
       Motif 0x00FF, facteur 2 : le compteur s'incrémente d'un pixel d'axe
       majeur, et le bit employé est (compteur / 2) & 15 — donc 16 pixels
       allumés, 16 éteints, et ainsi de suite depuis l'extrémité de départ. */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 4.5f, 20.5f, 1, 1, 0, 1); rv2c(&v, 44.5f, 20.5f, 1, 1, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_LINE_STIPPLE, 1);
    state(&e, QGPU_SK_LINE_STIPPLE_FACTOR, 2);
    state(&e, QGPU_SK_LINE_STIPPLE_PATTERN, 0x00FF);
    draw_raw(&e, QGPU_PRIM_MODE_LINES, 2, VF_P2C, 2, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 20) == 0xFFFF00 &&
          px(shmem, 20, 20) == 0 && px(shmem, 24, 20) == 0 &&
          px(shmem, 38, 20) == 0xFFFF00,
          "(e) pointillé de ligne 0x00FF ×2 : x10 %06x x20 %06x x24 %06x "
          "x38 %06x (st %u)", px(shmem, 10, 20), px(shmem, 20, 20),
          px(shmem, 24, 20), px(shmem, 38, 20), st);

    /* Continuité sur une bande : deux segments bout à bout. Si le compteur
       repartait de zéro au second, x = 24 serait allumé. */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 4.5f, 30.5f, 1, 1, 0, 1); rv2c(&v, 20.5f, 30.5f, 1, 1, 0, 1);
    rv2c(&v, 44.5f, 30.5f, 1, 1, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    draw_raw(&e, QGPU_PRIM_MODE_LINE_STRIP, 3, VF_P2C, 3, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_LINE_STIPPLE, 0);
    state(&e, QGPU_SK_LINE_WIDTH, qgpu_f2u(1.0f));
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 30) == 0xFFFF00 &&
          px(shmem, 24, 30) == 0 && px(shmem, 38, 30) == 0xFFFF00,
          "(e) le compteur court le long d'une bande : x10 %06x x24 %06x "
          "x38 %06x (st %u)", px(shmem, 10, 30), px(shmem, 24, 30),
          px(shmem, 38, 30), st);

    /* ── (f) POINTILLÉ DE POLYGONE ──────────────────────────────────────────
       Damier : lignes paires du MOTIF 0xAAAAAAAA (bit de poids fort = x 0,
       donc les x pairs), lignes impaires 0x55555555. La surface fait 64
       lignes, un multiple de 32, donc la ligne de surface ys emploie le mot
       (64 − ys) mod 32, de même parité que ys : le pixel est allumé quand
       x + ys est pair. */
    for (i = 0; i < 32; i++) {
        rows[i] = (i & 1) ? 0x55555555u : 0xAAAAAAAAu;
    }
    v.off = v.start = VTX_OFF;
    rv2c(&v, 8, 8, 0, 1, 0, 1); rv2c(&v, 49, 8, 0, 1, 0, 1);
    rv2c(&v, 49, 48, 0, 1, 0, 1); rv2c(&v, 8, 48, 0, 1, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    poly_stipple(&e, rows);
    state(&e, QGPU_SK_POLYGON_STIPPLE, 1);
    draw_raw(&e, QGPU_PRIM_MODE_QUADS, 4, VF_P2C, 4, QGPU_IDX_NONE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 10) == 0x00FF00 &&
          px(shmem, 11, 10) == 0 && px(shmem, 10, 11) == 0 &&
          px(shmem, 11, 11) == 0x00FF00,
          "(f) damier de pointillé : (10,10) %06x (11,10) %06x (10,11) %06x "
          "(11,11) %06x (st %u)", px(shmem, 10, 10), px(shmem, 11, 10),
          px(shmem, 10, 11), px(shmem, 11, 11), st);

    /* LE SENS VERTICAL. Une seule ligne du motif est pleine, la quatrième. Le
       mot 0 étant la ligne yw = 0 — le BAS de l'image —, la ligne de surface
       allumée est celle où (64 − ys) mod 32 = 4, soit ys = 28 (et 60). Si
       l'hôte indexait par ys, ce serait la ligne 4. */
    for (i = 0; i < 32; i++) {
        rows[i] = (i == 4) ? 0xFFFFFFFFu : 0u;
    }
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    poly_stipple(&e, rows);
    draw_raw(&e, QGPU_PRIM_MODE_QUADS, 4, VF_P2C, 4, QGPU_IDX_NONE, 0);
    state(&e, QGPU_SK_POLYGON_STIPPLE, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 20, 28) == 0x00FF00 &&
          px(shmem, 20, 27) == 0 && px(shmem, 20, 29) == 0 &&
          px(shmem, 20, 4) == 0,
          "(f) sens vertical du motif : ys28 %06x ys27 %06x ys29 %06x ys4 %06x "
          "(st %u)", px(shmem, 20, 28), px(shmem, 20, 27), px(shmem, 20, 29),
          px(shmem, 20, 4), st);

    /* ── (g) REQUÊTES D'OCCLUSION ───────────────────────────────────────────
       Rectangle aligné (8,8)–(25,24) : 17 × 16 = 272 centres de pixels, et sa
       diagonale de décomposition ne passe par AUCUN d'eux (17j − 16i − 7,5
       n'est jamais nul) — le compte est donc exact des deux côtés. */
    v.off = v.start = VTX_OFF;
    rv3c(&v, 8, 8, 0.5f, 1, 1, 1, 1);   rv3c(&v, 25, 8, 0.5f, 1, 1, 1, 1);
    rv3c(&v, 25, 24, 0.5f, 1, 1, 1, 1); rv3c(&v, 8, 24, 0.5f, 1, 1, 1, 1);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    query_op(&e, QGPU_OP_QUERY_BEGIN, 0);
    draw_raw(&e, QGPU_PRIM_MODE_QUADS, 4, VF_P3C, 4, QGPU_IDX_NONE, 0);
    query_op(&e, QGPU_OP_QUERY_END, 0);
    query_result(&e, 0, QRES_OFF);
    query_result(&e, 0, QRES_OFF + 8);       /* relu une seconde fois */
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && qword(shmem, QRES_OFF, 0) == 1 &&
          qword(shmem, QRES_OFF, 1) == 272 &&
          qword(shmem, QRES_OFF + 8, 0) == 1 &&
          qword(shmem, QRES_OFF + 8, 1) == 272,
          "(g) quad visible : disponible %u, %u échantillons (272 attendus) ; "
          "relecture %u/%u (st %u)", qword(shmem, QRES_OFF, 0),
          qword(shmem, QRES_OFF, 1), qword(shmem, QRES_OFF + 8, 0),
          qword(shmem, QRES_OFF + 8, 1), st);

    /* Masque de couleur fermé : le compte ne change pas — c'est tout l'usage
       d'une requête d'occlusion. */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_COLOR_MASK, 0x0);
    query_op(&e, QGPU_OP_QUERY_BEGIN, 1);
    draw_raw(&e, QGPU_PRIM_MODE_QUADS, 4, VF_P3C, 4, QGPU_IDX_NONE, 0);
    query_op(&e, QGPU_OP_QUERY_END, 1);
    state(&e, QGPU_SK_COLOR_MASK, 0xF);
    query_result(&e, 1, QRES_OFF);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && qword(shmem, QRES_OFF, 1) == 272,
          "(g) masque de couleur fermé : %u échantillons (272 attendus) (st %u)",
          qword(shmem, QRES_OFF, 1), st);

    /* Entièrement caché par la profondeur : fond effacé à 0,2, quad à 0,5,
       test LESS → aucun fragment ne passe. */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_WRITE, 1);
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF000000, 0.2f);
    state(&e, QGPU_SK_DEPTH_TEST, 1);
    state(&e, QGPU_SK_DEPTH_FUNC, 0x0201);               /* LESS */
    query_op(&e, QGPU_OP_QUERY_BEGIN, 2);
    draw_raw(&e, QGPU_PRIM_MODE_QUADS, 4, VF_P3C, 4, QGPU_IDX_NONE, 0);
    query_op(&e, QGPU_OP_QUERY_END, 2);
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    query_result(&e, 2, QRES_OFF);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && qword(shmem, QRES_OFF, 0) == 1 &&
          qword(shmem, QRES_OFF, 1) == 0,
          "(g) quad caché par la profondeur : %u échantillons (0 attendu) (st %u)",
          qword(shmem, QRES_OFF, 1), st);

    /* À moitié masqué par les ciseaux : 17 colonnes × 8 lignes = 136. */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_SCISSOR, 1);
    state(&e, QGPU_SK_SCISSOR_X, 8); state(&e, QGPU_SK_SCISSOR_Y, 8);
    state(&e, QGPU_SK_SCISSOR_W, 17); state(&e, QGPU_SK_SCISSOR_H, 8);
    query_op(&e, QGPU_OP_QUERY_BEGIN, 3);
    draw_raw(&e, QGPU_PRIM_MODE_QUADS, 4, VF_P3C, 4, QGPU_IDX_NONE, 0);
    query_op(&e, QGPU_OP_QUERY_END, 3);
    state(&e, QGPU_SK_SCISSOR, 0);
    query_result(&e, 3, QRES_OFF);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && qword(shmem, QRES_OFF, 1) == 136,
          "(g) moitié coupée par les ciseaux : %u échantillons (136 attendus) (st %u)",
          qword(shmem, QRES_OFF, 1), st);

    /* Validations. Une requête est laissée OUVERTE par l'imbrication refusée
       (le flux s'arrête sur la commande fautive) : on la referme ensuite. */
    e.off = e.start = CMD_OFF;
    query_op(&e, QGPU_OP_QUERY_BEGIN, 4);
    query_op(&e, QGPU_OP_QUERY_BEGIN, 5);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(g) imbrication de requêtes refusée : st %u", st);
    e.off = e.start = CMD_OFF;
    query_op(&e, QGPU_OP_QUERY_END, 4);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "(g) la requête restée ouverte se referme : st %u", st);
    e.off = e.start = CMD_OFF;
    query_op(&e, QGPU_OP_QUERY_BEGIN, QGPU_MAX_QUERIES);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(g) identifiant hors bornes : st %u", st);
    e.off = e.start = CMD_OFF;
    query_op(&e, QGPU_OP_QUERY_END, 6);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(g) QUERY_END sans BEGIN : st %u", st);
    e.off = e.start = CMD_OFF;
    query_result(&e, 7, QRES_OFF);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(g) résultat d'une requête jamais lancée : st %u", st);
    e.off = e.start = CMD_OFF;
    query_result(&e, 0, SHMEM_SIZE - 4);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OOB, "(g) offset de résultat hors de BAR0 : st %u", st);

    /* Validations des clés v8. */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_LOGIC_OP_MODE, 0x1510);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(g) opération logique inconnue : st %u", st);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_POLYGON_MODE_FRONT, 0x1B03);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(g) mode de polygone inconnu : st %u", st);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_LINE_STIPPLE_FACTOR, 0);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(g) facteur de pointillé 0 : st %u", st);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_LINE_STIPPLE_PATTERN, 0x10000);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "(g) motif de pointillé sur plus de 16 bits : st %u", st);

    /* état rendu au repos et contexte 0 repris pour la suite */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_DESTROY, QGPU_LEN_CTX)); emit(&e, 2);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 1);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v8 : retour au contexte 0 (st %u)", st);
}

/* Tests v2 : état GL par fragment, sur une surface avec profondeur (id 2). */
static void run_v2(QgpuCore *c, uint8_t *shmem)
{
    Emit e, v;
    uint32_t st;

    /* profondeur : rouge loin (0.8) puis vert près (0.2) sur la même zone,
       puis bleu plus loin (0.5) que le vert : seul le vert doit rester. */
    v.base = shmem; v.off = v.start = VTX_OFF;
    vertexz(&v, 0, 0, 0.8f, 1, 0, 0, 1); vertexz(&v, 64, 0, 0.8f, 1, 0, 0, 1); vertexz(&v, 0, 64, 0.8f, 1, 0, 0, 1);
    vertexz(&v, 0, 0, 0.2f, 0, 1, 0, 1); vertexz(&v, 64, 0, 0.2f, 0, 1, 0, 1); vertexz(&v, 0, 64, 0.2f, 0, 1, 0, 1);
    vertexz(&v, 0, 0, 0.5f, 0, 0, 1, 1); vertexz(&v, 64, 0, 0.5f, 0, 0, 1, 1); vertexz(&v, 0, 64, 0.5f, 0, 0, 1, 1);
    e.base = shmem; e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 2); emit(&e, W); emit(&e, H); emit(&e, QGPU_FMT_XRGB8888 | QGPU_FMT_FLAG_DEPTH);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 2);
    state(&e, QGPU_SK_DEPTH_TEST, 1);
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF000000, 1.0f);
    draw_cmd(&e, 9);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0x00FF00, "profondeur LESS : le plus proche gagne : %06x (st %u)", px(shmem, 8, 8), st);
    CHECK(px(shmem, 60, 60) == 0x000000, "hors triangles : fond : %06x", px(shmem, 60, 60));

    /* relecture de la profondeur au même point : 0.2 */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 2); emit(&e, RB_OFF); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    {
        float d = qgpu_u2f(qgpu_ld32(shmem + RB_OFF + 8 * STRIDE + 8 * 4));
        float bg = qgpu_u2f(qgpu_ld32(shmem + RB_OFF + 60 * STRIDE + 60 * 4));
        CHECK(st == QGPU_ST_OK && d > 0.19f && d < 0.21f && bg > 0.999f,
              "relecture profondeur : %g (triangle) %g (fond)", d, bg);
    }

    /* écriture de profondeur : 0.1 partout, puis un rouge à 0.15 ne passe plus */
    {
        uint32_t i;
        for (i = 0; i < W * H; i++) {
            qgpu_st32(shmem + RB_OFF + i * 4, qgpu_f2u(0.1f));
        }
    }
    v.off = v.start = VTX_OFF;
    vertexz(&v, 0, 0, 0.15f, 1, 0, 0, 1); vertexz(&v, 64, 0, 0.15f, 1, 0, 0, 1); vertexz(&v, 0, 64, 0.15f, 1, 0, 0, 1);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_UPLOAD, QGPU_LEN_SURF_XFER));
    emit(&e, 2); emit(&e, RB_OFF); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    draw_cmd(&e, 3);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0x00FF00, "profondeur envoyée respectée : %06x", px(shmem, 8, 8));

    /* GEQUAL + masque de profondeur fermé : le rouge passe mais n'écrit pas */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_FUNC, 0x0206);
    state(&e, QGPU_SK_DEPTH_WRITE, 0);
    draw_cmd(&e, 3);
    state(&e, QGPU_SK_DEPTH_FUNC, 0x0201);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 8, 8) == 0xFF0000, "GEQUAL : %06x", px(shmem, 8, 8));

    /* mélange SRC_ALPHA / ONE_MINUS_SRC_ALPHA : bleu à 50 % sur rouge */
    v.off = v.start = VTX_OFF;
    vertexz(&v, 0, 0, 0.0f, 0, 0, 1, 0.5f); vertexz(&v, 64, 0, 0.0f, 0, 0, 1, 0.5f); vertexz(&v, 0, 64, 0.0f, 0, 0, 1, 0.5f);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    state(&e, QGPU_SK_BLEND, 1);
    state(&e, QGPU_SK_BLEND_SRC_RGB, 0x0302); state(&e, QGPU_SK_BLEND_DST_RGB, 0x0303);
    draw_cmd(&e, 3);
    state(&e, QGPU_SK_BLEND, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    {
        uint32_t p = px(shmem, 8, 8), r = p >> 16, b = p & 255;
        CHECK(st == QGPU_ST_OK && r > 120 && r < 135 && b > 120 && b < 135 && ((p >> 8) & 255) == 0,
              "mélange 50 %% : %06x", p);
    }

    /* test alpha GREATER 0.5 : un fragment à 0.25 est rejeté */
    v.off = v.start = VTX_OFF;
    vertexz(&v, 0, 0, 0.0f, 1, 1, 1, 0.25f); vertexz(&v, 64, 0, 0.0f, 1, 1, 1, 0.25f); vertexz(&v, 0, 64, 0.0f, 1, 1, 1, 0.25f);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    state(&e, QGPU_SK_ALPHA_TEST, 1);
    state(&e, QGPU_SK_ALPHA_FUNC, 0x0204);
    state(&e, QGPU_SK_ALPHA_REF, qgpu_f2u(0.5f));
    draw_cmd(&e, 3);
    state(&e, QGPU_SK_ALPHA_REF, qgpu_f2u(0.125f));
    state(&e, QGPU_SK_COLOR_MASK, 0x2);                 /* vert seulement */
    state(&e, QGPU_SK_SCISSOR, 1);
    state(&e, QGPU_SK_SCISSOR_X, 10); state(&e, QGPU_SK_SCISSOR_Y, 0);
    state(&e, QGPU_SK_SCISSOR_W, 10); state(&e, QGPU_SK_SCISSOR_H, 64);
    draw_cmd(&e, 3);
    state(&e, QGPU_SK_SCISSOR, 0);
    state(&e, QGPU_SK_COLOR_MASK, 0xF);
    state(&e, QGPU_SK_ALPHA_TEST, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 4, 4) == 0x000000, "test alpha rejette 0.25 > 0.5 : %06x", px(shmem, 4, 4));
    CHECK(px(shmem, 12, 4) == 0x00FF00, "ciseaux + masque vert : %06x", px(shmem, 12, 4));
    CHECK(px(shmem, 30, 4) == 0x000000, "hors ciseaux : %06x", px(shmem, 30, 4));
    CHECK((pxa(shmem, 12, 4) >> 24) == 0xFF, "alpha préservé par le masque : %08x", pxa(shmem, 12, 4));

    /* effacement limité par les ciseaux */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_SCISSOR, 1);
    state(&e, QGPU_SK_SCISSOR_X, 0); state(&e, QGPU_SK_SCISSOR_Y, 0);
    state(&e, QGPU_SK_SCISSOR_W, 4); state(&e, QGPU_SK_SCISSOR_H, 4);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFFFFFFFF, 1.0f);
    state(&e, QGPU_SK_SCISSOR, 0);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 1, 1) == 0xFFFFFF && px(shmem, 5, 5) != 0xFFFFFF,
          "effacement sous ciseaux : %06x / %06x", px(shmem, 1, 1), px(shmem, 5, 5));

    /* validation des valeurs d'état */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_FUNC, 0x1234);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "fonction de profondeur invalide refusée : st %u", st);
    e.off = e.start = CMD_OFF;
    state(&e, 99, 0);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "clé d'état inconnue refusée : st %u", st);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "relecture de profondeur sans tampon refusée : st %u", st);
    /* H4 : un sommet malsain est ASSAINI, plus refusé — et surtout la fin de
       la soumission n'est plus perdue. Avant, le BAD_ARG faisait `break` :
       le CLEAR et le READBACK ci-dessous ne s'exécutaient jamais (en VM,
       c'était le SURF_PRESENT, donc l'écran figé). */
    v.off = v.start = VTX_OFF;
    vertexz(&v, 0, 0, 0.0f, 1, 1, 1, 1);
    vertexz(&v, 64, 0, 0.0f, 1, 1, 1, 1);
    vertexz(&v, 0, 64, 0.0f, 1, 1, 1, 1);
    qgpu_st32(shmem + VTX_OFF + 4, 0x7FC00000u);         /* y du premier sommet : NaN */
    qgpu_st32(shmem + VTX_OFF + 8 * 4, 0x7F800000u);     /* x du deuxième : +inf */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    draw_cmd(&e, 3);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF204060, 1.0f);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 30) == 0x204060 && px(shmem, 2, 62) == 0x204060,
          "H4 : sommet NaN assaini, CLEAR et READBACK suivants exécutés : %06x %06x (st %u pc %u)",
          px(shmem, 30, 30), px(shmem, 2, 62), st, c->status_pc);

    /* Un BAD_ARG de dessin qui n'a rien à voir avec les valeurs (nombre impair
       de sommets de ligne) : il est RETENU — rendu à la fin, avec son pc —
       mais il ne fait plus perdre le CLEAR qui suit. */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_LINES, QGPU_LEN_DRAW)); emit(&e, 3); emit(&e, VTX_OFF);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF00FF00, 1.0f);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG && c->status_pc == 0 && px(shmem, 30, 30) == 0x00FF00,
          "H4 : BAD_ARG de dessin non fatal, la suite s'exécute : st %u pc %u %06x",
          st, c->status_pc, px(shmem, 30, 30));

    /* … mais HORS dessin, une faute arrête toujours le flux : le CLEAR bleu
       ci-dessous ne doit PAS passer (le tampon de relecture garde le vert). */
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_FUNC, 0x1234);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF0000FF, 1.0f);
    readback_cmd(&e, 2);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG && px(shmem, 30, 30) == 0x00FF00,
          "H4 : hors dessin, la faute arrête toujours la soumission : st %u %06x",
          st, px(shmem, 30, 30));
}

/* Le scénario de référence : rouge sur fond bleu, puis relecture. */
static uint32_t build_scene(uint8_t *shmem)
{
    Emit e = { shmem, CMD_OFF, CMD_OFF };
    Emit v = { shmem, VTX_OFF, VTX_OFF };

    /* triangle rouge (4,4) (60,4) (4,60) ; puis un vert, en sens inverse */
    vertex(&v, 4, 4, 1, 0, 0); vertex(&v, 60, 4, 1, 0, 0); vertex(&v, 4, 60, 1, 0, 0);
    vertex(&v, 62, 62, 0, 1, 0); vertex(&v, 62, 40, 0, 1, 0); vertex(&v, 40, 62, 0, 1, 0);

    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 1); emit(&e, W); emit(&e, H); emit(&e, QGPU_FMT_XRGB8888);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 1);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR));
    emit(&e, QGPU_CLEAR_COLOR); emit(&e, 0x0000FF); emitf(&e, 1.0f);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW));
    emit(&e, 6); emit(&e, VTX_OFF);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0);
    emit(&e, W); emit(&e, H);
    return e.off - e.start;
}

/* ══════ v6/v8 : aller-retour profondeur/stencil, exigé BIT À BIT ══════
 *
 * Le bogue signalé depuis l'invité : sur une surface COMBINÉE
 * profondeur+stencil, QGPU_OP_STENCIL_UPLOAD abîme la profondeur déjà posée,
 * au point qu'une géométrie PLUS PROCHE se fait éliminer par un test LESS.
 * Les deux tests croisés de la v6 ne le voient pas : ils comparent une
 * profondeur à 0,01 près, et sur une valeur du milieu (0,1) — or le dégât est
 * soit d'un bit de poids faible d'un tampon 24 bits (6e-8), soit total mais
 * réservé à la valeur 1,0 (le fond, celui qu'un CLEAR pose partout).
 *
 * On exige donc ici : l'égalité BIT À BIT des relectures de part et d'autre
 * d'un téléversement, l'IDEMPOTENCE de l'aller-retour, et l'ÉGALITÉ des
 * valeurs relues entre une surface combinée et une surface à profondeur seule
 * (même tampon 24 bits : les deux doivent rendre exactement la même chose).
 * Puis on rejoue le flux réel de l'invité à quatre profondeurs voisines.
 * Surface 128×160 : ni carrée, ni de la taille des autres tests, pour
 * attraper au passage une erreur de pas ou de sens. */
#define ZW      128u
#define ZH      160u
#define ZSTR    (ZW * 4)
#define ZD0_OFF 0x40000u
#define ZD1_OFF 0x58000u
#define ZS0_OFF 0x70000u
#define ZS1_OFF 0x88000u
#define ZS2_OFF 0xA0000u
#define ZC_OFF  0xB8000u

static void zxfer(Emit *e, uint32_t op, uint32_t surf, uint32_t off)
{
    emit(e, QGPU_CMD_HDR(op, QGPU_LEN_SURF_XFER));
    emit(e, surf); emit(e, off); emit(e, ZSTR); emit(e, 0); emit(e, 0);
    emit(e, ZW); emit(e, ZH);
}

static uint32_t zld(const uint8_t *shmem, uint32_t off, uint32_t x, uint32_t y)
{
    return qgpu_ld32(shmem + off + y * ZSTR + x * 4);
}

/* Premier pixel où deux blocs relus diffèrent, ou -1 s'ils sont identiques. */
static long zdiff(const uint8_t *shmem, uint32_t a, uint32_t b)
{
    uint32_t x, y;
    for (y = 0; y < ZH; y++) {
        for (x = 0; x < ZW; x++) {
            if (zld(shmem, a, x, y) != zld(shmem, b, x, y)) {
                return (long)(y * ZW + x);
            }
        }
    }
    return -1;
}

static void zshow(const uint8_t *shmem, uint32_t a, uint32_t b, long k)
{
    if (k >= 0) {
        float u = qgpu_u2f(qgpu_ld32(shmem + a + (size_t)k * 4));
        float v = qgpu_u2f(qgpu_ld32(shmem + b + (size_t)k * 4));
        printf("       pixel %ld (%lu,%lu) : %.9g → %.9g (écart %g)\n",
               k, (unsigned long)(k % ZW), (unsigned long)(k / ZW),
               (double)u, (double)v, (double)(v - u));
    }
}

/* Rectangle plein (deux triangles) à la profondeur z. */
static void zrect(Emit *v, float x0, float y0, float x1, float y1,
                  float z, float r, float g, float b)
{
    vertexz(v, x0, y0, z, r, g, b, 1);
    vertexz(v, x1, y0, z, r, g, b, 1);
    vertexz(v, x1, y1, z, r, g, b, 1);
    vertexz(v, x0, y0, z, r, g, b, 1);
    vertexz(v, x1, y1, z, r, g, b, 1);
    vertexz(v, x0, y1, z, r, g, b, 1);
}

/* Points d'observation : trois dans la moitié gauche (là où la géométrie de
   base pose sa profondeur), deux dans la moitié droite (restée au fond, à
   1,0 — la valeur que le bogue de débordement détruit). Aucun sur une arête
   ni sur la diagonale qui sépare les deux triangles. */
static const uint32_t zpx[5] = { 2, 10, 40, 125, 110 };
static const uint32_t zpy[5] = { 3, 150, 120, 157, 10 };

static uint32_t zcolor_all(const uint8_t *shmem, uint32_t want)
{
    uint32_t k;
    for (k = 0; k < 5; k++) {
        uint32_t p = zld(shmem, ZC_OFF, zpx[k], zpy[k]) & 0xFFFFFFu;
        if (p != want) {
            return p;
        }
    }
    return want;
}

static void run_zs(QgpuCore *c, uint8_t *shmem)
{
    /* Profondeurs « réalistes » et voisines : au milieu, tout près de 1 et
       tout près de 0. L'écart le plus fin (1e-4) reste très au-dessus du pas
       d'un tampon 24 bits : seul un aller-retour cassé peut le rater. */
    static const float zbase[4] = { 0.5f,    0.5f,  0.99f,   0.01f  };
    static const float znear[4] = { 0.4999f, 0.49f, 0.9899f, 0.0099f };
    static const float zedge[8] = { 0.0f, 1.0f, 0.5f, 0.4999f,
                                    0.99f, 0.999999f, 1e-6f, 0.25f };
    Emit e, v;
    uint32_t st, i, x, y;
    long bad;

    e.base = shmem; v.base = shmem;
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 11); emit(&e, ZW); emit(&e, ZH);
    emit(&e, QGPU_FMT_XRGB8888 | QGPU_FMT_FLAG_DEPTH | QGPU_FMT_FLAG_STENCIL);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 12); emit(&e, ZW); emit(&e, ZH);
    emit(&e, QGPU_FMT_XRGB8888 | QGPU_FMT_FLAG_DEPTH);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK,
          "z/s : surfaces 128×160, combinée et profondeur seule (st %u)", st);

    /* (1) Motif couvrant tout [0,1] plus les valeurs de bord (0, 1, 1−ε…),
       téléversé sur les deux surfaces. La relecture doit être la MÊME des
       deux côtés (même tampon 24 bits), et un second aller-retour ne doit
       plus rien changer (idempotence). */
    for (y = 0; y < ZH; y++) {
        for (x = 0; x < ZW; x++) {
            uint32_t k = y * ZW + x;
            float d = k < 8 ? zedge[k] : (float)k / (float)(ZW * ZH - 1);
            qgpu_st32(shmem + ZD0_OFF + y * ZSTR + x * 4, qgpu_f2u(d));
        }
    }
    for (i = 0; i < 2; i++) {
        uint32_t surf = i ? 12 : 11;
        uint32_t r1 = i ? ZS2_OFF : ZD1_OFF;
        e.off = e.start = CMD_OFF;
        emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, surf);
        zxfer(&e, QGPU_OP_DEPTH_UPLOAD, surf, ZD0_OFF);
        zxfer(&e, QGPU_OP_DEPTH_READBACK, surf, r1);        /* R1 */
        zxfer(&e, QGPU_OP_DEPTH_UPLOAD, surf, r1);
        zxfer(&e, QGPU_OP_DEPTH_READBACK, surf, ZS1_OFF);   /* R2 */
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        bad = zdiff(shmem, r1, ZS1_OFF);
        CHECK(st == QGPU_ST_OK && bad < 0,
              "aller-retour de profondeur idempotent (%s) : %s (st %u)",
              i ? "profondeur seule" : "combinée",
              bad < 0 ? "identique" : "DIVERGE", st);
        zshow(shmem, r1, ZS1_OFF, bad);
    }
    bad = zdiff(shmem, ZD1_OFF, ZS2_OFF);
    CHECK(bad < 0, "profondeur relue identique, surface combinée ou non : %s",
          bad < 0 ? "identique" : "DIVERGE");
    zshow(shmem, ZD1_OFF, ZS2_OFF, bad);

    /* (2) STENCIL_UPLOAD ne doit RIEN changer à la profondeur, bit à bit, sur
       tout le dégradé laissé par (1) sur la surface combinée. */
    for (y = 0; y < ZH; y++) {
        for (x = 0; x < ZW; x++) {
            qgpu_st32(shmem + ZS0_OFF + y * ZSTR + x * 4, (y * 7u + x * 3u) & 0xFFu);
        }
    }
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 11);
    zxfer(&e, QGPU_OP_DEPTH_READBACK, 11, ZD1_OFF);
    zxfer(&e, QGPU_OP_STENCIL_UPLOAD, 11, ZS0_OFF);
    zxfer(&e, QGPU_OP_DEPTH_READBACK, 11, ZS1_OFF);
    zxfer(&e, QGPU_OP_STENCIL_READBACK, 11, ZS2_OFF);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    bad = zdiff(shmem, ZD1_OFF, ZS1_OFF);
    CHECK(st == QGPU_ST_OK && bad < 0,
          "STENCIL_UPLOAD laisse la profondeur intacte bit à bit : %s (st %u)",
          bad < 0 ? "identique" : "DIVERGE", st);
    zshow(shmem, ZD1_OFF, ZS1_OFF, bad);
    bad = zdiff(shmem, ZS0_OFF, ZS2_OFF);
    CHECK(st == QGPU_ST_OK && bad < 0,
          "aller-retour de stencil exact sur 128×160 : %s (st %u)",
          bad < 0 ? "identique" : "DIVERGE", st);

    /* (3) réciproque : DEPTH_UPLOAD ne doit rien changer au stencil. */
    e.off = e.start = CMD_OFF;
    zxfer(&e, QGPU_OP_DEPTH_UPLOAD, 11, ZD1_OFF);
    zxfer(&e, QGPU_OP_STENCIL_READBACK, 11, ZS2_OFF);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    bad = zdiff(shmem, ZS0_OFF, ZS2_OFF);
    CHECK(st == QGPU_ST_OK && bad < 0,
          "DEPTH_UPLOAD laisse le stencil intact bit à bit : %s (st %u)",
          bad < 0 ? "identique" : "DIVERGE", st);

    /* (4) Le flux exact rapporté par l'invité, à quatre profondeurs. La
       profondeur est posée par un DRAW (pas par un CLEAR) sur la MOITIÉ
       GAUCHE seulement : la moitié droite reste au fond (1,0), la valeur que
       tout CLEAR pose et que le débordement de la conversion 24 bits
       transforme en 0,0 — c'est-à-dire en un mur devant toute la scène.
       Les deux tirs de contrôle n'écrivent pas la profondeur : LESS et LEQUAL
       jugent donc la même valeur relue puis réécrite. */
    for (i = 0; i < 4; i++) {
        v.off = v.start = VTX_OFF;
        zrect(&v, 0, 0, ZW / 2, ZH, zbase[i], 0, 0, 1);
        e.off = e.start = CMD_OFF;
        emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 11);
        state(&e, QGPU_SK_STENCIL_TEST, 0);
        state(&e, QGPU_SK_STENCIL_WRITE_MASK, 0xFF);
        state(&e, QGPU_SK_STENCIL_CLEAR, 0x3C);
        state(&e, QGPU_SK_DEPTH_WRITE, 1);
        clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH | QGPU_CLEAR_STENCIL,
                  0xFF000000, 1.0f);
        /* le test doit être ACTIF pour que la profondeur soit écrite : c'est
           la règle d'OpenGL, et le backend logiciel la suit aussi. */
        state(&e, QGPU_SK_DEPTH_TEST, 1);
        state(&e, QGPU_SK_DEPTH_FUNC, 0x0207);          /* ALWAYS */
        draw_cmd(&e, 6);
        zxfer(&e, QGPU_OP_DEPTH_READBACK, 11, ZD0_OFF);
        zxfer(&e, QGPU_OP_DEPTH_UPLOAD, 11, ZD0_OFF);
        zxfer(&e, QGPU_OP_STENCIL_READBACK, 11, ZS0_OFF);
        zxfer(&e, QGPU_OP_STENCIL_UPLOAD, 11, ZS0_OFF);
        zxfer(&e, QGPU_OP_DEPTH_READBACK, 11, ZD1_OFF);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        bad = zdiff(shmem, ZD0_OFF, ZD1_OFF);
        CHECK(st == QGPU_ST_OK && bad < 0,
              "flux invité z=%g : STENCIL_UPLOAD préserve la profondeur : %s (st %u)",
              (double)zbase[i], bad < 0 ? "identique" : "DIVERGE", st);
        zshow(shmem, ZD0_OFF, ZD1_OFF, bad);

        v.off = v.start = VTX_OFF;
        zrect(&v, 0, 0, ZW, ZH, znear[i], 1, 0, 0);
        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_DEPTH_WRITE, 0);
        state(&e, QGPU_SK_DEPTH_FUNC, 0x0201);          /* LESS */
        draw_cmd(&e, 6);
        zxfer(&e, QGPU_OP_SURF_READBACK, 11, ZC_OFF);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        CHECK(st == QGPU_ST_OK && zcolor_all(shmem, 0xFF0000u) == 0xFF0000u,
              "flux invité z=%g : LESS accepte %g après l'aller-retour : %06x (st %u)",
              (double)zbase[i], (double)znear[i], zcolor_all(shmem, 0xFF0000u), st);

        v.off = v.start = VTX_OFF;
        zrect(&v, 0, 0, ZW, ZH, zbase[i], 0, 1, 0);
        e.off = e.start = CMD_OFF;
        clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
        state(&e, QGPU_SK_DEPTH_FUNC, 0x0203);          /* LEQUAL */
        draw_cmd(&e, 6);
        zxfer(&e, QGPU_OP_SURF_READBACK, 11, ZC_OFF);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        CHECK(st == QGPU_ST_OK && zcolor_all(shmem, 0x00FF00u) == 0x00FF00u,
              "flux invité z=%g : LEQUAL repasse à profondeur égale : %06x (st %u)",
              (double)zbase[i], zcolor_all(shmem, 0x00FF00u), st);
    }

    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_DEPTH_WRITE, 1);
    state(&e, QGPU_SK_DEPTH_TEST, 0);
    state(&e, QGPU_SK_DEPTH_FUNC, 0x0201);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 1);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_DESTROY, QGPU_LEN_SURF)); emit(&e, 11);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_DESTROY, QGPU_LEN_SURF)); emit(&e, 12);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "z/s : surfaces rendues (st %u)", st);
}

/* ── v9 : la file de soumissions ─────────────────────────────────────────────
 *
 * Le cœur n'a pas de file — elle est dans le device (qgpu-pci.c), avec son
 * thread de rendu, et c'est tests/qgpu_smoke.py qui l'exerce de bout en bout.
 * Ce qui SE TESTE ici, et qui casserait très silencieusement, c'est la CARTE
 * DES REGISTRES du contrat : les cinq registres ajoutés en v9 doivent être
 * distincts, alignés, sous QGPU_CTRL_TOPADDR — c'est ce même fichier que le
 * kext recopie, et une collision d'offset donnerait un invité qui lit la
 * barrière dans le compteur d'erreurs.
 *
 * Il exerce aussi l'invariant que le device tient sans thread : deux
 * soumissions à la suite s'exécutent dans l'ordre, chacune voyant l'état
 * laissée par la précédente — c'est exactement ce que la file promet. */
static void run_v9(QgpuCore *c, uint8_t *shmem)
{
    static const uint32_t regs[] = {
        QGPU_REG_MAGIC, QGPU_REG_VERSION, QGPU_REG_CAPS, QGPU_REG_SHMEM_SIZE,
        QGPU_REG_SUBMIT_OFF, QGPU_REG_SUBMIT_LEN, QGPU_REG_DOORBELL,
        QGPU_REG_FENCE, QGPU_REG_STATUS, QGPU_REG_STATUS_PC,
        QGPU_REG_IRQ_MASK, QGPU_REG_IRQ, QGPU_REG_DEBUG, QGPU_REG_BACKEND_NAME,
        QGPU_REG_QUEUE_FREE, QGPU_REG_FENCE_SUBMITTED, QGPU_REG_SUBMIT_ST,
        QGPU_REG_ERRORS, QGPU_REG_QUEUE_DEPTH,
    };
    const unsigned n = sizeof(regs) / sizeof(regs[0]);
    unsigned i, j, bad = 0;
    Emit e; uint32_t st;

    printf("-- v9 : file de soumissions --\n");
    for (i = 0; i < n; i++) {
        if ((regs[i] & 3) || regs[i] >= QGPU_CTRL_TOPADDR) {
            bad++;
        }
        for (j = i + 1; j < n; j++) {
            if (regs[i] == regs[j]) {
                bad++;
            }
        }
    }
    CHECK(bad == 0, "carte des registres : %u offsets alignés et distincts "
          "sous 0x%x (%u fautes)", n, (unsigned)QGPU_CTRL_TOPADDR, bad);
    CHECK(QGPU_PROTO_VERSION == 15, "version du protocole %d", QGPU_PROTO_VERSION);
    CHECK(QGPU_PROTO_MIN == 12, "version minimale d'attache %d", QGPU_PROTO_MIN);
    CHECK(QGPU_QUEUE_DEPTH >= 2 && (QGPU_QUEUE_DEPTH & (QGPU_QUEUE_DEPTH - 1)) == 0,
          "profondeur de file %d (puissance de 2, >= 2)", QGPU_QUEUE_DEPTH);
    CHECK((QGPU_DOORBELL_GO & QGPU_DOORBELL_ASYNC) == 0 && QGPU_DOORBELL_GO == 1,
          "bits du doorbell : GO=%d ASYNC=%d disjoints",
          QGPU_DOORBELL_GO, QGPU_DOORBELL_ASYNC);
    CHECK(QGPU_ST_QUEUE_FULL > QGPU_ST_BACKEND,
          "QGPU_ST_QUEUE_FULL=%d ne recouvre aucun statut antérieur",
          QGPU_ST_QUEUE_FULL);
    CHECK((QGPU_CAP_ASYNC & (QGPU_CAP_SOFT | QGPU_CAP_GL | QGPU_CAP_OCCLUSION)) == 0,
          "QGPU_CAP_ASYNC=0x%x disjoint des autres capacités", QGPU_CAP_ASYNC);

    /* Ordre : deux soumissions successives, la seconde relit ce que la
       première a laissé sur la surface. La file garantit cet ordre-là. */
    qgpu_core_reset(c);                 /* on repart d'un device vide */
    e.base = shmem; e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 1); emit(&e, W); emit(&e, H); emit(&e, QGPU_FMT_XRGB8888);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 1);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR));
    emit(&e, QGPU_CLEAR_COLOR); emit(&e, 0x00FF00); emit(&e, qgpu_f2u(1.0f));
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v9 : première soumission (st %u)", st);

    /* Soumission fautive entre les deux : elle ne doit rien casser pour la
       suivante — c'est l'indépendance que la file promet aussi. */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(0x7777, 1));
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_OPCODE, "v9 : soumission fautive (st %u)", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, STRIDE);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v9 : soumission suivante exécutée (st %u)", st);
    CHECK(px(shmem, 8, 8) == 0x00FF00,
          "v9 : elle voit l'état laissé par la première (0x%06x)",
          px(shmem, 8, 8));

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_DESTROY, QGPU_LEN_SURF)); emit(&e, 1);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_DESTROY, QGPU_LEN_CTX)); emit(&e, 0);
    (void)qgpu_core_execute(c, CMD_OFF, e.off - e.start);
}

/* ══════ v10 : textures 1D, 3D, cube, rectangle, profondeur, formats, LOD ══════
 *
 * Chaque cas a sa valeur attendue écrite à la main, et elle doit sortir
 * IDENTIQUE des deux backends : le logiciel est la référence, le GPU hôte doit
 * suivre. Les points testés sont au centre de texels (ou de bandes à couleur
 * constante) : jamais sur une frontière où l'arrondi du matériel décide. */

#define V10_CTX   12
#define V10_SURF  20
#define V10_TEX   300

static void tcreate3(Emit *e, uint32_t tex, uint32_t target)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_TEX_CREATE3, QGPU_LEN_TEX_CREATE3));
    emit(e, tex); emit(e, target);
}

static void timage3(Emit *e, uint32_t tex, uint32_t itarget, uint32_t lvl,
                    uint32_t w, uint32_t h, uint32_t d, uint32_t bfmt,
                    uint32_t fmt, uint32_t type, uint32_t off, uint32_t row, uint32_t img)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE3, QGPU_LEN_TEX_IMAGE3));
    emit(e, tex); emit(e, itarget); emit(e, lvl); emit(e, w); emit(e, h); emit(e, d);
    emit(e, bfmt); emit(e, fmt); emit(e, type); emit(e, off); emit(e, row); emit(e, img);
}

static void tsub(Emit *e, uint32_t tex, uint32_t itarget, uint32_t lvl,
                 uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d,
                 uint32_t fmt, uint32_t type, uint32_t off)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_TEX_SUBIMAGE, QGPU_LEN_TEX_SUBIMAGE));
    emit(e, tex); emit(e, itarget); emit(e, lvl); emit(e, x); emit(e, y); emit(e, z);
    emit(e, w); emit(e, h); emit(e, d); emit(e, fmt); emit(e, type); emit(e, off);
    emit(e, 0); emit(e, 0);
}

/* Image ARGB (mots big-endian) : GL_BGRA + GL_UNSIGNED_INT_8_8_8_8_REV. */
static void timage_argb(Emit *e, uint8_t *shmem, uint32_t tex, uint32_t itarget,
                        uint32_t lvl, uint32_t w, uint32_t h, uint32_t d,
                        const uint32_t *px, uint32_t off)
{
    uint32_t i;
    for (i = 0; i < w * h * d; i++) {
        qgpu_st32(shmem + off + i * 4, px[i]);
    }
    timage3(e, tex, itarget, lvl, w, h, d, 0x1908, 0x80E1, 0x8367, off, 0, 0);
}

static void vtx_str(Emit *v, float x, float y, uint32_t col, float s, float t, float r)
{
    emitf(v, x); emitf(v, y); emitf(v, 0.0f); emitf(v, 1.0f);
    emitf(v, ((col >> 16) & 255) / 255.0f); emitf(v, ((col >> 8) & 255) / 255.0f);
    emitf(v, (col & 255) / 255.0f); emitf(v, 1.0f);
    emitf(v, s); emitf(v, t); emitf(v, r); emitf(v, 1.0f);
}

/* Rectangle de pixels, (s, t) interpolés de (s0, t0) à (s1, t1), r constant. */
static void quad_str(Emit *v, float x0, float y0, float x1, float y1,
                     float s0, float t0, float s1, float t1, float r, uint32_t col)
{
    vtx_str(v, x0, y0, col, s0, t0, r); vtx_str(v, x1, y0, col, s1, t0, r);
    vtx_str(v, x1, y1, col, s1, t1, r); vtx_str(v, x0, y0, col, s0, t0, r);
    vtx_str(v, x1, y1, col, s1, t1, r); vtx_str(v, x0, y1, col, s0, t1, r);
}

/* Bande pleine largeur à direction constante (cartes de cube). */
static void band_dir(Emit *v, float y0, float y1, float dx, float dy, float dz)
{
    quad_str(v, 0, y0, (float)W, y1, dx, dy, dx, dy, dz, 0xFFFFFF);
}

/* Une soumission : pose la texture sur l'unité 0, efface, dessine `n`
   sommets texturés, relit la surface v10. */
static uint32_t v10_draw(QgpuCore *c, Emit *e, uint32_t tex, uint32_t n)
{
    state(e, QGPU_SK_TEX_BIND, tex);
    clear_cmd(e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    emit(e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_LEN_DRAW));
    emit(e, n); emit(e, VTX_OFF);
    readback_cmd(e, V10_SURF);
    return qgpu_core_execute(c, CMD_OFF, e->off - e->start);
}

static uint32_t v10_exec(QgpuCore *c, Emit *e)
{
    return qgpu_core_execute(c, CMD_OFF, e->off - e->start);
}

static bool near_argb(uint32_t a, uint32_t b, int tol)
{
    int k;
    for (k = 0; k < 32; k += 8) {
        int d = (int)((a >> k) & 255) - (int)((b >> k) & 255);
        if (d < -tol || d > tol) {
            return false;
        }
    }
    return true;
}

static void run_v10(QgpuCore *c, uint8_t *shmem)
{
    Emit e, v;
    uint32_t st, i, k;
    const uint32_t T = V10_TEX;

    printf("-- v10 : textures --\n");
    e.base = v.base = shmem;
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(&e, V10_CTX);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, V10_CTX);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, V10_SURF); emit(&e, W); emit(&e, H); emit(&e, QGPU_FMT_XRGB8888);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, V10_SURF);
    state(&e, QGPU_SK_TEXTURE, 1);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x1E01);            /* REPLACE */
    st = v10_exec(c, &e);
    CHECK(st == QGPU_ST_OK && (c->caps & QGPU_CAP_GL14),
          "v10 : contexte, surface, QGPU_CAP_GL14 annoncé (caps 0x%x, st %u)",
          c->caps, st);
    CHECK(QGPU_SK_TEX_LOD_BIAS0 + 3 < QGPU_SK_COUNT && QGPU_LEN_TEX_SUBIMAGE <= QGPU_MAX_CMD_ARGS + 1,
          "v10 : clés et longueurs dans leurs bornes (%d clés, SUBIMAGE %d mots)",
          QGPU_SK_COUNT, QGPU_LEN_TEX_SUBIMAGE);

    /* (a) TEXTURE 3D, au plus proche : 2×2×4, une couleur par tranche, envoyée
       en RGBA octets (conversion par l'hôte). Quatre bandes, r au centre de
       chaque tranche. */
    {
        static const uint8_t sl[4][4] = {
            { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 255, 255 } };
        for (k = 0; k < 4; k++) {
            for (i = 0; i < 4; i++) {
                memcpy(shmem + TEX_OFF + (k * 4 + i) * 4, sl[k], 4);
            }
        }
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T, QGPU_TT_3D);
        timage3(&e, T, QGPU_TT_3D, 0, 2, 2, 4, 0x1908, 0x1908, 0x1401, TEX_OFF, 0, 0);
        tparam(&e, T, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T, QGPU_TP_MAG_FILTER, 0x2600);
        v.off = v.start = VTX_OFF;
        for (k = 0; k < 4; k++) {
            quad_str(&v, 0, 16.0f * k, (float)W, 16.0f * k + 16, 0, 0, 1, 1,
                     (k + 0.5f) / 4.0f, 0xFFFFFF);
        }
        st = v10_draw(c, &e, T, 24);
        CHECK(st == QGPU_ST_OK && px(shmem, 32, 8) == 0xFF0000 && px(shmem, 32, 24) == 0x00FF00 &&
              px(shmem, 32, 40) == 0x0000FF && px(shmem, 32, 56) == 0xFFFFFF,
              "(a) 3D au plus proche, une tranche par bande : %06x %06x %06x %06x (st %u)",
              px(shmem, 32, 8), px(shmem, 32, 24), px(shmem, 32, 40), px(shmem, 32, 56), st);

        /* (b) filtrage LINÉAIRE en r : à mi-chemin des tranches 1 et 2 */
        e.off = e.start = CMD_OFF;
        tparam(&e, T, QGPU_TP_MIN_FILTER, 0x2601);
        tparam(&e, T, QGPU_TP_MAG_FILTER, 0x2601);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 1, 1, 0.5f, 0xFFFFFF);
        st = v10_draw(c, &e, T, 6);
        CHECK(st == QGPU_ST_OK && near_argb(px(shmem, 32, 32), 0x008080, 2),
              "(b) 3D trilinéaire, r entre deux tranches : %06x ≈ 008080 (st %u)",
              px(shmem, 32, 32), st);

        /* (c) répétition en r : REPEAT puis CLAMP_TO_EDGE */
        e.off = e.start = CMD_OFF;
        tparam(&e, T, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T, QGPU_TP_MAG_FILTER, 0x2600);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, 32, 0, 0, 1, 1, 1.125f, 0xFFFFFF);
        quad_str(&v, 0, 32, (float)W, (float)H, 0, 0, 1, 1, -0.625f, 0xFFFFFF);
        st = v10_draw(c, &e, T, 12);
        CHECK(st == QGPU_ST_OK && px(shmem, 32, 16) == 0xFF0000 && px(shmem, 32, 48) == 0x00FF00,
              "(c) WRAP_R REPEAT : r=1,125 → tranche 0, r=−0,625 → tranche 1 : %06x %06x (st %u)",
              px(shmem, 32, 16), px(shmem, 32, 48), st);
        e.off = e.start = CMD_OFF;
        tparam(&e, T, QGPU_TP_WRAP_R, 0x812F);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, 32, 0, 0, 1, 1, 1.3f, 0xFFFFFF);
        quad_str(&v, 0, 32, (float)W, (float)H, 0, 0, 1, 1, -0.2f, 0xFFFFFF);
        st = v10_draw(c, &e, T, 12);
        CHECK(st == QGPU_ST_OK && px(shmem, 32, 16) == 0xFFFFFF && px(shmem, 32, 48) == 0xFF0000,
              "(c) WRAP_R CLAMP_TO_EDGE : %06x %06x (st %u)",
              px(shmem, 32, 16), px(shmem, 32, 48), st);
    }

    /* (d) CARTE DE CUBE : six faces 1×1 de couleurs distinctes, six bandes à
       direction constante, chacune vers une face. */
    {
        static const uint32_t fc[6] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
                                        0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF };
        static const float dir[6][3] = { { 1, .1f, .2f }, { -1, .2f, .1f }, { .1f, 1, .2f },
                                         { .2f, -1, .1f }, { .1f, .2f, 1 }, { .2f, .1f, -1 } };
        bool ok = true;
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 1, QGPU_TT_CUBE_MAP);
        for (k = 0; k < 6; k++) {
            timage_argb(&e, shmem, T + 1, QGPU_TT_CUBE_FACE(k), 0, 1, 1, 1, &fc[k], TEX_OFF + k * 4);
        }
        tparam(&e, T + 1, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T + 1, QGPU_TP_MAG_FILTER, 0x2600);
        v.off = v.start = VTX_OFF;
        for (k = 0; k < 6; k++) {
            band_dir(&v, 10.0f * k, 10.0f * k + 10, dir[k][0], dir[k][1], dir[k][2]);
        }
        st = v10_draw(c, &e, T + 1, 36);
        for (k = 0; k < 6; k++) {
            ok = ok && px(shmem, 32, 10 * k + 5) == (fc[k] & 0xFFFFFF);
        }
        CHECK(st == QGPU_ST_OK && ok,
              "(d) cube, une face par axe : %06x %06x %06x %06x %06x %06x (st %u)",
              px(shmem, 32, 5), px(shmem, 32, 15), px(shmem, 32, 25), px(shmem, 32, 35),
              px(shmem, 32, 45), px(shmem, 32, 55), st);
    }

    /* (e) ORIENTATION DES FACES : +X en 2×2 [rouge, vert ; bleu, blanc] ; le
       GPU hôte fait foi, la table 3.21 d'OpenGL doit la reproduire. */
    {
        static const uint32_t px_x[4] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF };
        static const uint32_t blk[4] = { 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000 };
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 2, QGPU_TT_CUBE_MAP);
        timage_argb(&e, shmem, T + 2, QGPU_TT_CUBE_FACE(0), 0, 2, 2, 1, px_x, TEX_OFF);
        for (k = 1; k < 6; k++) {
            timage_argb(&e, shmem, T + 2, QGPU_TT_CUBE_FACE(k), 0, 2, 2, 1, blk, TEX_OFF + 16);
        }
        tparam(&e, T + 2, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T + 2, QGPU_TP_MAG_FILTER, 0x2600);
        v.off = v.start = VTX_OFF;
        band_dir(&v, 0, 16, 1, 0.5f, 0.5f);         /* s=t=0,25 → (0,0) rouge */
        band_dir(&v, 16, 32, 1, 0.5f, -0.5f);       /* s=0,75 t=0,25 → (1,0) vert */
        band_dir(&v, 32, 48, 1, -0.5f, 0.5f);       /* s=0,25 t=0,75 → (0,1) bleu */
        band_dir(&v, 48, 64, 1, -0.5f, -0.5f);      /* (1,1) blanc */
        st = v10_draw(c, &e, T + 2, 24);
        CHECK(st == QGPU_ST_OK && px(shmem, 32, 8) == 0xFF0000 && px(shmem, 32, 24) == 0x00FF00 &&
              px(shmem, 32, 40) == 0x0000FF && px(shmem, 32, 56) == 0xFFFFFF,
              "(e) cube, orientation de la face +X : %06x %06x %06x %06x (st %u)",
              px(shmem, 32, 8), px(shmem, 32, 24), px(shmem, 32, 40), px(shmem, 32, 56), st);

        /* (f) cube INCOMPLET (cinq faces) : texturage coupé, couleur du sommet */
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 3, QGPU_TT_CUBE_MAP);
        for (k = 0; k < 5; k++) {
            timage_argb(&e, shmem, T + 3, QGPU_TT_CUBE_FACE(k), 0, 2, 2, 1, px_x, TEX_OFF);
        }
        tparam(&e, T + 3, QGPU_TP_MIN_FILTER, 0x2600);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 1, 0.5f, 0.5f, 1, 0.5f, 0xFF3399);
        st = v10_draw(c, &e, T + 3, 6);
        CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0xFF3399,
              "(f) cube incomplet : texturage coupé, couleur du sommet %06x (st %u)",
              px(shmem, 32, 32), st);
    }

    /* (g) RECTANGLE 3×2, coordonnées en texels. */
    {
        static const uint32_t rp[6] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
                                        0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF };
        static const int cx[3] = { 10, 32, 53 }, cy[2] = { 16, 48 };
        bool ok = true;
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 4, QGPU_TT_RECTANGLE);
        timage_argb(&e, shmem, T + 4, QGPU_TT_RECTANGLE, 0, 3, 2, 1, rp, TEX_OFF);
        tparam(&e, T + 4, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T + 4, QGPU_TP_MAG_FILTER, 0x2600);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 3, 2, 0, 0xFFFFFF);
        st = v10_draw(c, &e, T + 4, 6);
        for (k = 0; k < 6; k++) {
            ok = ok && px(shmem, cx[k % 3], cy[k / 3]) == (rp[k] & 0xFFFFFF);
        }
        CHECK(st == QGPU_ST_OK && ok,
              "(g) rectangle 3×2 en texels : %06x %06x %06x / %06x %06x %06x (st %u)",
              px(shmem, 10, 16), px(shmem, 32, 16), px(shmem, 53, 16),
              px(shmem, 10, 48), px(shmem, 32, 48), px(shmem, 53, 48), st);
        {
            uint32_t s1, s2, s3, s4;
            e.off = e.start = CMD_OFF; tparam(&e, T + 4, QGPU_TP_WRAP_S, 0x2901); s1 = v10_exec(c, &e);
            e.off = e.start = CMD_OFF; tparam(&e, T + 4, QGPU_TP_MIN_FILTER, 0x2703); s2 = v10_exec(c, &e);
            e.off = e.start = CMD_OFF; tparam(&e, T + 4, QGPU_TP_BASE_LEVEL, 1); s3 = v10_exec(c, &e);
            e.off = e.start = CMD_OFF;
            timage_argb(&e, shmem, T + 4, QGPU_TT_RECTANGLE, 1, 1, 1, 1, rp, TEX_OFF);
            s4 = v10_exec(c, &e);
            CHECK(s1 == QGPU_ST_BAD_ARG && s2 == QGPU_ST_BAD_ARG && s3 == QGPU_ST_BAD_ARG &&
                  s4 == QGPU_ST_BAD_ARG,
                  "(g) rectangle : REPEAT, mipmaps, niveau de base 1, niveau 1 refusés : st %u %u %u %u",
                  s1, s2, s3, s4);
        }
    }

    /* (h) 1D : t est ignoré — même en GL_CLAMP et filtre linéaire, qui
       mêleraient la bordure noire à une texture 2D d'une ligne. */
    {
        static const uint32_t one[4] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF };
        static const uint32_t g1 = 0xFF00FF00;
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 5, QGPU_TT_1D);
        timage_argb(&e, shmem, T + 5, QGPU_TT_1D, 0, 4, 1, 1, one, TEX_OFF);
        tparam(&e, T + 5, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T + 5, QGPU_TP_MAG_FILTER, 0x2600);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 0, -5.3f, 1, 7.7f, 0, 0xFFFFFF);
        st = v10_draw(c, &e, T + 5, 6);
        CHECK(st == QGPU_ST_OK && px(shmem, 8, 5) == 0xFF0000 && px(shmem, 24, 30) == 0x00FF00 &&
              px(shmem, 40, 50) == 0x0000FF && px(shmem, 56, 60) == 0xFFFFFF,
              "(h) 1D au plus proche, t quelconque : %06x %06x %06x %06x (st %u)",
              px(shmem, 8, 5), px(shmem, 24, 30), px(shmem, 40, 50), px(shmem, 56, 60), st);
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 6, QGPU_TT_1D);
        timage_argb(&e, shmem, T + 6, QGPU_TT_1D, 0, 1, 1, 1, &g1, TEX_OFF);
        tparam(&e, T + 6, QGPU_TP_MIN_FILTER, 0x2601);
        tparam(&e, T + 6, QGPU_TP_MAG_FILTER, 0x2601);
        tparam(&e, T + 6, QGPU_TP_WRAP_T, 0x2900);         /* GL_CLAMP */
        st = v10_draw(c, &e, T + 6, 6);
        CHECK(st == QGPU_ST_OK && px(shmem, 5, 3) == 0x00FF00 && px(shmem, 60, 62) == 0x00FF00,
              "(h) 1D linéaire en GL_CLAMP : aucune bordure mêlée par t : %06x %06x (st %u)",
              px(shmem, 5, 3), px(shmem, 60, 62), st);
    }

    /* (i) GL_MIRRORED_REPEAT (1.4) : 2×1 rouge, bleu ; s de 0 à 2. */
    {
        static const uint32_t rb[2] = { 0xFFFF0000, 0xFF0000FF };
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 7, QGPU_TT_2D);
        timage_argb(&e, shmem, T + 7, QGPU_TT_2D, 0, 2, 1, 1, rb, TEX_OFF);
        tparam(&e, T + 7, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T + 7, QGPU_TP_MAG_FILTER, 0x2600);
        tparam(&e, T + 7, QGPU_TP_WRAP_S, QGPU_TW_MIRRORED_REPEAT);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 2, 1, 0, 0xFFFFFF);
        st = v10_draw(c, &e, T + 7, 6);
        CHECK(st == QGPU_ST_OK && px(shmem, 8, 32) == 0xFF0000 && px(shmem, 24, 32) == 0x0000FF &&
              px(shmem, 40, 32) == 0x0000FF && px(shmem, 56, 32) == 0xFF0000,
              "(i) MIRRORED_REPEAT : %06x %06x | %06x %06x (st %u)", px(shmem, 8, 32),
              px(shmem, 24, 32), px(shmem, 40, 32), px(shmem, 56, 32), st);
    }

    /* (j) GL_CLAMP_TO_BORDER (1.3) et couleur de bordure, alpha compris. */
    {
        static const uint32_t wh[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 8, QGPU_TT_2D);
        timage_argb(&e, shmem, T + 8, QGPU_TT_2D, 0, 2, 2, 1, wh, TEX_OFF);
        tparam(&e, T + 8, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T + 8, QGPU_TP_MAG_FILTER, 0x2600);
        tparam(&e, T + 8, QGPU_TP_WRAP_S, QGPU_TW_CLAMP_TO_BORDER);
        tparam(&e, T + 8, QGPU_TP_WRAP_T, QGPU_TW_CLAMP_TO_BORDER);
        tparam(&e, T + 8, QGPU_TP_BORDER_COLOR, 0x80FF8000);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, -1, 0, 2, 1, 0, 0xFFFFFF);
        st = v10_draw(c, &e, T + 8, 6);
        CHECK(st == QGPU_ST_OK && pxa(shmem, 10, 32) == 0x80FF8000 &&
              pxa(shmem, 32, 32) == 0xFFFFFFFF && pxa(shmem, 54, 32) == 0x80FF8000,
              "(j) CLAMP_TO_BORDER, bordure 80FF8000 : %08x %08x %08x (st %u)",
              pxa(shmem, 10, 32), pxa(shmem, 32, 32), pxa(shmem, 54, 32), st);
    }

    /* (k) PROFONDEUR (1.4) : 2×2 [0,2 0,4 ; 0,6 0,8] en GL_FLOAT, r = 0,5. */
    {
        static const float dv[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
        uint32_t tl, tr, bl, br;
        for (i = 0; i < 4; i++) {
            qgpu_st32(shmem + TEX_OFF + i * 4, qgpu_f2u(dv[i]));
        }
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 9, QGPU_TT_2D);
        timage3(&e, T + 9, QGPU_TT_2D, 0, 2, 2, 1, 0x1902, 0x1902, 0x1406, TEX_OFF, 0, 0);
        tparam(&e, T + 9, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T + 9, QGPU_TP_MAG_FILTER, 0x2600);
        tparam(&e, T + 9, QGPU_TP_COMPARE_MODE, QGPU_TC_COMPARE_R);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 1, 1, 0.5f, 0xFFFFFF);
        st = v10_draw(c, &e, T + 9, 6);
        tl = px(shmem, 16, 16); tr = px(shmem, 48, 16); bl = px(shmem, 16, 48); br = px(shmem, 48, 48);
        CHECK(st == QGPU_ST_OK && tl == 0 && tr == 0 && bl == 0xFFFFFF && br == 0xFFFFFF,
              "(k) comparaison LEQUAL, r=0,5 : %06x %06x / %06x %06x (st %u)", tl, tr, bl, br, st);
        e.off = e.start = CMD_OFF;
        tparam(&e, T + 9, QGPU_TP_COMPARE_FUNC, 0x0206);          /* GEQUAL */
        tparam(&e, T + 9, QGPU_TP_DEPTH_MODE, 0x1906);            /* ALPHA */
        st = v10_draw(c, &e, T + 9, 6);
        tl = pxa(shmem, 16, 16); bl = pxa(shmem, 16, 48);
        CHECK(st == QGPU_ST_OK && tl == 0xFFFFFFFF && bl == 0x00FFFFFF,
              "(k) GEQUAL, résultat en alpha : %08x / %08x (st %u)", tl, bl, st);
        e.off = e.start = CMD_OFF;
        tparam(&e, T + 9, QGPU_TP_COMPARE_MODE, QGPU_TC_NONE);
        tparam(&e, T + 9, QGPU_TP_DEPTH_MODE, 0x1909);            /* LUMINANCE */
        st = v10_draw(c, &e, T + 9, 6);
        tl = px(shmem, 16, 16); br = px(shmem, 48, 48);
        CHECK(st == QGPU_ST_OK && near_argb(tl, 0x333333, 1) && near_argb(br, 0xCCCCCC, 1),
              "(k) sans comparaison, D en luminance : %06x ≈ 333333, %06x ≈ cccccc (st %u)",
              tl, br, st);
        /* UNSIGNED_SHORT : 0xCCCC = 0,8 ≥ 0,5 */
        shmem[TEX_OFF] = 0xCC; shmem[TEX_OFF + 1] = 0xCC;
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 10, QGPU_TT_2D);
        timage3(&e, T + 10, QGPU_TT_2D, 0, 1, 1, 1, 0x1902, 0x1902, 0x1403, TEX_OFF, 0, 0);
        tparam(&e, T + 10, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T + 10, QGPU_TP_COMPARE_MODE, QGPU_TC_COMPARE_R);
        st = v10_draw(c, &e, T + 10, 6);
        CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0xFFFFFF,
              "(k) profondeur en UNSIGNED_SHORT, 0,5 ≤ 0,8 : %06x (st %u)", px(shmem, 32, 32), st);
    }

    /* (l) LOD (1.2, 1.4) : 4×4 rouge, 2×2 vert, 1×1 bleu, NEAREST_MIPMAP_NEAREST.
       Quad de 64 px : s de 0 à 16 → λ = 0, à 32 → λ = 1, à 64 → λ = 2. */
    {
        static const uint32_t red16[16] = {
            0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000,
            0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000,
            0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000 };
        static const uint32_t green4[4] = { 0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00 };
        static const uint32_t blue1 = 0xFF0000FF;
        static const struct { uint32_t key, val; float smax; uint32_t want; const char *what; } lc[] = {
            { 0, 0, 16, 0xFF0000, "λ=0 → niveau 0" },
            { 0, 0, 32, 0x00FF00, "λ=1 → niveau 1" },
            { 0, 0, 64, 0x0000FF, "λ=2 → niveau 2" },
            { QGPU_TP_BASE_LEVEL, 1, 16, 0x00FF00, "BASE_LEVEL 1" },
            { QGPU_TP_MAX_LEVEL, 1, 64, 0x00FF00, "MAX_LEVEL 1 à λ=2" },
            { QGPU_TP_MIN_LOD, 0x40000000, 16, 0x0000FF, "MIN_LOD 2 à λ=0" },
            { QGPU_TP_MAX_LOD, 0x3ECCCCCD, 64, 0xFF0000, "MAX_LOD 0,4 à λ=2" },
            { QGPU_TP_LOD_BIAS, 0x3F800000, 16, 0x00FF00, "biais de texture +1 à λ=0" },
        };
        static const uint32_t reset[] = { 0, 0, 0, 0, 1000, 0xC47A0000, 0x447A0000, 0 };
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 11, QGPU_TT_2D);
        timage_argb(&e, shmem, T + 11, QGPU_TT_2D, 0, 4, 4, 1, red16, TEX_OFF);
        timage_argb(&e, shmem, T + 11, QGPU_TT_2D, 1, 2, 2, 1, green4, TEX_OFF + 64);
        timage_argb(&e, shmem, T + 11, QGPU_TT_2D, 2, 1, 1, 1, &blue1, TEX_OFF + 80);
        tparam(&e, T + 11, QGPU_TP_MIN_FILTER, 0x2700);           /* NEAREST_MIPMAP_NEAREST */
        tparam(&e, T + 11, QGPU_TP_MAG_FILTER, 0x2600);
        st = v10_exec(c, &e);
        for (i = 0; i < sizeof(lc) / sizeof(lc[0]); i++) {
            e.off = e.start = CMD_OFF;
            if (lc[i].key) {
                tparam(&e, T + 11, lc[i].key, lc[i].val);
            }
            v.off = v.start = VTX_OFF;
            quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, lc[i].smax, lc[i].smax, 0, 0xFFFFFF);
            st = v10_draw(c, &e, T + 11, 6);
            CHECK(st == QGPU_ST_OK && px(shmem, 20, 20) == lc[i].want &&
                  px(shmem, 45, 50) == lc[i].want,
                  "(l) %s : %06x (attendu %06x, st %u)", lc[i].what, px(shmem, 20, 20),
                  lc[i].want, st);
            if (lc[i].key) {
                e.off = e.start = CMD_OFF;
                tparam(&e, T + 11, lc[i].key, reset[i]);
                v10_exec(c, &e);
            }
        }
        /* biais d'UNITÉ (glTexEnv, 1.4), qui s'ajoute à celui de la texture */
        e.off = e.start = CMD_OFF;
        tparam(&e, T + 11, QGPU_TP_LOD_BIAS, 0x3F800000);
        state(&e, QGPU_SK_TEX_LOD_BIAS0, 0x3F800000);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 16, 16, 0, 0xFFFFFF);
        st = v10_draw(c, &e, T + 11, 6);
        CHECK(st == QGPU_ST_OK && px(shmem, 20, 20) == 0x0000FF,
              "(l) biais de texture +1 et d'unité +1 à λ=0 → niveau 2 : %06x (st %u)",
              px(shmem, 20, 20), st);
        e.off = e.start = CMD_OFF;
        tparam(&e, T + 11, QGPU_TP_LOD_BIAS, 0);
        state(&e, QGPU_SK_TEX_LOD_BIAS0, 0);
        v10_exec(c, &e);
    }

    /* (m) MIPMAPS AUTOMATIQUES (1.4), calculés par le cœur : base 4×4 en quatre
       blocs 2×2 [rouge, bleu ; bleu, rouge]. */
    {
        uint32_t base[16], y, x;
        for (y = 0; y < 4; y++) {
            for (x = 0; x < 4; x++) {
                base[y * 4 + x] = ((x < 2) == (y < 2)) ? 0xFFFF0000 : 0xFF0000FF;
            }
        }
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 12, QGPU_TT_2D);
        tparam(&e, T + 12, QGPU_TP_GENERATE_MIPMAP, 1);
        timage_argb(&e, shmem, T + 12, QGPU_TT_2D, 0, 4, 4, 1, base, TEX_OFF);
        tparam(&e, T + 12, QGPU_TP_MIN_FILTER, 0x2700);
        tparam(&e, T + 12, QGPU_TP_MAG_FILTER, 0x2600);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 64, 64, 0, 0xFFFFFF);
        st = v10_draw(c, &e, T + 12, 6);
        CHECK(st == QGPU_ST_OK && px(shmem, 20, 20) == 0x800080,
              "(m) mipmap généré, niveau 2 = moyenne : %06x (attendu 800080, st %u)",
              px(shmem, 20, 20), st);
        e.off = e.start = CMD_OFF;
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 32, 32, 0, 0xFFFFFF);
        st = v10_draw(c, &e, T + 12, 6);
        CHECK(st == QGPU_ST_OK && px(shmem, 10, 10) == 0xFF0000 && px(shmem, 11, 10) == 0x0000FF &&
              px(shmem, 10, 11) == 0x0000FF && px(shmem, 11, 11) == 0xFF0000,
              "(m) niveau 1 généré : %06x %06x / %06x %06x (st %u)", px(shmem, 10, 10),
              px(shmem, 11, 10), px(shmem, 10, 11), px(shmem, 11, 11), st);
        /* une SOUS-IMAGE du niveau de base régénère la chaîne */
        for (i = 0; i < 16; i++) {
            qgpu_st32(shmem + TEX_OFF + i * 4, 0xFF00FF00);
        }
        e.off = e.start = CMD_OFF;
        tsub(&e, T + 12, QGPU_TT_2D, 0, 0, 0, 0, 4, 4, 1, 0x80E1, 0x8367, TEX_OFF);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 64, 64, 0, 0xFFFFFF);
        st = v10_draw(c, &e, T + 12, 6);
        CHECK(st == QGPU_ST_OK && px(shmem, 20, 20) == 0x00FF00,
              "(m) sous-image du niveau de base → niveau 2 régénéré : %06x (st %u)",
              px(shmem, 20, 20), st);
    }

    /* (n) FORMATS DE L'APPLICATION, convertis par l'hôte : deux texels chacun,
       RGBA de base, REPLACE — le mot relu est le texel. */
    {
        static const struct {
            uint32_t fmt, type; uint8_t b[8]; uint32_t want[2]; const char *name;
        } fc[] = {
            { 0x1908, 0x1401, { 10, 20, 30, 40, 50, 60, 70, 80 }, { 0x280A141E, 0x50323C46 }, "RGBA octets" },
            { 0x1907, 0x1401, { 10, 20, 30, 50, 60, 70 }, { 0xFF0A141E, 0xFF323C46 }, "RGB octets" },
            { 0x80E1, 0x1401, { 30, 20, 10, 40, 70, 60, 50, 80 }, { 0x280A141E, 0x50323C46 }, "BGRA octets" },
            { 0x80E0, 0x1401, { 30, 20, 10, 70, 60, 50 }, { 0xFF0A141E, 0xFF323C46 }, "BGR octets" },
            { 0x1909, 0x1401, { 10, 200 }, { 0xFF0A0A0A, 0xFFC8C8C8 }, "LUMINANCE" },
            { 0x190A, 0x1401, { 10, 40, 200, 80 }, { 0x280A0A0A, 0x50C8C8C8 }, "LUMINANCE_ALPHA" },
            { 0x1906, 0x1401, { 40, 80 }, { 0x28000000, 0x50000000 }, "ALPHA" },
            { 0x1903, 0x1401, { 10, 200 }, { 0xFF0A0000, 0xFFC80000 }, "RED" },
            { 0x1908, 0x8035, { 10, 20, 30, 40, 50, 60, 70, 80 }, { 0x280A141E, 0x50323C46 }, "RGBA 8888" },
            { 0x80E1, 0x8035, { 30, 20, 10, 40, 70, 60, 50, 80 }, { 0x280A141E, 0x50323C46 }, "BGRA 8888" },
            { 0x1908, 0x8367, { 40, 30, 20, 10, 80, 70, 60, 50 }, { 0x280A141E, 0x50323C46 }, "RGBA 8888_REV" },
            { 0x80E1, 0x8367, { 40, 10, 20, 30, 80, 50, 60, 70 }, { 0x280A141E, 0x50323C46 }, "BGRA 8888_REV" },
            { 0x1907, 0x8363, { 0x84, 0x08, 0xF8, 0x00 }, { 0xFF848242, 0xFFFF0000 }, "RGB 565" },
            { 0x1907, 0x8364, { 0x44, 0x10, 0x00, 0x1F }, { 0xFF848242, 0xFFFF0000 }, "RGB 565_REV" },
            { 0x1908, 0x8033, { 0x12, 0x34, 0xF0, 0x0F }, { 0x44112233, 0xFFFF0000 }, "RGBA 4444" },
            { 0x80E1, 0x8365, { 0x41, 0x23, 0xFF, 0x00 }, { 0x44112233, 0xFFFF0000 }, "BGRA 4444_REV" },
            { 0x1908, 0x8034, { 0xFC, 0x03, 0xF8, 0x00 }, { 0xFFFF8408, 0x00FF0000 }, "RGBA 5551" },
            { 0x80E1, 0x8366, { 0xFE, 0x01, 0x7C, 0x00 }, { 0xFFFF8408, 0x00FF0000 }, "BGRA 1555_REV" },
            /* H2 : les quatre couples CROISÉS, acceptés par tex_src depuis la
               v14 et émis par le plugin, que les `case` 16 bits décodaient
               sans regarder le format — R et B échangés. Chacun est le MIROIR
               exact de la ligne droite ci-dessus : mêmes texels attendus, mots
               réordonnés. Le second texel est un rouge pur, qui sortait bleu. */
            { 0x80E1, 0x8033, { 0x32, 0x14, 0x00, 0xFF }, { 0x44112233, 0xFFFF0000 }, "BGRA 4444" },
            { 0x1908, 0x8365, { 0x43, 0x21, 0xF0, 0x0F }, { 0x44112233, 0xFFFF0000 }, "RGBA 4444_REV" },
            { 0x80E1, 0x8034, { 0x0C, 0x3F, 0x00, 0x3E }, { 0xFFFF8408, 0x00FF0000 }, "BGRA 5551" },
            { 0x1908, 0x8366, { 0x86, 0x1F, 0x00, 0x1F }, { 0xFFFF8408, 0x00FF0000 }, "RGBA 1555_REV" },
        };
        for (i = 0; i < sizeof(fc) / sizeof(fc[0]); i++) {
            memcpy(shmem + TEX_OFF, fc[i].b, 8);
            e.off = e.start = CMD_OFF;
            tcreate3(&e, T + 20 + i, QGPU_TT_2D);
            timage3(&e, T + 20 + i, QGPU_TT_2D, 0, 2, 1, 1, 0x1908, fc[i].fmt, fc[i].type,
                    TEX_OFF, 0, 0);
            tparam(&e, T + 20 + i, QGPU_TP_MIN_FILTER, 0x2600);
            tparam(&e, T + 20 + i, QGPU_TP_MAG_FILTER, 0x2600);
            v.off = v.start = VTX_OFF;
            quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 1, 1, 0, 0xFFFFFF);
            st = v10_draw(c, &e, T + 20 + i, 6);
            CHECK(st == QGPU_ST_OK && pxa(shmem, 16, 32) == fc[i].want[0] &&
                  pxa(shmem, 48, 32) == fc[i].want[1],
                  "(n) %-15s : %08x %08x (attendu %08x %08x, st %u)", fc[i].name,
                  pxa(shmem, 16, 32), pxa(shmem, 48, 32), fc[i].want[0], fc[i].want[1], st);
        }
        /* H1 : GL_ALPHA en FORMAT DE BASE. Le cœur le promouvait en RGBA, si
           bien que le texel blanchi passait en couleur : REPLACE rendait du
           blanc au lieu de la couleur primaire (polices et HUD blancs). La
           table 3.22 dit Cv = Cf et Av = At : la couleur du sommet survit,
           seul l'alpha vient de la texture. */
        {
            uint32_t want = 0x80FF8000u;
            shmem[TEX_OFF] = 0x80;
            e.off = e.start = CMD_OFF;
            tcreate3(&e, T + 45, QGPU_TT_2D);
            timage3(&e, T + 45, QGPU_TT_2D, 0, 1, 1, 1, 0x1906, 0x1906, 0x1401,
                    TEX_OFF, 0, 0);
            tparam(&e, T + 45, QGPU_TP_MIN_FILTER, 0x2600);
            tparam(&e, T + 45, QGPU_TP_MAG_FILTER, 0x2600);
            v.off = v.start = VTX_OFF;
            quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 1, 1, 0, 0xFF8000);
            st = v10_draw(c, &e, T + 45, 6);
            CHECK(st == QGPU_ST_OK && pxa(shmem, 32, 32) == want,
                  "(n) ALPHA en format de base, REPLACE : %08x (attendu %08x, st %u)",
                  pxa(shmem, 32, 32), want, st);
        }

        /* pas de ligne (alignement) et pas de tranche (3D) */
        {
            static const uint8_t rows[16] = { 10, 20, 30, 50, 60, 70, 0xEE, 0xEE,
                                              90, 100, 110, 130, 140, 150, 0xEE, 0xEE };
            memcpy(shmem + TEX_OFF, rows, 16);
            e.off = e.start = CMD_OFF;
            tcreate3(&e, T + 43, QGPU_TT_2D);
            timage3(&e, T + 43, QGPU_TT_2D, 0, 2, 2, 1, 0x1907, 0x1907, 0x1401, TEX_OFF, 8, 0);
            tparam(&e, T + 43, QGPU_TP_MIN_FILTER, 0x2600);
            tparam(&e, T + 43, QGPU_TP_MAG_FILTER, 0x2600);
            v.off = v.start = VTX_OFF;
            quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 1, 1, 0, 0xFFFFFF);
            st = v10_draw(c, &e, T + 43, 6);
            CHECK(st == QGPU_ST_OK && px(shmem, 16, 16) == 0x0A141E && px(shmem, 48, 16) == 0x323C46 &&
                  px(shmem, 16, 48) == 0x5A646E && px(shmem, 48, 48) == 0x828C96,
                  "(n) RGB avec 8 octets par ligne : %06x %06x / %06x %06x (st %u)",
                  px(shmem, 16, 16), px(shmem, 48, 16), px(shmem, 16, 48), px(shmem, 48, 48), st);
            qgpu_st32(shmem + TEX_OFF, 0xFFFF0000);
            qgpu_st32(shmem + TEX_OFF + 4, 0xEEEEEEEE);
            qgpu_st32(shmem + TEX_OFF + 8, 0xFF00FF00);
            e.off = e.start = CMD_OFF;
            tcreate3(&e, T + 44, QGPU_TT_3D);
            timage3(&e, T + 44, QGPU_TT_3D, 0, 1, 1, 2, 0x1908, 0x80E1, 0x8367, TEX_OFF, 0, 8);
            tparam(&e, T + 44, QGPU_TP_MIN_FILTER, 0x2600);
            tparam(&e, T + 44, QGPU_TP_MAG_FILTER, 0x2600);
            v.off = v.start = VTX_OFF;
            quad_str(&v, 0, 0, (float)W, 32, 0, 0, 1, 1, 0.25f, 0xFFFFFF);
            quad_str(&v, 0, 32, (float)W, (float)H, 0, 0, 1, 1, 0.75f, 0xFFFFFF);
            st = v10_draw(c, &e, T + 44, 12);
            CHECK(st == QGPU_ST_OK && px(shmem, 32, 16) == 0xFF0000 && px(shmem, 32, 48) == 0x00FF00,
                  "(n) 3D avec 8 octets par tranche : %06x %06x (st %u)",
                  px(shmem, 32, 16), px(shmem, 32, 48), st);
        }
    }

    /* (o) S3TC, décompressé par le cœur. Un bloc 4×4, index 0,1,2,3 sur chaque
       ligne : les quatre couleurs du bloc, colonne par colonne. */
    {
        static const struct { uint32_t fmt, bfmt; uint8_t b[16]; uint32_t want[4]; const char *name; } dc[] = {
            { QGPU_TF_DXT1_RGB, 0x1907,
              { 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 },
              { 0xFFFF0000, 0xFF0000FF, 0xFFAA0055, 0xFF5500AA }, "DXT1 RGB, 4 couleurs" },
            { QGPU_TF_DXT1_RGBA, 0x1908,
              { 0x1F, 0x00, 0x00, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4 },
              { 0xFF0000FF, 0xFFFF0000, 0xFF800080, 0x00000000 }, "DXT1 RGBA, 3 couleurs" },
            { QGPU_TF_DXT3, 0x1908,
              { 0x8F, 0x04, 0x8F, 0x04, 0x8F, 0x04, 0x8F, 0x04,
                0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 },
              { 0xFFFF0000, 0x880000FF, 0x44AA0055, 0x005500AA }, "DXT3" },
            { QGPU_TF_DXT5, 0x1908,
              { 0xFF, 0x00, 0x88, 0x8E, 0xE8, 0x88, 0x8E, 0xE8,
                0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 },
              { 0xFFFF0000, 0x000000FF, 0xDBAA0055, 0x245500AA }, "DXT5" },
        };
        for (i = 0; i < sizeof(dc) / sizeof(dc[0]); i++) {
            bool ok;
            memcpy(shmem + TEX_OFF, dc[i].b, 16);
            e.off = e.start = CMD_OFF;
            tcreate3(&e, T + 50 + i, QGPU_TT_2D);
            timage3(&e, T + 50 + i, QGPU_TT_2D, 0, 4, 4, 1, dc[i].bfmt, dc[i].fmt, 0,
                    TEX_OFF, 0, 0);
            tparam(&e, T + 50 + i, QGPU_TP_MIN_FILTER, 0x2600);
            tparam(&e, T + 50 + i, QGPU_TP_MAG_FILTER, 0x2600);
            v.off = v.start = VTX_OFF;
            quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 1, 1, 0, 0xFFFFFF);
            st = v10_draw(c, &e, T + 50 + i, 6);
            ok = true;
            for (k = 0; k < 4; k++) {
                ok = ok && pxa(shmem, 8 + 16 * k, 40) == dc[i].want[k];
            }
            CHECK(st == QGPU_ST_OK && ok, "(o) %s : %08x %08x %08x %08x (st %u)", dc[i].name,
                  pxa(shmem, 8, 40), pxa(shmem, 24, 40), pxa(shmem, 40, 40), pxa(shmem, 56, 40), st);
        }
    }

    /* (p) NIVEAU SANS DONNÉES puis SOUS-IMAGE : noir transparent, puis la
       moitié droite en vert. */
    {
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 60, QGPU_TT_2D);
        timage3(&e, T + 60, QGPU_TT_2D, 0, 4, 4, 1, 0x1908, 0x80E1, 0x8367,
                (uint32_t)QGPU_TEX_NO_DATA, 0, 0);
        tparam(&e, T + 60, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T + 60, QGPU_TP_MAG_FILTER, 0x2600);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 1, 1, 0, 0xFFFFFF);
        st = v10_draw(c, &e, T + 60, 6);
        CHECK(st == QGPU_ST_OK && pxa(shmem, 16, 16) == 0 && pxa(shmem, 48, 48) == 0,
              "(p) niveau défini sans données : %08x %08x (st %u)",
              pxa(shmem, 16, 16), pxa(shmem, 48, 48), st);
        for (i = 0; i < 8; i++) {
            qgpu_st32(shmem + TEX_OFF + i * 4, 0xFF00FF00);
        }
        e.off = e.start = CMD_OFF;
        tsub(&e, T + 60, QGPU_TT_2D, 0, 2, 0, 0, 2, 4, 1, 0x80E1, 0x8367, TEX_OFF);
        st = v10_draw(c, &e, T + 60, 6);
        CHECK(st == QGPU_ST_OK && pxa(shmem, 16, 16) == 0 && pxa(shmem, 48, 16) == 0xFF00FF00 &&
              pxa(shmem, 48, 56) == 0xFF00FF00,
              "(p) sous-image 2×4 en (2,0) : %08x | %08x %08x (st %u)",
              pxa(shmem, 16, 16), pxa(shmem, 48, 16), pxa(shmem, 48, 56), st);
    }

    /* (q) CHEMIN BRUT (v7) : la même texture 3D et la même carte de cube,
       sommets bruts ; le cube par GL_NORMAL_MAP, la normale (−1, 0, 0) → face −X. */
    {
        float m[16], mv[16];
        static const float z4[4] = { 0, 0, 0, 0 };
        e.off = e.start = CMD_OFF;
        mat_ortho_px(m);
        mat_identity(mv);
        set_matrix(&e, QGPU_MTX_PROJECTION, m);
        set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
        state(&e, QGPU_SK_TEX_BIND, T);
        clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
        v.off = v.start = VTX_OFF;
        {
            static const float q[6][2] = { { 0, 0 }, { 64, 0 }, { 64, 64 }, { 0, 0 }, { 64, 64 }, { 0, 64 } };
            for (i = 0; i < 6; i++) {
                emitf(&v, q[i][0]); emitf(&v, q[i][1]);
                emitf(&v, 0.5f); emitf(&v, 0.5f); emitf(&v, 0.625f); emitf(&v, 1.0f);
            }
        }
        draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 6, VF_P2 | QGPU_VF_TEX(0), 6, QGPU_IDX_NONE, 0);
        readback_cmd(&e, V10_SURF);
        st = v10_exec(c, &e);
        CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0x0000FF,
              "(q) chemin brut, 3D, r = 0,625 → tranche 2 : %06x (st %u)", px(shmem, 32, 32), st);
        e.off = e.start = CMD_OFF;
        for (k = 0; k < 3; k++) {
            set_texgen(&e, 0, k, 1, QGPU_TG_NORMAL_MAP, z4, z4);
        }
        state(&e, QGPU_SK_TEX_BIND, T + 1);
        clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
        v.off = v.start = VTX_OFF;
        {
            static const float q[6][2] = { { 0, 0 }, { 64, 0 }, { 64, 64 }, { 0, 0 }, { 64, 64 }, { 0, 64 } };
            for (i = 0; i < 6; i++) {
                emitf(&v, q[i][0]); emitf(&v, q[i][1]); emitf(&v, 0.0f);
                emitf(&v, -1.0f); emitf(&v, 0.0f); emitf(&v, 0.0f);
            }
        }
        draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 6, VF_P3 | QGPU_VF_NORMAL, 6, QGPU_IDX_NONE, 0);
        readback_cmd(&e, V10_SURF);
        st = v10_exec(c, &e);
        CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0x00FF00,
              "(q) chemin brut, cube par GL_NORMAL_MAP, normale −X : %06x (st %u)",
              px(shmem, 32, 32), st);
        e.off = e.start = CMD_OFF;
        for (k = 0; k < 3; k++) {
            set_texgen(&e, 0, k, 0, QGPU_TG_EYE_LINEAR, z4, z4);
        }
        v10_exec(c, &e);
    }

    /* (u) CHEMIN BRUT et PROFONDEUR (1.4) : la texture de (k) — 2×2
       [0,2 0,4 ; 0,6 0,8] — dessinée par DRAW_RAW, r = 0,5 dans le sommet. */
    {
        static const struct { float s, t; uint32_t cmp; uint32_t want; const char *what; } dc[] = {
            { 0.25f, 0.25f, QGPU_TC_NONE,      0x333333, "D = 0,2 en luminance" },
            { 0.75f, 0.75f, QGPU_TC_NONE,      0xCCCCCC, "D = 0,8 en luminance" },
            { 0.25f, 0.25f, QGPU_TC_COMPARE_R, 0x000000, "LEQUAL 0,5 ≤ 0,2 → 0" },
            { 0.25f, 0.75f, QGPU_TC_COMPARE_R, 0xFFFFFF, "LEQUAL 0,5 ≤ 0,6 → 1" },
        };
        static const float q[6][2] = { { 0, 0 }, { 64, 0 }, { 64, 64 }, { 0, 0 }, { 64, 64 }, { 0, 64 } };
        for (k = 0; k < (int)(sizeof(dc) / sizeof(dc[0])); k++) {
            uint32_t got;
            e.off = e.start = CMD_OFF;
            tparam(&e, T + 9, QGPU_TP_COMPARE_MODE, dc[k].cmp);
            tparam(&e, T + 9, QGPU_TP_COMPARE_FUNC, 0x0203);
            tparam(&e, T + 9, QGPU_TP_DEPTH_MODE, 0x1909);
            state(&e, QGPU_SK_TEX_BIND, T + 9);
            state(&e, QGPU_SK_UNIT(0) + QGPU_SK_U_ENV_MODE, 0x1E01);   /* REPLACE */
            clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
            v.off = v.start = VTX_OFF;
            for (i = 0; i < 6; i++) {
                emitf(&v, q[i][0]); emitf(&v, q[i][1]);
                emitf(&v, dc[k].s); emitf(&v, dc[k].t); emitf(&v, 0.5f); emitf(&v, 1.0f);
            }
            draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 6, VF_P2 | QGPU_VF_TEX(0), 6, QGPU_IDX_NONE, 0);
            readback_cmd(&e, V10_SURF);
            st = v10_exec(c, &e);
            got = px(shmem, 32, 32);
            CHECK(st == QGPU_ST_OK && near_argb(got, dc[k].want, 1),
                  "(u) chemin brut, profondeur, %s : %06x (st %u)", dc[k].what, got, st);
        }
        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_UNIT(0) + QGPU_SK_U_ENV_MODE, 0x2100);       /* MODULATE */
        tparam(&e, T + 9, QGPU_TP_COMPARE_MODE, QGPU_TC_NONE);
        v10_exec(c, &e);
    }

    /* (s) COULEUR SECONDAIRE (1.4), chemin brut : primaire (0, 0,4, 0),
       secondaire (0,4, 0, 0). QGPU_CSUM_FORMAT (initial) garde la règle v7–v9. */
    {
        static const float q[6][2] = { { 0, 0 }, { 64, 0 }, { 64, 64 }, { 0, 0 }, { 64, 64 }, { 0, 64 } };
        static const struct { uint32_t cs; bool sec_in_fmt; uint32_t want; const char *what; } sc[] = {
            { QGPU_CSUM_FORMAT, true,  0x666600, "initiale, secondaire dans le format → ajoutée (v7–v9)" },
            { QGPU_CSUM_OFF,    true,  0x006600, "coupée, secondaire dans le format → ignorée" },
            { QGPU_CSUM_ON,     false, 0x666600, "allumée, valeur courante → ajoutée" },
            { QGPU_CSUM_FORMAT, false, 0x006600, "initiale, hors du format → ignorée (v7–v9)" },
        };
        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_TEXTURE, 0);
        set_current(&e, QGPU_CUR_SEC_COLOR, 0.4f, 0.0f, 0.0f, 0.0f);
        v10_exec(c, &e);
        for (i = 0; i < sizeof(sc) / sizeof(sc[0]); i++) {
            uint32_t fmt = VF_P2 | QGPU_VF_COLOR | (sc[i].sec_in_fmt ? QGPU_VF_SEC_COLOR : 0);
            v.off = v.start = VTX_OFF;
            for (k = 0; k < 6; k++) {
                emitf(&v, q[k][0]); emitf(&v, q[k][1]);
                emitf(&v, 0.0f); emitf(&v, 0.4f); emitf(&v, 0.0f); emitf(&v, 1.0f);
                if (sc[i].sec_in_fmt) {
                    emitf(&v, 0.4f); emitf(&v, 0.0f); emitf(&v, 0.0f);
                }
            }
            e.off = e.start = CMD_OFF;
            state(&e, QGPU_SK_COLOR_SUM, sc[i].cs);
            clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
            draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 6, fmt, 6, QGPU_IDX_NONE, 0);
            readback_cmd(&e, V10_SURF);
            st = v10_exec(c, &e);
            CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == sc[i].want,
                  "(s) GL_COLOR_SUM %s : %06x (attendu %06x, st %u)", sc[i].what,
                  px(shmem, 32, 32), sc[i].want, st);
        }
        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_COLOR_SUM, QGPU_CSUM_FORMAT);
        set_current(&e, QGPU_CUR_SEC_COLOR, 0.0f, 0.0f, 0.0f, 0.0f);
        v10_exec(c, &e);
    }

    /* (t) PARAMÈTRES DE POINT (1.4), chemin brut. Trois points à 20, 40 et 60
       de l'œil (modèle-vue identité : d = |(x, y)|), taille 8, c = 1/400 :
       tailles dérivées 8, 4 et 2,67. Chaque pixel testé est à plus d'un
       demi-pixel du bord de son carré : aucune règle de couverture ne décide. */
    {
        static const float pt[3][2] = { { 12, 16 }, { 24, 32 }, { 36, 48 } };
        uint32_t in1, out1, in2, out2, mx1, mn3;
        v.off = v.start = VTX_OFF;
        for (k = 0; k < 3; k++) {
            emitf(&v, pt[k][0]); emitf(&v, pt[k][1]);
        }
        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_POINT_SIZE, 0x41000000);            /* 8 */
        clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
        draw_raw(&e, QGPU_PRIM_MODE_POINTS, 3, VF_P2, 3, QGPU_IDX_NONE, 0);
        readback_cmd(&e, V10_SURF);
        st = v10_exec(c, &e);
        CHECK(st == QGPU_ST_OK && px(shmem, 27, 32) == 0xFFFFFF && px(shmem, 38, 48) == 0xFFFFFF,
              "(t) sans atténuation : taille 8 partout : %06x %06x (st %u)",
              px(shmem, 27, 32), px(shmem, 38, 48), st);

        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_POINT_ATT_CONST, 0);
        state(&e, QGPU_SK_POINT_ATT_QUAD, qgpu_f2u(1.0f / 400.0f));
        clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
        draw_raw(&e, QGPU_PRIM_MODE_POINTS, 3, VF_P2, 3, QGPU_IDX_NONE, 0);
        readback_cmd(&e, V10_SURF);
        st = v10_exec(c, &e);
        in1 = px(shmem, 14, 18); out1 = px(shmem, 17, 16);
        in2 = px(shmem, 25, 33); out2 = px(shmem, 27, 32);
        CHECK(st == QGPU_ST_OK && in1 == 0xFFFFFF && out1 == 0 && in2 == 0xFFFFFF && out2 == 0,
              "(t) atténuation c·d² : d=20 → 8 (%06x dedans, %06x dehors), d=40 → 4 "
              "(%06x dedans, %06x dehors) (st %u)", in1, out1, in2, out2, st);

        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_POINT_SIZE_MAX, 0x40C00000);        /* 6 */
        clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
        draw_raw(&e, QGPU_PRIM_MODE_POINTS, 3, VF_P2, 3, QGPU_IDX_NONE, 0);
        readback_cmd(&e, V10_SURF);
        st = v10_exec(c, &e);
        mx1 = px(shmem, 15, 16);
        CHECK(st == QGPU_ST_OK && mx1 == 0 && px(shmem, 14, 16) == 0xFFFFFF,
              "(t) POINT_SIZE_MAX 6 : d=20 borné à 6 : %06x dehors, %06x dedans (st %u)",
              mx1, px(shmem, 14, 16), st);

        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_POINT_SIZE_MAX, 0x42800000);
        state(&e, QGPU_SK_POINT_SIZE_MIN, 0x40C00000);        /* 6 */
        state(&e, QGPU_SK_POINT_FADE, 0x42000000);            /* 32 : sans effet */
        clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
        draw_raw(&e, QGPU_PRIM_MODE_POINTS, 3, VF_P2, 3, QGPU_IDX_NONE, 0);
        readback_cmd(&e, V10_SURF);
        st = v10_exec(c, &e);
        mn3 = px(shmem, 38, 48);
        CHECK(st == QGPU_ST_OK && mn3 == 0xFFFFFF && pxa(shmem, 25, 33) == 0xFFFFFFFF,
              "(t) POINT_SIZE_MIN 6 : d=60 porté à 6 (%06x) ; seuil de fondu sans effet "
              "(%08x) (st %u)", mn3, pxa(shmem, 25, 33), st);

        /* mode de polygone GL_POINT : mêmes tailles dérivées, sommet par sommet */
        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_POINT_SIZE_MIN, 0);
        state(&e, QGPU_SK_POLYGON_MODE_FRONT, QGPU_POLY_POINT);
        state(&e, QGPU_SK_POLYGON_MODE_BACK, QGPU_POLY_POINT);
        clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
        draw_raw(&e, QGPU_PRIM_MODE_TRIANGLES, 3, VF_P2, 3, QGPU_IDX_NONE, 0);
        readback_cmd(&e, V10_SURF);
        st = v10_exec(c, &e);
        CHECK(st == QGPU_ST_OK && px(shmem, 25, 33) == 0xFFFFFF && px(shmem, 27, 32) == 0 &&
              px(shmem, 14, 18) == 0xFFFFFF,
              "(t) polygone en GL_POINT : d=40 → 4 (%06x dedans, %06x dehors) (st %u)",
              px(shmem, 25, 33), px(shmem, 27, 32), st);

        /* les anciens opcodes n'en voient rien : DRAW_POINTS reste à 8 */
        v.off = v.start = VTX_OFF;
        vertex(&v, 24, 32, 1, 1, 1);
        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_POLYGON_MODE_FRONT, QGPU_POLY_FILL);
        state(&e, QGPU_SK_POLYGON_MODE_BACK, QGPU_POLY_FILL);
        clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
        emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_POINTS, QGPU_LEN_DRAW)); emit(&e, 1); emit(&e, VTX_OFF);
        readback_cmd(&e, V10_SURF);
        st = v10_exec(c, &e);
        CHECK(st == QGPU_ST_OK && px(shmem, 27, 32) == 0xFFFFFF,
              "(t) DRAW_POINTS (v4) ignore l'atténuation : taille 8 : %06x (st %u)",
              px(shmem, 27, 32), st);

        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_POINT_ATT_CONST, 0x3F800000);
        state(&e, QGPU_SK_POINT_ATT_QUAD, 0);
        state(&e, QGPU_SK_POINT_FADE, 0x3F800000);
        state(&e, QGPU_SK_POINT_SIZE, 0x3F800000);
        state(&e, QGPU_SK_TEXTURE, 1);
        v10_exec(c, &e);
    }

    /* (r) REFUS : chacun doit rendre le bon statut sans rien casser. */
    {
        static const struct { uint32_t want; const char *what; } er[] = {
            { QGPU_ST_BAD_ARG, "TEX_CREATE3, cible inconnue" },
            { QGPU_ST_BAD_ARG, "image 3D sur une texture 2D" },
            { QGPU_ST_BAD_ARG, "face de cube non carrée" },
            { QGPU_ST_BAD_ARG, "3D au-delà de QGPU_MAX_TEX_3D_DIM" },
            { QGPU_ST_BAD_ARG, "couple (format, type) inconnu" },
            { QGPU_ST_BAD_ARG, "données de profondeur, base RGBA" },
            { QGPU_ST_BAD_ARG, "S3TC sur une texture 3D" },
            { QGPU_ST_BAD_ARG, "pas de ligne plus court qu'une ligne" },
            { QGPU_ST_OOB,     "données hors de la fenêtre" },
            { QGPU_ST_BAD_ARG, "sous-image hors du niveau" },
            { QGPU_ST_BAD_ARG, "sous-image d'un niveau non défini" },
            { QGPU_ST_BAD_ARG, "sous-image S3TC non alignée" },
            { QGPU_ST_BAD_ARG, "TEX_IMAGE (v3) sur une texture 3D" },
            { QGPU_ST_BAD_ARG, "WRAP_S inconnu" },
            { QGPU_ST_BAD_ARG, "biais de LOD de 17" },
            { QGPU_ST_BAD_ARG, "mode de comparaison inconnu" },
            { QGPU_ST_BAD_ARG, "biais d'unité NaN (SET_STATE)" },
            { QGPU_ST_BAD_ARG, "profondeur sur une texture 3D" },
            { QGPU_ST_BAD_ARG, "COLOR_SUM = 3" },
            { QGPU_ST_BAD_ARG, "POINT_SIZE_MAX = 0" },
        };
        uint32_t n = sizeof(er) / sizeof(er[0]), got[20];
        bool ok = true;
        for (i = 0; i < n; i++) {
            e.off = e.start = CMD_OFF;
            switch (i) {
            case 0: tcreate3(&e, T + 70, 0x1234); break;
            case 1: timage3(&e, T + 7, QGPU_TT_3D, 0, 2, 1, 1, 0x1908, 0x80E1, 0x8367, TEX_OFF, 0, 0); break;
            case 2: timage3(&e, T + 1, QGPU_TT_CUBE_FACE(0), 0, 2, 1, 1, 0x1908, 0x80E1, 0x8367, TEX_OFF, 0, 0); break;
            case 3: timage3(&e, T, QGPU_TT_3D, 0, 512, 1, 1, 0x1908, 0x80E1, 0x8367, (uint32_t)QGPU_TEX_NO_DATA, 0, 0); break;
            case 4: timage3(&e, T + 7, QGPU_TT_2D, 0, 2, 1, 1, 0x1908, 0x1908, 0x8363, TEX_OFF, 0, 0); break;
            case 5: timage3(&e, T + 7, QGPU_TT_2D, 0, 2, 1, 1, 0x1908, 0x1902, 0x1406, TEX_OFF, 0, 0); break;
            case 6: timage3(&e, T, QGPU_TT_3D, 0, 4, 4, 1, 0x1908, QGPU_TF_DXT5, 0, TEX_OFF, 0, 0); break;
            case 7: timage3(&e, T + 7, QGPU_TT_2D, 0, 2, 1, 1, 0x1908, 0x1908, 0x1401, TEX_OFF, 4, 0); break;
            case 8: timage3(&e, T + 7, QGPU_TT_2D, 0, 2, 1, 1, 0x1908, 0x1908, 0x1401, SHMEM_SIZE - 4, 0, 0); break;
            case 9: tsub(&e, T + 60, QGPU_TT_2D, 0, 3, 0, 0, 2, 1, 1, 0x80E1, 0x8367, TEX_OFF); break;
            case 10: tsub(&e, T + 60, QGPU_TT_2D, 1, 0, 0, 0, 1, 1, 1, 0x80E1, 0x8367, TEX_OFF); break;
            case 11: tsub(&e, T + 50, QGPU_TT_2D, 0, 1, 0, 0, 1, 1, 1, QGPU_TF_DXT1_RGB, 0, TEX_OFF); break;
            case 12:
                emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
                emit(&e, T); emit(&e, 0); emit(&e, 1); emit(&e, 1); emit(&e, 0x1908); emit(&e, TEX_OFF);
                break;
            case 13: tparam(&e, T + 7, QGPU_TP_WRAP_S, 0x1234); break;
            case 14: tparam(&e, T + 7, QGPU_TP_LOD_BIAS, 0x41880000); break;
            case 15: tparam(&e, T + 9, QGPU_TP_COMPARE_MODE, 0x884D); break;
            case 16: state(&e, QGPU_SK_TEX_LOD_BIAS0 + 1, 0x7FC00000); break;
            case 17: timage3(&e, T, QGPU_TT_3D, 0, 1, 1, 1, 0x1902, 0x1902, 0x1406, TEX_OFF, 0, 0); break;
            case 18: state(&e, QGPU_SK_COLOR_SUM, 3); break;
            default: state(&e, QGPU_SK_POINT_SIZE_MAX, 0); break;
            }
            got[i] = v10_exec(c, &e);
            if (got[i] != er[i].want) {
                ok = false;
                printf("       refus « %s » : st %u, attendu %u\n", er[i].what, got[i], er[i].want);
            }
        }
        CHECK(ok, "(r) %u refus v10, chacun avec son statut", n);
        /* et le cœur est toujours là : la texture 3D rend encore sa tranche */
        v.off = v.start = VTX_OFF;
        e.off = e.start = CMD_OFF;
        quad_str(&v, 0, 0, (float)W, (float)H, 0, 0, 1, 1, 0.375f, 0xFFFFFF);
        st = v10_draw(c, &e, T, 6);
        CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0x00FF00,
              "(r) après les refus, la texture 3D rend toujours : %06x (st %u)",
              px(shmem, 32, 32), st);
    }

    /* (o) glDrawPixels tel que le plugin l'émet : RGB octets, largeur IMPAIRE
       (13), lignes de 40 octets (39 utiles, arrondi à 4), quad de 13×5 pixels
       au pixel près, NEAREST. Vu en VM (scène drawpack, 22/09) : lignes
       brouillées (38/0/0/24/1 pixels par couleur au lieu de 13 chacune). Chaque
       ligne de l'image doit ressortir avec SA couleur, sur les deux backends. */
    {
        static const uint8_t rowc[5][3] = {
            { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 }, { 0, 255, 255 } };
        uint32_t x, y, bad = 0, first = 0;
        memset(shmem + TEX_OFF, 0xA5, 40 * 5);
        for (y = 0; y < 5; y++)
            for (x = 0; x < 13; x++)
                memcpy(shmem + TEX_OFF + y * 40 + x * 3, rowc[y], 3);
        e.off = e.start = CMD_OFF;
        tcreate3(&e, T + 91, QGPU_TT_2D);
        timage3(&e, T + 91, QGPU_TT_2D, 0, 13, 5, 1, 0x1907, 0x1907, 0x1401, TEX_OFF, 40, 0);
        tparam(&e, T + 91, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, T + 91, QGPU_TP_MAG_FILTER, 0x2600);
        tparam(&e, T + 91, QGPU_TP_WRAP_S, 0x812F);
        tparam(&e, T + 91, QGPU_TP_WRAP_T, 0x812F);
        v.off = v.start = VTX_OFF;
        quad_str(&v, 10, 20, 23, 25, 0, 0, 1, 1, 0, 0xFFFFFF);
        st = v10_draw(c, &e, T + 91, 6);
        for (y = 0; y < 5; y++)
            for (x = 0; x < 13; x++) {
                uint32_t want = ((uint32_t)rowc[y][0] << 16) | ((uint32_t)rowc[y][1] << 8) | rowc[y][2];
                uint32_t got = px(shmem, 10 + x, 20 + y);
                if (got != want) { if (!bad) first = (y << 16) | (x << 8) | 0; bad++; if (!((bad - 1) & 0xFFFF)) first = got | (y << 24); }
            }
        CHECK(st == QGPU_ST_OK && bad == 0,
              "(o) DrawPixels RGB 13×5, 40 octets/ligne : %u pixel(s) faux (premier %08x, st %u pc %u)",
              bad, first, st, c->status_pc);
    }

    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_TEXTURE, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_DESTROY, QGPU_LEN_TEX)); emit(&e, T + 91);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_DESTROY, QGPU_LEN_CTX)); emit(&e, V10_CTX);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_DESTROY, QGPU_LEN_SURF)); emit(&e, V10_SURF);
    st = v10_exec(c, &e);
    CHECK(st == QGPU_ST_OK, "v10 : surface et contexte rendus (st %u)", st);

}

/* ══════ v11 : couleur secondaire sur le chemin hérité ══════ */

static void vtx_sec(Emit *v, float x, float y, float f, float pr, float sr, float sg,
                    float sb, int nunits)
{
    emitf(v, x); emitf(v, y); emitf(v, 0.0f); emitf(v, f);
    emitf(v, pr); emitf(v, pr); emitf(v, pr); emitf(v, 1.0f);
    if (nunits) {
        emitf(v, 0.5f); emitf(v, 0.5f); emitf(v, 0.0f); emitf(v, 1.0f);
    }
    emitf(v, sr); emitf(v, sg); emitf(v, sb);
}

static void quad_sec(Emit *v, float f, float pr, float sr, float sg, float sb, int nunits)
{
    vtx_sec(v, 0, 0, f, pr, sr, sg, sb, nunits); vtx_sec(v, 64, 0, f, pr, sr, sg, sb, nunits);
    vtx_sec(v, 64, 64, f, pr, sr, sg, sb, nunits); vtx_sec(v, 0, 0, f, pr, sr, sg, sb, nunits);
    vtx_sec(v, 64, 64, f, pr, sr, sg, sb, nunits); vtx_sec(v, 0, 64, f, pr, sr, sg, sb, nunits);
}

static uint32_t sec_draw(QgpuCore *c, Emit *e, uint32_t op, uint32_t nunits)
{
    emit(e, QGPU_CMD_HDR(op, QGPU_LEN_DRAW_N)); emit(e, 6); emit(e, VTX_OFF); emit(e, nunits);
    readback_cmd(e, 21);
    return qgpu_core_execute(c, CMD_OFF, e->off - e->start);
}

static void run_v11(QgpuCore *c, uint8_t *shmem)
{
    Emit e, v;
    uint32_t st, s1, s2;
    static const uint32_t black = 0xFF000000;

    printf("-- v11 : couleur secondaire sur le chemin hérité --\n");
    CHECK(QGPU_VERTEX_SEC_WORDS(0) == 11 && QGPU_VERTEX_SEC_WORDS(4) == 27,
          "v11 : sommets de %d à %d mots", QGPU_VERTEX_SEC_WORDS(0), QGPU_VERTEX_SEC_WORDS(4));
    e.base = v.base = shmem;
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(&e, 13);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 13);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 21); emit(&e, W); emit(&e, H); emit(&e, QGPU_FMT_XRGB8888);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 21);
    qgpu_st32(shmem + TEX_OFF, black);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX)); emit(&e, 380);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
    emit(&e, 380); emit(&e, 0); emit(&e, 1); emit(&e, 1); emit(&e, 0x1908); emit(&e, TEX_OFF);
    tparam(&e, 380, QGPU_TP_MIN_FILTER, 0x2600);
    tparam(&e, 380, QGPU_TP_MAG_FILTER, 0x2600);
    state(&e, QGPU_SK_TEXTURE, 1);
    state(&e, QGPU_SK_TEX_BIND, 380);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x2100);            /* MODULATE */
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v11 : contexte, surface, texture noire en MODULATE (st %u)", st);

    /* (a) spéculaire séparée : primaire × texture noire = 0, + secondaire */
    v.off = v.start = VTX_OFF;
    quad_sec(&v, 1, 0.2f, 0.4f, 0.6f, 1.0f, 1);
    e.off = e.start = CMD_OFF;
    st = sec_draw(c, &e, QGPU_OP_DRAW_TRIANGLES_SEC, 1);
    CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0x6699ff,
          "(a) texture noire, secondaire ajoutée APRÈS elle : %06x (attendu 6699ff, st %u)",
          px(shmem, 32, 32), st);

    /* (b) le même sommet par TEXN : pas de somme */
    v.off = v.start = VTX_OFF;
    {
        int k;
        for (k = 0; k < 6; k++) {
            float x = (k == 1 || k == 2 || k == 4) ? 64.0f : 0.0f;
            float y = (k == 2 || k == 4 || k == 5) ? 64.0f : 0.0f;
            emitf(&v, x); emitf(&v, y); emitf(&v, 0.0f); emitf(&v, 1.0f);
            emitf(&v, 0.2f); emitf(&v, 0.2f); emitf(&v, 0.2f); emitf(&v, 1.0f);
            emitf(&v, 0.5f); emitf(&v, 0.5f); emitf(&v, 0.0f); emitf(&v, 1.0f);
        }
    }
    e.off = e.start = CMD_OFF;
    st = sec_draw(c, &e, QGPU_OP_DRAW_TRIANGLES_TEXN, 1);
    CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0x000000,
          "(b) DRAW_TRIANGLES_TEXN : aucune somme : %06x (st %u)", px(shmem, 32, 32), st);

    /* (c) sans texture : 0,2 + 0,4 = 0,6 ; saturation 0,8 + 0,8 */
    v.off = v.start = VTX_OFF;
    quad_sec(&v, 1, 0.2f, 0.4f, 0.4f, 0.4f, 0);
    e.off = e.start = CMD_OFF;
    st = sec_draw(c, &e, QGPU_OP_DRAW_TRIANGLES_SEC, 0);
    CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0x999999,
          "(c) sans texture : 0,2 + 0,4 → %06x (attendu 999999, st %u)", px(shmem, 32, 32), st);
    v.off = v.start = VTX_OFF;
    quad_sec(&v, 1, 0.8f, 0.8f, 0.8f, 0.8f, 0);
    e.off = e.start = CMD_OFF;
    st = sec_draw(c, &e, QGPU_OP_DRAW_TRIANGLES_SEC, 0);
    CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0xffffff,
          "(c) somme saturée : %06x (st %u)", px(shmem, 32, 32), st);

    /* (d) le brouillard vient APRÈS la somme : facteur 0 → couleur du brouillard */
    v.off = v.start = VTX_OFF;
    quad_sec(&v, 0, 0.2f, 0.4f, 0.6f, 1.0f, 1);
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_FOG, 1);
    state(&e, QGPU_SK_FOG_COLOR, 0xFFFF0000);
    st = sec_draw(c, &e, QGPU_OP_DRAW_TRIANGLES_SEC, 1);
    CHECK(st == QGPU_ST_OK && px(shmem, 32, 32) == 0xff0000,
          "(d) brouillard après la somme : %06x (attendu ff0000, st %u)", px(shmem, 32, 32), st);

    /* (e) refus : 5 unités, longueur fausse */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_SEC, QGPU_LEN_DRAW_N));
    emit(&e, 6); emit(&e, VTX_OFF); emit(&e, 5);
    s1 = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_SEC, QGPU_LEN_DRAW)); emit(&e, 6); emit(&e, VTX_OFF);
    s2 = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(s1 == QGPU_ST_BAD_ARG && s2 == QGPU_ST_BAD_ARG,
          "(e) 5 unités, longueur fausse : st %u %u", s1, s2);

    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_FOG, 0);
    state(&e, QGPU_SK_TEXTURE, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_DESTROY, QGPU_LEN_CTX)); emit(&e, 13);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_DESTROY, QGPU_LEN_SURF)); emit(&e, 21);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v11 : contexte et surface rendus (st %u)", st);
}

/* ══════ v12 : sources croisées de GL_COMBINE (crossbar, OpenGL 1.4) ══════ */

/* Deux unités par DRAW_TRIANGLES_TEXN : sommet x y z brouillard, couleur, puis
   (s t r q) par unité. Unité 0 rouge, unité 1 (0,6 ; 1 ; 1). */
static uint32_t xbar_draw(QgpuCore *c, Emit *e, Emit *v, float pr, float pg, float pb)
{
    int k;
    v->off = v->start = VTX_OFF;
    for (k = 0; k < 6; k++) {
        float x = (k == 1 || k == 2 || k == 4) ? 64.0f : 0.0f;
        float y = (k == 2 || k == 4 || k == 5) ? 64.0f : 0.0f;
        emitf(v, x); emitf(v, y); emitf(v, 0.0f); emitf(v, 1.0f);
        emitf(v, pr); emitf(v, pg); emitf(v, pb); emitf(v, 1.0f);
        emitf(v, 0.5f); emitf(v, 0.5f); emitf(v, 0.0f); emitf(v, 1.0f);
        emitf(v, 0.5f); emitf(v, 0.5f); emitf(v, 0.0f); emitf(v, 1.0f);
    }
    clear_cmd(e, QGPU_CLEAR_COLOR, 0xFF000000, 1.0f);
    emit(e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEXN, QGPU_LEN_DRAW_N));
    emit(e, 6); emit(e, VTX_OFF); emit(e, 2);
    readback_cmd(e, 22);
    return qgpu_core_execute(c, CMD_OFF, e->off - e->start);
}

static void run_v12(QgpuCore *c, uint8_t *shmem)
{
    Emit e, v;
    uint32_t st, p;
    static const struct {
        uint32_t env0, cb0, src0, env1, src1; float pr, pg, pb; uint32_t want; const char *what;
    } xc[] = {
        /* unité 0 : REPLACE la texture de l'unité 1 ; unité 1 : REPLACE PREVIOUS */
        { 0x8570, QGPU_COMBINE(QGPU_CB_REPLACE, QGPU_CB_REPLACE, 0, 0),
          QGPU_COMBINE_SRC_RGB(0, QGPU_CS_TEXTURE0 + 1, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_A(0, QGPU_CS_TEXTURE0 + 1, QGPU_CA_ALPHA),
          0x8570, QGPU_COMBINE_SRC_RGB(0, QGPU_CS_PREVIOUS, QGPU_CO_COLOR) |
                  QGPU_COMBINE_SRC_A(0, QGPU_CS_PREVIOUS, QGPU_CA_ALPHA),
          1, 1, 1, 0x99FFFF, "unité 0 ← texture de l'unité 1" },
        /* unité 0 : MODULATE(TEXTURE0, TEXTURE1) = (0,6 0 0) */
        { 0x8570, QGPU_COMBINE(QGPU_CB_MODULATE, QGPU_CB_MODULATE, 0, 0),
          QGPU_COMBINE_SRC_RGB(0, QGPU_CS_TEXTURE0, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_RGB(1, QGPU_CS_TEXTURE0 + 1, QGPU_CO_COLOR) |
          QGPU_COMBINE_SRC_A(0, QGPU_CS_TEXTURE0, QGPU_CA_ALPHA) |
          QGPU_COMBINE_SRC_A(1, QGPU_CS_TEXTURE0 + 1, QGPU_CA_ALPHA),
          0x8570, QGPU_COMBINE_SRC_RGB(0, QGPU_CS_PREVIOUS, QGPU_CO_COLOR) |
                  QGPU_COMBINE_SRC_A(0, QGPU_CS_PREVIOUS, QGPU_CA_ALPHA),
          1, 1, 1, 0x990000, "MODULATE(TEXTURE0, TEXTURE1)" },
        /* unité 0 : MODULATE ordinaire avec le vert primaire → noir ; unité 1
           REPLACE la texture de l'unité 0 (et non PREVIOUS) → rouge */
        { 0x2100, QGPU_COMBINE_DEFAULT, QGPU_COMBINE_SRC_DEFAULT,
          0x8570, QGPU_COMBINE_SRC_RGB(0, QGPU_CS_TEXTURE0, QGPU_CO_COLOR) |
                  QGPU_COMBINE_SRC_A(0, QGPU_CS_TEXTURE0, QGPU_CA_ALPHA),
          0, 1, 0, 0xFF0000, "unité 1 ← texture de l'unité 0 (en arrière)" },
    };
    unsigned i;

    printf("-- v12 : sources croisées de GL_COMBINE --\n");
    e.base = v.base = shmem;
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(&e, 14);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 14);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 22); emit(&e, W); emit(&e, H); emit(&e, QGPU_FMT_XRGB8888);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 22);
    qgpu_st32(shmem + TEX_OFF, 0xFFFF0000);
    qgpu_st32(shmem + TEX_OFF + 4, 0xFF99FFFF);
    tex1x1(&e, 390, 0x1908, TEX_OFF);
    tex1x1(&e, 391, 0x1908, TEX_OFF + 4);
    state(&e, QGPU_SK_TEXTURE, 1); state(&e, QGPU_SK_TEX_BIND, 390);
    state(&e, QGPU_SK_TEXTURE1, 1); state(&e, QGPU_SK_TEX1_BIND, 391);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v12 : contexte, deux unités (st %u)", st);
    for (i = 0; i < sizeof(xc) / sizeof(xc[0]); i++) {
        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_TEX_ENV_MODE, xc[i].env0);
        state(&e, QGPU_SK_COMBINE0, xc[i].cb0);
        state(&e, QGPU_SK_COMBINE_SRC0, xc[i].src0);
        state(&e, QGPU_SK_TEX1_ENV_MODE, xc[i].env1);
        state(&e, QGPU_SK_COMBINE0 + 1, QGPU_COMBINE(QGPU_CB_REPLACE, QGPU_CB_REPLACE, 0, 0));
        state(&e, QGPU_SK_COMBINE_SRC0 + 1, xc[i].src1);
        st = xbar_draw(c, &e, &v, xc[i].pr, xc[i].pg, xc[i].pb);
        p = px(shmem, 32, 32);
        CHECK(st == QGPU_ST_OK && near_argb(p, xc[i].want, 1),
              "(%c) %s : %06x (attendu %06x, st %u)", 'a' + i, xc[i].what, p, xc[i].want, st);
    }
    e.off = e.start = CMD_OFF;
    state(&e, QGPU_SK_COMBINE0, QGPU_COMBINE_DEFAULT);
    state(&e, QGPU_SK_COMBINE_SRC0, QGPU_COMBINE_SRC_DEFAULT);
    state(&e, QGPU_SK_COMBINE0 + 1, QGPU_COMBINE_DEFAULT);
    state(&e, QGPU_SK_COMBINE_SRC0 + 1, QGPU_COMBINE_SRC_DEFAULT);
    state(&e, QGPU_SK_TEX_ENV_MODE, 0x2100); state(&e, QGPU_SK_TEX1_ENV_MODE, 0x2100);
    state(&e, QGPU_SK_TEXTURE, 0); state(&e, QGPU_SK_TEXTURE1, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_DESTROY, QGPU_LEN_CTX)); emit(&e, 14);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_DESTROY, QGPU_LEN_SURF)); emit(&e, 22);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v12 : contexte et surface rendus (st %u)", st);
}

static uint32_t g_present_dirty_n, g_present_dirty_off, g_present_dirty_len;

static void test_present_dirty(void *opaque, uint32_t off, uint32_t len)
{
    (void)opaque;
    g_present_dirty_n++;
    g_present_dirty_off = off;
    g_present_dirty_len = len;
}

static void run_v13(QgpuCore *c, uint8_t *shmem)
{
    enum { FB_W = 256u, FB_PITCH32 = FB_W * 4u, FB_PITCH16 = FB_W * 2u };
    uint8_t *fb = calloc(1, (size_t)FB_W * FB_W * 4);
    Emit e;
    uint32_t st, p, want;
    uint16_t pix;

    printf("-- v13 : SURF_PRESENT --\n");
    CHECK(fb != NULL, "tampon de scanout alloué");
    CHECK(QGPU_OP_SURF_PRESENT == 0x0019 && QGPU_LEN_SURF_PRESENT == 9,
          "opcode 0x%x longueur %d", QGPU_OP_SURF_PRESENT, QGPU_LEN_SURF_PRESENT);
    CHECK(QGPU_OP_COPY_TEX == 0x001A && QGPU_LEN_COPY_TEX == 11,
          "COPY_TEX opcode 0x%x longueur %d", QGPU_OP_COPY_TEX, QGPU_LEN_COPY_TEX);
    CHECK((QGPU_CAP_SCANOUT & (QGPU_CAP_GL14 | QGPU_CAP_ASYNC)) == 0,
          "QGPU_CAP_SCANOUT=0x%x disjoint", QGPU_CAP_SCANOUT);

    qgpu_core_reset(c);
    e.base = shmem; e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 1); emit(&e, W); emit(&e, H); emit(&e, QGPU_FMT_XRGB8888);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 1);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF0000, 1.0f);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v13 : surface rouge (st %u)", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_PRESENT, QGPU_LEN_SURF_PRESENT));
    emit(&e, 1); emit(&e, 0); emit(&e, FB_PITCH32);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_PF_XRGB8888);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "PRESENT sans scanout : st %u", st);

    qgpu_core_set_scanout(c, fb, FB_W * FB_W * 4, test_present_dirty, NULL);
    g_present_dirty_n = 0;
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_PRESENT, QGPU_LEN_SURF_PRESENT));
    emit(&e, 1); emit(&e, 0); emit(&e, FB_PITCH32);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_PF_XRGB8888);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = qgpu_ld32(fb + 10 * 4 + 20 * FB_PITCH32) & 0xFFFFFF;
    CHECK(st == QGPU_ST_OK && p == 0xFF0000,
          "PRESENT xRGB : st %u pixel %06x", st, p);
    CHECK(g_present_dirty_n == 1 && g_present_dirty_off == 0,
          "PRESENT marque sale : n=%u off=%u len=%u",
          g_present_dirty_n, g_present_dirty_off, g_present_dirty_len);

    memset(fb, 0, (size_t)FB_W * FB_W * 4);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_PRESENT, QGPU_LEN_SURF_PRESENT));
    emit(&e, 1); emit(&e, 0); emit(&e, FB_PITCH16);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_PF_RGB1555);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    pix = (uint16_t)(((uint16_t)fb[20 * FB_PITCH16 + 10 * 2] << 8) |
                     fb[20 * FB_PITCH16 + 10 * 2 + 1]);
    want = 0x7C00;
    CHECK(st == QGPU_ST_OK && pix == want,
          "PRESENT 1555 : st %u pixel %04x (attendu %04x)", st, pix, want);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_PRESENT, QGPU_LEN_SURF_PRESENT));
    emit(&e, 1); emit(&e, FB_W * FB_W * 4 - 4); emit(&e, FB_PITCH32);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_PF_XRGB8888);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OOB, "PRESENT hors VRAM : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_PRESENT, QGPU_LEN_SURF_PRESENT));
    emit(&e, 1); emit(&e, 0); emit(&e, FB_PITCH32);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, 99);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "PRESENT format inconnu : st %u", st);

    /* v16 (Q3) : quand le device a publié la géométrie de l'écran, un pas ou
       une profondeur qui ne sont pas les siens sont refusés (c'est le symptôme
       de Q1 : image écrite au pas d'un autre écran, statut OK). Sans
       géométrie (stride 0), rien n'est comparé — le cas des tests ci-dessus. */
    qgpu_core_set_scanout_geom(c, FB_PITCH32, FB_W, FB_W, 32);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_PRESENT, QGPU_LEN_SURF_PRESENT));
    emit(&e, 1); emit(&e, 0); emit(&e, FB_PITCH32 + 4);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_PF_XRGB8888);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "PRESENT au pas d'un autre écran : st %u", st);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_PRESENT, QGPU_LEN_SURF_PRESENT));
    emit(&e, 1); emit(&e, 0); emit(&e, FB_PITCH32);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_PF_RGB1555);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "PRESENT 1555 sur un écran 32 bits : st %u", st);
    memset(fb, 0, (size_t)FB_W * FB_W * 4);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_PRESENT, QGPU_LEN_SURF_PRESENT));
    emit(&e, 1); emit(&e, 0); emit(&e, FB_PITCH32);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_PF_XRGB8888);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    p = qgpu_ld32(fb + 10 * 4 + 20 * FB_PITCH32) & 0xFFFFFF;
    CHECK(st == QGPU_ST_OK && p == 0xFF0000,
          "PRESENT au bon pas avec géométrie : st %u pixel %06x", st, p);
    qgpu_core_set_scanout_geom(c, 0, 0, 0, 0);

    /* COPY_TEX : surface rouge → texture noire, puis REPLACE. */
    {
        uint32_t i;
        e.off = e.start = TEX_OFF;
        for (i = 0; i < 64; i++)
            emit(&e, 0);
        e.off = e.start = CMD_OFF;
        emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX)); emit(&e, 40);
        emit(&e, QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE));
        emit(&e, 40); emit(&e, 0); emit(&e, 8); emit(&e, 8); emit(&e, 0x1908);
        emit(&e, TEX_OFF);
        tparam(&e, 40, QGPU_TP_MIN_FILTER, 0x2600);
        tparam(&e, 40, QGPU_TP_MAG_FILTER, 0x2600);
        emit(&e, QGPU_CMD_HDR(QGPU_OP_COPY_TEX, QGPU_LEN_COPY_TEX));
        emit(&e, 40); emit(&e, QGPU_TT_2D); emit(&e, 0);
        emit(&e, 0); emit(&e, 0); emit(&e, 0);
        emit(&e, 0); emit(&e, 0); emit(&e, 4); emit(&e, 4);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        CHECK(st == QGPU_ST_OK, "COPY_TEX 4×4 : st %u", st);

        e.off = e.start = VTX_OFF;
        tex_quad(&e, 1, 1, 1, 1, 1, 1);
        e.off = e.start = CMD_OFF;
        state(&e, QGPU_SK_TEXTURE, 1);
        state(&e, QGPU_SK_TEX_BIND, 40);
        state(&e, QGPU_SK_TEX_ENV_MODE, 0x1E01);
        emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_LEN_DRAW));
        emit(&e, 6); emit(&e, VTX_OFF);
        emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
        emit(&e, 1); emit(&e, RB_OFF); emit(&e, STRIDE);
        emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        CHECK(st == QGPU_ST_OK && (px(shmem, 4, 4) & 0xFFFFFF) == 0xFF0000,
              "COPY_TEX texe (0,0) rouge : st %u pixel %06x", st, px(shmem, 4, 4));
        CHECK((px(shmem, 50, 50) & 0xFFFFFF) == 0,
              "COPY_TEX reste noir : %06x", px(shmem, 50, 50));

        e.off = e.start = CMD_OFF;
        emit(&e, QGPU_CMD_HDR(QGPU_OP_COPY_TEX, QGPU_LEN_COPY_TEX));
        emit(&e, 40); emit(&e, QGPU_TT_2D); emit(&e, 0);
        emit(&e, 0); emit(&e, 0); emit(&e, 0);
        emit(&e, 0); emit(&e, 0); emit(&e, 0); emit(&e, 4);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        CHECK(st == QGPU_ST_OK, "COPY_TEX w=0 : st %u", st);

        e.off = e.start = CMD_OFF;
        emit(&e, QGPU_CMD_HDR(QGPU_OP_COPY_TEX, QGPU_LEN_COPY_TEX));
        emit(&e, 40); emit(&e, QGPU_TT_2D); emit(&e, 0);
        emit(&e, 6); emit(&e, 0); emit(&e, 0);
        emit(&e, 0); emit(&e, 0); emit(&e, 4); emit(&e, 4);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        CHECK(st == QGPU_ST_BAD_ARG, "COPY_TEX hors niveau : st %u", st);

        e.off = e.start = CMD_OFF;
        emit(&e, QGPU_CMD_HDR(QGPU_OP_COPY_TEX, QGPU_LEN_COPY_TEX));
        emit(&e, 99); emit(&e, QGPU_TT_2D); emit(&e, 0);
        emit(&e, 0); emit(&e, 0); emit(&e, 0);
        emit(&e, 0); emit(&e, 0); emit(&e, 4); emit(&e, 4);
        st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
        CHECK(st == QGPU_ST_BAD_ARG, "COPY_TEX tex inconnue : st %u", st);
    }

    qgpu_core_reset(c);
    CHECK(c->scanout == fb, "reset conserve la cible de scanout");
    qgpu_core_set_scanout(c, NULL, 0, NULL, NULL);
    free(fb);
}

static void draw_raw_buf(Emit *e, uint32_t mode, uint32_t count, uint32_t vbuf,
                         uint32_t voff, uint32_t fmt, uint32_t ibuf, uint32_t ioff,
                         uint32_t itype, uint32_t nverts)
{
    emit(e, QGPU_CMD_HDR(QGPU_OP_DRAW_RAW_BUF, QGPU_LEN_DRAW_RAW_BUF));
    emit(e, mode); emit(e, count); emit(e, vbuf); emit(e, voff); emit(e, 0);
    emit(e, fmt); emit(e, ibuf); emit(e, ioff); emit(e, itype); emit(e, 0);
    emit(e, nverts);
}

static void run_v14(QgpuCore *c, uint8_t *shmem)
{
    Emit e, v;
    float m[16], mv[16];
    uint32_t st, packed, i;

    printf("-- v14 : tampons hôte --\n");
    CHECK(QGPU_OP_BUF_CREATE == 0x001B && QGPU_LEN_BUF_CREATE == 3,
          "BUF_CREATE opcode 0x%x longueur %d", QGPU_OP_BUF_CREATE, QGPU_LEN_BUF_CREATE);
    CHECK(QGPU_OP_BUF_DESTROY == 0x001C && QGPU_LEN_BUF == 2,
          "BUF_DESTROY opcode 0x%x longueur %d", QGPU_OP_BUF_DESTROY, QGPU_LEN_BUF);
    CHECK(QGPU_OP_BUF_SUBDATA == 0x001D && QGPU_LEN_BUF_SUBDATA == 5,
          "BUF_SUBDATA opcode 0x%x longueur %d", QGPU_OP_BUF_SUBDATA, QGPU_LEN_BUF_SUBDATA);
    CHECK(QGPU_OP_DRAW_RAW_BUF == 0x0059 && QGPU_LEN_DRAW_RAW_BUF == 12,
          "DRAW_RAW_BUF opcode 0x%x longueur %d", QGPU_OP_DRAW_RAW_BUF,
          QGPU_LEN_DRAW_RAW_BUF);
    CHECK(QGPU_BUF_SHMEM == 0xFFFFFFFFu, "sentinelle BAR0 0x%x", QGPU_BUF_SHMEM);

    qgpu_core_reset(c);
    e.base = shmem; v.base = shmem;
    mat_ortho_px(m);
    mat_identity(mv);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 1); emit(&e, W); emit(&e, H); emit(&e, QGPU_FMT_XRGB8888);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 1);
    set_matrix(&e, QGPU_MTX_PROJECTION, m);
    set_matrix(&e, QGPU_MTX_MODELVIEW, mv);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v14 : contexte (st %u)", st);

    v.off = v.start = VTX_OFF;
    rv2c(&v, 4, 4, 1, 0, 0, 1); rv2c(&v, 60, 4, 1, 0, 0, 1); rv2c(&v, 4, 20, 1, 0, 0, 1);
    packed = v.off - v.start;

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_BUF_CREATE, QGPU_LEN_BUF_CREATE));
    emit(&e, 10); emit(&e, packed);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_BUF_SUBDATA, QGPU_LEN_BUF_SUBDATA));
    emit(&e, 10); emit(&e, 0); emit(&e, VTX_OFF); emit(&e, packed);
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF0000FF, 1.0f);
    draw_raw_buf(&e, QGPU_PRIM_MODE_TRIANGLES, 3, 10, 0, VF_P2C,
                 QGPU_BUF_SHMEM, 0, QGPU_IDX_NONE, 3);
    readback_cmd(&e, 1);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0xFF0000 &&
          px(shmem, 8, 30) == 0x0000FF,
          "DRAW_RAW_BUF = DRAW_RAW : %06x %06x (st %u)",
          px(shmem, 30, 8), px(shmem, 8, 30), st);

    /* Réutiliser le tampon : plus aucun sommet dans BAR0. */
    memset(shmem + VTX_OFF, 0, packed);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF0000FF, 1.0f);
    draw_raw_buf(&e, QGPU_PRIM_MODE_TRIANGLES, 3, 10, 0, VF_P2C,
                 QGPU_BUF_SHMEM, 0, QGPU_IDX_NONE, 3);
    readback_cmd(&e, 1);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0xFF0000,
          "DRAW_RAW_BUF sans BAR0 : %06x (st %u)", px(shmem, 30, 8), st);

    /* Indices encore dans BAR0, sommets hôte. */
    shmem[IDX_OFF] = 0; shmem[IDX_OFF + 1] = 0;
    shmem[IDX_OFF + 2] = 0; shmem[IDX_OFF + 3] = 1;
    shmem[IDX_OFF + 4] = 0; shmem[IDX_OFF + 5] = 2;
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF0000FF, 1.0f);
    draw_raw_buf(&e, QGPU_PRIM_MODE_TRIANGLES, 3, 10, 0, VF_P2C,
                 QGPU_BUF_SHMEM, IDX_OFF, QGPU_IDX_U16, 3);
    readback_cmd(&e, 1);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0xFF0000,
          "DRAW_RAW_BUF indices BAR0 : %06x (st %u)", px(shmem, 30, 8), st);

    /* vbuf = SHMEM : même layout DRAW_RAW_BUF, offset dans BAR0. */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 4, 4, 1, 0, 0, 1); rv2c(&v, 60, 4, 1, 0, 0, 1); rv2c(&v, 4, 20, 1, 0, 0, 1);
    e.off = e.start = CMD_OFF;
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF0000FF, 1.0f);
    draw_raw_buf(&e, QGPU_PRIM_MODE_TRIANGLES, 3, QGPU_BUF_SHMEM, VTX_OFF, VF_P2C,
                 QGPU_BUF_SHMEM, 0, QGPU_IDX_NONE, 3);
    readback_cmd(&e, 1);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0xFF0000,
          "DRAW_RAW_BUF via BAR0 : %06x (st %u)", px(shmem, 30, 8), st);

    /* H4 par le chemin VBO : les sommets vivent sur l'HÔTE, le raw_fix_nan du
       plugin ne peut pas les atteindre — c'est l'exposition réelle que la
       contre-expertise a retenue. Un quatrième sommet entièrement NaN, jamais
       cité par les indices, ne doit plus rien coûter. */
    v.off = v.start = VTX_OFF;
    rv2c(&v, 4, 4, 1, 0, 0, 1); rv2c(&v, 60, 4, 1, 0, 0, 1); rv2c(&v, 4, 20, 1, 0, 0, 1);
    for (i = 0; i < 6; i++) {
        emit(&v, 0x7FC00000u);
    }
    for (i = 0; i < 3; i++) {
        qgpu_st16(shmem + IDX_OFF + i * 2, (uint16_t)i);
    }
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_BUF_CREATE, QGPU_LEN_BUF_CREATE));
    emit(&e, 12); emit(&e, packed + 24);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_BUF_SUBDATA, QGPU_LEN_BUF_SUBDATA));
    emit(&e, 12); emit(&e, 0); emit(&e, VTX_OFF); emit(&e, packed + 24);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF0000FF, 1.0f);
    draw_raw_buf(&e, QGPU_PRIM_MODE_TRIANGLES, 3, 12, 0, VF_P2C,
                 QGPU_BUF_SHMEM, IDX_OFF, QGPU_IDX_U16, 4);
    readback_cmd(&e, 1);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 8) == 0xFF0000 && px(shmem, 8, 30) == 0x0000FF,
          "H4/VBO : queue de sommets NaN jamais citée : %06x %06x (st %u)",
          px(shmem, 30, 8), px(shmem, 8, 30), st);

    /* Le NaN est maintenant dans un sommet CITÉ du tampon hôte : assaini, et
       le CLEAR puis le READBACK qui suivent s'exécutent. */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_BUF_SUBDATA, QGPU_LEN_BUF_SUBDATA));
    emit(&e, 12); emit(&e, 4); emit(&e, VTX_OFF + 18 * 4); emit(&e, 4);
    draw_raw_buf(&e, QGPU_PRIM_MODE_TRIANGLES, 3, 12, 0, VF_P2C,
                 QGPU_BUF_SHMEM, IDX_OFF, QGPU_IDX_U16, 4);
    clear_cmd(&e, QGPU_CLEAR_COLOR, 0xFF204060, 1.0f);
    readback_cmd(&e, 1);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 30, 30) == 0x204060 && px(shmem, 2, 2) == 0x204060,
          "H4/VBO : NaN dans un sommet cité, le flux continue : %06x %06x (st %u)",
          px(shmem, 30, 30), px(shmem, 2, 2), st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_BUF_DESTROY, QGPU_LEN_BUF)); emit(&e, 12);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "H4/VBO : tampon libéré (st %u)", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_BUF_CREATE, QGPU_LEN_BUF_CREATE));
    emit(&e, 10); emit(&e, packed);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_LIMIT, "recréer un id : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_BUF_CREATE, QGPU_LEN_BUF_CREATE));
    emit(&e, 11); emit(&e, 0);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "size 0 : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_BUF_SUBDATA, QGPU_LEN_BUF_SUBDATA));
    emit(&e, 10); emit(&e, packed - 4); emit(&e, VTX_OFF); emit(&e, 8);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OOB, "SUBDATA débordant : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_BUF_SUBDATA, QGPU_LEN_BUF_SUBDATA));
    emit(&e, 10); emit(&e, 0); emit(&e, VTX_OFF); emit(&e, 0);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "SUBDATA len=0 : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_BUF_DESTROY, QGPU_LEN_BUF)); emit(&e, 10);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "BUF_DESTROY : st %u", st);

    e.off = e.start = CMD_OFF;
    draw_raw_buf(&e, QGPU_PRIM_MODE_TRIANGLES, 3, 10, 0, VF_P2C,
                 QGPU_BUF_SHMEM, 0, QGPU_IDX_NONE, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "DRAW après DESTROY : st %u", st);

    qgpu_core_reset(c);
    CHECK(!c->buf[10].used && c->buf[10].data == NULL,
          "reset libère les tampons");
}

static void run_v15(QgpuCore *c, uint8_t *shmem)
{
    enum { PITCH16 = W * 2u };
    Emit e;
    uint32_t st, i, p;
    uint16_t pix;
    float d;

    printf("-- v15 : SURF/DEPTH xfer 16 bits --\n");
    CHECK(QGPU_LEN_SURF_XFER_PF == 9, "LEN_SURF_XFER_PF %d", QGPU_LEN_SURF_XFER_PF);
    CHECK(QGPU_DF_UNORM16 == 1 && QGPU_PF_RGB1555 == 1,
          "formats 16 bits couleur=%d profondeur=%d",
          QGPU_PF_RGB1555, QGPU_DF_UNORM16);

    qgpu_core_reset(c);
    e.base = shmem; e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 0);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(&e, 1); emit(&e, W); emit(&e, H);
    emit(&e, QGPU_FMT_XRGB8888 | QGPU_FMT_FLAG_DEPTH);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(&e, 1);
    clear_cmd(&e, QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH, 0xFF0000, 1.0f);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK, "v15 : surface rouge (st %u)", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, STRIDE);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 20) == 0xFF0000,
          "READBACK LEN 8 encore 8888 : %06x (st %u)", px(shmem, 10, 20), st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER_PF));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, PITCH16);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_PF_RGB1555);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    pix = qgpu_ld16(shmem + RB_OFF + 20 * PITCH16 + 10 * 2);
    CHECK(st == QGPU_ST_OK && pix == 0x7C00,
          "READBACK 1555 : st %u pixel %04x", st, pix);

    for (i = 0; i < W * H; i++) {
        qgpu_st16(shmem + RB_OFF + i * 2, 0x03E0);
    }
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_UPLOAD, QGPU_LEN_SURF_XFER_PF));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, PITCH16);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_PF_RGB1555);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 1); emit(&e, RB_OFF + 0x8000); emit(&e, STRIDE);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    /* H3 : le bit 15 de RGB1555 n'est pas un alpha (pack_rgb1555 le jette) et
       la surface est en XRGB8888 : un transfert en milliers de couleurs doit
       arriver OPAQUE. Le test v15 masquait l'alpha, ce qui cachait une
       surface entièrement transparente — DST_ALPHA inversé, COPY_TEX
       invisible. On lit donc le mot ENTIER. */
    p = qgpu_ld32(shmem + RB_OFF + 0x8000 + 8 * STRIDE + 8 * 4);
    CHECK(st == QGPU_ST_OK && p == 0xFF00FF00u,
          "UPLOAD 1555 → READBACK 8888, alpha compris : %08x (st %u)", p, st);

    for (i = 0; i < W * H; i++) {
        qgpu_st16(shmem + RB_OFF + i * 2, 0x8000);
    }
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_UPLOAD, QGPU_LEN_SURF_XFER_PF));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, PITCH16);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_DF_UNORM16);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 1); emit(&e, RB_OFF + 0x8000); emit(&e, STRIDE);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    d = qgpu_u2f(qgpu_ld32(shmem + RB_OFF + 0x8000 + 8 * STRIDE + 8 * 4));
    CHECK(st == QGPU_ST_OK && d > 0.49f && d < 0.51f,
          "UPLOAD UNORM16 → float : %g (st %u)", d, st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DEPTH_READBACK, QGPU_LEN_SURF_XFER_PF));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, PITCH16);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_DF_UNORM16);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    pix = qgpu_ld16(shmem + RB_OFF + 8 * PITCH16 + 8 * 2);
    CHECK(st == QGPU_ST_OK && pix == 0x8000,
          "READBACK UNORM16 : %04x (st %u)", pix, st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER_PF));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, STRIDE);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, 99);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "format couleur inconnu : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER_PF));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, 1);
    emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H); emit(&e, QGPU_PF_RGB1555);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "stride 1555 trop petit : st %u", st);

    qgpu_core_reset(c);
}

typedef struct { QgpuCore *c; uint8_t *shmem; } BackendRun;

/* Tout ce qui suit l'initialisation, sur le thread « de rendu ». */
static void *run_backend_body(void *arg)
{
    BackendRun *r = arg;
    QgpuCore *c = r->c;
    uint8_t *shmem = r->shmem;
    uint32_t len, st;
    Emit e;

    len = build_scene(shmem);
    st = qgpu_core_execute(c, CMD_OFF, len);
    CHECK(st == QGPU_ST_OK, "scène : statut %u (pc %u)", st, c->status_pc);
    CHECK(px(shmem, 8, 8) == 0xFF0000, "intérieur du triangle rouge : %06x", px(shmem, 8, 8));
    CHECK(px(shmem, 58, 58) == 0x00FF00, "intérieur du triangle vert (sens horaire) : %06x", px(shmem, 58, 58));
    CHECK(px(shmem, 40, 30) == 0x0000FF, "fond bleu : %06x", px(shmem, 40, 30));
    CHECK(px(shmem, 2, 2) == 0x0000FF, "coin hors triangle : %06x", px(shmem, 2, 2));

    /* Gouraud : sommet rouge → vert, milieu ≈ moitié-moitié. */
    e.base = shmem; e.off = e.start = VTX_OFF;
    vertex(&e, 0, 0, 1, 0, 0); vertex(&e, 64, 0, 0, 1, 0); vertex(&e, 0, 64, 1, 0, 0);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW)); emit(&e, 3); emit(&e, VTX_OFF);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    {
        uint32_t p = px(shmem, 31, 1), r = p >> 16, g = (p >> 8) & 255;
        CHECK(st == QGPU_ST_OK && r > 100 && r < 156 && g > 100 && g < 156,
              "Gouraud au milieu de l'arête rouge→vert : %06x", p);
    }

    /* Upload puis readback : aller-retour exact. */
    e.off = e.start = VTX_OFF;
    emit(&e, 0x123456); emit(&e, 0xABCDEF);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_UPLOAD, QGPU_LEN_SURF_XFER));
    emit(&e, 1); emit(&e, VTX_OFF); emit(&e, 8); emit(&e, 10); emit(&e, 20); emit(&e, 2); emit(&e, 1);
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 1); emit(&e, RB_OFF); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 20) == 0x123456 && px(shmem, 11, 20) == 0xABCDEF,
          "upload/readback : %06x %06x", px(shmem, 10, 20), px(shmem, 11, 20));

    /* Erreurs : chacune doit être signalée avec le bon pc, sans casser la suite. */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_NOP, 1));
    emit(&e, QGPU_CMD_HDR(0x7777, 1));
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_OPCODE && c->status_pc == 1, "opcode inconnu : st %u pc %u", st, c->status_pc);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW)); emit(&e, 3); emit(&e, SHMEM_SIZE - 8);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OOB, "sommets hors fenêtre : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 1); emit(&e, SHMEM_SIZE - 64); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OOB, "readback débordant : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CLEAR, 9));
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_HEADER, "longueur dépassant le flux : st %u", st);

    st = qgpu_core_execute(c, SHMEM_SIZE - 4, 8);
    CHECK(st == QGPU_ST_BAD_SUBMIT, "soumission hors fenêtre : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 5);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "bind d'un contexte inexistant : st %u", st);

    run_v2(c, shmem);
    run_v3(c, shmem);
    run_v4(c, shmem);
    run_v5(c, shmem);
    run_v6(c, shmem);
    run_v7(c, shmem);
    run_v8(c, shmem);
    run_zs(c, shmem);
    run_v9(c, shmem);
    run_v10(c, shmem);
    run_v11(c, shmem);
    run_v12(c, shmem);
    run_v13(c, shmem);
    run_v14(c, shmem);
    run_v15(c, shmem);

    qgpu_core_reset(c);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR)); emit(&e, 1); emit(&e, 0); emit(&e, 0);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_NO_CTX, "après reset, plus de contexte : st %u", st);

    qgpu_core_fini(c);
    return NULL;
}

static void run_backend(const char *name)
{
    uint8_t *shmem = calloc(1, SHMEM_SIZE);
    QgpuCore c;
    BackendRun r = { &c, shmem };
    pthread_t th;

    printf("== backend %s\n", name);
    if (!qgpu_core_init(&c, name, shmem, SHMEM_SIZE)) {
        printf("  –    indisponible sur cet hôte (ignoré)\n");
        free(shmem);
        return;
    }
    CHECK(!strcmp(c.be->name, name), "backend actif : %s", c.be->name);
    if (pthread_create(&th, NULL, run_backend_body, &r) != 0) {
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
