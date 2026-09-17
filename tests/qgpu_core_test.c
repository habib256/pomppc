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
 *   cc -I patches/qgpu tests/qgpu_core_test.c patches/qgpu/qgpu-{core,soft,gl}.c \
 *      [-framework OpenGL | -lEGL -lGL] -o qgpu_core_test && ./qgpu_core_test
 */
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
#define VF_P2C  (VF_P2 | QGPU_VF_COLOR)
#define VF_P3C  (VF_P3 | QGPU_VF_COLOR)

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
    v.off = v.start = VTX_OFF;
    vertexz(&v, 0, 0, 0.0f, 1, 1, 1, 1); emitf(&v, 0.0f / 0.0f);
    e.off = e.start = CMD_OFF;
    draw_cmd(&e, 3);
    st = qgpu_core_execute(c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "sommet NaN refusé : st %u", st);
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

static void run_backend(const char *name)
{
    uint8_t *shmem = calloc(1, SHMEM_SIZE);
    QgpuCore c;
    uint32_t len, st;
    Emit e;

    printf("== backend %s\n", name);
    if (!qgpu_core_init(&c, name, shmem, SHMEM_SIZE)) {
        printf("  –    indisponible sur cet hôte (ignoré)\n");
        free(shmem);
        return;
    }
    CHECK(!strcmp(c.be->name, name), "backend actif : %s", c.be->name);

    len = build_scene(shmem);
    st = qgpu_core_execute(&c, CMD_OFF, len);
    CHECK(st == QGPU_ST_OK, "scène : statut %u (pc %u)", st, c.status_pc);
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
    st = qgpu_core_execute(&c, CMD_OFF, e.off - e.start);
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
    st = qgpu_core_execute(&c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OK && px(shmem, 10, 20) == 0x123456 && px(shmem, 11, 20) == 0xABCDEF,
          "upload/readback : %06x %06x", px(shmem, 10, 20), px(shmem, 11, 20));

    /* Erreurs : chacune doit être signalée avec le bon pc, sans casser la suite. */
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_NOP, 1));
    emit(&e, QGPU_CMD_HDR(0x7777, 1));
    st = qgpu_core_execute(&c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_OPCODE && c.status_pc == 1, "opcode inconnu : st %u pc %u", st, c.status_pc);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW)); emit(&e, 3); emit(&e, SHMEM_SIZE - 8);
    st = qgpu_core_execute(&c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OOB, "sommets hors fenêtre : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
    emit(&e, 1); emit(&e, SHMEM_SIZE - 64); emit(&e, STRIDE); emit(&e, 0); emit(&e, 0); emit(&e, W); emit(&e, H);
    st = qgpu_core_execute(&c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_OOB, "readback débordant : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CLEAR, 9));
    st = qgpu_core_execute(&c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_HEADER, "longueur dépassant le flux : st %u", st);

    st = qgpu_core_execute(&c, SHMEM_SIZE - 4, 8);
    CHECK(st == QGPU_ST_BAD_SUBMIT, "soumission hors fenêtre : st %u", st);

    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(&e, 5);
    st = qgpu_core_execute(&c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_BAD_ARG, "bind d'un contexte inexistant : st %u", st);

    run_v2(&c, shmem);
    run_v3(&c, shmem);
    run_v4(&c, shmem);
    run_v5(&c, shmem);
    run_v6(&c, shmem);
    run_v7(&c, shmem);
    run_v8(&c, shmem);

    qgpu_core_reset(&c);
    e.off = e.start = CMD_OFF;
    emit(&e, QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR)); emit(&e, 1); emit(&e, 0); emit(&e, 0);
    st = qgpu_core_execute(&c, CMD_OFF, e.off - e.start);
    CHECK(st == QGPU_ST_NO_CTX, "après reset, plus de contexte : st %u", st);

    qgpu_core_fini(&c);
    free(shmem);
}

int main(void)
{
    run_backend("soft");
    run_backend("gl");
    printf("%s (%d échec(s))\n", failures ? "ÉCHEC" : "OK", failures);
    return failures ? 1 : 0;
}
