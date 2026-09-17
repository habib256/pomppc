/*
 * qgpu-gl.c — backend OpenGL du GPU paravirtuel « qgpu » : c'est ici que le
 * GPU de l'hôte travaille pour l'invité Tiger.
 *
 * Contexte hors écran :
 *   - macOS : CGL (OpenGL.framework, profil legacy 2.1 — déprécié mais présent,
 *             Metal derrière) ;
 *   - Linux : EGL + pbuffer 1x1 (Mesa, NVIDIA, ANGLE/Zink vers Vulkan…).
 * Chaque surface qgpu est un FBO + texture RGBA8. Les coordonnées invité
 * (origine en haut à gauche) sont projetées par glOrtho(0, w, 0, h) : la ligne
 * 0 de la surface est la ligne 0 de la texture, donc glReadPixels rend les
 * lignes dans l'ordre attendu, sans retournement.
 *
 * Threads : QEMU peut frapper le doorbell depuis n'importe quel thread vCPU
 * (MTTCG à 2 cœurs par défaut dans ce dépôt), et un contexte GL n'est courant
 * que pour un thread. On le rend donc courant AU DÉBUT DE CHAQUE OPÉRATION —
 * c'est bon marché et c'est le seul schéma sûr sans thread de rendu dédié.
 *
 * Sans OpenGL à la compilation, ce fichier fournit un stub dont init() renvoie
 * false : le cœur retombe sur le backend logiciel et QGPU_REG_CAPS le dit.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qgpu-core.h"

#if defined(__APPLE__)
#  define QGPU_GL_CGL 1
#elif defined(__has_include)
#  if __has_include(<EGL/egl.h>) && __has_include(<GL/gl.h>)
#    define QGPU_GL_EGL 1
#  endif
#endif

#if defined(QGPU_GL_CGL) || defined(QGPU_GL_EGL)

#ifdef QGPU_GL_CGL
#  define GL_SILENCE_DEPRECATION 1
#  include <OpenGL/OpenGL.h>
#  include <OpenGL/gl.h>
#  include <OpenGL/glext.h>
#else
#  define GL_GLEXT_PROTOTYPES 1
#  include <EGL/egl.h>
#  include <GL/gl.h>
#  include <GL/glext.h>
#endif

typedef struct GlState {
#ifdef QGPU_GL_CGL
    CGLContextObj ctx;
#else
    EGLDisplay dpy;
    EGLSurface surf;
    EGLContext ctx;
#endif
    /* FBO : résolus dynamiquement pour ne dépendre d'aucune version d'en-tête */
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void (*BlendEquationSeparate)(GLenum, GLenum);
    void (*FogCoordPointer)(GLenum, GLsizei, const GLvoid *);
    void (*ActiveTexture)(GLenum);
    void (*ClientActiveTexture)(GLenum);
    const char *renderer;
} GlState;

typedef struct GlSurface {
    GLuint fbo, tex, depth;        /* depth : texture DEPTH_COMPONENT, ou 0 */
} GlSurface;

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER          0x8D40
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT     0x8D00
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24    0x81A6
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD             0x8006
#endif
#ifndef GL_FOG_COORDINATE_SOURCE
#define GL_FOG_COORDINATE_SOURCE 0x8450
#define GL_FOG_COORDINATE        0x8451
#define GL_FOG_COORDINATE_ARRAY  0x8457
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0             0x84C0
#define GL_TEXTURE1             0x84C1
#endif
#ifndef GL_COMBINE
#define GL_COMBINE              0x8570
#define GL_COMBINE_RGB          0x8571
#define GL_COMBINE_ALPHA        0x8572
#define GL_RGB_SCALE            0x8573
#define GL_ADD_SIGNED           0x8574
#define GL_INTERPOLATE          0x8575
#define GL_CONSTANT             0x8576
#define GL_PRIMARY_COLOR        0x8577
#define GL_PREVIOUS             0x8578
#define GL_SOURCE0_RGB          0x8580
#define GL_SOURCE0_ALPHA        0x8588
#define GL_OPERAND0_RGB         0x8590
#define GL_OPERAND0_ALPHA       0x8598
#define GL_SUBTRACT             0x84E7
#define GL_DOT3_RGB             0x86AE
#define GL_DOT3_RGBA            0x86AF
#endif
#ifndef GL_ALPHA_SCALE
#define GL_ALPHA_SCALE          0x0D1C
#endif

static void *gl_proc(const char *name)
{
#ifdef QGPU_GL_CGL
    /* OpenGL.framework exporte les points d'entrée EXT/ARB directement. */
    extern void *dlsym(void *, const char *);
    extern void *dlopen(const char *, int);
    static void *h;
    if (!h) {
        h = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", 2);
    }
    return h ? dlsym(h, name) : NULL;
#else
    return (void *)eglGetProcAddress(name);
#endif
}

static bool gl_make_current(GlState *g)
{
#ifdef QGPU_GL_CGL
    return CGLSetCurrentContext(g->ctx) == kCGLNoError;
#else
    return eglMakeCurrent(g->dpy, g->surf, g->surf, g->ctx) == EGL_TRUE;
#endif
}

static bool gl_resolve(GlState *g)
{
    /* D'abord les noms cœur (GL 3.0+ / macOS legacy les exporte aussi),
       sinon la variante EXT (GL_EXT_framebuffer_object). */
    g->GenFramebuffers = gl_proc("glGenFramebuffers");
    g->DeleteFramebuffers = gl_proc("glDeleteFramebuffers");
    g->BindFramebuffer = gl_proc("glBindFramebuffer");
    g->FramebufferTexture2D = gl_proc("glFramebufferTexture2D");
    g->CheckFramebufferStatus = gl_proc("glCheckFramebufferStatus");
    if (!g->GenFramebuffers || !g->BindFramebuffer) {
        g->GenFramebuffers = gl_proc("glGenFramebuffersEXT");
        g->DeleteFramebuffers = gl_proc("glDeleteFramebuffersEXT");
        g->BindFramebuffer = gl_proc("glBindFramebufferEXT");
        g->FramebufferTexture2D = gl_proc("glFramebufferTexture2DEXT");
        g->CheckFramebufferStatus = gl_proc("glCheckFramebufferStatusEXT");
    }
    g->BlendFuncSeparate = gl_proc("glBlendFuncSeparate");
    g->BlendEquationSeparate = gl_proc("glBlendEquationSeparate");
    g->FogCoordPointer = gl_proc("glFogCoordPointer");
    g->ActiveTexture = gl_proc("glActiveTexture");
    g->ClientActiveTexture = gl_proc("glClientActiveTexture");
    return g->GenFramebuffers && g->DeleteFramebuffers && g->BindFramebuffer &&
           g->FramebufferTexture2D && g->CheckFramebufferStatus &&
           g->BlendFuncSeparate && g->BlendEquationSeparate &&
           g->FogCoordPointer && g->ActiveTexture && g->ClientActiveTexture;
}

static bool gl_init(QgpuCore *c)
{
    GlState *g = calloc(1, sizeof(*g));
    if (!g) {
        return false;
    }

#ifdef QGPU_GL_CGL
    {
        CGLPixelFormatObj pix = NULL;
        GLint npix = 0;
        CGLPixelFormatAttribute accel[] = {
            kCGLPFAAccelerated, kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8,
            kCGLPFADepthSize, 16, 0
        };
        CGLPixelFormatAttribute any[] = {
            kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, kCGLPFADepthSize, 16, 0
        };
        if (CGLChoosePixelFormat(accel, &pix, &npix) != kCGLNoError || !pix) {
            if (CGLChoosePixelFormat(any, &pix, &npix) != kCGLNoError || !pix) {
                goto fail;
            }
        }
        if (CGLCreateContext(pix, NULL, &g->ctx) != kCGLNoError || !g->ctx) {
            CGLDestroyPixelFormat(pix);
            goto fail;
        }
        CGLDestroyPixelFormat(pix);
    }
#else
    {
        static const EGLint cfg_attr[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_NONE
        };
        static const EGLint pb_attr[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        EGLConfig cfg;
        EGLint n = 0;

        g->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g->dpy == EGL_NO_DISPLAY || !eglInitialize(g->dpy, NULL, NULL)) {
            goto fail;
        }
        if (!eglBindAPI(EGL_OPENGL_API) ||
            !eglChooseConfig(g->dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
            goto fail;
        }
        g->surf = eglCreatePbufferSurface(g->dpy, cfg, pb_attr);
        g->ctx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, NULL);
        if (g->surf == EGL_NO_SURFACE || g->ctx == EGL_NO_CONTEXT) {
            goto fail;
        }
    }
#endif

    if (!gl_make_current(g) || !gl_resolve(g)) {
        goto fail;
    }
    g->renderer = (const char *)glGetString(GL_RENDERER);
    if (c->trace) {
        fprintf(stderr, "qgpu: backend gl : %s / %s\n",
                g->renderer ? g->renderer : "?",
                (const char *)glGetString(GL_VERSION));
    }
    c->be_priv = g;
    return true;

fail:
#ifdef QGPU_GL_CGL
    if (g->ctx) {
        CGLDestroyContext(g->ctx);
    }
#else
    if (g->dpy != EGL_NO_DISPLAY) {
        eglTerminate(g->dpy);
    }
#endif
    free(g);
    return false;
}

static void gl_fini(QgpuCore *c)
{
    GlState *g = c->be_priv;
    if (!g) {
        return;
    }
#ifdef QGPU_GL_CGL
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(g->ctx);
#else
    eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g->dpy, g->ctx);
    eglDestroySurface(g->dpy, g->surf);
    eglTerminate(g->dpy);
#endif
    free(g);
    c->be_priv = NULL;
}

static bool gl_surf_create(QgpuCore *c, QgpuSurface *s)
{
    GlState *g = c->be_priv;
    GlSurface *gs = calloc(1, sizeof(*gs));
    if (!gs || !gl_make_current(g)) {
        free(gs);
        return false;
    }
    glGenTextures(1, &gs->tex);
    glBindTexture(GL_TEXTURE_2D, gs->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->width, s->height, 0,
                 GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    if (s->has_depth) {
        glGenTextures(1, &gs->depth);
        glBindTexture(GL_TEXTURE_2D, gs->depth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, s->width, s->height, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    }
    g->GenFramebuffers(1, &gs->fbo);
    g->BindFramebuffer(GL_FRAMEBUFFER, gs->fbo);
    g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, gs->tex, 0);
    if (gs->depth) {
        g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_TEXTURE_2D, gs->depth, 0);
    }
    if (g->CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        g->BindFramebuffer(GL_FRAMEBUFFER, 0);
        g->DeleteFramebuffers(1, &gs->fbo);
        glDeleteTextures(1, &gs->tex);
        if (gs->depth) {
            glDeleteTextures(1, &gs->depth);
        }
        free(gs);
        return false;
    }
    /* Contenu initial défini (couleur 0, profondeur 1), comme le backend logiciel. */
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | (gs->depth ? GL_DEPTH_BUFFER_BIT : 0));
    g->BindFramebuffer(GL_FRAMEBUFFER, 0);
    s->priv = gs;
    return true;
}

static void gl_surf_destroy(QgpuCore *c, QgpuSurface *s)
{
    GlState *g = c->be_priv;
    GlSurface *gs = s->priv;
    if (gs && gl_make_current(g)) {
        g->DeleteFramebuffers(1, &gs->fbo);
        glDeleteTextures(1, &gs->tex);
        if (gs->depth) {
            glDeleteTextures(1, &gs->depth);
        }
    }
    free(gs);
    s->priv = NULL;
}

/* Rend la surface courante : FBO lié, viewport et projection posés, et —
 * si st n'est pas NULL — l'état GL du contexte qgpu appliqué. Sans st
 * (transferts), tout test est coupé et tous les masques sont ouverts.
 *
 * Projection : glOrtho(0, w, 0, h, 0, -1) envoie x,y en pixels (ligne 0 de
 * la surface = ligne 0 de la texture = première ligne de glReadPixels) et
 * z ∈ [0,1] sur la profondeur fenêtre z, sans transformation. */
static bool gl_target(QgpuCore *c, QgpuSurface *s, const QgpuState *st)
{
    GlState *g = c->be_priv;
    GlSurface *gs = s->priv;
    if (!gl_make_current(g)) {
        return false;
    }
    g->BindFramebuffer(GL_FRAMEBUFFER, gs->fbo);
    glViewport(0, 0, s->width, s->height);
    glDepthRange(0.0, 1.0);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, s->width, 0.0, s->height, 0.0, -1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    {
        int u;
        for (u = QGPU_MAX_UNITS - 1; u >= 0; u--) {
            g->ActiveTexture(GL_TEXTURE0 + u);
            glDisable(GL_TEXTURE_2D);
        }
    }
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glShadeModel(GL_SMOOTH);
    if (!st) {
        glDisable(GL_FOG);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_ALPHA_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        return true;
    }
    if (st->v[QGPU_SK_DEPTH_TEST] && gs->depth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(st->v[QGPU_SK_DEPTH_FUNC]);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(st->v[QGPU_SK_DEPTH_WRITE] ? GL_TRUE : GL_FALSE);
    glColorMask((st->v[QGPU_SK_COLOR_MASK] & 1) != 0, (st->v[QGPU_SK_COLOR_MASK] & 2) != 0,
                (st->v[QGPU_SK_COLOR_MASK] & 4) != 0, (st->v[QGPU_SK_COLOR_MASK] & 8) != 0);
    if (st->v[QGPU_SK_BLEND]) {
        glEnable(GL_BLEND);
        g->BlendFuncSeparate(st->v[QGPU_SK_BLEND_SRC_RGB], st->v[QGPU_SK_BLEND_DST_RGB],
                             st->v[QGPU_SK_BLEND_SRC_A], st->v[QGPU_SK_BLEND_DST_A]);
        g->BlendEquationSeparate(st->v[QGPU_SK_BLEND_EQ_RGB], st->v[QGPU_SK_BLEND_EQ_A]);
    } else {
        glDisable(GL_BLEND);
    }
    if (st->v[QGPU_SK_ALPHA_TEST]) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(st->v[QGPU_SK_ALPHA_FUNC], qgpu_u2f(st->v[QGPU_SK_ALPHA_REF]));
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    if (st->v[QGPU_SK_FOG]) {
        /* Le facteur f est déjà calculé par l'invité : brouillard linéaire
           de début 1 et de fin 0, coordonnée = f, donne un facteur = f. */
        uint32_t fc = st->v[QGPU_SK_FOG_COLOR];
        GLfloat col[4] = { ((fc >> 16) & 255) / 255.0f, ((fc >> 8) & 255) / 255.0f,
                           (fc & 255) / 255.0f, ((fc >> 24) & 255) / 255.0f };
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, 1.0f);
        glFogf(GL_FOG_END, 0.0f);
        glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
        glFogfv(GL_FOG_COLOR, col);
    } else {
        glDisable(GL_FOG);
    }
    if (st->v[QGPU_SK_POLY_OFFSET]) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(qgpu_u2f(st->v[QGPU_SK_POLY_FACTOR]),
                        qgpu_u2f(st->v[QGPU_SK_POLY_UNITS]));
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    glLineWidth(qgpu_u2f(st->v[QGPU_SK_LINE_WIDTH]));
    glPointSize(qgpu_u2f(st->v[QGPU_SK_POINT_SIZE]));
    if (st->v[QGPU_SK_SCISSOR]) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(st->v[QGPU_SK_SCISSOR_X], st->v[QGPU_SK_SCISSOR_Y],
                  st->v[QGPU_SK_SCISSOR_W], st->v[QGPU_SK_SCISSOR_H]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    return true;
}

static bool gl_clear(QgpuCore *c, QgpuSurface *s, const QgpuState *st,
                     uint32_t mask, uint32_t argb, float depth)
{
    GLbitfield bits = 0;
    GlSurface *gs = s->priv;
    if (!gl_target(c, s, st)) {
        return false;
    }
    if (mask & QGPU_CLEAR_COLOR) {
        glClearColor(((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f,
                     (argb & 255) / 255.0f, ((argb >> 24) & 255) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if ((mask & QGPU_CLEAR_DEPTH) && gs->depth) {
        glClearDepth(depth);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (bits) {
        glClear(bits);
    }
    return glGetError() == GL_NO_ERROR;
}

/* ───────────── textures (v3) : objets GL tenus à jour paresseusement ───────────── */

typedef struct GlTexture {
    GLuint id;
} GlTexture;

static void gl_tex_destroy(QgpuCore *c, QgpuTexture *t)
{
    GlState *g = c->be_priv;
    GlTexture *gt = t->priv;
    if (gt && gl_make_current(g)) {
        glDeleteTextures(1, &gt->id);
    }
    free(gt);
    t->priv = NULL;
}

static bool gl_tex_sync(QgpuCore *c, QgpuTexture *t)
{
    GlTexture *gt = t->priv;
    uint32_t l;
    (void)c;

    if (!gt) {
        gt = calloc(1, sizeof(*gt));
        if (!gt) {
            return false;
        }
        glGenTextures(1, &gt->id);
        t->priv = gt;
        t->params_dirty = true;
        t->dirty = ~0u;
    }
    glBindTexture(GL_TEXTURE_2D, gt->id);
    if (t->params_dirty) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t->min_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->mag_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, t->wrap_s);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t->wrap_t);
        t->params_dirty = false;
    }
    if (t->dirty) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        for (l = 0; l < QGPU_MAX_TEX_LEVELS; l++) {
            const QgpuTexLevel *lv = &t->level[l];
            if (!(t->dirty & (1u << l)) || !lv->px) {
                continue;
            }
            /* internalformat = format de base : L et I viennent du rouge, et
               les fonctions d'environnement suivent la table d'OpenGL. */
            glTexImage2D(GL_TEXTURE_2D, l, t->base_format, lv->w, lv->h, 0,
                         GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, lv->px);
        }
        t->dirty = 0;
    }
    return glGetError() == GL_NO_ERROR;
}

/* GL_COMBINE (v5) : état empaqueté → paramètres d'environnement natifs. */
static void gl_combine(const QgpuState *st, int u)
{
    static const GLenum fn[8] = {
        GL_REPLACE, GL_MODULATE, GL_ADD, GL_ADD_SIGNED, GL_INTERPOLATE,
        GL_SUBTRACT, GL_DOT3_RGB, GL_DOT3_RGBA,
    };
    static const GLenum srcs[4] = { GL_TEXTURE, GL_CONSTANT, GL_PRIMARY_COLOR, GL_PREVIOUS };
    static const GLenum ops_rgb[4] = {
        GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
    };
    static const GLenum ops_a[2] = { GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };
    uint32_t cb = st->v[QGPU_SK_COMBINE0 + u];
    uint32_t src = st->v[QGPU_SK_COMBINE_SRC0 + u];
    int i;

    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, fn[cb & 7]);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, fn[(cb >> 4) & 7]);
    glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, (GLfloat)(1u << ((cb >> 8) & 3)));
    glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, (GLfloat)(1u << ((cb >> 10) & 3)));
    for (i = 0; i < 3; i++) {
        uint32_t f = (src >> (5 * i)) & 31, g = (src >> (15 + 4 * i)) & 15;
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB + i, srcs[f & 3]);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB + i, ops_rgb[(f >> 3) & 3]);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA + i, srcs[g & 3]);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA + i, ops_a[(g >> 3) & 1]);
    }
}

/* Active la texture de l'unité u avec son environnement. */
static bool gl_bind_unit(QgpuCore *c, const QgpuState *st, int u, QgpuTexture *tex,
                         const float *verts, GLsizei stride)
{
    GlState *g = c->be_priv;
    uint32_t ec = st->v[QGPU_SK_UNIT(u) + QGPU_SK_U_ENV_COLOR];
    uint32_t mode = st->v[QGPU_SK_UNIT(u) + QGPU_SK_U_ENV_MODE];
    GLfloat col[4] = { ((ec >> 16) & 255) / 255.0f, ((ec >> 8) & 255) / 255.0f,
                       (ec & 255) / 255.0f, ((ec >> 24) & 255) / 255.0f };
    g->ActiveTexture(GL_TEXTURE0 + u);
    g->ClientActiveTexture(GL_TEXTURE0 + u);
    if (!gl_tex_sync(c, tex)) {
        return false;
    }
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode);
    if (mode == GL_COMBINE) {
        gl_combine(st, u);
    }
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, col);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(4, GL_FLOAT, stride, verts + 8 + 4 * u);
    return true;
}

static void gl_unbind_unit(QgpuCore *c, int u)
{
    GlState *g = c->be_priv;
    g->ActiveTexture(GL_TEXTURE0 + u);
    g->ClientActiveTexture(GL_TEXTURE0 + u);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

static bool gl_draw(QgpuCore *c, QgpuSurface *s, const QgpuState *st, uint32_t prim,
                    QgpuTexture *const *tex,
                    const float *verts, uint32_t nverts, uint32_t words)
{
    static const GLenum mode[3] = { GL_TRIANGLES, GL_LINES, GL_POINTS };
    GlState *g = c->be_priv;
    const GLsizei stride = words * sizeof(float);
    bool ok = true;
    int u;

    if (!gl_target(c, s, st)) {
        return false;
    }
    /* Tableau entrelacé x y f r g b a [s t r q]… : un seul appel de dessin.
       Une unité sans texture reste coupée (gl_target) : elle laisse passer
       la couleur, comme en OpenGL. */
    for (u = 0; u < QGPU_MAX_UNITS && ok; u++) {
        if (tex[u]) {
            ok = gl_bind_unit(c, st, u, tex[u], verts, stride);
        }
    }
    if (ok) {
        g->ClientActiveTexture(GL_TEXTURE0);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, stride, verts);
        glColorPointer(4, GL_FLOAT, stride, verts + 4);
        if (st->v[QGPU_SK_FOG]) {
            /* x y z f : la coordonnée de brouillard est le 4e mot */
            glEnableClientState(GL_FOG_COORDINATE_ARRAY);
            g->FogCoordPointer(GL_FLOAT, stride, verts + 3);
        }
        glDrawArrays(mode[prim], 0, nverts);
        glDisableClientState(GL_FOG_COORDINATE_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
    }
    for (u = QGPU_MAX_UNITS - 1; u >= 0; u--) {
        if (tex[u]) {
            gl_unbind_unit(c, u);
        }
    }
    g->ActiveTexture(GL_TEXTURE0);
    g->ClientActiveTexture(GL_TEXTURE0);
    return ok && glGetError() == GL_NO_ERROR;
}

static bool gl_readback(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, uint32_t *dst)
{
    if (!gl_target(c, s, NULL)) {
        return false;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(x, y, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dst);
    return glGetError() == GL_NO_ERROR;
}

static bool gl_depth_readback(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h, float *dst)
{
    if (!gl_target(c, s, NULL)) {
        return false;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x, y, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, dst);
    return glGetError() == GL_NO_ERROR;
}

static bool gl_depth_upload(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h, const float *src)
{
    GlSurface *gs = s->priv;
    if (!gl_target(c, s, NULL)) {
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, gs->depth);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, src);
    return glGetError() == GL_NO_ERROR;
}

static bool gl_upload(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, const uint32_t *src)
{
    GlSurface *gs = s->priv;
    if (!gl_target(c, s, NULL)) {
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, gs->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h,
                    GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, src);
    return glGetError() == GL_NO_ERROR;
}

const QgpuBackend qgpu_backend_gl = {
    .name           = "gl",
    .cap            = QGPU_CAP_GL,
    .init           = gl_init,
    .fini           = gl_fini,
    .surf_create    = gl_surf_create,
    .surf_destroy   = gl_surf_destroy,
    .clear          = gl_clear,
    .draw           = gl_draw,
    .readback       = gl_readback,
    .upload         = gl_upload,
    .depth_readback = gl_depth_readback,
    .depth_upload   = gl_depth_upload,
    .tex_destroy    = gl_tex_destroy,
};

#else /* ni CGL ni EGL : stub */

static bool gl_stub_init(QgpuCore *c)
{
    (void)c;
    return false;
}

const QgpuBackend qgpu_backend_gl = {
    .name = "gl",
    .cap  = QGPU_CAP_GL,
    .init = gl_stub_init,
};

#endif
