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
                     const float *v0, const float *v1, const float *v2, uint32_t prim)
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

#define MAXW QGPU_VERTEX_MAX_WORDS

/* Segment épais : le parallélogramme d'OpenGL (sans anticrénelage), étiré
   perpendiculairement à l'axe majeur, en deux triangles. */
static void soft_line(QgpuSurface *s, const QgpuState *st, const float *a,
                      const float *b, uint32_t words)
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
    soft_tri(s, st, no_tex, q[0], q[1], q[2], QGPU_PRIM_LINES);
    soft_tri(s, st, no_tex, q[0], q[2], q[3], QGPU_PRIM_LINES);
}

/* Point : carré de côté QGPU_SK_POINT_SIZE centré sur le sommet. */
static void soft_point(QgpuSurface *s, const QgpuState *st, const float *v, uint32_t words)
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
    soft_tri(s, st, no_tex, q[0], q[1], q[2], QGPU_PRIM_POINTS);
    soft_tri(s, st, no_tex, q[0], q[2], q[3], QGPU_PRIM_POINTS);
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
                     verts + (i + 2) * words, prim);
        }
        break;
    case QGPU_PRIM_LINES:
        for (i = 0; i + 1 < nverts; i += 2) {
            soft_line(s, st, verts + i * words, verts + (i + 1) * words, words);
        }
        break;
    default:
        for (i = 0; i < nverts; i++) {
            soft_point(s, st, verts + i * words, words);
        }
        break;
    }
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
    .readback       = soft_readback,
    .upload         = soft_upload,
    .depth_readback = soft_depth_readback,
    .depth_upload   = soft_depth_upload,
    .stencil_readback = soft_stencil_readback,
    .stencil_upload = soft_stencil_upload,
    .tex_destroy    = NULL,                /* les niveaux appartiennent au cœur */
};
