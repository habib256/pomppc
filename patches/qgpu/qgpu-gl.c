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
 * Threads : un contexte GL n'est courant que pour un thread, et EGL interdit
 * de le rendre courant ailleurs tant qu'un thread le tient (EGL_BAD_ACCESS ;
 * CGL est plus laxiste). Deux règles, donc : init() rend le contexte LIBRE en
 * sortant, et chaque opération le rend courant À SON DÉBUT. Depuis la v9, le
 * device appelle tout le reste — exécution, reset, libération — depuis son
 * seul thread de rendu (qgpu-pci.c) ; tests/qgpu_core_test.c fait de même.
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
    /* v7 : couleur secondaire et coordonnées multitexture hors tableau */
    void (*SecondaryColorPointer)(GLint, GLenum, GLsizei, const GLvoid *);
    void (*SecondaryColor3fv)(const GLfloat *);
    void (*MultiTexCoord4fv)(GLenum, const GLfloat *);
    void (*FogCoordf)(GLfloat);
    /* v8 : couleur constante de mélange (GL 1.2 / EXT_blend_color) */
    void (*BlendColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    /* v8 : requêtes d'occlusion (GL 1.5 / ARB_occlusion_query). Facultatives :
       si l'hôte ne les a pas, le backend ne l'annonce pas (QGPU_CAP_OCCLUSION)
       et les opcodes QUERY_* répondent proprement, au lieu de planter. */
    void (*GenQueries)(GLsizei, GLuint *);
    void (*DeleteQueries)(GLsizei, const GLuint *);
    void (*BeginQuery)(GLenum, GLuint);
    void (*EndQuery)(GLenum);
    void (*GetQueryObjectuiv)(GLuint, GLenum, GLuint *);
    bool has_query;
    /* v10 : textures 3D (GL 1.2) et paramètres de point (GL 1.4) ; `has_tex` =
       QGPU_CAP_GL14 annoncé */
    void (*TexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint,
                       GLenum, GLenum, const GLvoid *);
    void (*PointParameterf)(GLenum, GLfloat);
    void (*PointParameterfv)(GLenum, const GLfloat *);
    bool has_tex;
    const char *renderer;
} GlState;

typedef struct GlSurface {
    GLuint fbo, tex, depth;        /* depth : texture DEPTH_COMPONENT, ou 0 */
    bool   packed;                 /* v6 : `depth` est un DEPTH24_STENCIL8 combiné */
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
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT   0x8D20
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL        0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8    0x84FA
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8     0x88F0
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD             0x8006
#endif
/* v10 : cibles et paramètres de texture d'OpenGL 1.2 à 1.4 */
#ifndef GL_TEXTURE_3D
#define GL_TEXTURE_3D           0x806F
#endif
#ifndef GL_TEXTURE_WRAP_R
#define GL_TEXTURE_WRAP_R       0x8072
#endif
#ifndef GL_TEXTURE_CUBE_MAP
#define GL_TEXTURE_CUBE_MAP     0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#endif
#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE    0x84F5
#endif
#ifndef GL_TEXTURE_MIN_LOD
#define GL_TEXTURE_MIN_LOD      0x813A
#define GL_TEXTURE_MAX_LOD      0x813B
#define GL_TEXTURE_BASE_LEVEL   0x813C
#define GL_TEXTURE_MAX_LEVEL    0x813D
#endif
#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_FILTER_CONTROL 0x8500
#define GL_TEXTURE_LOD_BIAS     0x8501
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_DEPTH_TEXTURE_MODE   0x884B
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif
#ifndef GL_POINT_SIZE_MIN
#define GL_POINT_SIZE_MIN       0x8126
#define GL_POINT_SIZE_MAX       0x8127
#define GL_POINT_FADE_THRESHOLD_SIZE 0x8128
#define GL_POINT_DISTANCE_ATTENUATION 0x8129
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
/* v7 : constantes de l'étage géométrique, définies ici pour ne dépendre
   d'aucune version d'en-tête (le profil hérité d'Apple les a toutes). */
#ifndef GL_COLOR_SUM
#define GL_COLOR_SUM            0x8458
#endif
#ifndef GL_SECONDARY_COLOR_ARRAY
#define GL_SECONDARY_COLOR_ARRAY 0x845E
#endif
#ifndef GL_RESCALE_NORMAL
#define GL_RESCALE_NORMAL       0x803A
#endif
#ifndef GL_LIGHT_MODEL_COLOR_CONTROL
#define GL_LIGHT_MODEL_COLOR_CONTROL 0x81F8
#endif
#ifndef GL_FRAGMENT_DEPTH
#define GL_FRAGMENT_DEPTH       0x8452
#endif
/* v8 : constantes du reste du pipeline fixe, définies ici pour ne dépendre
   d'aucune version d'en-tête. */
#ifndef GL_COLOR_LOGIC_OP
#define GL_COLOR_LOGIC_OP       0x0BF2
#endif
#ifndef GL_POLYGON_OFFSET_POINT
#define GL_POLYGON_OFFSET_POINT 0x2A01
#endif
#ifndef GL_POLYGON_OFFSET_LINE
#define GL_POLYGON_OFFSET_LINE  0x2A02
#endif
#ifndef GL_LINE_STIPPLE
#define GL_LINE_STIPPLE         0x0B24
#endif
#ifndef GL_POLYGON_STIPPLE
#define GL_POLYGON_STIPPLE      0x0B42
#endif
#ifndef GL_UNPACK_LSB_FIRST
#define GL_UNPACK_LSB_FIRST     0x0CF1
#endif
#ifndef GL_SAMPLES_PASSED
#define GL_SAMPLES_PASSED       0x8914
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT         0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
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
    g->SecondaryColorPointer = gl_proc("glSecondaryColorPointer");
    g->SecondaryColor3fv = gl_proc("glSecondaryColor3fv");
    g->MultiTexCoord4fv = gl_proc("glMultiTexCoord4fv");
    g->FogCoordf = gl_proc("glFogCoordf");
    /* v8 : couleur constante de mélange. Elle est exigée comme les autres
       entrées du mélange : sans elle, les quatre facteurs constants seraient
       silencieusement faux, ce qui est pire qu'un backend indisponible. */
    g->BlendColor = gl_proc("glBlendColor");
    if (!g->BlendColor) {
        g->BlendColor = gl_proc("glBlendColorEXT");
    }
    /* v8 : requêtes d'occlusion — FACULTATIVES, cf. GlState. */
    g->GenQueries = gl_proc("glGenQueries");
    g->DeleteQueries = gl_proc("glDeleteQueries");
    g->BeginQuery = gl_proc("glBeginQuery");
    g->EndQuery = gl_proc("glEndQuery");
    g->GetQueryObjectuiv = gl_proc("glGetQueryObjectuiv");
    if (!g->GenQueries || !g->BeginQuery) {
        g->GenQueries = gl_proc("glGenQueriesARB");
        g->DeleteQueries = gl_proc("glDeleteQueriesARB");
        g->BeginQuery = gl_proc("glBeginQueryARB");
        g->EndQuery = gl_proc("glEndQueryARB");
        g->GetQueryObjectuiv = gl_proc("glGetQueryObjectuivARB");
    }
    g->has_query = g->GenQueries && g->DeleteQueries && g->BeginQuery &&
                   g->EndQuery && g->GetQueryObjectuiv;
    /* v10 : FACULTATIF, comme les requêtes (cf. QGPU_CAP_GL14). */
    g->TexImage3D = gl_proc("glTexImage3D");
    if (!g->TexImage3D) {
        g->TexImage3D = gl_proc("glTexImage3DEXT");
    }
    g->PointParameterf = gl_proc("glPointParameterf");
    g->PointParameterfv = gl_proc("glPointParameterfv");
    if (!g->PointParameterf || !g->PointParameterfv) {
        g->PointParameterf = gl_proc("glPointParameterfARB");
        g->PointParameterfv = gl_proc("glPointParameterfvARB");
    }
    return g->GenFramebuffers && g->DeleteFramebuffers && g->BindFramebuffer &&
           g->FramebufferTexture2D && g->CheckFramebufferStatus &&
           g->BlendFuncSeparate && g->BlendEquationSeparate &&
           g->FogCoordPointer && g->ActiveTexture && g->ClientActiveTexture &&
           g->SecondaryColorPointer && g->SecondaryColor3fv &&
           g->MultiTexCoord4fv && g->FogCoordf && g->BlendColor;
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
    if (g->has_query) {
        c->caps |= QGPU_CAP_OCCLUSION;         /* v8 : annoncé seulement si tenu */
    }
    {
        /* v10 : cibles, profondeur, comparaison, LOD — tout OpenGL 1.4. */
        const char *ver = (const char *)glGetString(GL_VERSION);
        int maj = 0, min = 0;
        if (ver && sscanf(ver, "%d.%d", &maj, &min) == 2 &&
            (maj > 1 || (maj == 1 && min >= 4)) && g->TexImage3D &&
            g->PointParameterf && g->PointParameterfv) {
            g->has_tex = true;
            c->caps |= QGPU_CAP_GL14;
        }
    }
    if (c->trace) {
        fprintf(stderr, "qgpu: backend gl : %s / %s\n",
                g->renderer ? g->renderer : "?",
                (const char *)glGetString(GL_VERSION));
    }
    /* Le contexte NAÎT LIBRE. L'initialisation se fait sur le thread qui
       réalise le device, l'exécution sur le thread de rendu (v9) : un contexte
       EGL encore courant ici ne peut pas y être rendu courant — eglMakeCurrent
       rend EGL_BAD_ACCESS sur le pilote NVIDIA, et toute soumission répondait
       QGPU_ST_BACKEND. CGL ne l'interdit pas, d'où un bogue resté invisible
       sur macOS. */
#ifdef QGPU_GL_CGL
    CGLSetCurrentContext(NULL);
#else
    eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
#endif
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

/* v10 : coupe TOUTES les cibles de l'unité active. Une seule allumée à la
   fois ensuite, celle de la texture liée : la priorité d'OpenGL entre cibles
   (cube > 3D > rectangle > 2D > 1D) ne joue donc jamais. */
static void gl_disable_targets(const GlState *g)
{
    glDisable(GL_TEXTURE_1D);
    glDisable(GL_TEXTURE_2D);
    if (g->has_tex) {
        glDisable(GL_TEXTURE_3D);
        glDisable(GL_TEXTURE_CUBE_MAP);
        glDisable(GL_TEXTURE_RECTANGLE);
    }
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
        /* v6 : avec stencil, UN SEUL tampon combiné DEPTH24_STENCIL8 — c'est le
           format universellement disponible, et le seul qui donne un FBO
           complet partout. Sans stencil, on garde le DEPTH_COMPONENT24 de v2. */
        gs->packed = s->has_stencil;
        glGenTextures(1, &gs->depth);
        glBindTexture(GL_TEXTURE_2D, gs->depth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (gs->packed) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, s->width, s->height, 0,
                         GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, s->width, s->height, 0,
                         GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        }
    }
    g->GenFramebuffers(1, &gs->fbo);
    g->BindFramebuffer(GL_FRAMEBUFFER, gs->fbo);
    g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, gs->tex, 0);
    if (gs->depth) {
        g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_TEXTURE_2D, gs->depth, 0);
        if (gs->packed) {
            /* Les deux points d'attache plutôt que DEPTH_STENCIL_ATTACHMENT :
               c'est la forme qu'accepte AUSSI EXT_framebuffer_object, qui ne
               connaît pas ce point d'attache unique. */
            g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                    GL_TEXTURE_2D, gs->depth, 0);
        }
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
    /* Contenu initial défini (couleur 0, profondeur 1, stencil 0), comme le
       backend logiciel. */
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | (gs->depth ? GL_DEPTH_BUFFER_BIT : 0) |
            (gs->packed ? GL_STENCIL_BUFFER_BIT : 0));
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
            gl_disable_targets(g);
        }
    }
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glShadeModel(GL_SMOOTH);
    /* v8 : ce que le chemin brut comme l'ancien peuvent avoir allumé. Remis à
       plat ici, l'unique porte d'entrée de toute opération. */
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_LINE_STIPPLE);
    glDisable(GL_POLYGON_STIPPLE);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDisable(GL_POLYGON_OFFSET_POINT);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    if (!st) {
        glDisable(GL_FOG);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        return true;
    }
    if (st->v[QGPU_SK_DEPTH_TEST] && gs->depth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(st->v[QGPU_SK_DEPTH_FUNC]);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(st->v[QGPU_SK_DEPTH_WRITE] ? GL_TRUE : GL_FALSE);
    /* v6 : le masque d'écriture vaut aussi pour glClear, donc il est posé même
       quand le test est coupé. */
    if (st->v[QGPU_SK_STENCIL_TEST] && gs->packed) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(st->v[QGPU_SK_STENCIL_FUNC], (GLint)st->v[QGPU_SK_STENCIL_REF],
                      st->v[QGPU_SK_STENCIL_VALUE_MASK]);
        glStencilOp(st->v[QGPU_SK_STENCIL_OP_FAIL], st->v[QGPU_SK_STENCIL_OP_ZFAIL],
                    st->v[QGPU_SK_STENCIL_OP_ZPASS]);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    glStencilMask(st->v[QGPU_SK_STENCIL_WRITE_MASK]);
    glColorMask((st->v[QGPU_SK_COLOR_MASK] & 1) != 0, (st->v[QGPU_SK_COLOR_MASK] & 2) != 0,
                (st->v[QGPU_SK_COLOR_MASK] & 4) != 0, (st->v[QGPU_SK_COLOR_MASK] & 8) != 0);
    if (st->v[QGPU_SK_BLEND]) {
        uint32_t bc = st->v[QGPU_SK_BLEND_COLOR];       /* v8 */
        glEnable(GL_BLEND);
        g->BlendFuncSeparate(st->v[QGPU_SK_BLEND_SRC_RGB], st->v[QGPU_SK_BLEND_DST_RGB],
                             st->v[QGPU_SK_BLEND_SRC_A], st->v[QGPU_SK_BLEND_DST_A]);
        g->BlendEquationSeparate(st->v[QGPU_SK_BLEND_EQ_RGB], st->v[QGPU_SK_BLEND_EQ_A]);
        g->BlendColor(((bc >> 16) & 255) / 255.0f, ((bc >> 8) & 255) / 255.0f,
                      (bc & 255) / 255.0f, ((bc >> 24) & 255) / 255.0f);
    } else {
        glDisable(GL_BLEND);
    }
    /* v8 : l'opération logique REMPLACE le mélange. La spécification le dit,
       mais on coupe GL_BLEND explicitement plutôt que de s'en remettre au
       pilote : le backend de référence fait exactement cela. */
    if (st->v[QGPU_SK_LOGIC_OP]) {
        glDisable(GL_BLEND);
        glLogicOp((GLenum)st->v[QGPU_SK_LOGIC_OP_MODE]);
        glEnable(GL_COLOR_LOGIC_OP);
    }
    /* v8 : modes de polygone. Le sens des faces est posé par l'appelant
       (gl_draw / gl_draw_raw), qui seul sait quel chemin il sert. */
    glPolygonMode(GL_FRONT, (GLenum)st->v[QGPU_SK_POLYGON_MODE_FRONT]);
    glPolygonMode(GL_BACK, (GLenum)st->v[QGPU_SK_POLYGON_MODE_BACK]);
    if (st->v[QGPU_SK_LINE_STIPPLE]) {
        glLineStipple((GLint)st->v[QGPU_SK_LINE_STIPPLE_FACTOR],
                      (GLushort)st->v[QGPU_SK_LINE_STIPPLE_PATTERN]);
        glEnable(GL_LINE_STIPPLE);
    }
    if (st->v[QGPU_SK_POLYGON_STIPPLE] && c->cur_stip) {
        /* LE SENS DE L'IMAGE. glPolygonStipple indexe le motif par la ligne de
           fenêtre d'OpenGL, qui vaut ICI la ligne de SURFACE (le FBO a sa ligne
           0 en haut, et le chemin brut a déjà retourné sa géométrie). Le
           protocole, lui, indexe par yw = hauteur − ys. On permute donc les 32
           lignes une fois pour toutes, plutôt que de tordre les deux chemins. */
        GLubyte mask[128];
        int k, b;
        for (k = 0; k < 32; k++) {
            uint32_t row = qgpu_stipple_row(c->cur_stip, s->height, k);
            for (b = 0; b < 4; b++) {
                /* MSB d'abord : le bit 31 du mot est la colonne x = 0. */
                mask[k * 4 + b] = (GLubyte)((row >> (24 - 8 * b)) & 0xFF);
            }
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
        glPolygonStipple(mask);
        glEnable(GL_POLYGON_STIPPLE);
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
    /* Décalage de polygone : les trois interrupteurs d'OpenGL partagent le même
       facteur et les mêmes unités (v4 pour le rempli, v8 pour ligne et point). */
    glPolygonOffset(qgpu_u2f(st->v[QGPU_SK_POLY_FACTOR]),
                    qgpu_u2f(st->v[QGPU_SK_POLY_UNITS]));
    if (st->v[QGPU_SK_POLY_OFFSET]) {
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    if (st->v[QGPU_SK_POLY_OFFSET_LINE]) {
        glEnable(GL_POLYGON_OFFSET_LINE);
    }
    if (st->v[QGPU_SK_POLY_OFFSET_POINT]) {
        glEnable(GL_POLYGON_OFFSET_POINT);
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
    /* v8 : glClear ne passe ni par le mélange ni par l'opération logique (il ne
       voit que les ciseaux et les masques). On le rend explicite plutôt que de
       s'en remettre au pilote — le backend de référence fait pareil. */
    glDisable(GL_COLOR_LOGIC_OP);
    if (mask & QGPU_CLEAR_COLOR) {
        glClearColor(((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f,
                     (argb & 255) / 255.0f, ((argb >> 24) & 255) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if ((mask & QGPU_CLEAR_DEPTH) && gs->depth) {
        glClearDepth(depth);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if ((mask & QGPU_CLEAR_STENCIL) && gs->packed) {
        glClearStencil((GLint)st->v[QGPU_SK_STENCIL_CLEAR]);
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    if (bits) {
        glClear(bits);
    }
    return glGetError() == GL_NO_ERROR;
}

/* ───────────── textures (v3) : objets GL tenus à jour paresseusement ───────────── */

typedef struct GlTexture {
    GLuint id;
    GLenum target;                 /* v10 : cible GL de l'objet */
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

/* Envoie un niveau d'une face. Les texels sont déjà des mots ARGB hôte-natifs
   (ou des flottants de profondeur) : le cœur a fait conversions et
   décompression. internalformat = format de base : L et I viennent du rouge,
   et les fonctions d'environnement suivent la table d'OpenGL. */
static void gl_tex_level(const GlState *g, const QgpuTexture *t, GLenum target,
                         uint32_t face, uint32_t l)
{
    const QgpuTexLevel *lv = &t->level[face][l];
    bool depth = lv->fmt == 0x1902;
    GLint ifmt = depth ? GL_DEPTH_COMPONENT24 : (GLint)lv->fmt;
    GLenum fmt = depth ? GL_DEPTH_COMPONENT : GL_BGRA;
    GLenum type = depth ? GL_FLOAT : GL_UNSIGNED_INT_8_8_8_8_REV;

    switch (target) {
    case GL_TEXTURE_1D:
        glTexImage1D(target, l, ifmt, lv->w, 0, fmt, type, lv->px);
        break;
    case GL_TEXTURE_3D:
        g->TexImage3D(target, l, ifmt, lv->w, lv->h, lv->d, 0, fmt, type, lv->px);
        break;
    case GL_TEXTURE_CUBE_MAP:
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, l, ifmt, lv->w, lv->h, 0,
                     fmt, type, lv->px);
        break;
    default:                                              /* 2D, RECTANGLE */
        glTexImage2D(target, l, ifmt, lv->w, lv->h, 0, fmt, type, lv->px);
    }
}

static bool gl_tex_sync(QgpuCore *c, QgpuTexture *t)
{
    GlState *g = c->be_priv;
    GlTexture *gt = t->priv;
    uint32_t f, l;

    if (!gt) {
        gt = calloc(1, sizeof(*gt));
        if (!gt) {
            return false;
        }
        glGenTextures(1, &gt->id);
        gt->target = t->target;     /* QGPU_TT_* = valeurs d'OpenGL */
        t->priv = gt;
        t->params_dirty = true;
        for (f = 0; f < QGPU_TEX_FACES; f++) {
            t->dirty[f] = ~0u;
        }
    }
    glBindTexture(gt->target, gt->id);
    if (t->params_dirty) {
        GLenum tg = gt->target;
        glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, t->min_filter);
        glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, t->mag_filter);
        glTexParameteri(tg, GL_TEXTURE_WRAP_S, t->wrap_s);
        glTexParameteri(tg, GL_TEXTURE_WRAP_T, t->wrap_t);
        if (g->has_tex) {
            uint32_t bc = t->border;
            GLfloat border[4] = { ((bc >> 16) & 255) / 255.0f, ((bc >> 8) & 255) / 255.0f,
                                  (bc & 255) / 255.0f, ((bc >> 24) & 255) / 255.0f };
            glTexParameterfv(tg, GL_TEXTURE_BORDER_COLOR, border);
            glTexParameteri(tg, GL_TEXTURE_WRAP_R, t->wrap_r);
            glTexParameterf(tg, GL_TEXTURE_LOD_BIAS, t->lod_bias);
            if (tg != GL_TEXTURE_RECTANGLE) {
                /* refusés sur un rectangle (GL_INVALID_OPERATION chez NVIDIA),
                   et le cœur ne les accepte pas pour cette cible */
                glTexParameterf(tg, GL_TEXTURE_MIN_LOD, t->min_lod);
                glTexParameterf(tg, GL_TEXTURE_MAX_LOD, t->max_lod);
                glTexParameteri(tg, GL_TEXTURE_BASE_LEVEL, t->base_level);
                glTexParameteri(tg, GL_TEXTURE_MAX_LEVEL, t->max_level);
            }
            glTexParameteri(tg, GL_TEXTURE_COMPARE_MODE, t->compare_mode);
            glTexParameteri(tg, GL_TEXTURE_COMPARE_FUNC, t->compare_func);
            glTexParameteri(tg, GL_DEPTH_TEXTURE_MODE, t->depth_mode);
        } else {
            /* hôte < 1.4 : la bordure seule (GL 1.0), pour GL_CLAMP */
            uint32_t bc = t->border;
            GLfloat border[4] = { ((bc >> 16) & 255) / 255.0f, ((bc >> 8) & 255) / 255.0f,
                                  (bc & 255) / 255.0f, ((bc >> 24) & 255) / 255.0f };
            glTexParameterfv(tg, GL_TEXTURE_BORDER_COLOR, border);
        }
        t->params_dirty = false;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (f = 0; f < t->nfaces; f++) {
        if (!t->dirty[f]) {
            continue;
        }
        for (l = 0; l < QGPU_MAX_TEX_LEVELS; l++) {
            if ((t->dirty[f] & (1u << l)) && t->level[f][l].px) {
                gl_tex_level(g, t, gt->target, f, l);
            }
        }
        t->dirty[f] = 0;
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

/* Rend l'unité u courante, y lie sa texture et pose son environnement.
   Séparé de gl_bind_unit parce que le chemin brut (v7) partage tout cela mais
   pose lui-même la matrice de texture et le pointeur de coordonnées. */
static bool gl_unit_env(QgpuCore *c, const QgpuState *st, int u, QgpuTexture *tex)
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
    gl_disable_targets(g);
    glEnable(((GlTexture *)tex->priv)->target);
    if (g->has_tex) {
        /* v10 : biais de LOD de l'unité (OpenGL 1.4) */
        glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS,
                  qgpu_u2f(st->v[QGPU_SK_TEX_LOD_BIAS0 + u]));
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode);
    if (mode == GL_COMBINE) {
        gl_combine(st, u);
    }
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, col);
    return true;
}

/* Active la texture de l'unité u avec son environnement. */
static bool gl_bind_unit(QgpuCore *c, const QgpuState *st, int u, QgpuTexture *tex,
                         const float *verts, GLsizei stride)
{
    if (!gl_unit_env(c, st, u, tex)) {
        return false;
    }
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
    gl_disable_targets(g);
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
    /* v8 : SENS DES FACES DU CHEMIN HÉRITÉ. Ces sommets sont en pixels de
       surface (y vers le BAS) et gl_target leur donne glOrtho(0, w, 0, h) : ce
       qu'OpenGL voit comme trigonométrique est donc HORAIRE à l'écran. On
       inverse, exactement comme le fait le chemin brut après son retournement,
       pour que « face avant » veuille dire la même chose partout : le sens
       trigonométrique tel qu'on le VOIT. */
    glFrontFace(st->v[QGPU_SK_FRONT_FACE] == 0x0901 ? GL_CW : GL_CCW);
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

/* ═══════════════ v7 : la géométrie sur le GPU hôte ═════════════════════════
 *
 * Ici, rien n'est calculé : on REPOSE l'état d'OpenGL que l'invité nous a
 * transmis (matrices, lumières, matériaux, texgen, plans de découpe, brouillard,
 * élimination des faces) et on laisse le pipeline fixe du GPU faire le travail
 * que GLEngine faisait sur le PowerPC émulé.
 *
 * LE RETOURNEMENT. Le chemin existant écrit en pixels de surface (origine en
 * haut, y vers le bas) et gl_target lui donne glOrtho(0, w, 0, h) : la ligne 0
 * de la surface est la ligne 0 de la texture du FBO, donc y de surface = y de
 * fenêtre GL. Le chemin brut, lui, reçoit un viewport en repère OpenGL (origine
 * EN BAS). Pour que la ligne de surface vaille « hauteur − yw » — donc pour que
 * les deux chemins produisent une image dans le même sens — on fait deux choses,
 * et deux seulement :
 *   1. projection = diag(1, −1, 1, 1) × P, et viewport posé à
 *      (vx, hauteur − vy − vh) : la composition des deux redonne exactement
 *      yw_surface = hauteur − yw_GL ;
 *   2. le sens des faces est INVERSÉ (GL_CCW ↔ GL_CW), puisque le retournement
 *      change l'orientation de tous les triangles. Le cœur, lui, garde la
 *      convention de l'invité.
 */

/* Coupe tout ce que le chemin brut a pu allumer : les opcodes v1–v6 qui
   suivront ne doivent rien voir de l'étage géométrique. */
static void gl_reset_raw(QgpuCore *c)
{
    GlState *g = c->be_priv;
    int i;

    if (g->has_tex) {
        /* v10 : points sans atténuation pour les anciens opcodes */
        static const GLfloat att[3] = { 1.0f, 0.0f, 0.0f };
        g->PointParameterf(GL_POINT_SIZE_MIN, 0.0f);
        g->PointParameterf(GL_POINT_SIZE_MAX, 64.0f);
        g->PointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, 1.0f);
        g->PointParameterfv(GL_POINT_DISTANCE_ATTENUATION, att);
    }
    glDisable(GL_LIGHTING);
    glDisable(GL_NORMALIZE);
    glDisable(GL_RESCALE_NORMAL);
    glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_COLOR_SUM);
    glDisable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    for (i = 0; i < QGPU_MAX_LIGHTS; i++) {
        glDisable(GL_LIGHT0 + i);
    }
    for (i = 0; i < QGPU_MAX_CLIP_PLANES; i++) {
        glDisable(GL_CLIP_PLANE0 + i);
    }
    for (i = QGPU_MAX_UNITS - 1; i >= 0; i--) {
        g->ActiveTexture(GL_TEXTURE0 + i);
        g->ClientActiveTexture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_GEN_S);
        glDisable(GL_TEXTURE_GEN_T);
        glDisable(GL_TEXTURE_GEN_R);
        glDisable(GL_TEXTURE_GEN_Q);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        gl_disable_targets(g);
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
    }
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_SECONDARY_COLOR_ARRAY);
    glDisableClientState(GL_FOG_COORDINATE_ARRAY);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

/* Lumières, matériaux, plans de découpe et plans œil du texgen sont posés
   AVEC UNE MODÈLE-VUE IDENTITÉ : OpenGL les transforme au moment de l'appel,
   et l'invité nous les a déjà donnés en coordonnées œil. */
static void gl_set_lights(const QgpuGeom *gm)
{
    int i;

    for (i = 0; i < QGPU_MAX_LIGHTS; i++) {
        const QgpuLight *l = &gm->light[i];
        GLenum id = GL_LIGHT0 + i;
        if (!l->enabled) {
            glDisable(id);
            continue;
        }
        glLightfv(id, GL_AMBIENT, l->ambient);
        glLightfv(id, GL_DIFFUSE, l->diffuse);
        glLightfv(id, GL_SPECULAR, l->specular);
        glLightfv(id, GL_POSITION, l->position);
        glLightfv(id, GL_SPOT_DIRECTION, l->spot_dir);
        glLightf(id, GL_SPOT_EXPONENT, l->spot_exp);
        glLightf(id, GL_SPOT_CUTOFF, l->spot_cutoff);
        glLightf(id, GL_CONSTANT_ATTENUATION, l->att[0]);
        glLightf(id, GL_LINEAR_ATTENUATION, l->att[1]);
        glLightf(id, GL_QUADRATIC_ATTENUATION, l->att[2]);
        glEnable(id);
    }
}

static void gl_set_material(const QgpuGeom *gm, GLenum face, int idx)
{
    const QgpuMaterial *m = &gm->mat[idx];
    glMaterialfv(face, GL_AMBIENT, m->ambient);
    glMaterialfv(face, GL_DIFFUSE, m->diffuse);
    glMaterialfv(face, GL_SPECULAR, m->specular);
    glMaterialfv(face, GL_EMISSION, m->emission);
    glMaterialf(face, GL_SHININESS, m->shininess);
}

static void gl_set_texgen(QgpuCore *c, const QgpuGeom *gm)
{
    GlState *g = c->be_priv;
    static const GLenum coord[4] = {
        GL_S, GL_T, GL_R, GL_Q
    };
    static const GLenum enab[4] = {
        GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q
    };
    int u, k;

    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        g->ActiveTexture(GL_TEXTURE0 + u);
        for (k = 0; k < 4; k++) {
            const QgpuTexgen *tg = &gm->texgen[u][k];
            if (!tg->enabled) {
                glDisable(enab[k]);
                continue;
            }
            glTexGeni(coord[k], GL_TEXTURE_GEN_MODE, (GLint)tg->mode);
            glTexGenfv(coord[k], GL_OBJECT_PLANE, tg->obj_plane);
            glTexGenfv(coord[k], GL_EYE_PLANE, tg->eye_plane);
            glEnable(enab[k]);
        }
    }
    g->ActiveTexture(GL_TEXTURE0);
}

static void gl_set_clip(const QgpuGeom *gm)
{
    int i, k;

    for (i = 0; i < QGPU_MAX_CLIP_PLANES; i++) {
        if (!gm->clip[i].enabled) {
            glDisable(GL_CLIP_PLANE0 + i);
            continue;
        }
        {
            GLdouble eq[4];
            for (k = 0; k < 4; k++) {
                eq[k] = gm->clip[i].eq[k];
            }
            glClipPlane(GL_CLIP_PLANE0 + i, eq);
            glEnable(GL_CLIP_PLANE0 + i);
        }
    }
}

static bool gl_draw_raw(QgpuCore *c, QgpuSurface *s, const QgpuState *st,
                        const QgpuGeom *gm, QgpuTexture *const *tex,
                        uint32_t mode, uint32_t fmt, const float *verts,
                        uint32_t nverts, uint32_t words,
                        const uint32_t *idx, uint32_t count, uint32_t first)
{
    GlState *g = c->be_priv;
    const GLsizei stride = (GLsizei)(words * sizeof(float));
    int off_n = qgpu_vf_offset(fmt, QGPU_VF_NORMAL);
    int off_c = qgpu_vf_offset(fmt, QGPU_VF_COLOR);
    int off_sc = qgpu_vf_offset(fmt, QGPU_VF_SEC_COLOR);
    int off_f = qgpu_vf_offset(fmt, QGPU_VF_FOG);
    int vx, vy, vw, vh;
    bool ok = true;
    int u;

    (void)nverts;
    if (!gl_target(c, s, st)) {
        return false;
    }
    if (gm->vp_set) {
        vx = gm->vp[0]; vy = gm->vp[1]; vw = gm->vp[2]; vh = gm->vp[3];
    } else {
        vx = 0; vy = 0; vw = (int)s->width; vh = (int)s->height;
    }
    glViewport(vx, (int)s->height - vy - vh, vw, vh);
    glDepthRange(gm->depth_near, gm->depth_far);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glScalef(1.0f, -1.0f, 1.0f);                  /* cf. LE RETOURNEMENT */
    glMultMatrixf(gm->mtx[QGPU_MTX_PROJECTION]);

    /* modèle-vue identité pendant qu'on pose ce qu'OpenGL transformerait */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gl_set_lights(gm);
    gl_set_clip(gm);
    gl_set_texgen(c, gm);
    glLoadMatrixf(gm->mtx[QGPU_MTX_MODELVIEW]);

    gl_set_material(gm, GL_FRONT, 0);
    gl_set_material(gm, GL_BACK, 1);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, gm->lm_ambient);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, st->v[QGPU_SK_LOCAL_VIEWER] ? 1 : 0);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, st->v[QGPU_SK_TWO_SIDE] ? 1 : 0);
    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, (GLint)st->v[QGPU_SK_COLOR_CONTROL]);
    if (st->v[QGPU_SK_COLOR_MATERIAL]) {
        glColorMaterial(st->v[QGPU_SK_COLOR_MAT_FACE], st->v[QGPU_SK_COLOR_MAT_MODE]);
        glEnable(GL_COLOR_MATERIAL);
    } else {
        glDisable(GL_COLOR_MATERIAL);
    }
    if (st->v[QGPU_SK_LIGHTING]) {
        glEnable(GL_LIGHTING);
    } else {
        glDisable(GL_LIGHTING);
    }
    if (st->v[QGPU_SK_NORMALIZE]) {
        glEnable(GL_NORMALIZE);
        glDisable(GL_RESCALE_NORMAL);
    } else {
        glDisable(GL_NORMALIZE);
        if (st->v[QGPU_SK_RESCALE_NORMAL]) {
            glEnable(GL_RESCALE_NORMAL);
        } else {
            glDisable(GL_RESCALE_NORMAL);
        }
    }
    glShadeModel(st->v[QGPU_SK_SHADE_MODEL]);
    if (st->v[QGPU_SK_CULL_FACE]) {
        glEnable(GL_CULL_FACE);
        glCullFace(st->v[QGPU_SK_CULL_MODE]);
    } else {
        glDisable(GL_CULL_FACE);
    }
    /* sens inversé : le retournement en y a changé l'orientation des triangles */
    glFrontFace(st->v[QGPU_SK_FRONT_FACE] == 0x0901 ? GL_CW : GL_CCW);
    if (st->v[QGPU_SK_FOG] && st->v[QGPU_SK_FOG_MODE] != QGPU_FOG_VERTEX) {
        /* Brouillard calculé par l'hôte : les valeurs d'énumération du mode
           sont celles d'OpenGL, on les repasse telles quelles. */
        glFogi(GL_FOG_MODE, (GLint)st->v[QGPU_SK_FOG_MODE]);
        glFogf(GL_FOG_DENSITY, qgpu_u2f(st->v[QGPU_SK_FOG_DENSITY]));
        glFogf(GL_FOG_START, qgpu_u2f(st->v[QGPU_SK_FOG_START]));
        glFogf(GL_FOG_END, qgpu_u2f(st->v[QGPU_SK_FOG_END]));
        glFogi(GL_FOG_COORDINATE_SOURCE,
               off_f >= 0 ? GL_FOG_COORDINATE : GL_FRAGMENT_DEPTH);
    }

    /* Tableaux de sommets : un attribut absent devient une valeur courante. */
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(QGPU_VF_POS_COUNT(fmt), GL_FLOAT, stride, verts);
    if (off_n >= 0) {
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, stride, verts + off_n);
    } else {
        glDisableClientState(GL_NORMAL_ARRAY);
        glNormal3fv(gm->cur_normal);
    }
    if (off_c >= 0) {
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_FLOAT, stride, verts + off_c);
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4fv(gm->cur_color);
    }
    if (off_sc >= 0) {
        glEnableClientState(GL_SECONDARY_COLOR_ARRAY);
        g->SecondaryColorPointer(3, GL_FLOAT, stride, verts + off_sc);
    } else {
        glDisableClientState(GL_SECONDARY_COLOR_ARRAY);
        g->SecondaryColor3fv(gm->cur_sec);
    }
    /* v10 : GL_COLOR_SUM selon la clé ; QGPU_CSUM_FORMAT garde la règle v7–v9.
       Éclairage allumé, OpenGL ajoute de lui-même la couleur secondaire qu'il a
       calculée : rien à faire de plus, comme le backend de référence. */
    if (st->v[QGPU_SK_COLOR_SUM] == QGPU_CSUM_ON ||
        (st->v[QGPU_SK_COLOR_SUM] == QGPU_CSUM_FORMAT && off_sc >= 0)) {
        glEnable(GL_COLOR_SUM);
    } else {
        glDisable(GL_COLOR_SUM);
    }
    /* v10 : paramètres de point (le cœur a refusé tout autre réglage que
       l'initial si l'hôte ne les a pas) */
    if (g->has_tex) {
        GLfloat att[3] = { qgpu_u2f(st->v[QGPU_SK_POINT_ATT_CONST]),
                           qgpu_u2f(st->v[QGPU_SK_POINT_ATT_LINEAR]),
                           qgpu_u2f(st->v[QGPU_SK_POINT_ATT_QUAD]) };
        g->PointParameterf(GL_POINT_SIZE_MIN, qgpu_u2f(st->v[QGPU_SK_POINT_SIZE_MIN]));
        g->PointParameterf(GL_POINT_SIZE_MAX, qgpu_u2f(st->v[QGPU_SK_POINT_SIZE_MAX]));
        g->PointParameterf(GL_POINT_FADE_THRESHOLD_SIZE,
                           qgpu_u2f(st->v[QGPU_SK_POINT_FADE]));
        g->PointParameterfv(GL_POINT_DISTANCE_ATTENUATION, att);
    }
    if (off_f >= 0) {
        glEnableClientState(GL_FOG_COORDINATE_ARRAY);
        g->FogCoordPointer(GL_FLOAT, stride, verts + off_f);
    } else {
        glDisableClientState(GL_FOG_COORDINATE_ARRAY);
        g->FogCoordf(gm->cur_fog);
    }
    for (u = 0; u < QGPU_MAX_UNITS && ok; u++) {
        int off_t = qgpu_vf_offset(fmt, (uint32_t)QGPU_VF_TEX(u));
        if (!tex[u]) {
            continue;
        }
        ok = gl_unit_env(c, st, u, tex[u]);
        glMatrixMode(GL_TEXTURE);
        glLoadMatrixf(gm->mtx[QGPU_MTX_TEXTURE0 + u]);
        glMatrixMode(GL_MODELVIEW);
        if (off_t >= 0) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(4, GL_FLOAT, stride, verts + off_t);
        } else {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            g->MultiTexCoord4fv(GL_TEXTURE0 + u, gm->cur_tex[u]);
        }
    }
    if (ok) {
        g->ClientActiveTexture(GL_TEXTURE0);
        if (idx) {
            glDrawElements(mode, (GLsizei)count, GL_UNSIGNED_INT, idx);
        } else {
            glDrawArrays(mode, (GLint)first, (GLsizei)count);
        }
    }
    gl_reset_raw(c);
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

/* Tampon combiné (v6) : on n'écrit QUE la composante demandée, et on ne
 * relit JAMAIS l'autre pour la remettre.
 *
 * POURQUOI (vu en vrai, sur cet hôte, avec tests/qgpu_core_test.c) : la
 * version précédente passait par glTexSubImage2D, qui exige le format de base
 * de la texture — donc GL_DEPTH_STENCIL / GL_UNSIGNED_INT_24_8 pour un
 * DEPTH24_STENCIL8. Elle relisait la composante à préserver, empaquetait les
 * deux sur 24+8 bits et réécrivait le bloc. Deux dégâts bien réels, tous deux
 * reproduits sur une surface 128×160 :
 *
 *   1. DÉBORDEMENT À 1,0 — le cas grave, et celui que l'invité voyait. Le
 *      calcul « (uint32_t)(d * 16777215.0f + 0.5f) » se fait en binary32 : au
 *      delà de 2^23 le pas vaut 1, donc 16777215.0f + 0.5f s'arrondit à
 *      16777216.0f, d24 vaut 0x1000000 et « d24 << 8 » déborde des 32 bits à
 *      ZÉRO. La profondeur 1,0 — celle que TOUT effacement pose sur le fond —
 *      devenait 0,0, c'est-à-dire un mur collé à l'œil : après un simple
 *      STENCIL_UPLOAD, plus aucune géométrie ne passait le test LESS.
 *   2. DÉRIVE À CHAQUE ALLER-RETOUR pour les autres valeurs. Sur Apple
 *      Silicon, GL_DEPTH24_STENCIL8 est émulé par un depth32float_stencil8
 *      (le GPU n'a pas de profondeur 24 bits entière) : le tampon garde donc
 *      un flottant 32 bits, et le passage par 24 bits entiers PERD de
 *      l'information. Mesuré ici : le pilote rend floor(d·(2^24−1)) pour la
 *      relecture 24_8, si bien qu'une profondeur relue puis réécrite se
 *      déplaçait d'un cran à chaque tour (0,5 → 0,50000006 → 0,500000119…),
 *      toujours vers le LOIN. Un LEQUAL sur une surface redessinée à la même
 *      profondeur finit par basculer.
 *
 * La seule façon sûre est donc de ne pas faire de lecture-modification-
 * écriture du tout. glDrawPixels le permet : GL_STENCIL_INDEX n'écrit que le
 * stencil (les fragments court-circuitent le pipeline, seuls comptent les
 * ciseaux et le masque d'écriture de stencil), et GL_DEPTH_COMPONENT en
 * GL_FLOAT laisse le pilote faire lui-même la conversion, exactement comme
 * pour le rastériseur et comme pour une surface à profondeur seule — donc
 * l'aller-retour redevient idempotent bit à bit. Il faut seulement fermer ce
 * qu'on ne veut pas toucher : le masque de couleur dans les deux cas, le
 * masque de stencil quand on pose la profondeur, le masque de profondeur
 * quand on pose le stencil. Et, pour la profondeur, RALLUMER le test en
 * GL_ALWAYS : sans test de profondeur, OpenGL n'écrit pas le tampon.
 *
 * gl_target(…, NULL) a déjà lié le FBO, posé le viewport et glDepthRange(0,1),
 * coupé tous les tests et ouvert tous les masques ; glWindowPos2i pose la
 * position de rastérisation en coordonnées de fenêtre, sans passer par les
 * matrices ni risquer d'être écartée par le test de validité. */
static bool gl_packed_upload(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h,
                             const float *depth, const uint8_t *sten)
{
    (void)c; (void)s;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    /* aucun transfert de pixels ne doit s'interposer : ni échelle et biais sur
       la profondeur, ni décalage et décalage d'index sur le stencil */
    glPixelTransferf(GL_DEPTH_SCALE, 1.0f);
    glPixelTransferf(GL_DEPTH_BIAS, 0.0f);
    glPixelTransferi(GL_INDEX_SHIFT, 0);
    glPixelTransferi(GL_INDEX_OFFSET, 0);
    glPixelZoom(1.0f, 1.0f);
    glWindowPos2i((GLint)x, (GLint)y);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    if (depth) {
        glStencilMask(0);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glDrawPixels((GLsizei)w, (GLsizei)h, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
        glDisable(GL_DEPTH_TEST);
        glStencilMask(0xFF);
    } else {
        glDepthMask(GL_FALSE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glDrawPixels((GLsizei)w, (GLsizei)h, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sten);
        glDepthMask(GL_TRUE);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    return glGetError() == GL_NO_ERROR;
}

static bool gl_depth_upload(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h, const float *src)
{
    GlSurface *gs = s->priv;
    if (!gl_target(c, s, NULL)) {
        return false;
    }
    /* Profondeur seule ou combinée : le MÊME chemin, celui des fragments.
       glTexSubImage2D passait par la conversion des transferts de pixels, qui
       n'arrondit pas forcément comme l'écriture d'un fragment : sur une RTX
       4060 Ti (pilote NVIDIA, Linux), 648/20479 devenait 0,0316421427 par
       glTexSubImage2D et 0,0316422023 par glDrawPixels — un cran de 24 bits
       d'écart : une application relisait une autre profondeur selon qu'elle
       avait demandé un stencil ou non. (Ce qui n'est PAS garanti, et ne l'était
       pas avant : qu'une profondeur téléversée soit bit à bit celle qu'aurait
       posée le rastériseur au même z — la transformation des sommets arrondit
       déjà, sur les deux backends.) */
    (void)gs;
    return gl_packed_upload(c, s, x, y, w, h, src, NULL);
}

static bool gl_stencil_readback(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h, uint8_t *dst)
{
    if (!gl_target(c, s, NULL)) {
        return false;
    }
    /* un octet par pixel, lignes jointives : alignement 1 */
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(x, y, w, h, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, dst);
    return glGetError() == GL_NO_ERROR;
}

static bool gl_stencil_upload(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h, const uint8_t *src)
{
    if (!gl_target(c, s, NULL)) {
        return false;
    }
    return gl_packed_upload(c, s, x, y, w, h, NULL, src);
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

/* ═══════════════ v8 : requêtes d'occlusion sur le GPU hôte ═════════════════
 *
 * GL_SAMPLES_PASSED natif (GL 1.5 ou ARB_occlusion_query) : c'est le GPU qui
 * compte, l'hôte ne rastérise rien. Le device étant synchrone, QUERY_RESULT
 * ATTEND le résultat — glGetQueryObjectuiv(GL_QUERY_RESULT) bloque jusqu'à ce
 * qu'il soit là, ce qui est exactement la sémantique attendue par l'invité.
 * Si les points d'entrée manquent, gl_init n'annonce pas QGPU_CAP_OCCLUSION et
 * le cœur refuse les opcodes avant d'arriver ici. */
typedef struct GlQuery {
    GLuint id;
} GlQuery;

static bool gl_query_begin(QgpuCore *c, QgpuQuery *q)
{
    GlState *g = c->be_priv;
    GlQuery *gq = q->priv;

    if (!g->has_query || !gl_make_current(g)) {
        return false;
    }
    if (!gq) {
        gq = calloc(1, sizeof(*gq));
        if (!gq) {
            return false;
        }
        g->GenQueries(1, &gq->id);
        q->priv = gq;
    }
    g->BeginQuery(GL_SAMPLES_PASSED, gq->id);
    return glGetError() == GL_NO_ERROR;
}

static bool gl_query_end(QgpuCore *c, QgpuQuery *q)
{
    GlState *g = c->be_priv;

    if (!g->has_query || !q->priv || !gl_make_current(g)) {
        return false;
    }
    g->EndQuery(GL_SAMPLES_PASSED);
    return glGetError() == GL_NO_ERROR;
}

static bool gl_query_result(QgpuCore *c, QgpuQuery *q)
{
    GlState *g = c->be_priv;
    GlQuery *gq = q->priv;
    GLuint n = 0;

    if (!g->has_query || !gq || !gl_make_current(g)) {
        return false;
    }
    g->GetQueryObjectuiv(gq->id, GL_QUERY_RESULT, &n);
    q->samples = n;
    return glGetError() == GL_NO_ERROR;
}

static void gl_query_destroy(QgpuCore *c, QgpuQuery *q)
{
    GlState *g = c->be_priv;
    GlQuery *gq = q->priv;

    if (gq && g->has_query && gl_make_current(g)) {
        g->DeleteQueries(1, &gq->id);
    }
    free(gq);
    q->priv = NULL;
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
    .draw_raw       = gl_draw_raw,
    .readback       = gl_readback,
    .upload         = gl_upload,
    .depth_readback = gl_depth_readback,
    .depth_upload   = gl_depth_upload,
    .stencil_readback = gl_stencil_readback,
    .stencil_upload = gl_stencil_upload,
    .tex_destroy    = gl_tex_destroy,
    .query_begin    = gl_query_begin,      /* v8 */
    .query_end      = gl_query_end,
    .query_result   = gl_query_result,
    .query_destroy  = gl_query_destroy,
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
