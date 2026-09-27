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
    /* G2 : ARB_texture_rectangle est une EXTENSION, pas un morceau d'OpenGL
       1.4 : `has_tex` ne dit rien d'elle. Sans elle, glDisable(RECTANGLE)
       rend GL_INVALID_ENUM à chaque unité et à chaque commande. */
    bool has_rect;
    /* G3 : vrai si les points d'entrée FBO résolus sont ceux d'EXT. */
    bool fbo_ext;
    /* Facultatifs, résolus pour les mineurs du rapport : liste d'extensions
       d'un contexte 3.0+, bornage des couleurs, compte d'occlusion 64 bits. */
    const GLubyte *(*GetStringi)(GLenum, GLuint);
    void (*ClampColor)(GLenum, GLenum);
    void (*GetQueryObjectui64v)(GLuint, GLenum, uint64_t *);
    const char *renderer;
    /* v16 : programmes ARB (ARB_vertex_program + ARB_fragment_program).
       Facultatifs : `has_prog` = QGPU_CAP_PROGRAMS annoncé. */
    void (*GenProgramsARB)(GLsizei, GLuint *);
    void (*DeleteProgramsARB)(GLsizei, const GLuint *);
    void (*BindProgramARB)(GLenum, GLuint);
    void (*ProgramStringARB)(GLenum, GLenum, GLsizei, const GLvoid *);
    void (*ProgramEnvParameter4fvARB)(GLenum, GLuint, const GLfloat *);
    void (*ProgramLocalParameter4fvARB)(GLenum, GLuint, const GLfloat *);
    void (*VertexAttribPointerARB)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                                   const GLvoid *);
    void (*EnableVertexAttribArrayARB)(GLuint);
    void (*DisableVertexAttribArrayARB)(GLuint);
    void (*GetProgramivARB)(GLenum, GLenum, GLint *);
    bool has_prog;
    GLint max_env[2], max_local[2];   /* limites de l'hôte, [VP, FP] */
    /* program.env est un état du contexte GL HÔTE, unique, que se partagent
       tous les contextes invités : on note à qui appartient ce qui y est. */
    const void *env_owner[2];
    uint32_t    env_high[2];       /* entrées de program.env possiblement non nulles */
    /* 27/09 (copie GPU) : blit de FBO (GL 3.0 / EXT_framebuffer_blit) pour le
       retournement de COPY_TEX, FACULTATIF (sans lui : une ligne par
       glCopyTexSubImage2D) ; copie vers une tranche 3D (GL 1.2). Le FBO
       intermédiaire `flip_*` reçoit le rectangle retourné, grandi à la
       demande. */
    void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                            GLbitfield, GLenum);
    void (*CopyTexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLint,
                              GLsizei, GLsizei);
    GLuint   flip_fbo, flip_tex;
    uint32_t flip_w, flip_h;
    /* v21 : programmes GLSL (OpenGL 2.0). `has_glsl` = QGPU_CAP_GLSL annoncé ;
       les attributs génériques passent par les entrées de la v16 (has_prog). */
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar **, const GLint *);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint *);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void (*DeleteShader)(GLuint);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint);
    void (*BindAttribLocation)(GLuint, GLuint, const GLchar *);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint *);
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void (*UseProgram)(GLuint);
    void (*DeleteProgram)(GLuint);
    GLint (*GetUniformLocation)(GLuint, const GLchar *);
    void (*Uniform1f)(GLint, GLfloat);
    void (*Uniform1fv)(GLint, GLsizei, const GLfloat *);
    void (*Uniform2fv)(GLint, GLsizei, const GLfloat *);
    void (*Uniform3fv)(GLint, GLsizei, const GLfloat *);
    void (*Uniform4fv)(GLint, GLsizei, const GLfloat *);
    void (*Uniform1iv)(GLint, GLsizei, const GLint *);
    void (*Uniform2iv)(GLint, GLsizei, const GLint *);
    void (*Uniform3iv)(GLint, GLsizei, const GLint *);
    void (*Uniform4iv)(GLint, GLsizei, const GLint *);
    void (*UniformMatrix2fv)(GLint, GLsizei, GLboolean, const GLfloat *);
    void (*UniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat *);
    void (*UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
    bool     has_glsl;
    GLuint   glsl_cur;             /* programme GLSL lié (glUseProgram), 0 sinon */
    GLfloat *glsl_fbuf;            /* QGPU_MAX_GLSL_SLOTS × 4, valeurs à pousser */
    GLint   *glsl_ibuf;
} GlState;

/* v16 : objet programme côté hôte. */
typedef struct GlProgram {
    GLuint id;
    bool   pos_invariant;          /* OPTION ARB_position_invariant : la position
                                      suit le pipeline fixe (et son retournement) */
    bool   uses_fpos;              /* fragments : lit fragment.position, réécrit en
                                      qgpu_fpos_ = (x, H − y) ; H dans program.env[fpos_env] */
} GlProgram;

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
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER      0x812D
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS       0x821D
#endif
#ifndef GL_MAX_TEXTURE_UNITS
#define GL_MAX_TEXTURE_UNITS    0x84E2
#endif
/* Bornage des couleurs (GL 2.0 / ARB_color_buffer_float) : hors profil de
   compatibilité classique, une implémentation peut laisser passer les
   composantes > 1 jusqu'au tampon, là où le backend de référence borne. */
#ifndef GL_CLAMP_VERTEX_COLOR
#define GL_CLAMP_VERTEX_COLOR   0x891A
#define GL_CLAMP_FRAGMENT_COLOR 0x891B
#define GL_CLAMP_READ_COLOR     0x891C
#define GL_FIXED_ONLY           0x891D
#endif

/* ─────────────────────── G1 : la file d'erreurs d'OpenGL ───────────────────
 *
 * OpenGL garde UN DRAPEAU PAR CODE d'erreur, et glGetError en rend (et en
 * efface) un seul par appel. Un « return glGetError() == GL_NO_ERROR » laisse
 * donc derrière lui les autres drapeaux, et une erreur née d'une commande
 * empoisonne la SUIVANTE, qui rend QGPU_ST_BACKEND alors qu'elle est saine.
 * Deux règles, désormais tenues partout :
 *   - gl_err_flush() à l'ENTRÉE (dans gl_make_current, la seule porte), pour
 *     ne juger que ce que cette commande-ci a fait ;
 *   - gl_err_ok() en SORTIE, qui VIDE la file et dit si elle était vide.
 * La borne de boucle est une ceinture : une implémentation qui rendrait
 * toujours la même erreur ne doit pas figer le thread de rendu. */
#define GL_ERR_DRAIN 64

static void gl_err_flush(void)
{
    int n = GL_ERR_DRAIN;
    while (n-- > 0 && glGetError() != GL_NO_ERROR) {
    }
}

static bool gl_err_ok(void)
{
    bool ok = true;
    int n = GL_ERR_DRAIN;

    while (n-- > 0 && glGetError() != GL_NO_ERROR) {
        ok = false;
    }
    return ok;
}

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
    if (CGLSetCurrentContext(g->ctx) != kCGLNoError) {
        return false;
    }
#else
    if (eglMakeCurrent(g->dpy, g->surf, g->surf, g->ctx) != EGL_TRUE) {
        return false;
    }
#endif
    /* G1 : PORTE D'ENTRÉE UNIQUE de toute opération du backend (gl_target,
       création/destruction de surface, requêtes d'occlusion). La purge est
       ici, donc aucune opération ne peut hériter de l'erreur d'une autre. */
    gl_err_flush();
    return true;
}

/* G2 : une extension est-elle là ? glGetString(GL_EXTENSIONS) est la forme
   1.x (et celle du profil de compatibilité) ; un contexte 3.0+ peut ne
   répondre que par glGetStringi. On accepte les deux, et on compare des MOTS
   ENTIERS (« GL_ARB_texture_rectangle » ne doit pas matcher un préfixe). */
static bool gl_has_ext(const GlState *g, const char *name)
{
    const char *s = (const char *)glGetString(GL_EXTENSIONS);
    size_t len = strlen(name);

    if (s) {
        const char *p = s;
        while ((p = strstr(p, name)) != NULL) {
            if ((p == s || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
                return true;
            }
            p += len;
        }
    } else if (g->GetStringi) {
        GLint n = 0, i;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (i = 0; i < n; i++) {
            const GLubyte *e = g->GetStringi(GL_EXTENSIONS, (GLuint)i);
            if (e && !strcmp((const char *)e, name)) {
                return true;
            }
        }
    }
    gl_err_flush();       /* un contexte cœur rend INVALID_ENUM sur EXTENSIONS */
    return false;
}

/* G3 : les deux jeux d'entrées FBO, au choix. `ext` demande la variante
 * EXT_framebuffer_object, sinon les noms cœur (ARB_framebuffer_object / GL 3.0).
 *
 * POURQUOI LE CHOIX. Sur macOS, OpenGL.framework exporte glGenFramebuffers
 * même dans un contexte hérité 2.1 (le symbole existe pour le profil cœur) :
 * dlsym réussit TOUJOURS et la bascule EXT ne jouait jamais. Si le contexte
 * n'a qu'EXT_framebuffer_object, la création de FBO échouerait — plus aucune
 * surface, donc zéro 3D. On prend donc l'EXT d'abord sur Apple, et gl_init
 * tranche pour de bon avec un FBO 1×1 : si le jeu choisi ne sait pas en faire
 * un, on rebascule sur l'autre et on re-sonde. */
static bool gl_resolve_fbo(GlState *g, bool ext)
{
    g->fbo_ext = ext;
    g->GenFramebuffers = gl_proc(ext ? "glGenFramebuffersEXT" : "glGenFramebuffers");
    g->DeleteFramebuffers = gl_proc(ext ? "glDeleteFramebuffersEXT" : "glDeleteFramebuffers");
    g->BindFramebuffer = gl_proc(ext ? "glBindFramebufferEXT" : "glBindFramebuffer");
    g->FramebufferTexture2D = gl_proc(ext ? "glFramebufferTexture2DEXT"
                                          : "glFramebufferTexture2D");
    g->CheckFramebufferStatus = gl_proc(ext ? "glCheckFramebufferStatusEXT"
                                            : "glCheckFramebufferStatus");
    return g->GenFramebuffers && g->DeleteFramebuffers && g->BindFramebuffer &&
           g->FramebufferTexture2D && g->CheckFramebufferStatus;
}

static bool gl_resolve(GlState *g)
{
#ifdef QGPU_GL_CGL
    const bool fbo_first_ext = true;              /* cf. gl_resolve_fbo */
#else
    const bool fbo_first_ext = false;
#endif
    if (!gl_resolve_fbo(g, fbo_first_ext)) {
        gl_resolve_fbo(g, !fbo_first_ext);
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
    /* FACULTATIFS : liste d'extensions d'un contexte 3.0+ (G2), bornage des
       couleurs, compte d'occlusion 64 bits. Leur absence n'interdit rien. */
    g->GetStringi = gl_proc("glGetStringi");
    g->ClampColor = gl_proc("glClampColor");
    if (!g->ClampColor) {
        g->ClampColor = gl_proc("glClampColorARB");
    }
    g->GetQueryObjectui64v = gl_proc("glGetQueryObjectui64v");
    if (!g->GetQueryObjectui64v) {
        g->GetQueryObjectui64v = gl_proc("glGetQueryObjectui64vEXT");
    }
    /* v16 : programmes ARB — FACULTATIFS (QGPU_CAP_PROGRAMS). Les noms ARB
       sont les seuls : ces extensions n'ont jamais été promues au cœur. */
    g->GenProgramsARB = gl_proc("glGenProgramsARB");
    g->DeleteProgramsARB = gl_proc("glDeleteProgramsARB");
    g->BindProgramARB = gl_proc("glBindProgramARB");
    g->ProgramStringARB = gl_proc("glProgramStringARB");
    g->ProgramEnvParameter4fvARB = gl_proc("glProgramEnvParameter4fvARB");
    g->ProgramLocalParameter4fvARB = gl_proc("glProgramLocalParameter4fvARB");
    g->VertexAttribPointerARB = gl_proc("glVertexAttribPointerARB");
    g->EnableVertexAttribArrayARB = gl_proc("glEnableVertexAttribArrayARB");
    g->DisableVertexAttribArrayARB = gl_proc("glDisableVertexAttribArrayARB");
    g->GetProgramivARB = gl_proc("glGetProgramivARB");
    /* 27/09 : copie GPU — FACULTATIFS (repli : une ligne à la fois, relecture). */
    g->BlitFramebuffer = gl_proc(g->fbo_ext ? "glBlitFramebufferEXT" : "glBlitFramebuffer");
    if (!g->BlitFramebuffer) {
        g->BlitFramebuffer = gl_proc(g->fbo_ext ? "glBlitFramebuffer" : "glBlitFramebufferEXT");
    }
    g->CopyTexSubImage3D = gl_proc("glCopyTexSubImage3D");
    if (!g->CopyTexSubImage3D) {
        g->CopyTexSubImage3D = gl_proc("glCopyTexSubImage3DEXT");
    }
    return g->GenFramebuffers && g->DeleteFramebuffers && g->BindFramebuffer &&
           g->FramebufferTexture2D && g->CheckFramebufferStatus &&
           g->BlendFuncSeparate && g->BlendEquationSeparate &&
           g->FogCoordPointer && g->ActiveTexture && g->ClientActiveTexture &&
           g->SecondaryColorPointer && g->SecondaryColor3fv &&
           g->MultiTexCoord4fv && g->FogCoordf && g->BlendColor;
}

/* Déclaré ici : l'auto-test de l'init (G6) emprunte EXACTEMENT le chemin de
   production du téléversement profondeur/stencil, plutôt qu'une imitation. */
static bool gl_packed_upload(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h,
                             const float *depth, const uint8_t *sten);

/* Un FBO 1×1 (couleur, plus profondeur+stencil si `zs`) lié et prêt. */
static bool gl_probe_make(GlState *g, bool zs, GLuint *fbo, GLuint *tex, GLuint *zt)
{
    *fbo = *tex = *zt = 0;
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
                 GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    if (zs) {
        glGenTextures(1, zt);
        glBindTexture(GL_TEXTURE_2D, *zt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 1, 1, 0,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    }
    g->GenFramebuffers(1, fbo);
    g->BindFramebuffer(GL_FRAMEBUFFER, *fbo);
    g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (zs) {
        g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, *zt, 0);
        g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, *zt, 0);
    }
    return g->CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void gl_probe_free(GlState *g, GLuint fbo, GLuint tex, GLuint zt)
{
    g->BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo) {
        g->DeleteFramebuffers(1, &fbo);
    }
    if (tex) {
        glDeleteTextures(1, &tex);
    }
    if (zt) {
        glDeleteTextures(1, &zt);
    }
    gl_err_flush();
}

/* G3 : un FBO couleur 1×1 se crée-t-il, et rend-il ce qu'on y efface ? C'est
   ce qui tranche entre les deux jeux d'entrées FBO, sans rien supposer de ce
   que dlsym a bien voulu trouver. */
static bool gl_probe_fbo(GlState *g)
{
    GLuint fbo, tex, zt;
    uint32_t px = 0;
    bool ok;

    gl_err_flush();
    ok = gl_probe_make(g, false, &fbo, &tex, &zt);
    if (ok) {
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(0, 0, 1, 1);
        glClearColor(0x34 / 255.0f, 0x56 / 255.0f, 0x78 / 255.0f, 0x12 / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glReadPixels(0, 0, 1, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &px);
        ok = px == 0x12345678u && gl_err_ok();
    }
    gl_probe_free(g, fbo, tex, zt);
    return ok;
}

/* G4 + G6 : ce que le backend EXIGE de l'hôte, vérifié une fois, à l'init.
 *
 * POURQUOI. Résoudre les symboles ne prouve rien : un contexte CŒUR les
 * résout tous et rend ensuite GL_INVALID_OPERATION sur tout le pipeline fixe
 * — le backend s'annoncerait (QGPU_CAP_GL) et l'invité verrait du noir. On
 * dessine donc VRAIMENT un triangle par les tableaux de sommets du pipeline
 * fixe, et on relit le pixel. Même chose pour l'aller-retour
 * profondeur/stencil par glDrawPixels (G6), le chemin le moins fréquenté des
 * pilotes modernes : s'il ment, la profondeur téléversée est fausse et
 * l'image l'est en silence. Tout échec ici rend gl_init false, donc repli sur
 * le backend logiciel — un rendu lent et juste vaut mieux qu'un rendu rapide
 * et faux. QGPU_GL_FORCE=1 lève le verdict (sans lever la trace) pour un hôte
 * dont on sait que seule la sonde est en tort. */
static bool gl_selftest(GlState *g, const char **why)
{
    static const float tri[3][6] = {              /* x y r g b a, espace de découpe */
        { -1.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f },
        {  3.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f },
        { -1.0f,  3.0f, 1.0f, 0.0f, 1.0f, 1.0f },
    };
    GLuint fbo, tex, zt;
    GLint units = 0;
    uint32_t px = 0;
    float d = 1.0f, d2 = -1.0f, d3 = -1.0f;
    uint8_t sv = 0;
    bool ok;

    *why = NULL;
    gl_err_flush();
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    if (!gl_err_ok() || units < QGPU_MAX_UNITS) {
        /* la requête elle-même n'existe plus en profil cœur */
        *why = "unités de texture du pipeline fixe";
        return false;
    }
    if (!gl_probe_make(g, true, &fbo, &tex, &zt)) {
        *why = "FBO 1×1 profondeur+stencil";
        gl_probe_free(g, fbo, tex, zt);
        return false;
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glViewport(0, 0, 1, 1);
    glDepthRange(0.0, 1.0);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, (GLsizei)sizeof(tri[0]), &tri[0][0]);
    glColorPointer(4, GL_FLOAT, (GLsizei)sizeof(tri[0]), &tri[0][2]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, 1, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &px);
    ok = gl_err_ok() && px == 0xFFFF00FFu;
    if (!ok) {
        *why = "pipeline fixe (tableaux de sommets, clear, relecture)";
    }
    /* G6 : aller-retour profondeur puis stencil, par le chemin de production.
       Le cas qui compte est 1,0 — celui que tout effacement pose au fond, et
       celui que l'ancien chemin 24_8 ramenait à 0,0 (mur collé à l'œil). */
    if (ok) {
        ok = gl_packed_upload(NULL, NULL, 0, 0, 1, 1, &d, NULL);
        glReadPixels(0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d2);
        ok = ok && gl_err_ok() && d2 == d;
        if (ok) {
            sv = 0xA5;
            ok = gl_packed_upload(NULL, NULL, 0, 0, 1, 1, NULL, &sv);
            sv = 0;
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, &sv);
            glReadPixels(0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d3);
            /* le stencil ne doit pas avoir bougé la profondeur : c'est TOUT
               l'intérêt de glDrawPixels plutôt que d'une lecture-écriture */
            ok = ok && gl_err_ok() && sv == 0xA5 && d3 == d2;
        }
        if (!ok) {
            *why = "aller-retour glDrawPixels profondeur/stencil";
        }
    }
    gl_probe_free(g, fbo, tex, zt);
    return ok;
}

/* ── v16 : programmes ARB ────────────────────────────────────────────────────
 *
 * LE RETOURNEMENT, encore. Le cœur rend la ligne 0 en haut ; le chemin brut
 * l'obtient en glissant un glScalef(1, −1, 1) dans la projection. Un programme
 * de sommets calcule result.position LUI-MÊME, le plus souvent avec des
 * matrices passées en program.env (Direct3D) que notre projection ne touche
 * pas. On réécrit donc le TEXTE : result.position devient un temporaire, et un
 * MUL final par {1, −1, 1, 1} le retourne. Un programme sous OPTION
 * ARB_position_invariant garde la transformation fixe — et son glScalef. */
#ifndef GL_VERTEX_PROGRAM_ARB
#define GL_VERTEX_PROGRAM_ARB              0x8620
#endif
#ifndef GL_FRAGMENT_PROGRAM_ARB
#define GL_FRAGMENT_PROGRAM_ARB            0x8804
#endif
#ifndef GL_PROGRAM_FORMAT_ASCII_ARB
#define GL_PROGRAM_FORMAT_ASCII_ARB        0x8875
#endif
#ifndef GL_PROGRAM_ERROR_POSITION_ARB
#define GL_PROGRAM_ERROR_POSITION_ARB      0x864B
#endif
#ifndef GL_PROGRAM_ERROR_STRING_ARB
#define GL_PROGRAM_ERROR_STRING_ARB        0x8874
#endif
#ifndef GL_MAX_PROGRAM_ENV_PARAMETERS_ARB
#define GL_MAX_PROGRAM_ENV_PARAMETERS_ARB  0x88B5
#endif
#ifndef GL_MAX_PROGRAM_LOCAL_PARAMETERS_ARB
#define GL_MAX_PROGRAM_LOCAL_PARAMETERS_ARB 0x88B4
#endif

static bool ident_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '$';
}

/* Réécrit un programme de sommets pour retourner y (cf. ci-dessus). Rend un
   texte alloué, ou NULL si la réécriture est impossible (pas d'en-tête, pas
   de END). Les occurrences de result.position deviennent qgpu_pos_ ; une
   déclaration « OUTPUT nom = qgpu_pos_; » devient un ALIAS, ce que la
   grammaire ARB permet pour un temporaire. */
static char *gl_prog_flip(const char *src, size_t len)
{
    static const char key[] = "result.position";
    static const char tmp[] = "qgpu_pos_";
    static const char decl[] = "TEMP qgpu_pos_;\n";
    static const char fin[] = "MUL result.position, qgpu_pos_, {1.0, -1.0, 1.0, 1.0};\n";
    const size_t klen = sizeof(key) - 1, tlen = sizeof(tmp) - 1;
    const char *hdr_end = memchr(src, '\n', len);
    const char *end_kw = NULL, *p;
    size_t i, o = 0;
    char *out;

    if (!hdr_end) {
        return NULL;
    }
    /* le dernier END à une frontière de mot */
    for (p = src; (p = memmem(p, (size_t)(src + len - p), "END", 3)) != NULL; p += 3) {
        bool left = (p == src) || !ident_char(p[-1]);
        bool right = (p + 3 >= src + len) || !ident_char(p[3]);
        if (left && right) {
            end_kw = p;
        }
    }
    if (!end_kw) {
        return NULL;
    }
    out = malloc(len + sizeof(decl) + sizeof(fin) + 8);
    if (!out) {
        return NULL;
    }
    /* en-tête, puis la déclaration du temporaire */
    i = (size_t)(hdr_end - src) + 1;
    memcpy(out, src, i);
    o = i;
    memcpy(out + o, decl, sizeof(decl) - 1);
    o += sizeof(decl) - 1;
    while (i < len) {
        if (src + i == end_kw) {
            memcpy(out + o, fin, sizeof(fin) - 1);
            o += sizeof(fin) - 1;
        }
        if (len - i >= klen && memcmp(src + i, key, klen) == 0 &&
            (i == 0 || !ident_char(src[i - 1])) &&
            (i + klen >= len || !ident_char(src[i + klen]))) {
            /* OUTPUT nom = result.position → ALIAS nom = qgpu_pos_ */
            size_t b = o;
            while (b > 0 && (out[b - 1] == ' ' || out[b - 1] == '\t')) b--;
            if (b > 0 && out[b - 1] == '=') {
                size_t q = b - 1;
                while (q > 0 && (out[q - 1] == ' ' || out[q - 1] == '\t')) q--;
                while (q > 0 && ident_char(out[q - 1])) q--;
                while (q > 0 && (out[q - 1] == ' ' || out[q - 1] == '\t')) q--;
                if (q >= 6 && memcmp(out + q - 6, "OUTPUT", 6) == 0 &&
                    (q == 6 || !ident_char(out[q - 7]))) {
                    memcpy(out + q - 6, "ALIAS ", 6);
                }
            }
            memcpy(out + o, tmp, tlen);
            o += tlen;
            i += klen;
            continue;
        }
        out[o++] = src[i++];
    }
    out[o] = '\0';
    return out;
}

/* v17 (24/09/2026, vitres de DOOM 3) — LE RETOURNEMENT, côté fragments.
   fragment.position.y est en coordonnées de fenêtre de l'hôte, où l'image est
   dessinée à l'envers ; un programme qui s'en sert pour relire une copie
   d'écran (_currentRender de DOOM 3 et Prey, heatHaze, vitres) échantillonnait
   la ligne miroir — hors de la zone copiée, donc du noir. On réécrit le
   texte : fragment.position devient qgpu_fpos_ = (x, H − y, z, w), avec H la
   hauteur de la surface, passée dans program.env[env_idx] (le dernier index
   que l'hôte tient ; le jeu n'y va pas). Insertion après les OPTION (la
   grammaire ARB les veut en tête). NULL si impossible. */
static char *gl_prog_flip_fp(const char *src, size_t len, int env_idx)
{
    static const char key[] = "fragment.position";
    static const char tmp[] = "qgpu_fpos_";
    const size_t klen = sizeof(key) - 1, tlen = sizeof(tmp) - 1;
    const char *hdr_end = memchr(src, '\n', len);
    char decl[200];
    size_t i, o = 0, ins, dlen;
    char *out;

    if (!hdr_end) {
        return NULL;
    }
    snprintf(decl, sizeof(decl),
             "PARAM qgpu_fh_ = program.env[%d];\nTEMP qgpu_fpos_;\n"
             "MOV qgpu_fpos_, fragment.position;\n"
             "ADD qgpu_fpos_.y, qgpu_fh_.y, -fragment.position.y;\n", env_idx);
    dlen = strlen(decl);
    /* point d'insertion : après l'en-tête et les lignes OPTION / vides / commentaires */
    ins = (size_t)(hdr_end - src) + 1;
    for (;;) {
        size_t j = ins;
        while (j < len && (src[j] == ' ' || src[j] == '\t')) j++;
        if (j < len && (src[j] == '\n' || src[j] == '\r' || src[j] == '#' ||
                        (len - j >= 6 && memcmp(src + j, "OPTION", 6) == 0))) {
            const char *nl = memchr(src + j, '\n', len - j);
            if (!nl) {
                return NULL;
            }
            ins = (size_t)(nl - src) + 1;
            continue;
        }
        break;
    }
    out = malloc(len + dlen + 8);
    if (!out) {
        return NULL;
    }
    i = 0;
    while (i < len) {
        if (i == ins) {
            memcpy(out + o, decl, dlen);
            o += dlen;
        }
        if (i >= ins && len - i >= klen && memcmp(src + i, key, klen) == 0 &&
            (i == 0 || !ident_char(src[i - 1])) &&
            (i + klen >= len || !ident_char(src[i + klen]))) {
            memcpy(out + o, tmp, tlen);
            o += tlen;
            i += klen;
            continue;
        }
        out[o++] = src[i++];
    }
    out[o] = '\0';
    return out;
}

/* Compile `text` dans l'objet lié ; rend false et explique sur stderr si le
   compilateur de l'hôte refuse. */
static bool gl_prog_compile(GlState *g, GLenum target, const char *text, size_t len,
                            const char *what)
{
    GLint pos = -1;
    gl_err_flush();
    g->ProgramStringARB(target, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)len, text);
    if (glGetError() == GL_NO_ERROR) {
        return true;
    }
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos);
    fprintf(stderr, "qgpu: programme %s refusé par l'hôte à l'octet %d : %s\n",
            what, (int)pos, (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB));
    gl_err_flush();
    return false;
}

static bool gl_prog_string(QgpuCore *c, QgpuProgram *p)
{
    GlState *g = c->be_priv;
    GlProgram *gp = p->priv;
    GLenum target = p->target == QGPU_PT_VERTEX ? GL_VERTEX_PROGRAM_ARB
                                                : GL_FRAGMENT_PROGRAM_ARB;
    char *flipped = NULL;
    bool ok;

    if (!g->has_prog || !gl_make_current(g)) {
        return false;
    }
    if (!gp) {
        gp = calloc(1, sizeof(*gp));
        if (!gp) {
            return false;
        }
        g->GenProgramsARB(1, &gp->id);
        p->priv = gp;
    }
    g->BindProgramARB(target, gp->id);
    gp->pos_invariant = (target == GL_FRAGMENT_PROGRAM_ARB) ||
                        strstr(p->text, "ARB_position_invariant") != NULL;
    gp->uses_fpos = false;
    if (target == GL_FRAGMENT_PROGRAM_ARB && memmem(p->text, p->len, "fragment.position", 17)) {
        flipped = gl_prog_flip_fp(p->text, p->len, g->max_env[QGPU_PROG_FP] - 1);
        ok = flipped && gl_prog_compile(g, target, flipped, strlen(flipped),
                                        "de fragments (fragment.position retourné)");
        if (!ok && c->trace && flipped) {
            fprintf(stderr, "qgpu: texte réécrit :\n%s\n", flipped);
        }
        free(flipped);
        flipped = NULL;
        gp->uses_fpos = ok;
    } else if (gp->pos_invariant) {
        ok = gl_prog_compile(g, target, p->text, p->len,
                             target == GL_VERTEX_PROGRAM_ARB ? "de sommets" : "de fragments");
    } else {
        flipped = gl_prog_flip(p->text, p->len);
        ok = flipped && gl_prog_compile(g, target, flipped, strlen(flipped),
                                        "de sommets (retourné)");
        if (!ok && c->trace && flipped) {
            fprintf(stderr, "qgpu: texte réécrit :\n%s\n", flipped);
        }
        free(flipped);
    }
    g->BindProgramARB(target, 0);
    return ok;
}

static void gl_glsl_destroy(GlState *g, QgpuProgram *p);

static void gl_prog_destroy(QgpuCore *c, QgpuProgram *p)
{
    GlState *g = c->be_priv;
    GlProgram *gp = p->priv;
    if (p->target == QGPU_PT_GLSL) {             /* v21 */
        gl_glsl_destroy(g, p);
        return;
    }
    if (gp) {
        if (g && g->has_prog && gl_make_current(g)) {
            g->DeleteProgramsARB(1, &gp->id);
        }
        free(gp);
        p->priv = NULL;
    }
}

/* À l'init : les deux extensions, tous les points d'entrée, et un programme
   d'essai de chaque cible — passé par la réécriture, comme en production. */
static bool gl_prog_probe(GlState *g)
{
    static const char vp[] =
        "!!ARBvp1.0\n"
        "OUTPUT oPos = result.position;\n"
        "DP4 oPos.x, state.matrix.mvp.row[0], vertex.position;\n"
        "DP4 oPos.y, state.matrix.mvp.row[1], vertex.position;\n"
        "DP4 oPos.z, state.matrix.mvp.row[2], vertex.position;\n"
        "DP4 oPos.w, state.matrix.mvp.row[3], vertex.position;\n"
        "MOV result.color, vertex.attrib[1];\n"
        "END\n";
    static const char fp[] =
        "!!ARBfp1.0\n"
        "TEMP t;\n"
        "TEX t, fragment.texcoord[0], texture[0], 2D;\n"
        "MUL result.color, t, fragment.color.primary;\n"
        "END\n";
    GLuint ids[2];
    char *flipped;
    bool ok;

    if (!g->GenProgramsARB || !g->DeleteProgramsARB || !g->BindProgramARB ||
        !g->ProgramStringARB || !g->ProgramEnvParameter4fvARB ||
        !g->ProgramLocalParameter4fvARB || !g->VertexAttribPointerARB ||
        !g->EnableVertexAttribArrayARB || !g->DisableVertexAttribArrayARB ||
        !g->GetProgramivARB) {
        return false;
    }
    if (!gl_has_ext(g, "GL_ARB_vertex_program") ||
        !gl_has_ext(g, "GL_ARB_fragment_program")) {
        return false;
    }
    g->GenProgramsARB(2, ids);
    g->BindProgramARB(GL_VERTEX_PROGRAM_ARB, ids[0]);
    flipped = gl_prog_flip(vp, sizeof(vp) - 1);
    ok = flipped && gl_prog_compile(g, GL_VERTEX_PROGRAM_ARB, flipped, strlen(flipped),
                                    "d'essai (sommets)");
    free(flipped);
    if (ok) {
        g->GetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_MAX_PROGRAM_ENV_PARAMETERS_ARB,
                           &g->max_env[QGPU_PROG_VP]);
        g->GetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_MAX_PROGRAM_LOCAL_PARAMETERS_ARB,
                           &g->max_local[QGPU_PROG_VP]);
        g->BindProgramARB(GL_FRAGMENT_PROGRAM_ARB, ids[1]);
        ok = gl_prog_compile(g, GL_FRAGMENT_PROGRAM_ARB, fp, sizeof(fp) - 1,
                             "d'essai (fragments)");
        g->GetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_MAX_PROGRAM_ENV_PARAMETERS_ARB,
                           &g->max_env[QGPU_PROG_FP]);
        g->GetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_MAX_PROGRAM_LOCAL_PARAMETERS_ARB,
                           &g->max_local[QGPU_PROG_FP]);
        g->BindProgramARB(GL_FRAGMENT_PROGRAM_ARB, 0);
    }
    g->BindProgramARB(GL_VERTEX_PROGRAM_ARB, 0);
    g->DeleteProgramsARB(2, ids);
    gl_err_flush();
    /* Colin McRae emploie program.env[0..95] : en deçà, on n'annonce rien. */
    return ok && g->max_env[QGPU_PROG_VP] >= 96 && g->max_env[QGPU_PROG_FP] >= 24 &&
           g->max_local[QGPU_PROG_VP] >= 96 && g->max_local[QGPU_PROG_FP] >= 24;
}

/* Au dessin : lie le programme de la cible et pousse ses paramètres. `w` est
   l'indice QGPU_PROG_*, `pg` le jeu du contexte invité courant. */
static void gl_prog_use(GlState *g, QgpuProgSet *pg, int w, QgpuProgram *p, float surf_h)
{
    GLenum target = w == QGPU_PROG_VP ? GL_VERTEX_PROGRAM_ARB : GL_FRAGMENT_PROGRAM_ARB;
    GlProgram *gp = p->priv;
    uint32_t i, n;

    g->BindProgramARB(target, gp->id);
    glEnable(target);
    if (w == QGPU_PROG_FP && gp->uses_fpos) {
        GLfloat fh[4];
        fh[0] = 0.0f; fh[1] = surf_h; fh[2] = 0.0f; fh[3] = 0.0f;
        g->ProgramEnvParameter4fvARB(target, (GLuint)(g->max_env[QGPU_PROG_FP] - 1), fh);
    }
    if (pg->env_dirty[w] || g->env_owner[w] != pg) {
        /* Tout ce que ce contexte a posé — et, si l'hôte porte encore les
           valeurs d'un autre contexte, assez de zéros pour les recouvrir. */
        n = pg->env_hi[w] > g->env_high[w] ? pg->env_hi[w] : g->env_high[w];
        if (n > (uint32_t)g->max_env[w]) {
            n = (uint32_t)g->max_env[w];
        }
        for (i = 0; i < n; i++) {
            g->ProgramEnvParameter4fvARB(target, i, pg->env[w][i]);
        }
        g->env_high[w] = n;
        pg->env_dirty[w] = false;
        g->env_owner[w] = pg;
    }
    if (p->local_dirty) {
        n = p->local_hi;
        if (n > (uint32_t)g->max_local[w]) {
            n = (uint32_t)g->max_local[w];
        }
        for (i = 0; i < n; i++) {
            g->ProgramLocalParameter4fvARB(target, i, p->local[i]);
        }
        p->local_dirty = false;
    }
}


/* ── v21 : programmes GLSL ──────────────────────────────────────────────────
 *
 * L'hôte recompile le texte GLSL de l'invité tel quel (GLSL 1.10 : un
 * contexte hérité 2.1 de macOS l'accepte sans #version), à deux réécritures
 * près, pour LE RETOURNEMENT (cf. la v16) :
 *
 *   - sommets : le main de l'invité devient qgpu_main_ et un main ajouté à la
 *     fin l'appelle puis retourne gl_Position.y. La projection de l'hôte n'est
 *     alors PAS retournée (gl_ProjectionMatrix, ftransform() sont ceux de
 *     l'invité) ; sans shader de sommets, le pipeline fixe garde son
 *     glScalef(1, −1, 1) ;
 *   - fragments : gl_FragCoord devient qgpu_FragCoord_ = (x, H − y, z, w),
 *     calculé par le main ajouté avant d'appeler qgpu_main_ ; H est l'uniform
 *     qgpu_fh_, la hauteur de la surface, posé à chaque dessin.
 *
 * Les déclarations ajoutées vont après la dernière ligne #version / #extension
 * (celles-ci doivent précéder tout code) ; les commentaires sont recopiés
 * sans être réécrits. */
#ifndef GL_VERTEX_SHADER
#define GL_FRAGMENT_SHADER                 0x8B30
#define GL_VERTEX_SHADER                   0x8B31
#define GL_COMPILE_STATUS                  0x8B81
#define GL_LINK_STATUS                     0x8B82
#define GL_INFO_LOG_LENGTH                 0x8B84
#endif
#ifndef GL_MAX_TEXTURE_IMAGE_UNITS
#define GL_MAX_TEXTURE_IMAGE_UNITS         0x8872
#endif

typedef struct GlGlsl {
    GLuint prog;
    GLint  fh_loc;                 /* qgpu_fh_ (gl_FragCoord réécrit), −1 sinon */
} GlGlsl;

static bool glsl_ident(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

/* Où insérer nos déclarations : après la dernière ligne qui commence (blancs
   admis) par #version ou #extension, sinon au début. */
static size_t glsl_insert_point(const char *s, size_t len)
{
    size_t i = 0, at = 0;
    while (i < len) {
        size_t j = i, e;
        while (j < len && (s[j] == ' ' || s[j] == '\t')) {
            j++;
        }
        e = j;
        while (e < len && s[e] != '\n') {
            e++;
        }
        if (j < len && s[j] == '#') {
            size_t k = j + 1;
            while (k < e && (s[k] == ' ' || s[k] == '\t')) {
                k++;
            }
            if ((e - k >= 7 && !memcmp(s + k, "version", 7)) ||
                (e - k >= 9 && !memcmp(s + k, "extension", 9))) {
                at = e < len ? e + 1 : len;
            }
        }
        i = e + 1;
    }
    return at;
}

/* Réécrit un texte GLSL (cf. ci-dessus). `fragcoord` : remplacer gl_FragCoord.
   `*had_main` : un main y a été renommé (ce texte reçoit le main ajouté).
   Rend un texte alloué, ou NULL. */
static char *glsl_rewrite(const char *s, size_t len, bool vertex, bool fragcoord,
                          bool *had_main)
{
    static const char vs_main[] =
        "\nvoid main()\n{\n    qgpu_main_();\n    gl_Position.y = -gl_Position.y;\n}\n";
    static const char fs_main[] =
        "\nvoid main()\n{\n    qgpu_FragCoord_ = vec4(gl_FragCoord.x, qgpu_fh_ - gl_FragCoord.y,"
        " gl_FragCoord.z, gl_FragCoord.w);\n    qgpu_main_();\n}\n";
    static const char fs_decl[] = "vec4 qgpu_FragCoord_;\n";
    static const char fs_decl_main[] = "uniform float qgpu_fh_;\n";
    size_t ins = glsl_insert_point(s, len), i = 0, o = 0, cap;
    bool mains = false, fc = false;
    char *out;

    *had_main = false;
    if (!vertex && !fragcoord) {             /* rien à réécrire */
        out = malloc(len + 1);
        if (out) {
            memcpy(out, s, len);
            out[len] = '\0';
        }
        return out;
    }

    /* taille : chaque renommage allonge d'au plus 16 octets */
    cap = len * 3 + sizeof(vs_main) + sizeof(fs_main) + sizeof(fs_decl) +
          sizeof(fs_decl_main) + 64;
    out = malloc(cap);
    if (!out) {
        return NULL;
    }
    /* premier passage : y a-t-il un main, un gl_FragCoord (hors commentaires) ? */
    while (i < len) {
        if (s[i] == '/' && i + 1 < len && s[i + 1] == '/') {
            while (i < len && s[i] != '\n') i++;
            continue;
        }
        if (s[i] == '/' && i + 1 < len && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        if (glsl_ident(s[i]) && (i == 0 || !glsl_ident(s[i - 1]))) {
            size_t j = i;
            while (j < len && glsl_ident(s[j])) j++;
            if (j - i == 4 && !memcmp(s + i, "main", 4)) {
                mains = true;
            } else if (fragcoord && j - i == 12 && !memcmp(s + i, "gl_FragCoord", 12)) {
                fc = true;
            }
            i = j;
            continue;
        }
        i++;
    }
    i = 0;
    while (i < len) {
        if (i == ins && !vertex && fragcoord && (fc || mains)) {
            memcpy(out + o, fs_decl, sizeof(fs_decl) - 1);
            o += sizeof(fs_decl) - 1;
            if (mains) {
                memcpy(out + o, fs_decl_main, sizeof(fs_decl_main) - 1);
                o += sizeof(fs_decl_main) - 1;
            }
        }
        if (s[i] == '/' && i + 1 < len && s[i + 1] == '/') {
            while (i < len && s[i] != '\n') out[o++] = s[i++];
            continue;
        }
        if (s[i] == '/' && i + 1 < len && s[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < len && !(s[j] == '*' && s[j + 1] == '/')) j++;
            j = j + 2 < len ? j + 2 : len;
            /* un commentaire qui enjambe le point d'insertion : on insère
               après lui (ins avance) */
            if (ins > i && ins < j) {
                ins = j;
            }
            memcpy(out + o, s + i, j - i);
            o += j - i;
            i = j;
            continue;
        }
        if (glsl_ident(s[i]) && (i == 0 || !glsl_ident(s[i - 1]))) {
            size_t j = i;
            while (j < len && glsl_ident(s[j])) j++;
            if (ins > i && ins < j) {
                ins = j;
            }
            if (j - i == 4 && !memcmp(s + i, "main", 4)) {
                memcpy(out + o, "qgpu_main_", 10);
                o += 10;
            } else if (!vertex && fragcoord && j - i == 12 &&
                       !memcmp(s + i, "gl_FragCoord", 12)) {
                memcpy(out + o, "qgpu_FragCoord_", 15);
                o += 15;
            } else {
                memcpy(out + o, s + i, j - i);
                o += j - i;
            }
            i = j;
            continue;
        }
        out[o++] = s[i++];
    }
    if (ins >= len && !vertex && fragcoord && (fc || mains)) {
        memcpy(out + o, fs_decl, sizeof(fs_decl) - 1);
        o += sizeof(fs_decl) - 1;
        if (mains) {
            memcpy(out + o, fs_decl_main, sizeof(fs_decl_main) - 1);
            o += sizeof(fs_decl_main) - 1;
        }
    }
    if (mains) {
        const char *m = vertex ? vs_main : fs_main;
        size_t ml = strlen(m);
        memcpy(out + o, m, ml);
        o += ml;
    }
    out[o] = '\0';
    *had_main = mains;
    return out;
}

/* Ajoute `what` au journal (qui grandit). */
static void glsl_log(char **log, const char *what)
{
    size_t a = *log ? strlen(*log) : 0, b = strlen(what);
    char *n;
    if (a + b + 1 > QGPU_MAX_GLSL_LOG) {
        return;
    }
    n = realloc(*log, a + b + 1);
    if (!n) {
        return;
    }
    memcpy(n + a, what, b + 1);
    *log = n;
}

static void glsl_log_object(GlState *g, char **log, GLuint obj, bool program,
                            const char *head)
{
    GLint n = 0;
    char *buf;
    if (program) {
        g->GetProgramiv(obj, GL_INFO_LOG_LENGTH, &n);
    } else {
        g->GetShaderiv(obj, GL_INFO_LOG_LENGTH, &n);
    }
    if (n <= 1) {
        return;
    }
    buf = malloc((size_t)n + 1);
    if (!buf) {
        return;
    }
    buf[0] = 0;
    if (program) {
        g->GetProgramInfoLog(obj, n, NULL, buf);
    } else {
        g->GetShaderInfoLog(obj, n, NULL, buf);
    }
    buf[n] = 0;
    glsl_log(log, head);
    glsl_log(log, buf);
    free(buf);
}

/* Compile et lie ; false = refus de l'hôte (journal dans *log). Ne touche à
   aucun état sauf le programme courant (remis à 0). */
static bool gl_glsl_build(GlState *g, QgpuGlsl *q, GlGlsl *gg, char **log, bool trace)
{
    GLuint prog, sh[QGPU_MAX_GLSL_SRC];
    GLint okv = 0;
    bool fragcoord = false, ok = true;
    uint32_t i, nsh = 0;
    char head[96];

    /* gl_FragCoord réécrit dans TOUS les textes de fragments si l'un d'eux
       le lit : la variable globale est partagée entre eux */
    for (i = 0; i < q->nsrc; i++) {
        if (q->stage[i] == QGPU_GLSL_FRAGMENT && strstr(q->src[i], "gl_FragCoord")) {
            fragcoord = true;
        }
    }
    prog = g->CreateProgram();
    if (!prog) {
        glsl_log(log, "qgpu : glCreateProgram a échoué\n");
        return false;
    }
    for (i = 0; i < q->nsrc && ok; i++) {
        bool vertex = q->stage[i] == QGPU_GLSL_VERTEX, had_main = false;
        char *text = glsl_rewrite(q->src[i], q->src_len[i], vertex, fragcoord, &had_main);
        const GLchar *tp;
        if (!text) {
            ok = false;
            break;
        }
        sh[nsh] = g->CreateShader(vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        tp = text;
        g->ShaderSource(sh[nsh], 1, &tp, NULL);
        g->CompileShader(sh[nsh]);
        g->GetShaderiv(sh[nsh], GL_COMPILE_STATUS, &okv);
        snprintf(head, sizeof(head), "-- texte %u (%s)%s :\n", i,
                 vertex ? "sommets" : "fragments", okv ? "" : " REFUSÉ");
        glsl_log_object(g, log, sh[nsh], false, head);
        if (!okv) {
            ok = false;
            if (trace) {
                fprintf(stderr, "qgpu: texte GLSL réécrit :\n%s\n", text);
            }
        }
        g->AttachShader(prog, sh[nsh]);
        nsh++;
        free(text);
    }
    if (ok) {
        for (i = 0; i < QGPU_MAX_GLSL_ATTRIBS; i++) {
            if (q->attr[i]) {
                g->BindAttribLocation(prog, i, q->attr[i]);
            }
        }
        g->LinkProgram(prog);
        g->GetProgramiv(prog, GL_LINK_STATUS, &okv);
        glsl_log_object(g, log, prog, true, okv ? "-- édition des liens :\n"
                                                : "-- édition des liens REFUSÉE :\n");
        ok = okv != 0;
    }
    for (i = 0; i < nsh; i++) {
        g->DeleteShader(sh[i]);              /* détachés à la destruction */
    }
    if (!ok) {
        g->DeleteProgram(prog);
        gl_err_flush();
        return false;
    }
    gg->prog = prog;
    for (i = 0; i < q->nunif; i++) {
        QgpuGlslUniform *u = &q->unif[i];
        GLint loc = g->GetUniformLocation(prog, u->name);
        if (loc < 0 && u->count > 1) {
            char nm[QGPU_MAX_GLSL_NAME + 8];
            snprintf(nm, sizeof(nm), "%s[0]", u->name);
            loc = g->GetUniformLocation(prog, nm);
        }
        u->host_loc = loc;
        u->dirty = true;
    }
    gg->fh_loc = fragcoord ? g->GetUniformLocation(prog, "qgpu_fh_") : -1;
    return gl_err_ok();
}

static bool gl_glsl_link(QgpuCore *c, QgpuProgram *p)
{
    GlState *g = c->be_priv;
    GlGlsl *gg;
    bool ok;

    if (!g->has_glsl || !gl_make_current(g)) {
        return false;
    }
    gg = calloc(1, sizeof(*gg));
    if (!gg) {
        return false;
    }
    gg->fh_loc = -1;
    ok = gl_glsl_build(g, p->glsl, gg, &p->glsl->log, c->trace);
    if (!ok) {
        fprintf(stderr, "qgpu: programme GLSL refusé par l'hôte :\n%s\n",
                p->glsl->log ? p->glsl->log : "(sans journal)");
        free(gg);
        return false;
    }
    p->priv = gg;
    return true;
}

static void gl_glsl_destroy(GlState *g, QgpuProgram *p)
{
    GlGlsl *gg = p->priv;
    if (gg) {
        if (g && g->has_glsl && gl_make_current(g)) {
            if (g->glsl_cur == gg->prog) {
                g->UseProgram(0);
                g->glsl_cur = 0;
            }
            g->DeleteProgram(gg->prog);
        }
        free(gg);
        p->priv = NULL;
    }
}

/* Composantes par élément et par emplacement (mat3 : 3 par colonne). */
static uint32_t gl_glsl_comps(uint32_t t)
{
    switch (t) {
    case QGPU_GT_FLOAT_VEC2: case QGPU_GT_INT_VEC2: case QGPU_GT_BOOL_VEC2:
    case QGPU_GT_FLOAT_MAT2:
        return 2;
    case QGPU_GT_FLOAT_VEC3: case QGPU_GT_INT_VEC3: case QGPU_GT_BOOL_VEC3:
    case QGPU_GT_FLOAT_MAT3:
        return 3;
    case QGPU_GT_FLOAT_VEC4: case QGPU_GT_INT_VEC4: case QGPU_GT_BOOL_VEC4:
    case QGPU_GT_FLOAT_MAT4:
        return 4;
    default:
        return 1;
    }
}

/* Au dessin : pousse les uniforms dont une valeur a changé (le programme est
   lié par l'appelant). Les valeurs vivent dans l'objet programme de l'hôte :
   seul ce qui a bougé repart. */
static void gl_glsl_uniforms(GlState *g, QgpuGlsl *q)
{
    uint32_t i;
    for (i = 0; i < q->nunif; i++) {
        QgpuGlslUniform *u = &q->unif[i];
        uint32_t slots, comps, j, k, n = 0;
        bool isint;
        if (!u->dirty) {
            continue;
        }
        u->dirty = false;
        if (u->host_loc < 0) {
            continue;
        }
        slots = (uint32_t)QGPU_GT_SLOTS(u->type);
        comps = gl_glsl_comps(u->type);
        isint = !(u->type == QGPU_GT_FLOAT || (u->type >= QGPU_GT_FLOAT_VEC2 &&
                                               u->type <= QGPU_GT_FLOAT_VEC4) ||
                  QGPU_GT_IS_MAT(u->type));
        for (j = 0; j < u->count * slots; j++) {
            const uint32_t *w = q->val[u->slot + j];
            for (k = 0; k < comps; k++) {
                if (isint) {
                    g->glsl_ibuf[n++] = (GLint)w[k];
                } else {
                    g->glsl_fbuf[n++] = qgpu_u2f(w[k]);
                }
            }
        }
        switch (u->type) {
        case QGPU_GT_FLOAT:      g->Uniform1fv(u->host_loc, u->count, g->glsl_fbuf); break;
        case QGPU_GT_FLOAT_VEC2: g->Uniform2fv(u->host_loc, u->count, g->glsl_fbuf); break;
        case QGPU_GT_FLOAT_VEC3: g->Uniform3fv(u->host_loc, u->count, g->glsl_fbuf); break;
        case QGPU_GT_FLOAT_VEC4: g->Uniform4fv(u->host_loc, u->count, g->glsl_fbuf); break;
        case QGPU_GT_FLOAT_MAT2:
            g->UniformMatrix2fv(u->host_loc, u->count, GL_FALSE, g->glsl_fbuf);
            break;
        case QGPU_GT_FLOAT_MAT3:
            g->UniformMatrix3fv(u->host_loc, u->count, GL_FALSE, g->glsl_fbuf);
            break;
        case QGPU_GT_FLOAT_MAT4:
            g->UniformMatrix4fv(u->host_loc, u->count, GL_FALSE, g->glsl_fbuf);
            break;
        case QGPU_GT_INT_VEC2: case QGPU_GT_BOOL_VEC2:
            g->Uniform2iv(u->host_loc, u->count, g->glsl_ibuf);
            break;
        case QGPU_GT_INT_VEC3: case QGPU_GT_BOOL_VEC3:
            g->Uniform3iv(u->host_loc, u->count, g->glsl_ibuf);
            break;
        case QGPU_GT_INT_VEC4: case QGPU_GT_BOOL_VEC4:
            g->Uniform4iv(u->host_loc, u->count, g->glsl_ibuf);
            break;
        default:                                  /* int, bool, samplers */
            g->Uniform1iv(u->host_loc, u->count, g->glsl_ibuf);
        }
    }
}

/* À l'init : les points d'entrée d'OpenGL 2.0, 16 unités d'image, et un
   programme d'essai (gl_FragCoord compris) passé par la réécriture. */
static bool gl_glsl_probe(GlState *g)
{
    static const char vs[] =
        "varying vec4 c;\nvoid main()\n{\n    c = gl_Color;\n    gl_Position = ftransform();\n}\n";
    static const char fs[] =
        "varying vec4 c;\nuniform sampler2D t;\nvoid main()\n{\n"
        "    gl_FragColor = c * texture2D(t, gl_FragCoord.xy);\n}\n";
    QgpuGlsl q;
    GlGlsl gg;
    char *log = NULL;
    GLint units = 0;
    const char *ver = (const char *)glGetString(GL_VERSION);
    int maj = 0, min = 0;
    bool ok;

    g->CreateShader = gl_proc("glCreateShader");
    g->ShaderSource = gl_proc("glShaderSource");
    g->CompileShader = gl_proc("glCompileShader");
    g->GetShaderiv = gl_proc("glGetShaderiv");
    g->GetShaderInfoLog = gl_proc("glGetShaderInfoLog");
    g->DeleteShader = gl_proc("glDeleteShader");
    g->CreateProgram = gl_proc("glCreateProgram");
    g->AttachShader = gl_proc("glAttachShader");
    g->BindAttribLocation = gl_proc("glBindAttribLocation");
    g->LinkProgram = gl_proc("glLinkProgram");
    g->GetProgramiv = gl_proc("glGetProgramiv");
    g->GetProgramInfoLog = gl_proc("glGetProgramInfoLog");
    g->UseProgram = gl_proc("glUseProgram");
    g->DeleteProgram = gl_proc("glDeleteProgram");
    g->GetUniformLocation = gl_proc("glGetUniformLocation");
    g->Uniform1fv = gl_proc("glUniform1fv");
    g->Uniform2fv = gl_proc("glUniform2fv");
    g->Uniform3fv = gl_proc("glUniform3fv");
    g->Uniform4fv = gl_proc("glUniform4fv");
    g->Uniform1iv = gl_proc("glUniform1iv");
    g->Uniform2iv = gl_proc("glUniform2iv");
    g->Uniform3iv = gl_proc("glUniform3iv");
    g->Uniform4iv = gl_proc("glUniform4iv");
    g->UniformMatrix2fv = gl_proc("glUniformMatrix2fv");
    g->UniformMatrix3fv = gl_proc("glUniformMatrix3fv");
    g->UniformMatrix4fv = gl_proc("glUniformMatrix4fv");
    g->Uniform1f = gl_proc("glUniform1f");
    if (!g->CreateShader || !g->ShaderSource || !g->CompileShader || !g->GetShaderiv ||
        !g->GetShaderInfoLog || !g->DeleteShader || !g->CreateProgram ||
        !g->AttachShader || !g->BindAttribLocation || !g->LinkProgram ||
        !g->GetProgramiv || !g->GetProgramInfoLog || !g->UseProgram ||
        !g->DeleteProgram || !g->GetUniformLocation || !g->Uniform1fv ||
        !g->Uniform2fv || !g->Uniform3fv || !g->Uniform4fv || !g->Uniform1iv ||
        !g->Uniform2iv || !g->Uniform3iv || !g->Uniform4iv || !g->UniformMatrix2fv ||
        !g->UniformMatrix3fv || !g->UniformMatrix4fv || !g->Uniform1f) {
        return false;
    }
    /* les attributs génériques passent par les entrées de la v16 */
    if (!g->has_prog) {
        return false;
    }
    if (!ver || sscanf(ver, "%d.%d", &maj, &min) != 2 || maj < 2) {
        return false;
    }
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
    if (units < QGPU_MAX_IMAGE_UNITS) {
        gl_err_flush();
        return false;
    }
    g->glsl_fbuf = malloc(sizeof(GLfloat) * QGPU_MAX_GLSL_SLOTS * 4);
    g->glsl_ibuf = malloc(sizeof(GLint) * QGPU_MAX_GLSL_SLOTS * 4);
    if (!g->glsl_fbuf || !g->glsl_ibuf) {
        return false;
    }
    memset(&q, 0, sizeof(q));
    memset(&gg, 0, sizeof(gg));
    q.nsrc = 2;
    q.stage[0] = QGPU_GLSL_VERTEX;
    q.src[0] = (char *)vs;
    q.src_len[0] = sizeof(vs) - 1;
    q.stage[1] = QGPU_GLSL_FRAGMENT;
    q.src[1] = (char *)fs;
    q.src_len[1] = sizeof(fs) - 1;
    g->has_glsl = true;                       /* gl_glsl_build le lit */
    ok = gl_glsl_build(g, &q, &gg, &log, false);
    if (ok) {
        ok = gg.fh_loc >= 0;
        g->DeleteProgram(gg.prog);
    } else {
        fprintf(stderr, "qgpu: programme GLSL d'essai refusé :\n%s\n", log ? log : "?");
    }
    free(log);
    g->has_glsl = ok;
    gl_err_flush();
    return ok;
}

static bool gl_init(QgpuCore *c)
{
    const char *why = NULL;
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
        /* G4 : tout ce backend est du pipeline FIXE. On demande donc
           explicitement un profil de COMPATIBILITÉ (EGL 1.5 / KHR_create_context) ;
           sans attributs, le pilote choisit, et un contexte cœur résoudrait
           tous les symboles pour ne rien savoir dessiner. Si la demande est
           refusée, on retombe sur le choix du pilote — et c'est alors
           l'auto-test qui tranche. */
        static const EGLint ctx_compat[] = {
            0x3098 /* EGL_CONTEXT_MAJOR_VERSION */, 3,
            0x30FB /* EGL_CONTEXT_MINOR_VERSION */, 2,
            0x30FD /* EGL_CONTEXT_OPENGL_PROFILE_MASK */,
            0x00000002 /* ..._COMPATIBILITY_PROFILE_BIT */,
            EGL_NONE
        };
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
        g->ctx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, ctx_compat);
        if (g->ctx == EGL_NO_CONTEXT) {
            g->ctx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, NULL);
        }
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
        /* G2 : la cible RECTANGLE ne s'allume et ne s'éteint que si l'hôte
           l'a. Cœur depuis 3.1, extension avant. */
        g->has_rect = (maj > 3 || (maj == 3 && min >= 1)) ||
                      gl_has_ext(g, "GL_ARB_texture_rectangle") ||
                      gl_has_ext(g, "GL_EXT_texture_rectangle") ||
                      gl_has_ext(g, "GL_NV_texture_rectangle");
        /* v16 : programmes ARB, annoncés seulement si un essai compile. */
        g->has_prog = gl_prog_probe(g);
        if (g->has_prog) {
            c->caps |= QGPU_CAP_PROGRAMS;
        }
        /* v21 : GLSL, annoncé seulement si un programme d'essai se lie
           (QGPU_GLSL=0 dans l'environnement : ne pas l'annoncer, A/B). */
        {
            const char *e = getenv("QGPU_GLSL");
            if (!(e && !strcmp(e, "0")) && gl_glsl_probe(g)) {
                c->caps |= QGPU_CAP_GLSL;
            } else {
                g->has_glsl = false;
            }
        }
    }
    /* Mineur : le backend de référence borne toutes ses couleurs à [0,1] ;
       un contexte dont le bornage a été éteint (ARB_color_buffer_float)
       laisserait passer les composantes > 1 jusqu'au tampon. On pose la
       valeur qu'attend un rendu 1.x, une fois. */
    if (g->ClampColor) {
        g->ClampColor(GL_CLAMP_VERTEX_COLOR, GL_TRUE);
        g->ClampColor(GL_CLAMP_FRAGMENT_COLOR, GL_TRUE);
        g->ClampColor(GL_CLAMP_READ_COLOR, GL_FIXED_ONLY);
        gl_err_flush();          /* facultatif : son refus n'est pas une panne */
    }
    /* G3 : le FBO tranche entre les deux jeux d'entrées. */
    if (!gl_probe_fbo(g)) {
        if (!gl_resolve_fbo(g, !g->fbo_ext) || !gl_probe_fbo(g)) {
            fprintf(stderr, "qgpu: backend gl refusé : pas de FBO utilisable "
                    "(%s)\n", g->renderer ? g->renderer : "?");
            goto fail;
        }
    }
    /* G4/G6 : ce qu'on annonce, on le tient — ou on laisse la place au soft. */
    if (!gl_selftest(g, &why)) {
        fprintf(stderr, "qgpu: backend gl refusé par l'auto-test : %s (%s)%s\n",
                why ? why : "?", g->renderer ? g->renderer : "?",
                getenv("QGPU_GL_FORCE") ? " — passé outre (QGPU_GL_FORCE)" : "");
        if (!getenv("QGPU_GL_FORCE")) {
            goto fail;
        }
    }
    if (c->trace) {
        fprintf(stderr, "qgpu: backend gl : %s / %s (fbo %s, rectangle %s)\n",
                g->renderer ? g->renderer : "?",
                (const char *)glGetString(GL_VERSION),
                g->fbo_ext ? "EXT" : "core", g->has_rect ? "oui" : "non");
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
    free(g->glsl_fbuf);
    free(g->glsl_ibuf);
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
    }
    /* G2 : RECTANGLE est une extension à part — sans elle, ce glDisable rend
       GL_INVALID_ENUM quatre fois par commande, et TOUT finit en BACKEND. */
    if (g->has_rect) {
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
        goto fail;
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
    /* G1 : une surface qui n'a pas pu naître (GL_OUT_OF_MEMORY sur un 4096²,
       format refusé…) doit le DIRE. Sans ce test, elle était comptée comme
       créée et c'est la commande suivante, saine, qui héritait de l'erreur. */
    if (!gl_err_ok()) {
        goto fail;
    }
    s->priv = gs;
    return true;

fail:
    g->BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (gs->fbo) {
        g->DeleteFramebuffers(1, &gs->fbo);
    }
    glDeleteTextures(1, &gs->tex);
    if (gs->depth) {
        glDeleteTextures(1, &gs->depth);
    }
    gl_err_flush();
    free(gs);
    return false;
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
    /* v16 : un programme ne survit pas à une commande — seul gl_draw_raw en
       allume, juste avant son dessin. */
    if (g->has_prog) {
        glDisable(GL_VERTEX_PROGRAM_ARB);
        glDisable(GL_FRAGMENT_PROGRAM_ARB);
    }
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
        return gl_err_ok();
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
    /* G1 : l'état est posé ICI pour TOUTE commande ; s'il n'a pas pu l'être,
       le dessin qui suit rendrait une image fausse en silence. */
    return gl_err_ok();
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
    return gl_err_ok();
}

/* ───────────── textures (v3) : objets GL tenus à jour paresseusement ───────────── */

typedef struct GlTexture {
    GLuint id;
    GLenum target;                 /* v10 : cible GL de l'objet */
    /* 27/09 (copie GPU) : géométrie et format de chaque niveau tel que l'objet
       GL le tient (dernier envoi ou dernière copie) ; w = 0 : jamais défini.
       SURF_TEX y voit s'il doit (re)définir le niveau (glCopyTexImage2D) ou
       seulement le recouvrir (glCopyTexSubImage2D). */
    struct { uint32_t w, h, d, fmt; } lv[QGPU_TEX_FACES][QGPU_MAX_TEX_LEVELS];
} GlTexture;

/* G5 : GL_CLAMP (0x2900) est le pincement vers la BORDURE d'OpenGL 1.x — pas
   vers le dernier texel. Le backend de référence l'implémente ainsi
   (wrap_index rend la couleur de bordure hors du niveau), et le mode a
   disparu du profil cœur comme de Metal, où il est au mieux rabattu sur
   CLAMP_TO_EDGE. On l'écrit donc explicitement, pour que les deux backends
   ne puissent pas diverger sur le bord d'une texture. */
static GLint gl_wrap(uint32_t w)
{
    return (GLint)(w == 0x2900 ? (uint32_t)GL_CLAMP_TO_BORDER : w);
}

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
   et les fonctions d'environnement suivent la table d'OpenGL.

   H1 : un niveau de base GL_ALPHA part en GL_ALPHA, comme l'invité l'a
   demandé. Le cœur le dépaquette en argb(a, 0, 0, 0) et ne le promeut plus en
   RGBA ; le format INTERNE doit suivre, sans quoi REPLACE rendrait le texel
   (noir) au lieu de « couleur primaire, alpha de la texture » — la table 3.22
   d'OpenGL, et ce que fait déjà le backend de référence. GL_ALPHA est un
   format interne valide du profil de compatibilité ; les données, elles,
   restent BGRA (le pilote n'en garde que l'alpha). */
static void gl_tex_level(const GlState *g, const QgpuTexture *t, GLenum target,
                         uint32_t face, uint32_t l)
{
    const QgpuTexLevel *lv = &t->level[face][l];
    bool depth = lv->fmt == 0x1902;
    GLint ifmt;
    GLenum fmt, type;
    const void *px = lv->px;
    uint8_t *tmp = NULL;
    uint32_t n, i;

    if (depth) {
        ifmt = GL_DEPTH_COMPONENT24;
        fmt = GL_DEPTH_COMPONENT;
        type = GL_FLOAT;
    } else if (lv->fmt == 0x1909 || lv->fmt == 0x8049 ||
               lv->fmt == 0x190A || lv->fmt == 0x1903) {
        n = lv->w * lv->h * (lv->d ? lv->d : 1);
        tmp = malloc(n * (lv->fmt == 0x190A ? 2 : 1));
        if (tmp && lv->px) {
            const uint32_t *s = lv->px;
            if (lv->fmt == 0x190A) {
                for (i = 0; i < n; i++) {
                    tmp[i * 2] = (uint8_t)(s[i] >> 16);
                    tmp[i * 2 + 1] = (uint8_t)(s[i] >> 24);
                }
            } else {
                for (i = 0; i < n; i++)
                    tmp[i] = (uint8_t)(s[i] >> 16);
            }
            px = tmp;
        }
        ifmt = (GLint)lv->fmt;
        fmt = (GLenum)lv->fmt;
        type = GL_UNSIGNED_BYTE;
        if (lv->fmt == 0x8049 || lv->fmt == 0x1903) {
            ifmt = lv->fmt == 0x8049 ? GL_INTENSITY : GL_LUMINANCE;
            fmt = GL_LUMINANCE;
        }
    } else {
        ifmt = (GLint)lv->fmt;          /* ALPHA, RGB, RGBA : tels quels (H1) */
        fmt = GL_BGRA;
        type = GL_UNSIGNED_INT_8_8_8_8_REV;
    }

    switch (target) {
    case GL_TEXTURE_1D:
        glTexImage1D(target, l, ifmt, lv->w, 0, fmt, type, px);
        break;
    case GL_TEXTURE_3D:
        g->TexImage3D(target, l, ifmt, lv->w, lv->h, lv->d, 0, fmt, type, px);
        break;
    case GL_TEXTURE_CUBE_MAP:
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, l, ifmt, lv->w, lv->h, 0,
                     fmt, type, px);
        break;
    default:                                              /* 2D, RECTANGLE */
        glTexImage2D(target, l, ifmt, lv->w, lv->h, 0, fmt, type, px);
    }
    free(tmp);
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
        glTexParameteri(tg, GL_TEXTURE_WRAP_S, gl_wrap(t->wrap_s));
        glTexParameteri(tg, GL_TEXTURE_WRAP_T, gl_wrap(t->wrap_t));
        if (g->has_tex) {
            uint32_t bc = t->border;
            GLfloat border[4] = { ((bc >> 16) & 255) / 255.0f, ((bc >> 8) & 255) / 255.0f,
                                  (bc & 255) / 255.0f, ((bc >> 24) & 255) / 255.0f };
            glTexParameterfv(tg, GL_TEXTURE_BORDER_COLOR, border);
            glTexParameteri(tg, GL_TEXTURE_WRAP_R, gl_wrap(t->wrap_r));
            if (tg != GL_TEXTURE_RECTANGLE) {
                /* refusés sur un rectangle (GL_INVALID_OPERATION chez NVIDIA),
                   et le cœur ne les accepte pas pour cette cible. Le biais de
                   LOD en fait partie : un rectangle n'a pas de chaîne de
                   niveaux, donc pas de λ à biaiser. */
                glTexParameterf(tg, GL_TEXTURE_LOD_BIAS, t->lod_bias);
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
            const QgpuTexLevel *lv = &t->level[f][l];
            /* 27/09 : un niveau tenu par le GPU (lv->gpu) a son px périmé :
               la texture GL est la vérité, on ne l'écrase pas. */
            if ((t->dirty[f] & (1u << l)) && lv->px && !lv->gpu) {
                gl_tex_level(g, t, gt->target, f, l);
                gt->lv[f][l].w = lv->w;
                gt->lv[f][l].h = lv->h;
                gt->lv[f][l].d = lv->d;
                gt->lv[f][l].fmt = lv->fmt;
            }
        }
        t->dirty[f] = 0;
    }
    return gl_err_ok();
}

/* GL_COMBINE (v5) : état empaqueté → paramètres d'environnement natifs. */
static void gl_combine(QgpuCore *c, const QgpuState *st, int u)
{
    static const GLenum fn[8] = {
        GL_REPLACE, GL_MODULATE, GL_ADD, GL_ADD_SIGNED, GL_INTERPOLATE,
        GL_SUBTRACT, GL_DOT3_RGB, GL_DOT3_RGBA,
    };
    const GlState *gs = c->be_priv;
    /* v12 : 4..7 = GL_TEXTURE0 + n (crossbar, OpenGL 1.4 : gs->has_tex). Le
       champ source fait 3 bits : les unités 4..7 de la v17 n'y tiennent pas. */
    const GLenum srcs[8] = { GL_TEXTURE, GL_CONSTANT, GL_PRIMARY_COLOR, GL_PREVIOUS,
                             gs->has_tex ? GL_TEXTURE0 : GL_TEXTURE,
                             gs->has_tex ? GL_TEXTURE0 + 1 : GL_TEXTURE,
                             gs->has_tex ? GL_TEXTURE0 + 2 : GL_TEXTURE,
                             gs->has_tex ? GL_TEXTURE0 + 3 : GL_TEXTURE };
    static const GLenum ops_rgb[4] = {
        GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
    };
    static const GLenum ops_a[2] = { GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };
    uint32_t cb = st->v[QGPU_SK_COMBINE(u)];
    uint32_t src = st->v[QGPU_SK_COMBINE_SRC(u)];
    int i;

    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, fn[cb & 7]);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, fn[(cb >> 4) & 7]);
    glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, (GLfloat)(1u << ((cb >> 8) & 3)));
    glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, (GLfloat)(1u << ((cb >> 10) & 3)));
    for (i = 0; i < 3; i++) {
        uint32_t f = (src >> (5 * i)) & 31, g = (src >> (15 + 4 * i)) & 15;
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB + i, srcs[f & 7]);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB + i, ops_rgb[(f >> 3) & 3]);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA + i, srcs[g & 7]);
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
                  qgpu_u2f(st->v[QGPU_SK_TEX_LOD_BIAS(u)]));
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode);
    if (mode == GL_COMBINE) {
        gl_combine(c, st, u);
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
        if (c->cur_sec >= 0) {
            /* v11 : somme des couleurs, après l'environnement de texture */
            glEnableClientState(GL_SECONDARY_COLOR_ARRAY);
            g->SecondaryColorPointer(3, GL_FLOAT, stride, verts + c->cur_sec);
            glEnable(GL_COLOR_SUM);
        }
        glDrawArrays(mode[prim], 0, nverts);
        if (c->cur_sec >= 0) {
            glDisable(GL_COLOR_SUM);
            glDisableClientState(GL_SECONDARY_COLOR_ARRAY);
        }
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
    return ok && gl_err_ok();
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
   suivront ne doivent rien voir de l'étage géométrique.
   G1 : rend false si la remise à plat a échoué — sinon l'état resté allumé
   serait DÉJÀ faux pour la commande suivante, qui n'y pourrait rien. */
static bool gl_reset_raw(QgpuCore *c)
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
    if (g->has_glsl && g->glsl_cur) {               /* v21 */
        g->UseProgram(0);
        g->glsl_cur = 0;
    }
    if (g->has_prog) {                              /* v16 */
        glDisable(GL_VERTEX_PROGRAM_ARB);
        glDisable(GL_FRAGMENT_PROGRAM_ARB);
        g->BindProgramARB(GL_VERTEX_PROGRAM_ARB, 0);
        g->BindProgramARB(GL_FRAGMENT_PROGRAM_ARB, 0);
        /* l'attribut 0 EST le tableau de sommets, déjà coupé */
        for (i = 1; i < QGPU_VF_GEN_MAX; i++) {
            g->DisableVertexAttribArrayARB((GLuint)i);
        }
    }
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    return gl_err_ok();
}

/* Lumières, matériaux, plans de découpe et plans œil du texgen sont posés
   AVEC UNE MODÈLE-VUE IDENTITÉ : OpenGL les transforme au moment de l'appel,
   et l'invité nous les a déjà donnés en coordonnées œil. */
static void gl_set_lights(const QgpuGeom *gm, bool all)
{
    int i;

    for (i = 0; i < QGPU_MAX_LIGHTS; i++) {
        const QgpuLight *l = &gm->light[i];
        GLenum id = GL_LIGHT0 + i;
        /* v21 : `all` — un programme GLSL lit gl_LightSource[i] même éteinte */
        if (!l->enabled && !all) {
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
        if (l->enabled) {
            glEnable(id);
        } else {
            glDisable(id);
        }
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

/* ───────── QGPU_RAW_TRACE=<file> : what the raw path really draws ─────────
 *
 * Warcraft III menu labels and grass blades reach this function with sane
 * vertices yet never show up. The probe reports, for small textured strips,
 * the state in force, the matrices OpenGL will actually use, where each vertex
 * lands in window pixels, and what the bound texture holds on the host. */
static FILE *raw_trace_file(void)
{
    static FILE *f;
    static int init;
    if (!init) {
        const char *path = getenv("QGPU_RAW_TRACE");
        init = 1;
        if (path && path[0] == '/' && (f = fopen(path, "w")) != NULL) {
            setvbuf(f, NULL, _IOLBF, 0);
        }
    }
    return f;
}

/* Object space to window pixels, the way OpenGL will do it. Column major. */
static void raw_trace_xform(const GLfloat *mv, const GLfloat *pr, const GLint *vp,
                            const float *v, int nc, float *win)
{
    float o[4], eye[4], clip[4];
    int i, j;
    for (i = 0; i < 4; i++) {
        o[i] = i < nc ? v[i] : (i == 3 ? 1.0f : 0.0f);
    }
    for (i = 0; i < 4; i++) {
        eye[i] = 0.0f;
        for (j = 0; j < 4; j++) {
            eye[i] += mv[j * 4 + i] * o[j];
        }
    }
    for (i = 0; i < 4; i++) {
        clip[i] = 0.0f;
        for (j = 0; j < 4; j++) {
            clip[i] += pr[j * 4 + i] * eye[j];
        }
    }
    win[3] = clip[3];
    if (clip[3] == 0.0f) {
        win[0] = win[1] = win[2] = 0.0f;
        return;
    }
    win[0] = vp[0] + (clip[0] / clip[3] + 1.0f) * 0.5f * vp[2];
    win[1] = vp[1] + (clip[1] / clip[3] + 1.0f) * 0.5f * vp[3];
    win[2] = clip[2] / clip[3];
}

/* Rend false si le sondage a vu une erreur ANTÉRIEURE au dessin : il
   CONSOMME la file d'erreurs (glGetError, glGetTexImage…), donc il doit la
   rendre à l'appelant — sans quoi le verdict d'une commande ne serait pas le
   même avec et sans QGPU_RAW_TRACE. */
static bool raw_trace(QgpuCore *c, const QgpuState *st, const QgpuGeom *gm,
                      QgpuTexture *const *tex, uint32_t mode, uint32_t fmt,
                      const float *verts, uint32_t words, uint32_t count)
{
    static int left = 400;
    static long skip = -1;
    FILE *f = raw_trace_file();
    GLfloat mv[16], pr[16];
    GLint vp[4], sc[4], bound = 0, tw = 0, th = 0;
    int off_c = qgpu_vf_offset(fmt, QGPU_VF_COLOR);
    int off_t = qgpu_vf_offset(fmt, (uint32_t)QGPU_VF_TEX(0));
    GLenum err;
    uint32_t i;

    if (!f || left <= 0 || mode != GL_TRIANGLE_STRIP || count > 6 || !tex[0]) {
        return true;
    }
    if (skip < 0) {                     /* skip the loading screen */
        const char *e = getenv("QGPU_RAW_TRACE_SKIP");
        skip = (e && *e) ? atol(e) : 0;
    }
    if (skip > 0) {
        skip--;
        return true;
    }
    left--;
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    glGetFloatv(GL_PROJECTION_MATRIX, pr);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    fprintf(f, "strip count %u fmt %08x words %u | lighting %u colormat %u"
            " cull %u/%04x front %04x | blend %u alpha %u/%04x/%g depth %u/%04x"
            " | scissor %u [%d %d %d %d] stencil %u | vp %d %d %d %d\n",
            count, fmt, words, st->v[QGPU_SK_LIGHTING], st->v[QGPU_SK_COLOR_MATERIAL],
            st->v[QGPU_SK_CULL_FACE], st->v[QGPU_SK_CULL_MODE], st->v[QGPU_SK_FRONT_FACE],
            st->v[QGPU_SK_BLEND], st->v[QGPU_SK_ALPHA_TEST], st->v[QGPU_SK_ALPHA_FUNC],
            qgpu_u2f(st->v[QGPU_SK_ALPHA_REF]), st->v[QGPU_SK_DEPTH_TEST],
            st->v[QGPU_SK_DEPTH_FUNC], st->v[QGPU_SK_SCISSOR],
            sc[0], sc[1], sc[2], sc[3], st->v[QGPU_SK_STENCIL_TEST],
            vp[0], vp[1], vp[2], vp[3]);
    fprintf(f, "  mat emi %g %g %g %g dif %g %g %g %g | tex id %d %dx%d env %04x\n",
            gm->mat[0].emission[0], gm->mat[0].emission[1], gm->mat[0].emission[2],
            gm->mat[0].emission[3], gm->mat[0].diffuse[0], gm->mat[0].diffuse[1],
            gm->mat[0].diffuse[2], gm->mat[0].diffuse[3], bound, tw, th,
            st->v[QGPU_SK_UNIT(0) + QGPU_SK_U_ENV_MODE]);
    for (i = 0; i < count; i++) {
        const float *v = verts + (size_t)i * words;
        float win[4];
        raw_trace_xform(mv, pr, vp, v, (int)QGPU_VF_POS_COUNT(fmt), win);
        fprintf(f, "  v%u obj %g %g %g %g -> win %.1f %.1f z %.4f w %g",
                i, v[0], v[1], v[2], QGPU_VF_POS_COUNT(fmt) == 4 ? v[3] : 1.0f,
                win[0], win[1], win[2], win[3]);
        if (off_c >= 0) {
            fprintf(f, " | rgba %g %g %g %g", v[off_c], v[off_c + 1],
                    v[off_c + 2], v[off_c + 3]);
        }
        if (off_t >= 0) {
            fprintf(f, " | st %g %g", v[off_t], v[off_t + 1]);
        }
        fprintf(f, "\n");
    }
    /* Is the atlas actually there on the host side? Sample its alpha. */
    if (tw > 0 && th > 0 && tw * th <= 1 << 20) {
        unsigned char *px = malloc((size_t)tw * th * 4);
        if (px) {
            unsigned amin = 255, amax = 0, nz = 0;
            long n = (long)tw * th, k;
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            for (k = 0; k < n; k++) {
                unsigned a = px[k * 4 + 3];
                if (a < amin) amin = a;
                if (a > amax) amax = a;
                if (px[k * 4] | px[k * 4 + 1] | px[k * 4 + 2] | a) nz++;
            }
            fprintf(f, "  atlas alpha %u..%u nonzero %u/%ld\n", amin, amax, nz, n);
            free(px);
        }
    }
    err = glGetError();
    fprintf(f, "  gl error before draw %04x\n", err);
    return err == GL_NO_ERROR;
}

/* v21 : unités d'image et programme GLSL du dessin. Les unités 0..7 ont déjà
   leur texture (gl_unit_env) ; ici, les unités 8..15, puis la texture 0 sur
   toute cible qu'un sampler lit sans que la texture de l'unité soit de cette
   cible (une unité échantillonnée sans texture rend du noir, comme sur une
   carte ; et une liaison laissée par un autre dessin ne doit pas fuir). */
static void gl_glsl_use(QgpuCore *c, QgpuProgram *gp, QgpuTexture *const *tex, float surf_h)
{
    GlState *g = c->be_priv;
    GlGlsl *gg = gp->priv;
    QgpuGlsl *q = gp->glsl;
    int u;

    for (u = 0; u < QGPU_MAX_IMAGE_UNITS; u++) {
        uint8_t m = q->samples[u], have = 0;
        if (u >= QGPU_MAX_UNITS && tex[u]) {
            g->ActiveTexture(GL_TEXTURE0 + u);
            gl_tex_sync(c, tex[u]);                  /* lie la texture */
        }
        if (!m) {
            continue;
        }
        if (tex[u] && tex[u]->priv) {
            switch (((GlTexture *)tex[u]->priv)->target) {
            case GL_TEXTURE_1D: have = QGPU_FPS_1D; break;
            case GL_TEXTURE_2D: have = QGPU_FPS_2D; break;
            case GL_TEXTURE_3D: have = QGPU_FPS_3D; break;
            case GL_TEXTURE_CUBE_MAP: have = QGPU_FPS_CUBE; break;
            default: have = QGPU_FPS_RECT; break;
            }
        }
        m &= (uint8_t)~have;
        if (!m) {
            continue;
        }
        g->ActiveTexture(GL_TEXTURE0 + u);
        if (m & QGPU_FPS_1D) glBindTexture(GL_TEXTURE_1D, 0);
        if (m & QGPU_FPS_2D) glBindTexture(GL_TEXTURE_2D, 0);
        if ((m & QGPU_FPS_3D) && g->has_tex) glBindTexture(GL_TEXTURE_3D, 0);
        if ((m & QGPU_FPS_CUBE) && g->has_tex) glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        if ((m & QGPU_FPS_RECT) && g->has_rect) glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    }
    g->ActiveTexture(GL_TEXTURE0);
    g->UseProgram(gg->prog);
    g->glsl_cur = gg->prog;
    if (gg->fh_loc >= 0) {
        g->Uniform1f(gg->fh_loc, surf_h);
    }
    gl_glsl_uniforms(g, q);
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
    /* v16 : programmes qui agissent sur CE dessin (le cœur a déjà écarté un
       programme cassé) ; `flip_in_proj` = le retournement y reste dans la
       projection (pipeline fixe, ou position invariante). */
    QgpuProgram *vp = qgpu_prog_active(st, c->cur_prg, QGPU_PROG_VP);
    QgpuProgram *fp = qgpu_prog_active(st, c->cur_prg, QGPU_PROG_FP);
    /* v21 : un programme GLSL lié prime sur les programmes ARB */
    QgpuProgram *gp = g->has_glsl ? qgpu_glsl_active(c->cur_prg) : NULL;
    bool flip_in_proj = !vp || ((GlProgram *)vp->priv)->pos_invariant;

    (void)nverts;
    if (!g->has_prog) {
        vp = fp = NULL;
        flip_in_proj = true;
    }
    if (gp) {
        vp = fp = NULL;
        /* sommets GLSL : le main ajouté retourne gl_Position.y ; sans eux,
           le pipeline fixe garde le retournement dans la projection */
        flip_in_proj = !gp->glsl->has_vs;
    }
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
    if (flip_in_proj) {
        glScalef(1.0f, -1.0f, 1.0f);              /* cf. LE RETOURNEMENT */
    }
    glMultMatrixf(gm->mtx[QGPU_MTX_PROJECTION]);

    /* modèle-vue identité pendant qu'on pose ce qu'OpenGL transformerait */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gl_set_lights(gm, gp != NULL);
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
    if ((st->v[QGPU_SK_FOG] || gp) && st->v[QGPU_SK_FOG_MODE] != QGPU_FOG_VERTEX) {
        /* Brouillard calculé par l'hôte : les valeurs d'énumération du mode
           sont celles d'OpenGL, on les repasse telles quelles. (v21 : un
           programme GLSL lit gl_Fog même brouillard éteint.) */
        glFogi(GL_FOG_MODE, (GLint)st->v[QGPU_SK_FOG_MODE]);
        glFogf(GL_FOG_DENSITY, qgpu_u2f(st->v[QGPU_SK_FOG_DENSITY]));
        glFogf(GL_FOG_START, qgpu_u2f(st->v[QGPU_SK_FOG_START]));
        glFogf(GL_FOG_END, qgpu_u2f(st->v[QGPU_SK_FOG_END]));
        glFogi(GL_FOG_COORDINATE_SOURCE,
               off_f >= 0 ? GL_FOG_COORDINATE : GL_FRAGMENT_DEPTH);
    }

    /* Tableaux de sommets : un attribut absent devient une valeur courante. */
    glEnableClientState(GL_VERTEX_ARRAY);
    {
        /* v16 : QGPU_VF_GEN(0) EST la position (aliasing ARB). On la donne
           par glVertexPointer plutôt que par glVertexAttribPointerARB(0) :
           l'hôte Apple ne fait pas gagner le dernier posé des deux. */
        int off_g0 = qgpu_vf_offset(fmt, (uint32_t)QGPU_VF_GEN(0));
        if (off_g0 >= 0 && g->has_prog) {
            glVertexPointer(4, GL_FLOAT, stride, verts + off_g0);
        } else {
            glVertexPointer(QGPU_VF_POS_COUNT(fmt), GL_FLOAT, stride, verts);
        }
    }
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
            /* v21 : sous GLSL, gl_TextureMatrix[u] et gl_MultiTexCoord<u>
               existent sans texture (DarkPlaces y passe ses tangentes) */
            if (!gp) {
                continue;
            }
            g->ActiveTexture(GL_TEXTURE0 + u);
            g->ClientActiveTexture(GL_TEXTURE0 + u);
        } else {
            ok = gl_unit_env(c, st, u, tex[u]);
        }
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
    /* v16 : attributs génériques, puis les programmes — en dernier, pour que
       l'état posé au-dessus (unités, matrices) soit celui qu'ils voient. */
    if (ok && g->has_prog) {
        int k;
        for (k = 1; k < QGPU_VF_GEN_MAX; k++) {      /* 0 : glVertexPointer, ci-dessus */
            int off_g = qgpu_vf_offset(fmt, (uint32_t)QGPU_VF_GEN(k));
            if (off_g >= 0) {
                g->EnableVertexAttribArrayARB((GLuint)k);
                g->VertexAttribPointerARB((GLuint)k, 4, GL_FLOAT, GL_FALSE, stride,
                                          verts + off_g);
            } else {
                g->DisableVertexAttribArrayARB((GLuint)k);
            }
        }
        if (gp) {
            gl_glsl_use(c, gp, tex, (float)s->height);
        }
        if (vp) {
            gl_prog_use(g, c->cur_prg, QGPU_PROG_VP, vp, (float)s->height);
        }
        if (fp) {
            /* 27/09 : une unité que le programme échantillonne mais qui n'a pas
               de texture rend la texture 0 (incomplète : noir), comme sur une
               vraie carte — et non la dernière liée par un autre dessin (le
               reflet de la carrosserie de Colin McRae changeait d'un dessin à
               l'autre, et le rejeu différait de la capture). */
            for (u = 0; u < QGPU_MAX_UNITS; u++) {
                uint8_t m = fp->fp_samples[u];
                if (!m || tex[u]) {
                    continue;
                }
                g->ActiveTexture(GL_TEXTURE0 + u);
                if (m & QGPU_FPS_1D) glBindTexture(GL_TEXTURE_1D, 0);
                if (m & QGPU_FPS_2D) glBindTexture(GL_TEXTURE_2D, 0);
                if ((m & QGPU_FPS_3D) && g->has_tex) glBindTexture(GL_TEXTURE_3D, 0);
                if ((m & QGPU_FPS_CUBE) && g->has_tex) glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
                if ((m & QGPU_FPS_RECT) && g->has_rect) glBindTexture(GL_TEXTURE_RECTANGLE, 0);
            }
            gl_prog_use(g, c->cur_prg, QGPU_PROG_FP, fp, (float)s->height);
        }
        ok = gl_err_ok();
    }
    if (ok) {
        g->ClientActiveTexture(GL_TEXTURE0);
        ok = raw_trace(c, st, gm, tex, mode, fmt, verts, words, count);
        if (idx) {
            glDrawElements(mode, (GLsizei)count, GL_UNSIGNED_INT, idx);
        } else {
            glDrawArrays(mode, (GLint)first, (GLsizei)count);
        }
        /* G1 : le verdict du DESSIN se prend ici, avant la remise à plat —
           les deux ont leurs propres raisons d'échouer. */
        ok = gl_err_ok() && ok;
    }
    if (!gl_reset_raw(c)) {
        ok = false;
    }
    g->ActiveTexture(GL_TEXTURE0);
    g->ClientActiveTexture(GL_TEXTURE0);
    return ok;
}

/* ── 27/09 : copies surface → texture sur le GPU de l'hôte ─────────────────
 *
 * Jusqu'ici SURF_TEX et COPY_TEX passaient par glReadPixels (la relecture
 * attend la fin du rendu de la surface), une copie dans le niveau du cœur,
 * puis un glTexImage2D du niveau entier au dessin suivant : deux traversées de
 * la mémoire de l'hôte et une synchronisation CPU/GPU par copie. Ici tout
 * reste dans le GPU :
 *
 *   - SURF_TEX (ligne 0 = haut) : le FBO de la surface a sa ligne 0 de
 *     SURFACE en ligne 0 GL, et un niveau envoyé par gl_tex_level a sa ligne 0
 *     de px en ligne 0 GL : glCopyTexSubImage2D copie donc sans retournement,
 *     exactement l'image que la relecture aurait rangée dans px ;
 *     glCopyTexImage2D quand le niveau change de taille ou de format ;
 *   - COPY_TEX (orientation OpenGL : la ligne sy + h − 1 en y) : un blit
 *     retourné du FBO de la surface vers un FBO intermédiaire (RGBA8, même
 *     taille, NEAREST : texels identiques), puis glCopyTexSubImage* de
 *     celui-ci vers le niveau — ce qui couvre toutes les cibles (1D, 2D,
 *     rectangle, faces de cube, tranches 3D) et tous les formats (la copie
 *     réduit RGBA au format du niveau comme l'envoi le faisait : L = R,
 *     A = A…). Sans blit : une ligne par glCopyTexSubImage2D.
 *
 * Rien ici ne dépend des tests ni des masques : gl_target(…, NULL) a coupé le
 * ciseau (le seul qui touche un blit) ; glCopyTex* les ignore. */
static bool gl_flip_fbo(GlState *g, uint32_t w, uint32_t h)
{
    if (g->flip_fbo && g->flip_w >= w && g->flip_h >= h) {
        return true;
    }
    if (g->flip_fbo) {
        g->DeleteFramebuffers(1, &g->flip_fbo);
        glDeleteTextures(1, &g->flip_tex);
        g->flip_fbo = g->flip_tex = 0;
    }
    w = w > g->flip_w ? w : g->flip_w;
    h = h > g->flip_h ? h : g->flip_h;
    glGenTextures(1, &g->flip_tex);
    glBindTexture(GL_TEXTURE_2D, g->flip_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    g->GenFramebuffers(1, &g->flip_fbo);
    g->BindFramebuffer(GL_FRAMEBUFFER, g->flip_fbo);
    g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            g->flip_tex, 0);
    if (g->CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        !gl_err_ok()) {
        g->DeleteFramebuffers(1, &g->flip_fbo);
        glDeleteTextures(1, &g->flip_tex);
        g->flip_fbo = g->flip_tex = 0;
        g->flip_w = g->flip_h = 0;
        gl_err_flush();
        return false;
    }
    g->flip_w = w;
    g->flip_h = h;
    return true;
}

/* Copie du rectangle (rx, ry, w, h) du framebuffer LU vers le niveau lié. */
static void gl_copy_sub(GlState *g, GLenum tg, GLenum itg, uint32_t lvl, uint32_t x,
                        uint32_t y, uint32_t z, GLint rx, GLint ry, uint32_t w, uint32_t h)
{
    switch (tg) {
    case GL_TEXTURE_1D:
        glCopyTexSubImage1D(tg, lvl, x, rx, ry, w);
        break;
    case GL_TEXTURE_3D:
        g->CopyTexSubImage3D(tg, lvl, x, y, z, rx, ry, w, h);
        break;
    default:                                   /* 2D, rectangle, face de cube */
        glCopyTexSubImage2D(itg, lvl, x, y, rx, ry, w, h);
    }
}

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif

static bool gl_tex_copy(QgpuCore *c, QgpuSurface *s, QgpuTexture *t, uint32_t face,
                        uint32_t lvl, uint32_t x, uint32_t y, uint32_t z,
                        uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, bool flip)
{
    GlState *g = c->be_priv;
    GlSurface *gs = s->priv;
    const QgpuTexLevel *lv = &t->level[face][lvl];
    GlTexture *gt;
    GLenum tg, itg;
    bool same;

    if (!gl_target(c, s, NULL)) {              /* contexte, FBO de la surface lié */
        return false;
    }
    g->ActiveTexture(GL_TEXTURE0);
    if (!gl_tex_sync(c, t)) {                  /* crée, envoie ce que le cœur a de neuf, lie */
        return false;
    }
    gt = t->priv;
    tg = gt->target;
    itg = tg == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : tg;
    if ((tg == GL_TEXTURE_3D && !g->CopyTexSubImage3D) || lv->fmt == 0x1902) {
        return false;
    }
    same = gt->lv[face][lvl].w == lv->w && gt->lv[face][lvl].h == lv->h &&
           gt->lv[face][lvl].d == lv->d && gt->lv[face][lvl].fmt == lv->fmt;
    if (!flip || h == 1) {
        if (same) {
            gl_copy_sub(g, tg, itg, lvl, x, y, z, sx, sy, w, h);
        } else if ((tg == GL_TEXTURE_2D || tg == GL_TEXTURE_RECTANGLE) &&
                   x == 0 && y == 0 && w == lv->w && h == lv->h) {
            /* SURF_TEX : niveau (re)défini à la taille de la surface */
            glCopyTexImage2D(itg, lvl, (GLint)lv->fmt, sx, sy, w, h, 0);
        } else {
            return false;
        }
    } else if (!same) {
        return false;                          /* COPY_TEX vise un niveau déjà envoyé */
    } else if (g->BlitFramebuffer && gl_flip_fbo(g, w, h)) {
        g->BindFramebuffer(GL_READ_FRAMEBUFFER, gs->fbo);
        g->BindFramebuffer(GL_DRAW_FRAMEBUFFER, g->flip_fbo);
        g->BlitFramebuffer(sx, sy, sx + w, sy + h, 0, h, w, 0,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
        g->BindFramebuffer(GL_FRAMEBUFFER, g->flip_fbo);
        glBindTexture(tg, gt->id);             /* gl_flip_fbo a pu lier la sienne */
        gl_copy_sub(g, tg, itg, lvl, x, y, z, 0, 0, w, h);
        g->BindFramebuffer(GL_FRAMEBUFFER, gs->fbo);
    } else {
        uint32_t r;
        g->BindFramebuffer(GL_FRAMEBUFFER, gs->fbo);
        glBindTexture(tg, gt->id);
        for (r = 0; r < h; r++) {
            gl_copy_sub(g, tg, itg, lvl, x, y + r, z, sx, sy + h - 1 - r, w, 1);
        }
    }
    if (!gl_err_ok()) {
        return false;
    }
    gt->lv[face][lvl].w = lv->w;
    gt->lv[face][lvl].h = lv->h;
    gt->lv[face][lvl].d = lv->d;
    gt->lv[face][lvl].fmt = lv->fmt;
    return true;
}

/* Rapatrie un niveau tenu par le GPU dans son px (cf. tex_cpu du cœur). */
static bool gl_tex_fetch(QgpuCore *c, QgpuTexture *t, uint32_t face, uint32_t lvl)
{
    GlState *g = c->be_priv;
    GlTexture *gt = t->priv;
    QgpuTexLevel *lv = &t->level[face][lvl];
    GLenum itg;

    if (!gt || !lv->px || lv->fmt == 0x1902 || !gl_make_current(g)) {
        return false;
    }
    itg = gt->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face
                                            : gt->target;
    g->ActiveTexture(GL_TEXTURE0);
    glBindTexture(gt->target, gt->id);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glGetTexImage(itg, lvl, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, lv->px);
    return gl_err_ok();
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
    return gl_err_ok();
}

static bool gl_depth_readback(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h, float *dst)
{
    if (!gl_target(c, s, NULL)) {
        return false;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x, y, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, dst);
    return gl_err_ok();
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
    return gl_err_ok();
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
    return gl_err_ok();
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
    return gl_err_ok();
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
    return gl_err_ok();
}

static bool gl_query_end(QgpuCore *c, QgpuQuery *q)
{
    GlState *g = c->be_priv;

    if (!g->has_query || !q->priv || !gl_make_current(g)) {
        return false;
    }
    g->EndQuery(GL_SAMPLES_PASSED);
    return gl_err_ok();
}

static bool gl_query_result(QgpuCore *c, QgpuQuery *q)
{
    GlState *g = c->be_priv;
    GlQuery *gq = q->priv;
    GLuint n = 0;

    if (!g->has_query || !gq || !gl_make_current(g)) {
        return false;
    }
    /* Mineur : un compte d'échantillons déborde 32 bits dès 4 milliards de
       fragments (quelques secondes sur un GPU moderne), et `samples` est un
       64 bits. On prend l'entrée 64 bits quand l'hôte l'a (GL 3.3 /
       EXT_timer_query), la 32 bits sinon. */
    if (g->GetQueryObjectui64v) {
        uint64_t n64 = 0;
        g->GetQueryObjectui64v(gq->id, GL_QUERY_RESULT, &n64);
        q->samples = n64;
    } else {
        g->GetQueryObjectuiv(gq->id, GL_QUERY_RESULT, &n);
        q->samples = n;
    }
    return gl_err_ok();
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
    .prog_string    = gl_prog_string,      /* v16 */
    .prog_destroy   = gl_prog_destroy,
    .glsl_link      = gl_glsl_link,        /* v21 */
    .tex_copy       = gl_tex_copy,         /* 27/09 : copie GPU */
    .tex_fetch      = gl_tex_fetch,
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
