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
#include <math.h>
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

static bool grow_sbuf(QgpuCore *c, uint32_t npix)
{
    if (npix <= c->sbuf_cap) {
        return true;
    }
    uint8_t *n = realloc(c->sbuf, npix);
    if (!n) {
        return false;
    }
    c->sbuf = n; c->sbuf_cap = npix;
    return true;
}

static bool grow_ibuf(QgpuCore *c, uint32_t n)
{
    if (n <= c->ibuf_cap) {
        return true;
    }
    {
        uint32_t *p = realloc(c->ibuf, (size_t)n * sizeof(uint32_t));
        if (!p) {
            return false;
        }
        c->ibuf = p; c->ibuf_cap = n;
    }
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
            st->v[QGPU_SK_COMBINE(u)] = QGPU_COMBINE_DEFAULT;
            st->v[QGPU_SK_COMBINE_SRC(u)] = QGPU_COMBINE_SRC_DEFAULT;
        }
    }
    st->v[QGPU_SK_LINE_WIDTH]    = 0x3F800000;     /* 1.0 */
    st->v[QGPU_SK_POINT_SIZE]    = 0x3F800000;
    /* v6 : stencil, valeurs initiales d'OpenGL (masques à tous les bits,
       tronqués à la largeur du tampon : 8 bits). */
    st->v[QGPU_SK_STENCIL_FUNC]       = 0x0207;    /* GL_ALWAYS */
    st->v[QGPU_SK_STENCIL_VALUE_MASK] = 0xFF;
    st->v[QGPU_SK_STENCIL_WRITE_MASK] = 0xFF;
    st->v[QGPU_SK_STENCIL_OP_FAIL]    = QGPU_SOP_KEEP;
    st->v[QGPU_SK_STENCIL_OP_ZFAIL]   = QGPU_SOP_KEEP;
    st->v[QGPU_SK_STENCIL_OP_ZPASS]   = QGPU_SOP_KEEP;
    /* v7 : étage géométrique, valeurs initiales d'OpenGL. */
    st->v[QGPU_SK_SHADE_MODEL]    = 0x1D01;        /* GL_SMOOTH */
    st->v[QGPU_SK_CULL_MODE]      = 0x0405;        /* GL_BACK */
    st->v[QGPU_SK_FRONT_FACE]     = 0x0901;        /* GL_CCW */
    st->v[QGPU_SK_COLOR_MAT_FACE] = 0x0408;        /* GL_FRONT_AND_BACK */
    st->v[QGPU_SK_COLOR_MAT_MODE] = 0x1602;        /* GL_AMBIENT_AND_DIFFUSE */
    st->v[QGPU_SK_COLOR_CONTROL]  = 0x81F9;        /* GL_SINGLE_COLOR */
    st->v[QGPU_SK_FOG_MODE]       = QGPU_FOG_VERTEX;
    st->v[QGPU_SK_FOG_DENSITY]    = 0x3F800000;    /* 1.0 */
    st->v[QGPU_SK_FOG_START]      = 0x00000000;    /* 0.0 */
    st->v[QGPU_SK_FOG_END]        = 0x3F800000;    /* 1.0 */
    /* v8 : fin du pipeline fixe, valeurs initiales d'OpenGL — toutes neutres,
       c'est ce qui garde les flux v1–v7 inchangés. */
    st->v[QGPU_SK_LOGIC_OP_MODE]  = QGPU_LO_COPY;
    st->v[QGPU_SK_POLYGON_MODE_FRONT] = QGPU_POLY_FILL;
    st->v[QGPU_SK_POLYGON_MODE_BACK]  = QGPU_POLY_FILL;
    st->v[QGPU_SK_LINE_STIPPLE_FACTOR]  = 1;
    st->v[QGPU_SK_LINE_STIPPLE_PATTERN] = 0xFFFF;
    /* v10 : couleur secondaire comme en v7–v9, points sans atténuation. */
    st->v[QGPU_SK_COLOR_SUM]        = QGPU_CSUM_FORMAT;
    st->v[QGPU_SK_POINT_SIZE_MAX]   = 0x42800000;    /* 64.0 */
    st->v[QGPU_SK_POINT_FADE]       = 0x3F800000;    /* 1.0 */
    st->v[QGPU_SK_POINT_ATT_CONST]  = 0x3F800000;    /* 1.0 */
}

/* v10 : vrai si les paramètres de point sont ceux de l'état initial — la
   taille de QGPU_SK_POINT_SIZE passe alors telle quelle. */
bool qgpu_points_plain(const QgpuState *st)
{
    return st->v[QGPU_SK_POINT_ATT_CONST] == 0x3F800000 &&
           st->v[QGPU_SK_POINT_ATT_LINEAR] == 0 && st->v[QGPU_SK_POINT_ATT_QUAD] == 0 &&
           st->v[QGPU_SK_POINT_SIZE_MIN] == 0 && st->v[QGPU_SK_POINT_SIZE_MAX] == 0x42800000;
}

/* v10 : taille dérivée d'un point de DRAW_RAW à la distance d de l'œil
   (OpenGL 1.4 §3.3, ARB_point_parameters) — la formule est ici pour que les
   deux backends ne puissent pas en avoir deux idées. */
float qgpu_point_size(const QgpuState *st, float d)
{
    float size = qgpu_u2f(st->v[QGPU_SK_POINT_SIZE]);
    float den = qgpu_u2f(st->v[QGPU_SK_POINT_ATT_CONST]) +
                qgpu_u2f(st->v[QGPU_SK_POINT_ATT_LINEAR]) * d +
                qgpu_u2f(st->v[QGPU_SK_POINT_ATT_QUAD]) * d * d;
    float mn = qgpu_u2f(st->v[QGPU_SK_POINT_SIZE_MIN]);
    float mx = qgpu_u2f(st->v[QGPU_SK_POINT_SIZE_MAX]);

    size = den > 0.0f ? size * sqrtf(1.0f / den) : mx;
    if (size > mx) size = mx;
    if (size < mn) size = mn;
    return size;
}

/* v8 : motif de pointillé initial — tout à 1, donc invisible. */
void qgpu_stipple_init(QgpuStipple *sp)
{
    int i;
    for (i = 0; i < 32; i++) {
        sp->row[i] = 0xFFFFFFFFu;
    }
}

static void set4(float *d, float a, float b, float c, float e)
{
    d[0] = a; d[1] = b; d[2] = c; d[3] = e;
}

/* v7 : état géométrique initial, celui d'OpenGL à la création d'un contexte. */
void qgpu_geom_init(QgpuGeom *gm)
{
    int i, u, k;

    memset(gm, 0, sizeof(*gm));
    for (i = 0; i < QGPU_MTX_COUNT; i++) {
        gm->mtx[i][0] = gm->mtx[i][5] = gm->mtx[i][10] = gm->mtx[i][15] = 1.0f;
    }
    for (i = 0; i < QGPU_MAX_LIGHTS; i++) {
        QgpuLight *l = &gm->light[i];
        set4(l->ambient, 0.0f, 0.0f, 0.0f, 1.0f);
        /* GL : seule GL_LIGHT0 a une diffuse et une spéculaire blanches. */
        set4(l->diffuse, i ? 0.0f : 1.0f, i ? 0.0f : 1.0f, i ? 0.0f : 1.0f, 1.0f);
        set4(l->specular, i ? 0.0f : 1.0f, i ? 0.0f : 1.0f, i ? 0.0f : 1.0f, 1.0f);
        set4(l->position, 0.0f, 0.0f, 1.0f, 0.0f);
        l->spot_dir[0] = 0.0f; l->spot_dir[1] = 0.0f; l->spot_dir[2] = -1.0f;
        l->spot_exp = 0.0f;
        l->spot_cutoff = 180.0f;
        l->att[0] = 1.0f; l->att[1] = 0.0f; l->att[2] = 0.0f;
    }
    for (i = 0; i < 2; i++) {
        set4(gm->mat[i].ambient, 0.2f, 0.2f, 0.2f, 1.0f);
        set4(gm->mat[i].diffuse, 0.8f, 0.8f, 0.8f, 1.0f);
        set4(gm->mat[i].specular, 0.0f, 0.0f, 0.0f, 1.0f);
        set4(gm->mat[i].emission, 0.0f, 0.0f, 0.0f, 1.0f);
        gm->mat[i].shininess = 0.0f;
    }
    set4(gm->lm_ambient, 0.2f, 0.2f, 0.2f, 1.0f);
    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        for (k = 0; k < 4; k++) {
            QgpuTexgen *tg = &gm->texgen[u][k];
            tg->mode = QGPU_TG_EYE_LINEAR;
            /* GL : plans initiaux (1,0,0,0) pour S, (0,1,0,0) pour T, 0 pour R et Q */
            if (k < 2) {
                tg->obj_plane[k] = 1.0f;
                tg->eye_plane[k] = 1.0f;
            }
        }
        set4(gm->cur_tex[u], 0.0f, 0.0f, 0.0f, 1.0f);
    }
    gm->cur_normal[2] = 1.0f;
    set4(gm->cur_color, 1.0f, 1.0f, 1.0f, 1.0f);
    /* 1.0 = « pas de brouillard » dans la convention du fil (cf. qgpu_proto.h) */
    gm->cur_fog = 1.0f;
    gm->depth_near = 0.0f;
    gm->depth_far = 1.0f;
}

/* v7 : offset d'un attribut dans un sommet, dans l'ordre fixe du protocole.
   QGPU_CAP_GEN_SIZES : `gs` = valeur de QGPU_SK_GEN_SIZES (0 = 4 composantes
   par générique, la forme d'origine). */
int qgpu_vf_offset_gs(uint32_t fmt, uint32_t gs, uint32_t bit)
{
    static const uint32_t order[6] = {
        QGPU_VF_NORMAL, QGPU_VF_COLOR, QGPU_VF_SEC_COLOR, QGPU_VF_FOG,
        QGPU_VF_TEX(0), QGPU_VF_TEX(1)
    };
    static const int size[6] = { 3, 4, 3, 1, 4, 4 };
    int off = QGPU_VF_POS_COUNT(fmt), i;

    if (bit == 0) {
        return 0;                                /* la position est toujours là */
    }
    for (i = 0; i < 6; i++) {
        if (bit == order[i]) {
            return (fmt & bit) ? off : -1;
        }
        if (fmt & order[i]) {
            off += size[i];
        }
    }
    /* unités 2 à 7, à la suite de l'unité 1 */
    for (i = 2; i < QGPU_MAX_UNITS; i++) {
        if (bit == (uint32_t)QGPU_VF_TEX(i)) {
            return (fmt & bit) ? off : -1;
        }
        if (fmt & (uint32_t)QGPU_VF_TEX(i)) {
            off += 4;
        }
    }
    /* v16 : attributs génériques 0..7, après les coordonnées de texture */
    for (i = 0; i < QGPU_VF_GEN_MAX; i++) {
        if (bit == (uint32_t)QGPU_VF_GEN(i)) {
            return (fmt & bit) ? off : -1;
        }
        if (fmt & (uint32_t)QGPU_VF_GEN(i)) {
            off += QGPU_GS_COUNT(gs, i);
        }
    }
    return -1;
}

int qgpu_vf_offset(uint32_t fmt, uint32_t bit)
{
    return qgpu_vf_offset_gs(fmt, 0, bit);
}

/* ── v16 : programmes ARB ─────────────────────────────────────────────────── */

static void prog_set_init(QgpuProgSet *pg)
{
    memset(pg, 0, sizeof(*pg));
    pg->bound[QGPU_PROG_VP] = -1;
    pg->bound[QGPU_PROG_FP] = -1;
}

static void prog_free(QgpuCore *c, QgpuProgram *p)
{
    if (p->priv && c->be && c->be->prog_destroy) {
        c->be->prog_destroy(c, p);
    }
    free(p->text);
    free(p->local);
    memset(p, 0, sizeof(*p));
}

/* Détruit les programmes d'un contexte (destruction, reset). */
static void prog_set_free(QgpuCore *c, QgpuProgSet *pg)
{
    int i;
    for (i = 0; i < QGPU_MAX_PROG; i++) {
        if (pg->prog[i].used) {
            prog_free(c, &pg->prog[i]);
        }
    }
    prog_set_init(pg);
}

/* Cible ARB → indice [VP, FP], ou -1. */
static int prog_which(uint32_t target)
{
    return target == QGPU_PT_VERTEX ? QGPU_PROG_VP :
           target == QGPU_PT_FRAGMENT ? QGPU_PROG_FP : -1;
}

/* Le texte d'un programme est-il recevable ? ASCII imprimable, tabulation et
   fins de ligne seulement, et l'en-tête de SA cible en tête : c'est ce que
   tout compilateur ARB exige, dit ici pour que le refus ait un statut et un
   pc plutôt qu'une erreur GL muette. */
static bool prog_text_ok(const uint8_t *s, uint32_t len, uint32_t target)
{
    static const char hv[] = "!!ARBvp1.0", hf[] = "!!ARBfp1.0";
    const char *h = target == QGPU_PT_VERTEX ? hv : hf;
    uint32_t i;

    if (len < 10 || memcmp(s, h, 10) != 0) {
        return false;
    }
    for (i = 0; i < len; i++) {
        uint8_t ch = s[i];
        if (!((ch >= 0x20 && ch <= 0x7e) || ch == '\t' || ch == '\n' || ch == '\r')) {
            return false;
        }
    }
    return true;
}

static bool in_shmem(const QgpuCore *c, uint64_t off, uint64_t len);

/* n × 4 flottants big-endian de BAR0, assainis comme read_f. */
static uint32_t read_f4_shmem(QgpuCore *c, uint32_t off, uint32_t n, float (*dst)[4])
{
    float tmp[QGPU_MAX_PROG_PARAMS][4];        /* tout ou rien : validé avant d'écrire */
    uint32_t i, j;
    const uint8_t *p;

    if (n > QGPU_MAX_PROG_PARAMS) {
        return QGPU_ST_BAD_ARG;
    }
    if (!in_shmem(c, off, (uint64_t)n * 16)) {
        return QGPU_ST_OOB;
    }
    p = c->shmem + off;
    for (i = 0; i < n; i++) {
        for (j = 0; j < 4; j++) {
            float v = qgpu_u2f(qgpu_ld32(p + (i * 4 + j) * 4));
            if (v != v || v > 1e9f || v < -1e9f) {
                return QGPU_ST_BAD_ARG;
            }
            tmp[i][j] = v;
        }
    }
    memcpy(dst, tmp, (size_t)n * sizeof(tmp[0]));
    return QGPU_ST_OK;
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

/* v10 : GL_MIRRORED_REPEAT et GL_CLAMP_TO_BORDER s'ajoutent ; une texture
   RECTANGLE n'accepte que les modes qui bornent (règle d'OpenGL). */
static bool valid_wrap(uint32_t w, uint32_t target)
{
    if (w == 0x2900 || w == 0x812F || w == QGPU_TW_CLAMP_TO_BORDER) {
        return true;
    }
    return target != QGPU_TT_RECTANGLE &&
           (w == 0x2901 || w == QGPU_TW_MIRRORED_REPEAT);
}

static bool valid_target(uint32_t t)
{
    return t == QGPU_TT_1D || t == QGPU_TT_2D || t == QGPU_TT_3D ||
           t == QGPU_TT_CUBE_MAP || t == QGPU_TT_RECTANGLE;
}

static bool finite_f(uint32_t u, float bound)
{
    float f = qgpu_u2f(u);
    return f == f && f >= -bound && f <= bound;
}

/* ── Textures (v3, v10) ─────────────────────────────────────────────────────
 *
 * Tout ce qui touche au CONTENU d'une texture est ici, dans le cœur : les
 * conversions de format, la décompression S3TC et la génération des mipmaps.
 * Les backends ne voient que des mots ARGB (ou des flottants de profondeur),
 * donc ils ne peuvent pas en avoir deux idées — et le backend de référence
 * reste la vérité terrain pour le GPU hôte. */

static void tex_free(QgpuCore *c, QgpuTexture *t)
{
    int f, l;
    if (c->be && c->be->tex_destroy) {
        c->be->tex_destroy(c, t);
    }
    for (f = 0; f < QGPU_TEX_FACES; f++) {
        for (l = 0; l < QGPU_MAX_TEX_LEVELS; l++) {
            free(t->level[f][l].px);
        }
    }
    memset(t, 0, sizeof(*t));
}

/* Valeurs initiales d'OpenGL d'une texture de cible `target`. */
static void tex_init(QgpuTexture *t, uint32_t target)
{
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->target = target;
    t->nfaces = target == QGPU_TT_CUBE_MAP ? QGPU_TEX_FACES : 1;
    if (target == QGPU_TT_RECTANGLE) {
        t->min_filter = 0x2601;                          /* LINEAR */
        t->wrap_s = t->wrap_t = t->wrap_r = 0x812F;      /* CLAMP_TO_EDGE */
    } else {
        t->min_filter = 0x2702;                          /* NEAREST_MIPMAP_LINEAR */
        t->wrap_s = t->wrap_t = t->wrap_r = 0x2901;      /* REPEAT */
    }
    t->mag_filter = 0x2601;                              /* LINEAR */
    t->min_lod = -1000.0f;
    t->max_lod = 1000.0f;
    t->max_level = 1000;
    t->compare_func = 0x0203;                            /* LEQUAL */
    t->depth_mode = 0x1909;                              /* LUMINANCE */
    t->params_dirty = true;
}

/* Le format de base d'une texture est celui de son niveau de base. */
static void tex_refresh_format(QgpuTexture *t)
{
    const QgpuTexLevel *lv = &t->level[0][t->base_level];
    t->base_format = lv->px ? lv->fmt : 0;
}

static uint32_t ilog2u(uint32_t v)
{
    uint32_t n = 0;
    while (v >>= 1) {
        n++;
    }
    return n;
}

/* Dernier niveau q de la chaîne qui part du niveau de base (OpenGL 1.2) :
   q = min(b + log2 de la plus grande dimension, MAX_LEVEL). */
static uint32_t tex_last_level(const QgpuTexture *t, const QgpuTexLevel *base)
{
    uint32_t m = base->w;
    if (base->h > m) m = base->h;
    if (base->d > m) m = base->d;
    m = t->base_level + ilog2u(m);
    return t->max_level < m ? t->max_level : m;
}

/* OpenGL 1.2 à 1.4 : complétude, niveau de base et MAX_LEVEL compris ; pour
   une carte de cube, les six faces carrées, de même taille et de même format. */
uint32_t qgpu_texture_levels(const QgpuTexture *t)
{
    uint32_t b = t->base_level, f, n, q, w, h, d;
    const QgpuTexLevel *l0;

    if (!t->used || b >= QGPU_MAX_TEX_LEVELS || !t->level[0][b].px) {
        return 0;
    }
    l0 = &t->level[0][b];
    for (f = 1; f < t->nfaces; f++) {
        const QgpuTexLevel *lf = &t->level[f][b];
        if (!lf->px || lf->w != l0->w || lf->h != l0->h || lf->fmt != l0->fmt) {
            return 0;
        }
    }
    if (t->min_filter == 0x2600 || t->min_filter == 0x2601) {
        return 1;                                        /* pas de mipmap requis */
    }
    q = tex_last_level(t, l0);
    if (q < b) {
        return 0;                                        /* MAX_LEVEL < BASE_LEVEL */
    }
    w = l0->w; h = l0->h; d = l0->d;
    for (n = b + 1; n <= q; n++) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        d = d > 1 ? d / 2 : 1;
        if (n >= QGPU_MAX_TEX_LEVELS) {
            return 0;
        }
        for (f = 0; f < t->nfaces; f++) {
            const QgpuTexLevel *lv = &t->level[f][n];
            if (!lv->px || lv->w != w || lv->h != h || lv->d != d || lv->fmt != l0->fmt) {
                return 0;
            }
        }
    }
    return q - b + 1;
}

/* Face désignée par une cible d'IMAGE, ou -1 si elle ne va pas à la texture. */
static int tex_face(const QgpuTexture *t, uint32_t itarget)
{
    if (t->target == QGPU_TT_CUBE_MAP) {
        return (itarget >= QGPU_TT_CUBE_FACE(0) && itarget <= QGPU_TT_CUBE_FACE(5))
               ? (int)(itarget - QGPU_TT_CUBE_FACE(0)) : -1;
    }
    return itarget == t->target ? 0 : -1;
}

/* Format des données d'une image (v10) : un couple (format, type) d'OpenGL. */
typedef struct TexSrc {
    uint32_t fmt, type;
    uint32_t bpp;            /* octets par texel ; 0 pour un format compressé */
    uint32_t block;          /* octets par bloc de 4×4 (compressé), 0 sinon */
    bool     depth;
} TexSrc;

static bool tex_src(uint32_t fmt, uint32_t type, TexSrc *s)
{
    s->fmt = fmt; s->type = type; s->bpp = 0; s->block = 0; s->depth = false;
    switch (type) {
    case 0x1401:                                         /* GL_UNSIGNED_BYTE */
        switch (fmt) {
        case 0x1908: case 0x80E1: s->bpp = 4; return true;           /* RGBA, BGRA */
        case 0x1907: case 0x80E0: s->bpp = 3; return true;           /* RGB, BGR */
        case 0x190A: s->bpp = 2; return true;                        /* LUMINANCE_ALPHA */
        case 0x1909: case 0x1906: case 0x1903:
        case 0x1900: case 0x80E5: case 0x8049:
            s->bpp = 1; return true;                    /* L, A, RED, INDEX, I */
        }
        return false;
    case 0x8035: case 0x8367:                            /* UINT_8_8_8_8 (_REV) */
        s->bpp = 4;
        return fmt == 0x1908 || fmt == 0x80E1;
    case 0x8363: case 0x8364:                            /* USHORT_5_6_5 (_REV) */
        s->bpp = 2;
        return fmt == 0x1907;
    case 0x8033: case 0x8034:                            /* USHORT_4_4_4_4, _5_5_5_1 */
        s->bpp = 2;
        return fmt == 0x1908 || fmt == 0x80E1;
    case 0x8365: case 0x8366:                            /* USHORT_4_4_4_4_REV, _1_5_5_5_REV */
        s->bpp = 2;
        return fmt == 0x80E1 || fmt == 0x1908;
    case 0x1406: case 0x1405:                            /* FLOAT, UNSIGNED_INT */
        s->bpp = 4; s->depth = true;
        return fmt == 0x1902;
    case 0x1403:                                         /* UNSIGNED_SHORT */
        s->bpp = 2; s->depth = true;
        return fmt == 0x1902;
    case 0:
        if (fmt == QGPU_TF_DXT1_RGB || fmt == QGPU_TF_DXT1_RGBA) {
            s->block = 8;
            return true;
        }
        if (fmt == QGPU_TF_DXT3 || fmt == QGPU_TF_DXT5) {
            s->block = 16;
            return true;
        }
        return false;
    }
    return false;
}

static inline uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline uint32_t x5(uint32_t v) { return (v << 3) | (v >> 2); }
static inline uint32_t x6(uint32_t v) { return (v << 2) | (v >> 4); }

static inline uint32_t ld16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

/* Un texel des données de l'application → mot ARGB hôte-natif (ou flottant de
   profondeur rangé bit à bit). Règles d'OpenGL (table 3.19) : L donne
   R = G = B, ALPHA donne (0, 0, 0, A), RED donne (R, 0, 0, 1) ; les types
   compactés sont des mots big-endian, comme tout le fil.

   H1 : le texel ALPHA a été blanchi un temps en (255, 255, 255, A) pour qu'un
   backend qui promeut GL_ALPHA en RGBA rende quand même du blanc modulé.
   C'était neutre en MODULATE et faux partout ailleurs — REPLACE, ADD, BLEND
   et COMBINE rendaient du BLANC au lieu de la couleur primaire (polices et
   HUD blancs) — et ça rendait mort le traitement correct du backend logiciel.
   Le texel reprend donc la valeur de la spécification ; c'est au backend de
   ne retenir que l'alpha (format interne GL_ALPHA côté GL). */
static uint32_t unpack_texel(const TexSrc *s, const uint8_t *p)
{
    uint32_t v;

    switch (s->type) {
    case 0x1401:
        switch (s->fmt) {
        case 0x1908: return argb(p[3], p[0], p[1], p[2]);
        case 0x80E1: return argb(p[3], p[2], p[1], p[0]);
        case 0x1907: return argb(255, p[0], p[1], p[2]);
        case 0x80E0: return argb(255, p[2], p[1], p[0]);
        case 0x190A: return argb(p[1], p[0], p[0], p[0]);
        case 0x1909: return argb(255, p[0], p[0], p[0]);
        case 0x1906: return argb(p[0], 0, 0, 0);         /* ALPHA : (0, 0, 0, A) */
        case 0x8049: return argb(p[0], p[0], p[0], p[0]); /* INTENSITY */
        case 0x1900: case 0x80E5:                        /* COLOR_INDEX, sans palette */
            return argb(255, p[0], p[0], p[0]);
        default:     return argb(255, p[0], 0, 0);          /* RED */
        }
    case 0x8035:                                            /* 1er composant en poids fort */
        v = qgpu_ld32(p);
        return s->fmt == 0x1908
               ? argb(v & 255, v >> 24, (v >> 16) & 255, (v >> 8) & 255)
               : argb(v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24);
    case 0x8367:                                            /* 1er composant en poids faible */
        v = qgpu_ld32(p);
        return s->fmt == 0x1908
               ? argb(v >> 24, v & 255, (v >> 8) & 255, (v >> 16) & 255)
               : v;                                         /* BGRA _REV = ARGB big-endian */
    case 0x8363:
        v = ld16(p);
        return argb(255, x5(v >> 11), x6((v >> 5) & 63), x5(v & 31));
    case 0x8364:
        v = ld16(p);
        return argb(255, x5(v & 31), x6((v >> 5) & 63), x5(v >> 11));
    /* H2 : depuis la v14 tex_src accepte GL_RGBA *et* GL_BGRA pour les quatre
       types 16 bits compactés. Le premier composant du mot est donc R ou B
       selon s->fmt, exactement comme pour 0x8035/0x8367 — sans ce test,
       BGRA+4444, RGBA+4444_REV, BGRA+5551 et RGBA+1555_REV sortaient R et B
       échangés (textures 16 bits bleues au lieu de rouges). */
    case 0x8033:                                            /* 4_4_4_4 */
        v = ld16(p);
        return s->fmt == 0x1908
               ? argb((v & 15) * 17, (v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17)
               : argb((v & 15) * 17, ((v >> 4) & 15) * 17, ((v >> 8) & 15) * 17, (v >> 12) * 17);
    case 0x8365:                                            /* 4_4_4_4_REV */
        v = ld16(p);
        return s->fmt == 0x80E1
               ? argb((v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17)
               : argb((v >> 12) * 17, (v & 15) * 17, ((v >> 4) & 15) * 17, ((v >> 8) & 15) * 17);
    case 0x8034:                                            /* 5_5_5_1 */
        v = ld16(p);
        return s->fmt == 0x1908
               ? argb((v & 1) * 255, x5(v >> 11), x5((v >> 6) & 31), x5((v >> 1) & 31))
               : argb((v & 1) * 255, x5((v >> 1) & 31), x5((v >> 6) & 31), x5(v >> 11));
    case 0x8366:                                            /* 1_5_5_5_REV */
        v = ld16(p);
        return s->fmt == 0x80E1
               ? argb((v >> 15) * 255, x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31))
               : argb((v >> 15) * 255, x5(v & 31), x5((v >> 5) & 31), x5((v >> 10) & 31));
    case 0x1406: {
        float f = qgpu_u2f(qgpu_ld32(p));
        f = (f == f) ? (f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f) : 0.0f;
        return qgpu_f2u(f);
    }
    case 0x1405:
        return qgpu_f2u((float)((double)qgpu_ld32(p) / 4294967295.0));
    case 0x1403:
        return qgpu_f2u((float)ld16(p) / 65535.0f);
    }
    return 0;
}

/* ── S3TC (EXT_texture_compression_s3tc) ──────────────────────────────────────
 * Les blocs sont définis OCTET PAR OCTET, en petit-boutiste : l'invité les
 * recopie tels que l'application les a donnés, sans rien échanger. Les
 * couleurs intermédiaires sont arrondies au plus proche ; la spécification
 * laisse l'arrondi libre, et le cœur décode pour tous les backends — ils
 * voient donc exactement les mêmes texels. */
static inline uint32_t le16(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8); }

static void dxt_colors(const uint8_t *b, bool four, bool punch, uint32_t out[16])
{
    uint32_t c0 = le16(b), c1 = le16(b + 2), bits, i;
    uint32_t r[4], g[4], bl[4], a[4] = { 255, 255, 255, 255 };

    r[0] = x5(c0 >> 11); g[0] = x6((c0 >> 5) & 63); bl[0] = x5(c0 & 31);
    r[1] = x5(c1 >> 11); g[1] = x6((c1 >> 5) & 63); bl[1] = x5(c1 & 31);
    if (four || c0 > c1) {
        r[2] = (2 * r[0] + r[1] + 1) / 3; g[2] = (2 * g[0] + g[1] + 1) / 3;
        bl[2] = (2 * bl[0] + bl[1] + 1) / 3;
        r[3] = (r[0] + 2 * r[1] + 1) / 3; g[3] = (g[0] + 2 * g[1] + 1) / 3;
        bl[3] = (bl[0] + 2 * bl[1] + 1) / 3;
    } else {
        r[2] = (r[0] + r[1] + 1) / 2; g[2] = (g[0] + g[1] + 1) / 2;
        bl[2] = (bl[0] + bl[1] + 1) / 2;
        r[3] = g[3] = bl[3] = 0;
        a[3] = punch ? 0 : 255;                          /* noir transparent en RGBA */
    }
    bits = b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    for (i = 0; i < 16; i++) {
        uint32_t k = (bits >> (2 * i)) & 3;
        out[i] = argb(a[k], r[k], g[k], bl[k]);
    }
}

/* Décode un bloc de 4×4 texels, rangés ligne par ligne. */
static void dxt_block(uint32_t fmt, const uint8_t *b, uint32_t out[16])
{
    uint32_t i;

    if (fmt == QGPU_TF_DXT1_RGB || fmt == QGPU_TF_DXT1_RGBA) {
        dxt_colors(b, false, fmt == QGPU_TF_DXT1_RGBA, out);
        return;
    }
    dxt_colors(b + 8, true, false, out);                 /* DXT3/5 : toujours 4 couleurs */
    if (fmt == QGPU_TF_DXT3) {
        for (i = 0; i < 16; i++) {
            uint32_t al = (b[i / 2] >> (4 * (i & 1))) & 15;
            out[i] = (out[i] & 0xFFFFFF) | ((al * 17) << 24);
        }
    } else {
        uint32_t a0 = b[0], a1 = b[1], al[8], k;
        uint64_t bits = 0;
        al[0] = a0; al[1] = a1;
        if (a0 > a1) {
            for (k = 1; k < 7; k++) {
                al[k + 1] = ((7 - k) * a0 + k * a1 + 3) / 7;
            }
        } else {
            for (k = 1; k < 5; k++) {
                al[k + 1] = ((5 - k) * a0 + k * a1 + 2) / 5;
            }
            al[6] = 0; al[7] = 255;
        }
        for (k = 0; k < 6; k++) {
            bits |= (uint64_t)b[2 + k] << (8 * k);
        }
        for (i = 0; i < 16; i++) {
            out[i] = (out[i] & 0xFFFFFF) | (al[(bits >> (3 * i)) & 7] << 24);
        }
    }
}

/* Taille en octets des données d'une boîte w×h×d, et validation des pas. */
static bool tex_src_size(const TexSrc *s, uint32_t w, uint32_t h, uint32_t d,
                         uint32_t *row, uint32_t *img, uint64_t *total)
{
    uint64_t line;

    if (s->block) {
        *row = *img = 0;
        *total = (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * s->block * d;
        return true;
    }
    line = (uint64_t)w * s->bpp;
    if (*row == 0) {
        *row = (uint32_t)line;
    }
    if (*row < line) {
        return false;
    }
    if (*img == 0) {
        *img = *row * h;
    }
    if (*img < (uint64_t)*row * (h - 1) + line) {
        return false;
    }
    *total = (uint64_t)*img * (d - 1) + (uint64_t)*row * (h - 1) + line;
    return true;
}

/* Recopie une boîte de données (fenêtre partagée) dans un niveau déjà alloué. */
static void tex_store(QgpuTexLevel *lv, const TexSrc *s, const uint8_t *src,
                      uint32_t x, uint32_t y, uint32_t z,
                      uint32_t w, uint32_t h, uint32_t d, uint32_t row, uint32_t img)
{
    uint32_t i, j, k;

    if (s->block) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bx, by, blk[16];
        for (by = 0; by < bh; by++) {
            for (bx = 0; bx < bw; bx++) {
                dxt_block(s->fmt, src + ((size_t)by * bw + bx) * s->block, blk);
                for (j = 0; j < 4 && by * 4 + j < h; j++) {
                    for (i = 0; i < 4 && bx * 4 + i < w; i++) {
                        lv->px[((size_t)(y + by * 4 + j)) * lv->w + x + bx * 4 + i] =
                            blk[j * 4 + i];
                    }
                }
            }
        }
        return;
    }
    for (k = 0; k < d; k++) {
        for (j = 0; j < h; j++) {
            const uint8_t *p = src + (size_t)k * img + (size_t)j * row;
            uint32_t *o = lv->px + ((size_t)(z + k) * lv->h + y + j) * lv->w + x;
            for (i = 0; i < w; i++, p += s->bpp) {
                o[i] = unpack_texel(s, p);
            }
        }
    }
}

/* Alloue (ou réemploie) un niveau de w×h×d texels. */
static bool tex_alloc_level(QgpuTexLevel *lv, uint32_t w, uint32_t h, uint32_t d,
                            uint32_t fmt, bool zero)
{
    size_t n = (size_t)w * h * d;
    uint32_t *px = lv->px;

    if (!px || (size_t)lv->w * lv->h * lv->d != n) {
        px = malloc(n * sizeof(uint32_t));
        if (!px) {
            return false;
        }
        free(lv->px);
    }
    if (zero) {
        memset(px, 0, n * sizeof(uint32_t));
    }
    lv->px = px;
    lv->w = w; lv->h = h; lv->d = d;
    lv->fmt = fmt;
    return true;
}

/* Mipmaps automatiques (OpenGL 1.4) d'une face : niveaux b+1..q recalculés
   depuis le niveau de base, moyenne des blocs de 2×2 (2×2×2 en 3D) du niveau
   précédent. Une dimension impaire perd sa dernière rangée (liberté laissée par
   la spécification) ; une dimension de 1 ne se moyenne pas. */
static bool tex_gen_mipmaps(QgpuTexture *t, uint32_t face)
{
    const QgpuTexLevel *base = &t->level[face][t->base_level];
    bool depth = base->fmt == 0x1902;
    uint32_t q, n;

    if (t->target == QGPU_TT_RECTANGLE || !base->px) {
        return true;
    }
    q = tex_last_level(t, base);
    if (q > QGPU_MAX_TEX_LEVELS - 1) {
        q = QGPU_MAX_TEX_LEVELS - 1;
    }
    for (n = t->base_level + 1; n <= q; n++) {
        const QgpuTexLevel *src = &t->level[face][n - 1];
        QgpuTexLevel *dst = &t->level[face][n];
        uint32_t w = src->w > 1 ? src->w / 2 : 1, h = src->h > 1 ? src->h / 2 : 1;
        uint32_t d = src->d > 1 ? src->d / 2 : 1;
        uint32_t sx = src->w > 1 ? 2 : 1, sy = src->h > 1 ? 2 : 1, sz = src->d > 1 ? 2 : 1;
        uint32_t x, y, z, i, j, k;

        if (!tex_alloc_level(dst, w, h, d, base->fmt, false)) {
            return false;
        }
        for (z = 0; z < d; z++) {
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    uint32_t cnt = sx * sy * sz, acc[4] = { 0, 0, 0, 0 }, ch;
                    float fd = 0.0f;
                    for (k = 0; k < sz; k++) {
                        for (j = 0; j < sy; j++) {
                            for (i = 0; i < sx; i++) {
                                uint32_t p = src->px[((size_t)(z * sz + k) * src->h +
                                                      y * sy + j) * src->w + x * sx + i];
                                if (depth) {
                                    fd += qgpu_u2f(p);
                                } else {
                                    for (ch = 0; ch < 4; ch++) {
                                        acc[ch] += (p >> (8 * ch)) & 255;
                                    }
                                }
                            }
                        }
                    }
                    dst->px[((size_t)z * h + y) * w + x] = depth
                        ? qgpu_f2u(fd / (float)cnt)
                        : ((acc[0] + cnt / 2) / cnt) | (((acc[1] + cnt / 2) / cnt) << 8) |
                          (((acc[2] + cnt / 2) / cnt) << 16) | (((acc[3] + cnt / 2) / cnt) << 24);
                }
            }
        }
        t->dirty[face] |= 1u << n;
    }
    return true;
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
    /* v8 : facteurs à couleur constante (GL 1.4). */
    case QGPU_BF_CONSTANT_COLOR: case QGPU_BF_ONE_MINUS_CONSTANT_COLOR:
    case QGPU_BF_CONSTANT_ALPHA: case QGPU_BF_ONE_MINUS_CONSTANT_ALPHA:
        return true;
    default:
        return false;
    }
}

/* v8 : les 16 opérations logiques d'OpenGL sont contiguës. */
static bool valid_logic_op(uint32_t o)
{
    return o >= QGPU_LO_CLEAR && o <= QGPU_LO_SET;
}

static bool valid_polygon_mode(uint32_t m)
{
    return m == QGPU_POLY_POINT || m == QGPU_POLY_LINE || m == QGPU_POLY_FILL;
}

static bool valid_stencil_op(uint32_t o)
{
    switch (o) {
    case QGPU_SOP_ZERO: case QGPU_SOP_INVERT: case QGPU_SOP_KEEP:
    case QGPU_SOP_REPLACE: case QGPU_SOP_INCR: case QGPU_SOP_DECR:
    case QGPU_SOP_INCR_WRAP: case QGPU_SOP_DECR_WRAP:
        return true;
    default:
        return false;
    }
}

static bool valid_blend_eq(uint32_t e)
{
    /* v8 : GL_MIN et GL_MAX s'ajoutent aux trois de la v2. */
    return e == QGPU_BEQ_ADD || e == QGPU_BEQ_SUBTRACT ||
           e == QGPU_BEQ_REVERSE_SUBTRACT ||
           e == QGPU_BEQ_MIN || e == QGPU_BEQ_MAX;
}

/* v7 : GL_FRONT, GL_BACK, GL_FRONT_AND_BACK. */
static bool valid_face(uint32_t f)
{
    return f == 0x0404 || f == 0x0405 || f == 0x0408;
}

/* v7 : un flottant du flux. Même règle que pour les sommets : ni NaN ni
   infini ni valeur démesurée, pour que les backends n'en voient jamais. */
static bool read_f(const uint32_t *a, int n, float *out)
{
    int i;
    for (i = 0; i < n; i++) {
        float v = qgpu_u2f(a[i]);
        if (v != v || v > 1e9f || v < -1e9f) {
            return false;
        }
        out[i] = v;
    }
    return true;
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
    case QGPU_SK_UNIT(4): case QGPU_SK_UNIT(5): case QGPU_SK_UNIT(6): case QGPU_SK_UNIT(7):
        return val <= 1;
    case QGPU_SK_TEX_BIND: case QGPU_SK_TEX1_BIND: case QGPU_SK_TEX2_BIND:
    case QGPU_SK_TEX3_BIND:
    case QGPU_SK_UNIT(4) + QGPU_SK_U_BIND: case QGPU_SK_UNIT(5) + QGPU_SK_U_BIND:
    case QGPU_SK_UNIT(6) + QGPU_SK_U_BIND: case QGPU_SK_UNIT(7) + QGPU_SK_U_BIND:
        return val < QGPU_MAX_TEX;
    case QGPU_SK_TEX_ENV_MODE: case QGPU_SK_TEX1_ENV_MODE: case QGPU_SK_TEX2_ENV_MODE:
    case QGPU_SK_TEX3_ENV_MODE:
    case QGPU_SK_UNIT(4) + QGPU_SK_U_ENV_MODE: case QGPU_SK_UNIT(5) + QGPU_SK_U_ENV_MODE:
    case QGPU_SK_UNIT(6) + QGPU_SK_U_ENV_MODE: case QGPU_SK_UNIT(7) + QGPU_SK_U_ENV_MODE:
        return valid_env_mode(val);
    case QGPU_SK_TEX_ENV_COLOR: case QGPU_SK_TEX1_ENV_COLOR: case QGPU_SK_TEX2_ENV_COLOR:
    case QGPU_SK_TEX3_ENV_COLOR: case QGPU_SK_FOG_COLOR:
    case QGPU_SK_UNIT(4) + QGPU_SK_U_ENV_COLOR: case QGPU_SK_UNIT(5) + QGPU_SK_U_ENV_COLOR:
    case QGPU_SK_UNIT(6) + QGPU_SK_U_ENV_COLOR: case QGPU_SK_UNIT(7) + QGPU_SK_U_ENV_COLOR:
        return true;
    case QGPU_SK_COMBINE0: case QGPU_SK_COMBINE0 + 1:
    case QGPU_SK_COMBINE0 + 2: case QGPU_SK_COMBINE0 + 3:
    case QGPU_SK_COMBINE4: case QGPU_SK_COMBINE4 + 1:
    case QGPU_SK_COMBINE4 + 2: case QGPU_SK_COMBINE4 + 3:
        return valid_combine(val);
    case QGPU_SK_COMBINE_SRC0: case QGPU_SK_COMBINE_SRC0 + 1:
    case QGPU_SK_COMBINE_SRC0 + 2: case QGPU_SK_COMBINE_SRC0 + 3:
    case QGPU_SK_COMBINE_SRC4: case QGPU_SK_COMBINE_SRC4 + 1:
    case QGPU_SK_COMBINE_SRC4 + 2: case QGPU_SK_COMBINE_SRC4 + 3:
        return valid_combine_src(val);
    case QGPU_SK_LINE_WIDTH: case QGPU_SK_POINT_SIZE: {
        float f = qgpu_u2f(val);
        return f > 0.0f && f <= 64.0f;
    }
    case QGPU_SK_STENCIL_TEST:
        return val <= 1;
    case QGPU_SK_STENCIL_FUNC:
        return valid_func(val);
    case QGPU_SK_STENCIL_REF: case QGPU_SK_STENCIL_VALUE_MASK:
    case QGPU_SK_STENCIL_WRITE_MASK: case QGPU_SK_STENCIL_CLEAR:
        return val <= 255;                           /* stencil de 8 bits */
    case QGPU_SK_STENCIL_OP_FAIL: case QGPU_SK_STENCIL_OP_ZFAIL:
    case QGPU_SK_STENCIL_OP_ZPASS:
        return valid_stencil_op(val);
    case QGPU_SK_POLY_FACTOR: case QGPU_SK_POLY_UNITS: {
        /* borne INCLUSIVE : le plugin sature à ±POLY_OFFSET_MAX < 1e6, et un
           « < 1e6 » strict face à un plugin qui saturait à 1e6 refusait le
           SET_STATE (soumission perdue, image noire — scène `offset`, 22/09). */
        float f = qgpu_u2f(val);
        return f == f && f >= -1e6f && f <= 1e6f;
    }
    /* v7 */
    case QGPU_SK_LIGHTING: case QGPU_SK_NORMALIZE: case QGPU_SK_RESCALE_NORMAL:
    case QGPU_SK_CULL_FACE: case QGPU_SK_COLOR_MATERIAL:
    case QGPU_SK_LOCAL_VIEWER: case QGPU_SK_TWO_SIDE:
        return val <= 1;
    case QGPU_SK_SHADE_MODEL:
        return val == 0x1D00 || val == 0x1D01;       /* GL_FLAT, GL_SMOOTH */
    case QGPU_SK_CULL_MODE: case QGPU_SK_COLOR_MAT_FACE:
        return valid_face(val);
    case QGPU_SK_FRONT_FACE:
        return val == 0x0900 || val == 0x0901;       /* GL_CW, GL_CCW */
    case QGPU_SK_COLOR_MAT_MODE:
        return val == 0x1600 || val == 0x1200 || val == 0x1201 ||
               val == 0x1202 || val == 0x1602;
    case QGPU_SK_COLOR_CONTROL:
        return val == 0x81F9 || val == 0x81FA;
    case QGPU_SK_FOG_MODE:
        return val == QGPU_FOG_VERTEX || val == QGPU_FOG_LINEAR ||
               val == QGPU_FOG_EXP || val == QGPU_FOG_EXP2;
    case QGPU_SK_FOG_DENSITY: {
        float f = qgpu_u2f(val);
        return f == f && f >= 0.0f && f < 1e9f;      /* GL : densité positive */
    }
    case QGPU_SK_FOG_START: case QGPU_SK_FOG_END: {
        float f = qgpu_u2f(val);
        return f == f && f > -1e9f && f < 1e9f;
    }
    /* v8 */
    case QGPU_SK_BLEND_COLOR:
        return true;                                 /* 0xAARRGGBB, tout est valide */
    case QGPU_SK_LOGIC_OP: case QGPU_SK_POLYGON_STIPPLE:
    case QGPU_SK_POLY_OFFSET_LINE: case QGPU_SK_POLY_OFFSET_POINT:
    case QGPU_SK_LINE_STIPPLE:
        return val <= 1;
    case QGPU_SK_LOGIC_OP_MODE:
        return valid_logic_op(val);
    case QGPU_SK_POLYGON_MODE_FRONT: case QGPU_SK_POLYGON_MODE_BACK:
        return valid_polygon_mode(val);
    case QGPU_SK_LINE_STIPPLE_FACTOR:
        return val >= 1 && val <= 256;               /* borne d'OpenGL */
    case QGPU_SK_LINE_STIPPLE_PATTERN:
        return val <= 0xFFFF;
    /* v10 : biais de LOD d'unité (initial 0.0, donc neutre pour un flux v9) */
    case QGPU_SK_TEX_LOD_BIAS0: case QGPU_SK_TEX_LOD_BIAS0 + 1:
    case QGPU_SK_TEX_LOD_BIAS0 + 2: case QGPU_SK_TEX_LOD_BIAS0 + 3:
    case QGPU_SK_TEX_LOD_BIAS4: case QGPU_SK_TEX_LOD_BIAS4 + 1:
    case QGPU_SK_TEX_LOD_BIAS4 + 2: case QGPU_SK_TEX_LOD_BIAS4 + 3:
        return finite_f(val, QGPU_MAX_LOD_BIAS);
    case QGPU_SK_COLOR_SUM:
        return val <= QGPU_CSUM_FORMAT;
    case QGPU_SK_POINT_SIZE_MIN:
        return finite_f(val, 64.0f) && qgpu_u2f(val) >= 0.0f;
    case QGPU_SK_POINT_SIZE_MAX:
        return finite_f(val, 64.0f) && qgpu_u2f(val) > 0.0f;
    case QGPU_SK_POINT_FADE:
    case QGPU_SK_POINT_ATT_CONST: case QGPU_SK_POINT_ATT_LINEAR:
    case QGPU_SK_POINT_ATT_QUAD:
        return finite_f(val, 1e9f) && qgpu_u2f(val) >= 0.0f;
    case QGPU_SK_VERTEX_PROGRAM: case QGPU_SK_FRAGMENT_PROGRAM:     /* v16 */
        return val <= 1;
    case QGPU_SK_GEN_SIZES:          /* QGPU_CAP_GEN_SIZES : 16 × 2 bits, tous valides */
        return true;
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
        c->ctx[i].query = -1;
        qgpu_state_init(&c->ctx[i].st);
        qgpu_geom_init(&c->ctx[i].gm);
        qgpu_stipple_init(&c->ctx[i].stip);
        prog_set_init(&c->ctx[i].prg);
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
    if (c->be) {
        /* init() a pu ajouter des bits à chaud (v8 : QGPU_CAP_OCCLUSION) ;
           ceux du backend sont acquis d'office. */
        c->caps |= c->be->cap;
        /* Génériques à taille déclarée : c'est le cœur qui complète les
           composantes (conv_raw_vertex_gs), tout backend à programmes les
           tient donc. */
        if (c->caps & QGPU_CAP_PROGRAMS) {
            c->caps |= QGPU_CAP_GEN_SIZES;
        }
        /* v18 : DRAW_NATIVE est converti par le cœur vers la forme de
           DRAW_RAW ; tout backend qui dessine en brut le tient. */
        if (c->be->draw_raw) {
            c->caps |= QGPU_CAP_NATIVE;
        }
    } else {
        c->caps = 0;
    }
    return c->be != NULL;
}

void qgpu_core_set_scanout(QgpuCore *c, uint8_t *ram, uint32_t size,
                           void (*dirty)(void *opaque, uint32_t off,
                                         uint32_t len),
                           void *opaque)
{
    c->scanout = ram;
    c->scanout_size = ram ? size : 0;
    c->scanout_dirty = ram ? dirty : NULL;
    c->scanout_opaque = ram ? opaque : NULL;
    /* la géométrie va avec la cible : une nouvelle cible sans géométrie ne
       doit pas hériter du pas de l'ancienne */
    c->scanout_stride = c->scanout_width = 0;
    c->scanout_height = c->scanout_depth = 0;
}

void qgpu_core_set_scanout_geom(QgpuCore *c, uint32_t stride, uint32_t width,
                                uint32_t height, uint32_t depth)
{
    if (!c->scanout) {
        stride = width = height = depth = 0;
    }
    c->scanout_stride = stride;
    c->scanout_width  = width;
    c->scanout_height = height;
    c->scanout_depth  = depth;
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
        prog_set_free(c, &c->ctx[i].prg);          /* v16 : avant le memset */
        memset(&c->ctx[i], 0, sizeof(c->ctx[i]));
        c->ctx[i].surf = -1;
        c->ctx[i].query = -1;
        qgpu_state_init(&c->ctx[i].st);
        qgpu_geom_init(&c->ctx[i].gm);
        qgpu_stipple_init(&c->ctx[i].stip);
        prog_set_init(&c->ctx[i].prg);
    }
    for (i = 0; i < QGPU_MAX_TEX; i++) {
        if (c->tex[i].used) {
            tex_free(c, &c->tex[i]);
        }
    }
    for (i = 0; i < QGPU_MAX_QUERIES; i++) {         /* v8 */
        if (c->query[i].used && c->be->query_destroy) {
            c->be->query_destroy(c, &c->query[i]);
        }
        memset(&c->query[i], 0, sizeof(c->query[i]));
    }
    for (i = 0; i < QGPU_MAX_BUF; i++) {             /* v14 */
        free(c->buf[i].data);
        memset(&c->buf[i], 0, sizeof(c->buf[i]));
    }
    c->cur_stip = NULL;
    c->cur_query = NULL;
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
    free(c->sbuf); c->sbuf = NULL; c->sbuf_cap = 0;
    free(c->ibuf); c->ibuf = NULL; c->ibuf_cap = 0;
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

/* Valide un transfert surface ↔ BAR0 : [surf, off, stride, x, y, w, h].
   bpp = 4 (xRGB / float32 / stencil) ou 2 (1555 / UNORM16). */
static uint32_t check_xfer(QgpuCore *c, const uint32_t *a, QgpuSurface **sp,
                           uint32_t bpp)
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
    if (bpp != 2 && bpp != 4) {
        return QGPU_ST_BAD_ARG;
    }
    if ((bpp == 4 && (stride & 3)) || (bpp == 2 && (stride & 1)) ||
        stride < (uint64_t)w * bpp) {
        return QGPU_ST_BAD_ARG;
    }
    if (!in_shmem(c, off, (uint64_t)(h - 1) * stride + (uint64_t)w * bpp)) {
        return QGPU_ST_OOB;
    }
    *sp = s;
    return QGPU_ST_OK;
}

static uint16_t pack_rgb1555(uint32_t p)
{
    return (uint16_t)(((p >> 9) & 0x7c00u) |
                      ((p >> 6) & 0x03e0u) |
                      ((p >> 3) & 0x001fu));
}

/* H3 : le bit 15 de RGB1555 n'est PAS un alpha de transfert (pack_rgb1555 le
   jette), et la surface est en XRGB8888 : un pixel envoyé en « milliers de
   couleurs » doit arriver OPAQUE. Sans ce 0xFF, toute la surface devenait
   transparente — DST_ALPHA/ONE_MINUS_DST_ALPHA inversés, COPY_TEX invisible,
   relecture en 00RRGGBB. */
static uint32_t unpack_rgb1555(uint16_t pix)
{
    uint32_t r = (pix >> 10) & 31u;
    uint32_t g = (pix >> 5) & 31u;
    uint32_t b = pix & 31u;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint16_t pack_unorm16(float d)
{
    if (!(d > 0.0f)) {
        return 0;
    }
    if (d >= 1.0f) {
        return 65535u;
    }
    return (uint16_t)(d * 65535.0f + 0.5f);
}

static float unpack_unorm16(uint16_t z)
{
    return (float)z / 65535.0f;
}

/* ── H4 : sommets malsains — assainir, plus jamais refuser ──────────────────
 *
 * Un flux invité contient des NaN, des infinis et des valeurs démesurées dès
 * qu'un tampon de sommets est plus grand que ce que l'application remplit
 * (queue jamais écrite d'un VBO), ou qu'un calcul du jeu a divisé par zéro.
 * Le cœur répondait alors QGPU_ST_BAD_ARG, et la boucle d'exécution
 * ABANDONNAIT la fin de la soumission : le CLEAR, le SURF_PRESENT et tout
 * l'état qui suivaient étaient perdus. Ce n'était donc pas « un triangle
 * manquant » mais une image entière figée ou déchirée.
 *
 * On assainit maintenant chaque mot : NaN → 0, et SATURATION à ±QGPU_COORD_MAX
 * au lieu d'un rejet. La borne ne juge pas le flux — elle protège seulement
 * les conversions en entier du rasteriseur logiciel (bornes de boîte
 * englobante, index de texel) ; une coordonnée de texture au-delà n'est pas
 * une faute, et la ramener sur la borne ne change rien de visible là où
 * REPEAT a déjà tout replié. */
#define QGPU_COORD_MAX  1e9f

static inline float sane_coord(float v)
{
    if (v != v) {                                /* NaN */
        return 0.0f;
    }
    if (v > QGPU_COORD_MAX) {                    /* +inf compris */
        return QGPU_COORD_MAX;
    }
    if (v < -QGPU_COORD_MAX) {
        return -QGPU_COORD_MAX;
    }
    return v;
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

/* v16 : sous un programme de fragments, texture[u] est la texture LIÉE à
   l'unité, que l'unité soit allumée ou non (Direct3D n'allume rien). */
static QgpuTexture *unit_texture_bound(QgpuCore *c, const QgpuState *st, int u)
{
    QgpuTexture *t = &c->tex[st->v[QGPU_SK_UNIT(u) + QGPU_SK_U_BIND]];
    return qgpu_texture_levels(t) ? t : NULL;
}

/* v8 : ce qu'un dessin doit voir du contexte courant en plus de l'état GL —
   le motif de pointillé, et la requête d'occlusion ouverte s'il y en a une.
   Posé ici pour qu'un backend n'ait jamais à remonter au contexte. */
static void arm_draw(QgpuCore *c)
{
    QgpuContext *cx = &c->ctx[c->cur_ctx];
    c->cur_stip = &cx->stip;
    c->cur_query = (cx->query >= 0) ? &c->query[cx->query] : NULL;
    c->cur_sec = -1;
    c->cur_prg = &cx->prg;                           /* v16 */
}

/* Commun aux opcodes de dessin : a = [nverts, off] ; ntex unités texturées
   (coordonnées dans les sommets). */
static uint32_t do_draw(QgpuCore *c, const uint32_t *a, uint32_t prim,
                        uint32_t words, int ntex, bool sec)
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
        /* Ces opcodes donnent déjà des coordonnées FENÊTRE : pas de division
           perspective par la position, donc rien d'autre à garder que la
           finitude (cf. sane_coord). */
        c->vbuf[i] = sane_coord(qgpu_u2f(qgpu_ld32(c->shmem + off + i * 4)));
    }
    cs = cur_state(c);
    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        tex[u] = u < ntex ? unit_texture(c, cs, u) : NULL;
    }
    arm_draw(c);
    if (sec) {
        /* v11 : couleur secondaire après les coordonnées, bornée comme la
           primaire (le backend de référence l'ajoute telle quelle) */
        c->cur_sec = (int32_t)QGPU_VERTEX_TEXN_WORDS(ntex);
        for (i = 0; i < nverts; i++) {
            float *sc = c->vbuf + i * words + c->cur_sec;
            int k;
            for (k = 0; k < 3; k++) {
                sc[k] = sc[k] < 0.0f ? 0.0f : sc[k] > 1.0f ? 1.0f : sc[k];
            }
        }
    }
    if (!c->be->draw(c, s, cs, prim, tex, c->vbuf, nverts, words)) {
        return QGPU_ST_BACKEND;
    }
    return QGPU_ST_OK;
}

static QgpuGeom *cur_geom(QgpuCore *c)
{
    return &c->ctx[c->cur_ctx].gm;
}

/* v7 : DRAW_RAW [mode, n, voff, pas, format, ioff, itype, premier, nverts].
   Toute la validation est ici : format, pas, bornes de BAR0, indices bornés
   au nombre de sommets déclaré, NaN. Les sommets sont remis à plat (serrés,
   hôte-natifs) et les indices élargis en uint32_t, pour qu'un backend n'ait
   plus rien à vérifier ni à décoder.
   v14 : `vbuf`/`ibuf` = QGPU_BUF_SHMEM → offset dans BAR0, sinon offset
   dans ce tampon hôte. */
static const uint8_t *src_bytes(QgpuCore *c, uint32_t buf, uint32_t off,
                                uint64_t need, uint32_t *st)
{
    if (buf == QGPU_BUF_SHMEM) {
        if (!in_shmem(c, off, need)) {
            *st = QGPU_ST_OOB;
            return NULL;
        }
        *st = QGPU_ST_OK;
        return c->shmem + off;
    }
    if (buf >= QGPU_MAX_BUF || !c->buf[buf].used || !c->buf[buf].data) {
        *st = QGPU_ST_BAD_ARG;
        return NULL;
    }
    if ((uint64_t)off + need > c->buf[buf].size) {
        *st = QGPU_ST_OOB;
        return NULL;
    }
    *st = QGPU_ST_OK;
    return c->buf[buf].data + off;
}

/* Un sommet du chemin brut : mots big-endian → flottants hôte assainis. */
static void conv_raw_vertex(QgpuCore *c, const uint8_t *vbase, uint32_t stride,
                            uint32_t words, uint32_t i)
{
    const uint8_t *p = vbase + (size_t)i * stride * 4;
    float *d = c->vbuf + (size_t)i * words;
    uint32_t j;

    for (j = 0; j < words; j++) {
        d[j] = sane_coord(qgpu_u2f(qgpu_ld32(p + j * 4)));
    }
}

/* QGPU_CAP_GEN_SIZES : le même sommet quand des génériques sont déclarés plus
   courts que 4. `pre` mots avant les génériques sont recopiés tels quels,
   puis chaque générique présent (gn[g] composantes sur le fil) est complété
   à 4 comme OpenGL : y = 0, z = 0, w = 1. Le backend reçoit ainsi exactement
   la forme serrée QGPU_VF_WORDS(fmt) d'avant la clé. */
static void conv_raw_vertex_gs(QgpuCore *c, const uint8_t *vbase, uint32_t stride,
                               uint32_t words, uint32_t i, uint32_t pre,
                               const uint8_t *gn, uint32_t ng)
{
    const uint8_t *p = vbase + (size_t)i * stride * 4;
    float *d = c->vbuf + (size_t)i * words;
    uint32_t j, g;

    for (j = 0; j < pre; j++) {
        d[j] = sane_coord(qgpu_u2f(qgpu_ld32(p + j * 4)));
    }
    p += (size_t)pre * 4;
    d += pre;
    for (g = 0; g < ng; g++) {
        uint32_t n = gn[g];
        for (j = 0; j < n; j++) {
            d[j] = sane_coord(qgpu_u2f(qgpu_ld32(p + j * 4)));
        }
        for (; j < 4; j++) {
            d[j] = j == 3 ? 1.0f : 0.0f;
        }
        p += (size_t)n * 4;
        d += 4;
    }
}

/* Position utilisable ? w ≈ 0 ne l'est pas : l'étage géométrique divise par w,
   et un w sous-normal donne un triangle géant (le ciel de Colin McRae) puis,
   après (int)ceilf(inf), un comportement indéfini qui ne rend pas la même
   image d'un hôte à l'autre. Le plugin jette déjà ces sommets sur le G4 ;
   l'hôte doit le faire aussi, car les sommets d'un VBO v14 ne passent jamais
   par lui. */
static bool raw_pos_usable(const QgpuCore *c, uint32_t words, uint32_t fmt, uint32_t i)
{
    const float *d = c->vbuf + (size_t)i * words;

    return QGPU_VF_POS_COUNT(fmt) != 4 || d[3] > 1e-6f || d[3] < -1e-6f;
}

/* Fin commune de DRAW_RAW, DRAW_RAW_BUF et (v18) DRAW_NATIVE : c->vbuf porte
   nverts sommets à plat (`words` flottants hôte assainis, forme QGPU_VF_WORDS
   de `fmt`), c->ibuf les `count` indices (< nverts) quand itype ≠ AUCUN ;
   [lo, hi] est la plage des sommets dessinés sans indices. Programmes
   cassés, test w ≈ 0, textures, backend : rien ici ne dépend de la façon
   dont les sommets sont arrivés. */
static uint32_t raw_finish(QgpuCore *c, QgpuSurface *s, uint32_t mode, uint32_t fmt,
                           uint32_t words, uint32_t nverts, uint32_t itype,
                           uint32_t count, uint32_t first, uint32_t lo, uint32_t hi)
{
    QgpuTexture *tex[QGPU_MAX_UNITS];
    QgpuState *cs;
    bool vp_on, fp_on;
    uint32_t i, j;
    int u;

    cs = cur_state(c);
    /* v16 : un programme lié, actif et CASSÉ (texte refusé par l'hôte) jette
       le dessin — BAD_ARG non fatal, comme un dessin mal formé. */
    {
        QgpuProgSet *pg = &c->ctx[c->cur_ctx].prg;
        int w;
        for (w = 0; w < 2; w++) {
            uint32_t key = w == QGPU_PROG_VP ? QGPU_SK_VERTEX_PROGRAM
                                             : QGPU_SK_FRAGMENT_PROGRAM;
            if (cs->v[key] && pg->bound[w] >= 0 && pg->prog[pg->bound[w]].broken) {
                TRACE(c, "  dessin brut jeté : programme %d cassé", pg->bound[w]);
                return QGPU_ST_BAD_ARG;
            }
        }
        vp_on = qgpu_prog_active(cs, pg, QGPU_PROG_VP) != NULL;
        fp_on = qgpu_prog_active(cs, pg, QGPU_PROG_FP) != NULL;
    }
    j = 0;                                       /* sommet inutilisable vu ? */
    /* Sous un programme de sommets, la position du sommet n'est pas la position
       de découpe : le test w ≈ 0 (H4) n'a pas de sens, le programme décide. */
    if (vp_on) {
        /* rien */
    } else if (itype != QGPU_IDX_NONE) {
        for (i = 0; i < count; i++) {
            j |= !raw_pos_usable(c, words, fmt, c->ibuf[i]);
        }
    } else {
        for (i = lo; i <= hi; i++) {
            j |= !raw_pos_usable(c, words, fmt, i);
        }
    }
    if (j) {
        /* Jeter la PRIMITIVE fautive demanderait de reconstruire la topologie
           des dix modes ; on jette ce dessin-là, et surtout on rend OK pour
           que la suite de la soumission — état, effacement, présentation —
           s'exécute. C'est tout ce que H4 coûtait vraiment. */
        TRACE(c, "  dessin brut jeté : position à w ≈ 0");
        return QGPU_ST_OK;
    }
    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        tex[u] = fp_on ? unit_texture_bound(c, cs, u) : unit_texture(c, cs, u);
    }
    if (!c->be->draw_raw) {
        return QGPU_ST_BACKEND;
    }
    arm_draw(c);
    if (!c->be->draw_raw(c, s, cs, cur_geom(c), tex, mode, fmt, c->vbuf, nverts,
                         words, itype == QGPU_IDX_NONE ? NULL : c->ibuf,
                         count, first)) {
        return QGPU_ST_BACKEND;
    }
    return QGPU_ST_OK;
}

static uint32_t do_draw_raw(QgpuCore *c, const uint32_t *a, uint32_t vbuf, uint32_t ibuf, int raw_buf)
{
    uint32_t mode = a[0], count = a[1], voff, stride, fmt, ioff, itype, first, nverts;
    uint32_t words, swords, gs, pre = 0, ng = 0, i, st, lo, hi;
    uint8_t gn[QGPU_VF_GEN_MAX];
    bool dense;
    QgpuSurface *s = bound_surface(c, &st);
    const uint8_t *vbase, *ibase;

    if (raw_buf) {
        /* DRAW_RAW_BUF : [mode, n, vbuf, voff, pas, format, ibuf, ioff, itype, premier, nverts] */
        voff = a[3]; stride = a[4]; fmt = a[5];
        ioff = a[7]; itype = a[8]; first = a[9]; nverts = a[10];
    } else {
        voff = a[2]; stride = a[3]; fmt = a[4];
        ioff = a[5]; itype = a[6]; first = a[7]; nverts = a[8];
    }

    if (!s) {
        return st;
    }
    if (mode > QGPU_PRIM_MODE_POLYGON || itype > QGPU_IDX_U32) {
        return QGPU_ST_BAD_ARG;
    }
    if ((fmt & ~(uint32_t)QGPU_VF_ALL) || (fmt & QGPU_VF_POS_MASK) == 3) {
        return QGPU_ST_BAD_ARG;
    }
    /* v16 : des attributs génériques sans backend capable — un flux v16 sur
       un backend logiciel — sont un dessin refusé, pas une panne. */
    if ((fmt & QGPU_VF_GEN_MASK) && !(c->caps & QGPU_CAP_PROGRAMS)) {
        return QGPU_ST_BAD_ARG;
    }
    words = (uint32_t)QGPU_VF_WORDS(fmt);
    /* QGPU_CAP_GEN_SIZES : `words` est la forme remise au backend (4 flottants
       par générique), `swords` celle du fil. Seuls les codes des génériques
       PRÉSENTS comptent ; clé à 0 (tout flux qui ne la pose pas) : swords =
       words, chemin d'avant. */
    gs = cur_state(c)->v[QGPU_SK_GEN_SIZES];
    if (fmt & QGPU_VF_GEN_MASK) {
        uint32_t k, used = 0;
        for (k = 0; k < QGPU_VF_GEN_MAX; k++) {
            if (fmt & (uint32_t)QGPU_VF_GEN(k)) {
                used |= (uint32_t)QGPU_GS_FIELD(k);
            }
        }
        gs &= used;
    } else {
        gs = 0;
    }
    swords = (uint32_t)QGPU_VF_WORDS_GS(fmt, gs);
    if (gs) {
        uint32_t k;
        pre = (uint32_t)QGPU_VF_WORDS(fmt & ~(uint32_t)QGPU_VF_GEN_MASK);
        for (k = 0; k < QGPU_VF_GEN_MAX; k++) {
            if (fmt & (uint32_t)QGPU_VF_GEN(k)) {
                gn[ng++] = (uint8_t)QGPU_GS_COUNT(gs, k);
            }
        }
    }
    if (stride == 0) {
        stride = swords;
    }
    if (stride < swords || stride > 4096) {         /* pas démesuré = flux douteux */
        return QGPU_ST_BAD_ARG;
    }
    if (count == 0 || count > QGPU_MAX_VERTS ||
        nverts == 0 || nverts > QGPU_MAX_VERTS) {
        return QGPU_ST_BAD_ARG;
    }
    if (itype == QGPU_IDX_NONE) {
        if ((uint64_t)first + count > nverts) {
            return QGPU_ST_BAD_ARG;
        }
    } else if (first != 0) {
        /* Un dessin indexé n'a pas de « premier » : le dire plutôt que de
           l'ignorer silencieusement. */
        return QGPU_ST_BAD_ARG;
    }
    vbase = src_bytes(c, vbuf, voff,
                      (uint64_t)(nverts - 1) * stride * 4 + (uint64_t)swords * 4, &st);
    if (!vbase) {
        return st;
    }
    ibase = NULL;
    if (itype != QGPU_IDX_NONE) {
        ibase = src_bytes(c, ibuf, ioff,
                          (uint64_t)count * (itype == QGPU_IDX_U16 ? 2 : 4), &st);
        if (!ibase) {
            return st;
        }
    }
    if (!grow_vbuf(c, nverts * words)) {
        return QGPU_ST_BACKEND;
    }
    /* H4 : les INDICES d'abord — ce sont eux qui disent quels sommets sont
       réellement lus. Un VBO surdimensionné (queue jamais écrite par
       l'application) ne doit plus ni coûter une conversion ni, autrefois,
       tuer l'image entière parce qu'un mot jamais lu contenait un NaN. */
    lo = first;
    hi = first + count - 1;
    if (itype != QGPU_IDX_NONE) {
        if (!grow_ibuf(c, count)) {
            return QGPU_ST_BACKEND;
        }
        lo = ~0u;
        hi = 0;
        for (i = 0; i < count; i++) {
            uint32_t idx;
            if (itype == QGPU_IDX_U16) {
                idx = ((uint32_t)ibase[i * 2] << 8) | ibase[i * 2 + 1];
            } else {
                idx = qgpu_ld32(ibase + i * 4);
            }
            if (idx >= nverts) {
                return QGPU_ST_BAD_ARG;
            }
            c->ibuf[i] = idx;
            if (idx < lo) lo = idx;
            if (idx > hi) hi = idx;
        }
    }
    /* Un maillage indexé cite presque toujours un bloc compact : le convertir
       d'un trait ne touche chaque sommet qu'une fois, là où suivre les indices
       reconvertirait les sommets partagés. On ne le fait que si le bloc n'est
       pas plus large que la liste d'indices — sinon (indices épars sur un gros
       tampon) on suit les indices. Dans les deux cas, la queue jamais citée du
       tampon n'est plus lue. */
    dense = (itype == QGPU_IDX_NONE) || ((uint64_t)hi - lo + 1 <= count);
    /* Le backend reçoit le tableau ENTIER (il indexe dedans) : ce qu'on ne
       convertit pas doit être mis à zéro, jamais laissé au hasard de realloc. */
    if (!dense || lo != 0 || hi + 1 != nverts) {
        memset(c->vbuf, 0, (size_t)nverts * words * sizeof(float));
    }
    if (gs) {
        if (dense) {
            for (i = lo; i <= hi; i++) {
                conv_raw_vertex_gs(c, vbase, stride, words, i, pre, gn, ng);
            }
        } else {
            for (i = 0; i < count; i++) {
                conv_raw_vertex_gs(c, vbase, stride, words, c->ibuf[i], pre, gn, ng);
            }
        }
    } else if (dense) {
        for (i = lo; i <= hi; i++) {
            conv_raw_vertex(c, vbase, stride, words, i);
        }
    } else {
        for (i = 0; i < count; i++) {
            conv_raw_vertex(c, vbase, stride, words, c->ibuf[i]);
        }
    }
    return raw_finish(c, s, mode, fmt, words, nverts, itype, count, first, lo, hi);
}

/* ── v18 : DRAW_NATIVE ────────────────────────────────────────────────────────
   Les sommets restent dans les tampons hôte au format de l'application (types
   d'OpenGL, pas et décalages quelconques, grand-boutistes) ; le cœur les
   convertit vers la forme de DRAW_RAW puis prend raw_finish. Le tableau de
   travail est c->vbuf (agrandi à la demande, jamais rendu) : aucune allocation
   par dessin une fois le régime atteint. */
typedef struct {
    const uint8_t *data;     /* le tampon hôte */
    uint32_t size;           /* ses octets */
    uint32_t off, stride;    /* octets ; stride déjà résolu (0 → serré) */
    uint32_t type, tb;       /* QGPU_NT_*, octets par composante */
    uint32_t n;              /* composantes sur le fil (1..4) */
    uint32_t rn, dn;         /* composantes lues, composantes de la forme */
    uint32_t code, dst;      /* QGPU_NA_*, mot de l'attribut dans le sommet */
    bool norm;
} NatAttr;

static uint32_t nat_type_bytes(uint32_t t)
{
    switch (t) {
    case QGPU_NT_BYTE: case QGPU_NT_UBYTE:
        return 1;
    case QGPU_NT_SHORT: case QGPU_NT_USHORT:
        return 2;
    case QGPU_NT_INT: case QGPU_NT_UINT: case QGPU_NT_FLOAT:
        return 4;
    case QGPU_NT_DOUBLE:
        return 8;
    default:
        return 0;
    }
}

/* Une composante. Entiers normalisés selon OpenGL 2.1 (celui de l'hôte) :
   non signé c / (2^b − 1), signé (2c + 1) / (2^b − 1). Flottants et doubles
   sont assainis comme DRAW_RAW (sane_coord). */
static inline float nat_comp(const uint8_t *p, uint32_t type, bool norm)
{
    switch (type) {
    case QGPU_NT_FLOAT:
        return sane_coord(qgpu_u2f(qgpu_ld32(p)));
    case QGPU_NT_UBYTE:
        return norm ? (float)p[0] / 255.0f : (float)p[0];
    case QGPU_NT_BYTE: {
        float v = (float)(int8_t)p[0];
        return norm ? (2.0f * v + 1.0f) / 255.0f : v;
    }
    case QGPU_NT_USHORT:
        return norm ? (float)qgpu_ld16(p) / 65535.0f : (float)qgpu_ld16(p);
    case QGPU_NT_SHORT: {
        float v = (float)(int16_t)qgpu_ld16(p);
        return norm ? (2.0f * v + 1.0f) / 65535.0f : v;
    }
    case QGPU_NT_UINT:
        return norm ? (float)((double)qgpu_ld32(p) / 4294967295.0)
                    : (float)qgpu_ld32(p);
    case QGPU_NT_INT: {
        double v = (double)(int32_t)qgpu_ld32(p);
        return norm ? (float)((2.0 * v + 1.0) / 4294967295.0) : (float)v;
    }
    case QGPU_NT_DOUBLE: {
        uint64_t u = ((uint64_t)qgpu_ld32(p) << 32) | qgpu_ld32(p + 4);
        double d;
        memcpy(&d, &u, sizeof(d));
        if (d != d) {
            return 0.0f;
        }
        /* saturer AVANT de convertir : un double hors des flottants donnerait
           un comportement indéfini */
        return d > QGPU_COORD_MAX ? QGPU_COORD_MAX :
               d < -QGPU_COORD_MAX ? -QGPU_COORD_MAX : (float)d;
    }
    default:
        return 0.0f;
    }
}

/* Un attribut, pour les sommets lo..hi (list = NULL) ou pour ceux que citent
   list[0..nl-1] (indices déjà rebasés sur lo). Boucle PAR ATTRIBUT : le type
   est tranché une fois ; les formes d'idDrawVert — flottants × 2 et × 3,
   octets × 4 normalisés — ont leur boucle sans aiguillage. */
static void nat_conv_attr(QgpuCore *c, const NatAttr *at, uint32_t words,
                          uint32_t lo, uint32_t hi, const uint32_t *list, uint32_t nl)
{
    static const float dflt[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const uint8_t *base = at->data + at->off + (size_t)lo * at->stride;
    float *out = c->vbuf + at->dst;
    uint32_t i, j, n = list ? nl : hi - lo + 1;

#define NAT_EACH(body) \
    for (i = 0; i < n; i++) { \
        uint32_t r = list ? list[i] : i; \
        const uint8_t *p = base + (size_t)r * at->stride; \
        float *d = out + (size_t)r * words; \
        body \
    }
    if (at->type == QGPU_NT_FLOAT && at->rn == 3) {
        bool w1 = at->dn == 4;
        NAT_EACH(
            d[0] = sane_coord(qgpu_u2f(qgpu_ld32(p)));
            d[1] = sane_coord(qgpu_u2f(qgpu_ld32(p + 4)));
            d[2] = sane_coord(qgpu_u2f(qgpu_ld32(p + 8)));
            if (w1) {
                d[3] = 1.0f;
            }
        )
    } else if (at->type == QGPU_NT_FLOAT && at->rn == 2 && at->dn == 4) {
        NAT_EACH(
            d[0] = sane_coord(qgpu_u2f(qgpu_ld32(p)));
            d[1] = sane_coord(qgpu_u2f(qgpu_ld32(p + 4)));
            d[2] = 0.0f;
            d[3] = 1.0f;
        )
    } else if (at->type == QGPU_NT_UBYTE && at->norm && at->rn == 4) {
        NAT_EACH(
            d[0] = (float)p[0] / 255.0f;
            d[1] = (float)p[1] / 255.0f;
            d[2] = (float)p[2] / 255.0f;
            d[3] = (float)p[3] / 255.0f;
        )
    } else {
        NAT_EACH(
            for (j = 0; j < at->rn; j++) {
                d[j] = nat_comp(p + j * at->tb, at->type, at->norm);
            }
            for (; j < at->dn; j++) {
                d[j] = dflt[j];
            }
        )
    }
#undef NAT_EACH
}

/* DRAW_NATIVE [mode, n, ibuf, ioff, itype, premier, nattr, aoff]. Tout refus
   est BAD_ARG, non fatal (draw_op) : cf. qgpu_proto.h, section v18. */
static uint32_t do_draw_native(QgpuCore *c, const uint32_t *a)
{
    uint32_t mode = a[0], count = a[1], ibuf = a[2], ioff = a[3], itype = a[4];
    uint32_t first = a[5], nattr = a[6], aoff = a[7];
    NatAttr at[QGPU_NATIVE_MAX_ATTRS];
    uint32_t seen = 0, fmt = 0, possz = 2, words, st, i, k, lo, hi, range;
    bool dense;
    QgpuSurface *s = bound_surface(c, &st);
    const uint8_t *tab;

    if (!s) {
        return st;
    }
    if (mode > QGPU_PRIM_MODE_POLYGON || itype > QGPU_IDX_U32 ||
        count == 0 || count > QGPU_MAX_VERTS ||
        nattr == 0 || nattr > QGPU_NATIVE_MAX_ATTRS ||
        !in_shmem(c, aoff, (uint64_t)nattr * QGPU_NATIVE_DESC_WORDS * 4)) {
        return QGPU_ST_BAD_ARG;
    }
    if (itype == QGPU_IDX_NONE ? (uint64_t)first + count - 1 > 0xFFFFFFFFu
                               : first != 0) {
        return QGPU_ST_BAD_ARG;
    }

    /* 1. la table : codes, tampons, types, tailles ; le format en découle */
    tab = c->shmem + aoff;
    for (k = 0; k < nattr; k++) {
        const uint8_t *d = tab + (size_t)k * QGPU_NATIVE_DESC_WORDS * 4;
        NatAttr *t = &at[k];
        uint32_t code = qgpu_ld32(d), buf = qgpu_ld32(d + 4);
        uint32_t szf = qgpu_ld32(d + 20);

        t->code = code;
        t->off = qgpu_ld32(d + 8);
        t->stride = qgpu_ld32(d + 12);
        t->type = qgpu_ld32(d + 16);
        t->tb = nat_type_bytes(t->type);
        t->n = szf & QGPU_NA_SIZE_MASK;
        t->norm = (szf & QGPU_NA_NORMALIZED) != 0;
        if (code > QGPU_NA_CODE_MAX ||
            (code > QGPU_NA_FOG && code < QGPU_NA_TEX(0)) ||
            ((seen >> code) & 1) ||
            buf >= QGPU_MAX_BUF || !c->buf[buf].used || !c->buf[buf].data ||
            t->tb == 0 || t->n == 0 || t->n > 4 ||
            (szf & ~(uint32_t)(QGPU_NA_SIZE_MASK | QGPU_NA_NORMALIZED))) {
            return QGPU_ST_BAD_ARG;
        }
        seen |= 1u << code;
        t->data = c->buf[buf].data;
        t->size = c->buf[buf].size;
        if (t->stride == 0) {
            t->stride = t->n * t->tb;
        }
        switch (code) {
        case QGPU_NA_POSITION:
            if (t->n < 2) {
                return QGPU_ST_BAD_ARG;
            }
            possz = t->n;
            t->dn = t->n;
            break;
        case QGPU_NA_NORMAL:    fmt |= QGPU_VF_NORMAL;    t->dn = 3; break;
        case QGPU_NA_COLOR:     fmt |= QGPU_VF_COLOR;     t->dn = 4; break;
        case QGPU_NA_SEC_COLOR: fmt |= QGPU_VF_SEC_COLOR; t->dn = 3; break;
        case QGPU_NA_FOG:       fmt |= QGPU_VF_FOG;       t->dn = 1; break;
        default:
            if (code < QGPU_NA_GEN(0)) {
                fmt |= (uint32_t)QGPU_VF_TEX(code - QGPU_NA_TEX(0));
            } else {
                /* comme QGPU_VF_GEN(k) sous DRAW_RAW */
                if (!(c->caps & QGPU_CAP_PROGRAMS)) {
                    return QGPU_ST_BAD_ARG;
                }
                fmt |= (uint32_t)QGPU_VF_GEN(code - QGPU_NA_GEN(0));
            }
            t->dn = 4;
            break;
        }
        t->rn = t->n < t->dn ? t->n : t->dn;
    }
    /* sans position, le générique 0 l'est (aliasing ARB) ; le champ de
       position (2 mots) est alors mis à zéro et ignoré */
    if (!(seen & (1u << QGPU_NA_POSITION)) && !(seen & (1u << QGPU_NA_GEN(0)))) {
        return QGPU_ST_BAD_ARG;
    }
    fmt |= (uint32_t)QGPU_VF_POS(possz);
    words = (uint32_t)QGPU_VF_WORDS(fmt);
    for (k = 0; k < nattr; k++) {
        uint32_t bit;
        switch (at[k].code) {
        case QGPU_NA_POSITION:  bit = 0; break;
        case QGPU_NA_NORMAL:    bit = QGPU_VF_NORMAL; break;
        case QGPU_NA_COLOR:     bit = QGPU_VF_COLOR; break;
        case QGPU_NA_SEC_COLOR: bit = QGPU_VF_SEC_COLOR; break;
        case QGPU_NA_FOG:       bit = QGPU_VF_FOG; break;
        default:
            bit = at[k].code < QGPU_NA_GEN(0)
                ? (uint32_t)QGPU_VF_TEX(at[k].code - QGPU_NA_TEX(0))
                : (uint32_t)QGPU_VF_GEN(at[k].code - QGPU_NA_GEN(0));
            break;
        }
        at[k].dst = bit ? (uint32_t)qgpu_vf_offset(fmt, bit) : 0;
    }

    /* 2. les indices : ils disent quels sommets sont lus */
    if (itype == QGPU_IDX_NONE) {
        lo = first;
        hi = first + count - 1;
    } else {
        const uint8_t *ib = src_bytes(c, ibuf, ioff,
                                      (uint64_t)count * (itype == QGPU_IDX_U16 ? 2 : 4), &st);
        if (!ib || !grow_ibuf(c, count)) {
            return ib ? QGPU_ST_BACKEND : QGPU_ST_BAD_ARG;
        }
        lo = ~0u;
        hi = 0;
        if (itype == QGPU_IDX_U16) {
            for (i = 0; i < count; i++) {
                uint32_t x = qgpu_ld16(ib + i * 2);
                c->ibuf[i] = x;
                lo = x < lo ? x : lo;
                hi = x > hi ? x : hi;
            }
        } else {
            for (i = 0; i < count; i++) {
                uint32_t x = qgpu_ld32(ib + i * 4);
                c->ibuf[i] = x;
                lo = x < lo ? x : lo;
                hi = x > hi ? x : hi;
            }
        }
    }
    range = hi - lo + 1;                       /* hi >= lo ; 0 si 2^32 */
    if (range == 0 || range > QGPU_MAX_VERTS) {
        return QGPU_ST_BAD_ARG;
    }

    /* 3. chaque attribut reste dans son tampon jusqu'au sommet hi */
    for (k = 0; k < nattr; k++) {
        if ((uint64_t)at[k].off + (uint64_t)at[k].stride * hi +
            (uint64_t)at[k].n * at[k].tb > at[k].size) {
            return QGPU_ST_BAD_ARG;
        }
    }

    /* 4. conversion : bloc lo..hi s'il n'est pas plus large que la liste
       d'indices, sinon seulement les sommets cités (le reste à zéro : le
       backend reçoit le tableau entier) */
    if (!grow_vbuf(c, range * words)) {
        return QGPU_ST_BACKEND;
    }
    dense = itype == QGPU_IDX_NONE || range <= count;
    if (itype != QGPU_IDX_NONE && lo) {
        for (i = 0; i < count; i++) {
            c->ibuf[i] -= lo;
        }
    }
    if (!dense || !(seen & (1u << QGPU_NA_POSITION))) {
        memset(c->vbuf, 0, (size_t)range * words * sizeof(float));
    }
    for (k = 0; k < nattr; k++) {
        nat_conv_attr(c, &at[k], words, lo, hi,
                      dense ? NULL : c->ibuf, dense ? 0 : count);
    }
    return raw_finish(c, s, mode, fmt, words, range, itype, count, 0, 0, range - 1);
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
        c->ctx[a[0]].query = -1;
        qgpu_state_init(&c->ctx[a[0]].st);
        qgpu_geom_init(&c->ctx[a[0]].gm);
        qgpu_stipple_init(&c->ctx[a[0]].stip);
        prog_set_init(&c->ctx[a[0]].prg);         /* v16 */
        return QGPU_ST_OK;

    case QGPU_OP_CTX_DESTROY:
        WANT(QGPU_LEN_CTX);
        if (a[0] >= QGPU_MAX_CTX || !c->ctx[a[0]].used) {
            return QGPU_ST_BAD_ARG;
        }
        /* v8 : une requête restée ouverte s'en va avec le contexte, sans quoi
           son identifiant resterait bloqué « actif » pour toujours. */
        if (c->ctx[a[0]].query >= 0) {
            QgpuQuery *q = &c->query[c->ctx[a[0]].query];
            if (c->be->query_end) {
                c->be->query_end(c, q);
            }
            q->active = false;
            c->ctx[a[0]].query = -1;
        }
        prog_set_free(c, &c->ctx[a[0]].prg);      /* v16 */
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
            (a[3] & ~(uint32_t)(QGPU_FMT_MASK | QGPU_FMT_FLAG_DEPTH |
                                QGPU_FMT_FLAG_STENCIL)) ||
            a[1] == 0 || a[2] == 0 ||
            a[1] > QGPU_MAX_SURF_DIM || a[2] > QGPU_MAX_SURF_DIM) {
            return QGPU_ST_BAD_ARG;
        }
        /* v6 : le stencil exige la profondeur (cf. qgpu_proto.h) — l'hôte GL
           n'a alors qu'un seul tampon combiné à gérer. */
        if ((a[3] & QGPU_FMT_FLAG_STENCIL) && !(a[3] & QGPU_FMT_FLAG_DEPTH)) {
            return QGPU_ST_BAD_ARG;
        }
        if (c->surf[a[0]].used) {
            return QGPU_ST_LIMIT;
        }
        /* La case doit être renseignée AVANT l'appel — le backend y lit les
           dimensions — mais elle ne doit rien garder si celui-ci échoue :
           sans ce nettoyage, la case porte des dimensions et un `priv` à
           demi construits qu'un SURF_CREATE ultérieur relirait. */
        s = &c->surf[a[0]];
        s->width = a[1]; s->height = a[2]; s->format = a[3]; s->priv = NULL;
        s->has_depth = (a[3] & QGPU_FMT_FLAG_DEPTH) != 0;
        s->has_stencil = (a[3] & QGPU_FMT_FLAG_STENCIL) != 0;
        if (!c->be->surf_create(c, s)) {
            memset(s, 0, sizeof(*s));
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
        uint32_t off, stride, x, y, w, h, row, fmt, bpp;
        if (nargs != QGPU_LEN_SURF_XFER - 1 &&
            nargs != QGPU_LEN_SURF_XFER_PF - 1) {
            return QGPU_ST_BAD_ARG;
        }
        fmt = (nargs == QGPU_LEN_SURF_XFER_PF - 1) ? a[7] : QGPU_PF_XRGB8888;
        if (fmt != QGPU_PF_XRGB8888 && fmt != QGPU_PF_RGB1555) {
            return QGPU_ST_BAD_ARG;
        }
        bpp = (fmt == QGPU_PF_RGB1555) ? 2u : 4u;
        st = check_xfer(c, a, &s, bpp);
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
                if (fmt == QGPU_PF_RGB1555) {
                    for (i = 0; i < w; i++) {
                        qgpu_st16(dst + i * 2, pack_rgb1555(src[i]));
                    }
                } else {
                    for (i = 0; i < w; i++) {
                        qgpu_st32(dst + i * 4, src[i]);
                    }
                }
            }
        } else {
            for (row = 0; row < h; row++) {
                const uint8_t *src = c->shmem + off + (size_t)row * stride;
                uint32_t *dst = c->pbuf + (size_t)row * w;
                uint32_t i;
                if (fmt == QGPU_PF_RGB1555) {
                    for (i = 0; i < w; i++) {
                        dst[i] = unpack_rgb1555(qgpu_ld16(src + i * 2));
                    }
                } else {
                    for (i = 0; i < w; i++) {
                        dst[i] = qgpu_ld32(src + i * 4);
                    }
                }
            }
            if (!c->be->upload(c, s, x, y, w, h, c->pbuf)) {
                return QGPU_ST_BACKEND;
            }
        }
        return QGPU_ST_OK;
    }

    case QGPU_OP_SURF_PRESENT: {
        uint32_t off, stride, x, y, w, h, fmt, bpp, row, i;
        uint64_t rowbytes, total;
        WANT(QGPU_LEN_SURF_PRESENT);
        if (!c->scanout || c->scanout_size == 0) {
            return QGPU_ST_BAD_ARG;
        }
        s = surf_lookup(c, a[0]);
        if (!s) {
            return QGPU_ST_NO_SURF;
        }
        off = a[1]; stride = a[2]; x = a[3]; y = a[4];
        w = a[5]; h = a[6]; fmt = a[7];
        if (fmt != QGPU_PF_XRGB8888 && fmt != QGPU_PF_RGB1555) {
            return QGPU_ST_BAD_ARG;
        }
        bpp = (fmt == QGPU_PF_RGB1555) ? 2u : 4u;
        if (w == 0 || h == 0 || x > s->width || y > s->height ||
            w > s->width - x || h > s->height - y) {
            return QGPU_ST_BAD_ARG;
        }
        if (stride < (uint64_t)w * bpp) {
            return QGPU_ST_BAD_ARG;
        }
        if ((fmt == QGPU_PF_XRGB8888 && (stride & 3)) ||
            (fmt == QGPU_PF_RGB1555 && (stride & 1))) {
            return QGPU_ST_BAD_ARG;
        }
        /* Q3 : quand le device connaît la géométrie de l'écran, un pas ou une
           profondeur qui ne sont pas les siens sont refusés — c'est exactement
           le symptôme de Q1 (image écrite au pas d'un autre écran, en diagonale
           ou figée, avec statut OK). Le plugin, sur BAD_ARG d'un SURF_PRESENT,
           coupe la présentation directe et rend la main à Apple (Q2). */
        if (c->scanout_stride != 0) {
            uint32_t sbpp = (c->scanout_depth == 15 || c->scanout_depth == 16)
                            ? 2u
                            : (c->scanout_depth == 24 || c->scanout_depth == 32)
                              ? 4u : 0u;
            if (stride != c->scanout_stride || (sbpp && sbpp != bpp)) {
                TRACE(c, "SURF_PRESENT pas %u/bpp %u, écran pas %u/prof %u : "
                      "refusé", stride, bpp, c->scanout_stride,
                      c->scanout_depth);
                return QGPU_ST_BAD_ARG;
            }
        }
        rowbytes = (uint64_t)w * bpp;
        total = (uint64_t)(h - 1) * stride + rowbytes;
        if ((uint64_t)off + total > c->scanout_size) {
            return QGPU_ST_OOB;
        }
        if (!grow_pbuf(c, w * h)) {
            return QGPU_ST_BACKEND;
        }
        if (!c->be->readback(c, s, x, y, w, h, c->pbuf)) {
            return QGPU_ST_BACKEND;
        }
        for (row = 0; row < h; row++) {
            uint8_t *dst = c->scanout + off + (size_t)row * stride;
            const uint32_t *src = c->pbuf + (size_t)row * w;
            if (fmt == QGPU_PF_XRGB8888) {
                for (i = 0; i < w; i++) {
                    qgpu_st32(dst + i * 4, src[i]);
                }
            } else {
                for (i = 0; i < w; i++) {
                    uint16_t pix = pack_rgb1555(src[i]);
                    qgpu_st16(dst + i * 2, pix);
                }
            }
        }
        if (c->scanout_dirty) {
            c->scanout_dirty(c->scanout_opaque, off, (uint32_t)total);
        }
        return QGPU_ST_OK;
    }

    case QGPU_OP_DEPTH_READBACK:
    case QGPU_OP_DEPTH_UPLOAD: {
        uint32_t off, stride, x, y, w, h, row, i, df, bpp;
        if (nargs != QGPU_LEN_SURF_XFER - 1 &&
            nargs != QGPU_LEN_SURF_XFER_PF - 1) {
            return QGPU_ST_BAD_ARG;
        }
        df = (nargs == QGPU_LEN_SURF_XFER_PF - 1) ? a[7] : QGPU_DF_FLOAT32;
        if (df != QGPU_DF_FLOAT32 && df != QGPU_DF_UNORM16) {
            return QGPU_ST_BAD_ARG;
        }
        bpp = (df == QGPU_DF_UNORM16) ? 2u : 4u;
        st = check_xfer(c, a, &s, bpp);
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
                    float d = c->dbuf[(size_t)row * w + i];
                    if (df == QGPU_DF_UNORM16) {
                        qgpu_st16(dst + i * 2, pack_unorm16(d));
                    } else {
                        qgpu_st32(dst + i * 4, qgpu_f2u(d));
                    }
                }
            }
        } else {
            for (row = 0; row < h; row++) {
                const uint8_t *src = c->shmem + off + (size_t)row * stride;
                for (i = 0; i < w; i++) {
                    float d;
                    if (df == QGPU_DF_UNORM16) {
                        d = unpack_unorm16(qgpu_ld16(src + i * 2));
                    } else {
                        d = qgpu_u2f(qgpu_ld32(src + i * 4));
                    }
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

    case QGPU_OP_STENCIL_READBACK:
    case QGPU_OP_STENCIL_UPLOAD: {
        uint32_t off, stride, x, y, w, h, row, i;
        WANT(QGPU_LEN_SURF_XFER);
        st = check_xfer(c, a, &s, 4);
        if (st != QGPU_ST_OK) {
            return st;
        }
        if (!s->has_stencil) {
            return QGPU_ST_BAD_ARG;
        }
        off = a[1]; stride = a[2]; x = a[3]; y = a[4]; w = a[5]; h = a[6];
        if (!grow_sbuf(c, w * h)) {
            return QGPU_ST_BACKEND;
        }
        if (op == QGPU_OP_STENCIL_READBACK) {
            if (!c->be->stencil_readback(c, s, x, y, w, h, c->sbuf)) {
                return QGPU_ST_BACKEND;
            }
            for (row = 0; row < h; row++) {
                uint8_t *dst = c->shmem + off + (size_t)row * stride;
                for (i = 0; i < w; i++) {
                    /* un mot de 32 bits par pixel, 24 bits de poids fort nuls */
                    qgpu_st32(dst + i * 4, c->sbuf[(size_t)row * w + i]);
                }
            }
        } else {
            for (row = 0; row < h; row++) {
                const uint8_t *src = c->shmem + off + (size_t)row * stride;
                for (i = 0; i < w; i++) {
                    /* les bits au-delà du 8e sont ignorés, comme en OpenGL */
                    c->sbuf[(size_t)row * w + i] =
                        (uint8_t)(qgpu_ld32(src + i * 4) & 0xFF);
                }
            }
            if (!c->be->stencil_upload(c, s, x, y, w, h, c->sbuf)) {
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
        /* v10 : une atténuation de point que le backend ne sait pas faire est
           refusée au moment où elle est posée (cf. QGPU_CAP_GL14), sans rien
           écrire. */
        if (a[0] >= QGPU_SK_POINT_SIZE_MIN && a[0] <= QGPU_SK_POINT_ATT_QUAD &&
            a[0] != QGPU_SK_POINT_FADE && !(c->caps & QGPU_CAP_GL14)) {
            QgpuState tmp = *cur_state(c);
            tmp.v[a[0]] = a[1];
            if (!qgpu_points_plain(&tmp)) {
                return QGPU_ST_BACKEND;
            }
        }
        /* v16 : activer un programme sans backend capable est refusé ici,
           sans rien écrire — même règle que les paramètres de point. */
        if ((a[0] == QGPU_SK_VERTEX_PROGRAM || a[0] == QGPU_SK_FRAGMENT_PROGRAM) &&
            a[1] && !(c->caps & QGPU_CAP_PROGRAMS)) {
            return QGPU_ST_BACKEND;
        }
        /* Génériques à taille déclarée : même règle. Poser 0 reste permis
           (c'est la valeur initiale, neutre). */
        if (a[0] == QGPU_SK_GEN_SIZES && a[1] && !(c->caps & QGPU_CAP_GEN_SIZES)) {
            return QGPU_ST_BACKEND;
        }
        cur_state(c)->v[a[0]] = a[1];
        return QGPU_ST_OK;

    case QGPU_OP_CLEAR:
        WANT(QGPU_LEN_CLEAR);
        s = bound_surface(c, &st);
        if (!s) {
            return st;
        }
        if (a[0] & ~(uint32_t)(QGPU_CLEAR_COLOR | QGPU_CLEAR_DEPTH |
                               QGPU_CLEAR_STENCIL)) {
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
        /* v7 : le viewport a enfin un sens — pour le chemin brut seulement.
           x et y sont signés et comptés depuis le coin BAS-GAUCHE de la
           surface, comme glViewport ; w et h ne peuvent pas être négatifs. */
        if ((int32_t)a[2] < 0 || (int32_t)a[3] < 0 ||
            a[2] > QGPU_MAX_SURF_DIM || a[3] > QGPU_MAX_SURF_DIM ||
            (int32_t)a[0] < -QGPU_MAX_SURF_DIM || (int32_t)a[0] > QGPU_MAX_SURF_DIM ||
            (int32_t)a[1] < -QGPU_MAX_SURF_DIM || (int32_t)a[1] > QGPU_MAX_SURF_DIM) {
            return QGPU_ST_BAD_ARG;
        }
        cur_geom(c)->vp[0] = (int32_t)a[0]; cur_geom(c)->vp[1] = (int32_t)a[1];
        cur_geom(c)->vp[2] = (int32_t)a[2]; cur_geom(c)->vp[3] = (int32_t)a[3];
        cur_geom(c)->vp_set = true;
        return QGPU_ST_OK;

    /* ── v7 : état de l'étage géométrique ───────────────────────────────── */
    case QGPU_OP_SET_MATRIX:
        WANT(QGPU_LEN_SET_MATRIX);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (a[0] >= QGPU_MTX_COUNT) {
            return QGPU_ST_BAD_ARG;
        }
        if (!read_f(a + 1, 16, cur_geom(c)->mtx[a[0]])) {
            return QGPU_ST_BAD_ARG;
        }
        return QGPU_ST_OK;

    case QGPU_OP_DEPTH_RANGE: {
        float d[2];
        WANT(QGPU_LEN_DEPTH_RANGE);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (!read_f(a, 2, d) || d[0] < 0.0f || d[0] > 1.0f ||
            d[1] < 0.0f || d[1] > 1.0f) {
            return QGPU_ST_BAD_ARG;
        }
        cur_geom(c)->depth_near = d[0];
        cur_geom(c)->depth_far = d[1];
        return QGPU_ST_OK;
    }

    case QGPU_OP_SET_LIGHT: {
        QgpuLight l;
        WANT(QGPU_LEN_SET_LIGHT);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (a[0] >= QGPU_MAX_LIGHTS || a[1] > 1) {
            return QGPU_ST_BAD_ARG;
        }
        memset(&l, 0, sizeof(l));
        l.enabled = a[1] != 0;
        if (!read_f(a + 2, 4, l.ambient) || !read_f(a + 6, 4, l.diffuse) ||
            !read_f(a + 10, 4, l.specular) || !read_f(a + 14, 4, l.position) ||
            !read_f(a + 18, 3, l.spot_dir) || !read_f(a + 21, 1, &l.spot_exp) ||
            !read_f(a + 22, 1, &l.spot_cutoff) || !read_f(a + 23, 3, l.att)) {
            return QGPU_ST_BAD_ARG;
        }
        /* Bornes d'OpenGL : elles sont vérifiées ICI, une fois pour toutes. */
        if (l.spot_exp < 0.0f || l.spot_exp > 128.0f) {
            return QGPU_ST_BAD_ARG;
        }
        if (!((l.spot_cutoff >= 0.0f && l.spot_cutoff <= 90.0f) ||
              l.spot_cutoff == 180.0f)) {
            return QGPU_ST_BAD_ARG;
        }
        if (l.att[0] < 0.0f || l.att[1] < 0.0f || l.att[2] < 0.0f) {
            return QGPU_ST_BAD_ARG;
        }
        cur_geom(c)->light[a[0]] = l;
        return QGPU_ST_OK;
    }

    case QGPU_OP_SET_MATERIAL: {
        QgpuMaterial m;
        int f0, f1, i;
        WANT(QGPU_LEN_SET_MATERIAL);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (!valid_face(a[0])) {
            return QGPU_ST_BAD_ARG;
        }
        memset(&m, 0, sizeof(m));
        if (!read_f(a + 1, 4, m.ambient) || !read_f(a + 5, 4, m.diffuse) ||
            !read_f(a + 9, 4, m.specular) || !read_f(a + 13, 4, m.emission) ||
            !read_f(a + 17, 1, &m.shininess)) {
            return QGPU_ST_BAD_ARG;
        }
        if (m.shininess < 0.0f || m.shininess > 128.0f) {
            return QGPU_ST_BAD_ARG;
        }
        f0 = (a[0] == 0x0405) ? 1 : 0;                  /* GL_BACK */
        f1 = (a[0] == 0x0404) ? 0 : 1;                  /* GL_FRONT */
        for (i = f0; i <= f1; i++) {
            cur_geom(c)->mat[i] = m;
        }
        return QGPU_ST_OK;
    }

    case QGPU_OP_SET_LIGHT_MODEL:
        WANT(QGPU_LEN_SET_LIGHT_MODEL);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (!read_f(a, 4, cur_geom(c)->lm_ambient)) {
            return QGPU_ST_BAD_ARG;
        }
        return QGPU_ST_OK;

    case QGPU_OP_SET_TEXGEN: {
        QgpuTexgen tg;
        WANT(QGPU_LEN_SET_TEXGEN);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (a[0] >= QGPU_MAX_UNITS || a[1] > QGPU_TG_Q || a[2] > 1) {
            return QGPU_ST_BAD_ARG;
        }
        if (a[3] != QGPU_TG_OBJECT_LINEAR && a[3] != QGPU_TG_EYE_LINEAR &&
            a[3] != QGPU_TG_SPHERE_MAP && a[3] != QGPU_TG_NORMAL_MAP &&
            a[3] != QGPU_TG_REFLECTION_MAP) {
            return QGPU_ST_BAD_ARG;
        }
        /* GL : SPHERE_MAP n'existe que pour S et T ; NORMAL_MAP et
           REFLECTION_MAP que pour S, T et R. */
        if ((a[3] == QGPU_TG_SPHERE_MAP && a[1] > QGPU_TG_T) ||
            ((a[3] == QGPU_TG_NORMAL_MAP || a[3] == QGPU_TG_REFLECTION_MAP) &&
             a[1] > QGPU_TG_R)) {
            return QGPU_ST_BAD_ARG;
        }
        memset(&tg, 0, sizeof(tg));
        tg.enabled = a[2] != 0;
        tg.mode = a[3];
        if (!read_f(a + 4, 4, tg.obj_plane) || !read_f(a + 8, 4, tg.eye_plane)) {
            return QGPU_ST_BAD_ARG;
        }
        cur_geom(c)->texgen[a[0]][a[1]] = tg;
        return QGPU_ST_OK;
    }

    case QGPU_OP_SET_CLIP_PLANE: {
        QgpuClipPlane cp;
        WANT(QGPU_LEN_SET_CLIP_PLANE);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (a[0] >= QGPU_MAX_CLIP_PLANES || a[1] > 1) {
            return QGPU_ST_BAD_ARG;
        }
        memset(&cp, 0, sizeof(cp));
        cp.enabled = a[1] != 0;
        if (!read_f(a + 2, 4, cp.eq)) {
            return QGPU_ST_BAD_ARG;
        }
        cur_geom(c)->clip[a[0]] = cp;
        return QGPU_ST_OK;
    }

    case QGPU_OP_SET_CURRENT: {
        float v[4];
        QgpuGeom *gm;
        WANT(QGPU_LEN_SET_CURRENT);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (a[0] >= QGPU_CUR_COUNT || !read_f(a + 1, 4, v)) {
            return QGPU_ST_BAD_ARG;
        }
        gm = cur_geom(c);
        switch (a[0]) {
        case QGPU_CUR_NORMAL:
            memcpy(gm->cur_normal, v, 3 * sizeof(float));
            break;
        case QGPU_CUR_COLOR:
            memcpy(gm->cur_color, v, 4 * sizeof(float));
            break;
        case QGPU_CUR_SEC_COLOR:
            memcpy(gm->cur_sec, v, 3 * sizeof(float));
            break;
        case QGPU_CUR_FOG:
            gm->cur_fog = v[0];
            break;
        default:
            memcpy(gm->cur_tex[a[0] - QGPU_CUR_TEXCOORD0], v, 4 * sizeof(float));
            break;
        }
        return QGPU_ST_OK;
    }

    case QGPU_OP_DRAW_RAW:
        WANT(QGPU_LEN_DRAW_RAW);
        return do_draw_raw(c, a, QGPU_BUF_SHMEM, QGPU_BUF_SHMEM, 0);

    case QGPU_OP_DRAW_RAW_BUF:
        WANT(QGPU_LEN_DRAW_RAW_BUF);
        return do_draw_raw(c, a, a[2], a[6], 1);

    case QGPU_OP_DRAW_NATIVE:                        /* v18 */
        WANT(QGPU_LEN_DRAW_NATIVE);
        return do_draw_native(c, a);

    /* ── v8 : pointillé de polygone et requêtes d'occlusion ─────────────── */
    case QGPU_OP_SET_POLYGON_STIPPLE: {
        int i;
        WANT(QGPU_LEN_SET_POLYGON_STIPPLE);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        /* Aucune valeur n'est invalide : 32 mots de bits bruts. */
        for (i = 0; i < 32; i++) {
            c->ctx[c->cur_ctx].stip.row[i] = a[i];
        }
        return QGPU_ST_OK;
    }

    case QGPU_OP_QUERY_BEGIN: {
        QgpuContext *cx;
        WANT(QGPU_LEN_QUERY);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (a[0] >= QGPU_MAX_QUERIES) {
            return QGPU_ST_BAD_ARG;
        }
        cx = &c->ctx[c->cur_ctx];
        /* OpenGL : pas d'imbrication, et une requête déjà ouverte (fût-ce par
           un autre contexte) ne peut pas l'être deux fois. */
        if (cx->query >= 0 || c->query[a[0]].active) {
            return QGPU_ST_BAD_ARG;
        }
        if (!(c->caps & QGPU_CAP_OCCLUSION) || !c->be->query_begin) {
            return QGPU_ST_BACKEND;
        }
        c->query[a[0]].samples = 0;
        if (!c->be->query_begin(c, &c->query[a[0]])) {
            return QGPU_ST_BACKEND;
        }
        /* `used` seulement après l'accord du backend : sinon un BEGIN refusé
           laisse la requête « lancée » et QUERY_RESULT rend 0 au lieu de
           QGPU_ST_BAD_ARG. */
        c->query[a[0]].used = true;
        c->query[a[0]].active = true;
        cx->query = (int32_t)a[0];
        return QGPU_ST_OK;
    }

    case QGPU_OP_QUERY_END: {
        QgpuContext *cx;
        WANT(QGPU_LEN_QUERY);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (a[0] >= QGPU_MAX_QUERIES) {
            return QGPU_ST_BAD_ARG;
        }
        cx = &c->ctx[c->cur_ctx];
        /* Fermer ce qui n'est pas ouvert ICI est une faute du flux, pas un
           non-événement : c'est la seule façon de repérer un BEGIN perdu. */
        if (cx->query != (int32_t)a[0]) {
            return QGPU_ST_BAD_ARG;
        }
        cx->query = -1;
        c->query[a[0]].active = false;
        if (!c->be->query_end || !c->be->query_end(c, &c->query[a[0]])) {
            return QGPU_ST_BACKEND;
        }
        return QGPU_ST_OK;
    }

    case QGPU_OP_QUERY_RESULT: {
        QgpuQuery *q;
        uint64_t n;
        WANT(QGPU_LEN_QUERY_RESULT);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (a[0] >= QGPU_MAX_QUERIES) {
            return QGPU_ST_BAD_ARG;
        }
        q = &c->query[a[0]];
        if (!q->used || q->active) {
            return QGPU_ST_BAD_ARG;       /* jamais lancée, ou encore ouverte */
        }
        if (!in_shmem(c, a[1], 8)) {
            return QGPU_ST_OOB;
        }
        if (!c->be->query_result || !c->be->query_result(c, q)) {
            return QGPU_ST_BACKEND;
        }
        /* Le device est synchrone : après un QUERY_END le résultat est là. */
        n = q->samples > 0xFFFFFFFFu ? 0xFFFFFFFFu : q->samples;
        qgpu_st32(c->shmem + a[1], 1);
        qgpu_st32(c->shmem + a[1] + 4, (uint32_t)n);
        return QGPU_ST_OK;
    }

    case QGPU_OP_DRAW_TRIANGLES:
        WANT(QGPU_LEN_DRAW);
        return do_draw(c, a, QGPU_PRIM_TRIANGLES, QGPU_VERTEX_WORDS, 0, false);
    case QGPU_OP_DRAW_TRIANGLES_TEX:
        WANT(QGPU_LEN_DRAW);
        return do_draw(c, a, QGPU_PRIM_TRIANGLES, QGPU_VERTEX_TEX_WORDS, 1, false);
    case QGPU_OP_DRAW_TRIANGLES_TEX2:
        WANT(QGPU_LEN_DRAW);
        return do_draw(c, a, QGPU_PRIM_TRIANGLES, QGPU_VERTEX_TEX2_WORDS, 2, false);
    case QGPU_OP_DRAW_TRIANGLES_TEXN:
        WANT(QGPU_LEN_DRAW_N);
        if (a[2] < 1 || a[2] > QGPU_MAX_UNITS) {
            return QGPU_ST_BAD_ARG;
        }
        return do_draw(c, a, QGPU_PRIM_TRIANGLES, QGPU_VERTEX_TEXN_WORDS(a[2]), a[2], false);
    case QGPU_OP_DRAW_TRIANGLES_SEC:                     /* v11 */
        WANT(QGPU_LEN_DRAW_N);
        if (a[2] > QGPU_MAX_UNITS) {
            return QGPU_ST_BAD_ARG;
        }
        return do_draw(c, a, QGPU_PRIM_TRIANGLES, QGPU_VERTEX_SEC_WORDS(a[2]), a[2], true);
    case QGPU_OP_DRAW_LINES:
        WANT(QGPU_LEN_DRAW);
        return do_draw(c, a, QGPU_PRIM_LINES, QGPU_VERTEX_WORDS, 0, false);
    case QGPU_OP_DRAW_POINTS:
        WANT(QGPU_LEN_DRAW);
        return do_draw(c, a, QGPU_PRIM_POINTS, QGPU_VERTEX_WORDS, 0, false);

    case QGPU_OP_TEX_CREATE:
    case QGPU_OP_TEX_CREATE3: {
        uint32_t target = QGPU_TT_2D;
        if (op == QGPU_OP_TEX_CREATE) {
            WANT(QGPU_LEN_TEX);
        } else {
            WANT(QGPU_LEN_TEX_CREATE3);
            target = a[1];
        }
        if (a[0] >= QGPU_MAX_TEX || !valid_target(target)) {
            return QGPU_ST_BAD_ARG;
        }
        if (c->tex[a[0]].used) {
            return QGPU_ST_LIMIT;
        }
        if (target != QGPU_TT_2D && !(c->caps & QGPU_CAP_GL14)) {
            return QGPU_ST_BACKEND;
        }
        tex_init(&c->tex[a[0]], target);
        return QGPU_ST_OK;
    }

    case QGPU_OP_TEX_DESTROY:
        WANT(QGPU_LEN_TEX);
        if (a[0] >= QGPU_MAX_TEX || !c->tex[a[0]].used) {
            return QGPU_ST_BAD_ARG;
        }
        tex_free(c, &c->tex[a[0]]);
        return QGPU_ST_OK;

    case QGPU_OP_TEX_PARAM: {
        QgpuTexture *t;
        bool rect;
        WANT(QGPU_LEN_TEX_PARAM);
        if (a[0] >= QGPU_MAX_TEX || !c->tex[a[0]].used) {
            return QGPU_ST_BAD_ARG;
        }
        t = &c->tex[a[0]];
        rect = t->target == QGPU_TT_RECTANGLE;
        /* v10 : ce que le backend doit savoir faire lui-même (cf. QGPU_CAP_GL14) */
        if (((a[1] >= QGPU_TP_WRAP_R && a[1] <= QGPU_TP_DEPTH_MODE) ||
             ((a[1] >= QGPU_TP_WRAP_S && a[1] <= QGPU_TP_WRAP_T) &&
              (a[2] == QGPU_TW_MIRRORED_REPEAT || a[2] == QGPU_TW_CLAMP_TO_BORDER))) &&
            !(c->caps & QGPU_CAP_GL14)) {
            return QGPU_ST_BACKEND;
        }
        switch (a[1]) {
        case QGPU_TP_MIN_FILTER:
            if (!valid_filter(a[2], !rect)) return QGPU_ST_BAD_ARG;
            t->min_filter = a[2];
            break;
        case QGPU_TP_MAG_FILTER:
            if (!valid_filter(a[2], false)) return QGPU_ST_BAD_ARG;
            t->mag_filter = a[2];
            break;
        case QGPU_TP_WRAP_S:
        case QGPU_TP_WRAP_T:
        case QGPU_TP_WRAP_R:
            if (!valid_wrap(a[2], t->target)) return QGPU_ST_BAD_ARG;
            *(a[1] == QGPU_TP_WRAP_S ? &t->wrap_s :
              a[1] == QGPU_TP_WRAP_T ? &t->wrap_t : &t->wrap_r) = a[2];
            break;
        case QGPU_TP_BORDER_COLOR:
            t->border = a[2];
            break;
        case QGPU_TP_MIN_LOD:
        case QGPU_TP_MAX_LOD:
            /* RECTANGLE : pas de mipmaps, et le pilote NVIDIA refuse ces deux
               paramètres (GL_INVALID_OPERATION) — refusés ici pour tous. */
            if (rect || !finite_f(a[2], 1e6f)) return QGPU_ST_BAD_ARG;
            *(a[1] == QGPU_TP_MIN_LOD ? &t->min_lod : &t->max_lod) = qgpu_u2f(a[2]);
            break;
        case QGPU_TP_BASE_LEVEL:
            if (a[2] >= QGPU_MAX_TEX_LEVELS || (rect && a[2] != 0)) return QGPU_ST_BAD_ARG;
            t->base_level = a[2];
            tex_refresh_format(t);
            break;
        case QGPU_TP_MAX_LEVEL:
            if (a[2] > 1000) return QGPU_ST_BAD_ARG;
            t->max_level = a[2];
            break;
        case QGPU_TP_LOD_BIAS:
            if (!finite_f(a[2], QGPU_MAX_LOD_BIAS)) return QGPU_ST_BAD_ARG;
            t->lod_bias = qgpu_u2f(a[2]);
            break;
        case QGPU_TP_COMPARE_MODE:
            if (a[2] != QGPU_TC_NONE && a[2] != QGPU_TC_COMPARE_R) return QGPU_ST_BAD_ARG;
            t->compare_mode = a[2];
            break;
        case QGPU_TP_COMPARE_FUNC:
            if (!valid_func(a[2])) return QGPU_ST_BAD_ARG;
            t->compare_func = a[2];
            break;
        case QGPU_TP_DEPTH_MODE:
            if (a[2] != 0x1909 && a[2] != 0x8049 && a[2] != 0x1906) return QGPU_ST_BAD_ARG;
            t->depth_mode = a[2];
            break;
        case QGPU_TP_GENERATE_MIPMAP:
            if (a[2] > 1) return QGPU_ST_BAD_ARG;
            t->gen_mipmap = a[2];
            break;
        default:
            return QGPU_ST_BAD_ARG;
        }
        t->params_dirty = true;
        return QGPU_ST_OK;
    }

    case QGPU_OP_TEX_IMAGE:
    case QGPU_OP_TEX_IMAGE3: {
        /* TEX_IMAGE (v3) est TEX_IMAGE3 sur une texture 2D, avec des texels
           ARGB big-endian — c'est-à-dire GL_BGRA + GL_UNSIGNED_INT_8_8_8_8_REV. */
        QgpuTexture *t;
        QgpuTexLevel *lv;
        TexSrc src;
        uint32_t itarget, lvl, w, h, d, bfmt, off, row, img;
        uint64_t total = 0;
        int face;

        if (op == QGPU_OP_TEX_IMAGE) {
            WANT(QGPU_LEN_TEX_IMAGE);
            itarget = QGPU_TT_2D; lvl = a[1]; w = a[2]; h = a[3]; d = 1;
            bfmt = a[4]; off = a[5]; row = img = 0;
            tex_src(0x80E1, 0x8367, &src);
            /* H1 : GL_ALPHA reste GL_ALPHA jusqu'au backend (cf. unpack_texel).
               COLOR_INDEX, lui, n'a pas de palette ici : il est lu en luminance. */
            if (bfmt == 0x1900 || bfmt == 0x80E5)
                bfmt = 0x1909;
            if (!valid_base_format(bfmt)) {
                return QGPU_ST_BAD_ARG;
            }
        } else {
            WANT(QGPU_LEN_TEX_IMAGE3);
            itarget = a[1]; lvl = a[2]; w = a[3]; h = a[4]; d = a[5];
            bfmt = a[6]; off = a[9]; row = a[10]; img = a[11];
            if (!tex_src(a[7], a[8], &src)) {
                return QGPU_ST_BAD_ARG;
            }
            if (bfmt == 0x1900 || bfmt == 0x80E5)
                bfmt = 0x1909;
            if (src.depth ? bfmt != 0x1902
                : src.block ? bfmt != (src.fmt == QGPU_TF_DXT1_RGB ? 0x1907u : 0x1908u)
                : !valid_base_format(bfmt)) {
                return QGPU_ST_BAD_ARG;
            }
        }
        if (a[0] >= QGPU_MAX_TEX || !c->tex[a[0]].used) {
            return QGPU_ST_BAD_ARG;
        }
        t = &c->tex[a[0]];
        face = tex_face(t, itarget);
        if (face < 0 || lvl >= QGPU_MAX_TEX_LEVELS || w == 0 || h == 0 || d == 0) {
            return QGPU_ST_BAD_ARG;
        }
        switch (t->target) {
        case QGPU_TT_1D:
            if (h != 1 || d != 1 || w > QGPU_MAX_TEX_DIM || src.block) return QGPU_ST_BAD_ARG;
            break;
        case QGPU_TT_3D:
            if (w > QGPU_MAX_TEX_3D_DIM || h > QGPU_MAX_TEX_3D_DIM ||
                d > QGPU_MAX_TEX_3D_DIM || src.depth || src.block) return QGPU_ST_BAD_ARG;
            break;
        case QGPU_TT_CUBE_MAP:
            if (w != h || d != 1 || w > QGPU_MAX_TEX_DIM || src.depth) return QGPU_ST_BAD_ARG;
            break;
        case QGPU_TT_RECTANGLE:
            if (lvl != 0 || d != 1 || w > QGPU_MAX_TEX_DIM || h > QGPU_MAX_TEX_DIM ||
                src.block) return QGPU_ST_BAD_ARG;
            break;
        default:                                         /* 2D */
            if (d != 1 || w > QGPU_MAX_TEX_DIM || h > QGPU_MAX_TEX_DIM) return QGPU_ST_BAD_ARG;
        }
        if (src.depth && !(c->caps & QGPU_CAP_GL14)) {
            return QGPU_ST_BACKEND;
        }
        if (off != QGPU_TEX_NO_DATA) {
            if (!tex_src_size(&src, w, h, d, &row, &img, &total)) {
                return QGPU_ST_BAD_ARG;
            }
            if (!in_shmem(c, off, total)) {
                return QGPU_ST_OOB;
            }
        }
        lv = &t->level[face][lvl];
        if (!tex_alloc_level(lv, w, h, d, bfmt, off == QGPU_TEX_NO_DATA)) {
            return QGPU_ST_BACKEND;
        }
        if (off != QGPU_TEX_NO_DATA) {
            tex_store(lv, &src, c->shmem + off, 0, 0, 0, w, h, d, row, img);
        }
        t->dirty[face] |= 1u << lvl;
        if (t->gen_mipmap && lvl == t->base_level && !tex_gen_mipmaps(t, face)) {
            return QGPU_ST_BACKEND;
        }
        tex_refresh_format(t);
        return QGPU_ST_OK;
    }

    case QGPU_OP_TEX_SUBIMAGE: {
        QgpuTexture *t;
        QgpuTexLevel *lv;
        TexSrc src;
        uint32_t lvl, x, y, z, w, h, d, off, row, img;
        uint64_t total;
        int face;
        WANT(QGPU_LEN_TEX_SUBIMAGE);
        lvl = a[2]; x = a[3]; y = a[4]; z = a[5]; w = a[6]; h = a[7]; d = a[8];
        off = a[11]; row = a[12]; img = a[13];
        if (a[0] >= QGPU_MAX_TEX || !c->tex[a[0]].used || !tex_src(a[9], a[10], &src)) {
            return QGPU_ST_BAD_ARG;
        }
        t = &c->tex[a[0]];
        face = tex_face(t, a[1]);
        if (face < 0 || lvl >= QGPU_MAX_TEX_LEVELS || !t->level[face][lvl].px) {
            return QGPU_ST_BAD_ARG;
        }
        lv = &t->level[face][lvl];
        if ((uint64_t)x + w > lv->w || (uint64_t)y + h > lv->h || (uint64_t)z + d > lv->d ||
            src.depth != (lv->fmt == 0x1902)) {
            return QGPU_ST_BAD_ARG;
        }
        if (src.block && ((x | y) & 3 || ((w & 3) && x + w != lv->w) ||
                          ((h & 3) && y + h != lv->h) || t->target == QGPU_TT_3D)) {
            return QGPU_ST_BAD_ARG;
        }
        if (w == 0 || h == 0 || d == 0) {
            return QGPU_ST_OK;                           /* comme OpenGL : sans effet */
        }
        if (!tex_src_size(&src, w, h, d, &row, &img, &total)) {
            return QGPU_ST_BAD_ARG;
        }
        if (!in_shmem(c, off, total)) {
            return QGPU_ST_OOB;
        }
        tex_store(lv, &src, c->shmem + off, x, y, z, w, h, d, row, img);
        t->dirty[face] |= 1u << lvl;
        if (t->gen_mipmap && lvl == t->base_level && !tex_gen_mipmaps(t, face)) {
            return QGPU_ST_BACKEND;
        }
        return QGPU_ST_OK;
    }

    case QGPU_OP_COPY_TEX: {
        QgpuTexture *t;
        QgpuTexLevel *lv;
        uint32_t lvl, x, y, z, sx, sy, w, h, row;
        int face;

        WANT(QGPU_LEN_COPY_TEX);
        s = bound_surface(c, &st);
        if (!s) {
            return st;
        }
        if (a[0] >= QGPU_MAX_TEX || !c->tex[a[0]].used) {
            return QGPU_ST_BAD_ARG;
        }
        t = &c->tex[a[0]];
        face = tex_face(t, a[1]);
        lvl = a[2]; x = a[3]; y = a[4]; z = a[5];
        sx = a[6]; sy = a[7]; w = a[8]; h = a[9];
        if (face < 0 || lvl >= QGPU_MAX_TEX_LEVELS || !t->level[face][lvl].px) {
            return QGPU_ST_BAD_ARG;
        }
        lv = &t->level[face][lvl];
        if (lv->fmt == 0x1902) {
            return QGPU_ST_BAD_ARG;          /* copie couleur → profondeur : non */
        }
        if (w == 0 || h == 0) {
            return QGPU_ST_OK;
        }
        if ((uint64_t)x + w > lv->w || (uint64_t)y + h > lv->h ||
            (uint64_t)z + 1 > lv->d ||
            sx > s->width || sy > s->height ||
            w > s->width - sx || h > s->height - sy) {
            return QGPU_ST_BAD_ARG;
        }
        if (!grow_pbuf(c, w * h)) {
            return QGPU_ST_BACKEND;
        }
        if (!c->be->readback(c, s, sx, sy, w, h, c->pbuf)) {
            return QGPU_ST_BACKEND;
        }
        /* v17 (24/09/2026, vitres de DOOM 3) : la relecture rend les lignes de
           HAUT en bas (convention de l'invité) ; en OpenGL, la ligne 0 d'une
           texture est le BAS de la fenêtre. La copie est donc retournée :
           un programme qui relit la copie par fragment.position (retourné
           lui aussi dans qgpu-gl.c) ou par des coordonnées calculées de la
           position de clip voit l'image à l'endroit, comme sur une vraie
           carte. Avant : le reflet des vitres était à l'envers. */
        for (row = 0; row < h; row++) {
            memcpy(lv->px + ((size_t)z * lv->h + y + row) * lv->w + x,
                   c->pbuf + (size_t)(h - 1 - row) * w, (size_t)w * 4);
        }
        t->dirty[face] |= 1u << lvl;
        if (t->gen_mipmap && lvl == t->base_level && !tex_gen_mipmaps(t, face)) {
            return QGPU_ST_BACKEND;
        }
        return QGPU_ST_OK;
    }

    case QGPU_OP_BUF_CREATE: {
        uint32_t id, size;
        uint8_t *p;
        WANT(QGPU_LEN_BUF_CREATE);
        id = a[0]; size = a[1];
        if (id >= QGPU_MAX_BUF || size == 0 || size > QGPU_MAX_BUF_SIZE) {
            return QGPU_ST_BAD_ARG;
        }
        if (c->buf[id].used) {
            return QGPU_ST_LIMIT;
        }
        p = calloc(1, size);
        if (!p) {
            return QGPU_ST_BACKEND;
        }
        c->buf[id].used = true;
        c->buf[id].data = p;
        c->buf[id].size = size;
        return QGPU_ST_OK;
    }

    case QGPU_OP_BUF_DESTROY:
        WANT(QGPU_LEN_BUF);
        if (a[0] >= QGPU_MAX_BUF || !c->buf[a[0]].used) {
            return QGPU_ST_BAD_ARG;
        }
        free(c->buf[a[0]].data);
        memset(&c->buf[a[0]], 0, sizeof(c->buf[a[0]]));
        return QGPU_ST_OK;

    case QGPU_OP_BUF_SUBDATA: {
        uint32_t id, dst, src, len;
        WANT(QGPU_LEN_BUF_SUBDATA);
        id = a[0]; dst = a[1]; src = a[2]; len = a[3];
        if (id >= QGPU_MAX_BUF || !c->buf[id].used) {
            return QGPU_ST_BAD_ARG;
        }
        if (len == 0) {
            return QGPU_ST_OK;
        }
        if ((uint64_t)dst + len > c->buf[id].size) {
            return QGPU_ST_OOB;
        }
        if (!in_shmem(c, src, len)) {
            return QGPU_ST_OOB;
        }
        memcpy(c->buf[id].data + dst, c->shmem + src, len);
        return QGPU_ST_OK;
    }

    /* ── v16 : programmes ARB ───────────────────────────────────────────── */
    case QGPU_OP_PROG_CREATE: {
        QgpuProgram *p;
        WANT(QGPU_LEN_PROG_CREATE);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (!(c->caps & QGPU_CAP_PROGRAMS) || !c->be->prog_string) {
            return QGPU_ST_BACKEND;
        }
        if (a[0] >= QGPU_MAX_PROG || prog_which(a[1]) < 0) {
            return QGPU_ST_BAD_ARG;
        }
        p = &c->ctx[c->cur_ctx].prg.prog[a[0]];
        if (p->used) {
            return QGPU_ST_LIMIT;
        }
        p->local = calloc(QGPU_MAX_PROG_PARAMS, sizeof(*p->local));
        if (!p->local) {
            return QGPU_ST_BACKEND;
        }
        p->used = true;
        p->target = a[1];
        return QGPU_ST_OK;
    }

    case QGPU_OP_PROG_STRING: {
        QgpuProgram *p;
        uint32_t len = a[1], off = a[2];
        char *text;
        WANT(QGPU_LEN_PROG_STRING);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (!(c->caps & QGPU_CAP_PROGRAMS) || !c->be->prog_string) {
            return QGPU_ST_BACKEND;
        }
        if (a[0] >= QGPU_MAX_PROG || !c->ctx[c->cur_ctx].prg.prog[a[0]].used ||
            len == 0 || len > QGPU_MAX_PROG_LEN) {
            return QGPU_ST_BAD_ARG;
        }
        /* le texte n'a pas à être aligné : on ne vérifie que la fenêtre */
        if ((uint64_t)off + len > c->shmem_size) {
            return QGPU_ST_OOB;
        }
        p = &c->ctx[c->cur_ctx].prg.prog[a[0]];
        if (!prog_text_ok(c->shmem + off, len, p->target)) {
            return QGPU_ST_BAD_ARG;
        }
        text = malloc(len + 1);
        if (!text) {
            return QGPU_ST_BACKEND;
        }
        memcpy(text, c->shmem + off, len);
        text[len] = '\0';
        free(p->text);
        p->text = text;
        p->len = len;
        p->compiled = false;
        p->local_dirty = true;             /* un nouvel objet hôte repart de zéro */
        if (!c->be->prog_string(c, p)) {
            /* Refus du compilateur de l'hôte : BAD_ARG NON FATAL (cf.
               soft_fail_op) et programme cassé jusqu'au prochain texte. */
            p->broken = true;
            TRACE(c, "  programme %u refusé par l'hôte", a[0]);
            return QGPU_ST_BAD_ARG;
        }
        p->compiled = true;
        p->broken = false;
        return QGPU_ST_OK;
    }

    case QGPU_OP_PROG_DESTROY: {
        QgpuProgSet *pg;
        WANT(QGPU_LEN_PROG);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (!(c->caps & QGPU_CAP_PROGRAMS)) {
            return QGPU_ST_BACKEND;
        }
        pg = &c->ctx[c->cur_ctx].prg;
        if (a[0] >= QGPU_MAX_PROG || !pg->prog[a[0]].used) {
            return QGPU_ST_BAD_ARG;
        }
        if (pg->bound[QGPU_PROG_VP] == (int32_t)a[0]) {
            pg->bound[QGPU_PROG_VP] = -1;
        }
        if (pg->bound[QGPU_PROG_FP] == (int32_t)a[0]) {
            pg->bound[QGPU_PROG_FP] = -1;
        }
        prog_free(c, &pg->prog[a[0]]);
        return QGPU_ST_OK;
    }

    case QGPU_OP_PROG_BIND: {
        QgpuProgSet *pg;
        int w = prog_which(a[0]);
        WANT(QGPU_LEN_PROG_BIND);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (!(c->caps & QGPU_CAP_PROGRAMS)) {
            return QGPU_ST_BACKEND;
        }
        pg = &c->ctx[c->cur_ctx].prg;
        if (w < 0) {
            return QGPU_ST_BAD_ARG;
        }
        if (a[1] == QGPU_PROG_NONE) {
            pg->bound[w] = -1;
            return QGPU_ST_OK;
        }
        if (a[1] >= QGPU_MAX_PROG || !pg->prog[a[1]].used ||
            pg->prog[a[1]].target != a[0]) {
            return QGPU_ST_BAD_ARG;
        }
        pg->bound[w] = (int32_t)a[1];
        return QGPU_ST_OK;
    }

    case QGPU_OP_PROG_ENV:
    case QGPU_OP_PROG_LOCAL: {
        QgpuProgSet *pg;
        uint32_t first = a[1], n = a[2], off = a[3];
        float (*dst)[4];
        WANT(QGPU_LEN_PROG_PARAMS);
        if (c->cur_ctx < 0) {
            return QGPU_ST_NO_CTX;
        }
        if (!(c->caps & QGPU_CAP_PROGRAMS)) {
            return QGPU_ST_BACKEND;
        }
        pg = &c->ctx[c->cur_ctx].prg;
        if ((uint64_t)first + n > QGPU_MAX_PROG_PARAMS) {
            return QGPU_ST_BAD_ARG;
        }
        if (op == QGPU_OP_PROG_ENV) {
            int w = prog_which(a[0]);
            if (w < 0) {
                return QGPU_ST_BAD_ARG;
            }
            if (n == 0) {
                return QGPU_ST_OK;
            }
            dst = pg->env[w] + first;
            st = read_f4_shmem(c, off, n, dst);
            if (st != QGPU_ST_OK) {
                return st;
            }
            if (first + n > pg->env_hi[w]) {
                pg->env_hi[w] = first + n;
            }
            pg->env_dirty[w] = true;
        } else {
            QgpuProgram *p;
            if (a[0] >= QGPU_MAX_PROG || !pg->prog[a[0]].used) {
                return QGPU_ST_BAD_ARG;
            }
            if (n == 0) {
                return QGPU_ST_OK;
            }
            p = &pg->prog[a[0]];
            dst = p->local + first;
            st = read_f4_shmem(c, off, n, dst);
            if (st != QGPU_ST_OK) {
                return st;
            }
            if (first + n > p->local_hi) {
                p->local_hi = first + n;
            }
            p->local_dirty = true;
        }
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
    case QGPU_OP_SURF_PRESENT: case QGPU_OP_COPY_TEX:
    case QGPU_OP_BUF_CREATE: case QGPU_OP_BUF_DESTROY: case QGPU_OP_BUF_SUBDATA:
    case QGPU_OP_DEPTH_READBACK: case QGPU_OP_DEPTH_UPLOAD:
    case QGPU_OP_STENCIL_READBACK: case QGPU_OP_STENCIL_UPLOAD: case QGPU_OP_CLEAR:
    case QGPU_OP_VIEWPORT: case QGPU_OP_SET_STATE: case QGPU_OP_DRAW_TRIANGLES:
    case QGPU_OP_DRAW_TRIANGLES_TEX: case QGPU_OP_TEX_CREATE: case QGPU_OP_TEX_DESTROY:
    case QGPU_OP_DRAW_TRIANGLES_TEX2: case QGPU_OP_DRAW_LINES: case QGPU_OP_DRAW_POINTS:
    case QGPU_OP_DRAW_TRIANGLES_TEXN:
    case QGPU_OP_TEX_IMAGE: case QGPU_OP_TEX_PARAM:
    /* v10 */
    case QGPU_OP_TEX_CREATE3: case QGPU_OP_TEX_IMAGE3: case QGPU_OP_TEX_SUBIMAGE:
    /* v11 */
    case QGPU_OP_DRAW_TRIANGLES_SEC:
    /* v7 */
    case QGPU_OP_SET_MATRIX: case QGPU_OP_DEPTH_RANGE: case QGPU_OP_SET_LIGHT:
    case QGPU_OP_SET_MATERIAL: case QGPU_OP_SET_LIGHT_MODEL:
    case QGPU_OP_SET_TEXGEN: case QGPU_OP_SET_CLIP_PLANE:
    case QGPU_OP_SET_CURRENT: case QGPU_OP_DRAW_RAW: case QGPU_OP_DRAW_RAW_BUF:
    case QGPU_OP_DRAW_NATIVE:                        /* v18 */
    /* v8 */
    case QGPU_OP_SET_POLYGON_STIPPLE: case QGPU_OP_QUERY_BEGIN:
    case QGPU_OP_QUERY_END: case QGPU_OP_QUERY_RESULT:
    /* v16 */
    case QGPU_OP_PROG_CREATE: case QGPU_OP_PROG_STRING: case QGPU_OP_PROG_DESTROY:
    case QGPU_OP_PROG_BIND: case QGPU_OP_PROG_ENV: case QGPU_OP_PROG_LOCAL:
        return true;
    default:
        return false;
    }
}

/* Les opcodes de DESSIN : 0x0030–0x0036 sont contigus, plus les trois chemins
   bruts (v18 : DRAW_NATIVE, dont TOUS les refus sont des BAD_ARG). Seuls ceux-là — et, v16, PROG_STRING, dont le refus par le
   compilateur de l'hôte ne met pas la suite du flux en doute — ont droit à un
   BAD_ARG non fatal (cf. ci-dessous). */
static bool draw_op(uint32_t op)
{
    return (op >= QGPU_OP_DRAW_TRIANGLES && op <= QGPU_OP_DRAW_TRIANGLES_SEC) ||
           op == QGPU_OP_DRAW_RAW || op == QGPU_OP_DRAW_RAW_BUF ||
           op == QGPU_OP_DRAW_NATIVE || op == QGPU_OP_PROG_STRING;
}

uint32_t qgpu_core_execute(QgpuCore *c, uint32_t off, uint32_t len)
{
    uint32_t nwords, pc = 0, st = QGPU_ST_OK;
    /* H4 : première faute retenue d'un opcode de dessin — le flux continue,
       mais le device en est informé à la fin de la soumission. */
    uint32_t held = QGPU_ST_OK, held_pc = 0;
    /* v8 : la plus longue commande est SET_POLYGON_STIPPLE (32 arguments) */
    uint32_t args[QGPU_MAX_CMD_ARGS];

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
            /* Un DESSIN mal formé ne coûte que lui-même : les SET_STATE, le
               CLEAR et le SURF_PRESENT qui suivent doivent s'exécuter, sans
               quoi une seule primitive douteuse fige l'écran (H4). Tout le
               reste garde l'arrêt immédiat : une faute d'objet, de bornes ou
               d'en-tête met en doute la suite du flux. */
            if (st != QGPU_ST_BAD_ARG || !draw_op(op)) {
                break;
            }
            if (held == QGPU_ST_OK) {
                held = st;
                held_pc = pc;
            }
        } else {
            c->ncmds++;
        }
        pc += clen;
    }

    /* La PREMIÈRE faute est celle qui décrit le flux : un dessin refusé au
       début compte plus qu'un arrêt provoqué plus loin. */
    if (held != QGPU_ST_OK) {
        st = held;
        pc = held_pc;
    }
    c->status = st;
    c->status_pc = (st == QGPU_ST_OK) ? 0 : pc;
    return st;
}
