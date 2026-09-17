/*
 * qgpu-soft.c — backend logiciel de référence du GPU paravirtuel « qgpu ».
 *
 * Rasteriseur simple (fonctions d'arête, interpolation barycentrique des
 * couleurs et de la profondeur, centre de pixel à +0,5) avec le pipeline par
 * fragment d'OpenGL 1.x, dans l'ordre de la spécification :
 *   ciseaux → test alpha → test de stencil → test de profondeur (+ écriture)
 *   → mélange → masque.
 * Il sert de vérité terrain aux tests (le backend GL doit donner les mêmes
 * pixels à l'intérieur des primitives) et de repli sans OpenGL hôte. Aucune
 * ambition de performance.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qgpu-core.h"

/* v7 : format de sommet INTERNE du chemin brut. C'est celui du protocole
   (8 mots + 4 par unité de texture) allongé de trois mots de couleur
   secondaire : OpenGL l'ajoute APRÈS l'environnement de texture, donc elle ne
   peut pas être fondue dans la couleur primaire. Les chemins v1–v6 n'en ont
   pas et passent sec_off = -1. */
#define SOFT_SEC_OFF    QGPU_VERTEX_TEXN_WORDS(QGPU_MAX_UNITS)
#define SOFT_RAW_WORDS  (SOFT_SEC_OFF + 3)

typedef struct SoftSurface {
    uint32_t *px;                  /* width*height, 0xAARRGGBB */
    float    *depth;               /* width*height, ou NULL */
    uint8_t  *stencil;             /* width*height, ou NULL (v6) */
} SoftSurface;

static bool soft_init(QgpuCore *c)
{
    (void)c;
    return true;
}

static void soft_fini(QgpuCore *c)
{
    (void)c;
}

static bool soft_surf_create(QgpuCore *c, QgpuSurface *s)
{
    SoftSurface *ss = calloc(1, sizeof(*ss));
    size_t n = (size_t)s->width * s->height;
    size_t i;
    (void)c;
    if (!ss) {
        return false;
    }
    ss->px = calloc(n, sizeof(uint32_t));
    if (s->has_depth) {
        ss->depth = malloc(n * sizeof(float));
        if (ss->depth) {
            for (i = 0; i < n; i++) {
                ss->depth[i] = 1.0f;
            }
        }
    }
    if (s->has_stencil) {
        ss->stencil = calloc(n, 1);    /* stencil initial : 0, comme en OpenGL */
    }
    if (!ss->px || (s->has_depth && !ss->depth) ||
        (s->has_stencil && !ss->stencil)) {
        free(ss->px);
        free(ss->depth);
        free(ss->stencil);
        free(ss);
        return false;
    }
    s->priv = ss;
    return true;
}

static void soft_surf_destroy(QgpuCore *c, QgpuSurface *s)
{
    SoftSurface *ss = s->priv;
    (void)c;
    if (ss) {
        free(ss->px);
        free(ss->depth);
        free(ss->stencil);
        free(ss);
    }
    s->priv = NULL;
}

/* Rectangle effectif [x0,x1)×[y0,y1) : surface ∩ ciseaux. */
static void clip_rect(const QgpuSurface *s, const QgpuState *st,
                      int *x0, int *y0, int *x1, int *y1)
{
    *x0 = 0; *y0 = 0; *x1 = s->width; *y1 = s->height;
    if (st->v[QGPU_SK_SCISSOR]) {
        int sx = st->v[QGPU_SK_SCISSOR_X], sy = st->v[QGPU_SK_SCISSOR_Y];
        int sx1 = sx + (int)st->v[QGPU_SK_SCISSOR_W];
        int sy1 = sy + (int)st->v[QGPU_SK_SCISSOR_H];
        if (sx > *x0) *x0 = sx;
        if (sy > *y0) *y0 = sy;
        if (sx1 < *x1) *x1 = sx1;
        if (sy1 < *y1) *y1 = sy1;
    }
}

static inline uint32_t to_u8(float v)
{
    if (!(v > 0.0f)) {
        return 0;
    }
    if (v >= 1.0f) {
        return 255;
    }
    return (uint32_t)(v * 255.0f + 0.5f);
}

static inline float clamp01(float v)
{
    return !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* Écrit un pixel en respectant le masque de couleur (bit0 R … bit3 A). */
static inline uint32_t masked(uint32_t dst, uint32_t src, uint32_t mask)
{
    uint32_t keep = 0;
    if (!(mask & 1)) keep |= 0x00FF0000u;
    if (!(mask & 2)) keep |= 0x0000FF00u;
    if (!(mask & 4)) keep |= 0x000000FFu;
    if (!(mask & 8)) keep |= 0xFF000000u;
    return (dst & keep) | (src & ~keep);
}

static inline bool compare(uint32_t func, float a, float b)
{
    switch (func) {
    case 0x0200: return false;        /* NEVER */
    case 0x0201: return a <  b;       /* LESS */
    case 0x0202: return a == b;       /* EQUAL */
    case 0x0203: return a <= b;       /* LEQUAL */
    case 0x0204: return a >  b;       /* GREATER */
    case 0x0205: return a != b;       /* NOTEQUAL */
    case 0x0206: return a >= b;       /* GEQUAL */
    default:     return true;         /* ALWAYS */
    }
}

/* Comparaison du test de stencil : (ref & masque) fonc (tampon & masque),
   sur des entiers — `compare` ci-dessus travaille sur des flottants. */
static inline bool compare_u(uint32_t func, uint32_t a, uint32_t b)
{
    switch (func) {
    case 0x0200: return false;        /* NEVER */
    case 0x0201: return a <  b;       /* LESS */
    case 0x0202: return a == b;       /* EQUAL */
    case 0x0203: return a <= b;       /* LEQUAL */
    case 0x0204: return a >  b;       /* GREATER */
    case 0x0205: return a != b;       /* NOTEQUAL */
    case 0x0206: return a >= b;       /* GEQUAL */
    default:     return true;         /* ALWAYS */
    }
}

/* Nouvelle valeur d'un texel de stencil après l'opération `op`. INCR et DECR
   saturent, leurs variantes _WRAP bouclent sur 8 bits. */
static inline uint8_t stencil_op(uint32_t op, uint8_t sv, uint32_t ref)
{
    switch (op) {
    case QGPU_SOP_ZERO:       return 0;
    case QGPU_SOP_REPLACE:    return (uint8_t)ref;
    case QGPU_SOP_INCR:       return sv < 255 ? (uint8_t)(sv + 1) : 255;
    case QGPU_SOP_DECR:       return sv > 0 ? (uint8_t)(sv - 1) : 0;
    case QGPU_SOP_INVERT:     return (uint8_t)~sv;
    case QGPU_SOP_INCR_WRAP:  return (uint8_t)(sv + 1);
    case QGPU_SOP_DECR_WRAP:  return (uint8_t)(sv - 1);
    default:                  return sv;                 /* KEEP */
    }
}

/* Écrit le stencil en respectant le masque d'écriture. */
static inline void stencil_write(uint8_t *p, uint8_t nv, uint32_t wmask)
{
    *p = (uint8_t)((*p & ~wmask) | (nv & wmask));
}

/* Facteur de mélange pour un canal (ch 0..2 = RGB, 3 = A). */
static inline float factor(uint32_t f, const float *s, const float *d, int ch)
{
    switch (f) {
    case 0x0000: return 0.0f;
    case 0x0001: return 1.0f;
    case 0x0300: return s[ch];                     /* SRC_COLOR */
    case 0x0301: return 1.0f - s[ch];
    case 0x0302: return s[3];                      /* SRC_ALPHA */
    case 0x0303: return 1.0f - s[3];
    case 0x0304: return d[3];                      /* DST_ALPHA */
    case 0x0305: return 1.0f - d[3];
    case 0x0306: return d[ch];                     /* DST_COLOR */
    case 0x0307: return 1.0f - d[ch];
    case 0x0308:                                   /* SRC_ALPHA_SATURATE */
        if (ch == 3) {
            return 1.0f;
        }
        return s[3] < 1.0f - d[3] ? s[3] : 1.0f - d[3];
    default:     return 1.0f;
    }
}

static uint32_t blend(const QgpuState *st, float r, float g, float b, float a,
                      uint32_t dst)
{
    float s[4] = { clamp01(r), clamp01(g), clamp01(b), clamp01(a) };
    float d[4] = { ((dst >> 16) & 255) / 255.0f, ((dst >> 8) & 255) / 255.0f,
                   (dst & 255) / 255.0f, ((dst >> 24) & 255) / 255.0f };
    float o[4];
    int ch;

    if (!st->v[QGPU_SK_BLEND]) {
        memcpy(o, s, sizeof(o));
    } else {
        for (ch = 0; ch < 4; ch++) {
            uint32_t sf = st->v[ch < 3 ? QGPU_SK_BLEND_SRC_RGB : QGPU_SK_BLEND_SRC_A];
            uint32_t df = st->v[ch < 3 ? QGPU_SK_BLEND_DST_RGB : QGPU_SK_BLEND_DST_A];
            uint32_t eq = st->v[ch < 3 ? QGPU_SK_BLEND_EQ_RGB : QGPU_SK_BLEND_EQ_A];
            float sv = s[ch] * factor(sf, s, d, ch);
            float dv = d[ch] * factor(df, s, d, ch);
            /* GL : FUNC_SUBTRACT = S·s − D·d ; FUNC_REVERSE_SUBTRACT = D·d − S·s */
            if (eq == 0x800A) {
                o[ch] = sv - dv;
            } else if (eq == 0x800B) {
                o[ch] = dv - sv;
            } else {
                o[ch] = sv + dv;
            }
        }
    }
    return (to_u8(o[3]) << 24) | (to_u8(o[0]) << 16) | (to_u8(o[1]) << 8) | to_u8(o[2]);
}

static bool soft_clear(QgpuCore *c, QgpuSurface *s, const QgpuState *st,
                       uint32_t mask, uint32_t argb, float depth)
{
    SoftSurface *ss = s->priv;
    int x0, y0, x1, y1, x, y;
    uint32_t cmask = st->v[QGPU_SK_COLOR_MASK];
    uint32_t swmask = st->v[QGPU_SK_STENCIL_WRITE_MASK];
    uint8_t sclear = (uint8_t)st->v[QGPU_SK_STENCIL_CLEAR];
    (void)c;

    clip_rect(s, st, &x0, &y0, &x1, &y1);
    for (y = y0; y < y1; y++) {
        uint32_t *row = ss->px + (size_t)y * s->width;
        float *drow = ss->depth ? ss->depth + (size_t)y * s->width : NULL;
        uint8_t *srow = ss->stencil ? ss->stencil + (size_t)y * s->width : NULL;
        for (x = x0; x < x1; x++) {
            if (mask & QGPU_CLEAR_COLOR) {
                row[x] = masked(row[x], argb, cmask);
            }
            if ((mask & QGPU_CLEAR_DEPTH) && drow && st->v[QGPU_SK_DEPTH_WRITE]) {
                drow[x] = depth;
            }
            /* glClear : l'effacement du stencil passe par le masque d'écriture */
            if ((mask & QGPU_CLEAR_STENCIL) && srow) {
                stencil_write(&srow[x], sclear, swmask);
            }
        }
    }
    return true;
}

static inline float edge(float ax, float ay, float bx, float by,
                         float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* ───────────────────────────── texturage (v3) ───────────────────────────── */

typedef struct Rgba { float r, g, b, a; } Rgba;

static int wrap_index(int i, int n, uint32_t mode, bool *border)
{
    if (mode == 0x2901) {                               /* REPEAT */
        i %= n;
        return i < 0 ? i + n : i;
    }
    if (mode == 0x2900 && (i < 0 || i >= n)) {          /* CLAMP : bordure */
        *border = true;
        return 0;
    }
    return i < 0 ? 0 : i >= n ? n - 1 : i;              /* CLAMP_TO_EDGE */
}

static Rgba texel(const QgpuTexture *t, const QgpuTexLevel *lv, int x, int y)
{
    bool border = false;
    uint32_t p;
    Rgba c;
    x = wrap_index(x, lv->w, t->wrap_s, &border);
    y = wrap_index(y, lv->h, t->wrap_t, &border);
    if (border) {
        c.r = c.g = c.b = c.a = 0.0f;                   /* couleur de bordure par défaut */
        return c;
    }
    p = lv->px[(size_t)y * lv->w + x];
    c.r = ((p >> 16) & 255) / 255.0f;
    c.g = ((p >> 8) & 255) / 255.0f;
    c.b = (p & 255) / 255.0f;
    c.a = ((p >> 24) & 255) / 255.0f;
    return c;
}

static Rgba sample_level(const QgpuTexture *t, const QgpuTexLevel *lv, float s, float tt,
                         bool linear)
{
    float u, v, fu, fv;
    int i0, j0;
    Rgba c00, c10, c01, c11, r;

    if (t->wrap_s == 0x2900) s = clamp01(s);            /* GL_CLAMP borne s */
    if (t->wrap_t == 0x2900) tt = clamp01(tt);
    u = s * lv->w;
    v = tt * lv->h;
    if (!linear) {
        i0 = (int)floorf(u);
        j0 = (int)floorf(v);
        if (t->wrap_s != 0x2901 && i0 >= (int)lv->w) i0 = lv->w - 1;
        if (t->wrap_t != 0x2901 && j0 >= (int)lv->h) j0 = lv->h - 1;
        return texel(t, lv, i0, j0);
    }
    u -= 0.5f;
    v -= 0.5f;
    i0 = (int)floorf(u);
    j0 = (int)floorf(v);
    fu = u - i0;
    fv = v - j0;
    c00 = texel(t, lv, i0, j0);
    c10 = texel(t, lv, i0 + 1, j0);
    c01 = texel(t, lv, i0, j0 + 1);
    c11 = texel(t, lv, i0 + 1, j0 + 1);
#define LERP2(f) ((1 - fu) * (1 - fv) * c00.f + fu * (1 - fv) * c10.f + \
                  (1 - fu) * fv * c01.f + fu * fv * c11.f)
    r.r = LERP2(r); r.g = LERP2(g); r.b = LERP2(b); r.a = LERP2(a);
#undef LERP2
    return r;
}

/* Échantillonnage OpenGL 1.x ; `lod` = λ, constant par triangle ici (ρ calculé
   sur les aires : approximation assumée du backend de référence). */
static Rgba sample(const QgpuTexture *t, uint32_t nlevels, float s, float tt, float lod)
{
    uint32_t minf = t->min_filter;
    bool mag_linear = t->mag_filter == 0x2601;
    float c = (mag_linear && (minf == 0x2700 || minf == 0x2702)) ? 0.5f : 0.0f;
    float maxd = (float)(nlevels - 1);
    int d;
    Rgba a, b, r;

    if (lod <= c) {
        return sample_level(t, &t->level[0], s, tt, mag_linear);
    }
    switch (minf) {
    case 0x2600: return sample_level(t, &t->level[0], s, tt, false);
    case 0x2601: return sample_level(t, &t->level[0], s, tt, true);
    case 0x2700: case 0x2701:                           /* *_MIPMAP_NEAREST */
        d = (int)ceilf(lod + 0.5f) - 1;
        if (d < 0) d = 0;
        if (d > (int)maxd) d = (int)maxd;
        return sample_level(t, &t->level[d], s, tt, minf == 0x2701);
    default: {                                          /* *_MIPMAP_LINEAR */
        float f;
        int d2;
        if (lod >= maxd) {
            return sample_level(t, &t->level[(int)maxd], s, tt, minf == 0x2703);
        }
        d = (int)floorf(lod);
        d2 = d + 1;
        f = lod - d;
        a = sample_level(t, &t->level[d], s, tt, minf == 0x2703);
        b = sample_level(t, &t->level[d2], s, tt, minf == 0x2703);
        r.r = a.r + f * (b.r - a.r); r.g = a.g + f * (b.g - a.g);
        r.b = a.b + f * (b.b - a.b); r.a = a.a + f * (b.a - a.a);
        return r;
    }
    }
}

/* Composantes de source d'une texture selon son format de base (OpenGL 1.3,
   table 3.19) : utilisées par GL_COMBINE. */
static Rgba tex_source(uint32_t fmt, Rgba tc)
{
    Rgba o = tc;
    switch (fmt) {
    case 0x1906: o.r = o.g = o.b = 0.0f; break;                 /* ALPHA */
    case 0x1909: o.g = o.b = tc.r; o.a = 1.0f; break;          /* LUMINANCE */
    case 0x190A: o.g = o.b = tc.r; break;                       /* LUMINANCE_ALPHA */
    case 0x8049: o.g = o.b = o.a = tc.r; break;                 /* INTENSITY */
    case 0x1907: o.a = 1.0f; break;                             /* RGB */
    default: break;                                             /* RGBA */
    }
    return o;
}

static float combine_fn(uint32_t fn, float a0, float a1, float a2)
{
    switch (fn) {
    case QGPU_CB_REPLACE:     return a0;
    case QGPU_CB_MODULATE:    return a0 * a1;
    case QGPU_CB_ADD:         return a0 + a1;
    case QGPU_CB_ADD_SIGNED:  return a0 + a1 - 0.5f;
    case QGPU_CB_INTERPOLATE: return a0 * a2 + a1 * (1.0f - a2);
    default:                  return a0 - a1;                   /* SUBTRACT */
    }
}

/* GL_COMBINE (ARB_texture_env_combine, ARB_texture_env_dot3). */
static void tex_combine(const QgpuState *st, int unit, uint32_t fmt, Rgba tc,
                        const Rgba *prim, Rgba *cur)
{
    uint32_t cb = st->v[QGPU_SK_COMBINE0 + unit];
    uint32_t src = st->v[QGPU_SK_COMBINE_SRC0 + unit];
    uint32_t ec = st->v[QGPU_SK_UNIT(unit) + QGPU_SK_U_ENV_COLOR];
    Rgba k = { ((ec >> 16) & 255) / 255.0f, ((ec >> 8) & 255) / 255.0f,
               (ec & 255) / 255.0f, ((ec >> 24) & 255) / 255.0f };
    Rgba t = tex_source(fmt, tc);
    Rgba arg[3];
    float aa[3], rs = (float)(1u << ((cb >> 8) & 3)), as = (float)(1u << ((cb >> 10) & 3));
    uint32_t frgb = cb & 0xF, fa = (cb >> 4) & 0xF;
    Rgba o;
    int i;

    for (i = 0; i < 3; i++) {
        uint32_t f = (src >> (5 * i)) & 31, g = (src >> (15 + 4 * i)) & 15;
        const Rgba *sr, *sa;
        const Rgba *pick[4] = { &t, &k, prim, cur };
        sr = pick[(f & 7) & 3];
        sa = pick[(g & 7) & 3];
        switch (f >> 3) {
        case QGPU_CO_COLOR:           arg[i] = *sr; break;
        case QGPU_CO_ONE_MINUS_COLOR:
            arg[i].r = 1 - sr->r; arg[i].g = 1 - sr->g; arg[i].b = 1 - sr->b; break;
        case QGPU_CO_ALPHA:           arg[i].r = arg[i].g = arg[i].b = sr->a; break;
        default:                      arg[i].r = arg[i].g = arg[i].b = 1 - sr->a; break;
        }
        aa[i] = (g >> 3) ? 1 - sa->a : sa->a;
    }
    if (frgb == QGPU_CB_DOT3_RGB || frgb == QGPU_CB_DOT3_RGBA) {
        float d = 4.0f * ((arg[0].r - 0.5f) * (arg[1].r - 0.5f) +
                          (arg[0].g - 0.5f) * (arg[1].g - 0.5f) +
                          (arg[0].b - 0.5f) * (arg[1].b - 0.5f));
        o.r = o.g = o.b = d;
    } else {
        o.r = combine_fn(frgb, arg[0].r, arg[1].r, arg[2].r);
        o.g = combine_fn(frgb, arg[0].g, arg[1].g, arg[2].g);
        o.b = combine_fn(frgb, arg[0].b, arg[1].b, arg[2].b);
    }
    if (frgb == QGPU_CB_DOT3_RGBA) {
        o.a = o.r;                                  /* avant échelle, comme l'RGB */
    } else {
        o.a = combine_fn(fa, aa[0], aa[1], aa[2]) * as;
    }
    cur->r = clamp01(o.r * rs);
    cur->g = clamp01(o.g * rs);
    cur->b = clamp01(o.b * rs);
    cur->a = clamp01(frgb == QGPU_CB_DOT3_RGBA ? o.a * rs : o.a);
}

/* Fonctions d'environnement de texture (OpenGL 1.x, tables 3.22/3.23). */
static void tex_env(const QgpuState *st, int unit, uint32_t fmt, Rgba tc,
                    const Rgba *prim, float *r, float *g, float *b, float *a)
{
    uint32_t mode = st->v[QGPU_SK_UNIT(unit) + QGPU_SK_U_ENV_MODE];
    uint32_t ec = st->v[QGPU_SK_UNIT(unit) + QGPU_SK_U_ENV_COLOR];
    float cr = ((ec >> 16) & 255) / 255.0f, cg = ((ec >> 8) & 255) / 255.0f;
    float cb = (ec & 255) / 255.0f, ca = ((ec >> 24) & 255) / 255.0f;
    float L = tc.r, I = tc.r;
    float fr = clamp01(*r), fg = clamp01(*g), fb = clamp01(*b), fa = clamp01(*a);
    bool has_rgb = fmt == 0x1907 || fmt == 0x1908;
    bool has_l = fmt == 0x1909 || fmt == 0x190A;
    bool has_a = fmt == 0x1906 || fmt == 0x1908 || fmt == 0x190A;
    bool is_i = fmt == 0x8049;
    float tr = has_rgb ? tc.r : L, tg = has_rgb ? tc.g : L, tb = has_rgb ? tc.b : L;

    switch (mode) {
    case 0x8570: {                                      /* COMBINE (v5) */
        Rgba cur = { fr, fg, fb, fa };
        tex_combine(st, unit, fmt, tc, prim, &cur);
        fr = cur.r; fg = cur.g; fb = cur.b; fa = cur.a;
        break;
    }
    case 0x1E01:                                        /* REPLACE */
        if (has_rgb || has_l || is_i) { fr = tr; fg = tg; fb = tb; }
        if (has_a) fa = tc.a;
        if (is_i) fa = I;
        break;
    case 0x2100:                                        /* MODULATE */
        if (has_rgb || has_l || is_i) { fr *= tr; fg *= tg; fb *= tb; }
        if (has_a) fa *= tc.a;
        if (is_i) fa *= I;
        break;
    case 0x2101:                                        /* DECAL (RGB, RGBA) */
        if (fmt == 0x1907) { fr = tc.r; fg = tc.g; fb = tc.b; }
        if (fmt == 0x1908) {
            fr = fr * (1 - tc.a) + tc.r * tc.a;
            fg = fg * (1 - tc.a) + tc.g * tc.a;
            fb = fb * (1 - tc.a) + tc.b * tc.a;
        }
        break;
    case 0x0BE2:                                        /* BLEND */
        if (has_rgb || has_l || is_i) {
            fr = fr * (1 - tr) + cr * tr;
            fg = fg * (1 - tg) + cg * tg;
            fb = fb * (1 - tb) + cb * tb;
        }
        if (has_a) fa *= tc.a;
        if (is_i) fa = fa * (1 - I) + ca * I;
        break;
    case 0x0104:                                        /* ADD */
        if (has_rgb || has_l || is_i) { fr += tr; fg += tg; fb += tb; }
        if (has_a) fa *= tc.a;
        if (is_i) fa += I;
        break;
    }
    *r = clamp01(fr); *g = clamp01(fg); *b = clamp01(fb); *a = clamp01(fa);
}

static QgpuTexture *const no_tex[QGPU_MAX_UNITS];

/* Triangle générique : `words` mots par sommet, 0 à 4 unités de texture. */
static void soft_tri(QgpuSurface *s, const QgpuState *st, QgpuTexture *const *tex,
                     const float *v0, const float *v1, const float *v2, uint32_t prim,
                     int sec_off)
{
    uint32_t nlevels[QGPU_MAX_UNITS];
    float lod[QGPU_MAX_UNITS];
    SoftSurface *ss = s->priv;
    float area = edge(v0[0], v0[1], v1[0], v1[1], v2[0], v2[1]);
    float sign = 1.0f;
    float minx, maxx, miny, maxy, zoff = 0.0f;
    int cx0, cy0, cx1, cy1, x0, x1, y0, y1, x, y, u;
    bool dtest = st->v[QGPU_SK_DEPTH_TEST] && ss->depth;
    bool dwrite = dtest && st->v[QGPU_SK_DEPTH_WRITE];
    bool atest = st->v[QGPU_SK_ALPHA_TEST];
    /* v6 : sans tampon de stencil, le test est inopérant, comme en OpenGL. */
    bool stest = st->v[QGPU_SK_STENCIL_TEST] && ss->stencil;
    uint32_t sfunc = st->v[QGPU_SK_STENCIL_FUNC];
    uint32_t sref = st->v[QGPU_SK_STENCIL_REF];
    uint32_t svmask = st->v[QGPU_SK_STENCIL_VALUE_MASK];
    uint32_t swmask = st->v[QGPU_SK_STENCIL_WRITE_MASK];
    bool fog = st->v[QGPU_SK_FOG];
    uint32_t fc = st->v[QGPU_SK_FOG_COLOR];
    float fcr = ((fc >> 16) & 255) / 255.0f, fcg = ((fc >> 8) & 255) / 255.0f;
    float fcb = (fc & 255) / 255.0f;
    float aref = clamp01(qgpu_u2f(st->v[QGPU_SK_ALPHA_REF]));
    uint32_t cmask = st->v[QGPU_SK_COLOR_MASK];

    if (area == 0.0f || area != area) {   /* dégénéré ou NaN */
        return;
    }
    if (area < 0.0f) {
        sign = -1.0f;
        area = -area;
    }
    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        nlevels[u] = tex[u] ? qgpu_texture_levels(tex[u]) : 0;
        lod[u] = 0.0f;
        if (tex[u]) {
            /* λ par triangle : ρ² = aire en texels / aire en pixels */
            const QgpuTexLevel *l0 = &tex[u]->level[0];
            int k = 8 + 4 * u;
            float s0 = v0[k] / v0[k + 3], t0 = v0[k + 1] / v0[k + 3];
            float s1 = v1[k] / v1[k + 3], t1 = v1[k + 1] / v1[k + 3];
            float s2 = v2[k] / v2[k + 3], t2 = v2[k + 1] / v2[k + 3];
            float ta = fabsf((s1 - s0) * (t2 - t0) - (s2 - s0) * (t1 - t0)) * l0->w * l0->h;
            lod[u] = (ta > 0.0f) ? 0.5f * log2f(ta / area) : 0.0f;
        }
    }
    if (prim == QGPU_PRIM_TRIANGLES && st->v[QGPU_SK_POLY_OFFSET]) {
        /* glPolygonOffset : facteur × pente max + unités × résolution (24 bits) */
        float dzdx = ((v1[2] - v0[2]) * (v2[1] - v0[1]) - (v2[2] - v0[2]) * (v1[1] - v0[1])) / (sign * area);
        float dzdy = ((v2[2] - v0[2]) * (v1[0] - v0[0]) - (v1[2] - v0[2]) * (v2[0] - v0[0])) / (sign * area);
        float m = fabsf(dzdx) > fabsf(dzdy) ? fabsf(dzdx) : fabsf(dzdy);
        zoff = qgpu_u2f(st->v[QGPU_SK_POLY_FACTOR]) * m +
               qgpu_u2f(st->v[QGPU_SK_POLY_UNITS]) / 16777216.0f;
    }

    clip_rect(s, st, &cx0, &cy0, &cx1, &cy1);
    minx = fminf(v0[0], fminf(v1[0], v2[0]));
    maxx = fmaxf(v0[0], fmaxf(v1[0], v2[0]));
    miny = fminf(v0[1], fminf(v1[1], v2[1]));
    maxy = fmaxf(v0[1], fmaxf(v1[1], v2[1]));
    x0 = (int)floorf(minx); if (x0 < cx0) x0 = cx0;
    y0 = (int)floorf(miny); if (y0 < cy0) y0 = cy0;
    x1 = (int)ceilf(maxx);  if (x1 > cx1) x1 = cx1;
    y1 = (int)ceilf(maxy);  if (y1 > cy1) y1 = cy1;

    for (y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        for (x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = sign * edge(v1[0], v1[1], v2[0], v2[1], px, py);
            float w1 = sign * edge(v2[0], v2[1], v0[0], v0[1], px, py);
            float w2 = sign * edge(v0[0], v0[1], v1[0], v1[1], px, py);
            float r, g, b, a, z;
            Rgba prim_c;
            size_t idx;

            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                continue;
            }
            w0 /= area; w1 /= area; w2 /= area;
            r = w0 * v0[4] + w1 * v1[4] + w2 * v2[4];
            g = w0 * v0[5] + w1 * v1[5] + w2 * v2[5];
            b = w0 * v0[6] + w1 * v1[6] + w2 * v2[6];
            a = w0 * v0[7] + w1 * v1[7] + w2 * v2[7];
            z = clamp01(w0 * v0[2] + w1 * v1[2] + w2 * v2[2] + zoff);
            idx = (size_t)y * s->width + x;
            prim_c.r = clamp01(r); prim_c.g = clamp01(g);
            prim_c.b = clamp01(b); prim_c.a = clamp01(a);
            for (u = 0; u < QGPU_MAX_UNITS; u++) {
                int k = 8 + 4 * u;
                float ts, tt, tq;
                if (!tex[u]) {
                    continue;
                }
                ts = w0 * v0[k] + w1 * v1[k] + w2 * v2[k];
                tt = w0 * v0[k + 1] + w1 * v1[k + 1] + w2 * v2[k + 1];
                tq = w0 * v0[k + 3] + w1 * v1[k + 3] + w2 * v2[k + 3];
                if (tq != 0.0f) {
                    Rgba tc = sample(tex[u], nlevels[u], ts / tq, tt / tq, lod[u]);
                    tex_env(st, u, tex[u]->base_format, tc, &prim_c, &r, &g, &b, &a);
                }
            }
            if (sec_off >= 0) {
                /* v7 : la couleur secondaire s'ajoute après l'environnement de
                   texture et avant le brouillard, comme en OpenGL. */
                r += w0 * v0[sec_off] + w1 * v1[sec_off] + w2 * v2[sec_off];
                g += w0 * v0[sec_off + 1] + w1 * v1[sec_off + 1] + w2 * v2[sec_off + 1];
                b += w0 * v0[sec_off + 2] + w1 * v1[sec_off + 2] + w2 * v2[sec_off + 2];
            }
            if (fog) {
                float f = clamp01(w0 * v0[3] + w1 * v1[3] + w2 * v2[3]);
                r = f * clamp01(r) + (1 - f) * fcr;
                g = f * clamp01(g) + (1 - f) * fcg;
                b = f * clamp01(b) + (1 - f) * fcb;
            }

            if (atest && !compare(st->v[QGPU_SK_ALPHA_FUNC], clamp01(a), aref)) {
                continue;
            }
            if (stest) {
                /* Le test de stencil précède celui de profondeur, et c'est le
                   RÉSULTAT de ce dernier qui choisit entre zfail et zpass.
                   Sans tampon de profondeur ou test coupé, le fragment « passe »
                   la profondeur : c'est zpass. */
                uint8_t sv = ss->stencil[idx];
                bool spass = compare_u(sfunc, sref & svmask, (uint32_t)sv & svmask);
                bool zpass = spass && (!dtest ||
                    compare(st->v[QGPU_SK_DEPTH_FUNC], z, ss->depth[idx]));
                uint32_t sop = !spass ? st->v[QGPU_SK_STENCIL_OP_FAIL] :
                               zpass ? st->v[QGPU_SK_STENCIL_OP_ZPASS] :
                                       st->v[QGPU_SK_STENCIL_OP_ZFAIL];
                stencil_write(&ss->stencil[idx], stencil_op(sop, sv, sref), swmask);
                if (!spass || !zpass) {
                    continue;
                }
                if (dwrite) {
                    ss->depth[idx] = z;
                }
            } else if (dtest) {
                if (!compare(st->v[QGPU_SK_DEPTH_FUNC], z, ss->depth[idx])) {
                    continue;
                }
                if (dwrite) {
                    ss->depth[idx] = z;
                }
            }
            ss->px[idx] = masked(ss->px[idx], blend(st, r, g, b, a, ss->px[idx]), cmask);
        }
    }
}

#define MAXW SOFT_RAW_WORDS

/* Segment épais : le parallélogramme d'OpenGL (sans anticrénelage), étiré
   perpendiculairement à l'axe majeur, en deux triangles. */
static void soft_line(QgpuSurface *s, const QgpuState *st, const float *a,
                      const float *b, uint32_t words, int sec_off)
{
    float q[4][MAXW];
    float hw = qgpu_u2f(st->v[QGPU_SK_LINE_WIDTH]) * 0.5f;
    bool xmajor = fabsf(b[0] - a[0]) >= fabsf(b[1] - a[1]);
    int k;
    memcpy(q[0], a, words * sizeof(float));
    memcpy(q[1], a, words * sizeof(float));
    memcpy(q[2], b, words * sizeof(float));
    memcpy(q[3], b, words * sizeof(float));
    for (k = 0; k < 4; k++) {
        float d = (k == 0 || k == 3) ? -hw : hw;
        if (xmajor) {
            q[k][1] += d;
        } else {
            q[k][0] += d;
        }
    }
    soft_tri(s, st, no_tex, q[0], q[1], q[2], QGPU_PRIM_LINES, sec_off);
    soft_tri(s, st, no_tex, q[0], q[2], q[3], QGPU_PRIM_LINES, sec_off);
}

/* Point : carré de côté QGPU_SK_POINT_SIZE centré sur le sommet. */
static void soft_point(QgpuSurface *s, const QgpuState *st, const float *v, uint32_t words,
                       int sec_off)
{
    float q[4][MAXW];
    float h = qgpu_u2f(st->v[QGPU_SK_POINT_SIZE]) * 0.5f;
    static const float dx[4] = { -1, 1, 1, -1 }, dy[4] = { -1, -1, 1, 1 };
    int k;
    for (k = 0; k < 4; k++) {
        memcpy(q[k], v, words * sizeof(float));
        q[k][0] += dx[k] * h;
        q[k][1] += dy[k] * h;
    }
    soft_tri(s, st, no_tex, q[0], q[1], q[2], QGPU_PRIM_POINTS, sec_off);
    soft_tri(s, st, no_tex, q[0], q[2], q[3], QGPU_PRIM_POINTS, sec_off);
}

static bool soft_draw(QgpuCore *c, QgpuSurface *s, const QgpuState *st, uint32_t prim,
                      QgpuTexture *const *tex,
                      const float *verts, uint32_t nverts, uint32_t words)
{
    uint32_t i;
    (void)c;

    switch (prim) {
    case QGPU_PRIM_TRIANGLES:
        for (i = 0; i + 2 < nverts; i += 3) {
            soft_tri(s, st, tex, verts + i * words, verts + (i + 1) * words,
                     verts + (i + 2) * words, prim, -1);
        }
        break;
    case QGPU_PRIM_LINES:
        for (i = 0; i + 1 < nverts; i += 2) {
            soft_line(s, st, verts + i * words, verts + (i + 1) * words, words, -1);
        }
        break;
    default:
        for (i = 0; i < nverts; i++) {
            soft_point(s, st, verts + i * words, words, -1);
        }
        break;
    }
    return true;
}

/* ═══════════════ v7 : étage géométrique de référence ═══════════════════════
 *
 * Tout le pipeline fixe d'OpenGL 1.x, écrit pour être LU (c'est la vérité
 * terrain des tests, pas un chemin rapide) : transformation modèle-vue et
 * projection, éclairage, normalisation, génération de coordonnées, découpe
 * (les six plans du volume de vue et les plans utilisateur), division
 * perspective, viewport, élimination des faces, assemblage des dix modes de
 * primitives, brouillard. La sortie est faite de sommets au format INTERNE
 * (celui du chemin existant, allongé de la couleur secondaire) : ce sont
 * soft_tri / soft_line / soft_point qui rastérisent, exactement comme pour les
 * opcodes v1–v6. Un seul rasteriseur, donc un seul jeu de règles de remplissage.
 *
 * Repère : le viewport est donné en coordonnées OpenGL (origine EN BAS à
 * gauche de la surface). La ligne de surface vaut « hauteur − yw », et le SENS
 * DES FACES est établi AVANT ce retournement, sur les coordonnées fenêtre GL :
 * GL_CCW veut donc dire la même chose côté invité et côté hôte.
 */

/* Sommet au sortir de l'étage sommet. Un seul tableau de flottants : la
   découpe interpole alors tous les attributs par une boucle, sans oubli. */
enum {
    GV_CLIP = 0,          /* 4 : position en espace de découpe */
    GV_EYE  = 4,          /* 4 : position en coordonnées œil (plans utilisateur) */
    GV_COL  = 8,          /* 4 : couleur primaire, face avant */
    GV_BCOL = 12,         /* 4 : couleur primaire, face arrière */
    GV_SEC  = 16,         /* 3 : couleur secondaire, face avant */
    GV_BSEC = 19,         /* 3 : couleur secondaire, face arrière */
    GV_FOG  = 22,         /* 1 : facteur de brouillard */
    GV_TC   = 23,         /* 4 par unité : coordonnées de texture finales */
    GV_N    = GV_TC + 4 * QGPU_MAX_UNITS
};
typedef struct GVert { float v[GV_N]; } GVert;

/* 3 sommets + au plus un par plan de découpe (6 + 6) : 16 suffit, 32 rassure. */
#define GV_MAXPOLY 32

typedef struct Geo {
    const QgpuState    *st;
    const QgpuGeom     *gm;
    QgpuSurface        *s;
    QgpuTexture *const *tex;
    float    inv3[9];          /* inverse 3×3 de la modèle-vue, rangée par LIGNES */
    float    rescale;          /* facteur de GL_RESCALE_NORMAL */
    bool     lighting, two_side, sep_spec, local_viewer, color_material;
    bool     cm_front, cm_back, flat;
    uint32_t cm_mode, fog_mode;
    int      pos_n, off_n, off_c, off_sc, off_f, off_t[QGPU_MAX_UNITS];
    int      sec_off;          /* mot de la couleur secondaire, ou -1 */
    float    vx, vy, vw, vh, dn, df;
} Geo;

/* m est en ORDRE COLONNE : m[4c + r] est la ligne r, colonne c. */
static void mat_vec4(const float *m, const float *v, float *o)
{
    int i;
    for (i = 0; i < 4; i++) {
        o[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
    }
}

static float vdot3(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float vdot4(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static void vnorm3(float *v)
{
    float l = sqrtf(vdot3(v, v));
    if (l > 0.0f) {
        v[0] /= l; v[1] /= l; v[2] /= l;
    }
}

/* Inverse du bloc 3×3 supérieur gauche, rangé par LIGNES (o[3r + c]). */
static bool mat3_inverse(const float *m, float *o)
{
    float a00 = m[0], a01 = m[4], a02 = m[8];
    float a10 = m[1], a11 = m[5], a12 = m[9];
    float a20 = m[2], a21 = m[6], a22 = m[10];
    float c00 =  (a11 * a22 - a12 * a21);
    float c01 = -(a10 * a22 - a12 * a20);
    float c02 =  (a10 * a21 - a11 * a20);
    float det = a00 * c00 + a01 * c01 + a02 * c02;

    if (det == 0.0f || det != det) {
        return false;
    }
    o[0] = c00 / det;
    o[1] = -(a01 * a22 - a02 * a21) / det;
    o[2] =  (a01 * a12 - a02 * a11) / det;
    o[3] = c01 / det;
    o[4] =  (a00 * a22 - a02 * a20) / det;
    o[5] = -(a00 * a12 - a02 * a10) / det;
    o[6] = c02 / det;
    o[7] = -(a00 * a21 - a01 * a20) / det;
    o[8] =  (a00 * a11 - a01 * a10) / det;
    return true;
}

/* GL : n' = n · M⁻¹ (vecteur ligne), soit (M⁻¹)ᵀ · n en colonne. */
static void xform_normal(const float *inv3, const float *n, float *o)
{
    o[0] = inv3[0] * n[0] + inv3[3] * n[1] + inv3[6] * n[2];
    o[1] = inv3[1] * n[0] + inv3[4] * n[1] + inv3[7] * n[2];
    o[2] = inv3[2] * n[0] + inv3[5] * n[1] + inv3[8] * n[2];
}

/* Éclairage d'OpenGL 1.x, formule de la spécification, pour UNE face.
   `nrm` est déjà retournée quand on éclaire la face arrière. */
static void light_face(const Geo *G, const float *eye, const float *nrm,
                       const float *vcol, int face, float *col, float *sec)
{
    const QgpuGeom *gm = G->gm;
    const QgpuMaterial *m = &gm->mat[face];
    float amb[4], dif[4], spc[4], emi[4], shin = m->shininess;
    float acc[3], sacc[3];
    float pe[3], vpe[3];
    bool cm = G->color_material && (face == 0 ? G->cm_front : G->cm_back);
    int i, k;

    memcpy(amb, m->ambient, sizeof(amb));
    memcpy(dif, m->diffuse, sizeof(dif));
    memcpy(spc, m->specular, sizeof(spc));
    memcpy(emi, m->emission, sizeof(emi));
    if (cm) {
        switch (G->cm_mode) {
        case 0x1600: memcpy(emi, vcol, sizeof(emi)); break;      /* GL_EMISSION */
        case 0x1200: memcpy(amb, vcol, sizeof(amb)); break;      /* GL_AMBIENT */
        case 0x1201: memcpy(dif, vcol, sizeof(dif)); break;      /* GL_DIFFUSE */
        case 0x1202: memcpy(spc, vcol, sizeof(spc)); break;      /* GL_SPECULAR */
        default:                                                 /* AMBIENT_AND_DIFFUSE */
            memcpy(amb, vcol, sizeof(amb));
            memcpy(dif, vcol, sizeof(dif));
            break;
        }
    }
    for (k = 0; k < 3; k++) {
        pe[k] = (eye[3] != 0.0f) ? eye[k] / eye[3] : eye[k];
        acc[k] = emi[k] + amb[k] * gm->lm_ambient[k];
        sacc[k] = 0.0f;
    }
    if (G->local_viewer) {
        vpe[0] = -pe[0]; vpe[1] = -pe[1]; vpe[2] = -pe[2];
        vnorm3(vpe);
    } else {
        vpe[0] = 0.0f; vpe[1] = 0.0f; vpe[2] = 1.0f;
    }
    for (i = 0; i < QGPU_MAX_LIGHTS; i++) {
        const QgpuLight *l = &gm->light[i];
        float vp[3], att = 1.0f, spot = 1.0f, ndotvp, f;
        if (!l->enabled) {
            continue;
        }
        if (l->position[3] != 0.0f) {
            float d, den;
            for (k = 0; k < 3; k++) {
                vp[k] = l->position[k] / l->position[3] - pe[k];
            }
            d = sqrtf(vdot3(vp, vp));
            if (d > 0.0f) {
                vp[0] /= d; vp[1] /= d; vp[2] /= d;
            }
            den = l->att[0] + l->att[1] * d + l->att[2] * d * d;
            att = (den > 0.0f) ? 1.0f / den : 1.0f;
            if (l->spot_cutoff != 180.0f) {
                float sd[3], cosang;
                memcpy(sd, l->spot_dir, sizeof(sd));
                vnorm3(sd);
                /* angle entre la direction lumière → sommet et l'axe du spot */
                cosang = -vdot3(vp, sd);
                if (cosang <= 0.0f ||
                    cosang < cosf(l->spot_cutoff * 3.14159265358979f / 180.0f)) {
                    spot = 0.0f;
                } else {
                    spot = powf(cosang, l->spot_exp);
                }
            }
        } else {
            memcpy(vp, l->position, 3 * sizeof(float));
            vnorm3(vp);                       /* lumière directionnelle */
        }
        f = att * spot;
        for (k = 0; k < 3; k++) {
            acc[k] += f * amb[k] * l->ambient[k];
        }
        ndotvp = vdot3(nrm, vp);
        if (ndotvp > 0.0f) {
            float h[3], ndoth, sf;
            for (k = 0; k < 3; k++) {
                acc[k] += f * ndotvp * dif[k] * l->diffuse[k];
                h[k] = vp[k] + vpe[k];
            }
            vnorm3(h);
            ndoth = vdot3(nrm, h);
            sf = (ndoth > 0.0f) ? powf(ndoth, shin) : 0.0f;
            for (k = 0; k < 3; k++) {
                float v = f * sf * spc[k] * l->specular[k];
                if (G->sep_spec) {
                    sacc[k] += v;
                } else {
                    acc[k] += v;
                }
            }
        }
    }
    for (k = 0; k < 3; k++) {
        col[k] = clamp01(acc[k]);
        sec[k] = clamp01(sacc[k]);
    }
    col[3] = clamp01(dif[3]);                 /* GL : l'alpha vient de la diffuse */
}

/* Génération des coordonnées d'une unité, les cinq modes d'OpenGL 1.3. */
static void texgen_unit(const Geo *G, int u, const float *obj, const float *eye,
                        const float *nrm, float *tcv)
{
    const QgpuTexgen *tg = G->gm->texgen[u];
    float uv[3], refl[3], m = 1.0f;
    bool need_refl = false;
    int k;

    for (k = 0; k < 4; k++) {
        if (tg[k].enabled && (tg[k].mode == QGPU_TG_SPHERE_MAP ||
                              tg[k].mode == QGPU_TG_REFLECTION_MAP)) {
            need_refl = true;
        }
    }
    if (need_refl) {
        float d;
        for (k = 0; k < 3; k++) {
            uv[k] = (eye[3] != 0.0f) ? eye[k] / eye[3] : eye[k];
        }
        vnorm3(uv);                            /* u : origine → sommet, en œil */
        d = 2.0f * vdot3(nrm, uv);
        for (k = 0; k < 3; k++) {
            refl[k] = uv[k] - d * nrm[k];
        }
        m = 2.0f * sqrtf(refl[0] * refl[0] + refl[1] * refl[1] +
                         (refl[2] + 1.0f) * (refl[2] + 1.0f));
    }
    for (k = 0; k < 4; k++) {
        if (!tg[k].enabled) {
            continue;
        }
        switch (tg[k].mode) {
        case QGPU_TG_OBJECT_LINEAR:
            tcv[k] = vdot4(tg[k].obj_plane, obj);
            break;
        case QGPU_TG_EYE_LINEAR:
            tcv[k] = vdot4(tg[k].eye_plane, eye);
            break;
        case QGPU_TG_SPHERE_MAP:
            tcv[k] = (m != 0.0f ? refl[k] / m : 0.0f) + 0.5f;
            break;
        case QGPU_TG_NORMAL_MAP:
            tcv[k] = nrm[k];
            break;
        default:                               /* QGPU_TG_REFLECTION_MAP */
            tcv[k] = refl[k];
            break;
        }
    }
}

static float fog_factor(const Geo *G, const float *eye, float fogc)
{
    float c, f, d;

    if (G->fog_mode == QGPU_FOG_VERTEX) {
        return clamp01(fogc);                  /* v4 : le sommet donne le facteur */
    }
    /* Sans coordonnée de brouillard dans le sommet, GL prend |z œil|. */
    c = (G->off_f >= 0) ? fogc : fabsf(eye[2]);
    switch (G->fog_mode) {
    case QGPU_FOG_LINEAR: {
        float s = qgpu_u2f(G->st->v[QGPU_SK_FOG_START]);
        float e = qgpu_u2f(G->st->v[QGPU_SK_FOG_END]);
        f = (e == s) ? 1.0f : (e - c) / (e - s);
        break;
    }
    case QGPU_FOG_EXP:
        d = qgpu_u2f(G->st->v[QGPU_SK_FOG_DENSITY]);
        f = expf(-d * c);
        break;
    default:                                   /* QGPU_FOG_EXP2 */
        d = qgpu_u2f(G->st->v[QGPU_SK_FOG_DENSITY]) * c;
        f = expf(-d * d);
        break;
    }
    return clamp01(f);
}

/* Étage sommet : un sommet brut du protocole → un GVert prêt à découper. */
static void vstage(const Geo *G, const float *src, GVert *g)
{
    const QgpuGeom *gm = G->gm;
    float obj[4], eye[4], nrm[3], neye[3], vcol[4], vsec[3], fogc;
    int u, k;

    obj[0] = src[0];
    obj[1] = src[1];
    obj[2] = (G->pos_n >= 3) ? src[2] : 0.0f;
    obj[3] = (G->pos_n >= 4) ? src[3] : 1.0f;
    memcpy(nrm, G->off_n >= 0 ? src + G->off_n : gm->cur_normal, 3 * sizeof(float));
    memcpy(vcol, G->off_c >= 0 ? src + G->off_c : gm->cur_color, 4 * sizeof(float));
    memcpy(vsec, G->off_sc >= 0 ? src + G->off_sc : gm->cur_sec, 3 * sizeof(float));
    fogc = (G->off_f >= 0) ? src[G->off_f] : gm->cur_fog;

    mat_vec4(gm->mtx[QGPU_MTX_MODELVIEW], obj, eye);
    xform_normal(G->inv3, nrm, neye);
    if (G->st->v[QGPU_SK_NORMALIZE]) {
        vnorm3(neye);
    } else if (G->st->v[QGPU_SK_RESCALE_NORMAL]) {
        for (k = 0; k < 3; k++) {
            neye[k] *= G->rescale;
        }
    }

    if (G->lighting) {
        light_face(G, eye, neye, vcol, 0, g->v + GV_COL, g->v + GV_SEC);
        if (G->two_side) {
            float back[3];
            for (k = 0; k < 3; k++) {
                back[k] = -neye[k];
            }
            light_face(G, eye, back, vcol, 1, g->v + GV_BCOL, g->v + GV_BSEC);
        } else {
            memcpy(g->v + GV_BCOL, g->v + GV_COL, 4 * sizeof(float));
            memcpy(g->v + GV_BSEC, g->v + GV_SEC, 3 * sizeof(float));
        }
    } else {
        memcpy(g->v + GV_COL, vcol, 4 * sizeof(float));
        memcpy(g->v + GV_BCOL, vcol, 4 * sizeof(float));
        memcpy(g->v + GV_SEC, vsec, 3 * sizeof(float));
        memcpy(g->v + GV_BSEC, vsec, 3 * sizeof(float));
    }

    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        float gen[4];
        memcpy(gen, G->off_t[u] >= 0 ? src + G->off_t[u] : gm->cur_tex[u],
               4 * sizeof(float));
        texgen_unit(G, u, obj, eye, neye, gen);
        mat_vec4(gm->mtx[QGPU_MTX_TEXTURE0 + u], gen, g->v + GV_TC + 4 * u);
    }

    mat_vec4(gm->mtx[QGPU_MTX_PROJECTION], eye, g->v + GV_CLIP);
    memcpy(g->v + GV_EYE, eye, 4 * sizeof(float));
    g->v[GV_FOG] = fog_factor(G, eye, fogc);
}

/* Distance signée au plan `p` : 0..5 = volume de vue (espace de découpe),
   6.. = plans utilisateur (coordonnées œil). */
static float plane_dist(const Geo *G, int p, const GVert *v)
{
    const float *c = v->v + GV_CLIP;
    switch (p) {
    case 0:  return c[3] + c[0];
    case 1:  return c[3] - c[0];
    case 2:  return c[3] + c[1];
    case 3:  return c[3] - c[1];
    case 4:  return c[3] + c[2];
    case 5:  return c[3] - c[2];
    default: return vdot4(G->gm->clip[p - 6].eq, v->v + GV_EYE);
    }
}

static bool plane_live(const Geo *G, int p)
{
    return p < 6 || G->gm->clip[p - 6].enabled;
}

static void gv_lerp(GVert *o, const GVert *a, const GVert *b, float t)
{
    int i;
    for (i = 0; i < GV_N; i++) {
        o->v[i] = a->v[i] + t * (b->v[i] - a->v[i]);
    }
}

/* Sutherland-Hodgman sur tous les plans actifs ; renvoie le nouveau nombre de
   sommets, 0 si le polygone disparaît. */
static int clip_poly(const Geo *G, GVert *poly, int n)
{
    GVert tmp[GV_MAXPOLY];
    int p, i, m;

    for (p = 0; p < 6 + QGPU_MAX_CLIP_PLANES && n >= 3; p++) {
        if (!plane_live(G, p)) {
            continue;
        }
        m = 0;
        for (i = 0; i < n; i++) {
            const GVert *a = &poly[i], *b = &poly[(i + 1) % n];
            float da = plane_dist(G, p, a), db = plane_dist(G, p, b);
            if (da >= 0.0f && m < GV_MAXPOLY) {
                tmp[m++] = *a;
            }
            if ((da >= 0.0f) != (db >= 0.0f) && m < GV_MAXPOLY) {
                gv_lerp(&tmp[m++], a, b, da / (da - db));
            }
        }
        n = m;
        memcpy(poly, tmp, (size_t)n * sizeof(GVert));
    }
    return n >= 3 ? n : 0;
}

/* Division perspective, viewport, profondeur, puis mise au format interne. */
static void project(const Geo *G, const GVert *g, bool back, float *out)
{
    float w = g->v[GV_CLIP + 3];
    float iw = (w != 0.0f) ? 1.0f / w : 0.0f;
    float xd = g->v[GV_CLIP] * iw, yd = g->v[GV_CLIP + 1] * iw;
    float zd = g->v[GV_CLIP + 2] * iw;
    const float *col = g->v + (back ? GV_BCOL : GV_COL);
    const float *sec = g->v + (back ? GV_BSEC : GV_SEC);
    int u, k;

    out[0] = G->vx + (xd + 1.0f) * 0.5f * G->vw;
    /* passage au repère de surface : origine en haut, y vers le bas */
    out[1] = (float)G->s->height - (G->vy + (yd + 1.0f) * 0.5f * G->vh);
    out[2] = clamp01(G->dn + (zd + 1.0f) * 0.5f * (G->df - G->dn));
    out[3] = g->v[GV_FOG];
    for (k = 0; k < 4; k++) {
        out[4 + k] = col[k];
    }
    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        for (k = 0; k < 4; k++) {
            /* le rasteriseur attend s/w, t/w, r/w, q/w (cf. DRAW_TRIANGLES_TEX) */
            out[8 + 4 * u + k] = g->v[GV_TC + 4 * u + k] * iw;
        }
    }
    for (k = 0; k < 3; k++) {
        out[SOFT_SEC_OFF + k] = sec[k];
    }
}

static void raw_tri(const Geo *G, const GVert *a, const GVert *b, const GVert *c)
{
    GVert poly[GV_MAXPOLY];
    float sv[GV_MAXPOLY][SOFT_RAW_WORDS];
    float area = 0.0f;
    bool front;
    int n, i;

    poly[0] = *a; poly[1] = *b; poly[2] = *c;
    n = clip_poly(G, poly, 3);
    if (!n) {
        return;
    }
    for (i = 0; i < n; i++) {
        project(G, &poly[i], false, sv[i]);
    }
    /* Aire signée en coordonnées fenêtre GL. sv[] a y vers le bas, d'où le
       signe inversé : c'est ce qui fait que GL_CCW veut dire la même chose
       ici que chez l'invité. */
    for (i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += sv[i][0] * sv[j][1] - sv[j][0] * sv[i][1];
    }
    area = -area;
    front = (area > 0.0f) == (G->st->v[QGPU_SK_FRONT_FACE] == 0x0901);
    if (G->st->v[QGPU_SK_CULL_FACE]) {
        uint32_t cm = G->st->v[QGPU_SK_CULL_MODE];
        if (cm == 0x0408 || (cm == 0x0405 && !front) || (cm == 0x0404 && front)) {
            return;
        }
    }
    if (!front) {
        for (i = 0; i < n; i++) {
            project(G, &poly[i], true, sv[i]);     /* couleurs de la face arrière */
        }
    }
    for (i = 1; i + 1 < n; i++) {
        soft_tri(G->s, G->st, G->tex, sv[0], sv[i], sv[i + 1],
                 QGPU_PRIM_TRIANGLES, G->sec_off);
    }
}

static void raw_line(const Geo *G, const GVert *a, const GVert *b)
{
    GVert p = *a, q = *b, t;
    float sa[SOFT_RAW_WORDS], sb[SOFT_RAW_WORDS];
    int pl;

    for (pl = 0; pl < 6 + QGPU_MAX_CLIP_PLANES; pl++) {
        float da, db;
        if (!plane_live(G, pl)) {
            continue;
        }
        da = plane_dist(G, pl, &p);
        db = plane_dist(G, pl, &q);
        if (da < 0.0f && db < 0.0f) {
            return;
        }
        if (da < 0.0f) {
            gv_lerp(&t, &p, &q, da / (da - db));
            p = t;
        } else if (db < 0.0f) {
            gv_lerp(&t, &p, &q, da / (da - db));
            q = t;
        }
    }
    project(G, &p, false, sa);
    project(G, &q, false, sb);
    soft_line(G->s, G->st, sa, sb, SOFT_RAW_WORDS, G->sec_off);
}

static void raw_point(const Geo *G, const GVert *a)
{
    float sa[SOFT_RAW_WORDS];
    int pl;

    for (pl = 0; pl < 6 + QGPU_MAX_CLIP_PLANES; pl++) {
        if (plane_live(G, pl) && plane_dist(G, pl, a) < 0.0f) {
            return;
        }
    }
    project(G, a, false, sa);
    soft_point(G->s, G->st, sa, SOFT_RAW_WORDS, G->sec_off);
}

/* Ombrage plat : la primitive entière prend les couleurs du sommet dit
   « provoquant » (OpenGL 1.x, table 2.12). */
static void flat_set(GVert *d, const GVert *src, const GVert *prov)
{
    *d = *src;
    memcpy(d->v + GV_COL, prov->v + GV_COL, 4 * sizeof(float));
    memcpy(d->v + GV_BCOL, prov->v + GV_BCOL, 4 * sizeof(float));
    memcpy(d->v + GV_SEC, prov->v + GV_SEC, 3 * sizeof(float));
    memcpy(d->v + GV_BSEC, prov->v + GV_BSEC, 3 * sizeof(float));
}

static void emit_tri(const Geo *G, const GVert *a, const GVert *b, const GVert *c,
                     const GVert *prov)
{
    if (G->flat) {
        GVert t[3];
        flat_set(&t[0], a, prov);
        flat_set(&t[1], b, prov);
        flat_set(&t[2], c, prov);
        raw_tri(G, &t[0], &t[1], &t[2]);
    } else {
        raw_tri(G, a, b, c);
    }
}

static void emit_line(const Geo *G, const GVert *a, const GVert *b, const GVert *prov)
{
    if (G->flat) {
        GVert t[2];
        flat_set(&t[0], a, prov);
        flat_set(&t[1], b, prov);
        raw_line(G, &t[0], &t[1]);
    } else {
        raw_line(G, a, b);
    }
}

static const GVert *gv_at(const GVert *gv, const uint32_t *idx, uint32_t first,
                          uint32_t i)
{
    return idx ? &gv[idx[i]] : &gv[first + i];
}

/* Assemblage des dix modes, avec le sommet provoquant de chacun. */
static void assemble(const Geo *G, const GVert *gv, const uint32_t *idx,
                     uint32_t first, uint32_t count, uint32_t mode)
{
    uint32_t i;
#define V(k) gv_at(gv, idx, first, (k))

    switch (mode) {
    case QGPU_PRIM_MODE_POINTS:
        for (i = 0; i < count; i++) {
            raw_point(G, V(i));
        }
        break;
    case QGPU_PRIM_MODE_LINES:
        for (i = 0; i + 1 < count; i += 2) {
            emit_line(G, V(i), V(i + 1), V(i + 1));
        }
        break;
    case QGPU_PRIM_MODE_LINE_STRIP:
        for (i = 0; i + 1 < count; i++) {
            emit_line(G, V(i), V(i + 1), V(i + 1));
        }
        break;
    case QGPU_PRIM_MODE_LINE_LOOP:
        for (i = 0; i + 1 < count; i++) {
            emit_line(G, V(i), V(i + 1), V(i + 1));
        }
        if (count > 2) {
            /* GL : le segment de fermeture prend la couleur du PREMIER sommet */
            emit_line(G, V(count - 1), V(0), V(0));
        }
        break;
    case QGPU_PRIM_MODE_TRIANGLES:
        for (i = 0; i + 2 < count; i += 3) {
            emit_tri(G, V(i), V(i + 1), V(i + 2), V(i + 2));
        }
        break;
    case QGPU_PRIM_MODE_TRIANGLE_STRIP:
        for (i = 0; i + 2 < count; i++) {
            /* un triangle sur deux est retourné, pour garder l'orientation */
            if (i & 1) {
                emit_tri(G, V(i + 1), V(i), V(i + 2), V(i + 2));
            } else {
                emit_tri(G, V(i), V(i + 1), V(i + 2), V(i + 2));
            }
        }
        break;
    case QGPU_PRIM_MODE_TRIANGLE_FAN:
        for (i = 1; i + 1 < count; i++) {
            emit_tri(G, V(0), V(i), V(i + 1), V(i + 1));
        }
        break;
    case QGPU_PRIM_MODE_QUADS:
        for (i = 0; i + 3 < count; i += 4) {
            emit_tri(G, V(i), V(i + 1), V(i + 2), V(i + 3));
            emit_tri(G, V(i), V(i + 2), V(i + 3), V(i + 3));
        }
        break;
    case QGPU_PRIM_MODE_QUAD_STRIP:
        for (i = 0; i + 3 < count; i += 2) {
            emit_tri(G, V(i), V(i + 1), V(i + 3), V(i + 3));
            emit_tri(G, V(i), V(i + 3), V(i + 2), V(i + 3));
        }
        break;
    default:                                   /* QGPU_PRIM_MODE_POLYGON */
        for (i = 1; i + 1 < count; i++) {
            emit_tri(G, V(0), V(i), V(i + 1), V(0));
        }
        break;
    }
#undef V
}

static bool soft_draw_raw(QgpuCore *c, QgpuSurface *s, const QgpuState *st,
                          const QgpuGeom *gm, QgpuTexture *const *tex,
                          uint32_t mode, uint32_t fmt, const float *verts,
                          uint32_t nverts, uint32_t words,
                          const uint32_t *idx, uint32_t count, uint32_t first)
{
    Geo G;
    GVert *gv;
    uint32_t i;
    int u;
    (void)c;

    memset(&G, 0, sizeof(G));
    G.st = st; G.gm = gm; G.s = s; G.tex = tex;
    G.pos_n = QGPU_VF_POS_COUNT(fmt);
    G.off_n = qgpu_vf_offset(fmt, QGPU_VF_NORMAL);
    G.off_c = qgpu_vf_offset(fmt, QGPU_VF_COLOR);
    G.off_sc = qgpu_vf_offset(fmt, QGPU_VF_SEC_COLOR);
    G.off_f = qgpu_vf_offset(fmt, QGPU_VF_FOG);
    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        G.off_t[u] = qgpu_vf_offset(fmt, (uint32_t)QGPU_VF_TEX(u));
    }
    G.lighting = st->v[QGPU_SK_LIGHTING] != 0;
    G.two_side = st->v[QGPU_SK_TWO_SIDE] != 0;
    G.sep_spec = st->v[QGPU_SK_COLOR_CONTROL] == 0x81FA;
    G.local_viewer = st->v[QGPU_SK_LOCAL_VIEWER] != 0;
    G.color_material = st->v[QGPU_SK_COLOR_MATERIAL] != 0;
    G.cm_front = st->v[QGPU_SK_COLOR_MAT_FACE] != 0x0405;      /* pas GL_BACK seul */
    G.cm_back = st->v[QGPU_SK_COLOR_MAT_FACE] != 0x0404;       /* pas GL_FRONT seul */
    G.cm_mode = st->v[QGPU_SK_COLOR_MAT_MODE];
    G.fog_mode = st->v[QGPU_SK_FOG_MODE];
    G.flat = st->v[QGPU_SK_SHADE_MODEL] == 0x1D00;
    /* La couleur secondaire ne coûte son étage que si elle peut être non nulle. */
    G.sec_off = ((G.lighting && G.sep_spec) || G.off_sc >= 0) ? SOFT_SEC_OFF : -1;
    if (!mat3_inverse(gm->mtx[QGPU_MTX_MODELVIEW], G.inv3)) {
        /* Modèle-vue singulière : GL laisse le résultat indéfini ; l'identité
           vaut mieux que des NaN dans le rasteriseur. */
        memset(G.inv3, 0, sizeof(G.inv3));
        G.inv3[0] = G.inv3[4] = G.inv3[8] = 1.0f;
    }
    {
        /* GL_RESCALE_NORMAL : 1/‖3e ligne de l'inverse de la modèle-vue‖. */
        float l = sqrtf(G.inv3[6] * G.inv3[6] + G.inv3[7] * G.inv3[7] +
                        G.inv3[8] * G.inv3[8]);
        G.rescale = (l > 0.0f) ? 1.0f / l : 1.0f;
    }
    if (gm->vp_set) {
        G.vx = (float)gm->vp[0]; G.vy = (float)gm->vp[1];
        G.vw = (float)gm->vp[2]; G.vh = (float)gm->vp[3];
    } else {
        G.vx = 0.0f; G.vy = 0.0f;
        G.vw = (float)s->width; G.vh = (float)s->height;
    }
    G.dn = gm->depth_near;
    G.df = gm->depth_far;

    gv = malloc((size_t)nverts * sizeof(GVert));
    if (!gv) {
        return false;
    }
    for (i = 0; i < nverts; i++) {
        vstage(&G, verts + (size_t)i * words, &gv[i]);
    }
    assemble(&G, gv, idx, first, count, mode);
    free(gv);
    return true;
}

static bool soft_readback(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint32_t *dst)
{
    SoftSurface *ss = s->priv;
    uint32_t row;
    (void)c;

    for (row = 0; row < h; row++) {
        memcpy(dst + (size_t)row * w,
               ss->px + (size_t)(y + row) * s->width + x,
               (size_t)w * sizeof(uint32_t));
    }
    return true;
}

static bool soft_upload(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, const uint32_t *src)
{
    SoftSurface *ss = s->priv;
    uint32_t row;
    (void)c;

    for (row = 0; row < h; row++) {
        memcpy(ss->px + (size_t)(y + row) * s->width + x,
               src + (size_t)row * w, (size_t)w * sizeof(uint32_t));
    }
    return true;
}

static bool soft_depth_readback(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h, float *dst)
{
    SoftSurface *ss = s->priv;
    uint32_t row;
    (void)c;

    for (row = 0; row < h; row++) {
        memcpy(dst + (size_t)row * w,
               ss->depth + (size_t)(y + row) * s->width + x,
               (size_t)w * sizeof(float));
    }
    return true;
}

static bool soft_depth_upload(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h, const float *src)
{
    SoftSurface *ss = s->priv;
    uint32_t row;
    (void)c;

    for (row = 0; row < h; row++) {
        memcpy(ss->depth + (size_t)(y + row) * s->width + x,
               src + (size_t)row * w, (size_t)w * sizeof(float));
    }
    return true;
}

static bool soft_stencil_readback(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                                  uint32_t w, uint32_t h, uint8_t *dst)
{
    SoftSurface *ss = s->priv;
    uint32_t row;
    (void)c;

    for (row = 0; row < h; row++) {
        memcpy(dst + (size_t)row * w,
               ss->stencil + (size_t)(y + row) * s->width + x, w);
    }
    return true;
}

static bool soft_stencil_upload(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h, const uint8_t *src)
{
    SoftSurface *ss = s->priv;
    uint32_t row;
    (void)c;

    for (row = 0; row < h; row++) {
        memcpy(ss->stencil + (size_t)(y + row) * s->width + x,
               src + (size_t)row * w, w);
    }
    return true;
}

const QgpuBackend qgpu_backend_soft = {
    .name           = "soft",
    .cap            = QGPU_CAP_SOFT,
    .init           = soft_init,
    .fini           = soft_fini,
    .surf_create    = soft_surf_create,
    .surf_destroy   = soft_surf_destroy,
    .clear          = soft_clear,
    .draw           = soft_draw,
    .draw_raw       = soft_draw_raw,
    .readback       = soft_readback,
    .upload         = soft_upload,
    .depth_readback = soft_depth_readback,
    .depth_upload   = soft_depth_upload,
    .stencil_readback = soft_stencil_readback,
    .stencil_upload = soft_stencil_upload,
    .tex_destroy    = NULL,                /* les niveaux appartiennent au cœur */
};
