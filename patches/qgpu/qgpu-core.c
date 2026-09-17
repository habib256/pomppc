/*
 * qgpu-core.c — analyse et exécution du flux de commandes « qgpu ».
 *
 * Règle : TOUTE validation se fait ici, une fois, avant d'appeler le backend.
 * Un flux invalide s'arrête à la première commande fautive ; les commandes
 * précédentes restent appliquées (pas de transaction), et status_pc désigne
 * la fautive pour que l'invité puisse la retrouver.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qgpu-core.h"

#define TRACE(c, ...) do { if ((c)->trace) { \
        fprintf(stderr, "qgpu: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static bool grow_vbuf(QgpuCore *c, uint32_t nfloats)
{
    if (nfloats <= c->vbuf_cap) {
        return true;
    }
    float *n = realloc(c->vbuf, (size_t)nfloats * sizeof(float));
    if (!n) {
        return false;
    }
    c->vbuf = n; c->vbuf_cap = nfloats;
    return true;
}

static bool grow_dbuf(QgpuCore *c, uint32_t npix)
{
    if (npix <= c->dbuf_cap) {
        return true;
    }
    float *n = realloc(c->dbuf, (size_t)npix * sizeof(float));
    if (!n) {
        return false;
    }
    c->dbuf = n; c->dbuf_cap = npix;
    return true;
}

/* GL : état initial d'un contexte. */
void qgpu_state_init(QgpuState *st)
{
    memset(st, 0, sizeof(*st));
    st->v[QGPU_SK_DEPTH_FUNC]    = 0x0201;         /* GL_LESS */
    st->v[QGPU_SK_DEPTH_WRITE]   = 1;
    st->v[QGPU_SK_COLOR_MASK]    = 0xF;
    st->v[QGPU_SK_BLEND_SRC_RGB] = 0x0001;         /* GL_ONE */
    st->v[QGPU_SK_BLEND_DST_RGB] = 0x0000;         /* GL_ZERO */
    st->v[QGPU_SK_BLEND_SRC_A]   = 0x0001;
    st->v[QGPU_SK_BLEND_DST_A]   = 0x0000;
    st->v[QGPU_SK_BLEND_EQ_RGB]  = 0x8006;         /* GL_FUNC_ADD */
    st->v[QGPU_SK_BLEND_EQ_A]    = 0x8006;
    st->v[QGPU_SK_ALPHA_FUNC]    = 0x0207;         /* GL_ALWAYS */
    {
        int u;
        for (u = 0; u < QGPU_MAX_UNITS; u++) {
            st->v[QGPU_SK_UNIT(u) + QGPU_SK_U_ENV_MODE] = 0x2100;   /* GL_MODULATE */
            st->v[QGPU_SK_COMBINE0 + u] = QGPU_COMBINE_DEFAULT;
            st->v[QGPU_SK_COMBINE_SRC0 + u] = QGPU_COMBINE_SRC_DEFAULT;
        }
    }
    st->v[QGPU_SK_LINE_WIDTH]    = 0x3F800000;     /* 1.0 */
    st->v[QGPU_SK_POINT_SIZE]    = 0x3F800000;
}

static bool valid_base_format(uint32_t f)
{
    return (f >= 0x1906 && f <= 0x190A) || f == 0x8049;
}

static bool valid_env_mode(uint32_t m)
{
    return m == 0x2100 || m == 0x2101 || m == 0x0BE2 || m == 0x1E01 || m == 0x0104 ||
           m == 0x8570;                              /* v5 : GL_COMBINE */
}

/* QGPU_SK_COMBINE* : champs connus, échelles 1, 2 ou 4. */
static bool valid_combine(uint32_t v)
{
    return (v & ~0xFFFu) == 0 && (v & 0xF) <= QGPU_CB_DOT3_RGBA &&
           ((v >> 4) & 0xF) <= QGPU_CB_SUBTRACT &&
           ((v >> 8) & 3) <= 2 && ((v >> 10) & 3) <= 2;
}

static bool valid_combine_src(uint32_t v)
{
    return (v & ~((1u << 27) - 1)) == 0;             /* tout champ de 5 / 4 bits est valide */
}

static bool valid_filter(uint32_t f, bool min)
{
    if (f == 0x2600 || f == 0x2601) {
        return true;
    }
    return min && f >= 0x2700 && f <= 0x2703;
}

static bool valid_wrap(uint32_t w)
{
    return w == 0x2901 || w == 0x2900 || w == 0x812F;
}

static void tex_free(QgpuCore *c, QgpuTexture *t)
{
    int l;
    if (c->be && c->be->tex_destroy) {
        c->be->tex_destroy(c, t);
    }
    for (l = 0; l < QGPU_MAX_TEX_LEVELS; l++) {
        free(t->level[l].px);
    }
    memset(t, 0, sizeof(*t));
}

/* OpenGL 1.x : complétude d'une texture 2D. */
uint32_t qgpu_texture_levels(const QgpuTexture *t)
{
    uint32_t w, h, n;
    if (!t->used || !t->level[0].px) {
        return 0;
    }
    if (t->min_filter == 0x2600 || t->min_filter == 0x2601) {
        return 1;                                    /* pas de mipmap requis */
    }
    w = t->level[0].w;
    h = t->level[0].h;
    for (n = 1; w > 1 || h > 1; n++) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        if (n >= QGPU_MAX_TEX_LEVELS || !t->level[n].px ||
            t->level[n].w != w || t->level[n].h != h) {
            return 0;
        }
    }
    return n;
}

static bool valid_func(uint32_t f)
{
    return f >= 0x0200 && f <= 0x0207;
}

static bool valid_blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: case 0x0001:                        /* ZERO, ONE */
    case 0x0300: case 0x0301: case 0x0302: case 0x0303:  /* SRC_COLOR … ONE_MINUS_SRC_ALPHA */
    case 0x0304: case 0x0305: case 0x0306: case 0x0307:  /* DST_ALPHA … ONE_MINUS_DST_COLOR */
    case 0x0308:                                     /* SRC_ALPHA_SATURATE */
        return true;
    default:
        return false;
    }
}

static bool valid_blend_eq(uint32_t e)
{
    return e == 0x8006 || e == 0x800A || e == 0x800B;
}

/* Valide une valeur d'état ; renvoie false si elle est hors du domaine. */
static bool valid_state(uint32_t key, uint32_t val)
{
    switch (key) {
    case QGPU_SK_DEPTH_TEST: case QGPU_SK_DEPTH_WRITE: case QGPU_SK_BLEND:
    case QGPU_SK_ALPHA_TEST: case QGPU_SK_SCISSOR:
        return val <= 1;
    case QGPU_SK_DEPTH_FUNC: case QGPU_SK_ALPHA_FUNC:
        return valid_func(val);
    case QGPU_SK_COLOR_MASK:
        return val <= 0xF;
    case QGPU_SK_BLEND_SRC_RGB: case QGPU_SK_BLEND_DST_RGB:
    case QGPU_SK_BLEND_SRC_A: case QGPU_SK_BLEND_DST_A:
        return valid_blend_factor(val);
    case QGPU_SK_BLEND_EQ_RGB: case QGPU_SK_BLEND_EQ_A:
        return valid_blend_eq(val);
    case QGPU_SK_ALPHA_REF: {
        float f = qgpu_u2f(val);
        return f == f;                               /* pas de NaN */
    }
    case QGPU_SK_SCISSOR_X: case QGPU_SK_SCISSOR_Y:
    case QGPU_SK_SCISSOR_W: case QGPU_SK_SCISSOR_H:
        return val <= QGPU_MAX_SURF_DIM;
    case QGPU_SK_TEXTURE: case QGPU_SK_TEXTURE1: case QGPU_SK_TEXTURE2:
    case QGPU_SK_TEXTURE3: case QGPU_SK_FOG: case QGPU_SK_POLY_OFFSET:
        return val <= 1;
    case QGPU_SK_TEX_BIND: case QGPU_SK_TEX1_BIND: case QGPU_SK_TEX2_BIND:
    case QGPU_SK_TEX3_BIND:
        return val < QGPU_MAX_TEX;
    case QGPU_SK_TEX_ENV_MODE: case QGPU_SK_TEX1_ENV_MODE: case QGPU_SK_TEX2_ENV_MODE:
    case QGPU_SK_TEX3_ENV_MODE:
        return valid_env_mode(val);
    case QGPU_SK_TEX_ENV_COLOR: case QGPU_SK_TEX1_ENV_COLOR: case QGPU_SK_TEX2_ENV_COLOR:
    case QGPU_SK_TEX3_ENV_COLOR: case QGPU_SK_FOG_COLOR:
        return true;
    case QGPU_SK_COMBINE0: case QGPU_SK_COMBINE0 + 1:
    case QGPU_SK_COMBINE0 + 2: case QGPU_SK_COMBINE0 + 3:
        return valid_combine(val);
    case QGPU_SK_COMBINE_SRC0: case QGPU_SK_COMBINE_SRC0 + 1:
    case QGPU_SK_COMBINE_SRC0 + 2: case QGPU_SK_COMBINE_SRC0 + 3:
        return valid_combine_src(val);
    case QGPU_SK_LINE_WIDTH: case QGPU_SK_POINT_SIZE: {
        float f = qgpu_u2f(val);
        return f > 0.0f && f <= 64.0f;
    }
    case QGPU_SK_POLY_FACTOR: case QGPU_SK_POLY_UNITS: {
        float f = qgpu_u2f(val);
        return f == f && f > -1e6f && f < 1e6f;
    }
    default:
        return false;
    }
}

static bool grow_pbuf(QgpuCore *c, uint32_t npix)
{
    if (npix <= c->pbuf_cap) {
        return true;
    }
    uint32_t *n = realloc(c->pbuf, (size_t)npix * sizeof(uint32_t));
    if (!n) {
        return false;
    }
    c->pbuf = n; c->pbuf_cap = npix;
    return true;
}

bool qgpu_core_init(QgpuCore *c, const char *backend,
                    uint8_t *shmem, uint32_t shmem_size)
{
    int i;

    memset(c, 0, sizeof(*c));
    c->shmem = shmem;
    c->shmem_size = shmem_size;
    c->cur_ctx = -1;
    for (i = 0; i < QGPU_MAX_CTX; i++) {
        c->ctx[i].surf = -1;
        qgpu_state_init(&c->ctx[i].st);
    }

    if (!backend || !strcmp(backend, "auto")) {
        if (qgpu_backend_gl.init && qgpu_backend_gl.init(c)) {
            c->be = &qgpu_backend_gl;
        } else if (qgpu_backend_soft.init(c)) {
            c->be = &qgpu_backend_soft;
        }
    } else if (!strcmp(backend, "gl")) {
        if (qgpu_backend_gl.init && qgpu_backend_gl.init(c)) {
            c->be = &qgpu_backend_gl;
        }
    } else if (!strcmp(backend, "soft")) {
        if (qgpu_backend_soft.init(c)) {
            c->be = &qgpu_backend_soft;
        }
    }
    return c->be != NULL;
}

void qgpu_core_reset(QgpuCore *c)
{
    int i;

    for (i = 0; i < QGPU_MAX_SURF; i++) {
        if (c->surf[i].used) {
            c->be->surf_destroy(c, &c->surf[i]);
            memset(&c->surf[i], 0, sizeof(c->surf[i]));
        }
    }
    for (i = 0; i < QGPU_MAX_CTX; i++) {
        memset(&c->ctx[i], 0, sizeof(c->ctx[i]));
        c->ctx[i].surf = -1;
        qgpu_state_init(&c->ctx[i].st);
    }
    for (i = 0; i < QGPU_MAX_TEX; i++) {
        if (c->tex[i].used) {
            tex_free(c, &c->tex[i]);
        }
    }
    c->cur_ctx = -1;
    c->status = QGPU_ST_OK;
    c->status_pc = 0;
}

void qgpu_core_fini(QgpuCore *c)
{
    if (c->be) {
        qgpu_core_reset(c);
        c->be->fini(c);
        c->be = NULL;
    }
    free(c->vbuf); c->vbuf = NULL; c->vbuf_cap = 0;
    free(c->pbuf); c->pbuf = NULL; c->pbuf_cap = 0;
    free(c->dbuf); c->dbuf = NULL; c->dbuf_cap = 0;
}

uint32_t qgpu_core_backend_tag(const QgpuCore *c)
{
    char t[4] = { ' ', ' ', ' ', ' ' };
    const char *n = c->be ? c->be->name : "none";
    int i;

    for (i = 0; i < 4 && n[i]; i++) {
        t[i] = n[i];
    }
    return ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) |
           ((uint32_t)t[2] << 8) | (uint32_t)t[3];
}

/* Une plage [off, off+len) est-elle dans la fenêtre, alignée sur 4 ? */
static bool in_shmem(const QgpuCore *c, uint64_t off, uint64_t len)
{
    return (off & 3) == 0 && off <= c->shmem_size &&
           len <= c->shmem_size - off;
}

static QgpuSurface *surf_lookup(QgpuCore *c, uint32_t id)
{
    if (id >= QGPU_MAX_SURF || !c->surf[id].used) {
        return NULL;
    }
    return &c->surf[id];
}

/* Surface cible du contexte courant, ou NULL avec le statut posé. */
static QgpuState *cur_state(QgpuCore *c)
{
    return &c->ctx[c->cur_ctx].st;
}

static QgpuSurface *bound_surface(QgpuCore *c, uint32_t *st)
{
    if (c->cur_ctx < 0) {
        *st = QGPU_ST_NO_CTX;
        return NULL;
    }
    if (c->ctx[c->cur_ctx].surf < 0) {
        *st = QGPU_ST_NO_SURF;
        return NULL;
    }
    return &c->surf[c->ctx[c->cur_ctx].surf];
}

/* Valide un transfert surface ↔ BAR0 : [surf, off, stride, x, y, w, h]. */
static uint32_t check_xfer(QgpuCore *c, const uint32_t *a, QgpuSurface **sp)
{
    QgpuSurface *s = surf_lookup(c, a[0]);
    uint32_t off = a[1], stride = a[2], x = a[3], y = a[4], w = a[5], h = a[6];

    if (!s) {
        return QGPU_ST_NO_SURF;
    }
    if (w == 0 || h == 0 || x > s->width || y > s->height ||
        w > s->width - x || h > s->height - y) {
        return QGPU_ST_BAD_ARG;
    }
    if ((stride & 3) || stride < (uint64_t)w * 4) {
        return QGPU_ST_BAD_ARG;
    }
    if (!in_shmem(c, off, (uint64_t)(h - 1) * stride + (uint64_t)w * 4)) {
        return QGPU_ST_OOB;
    }
    *sp = s;
    return QGPU_ST_OK;
}

/* Texture de l'unité u si le texturage y est actif et la texture complète. */
static QgpuTexture *unit_texture(QgpuCore *c, const QgpuState *st, int u)
{
    QgpuTexture *t;
    if (!st->v[QGPU_SK_UNIT(u) + QGPU_SK_U_ENABLE]) {
        return NULL;
    }
    t = &c->tex[st->v[QGPU_SK_UNIT(u) + QGPU_SK_U_BIND]];
    return qgpu_texture_levels(t) ? t : NULL;
}

/* Commun aux opcodes de dessin : a = [nverts, off] ; ntex unités texturées
   (coordonnées dans les sommets). */
static uint32_t do_draw(QgpuCore *c, const uint32_t *a, uint32_t prim,
                        uint32_t words, int ntex)
{
    QgpuTexture *tex[QGPU_MAX_UNITS];
    int u;
    static const uint32_t group[3] = { 3, 2, 1 };
    uint32_t nverts = a[0], off = a[1], i, st;
    QgpuSurface *s = bound_surface(c, &st);
    QgpuState *cs;

    if (!s) {
        return st;
    }
    if (nverts == 0 || nverts % group[prim] || nverts > QGPU_MAX_VERTS) {
        return QGPU_ST_BAD_ARG;
    }
    if (!in_shmem(c, off, (uint64_t)nverts * words * 4)) {
        return QGPU_ST_OOB;
    }
    if (!grow_vbuf(c, nverts * words)) {
        return QGPU_ST_BACKEND;
    }
    for (i = 0; i < nverts * words; i++) {
        float v = qgpu_u2f(qgpu_ld32(c->shmem + off + i * 4));
        /* NaN/infini : le rasteriseur logiciel ne doit jamais les voir
           (bornes de boucles), et GL les traite de façon indéfinie. */
        if (v != v || v > 1e9f || v < -1e9f) {
            return QGPU_ST_BAD_ARG;
        }
        c->vbuf[i] = v;
    }
    cs = cur_state(c);
    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        tex[u] = u < ntex ? unit_texture(c, cs, u) : NULL;
    }
    if (!c->be->draw(c, s, cs, prim, tex, c->vbuf, nverts, words)) {
        return QGPU_ST_BACKEND;
    }
    return QGPU_ST_OK;
}

/* Exécute une commande déjà découpée ; renvoie QGPU_ST_*. */
static uint32_t exec_one(QgpuCore *c, uint32_t op, const uint32_t *a,
                         uint32_t nargs)
{
    uint32_t st;
    QgpuSurface *s;

#define WANT(n) do { if (nargs != (n) - 1) return QGPU_ST_BAD_ARG; } while (0)

    switch (op) {
    case QGPU_OP_NOP:
        WANT(QGPU_LEN_NOP);
        return QGPU_ST_OK;

    case QGPU_OP_CTX_CREATE:
        WANT(QGPU_LEN_CTX);
        if (a[0] >= QGPU_MAX_CTX) {
            return QGPU_ST_BAD_ARG;
        }
        if (c->ctx[a[0]].used) {
            return QGPU_ST_LIMIT;
        }
        c->ctx[a[0]].used = true;
        c->ctx[a[0]].surf = -1;
        qgpu_state_init(&c->ctx[a[0]].st);
        return QGPU_ST_OK;

    case QGPU_OP_CTX_DESTROY:
        WANT(QGPU_LEN_CTX);
        if (a[0] >= QGPU_MAX_CTX || !c->ctx[a[0]].used) {
            return QGPU_ST_BAD_ARG;
        }
        c->ctx[a[0]].used = false;
        c->ctx[a[0]].surf = -1;
        if (c->cur_ctx == (int32_t)a[0]) {
            c->cur_ctx = -1;
        }
        return QGPU_ST_OK;

    case QGPU_OP_CTX_BIND:
        WANT(QGPU_LEN_CTX);
        if (a[0] >= QGPU_MAX_CTX || !c->ctx[a[0]].used) {
            return QGPU_ST_BAD_ARG;
        }
        c->cur_ctx = a[0];
        return QGPU_ST_OK;

    case QGPU_OP_SURF_CREATE:
        WANT(QGPU_LEN_SURF_CREATE);
        if (a[0] >= QGPU_MAX_SURF ||
            (a[3] & QGPU_FMT_MASK) != QGPU_FMT_XRGB8888 ||
            (a[3] & ~(uint32_t)(QGPU_FMT_MASK | QGPU_FMT_FLAG_DEPTH)) ||
            a[1] == 0 || a[2] == 0 ||
            a[1] > QGPU_MAX_SURF_DIM || a[2] > QGPU_MAX_SURF_DIM) {
            return QGPU_ST_BAD_ARG;
        }
        if (c->surf[a[0]].used) {
            return QGPU_ST_LIMIT;
        }
        s = &c->surf[a[0]];
        s->width = a[1]; s->height = a[2]; s->format = a[3]; s->priv = NULL;
        s->has_depth = (a[3] & QGPU_FMT_FLAG_DEPTH) != 0;
        if (!c->be->surf_create(c, s)) {
            return QGPU_ST_BACKEND;
        }
        s->used = true;
        return QGPU_ST_OK;

    case QGPU_OP_SURF_DESTROY: {
        int i;
        WANT(QGPU_LEN_SURF);
        s = surf_lookup(c, a[0]);
        if (!s) {
            return QGPU_ST_NO_SURF;
        }
        c->be->surf_destroy(c, s);
        memset(s, 0, sizeof(*s));
        for (i = 0; i < QGPU_MAX_CTX; i++) {
            if (c->ctx[i].surf == (int32_t)a[0]) {
                c->ctx[i].surf = -1;
            }
        }
        return QGPU_ST_OK;
    }

    case QGPU_OP_SURF_BIND:
        WANT(QGPU_LEN_SURF);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (!surf_lookup(c, a[0])) {
            return QGPU_ST_NO_SURF;
        }
        c->ctx[c->cur_ctx].surf = a[0];
        return QGPU_ST_OK;

    case QGPU_OP_SURF_READBACK:
    case QGPU_OP_SURF_UPLOAD: {
        uint32_t off, stride, x, y, w, h, row;
        WANT(QGPU_LEN_SURF_XFER);
        st = check_xfer(c, a, &s);
        if (st != QGPU_ST_OK) {
            return st;
        }
        off = a[1]; stride = a[2]; x = a[3]; y = a[4]; w = a[5]; h = a[6];
        if (!grow_pbuf(c, w * h)) {
            return QGPU_ST_BACKEND;
        }
        if (op == QGPU_OP_SURF_READBACK) {
            if (!c->be->readback(c, s, x, y, w, h, c->pbuf)) {
                return QGPU_ST_BACKEND;
            }
            for (row = 0; row < h; row++) {
                uint8_t *dst = c->shmem + off + (size_t)row * stride;
                const uint32_t *src = c->pbuf + (size_t)row * w;
                uint32_t i;
                for (i = 0; i < w; i++) {
                    qgpu_st32(dst + i * 4, src[i]);
                }
            }
        } else {
            for (row = 0; row < h; row++) {
                const uint8_t *src = c->shmem + off + (size_t)row * stride;
                uint32_t *dst = c->pbuf + (size_t)row * w;
                uint32_t i;
                for (i = 0; i < w; i++) {
                    dst[i] = qgpu_ld32(src + i * 4);
                }
            }
            if (!c->be->upload(c, s, x, y, w, h, c->pbuf)) {
                return QGPU_ST_BACKEND;
            }
        }
        return QGPU_ST_OK;
    }

    case QGPU_OP_DEPTH_READBACK:
    case QGPU_OP_DEPTH_UPLOAD: {
        uint32_t off, stride, x, y, w, h, row, i;
        WANT(QGPU_LEN_SURF_XFER);
        st = check_xfer(c, a, &s);
        if (st != QGPU_ST_OK) {
            return st;
        }
        if (!s->has_depth) {
            return QGPU_ST_BAD_ARG;
        }
        off = a[1]; stride = a[2]; x = a[3]; y = a[4]; w = a[5]; h = a[6];
        if (!grow_dbuf(c, w * h)) {
            return QGPU_ST_BACKEND;
        }
        if (op == QGPU_OP_DEPTH_READBACK) {
            if (!c->be->depth_readback(c, s, x, y, w, h, c->dbuf)) {
                return QGPU_ST_BACKEND;
            }
            for (row = 0; row < h; row++) {
                uint8_t *dst = c->shmem + off + (size_t)row * stride;
                for (i = 0; i < w; i++) {
                    qgpu_st32(dst + i * 4, qgpu_f2u(c->dbuf[(size_t)row * w + i]));
                }
            }
        } else {
            for (row = 0; row < h; row++) {
                const uint8_t *src = c->shmem + off + (size_t)row * stride;
                for (i = 0; i < w; i++) {
                    float d = qgpu_u2f(qgpu_ld32(src + i * 4));
                    /* bornage : une profondeur hors [0,1] ou NaN n'a pas de sens */
                    c->dbuf[(size_t)row * w + i] = !(d > 0.0f) ? 0.0f : d > 1.0f ? 1.0f : d;
                }
            }
            if (!c->be->depth_upload(c, s, x, y, w, h, c->dbuf)) {
                return QGPU_ST_BACKEND;
            }
        }
        return QGPU_ST_OK;
    }

    case QGPU_OP_SET_STATE:
        WANT(QGPU_LEN_SET_STATE);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (a[0] == 0 || a[0] >= QGPU_SK_COUNT || !valid_state(a[0], a[1])) {
            return QGPU_ST_BAD_ARG;
        }
        cur_state(c)->v[a[0]] = a[1];
        return QGPU_ST_OK;

    case QGPU_OP_CLEAR:
        WANT(QGPU_LEN_CLEAR);
        s = bound_surface(c, &st);
        if (!s) {
            return st;
        }
        if (a[0] & ~(uint32_t)(QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH)) {
            return QGPU_ST_BAD_ARG;
        }
        {
            float d = qgpu_u2f(a[2]);
            d = !(d > 0.0f) ? 0.0f : d > 1.0f ? 1.0f : d;
            if (!c->be->clear(c, s, cur_state(c), a[0], a[1], d)) {
                return QGPU_ST_BACKEND;
            }
        }
        return QGPU_ST_OK;

    case QGPU_OP_VIEWPORT:
        WANT(QGPU_LEN_VIEWPORT);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        c->ctx[c->cur_ctx].vp[0] = a[0]; c->ctx[c->cur_ctx].vp[1] = a[1];
        c->ctx[c->cur_ctx].vp[2] = a[2]; c->ctx[c->cur_ctx].vp[3] = a[3];
        return QGPU_ST_OK;

    case QGPU_OP_DRAW_TRIANGLES:
        WANT(QGPU_LEN_DRAW);
        return do_draw(c, a, QGPU_PRIM_TRIANGLES, QGPU_VERTEX_WORDS, 0);
    case QGPU_OP_DRAW_TRIANGLES_TEX:
        WANT(QGPU_LEN_DRAW);
        return do_draw(c, a, QGPU_PRIM_TRIANGLES, QGPU_VERTEX_TEX_WORDS, 1);
    case QGPU_OP_DRAW_TRIANGLES_TEX2:
        WANT(QGPU_LEN_DRAW);
        return do_draw(c, a, QGPU_PRIM_TRIANGLES, QGPU_VERTEX_TEX2_WORDS, 2);
    case QGPU_OP_DRAW_TRIANGLES_TEXN:
        WANT(QGPU_LEN_DRAW_N);
        if (a[2] < 1 || a[2] > QGPU_MAX_UNITS) {
            return QGPU_ST_BAD_ARG;
        }
        return do_draw(c, a, QGPU_PRIM_TRIANGLES, QGPU_VERTEX_TEXN_WORDS(a[2]), a[2]);
    case QGPU_OP_DRAW_LINES:
        WANT(QGPU_LEN_DRAW);
        return do_draw(c, a, QGPU_PRIM_LINES, QGPU_VERTEX_WORDS, 0);
    case QGPU_OP_DRAW_POINTS:
        WANT(QGPU_LEN_DRAW);
        return do_draw(c, a, QGPU_PRIM_POINTS, QGPU_VERTEX_WORDS, 0);

    case QGPU_OP_TEX_CREATE:
        WANT(QGPU_LEN_TEX);
        if (a[0] >= QGPU_MAX_TEX) {
            return QGPU_ST_BAD_ARG;
        }
        if (c->tex[a[0]].used) {
            return QGPU_ST_LIMIT;
        }
        memset(&c->tex[a[0]], 0, sizeof(c->tex[a[0]]));
        c->tex[a[0]].used = true;
        /* valeurs initiales d'OpenGL */
        c->tex[a[0]].min_filter = 0x2702;          /* NEAREST_MIPMAP_LINEAR */
        c->tex[a[0]].mag_filter = 0x2601;          /* LINEAR */
        c->tex[a[0]].wrap_s = c->tex[a[0]].wrap_t = 0x2901;
        c->tex[a[0]].params_dirty = true;
        return QGPU_ST_OK;

    case QGPU_OP_TEX_DESTROY:
        WANT(QGPU_LEN_TEX);
        if (a[0] >= QGPU_MAX_TEX || !c->tex[a[0]].used) {
            return QGPU_ST_BAD_ARG;
        }
        tex_free(c, &c->tex[a[0]]);
        return QGPU_ST_OK;

    case QGPU_OP_TEX_PARAM: {
        QgpuTexture *t;
        WANT(QGPU_LEN_TEX_PARAM);
        if (a[0] >= QGPU_MAX_TEX || !c->tex[a[0]].used) {
            return QGPU_ST_BAD_ARG;
        }
        t = &c->tex[a[0]];
        switch (a[1]) {
        case QGPU_TP_MIN_FILTER:
            if (!valid_filter(a[2], true)) return QGPU_ST_BAD_ARG;
            t->min_filter = a[2];
            break;
        case QGPU_TP_MAG_FILTER:
            if (!valid_filter(a[2], false)) return QGPU_ST_BAD_ARG;
            t->mag_filter = a[2];
            break;
        case QGPU_TP_WRAP_S:
            if (!valid_wrap(a[2])) return QGPU_ST_BAD_ARG;
            t->wrap_s = a[2];
            break;
        case QGPU_TP_WRAP_T:
            if (!valid_wrap(a[2])) return QGPU_ST_BAD_ARG;
            t->wrap_t = a[2];
            break;
        default:
            return QGPU_ST_BAD_ARG;
        }
        t->params_dirty = true;
        return QGPU_ST_OK;
    }

    case QGPU_OP_TEX_IMAGE: {
        QgpuTexture *t;
        QgpuTexLevel *lv;
        uint32_t lvl = a[1], w = a[2], h = a[3], fmt = a[4], off = a[5], n, i;
        uint32_t *px;
        WANT(QGPU_LEN_TEX_IMAGE);
        if (a[0] >= QGPU_MAX_TEX || !c->tex[a[0]].used || lvl >= QGPU_MAX_TEX_LEVELS ||
            w == 0 || h == 0 || w > QGPU_MAX_TEX_DIM || h > QGPU_MAX_TEX_DIM ||
            !valid_base_format(fmt)) {
            return QGPU_ST_BAD_ARG;
        }
        n = w * h;
        if (!in_shmem(c, off, (uint64_t)n * 4)) {
            return QGPU_ST_OOB;
        }
        t = &c->tex[a[0]];
        lv = &t->level[lvl];
        px = (lv->px && lv->w * lv->h == n) ? lv->px : malloc((size_t)n * sizeof(uint32_t));
        if (!px) {
            return QGPU_ST_BACKEND;
        }
        if (px != lv->px) {
            free(lv->px);
        }
        for (i = 0; i < n; i++) {
            px[i] = qgpu_ld32(c->shmem + off + (size_t)i * 4);
        }
        lv->px = px;
        lv->w = w;
        lv->h = h;
        if (lvl == 0) {
            t->base_format = fmt;
        }
        t->dirty |= 1u << lvl;
        return QGPU_ST_OK;
    }

    default:
        return QGPU_ST_BAD_OPCODE;
    }
#undef WANT
}

static bool known_op(uint32_t op)
{
    switch (op) {
    case QGPU_OP_NOP: case QGPU_OP_CTX_CREATE: case QGPU_OP_CTX_DESTROY:
    case QGPU_OP_CTX_BIND: case QGPU_OP_SURF_CREATE: case QGPU_OP_SURF_DESTROY:
    case QGPU_OP_SURF_BIND: case QGPU_OP_SURF_READBACK: case QGPU_OP_SURF_UPLOAD:
    case QGPU_OP_DEPTH_READBACK: case QGPU_OP_DEPTH_UPLOAD: case QGPU_OP_CLEAR:
    case QGPU_OP_VIEWPORT: case QGPU_OP_SET_STATE: case QGPU_OP_DRAW_TRIANGLES:
    case QGPU_OP_DRAW_TRIANGLES_TEX: case QGPU_OP_TEX_CREATE: case QGPU_OP_TEX_DESTROY:
    case QGPU_OP_DRAW_TRIANGLES_TEX2: case QGPU_OP_DRAW_LINES: case QGPU_OP_DRAW_POINTS:
    case QGPU_OP_DRAW_TRIANGLES_TEXN:
    case QGPU_OP_TEX_IMAGE: case QGPU_OP_TEX_PARAM:
        return true;
    default:
        return false;
    }
}

uint32_t qgpu_core_execute(QgpuCore *c, uint32_t off, uint32_t len)
{
    uint32_t nwords, pc = 0, st = QGPU_ST_OK;
    uint32_t args[QGPU_LEN_SURF_XFER];    /* la plus longue commande (8 mots) */

    c->status_pc = 0;
    if (!in_shmem(c, off, len) || (len & 3) || len == 0 ||
        len / 4 > QGPU_MAX_CMD_WORDS) {
        c->status = QGPU_ST_BAD_SUBMIT;
        return c->status;
    }
    nwords = len / 4;

    while (pc < nwords) {
        const uint8_t *p = c->shmem + off + (size_t)pc * 4;
        uint32_t hdr = qgpu_ld32(p);
        uint32_t op = QGPU_CMD_OP(hdr), clen = QGPU_CMD_LEN(hdr), i;

        if (clen == 0 || clen > nwords - pc) {
            st = QGPU_ST_BAD_HEADER;
            break;
        }
        if (clen - 1 > sizeof(args) / sizeof(args[0])) {
            /* Trop d'arguments pour toute commande connue : soit un opcode
               inconnu (dit tel quel), soit une longueur incohérente. */
            st = known_op(op) ? QGPU_ST_BAD_ARG : QGPU_ST_BAD_OPCODE;
            break;
        }
        for (i = 1; i < clen; i++) {
            args[i - 1] = qgpu_ld32(p + i * 4);
        }
        TRACE(c, "pc=%u op=0x%04x len=%u", pc, op, clen);
        st = exec_one(c, op, args, clen - 1);
        if (st != QGPU_ST_OK) {
            TRACE(c, "  -> statut %u", st);
            break;
        }
        c->ncmds++;
        pc += clen;
    }

    c->status = st;
    c->status_pc = (st == QGPU_ST_OK) ? 0 : pc;
    return st;
}
