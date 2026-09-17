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
