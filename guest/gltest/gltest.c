/*
 * gltest.c — programme OpenGL de Tiger, hors écran (CGL), sans WindowServer.
 *
 *   gltest [scène] [largeur hauteur] [sortie.ppm]
 *     scène : tri (triangle rouge sur fond bleu, pixels témoins vérifiés)
 *             gouraud, depth (deux triangles qui se croisent), spin (N images, FPS)
 *
 * Sans WindowServer (single-user), CGL compte zéro écran et refuse tout pixel
 * format, SAUF si la liste contient kCGLPFARemotePBuffer (91) : ce drapeau
 * interdit la connexion au WindowServer (lu dans cglConvertAttribs de 10.4.6).
 * Le programme l'ajoute quand GLTEST_NOWS est défini.
 *
 * Affiche le renderer choisi (GL_RENDERER) et la liste des renderers connus
 * de CGL : c'est ce qui prouve quel plugin GL a réellement rendu. Écrit l'image
 * dans un PPM. Code de sortie 0 si les pixels témoins sont bons.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glu.h>
#include <math.h>
#include <dlfcn.h>

/* Entrées ARB/EXT utilisées par la scène « tclprobe » : libGL.dylib de 10.4.6
   les exporte toutes (relevé dans OpenGL.framework/Libraries/libGL.dylib) ;
   on les déclare ici pour ne dépendre d'aucune version de <OpenGL/glext.h>. */
#ifndef GL_TEXTURE0_ARB
#define GL_TEXTURE0_ARB 0x84C0
#endif
#ifndef GL_TEXTURE1_ARB
#define GL_TEXTURE1_ARB 0x84C1
#endif
#ifndef GL_TEXTURE2_ARB
#define GL_TEXTURE2_ARB 0x84C2
#endif
#ifndef GL_TEXTURE3_ARB
#define GL_TEXTURE3_ARB 0x84C3
#endif
extern void glMultiTexCoord2fARB(GLenum, GLfloat, GLfloat);
extern void glClientActiveTextureARB(GLenum);
extern void glSecondaryColor3fvEXT(const GLfloat *);
extern void glSecondaryColorPointerEXT(GLint, GLenum, GLsizei, const GLvoid *);
extern void glFogCoordfEXT(GLfloat);
extern void glFogCoordPointerEXT(GLenum, GLsizei, const GLvoid *);

/* Constantes et entrées des sondes d'état T&L (docs/re/etat-tcl.md). */
#ifndef GL_RESCALE_NORMAL
#define GL_RESCALE_NORMAL 0x803A
#endif
#ifndef GL_LIGHT_MODEL_COLOR_CONTROL
#define GL_LIGHT_MODEL_COLOR_CONTROL 0x81F8
#endif
#ifndef GL_SINGLE_COLOR
#define GL_SINGLE_COLOR 0x81F9
#endif
#ifndef GL_SEPARATE_SPECULAR_COLOR
#define GL_SEPARATE_SPECULAR_COLOR 0x81FA
#endif
#ifndef GL_FOG_COORDINATE_SOURCE_EXT
#define GL_FOG_COORDINATE_SOURCE_EXT 0x8450
#define GL_FOG_COORDINATE_EXT        0x8451
#define GL_FRAGMENT_DEPTH_EXT        0x8452
#endif
#ifndef GL_POINT_SIZE_MIN_ARB
#define GL_POINT_SIZE_MIN_ARB              0x8126
#define GL_POINT_SIZE_MAX_ARB              0x8127
#define GL_POINT_FADE_THRESHOLD_SIZE_ARB   0x8128
#define GL_POINT_DISTANCE_ATTENUATION_ARB  0x8129
#endif
#ifndef GL_NORMAL_MAP_ARB
#define GL_NORMAL_MAP_ARB     0x8511
#define GL_REFLECTION_MAP_ARB 0x8512
#endif
/* glPointParameter* : résolu par dlsym pour ne dépendre d'aucun nom de symbole
   (ARB ou EXT) à l'édition de liens. */
typedef void (*pp_f)(GLenum, GLfloat);
typedef void (*pp_fv)(GLenum, const GLfloat *);
static void *gl_sym(const char *a, const char *b)
{
    void *p = dlsym(RTLD_DEFAULT, a);
    return p ? p : dlsym(RTLD_DEFAULT, b);
}

/* Une étape de sonde = un réglage GL suivi d'un glClear ; avec
   POMPPC_GLTRACE_STATE=1 le traceur vide l'état GL à chaque effacement, et la
   ligne imprimée ici nomme l'appel qui a produit le vidage de même rang
   (fichier d'étiquettes de tools/re/diffstate.py). */
static int probe_step;
static void pstep(const char *what)
{
    glClear(GL_COLOR_BUFFER_BIT);
    printf("ÉTAPE %2d %s\n", ++probe_step, what);
}

static int W = 64, H = 64;
static int ROWB;                        /* octets par ligne (GLTEST_ROWPAD en plus de 4·W) */
static unsigned char *buf;

static unsigned long px(int x, int y)   /* 0x00RRGGBB, y depuis le HAUT */
{
    unsigned char *p = buf + y * ROWB + x * 4;
    return ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}

static int failures;
static void check(const char *what, int x, int y, unsigned long want)
{
    unsigned long got = px(x, y);
    int ok = (got == want);
    printf("  %s %-28s (%3d,%3d) = %06lx%s\n", ok ? "ok  " : "FAIL", what, x, y, got,
           ok ? "" : " (attendu autre)");
    if (!ok) failures++;
}

/* Comparaison de deux PPM : écart maximal par composante, et où. C'est la
   preuve « image entière » demandée à côté des pixels témoins ; elle se fait
   DANS l'invité, sur les deux PPM produits par le même programme. */
static int ppm_diff(const char *fa, const char *fb)
{
    FILE *a = fopen(fa, "rb"), *b = fopen(fb, "rb");
    int wa, ha, wb, hb, ma = 0, mb2 = 0, n2 = 0, n8 = 0, i, worst = -1;
    unsigned char *pa, *pb;
    long np;
    if (!a || !b) { printf("diff : fichier illisible\n"); return 2; }
    if (fscanf(a, "P6 %d %d %d", &wa, &ha, &ma) != 3 ||
        fscanf(b, "P6 %d %d %d", &wb, &hb, &mb2) != 3) {
        printf("diff : en-tête PPM invalide\n"); return 2;
    }
    fgetc(a); fgetc(b);
    if (wa != wb || ha != hb) { printf("diff : tailles différentes\n"); return 2; }
    np = (long)wa * ha * 3;
    pa = malloc(np); pb = malloc(np);
    if (fread(pa, 1, np, a) != (size_t)np || fread(pb, 1, np, b) != (size_t)np) {
        printf("diff : lecture courte\n"); return 2;
    }
    fclose(a); fclose(b);
    ma = 0;
    for (i = 0; i < np; i++) {
        int d = pa[i] - pb[i];
        if (d < 0) d = -d;
        if (d > ma) { ma = d; worst = i; }
        if (d > 2) n2++;
        if (d > 8) n8++;
    }
    {
        /* Écart maximal HORS ARÊTES : un pixel dont le voisinage 3×3 est
           uniforme dans l'image de référence n'est sur aucune silhouette ; s'il
           diffère, ce n'est pas une question de règle de remplissage. C'est là
           que se juge la tolérance de 2/255. */
        int x, y, k, mf = 0, nf = 0, fx = -1, fy = -1;
        for (y = 1; y < ha - 1; y++) for (x = 1; x < wa - 1; x++) {
            int uniform = 1, u, v;
            const unsigned char *c0 = pa + (y * wa + x) * 3;
            for (v = -1; v <= 1 && uniform; v++) for (u = -1; u <= 1; u++) {
                const unsigned char *c1 = pa + ((y + v) * wa + x + u) * 3;
                if (c1[0] != c0[0] || c1[1] != c0[1] || c1[2] != c0[2]) { uniform = 0; break; }
            }
            if (!uniform) continue;
            for (k = 0; k < 3; k++) {
                int d = pa[(y * wa + x) * 3 + k] - pb[(y * wa + x) * 3 + k];
                if (d < 0) d = -d;
                if (d > 2) nf++;
                if (d > mf) { mf = d; fx = x; fy = y; }
            }
        }
        printf("     hors arêtes (voisinage 3x3 uniforme) : écart max %d/255 "
               "(en %d,%d), %d composantes > 2\n", mf, fx, fy, nf);
    }
    printf("diff %s vs %s : %dx%d, écart max %d/255 (en %d,%d), "
           "%d composantes > 2 (%.3f %%), %d > 8 (%.3f %%)\n",
           fa, fb, wa, ha, ma, worst >= 0 ? (worst / 3) % wa : -1,
           worst >= 0 ? (worst / 3) / wa : -1,
           n2, 100.0 * n2 / np, n8, 100.0 * n8 / np);
    if (ma > 2) {
        /* carte des écarts par tuile de 32×32 : elle dit tout de suite si la
           différence est une arête isolée ou toute une zone */
        int tx, ty;
        for (ty = 0; ty < ha; ty += 32) {
            printf("      ");
            for (tx = 0; tx < wa; tx += 32) {
                int u, v, mm = 0;
                for (v = ty; v < ty + 32 && v < ha; v++)
                    for (u = tx; u < tx + 32 && u < wa; u++) {
                        int k;
                        for (k = 0; k < 3; k++) {
                            int d = pa[(v * wa + u) * 3 + k] - pb[(v * wa + u) * 3 + k];
                            if (d < 0) d = -d;
                            if (d > mm) mm = d;
                        }
                    }
                printf("%4d", mm);
            }
            printf("\n");
        }
    }
    free(pa); free(pb);
    return 0;
}

static void list_renderers(void)
{
    CGLRendererInfoObj ri;
    long n = 0, i, v;
    if (CGLQueryRendererInfo(0xFFFFFFFF, &ri, &n) != kCGLNoError) {
        printf("CGLQueryRendererInfo: échec\n");
        return;
    }
    printf("renderers CGL : %ld\n", n);
    for (i = 0; i < n; i++) {
        long acc = 0, off = 0, mask = 0;
        CGLDescribeRenderer(ri, i, kCGLRPRendererID, &v);
        CGLDescribeRenderer(ri, i, kCGLRPAccelerated, &acc);
        CGLDescribeRenderer(ri, i, kCGLRPOffScreen, &off);
        CGLDescribeRenderer(ri, i, kCGLRPDisplayMask, &mask);
        printf("  [%ld] id=0x%08lx accel=%ld offscreen=%ld mask=0x%lx\n", i, v, acc, off, mask);
    }
    CGLDestroyRendererInfo(ri);
}

static double now(void)
{
    struct timeval tv; gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Six couleurs unies, en OCTETS : l'aller-retour 8 bits → flottant → 8 bits est
   exact, donc les pixels témoins de la scène « fusion » le sont aussi. */
static const unsigned char fus_col[6][3] = {
    { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 },
    { 255, 255, 0 }, { 255, 0, 255 }, { 0, 255, 255 }
};
#define FCOL(k) fus_col[k][0], fus_col[k][1], fus_col[k][2]

int main(int argc, char **argv)
{
    const char *scene = argc > 1 ? argv[1] : "tri";
    if (argc > 3 && !strcmp(scene, "diff"))
        return ppm_diff(argv[2], argv[3]);
    {
    const char *out = argc > 4 ? argv[4] : "gltest.ppm";
    CGLPixelFormatAttribute attrs[24];
    CGLPixelFormatObj pix;
    CGLContextObj ctx;
    long npix = 0, rid = 0;
    int k = 0, y;
    CGLError e;

    if (argc > 3) { W = atoi(argv[2]); H = atoi(argv[3]); }
    list_renderers();

    if (getenv("GLTEST_NOWS"))
        attrs[k++] = kCGLPFARemotePBuffer;
    if (getenv("GLTEST_ACCEL"))
        attrs[k++] = kCGLPFAAccelerated;
    attrs[k++] = kCGLPFAOffScreen;
    attrs[k++] = kCGLPFAColorSize; attrs[k++] = 32;
    attrs[k++] = kCGLPFADepthSize; attrs[k++] = 16;
    if (getenv("GLTEST_STENCIL")) {         /* tampon de stencil de 8 bits */
        attrs[k++] = kCGLPFAStencilSize; attrs[k++] = 8;
    }
    if (getenv("GLTEST_RENDERER")) {
        attrs[k++] = kCGLPFARendererID;
        attrs[k++] = strtoul(getenv("GLTEST_RENDERER"), 0, 0);
    }
    attrs[k] = 0;
    e = CGLChoosePixelFormat(attrs, &pix, &npix);
    printf("CGLChoosePixelFormat: err=%d pix=%p n=%ld\n", e, (void *)pix, npix);
    if (e || !pix) return 2;
    CGLDescribePixelFormat(pix, 0, kCGLPFARendererID, &rid);
    printf("renderer du pixel format : 0x%08lx\n", rid);
    e = CGLCreateContext(pix, 0, &ctx);
    printf("CGLCreateContext: err=%d ctx=%p\n", e, (void *)ctx);
    CGLDestroyPixelFormat(pix);
    if (e || !ctx) return 3;
    ROWB = W * 4 + (getenv("GLTEST_ROWPAD") ? atoi(getenv("GLTEST_ROWPAD")) : 0);
    buf = calloc(ROWB * H, 1);
    e = CGLSetOffScreen(ctx, W, H, ROWB, buf);
    printf("CGLSetOffScreen: err=%d\n", e);
    CGLSetCurrentContext(ctx);
    printf("GL_VENDOR   = %s\n", glGetString(GL_VENDOR));
    printf("GL_RENDERER = %s\n", glGetString(GL_RENDERER));
    printf("GL_VERSION  = %s\n", glGetString(GL_VERSION));
    {
        /* Sonde V1 de docs/re/capacites-glengine.md §9 : _gliGetInteger rend
           ctx+0x7580 pour 310 (kCGLCPGPUVertexProcessing) et ctx+0x7581 pour
           311 (kCGLCPGPUFragmentProcessing) — c'est-à-dire l'octet +0x79 du
           bloc de configuration. La sonde la moins chère du verrou T&L. */
        long v310 = -1, v311 = -1;
        CGLError e310 = CGLGetParameter(ctx, (CGLContextParameter)310, &v310);
        CGLError e311 = CGLGetParameter(ctx, (CGLContextParameter)311, &v311);
        printf("CGLGetParameter 310 (GPUVertexProcessing)   = %ld (err %d)\n", v310, e310);
        printf("CGLGetParameter 311 (GPUFragmentProcessing) = %ld (err %d)\n", v311, e311);
    }

    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, H, 0, -1, 1);           /* y vers le bas, comme le protocole qgpu */
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    if (!strcmp(scene, "tri") || !strcmp(scene, "gouraud")) {
        float s = W / 64.0f;
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        if (!strcmp(scene, "tri")) {
            glColor3f(1, 0, 0);
            glVertex2f(4 * s, 4 * s); glVertex2f(60 * s, 4 * s); glVertex2f(4 * s, 60 * s);
        } else {
            glColor3f(1, 0, 0); glVertex2f(0, 0);
            glColor3f(0, 1, 0); glVertex2f(64 * s, 0);
            glColor3f(1, 0, 0); glVertex2f(0, 64 * s);
        }
        glEnd();
        glFinish();
        if (!strcmp(scene, "tri")) {
            check("intérieur triangle rouge", (int)(8 * s), (int)(8 * s), 0xFF0000);
            check("fond bleu", (int)(60 * s), (int)(60 * s), 0x0000FF);
            check("coin bleu", (int)(2 * s), (int)(2 * s), 0x0000FF);
        } else {
            unsigned long p = px((int)(31 * s), (int)(1 * s));
            int r = p >> 16, g = (p >> 8) & 255;
            printf("  %s Gouraud milieu arête = %06lx\n",
                   (r > 100 && r < 156 && g > 100 && g < 156) ? "ok  " : "FAIL", p);
            if (!(r > 100 && r < 156 && g > 100 && g < 156)) failures++;
        }
    } else if (!strcmp(scene, "depth")) {
        glEnable(GL_DEPTH_TEST);
        glClearColor(0, 0, 0, 1);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        /* rouge : z de -0.5 (gauche, devant) à +0.5 (droite, derrière) */
        glColor3f(1, 0, 0);
        glVertex3f(0, 8, 0.5f); glVertex3f(W, 8, -0.5f); glVertex3f(W / 2, H - 8, 0.0f);
        /* vert : l'inverse */
        glColor3f(0, 1, 0);
        glVertex3f(0, 8, -0.5f); glVertex3f(W, 8, 0.5f); glVertex3f(W / 2, H - 8, 0.0f);
        glEnd();
        glFinish();
        /* glOrtho(near=-1, far=1) donne z_ndc = −z : le plus GRAND z objet est
           le plus proche. À gauche le rouge (z = +0.5) est devant. */
        check("gauche : rouge devant", W / 8 + 2, 12, 0xFF0000);
        check("droite : vert devant", W - W / 8 - 2, 12, 0x00FF00);
        check("fond noir", 1, H - 2, 0x000000);
    } else if (!strcmp(scene, "prims")) {
        /* Une primitive de chaque sorte, couleurs distinctes, pour la trace :
           culling (arrière), bande, éventail, quads, bande de quads, polygone.
           Chaque primitive tient dans sa propre case de 16×16. */
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_CULL_FACE);            /* GL_BACK ; glOrtho y inversé : */
        glBegin(GL_TRIANGLES);             /* à l'écran, anti-horaire = face avant */
        glColor3f(1, 0, 0); glVertex2f(1, 1); glVertex2f(1, 15); glVertex2f(15, 1);   /* avant */
        glColor3f(0, 1, 0); glVertex2f(17, 1); glVertex2f(31, 1); glVertex2f(17, 15); /* arrière */
        glEnd();
        glDisable(GL_CULL_FACE);
        glBegin(GL_TRIANGLE_STRIP);
        glColor3f(0, 0, 1); glVertex2f(33, 1); glVertex2f(33, 15); glVertex2f(47, 1); glVertex2f(47, 15);
        glEnd();
        glBegin(GL_TRIANGLE_FAN);
        glColor3f(1, 1, 0); glVertex2f(49, 1); glVertex2f(63, 1); glVertex2f(63, 15); glVertex2f(49, 15);
        glEnd();
        glBegin(GL_QUADS);
        glColor3f(1, 0, 1); glVertex2f(1, 17); glVertex2f(15, 17); glVertex2f(15, 31); glVertex2f(1, 31);
        glEnd();
        glBegin(GL_QUAD_STRIP);
        glColor3f(0, 1, 1); glVertex2f(17, 17); glVertex2f(17, 31); glVertex2f(24, 17); glVertex2f(24, 31);
        glVertex2f(31, 17); glVertex2f(31, 31);
        glEnd();
        glBegin(GL_POLYGON);
        glColor3f(1, 1, 1); glVertex2f(33, 17); glVertex2f(47, 17); glVertex2f(47, 31); glVertex2f(40, 34); glVertex2f(33, 31);
        glEnd();
        glShadeModel(GL_FLAT);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex2f(49, 17); glColor3f(0, 1, 0); glVertex2f(49, 31);
        glColor3f(0, 0, 1); glVertex2f(63, 17);
        glEnd();
        glShadeModel(GL_SMOOTH);
        glFinish();
        check("triangle avant visible", 4, 4, 0xFF0000);
        check("triangle arrière éliminé", 20, 4, 0x000000);
        check("bande", 40, 8, 0x0000FF);
        check("éventail", 56, 8, 0xFFFF00);
        check("quad", 8, 24, 0xFF00FF);
        check("bande de quads", 24, 24, 0x00FFFF);
        check("polygone", 40, 24, 0xFFFFFF);
        check("ombrage plat = dernier sommet", 52, 20, 0x0000FF);
    } else if (!strcmp(scene, "tex") || !strcmp(scene, "texpersp") || !strcmp(scene, "texfmt")) {
        /* Damier 64×64 (cases de 8), mipmaps construits par GLU. */
        unsigned char img[64 * 64 * 4], lum[64 * 64], la[64 * 64 * 2];
        int x, y, f;
        GLuint id;
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++) {
                int on = ((x / 8) + (y / 8)) & 1;
                unsigned char *q = img + (y * 64 + x) * 4;
                q[0] = on ? 255 : 30; q[1] = on ? 255 : 60; q[2] = on ? 255 : 200;
                q[3] = on ? 255 : 96;
                lum[y * 64 + x] = on ? 220 : 40;
                la[(y * 64 + x) * 2] = on ? 200 : 50;
                la[(y * 64 + x) * 2 + 1] = (unsigned char)(x * 4);
            }
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glEnable(GL_TEXTURE_2D);
        if (!strcmp(scene, "tex")) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glBegin(GL_QUADS);                 /* 1 texel = W/64 pixels, y vers le bas */
            glTexCoord2f(0, 0); glVertex2f(0, 0);
            glTexCoord2f(1, 0); glVertex2f(W, 0);
            glTexCoord2f(1, 1); glVertex2f(W, H);
            glTexCoord2f(0, 1); glVertex2f(0, H);
            glEnd();
            glFinish();
            check("case (0,0) sombre", W / 32, H / 32, 0x1E3CC8);
            check("case (1,0) claire", W / 8 + W / 32, H / 32, 0xFFFFFF);
            check("case (1,1) sombre", W / 8 + W / 32, H / 8 + H / 32, 0x1E3CC8);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glColor3f(0, 1, 0);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(0, 0);
            glTexCoord2f(1, 0); glVertex2f(W, 0);
            glTexCoord2f(1, 1); glVertex2f(W, H / 2);
            glTexCoord2f(0, 1); glVertex2f(0, H / 2);
            glEnd();
            glFinish();
            check("modulé vert, case claire", W / 8 + W / 32, H / 64, 0x00FF00);
            check("modulé vert, case sombre", W / 32, H / 64, 0x003C00);
        } else if (!strcmp(scene, "texpersp")) {
            int frames = 30;
            double t0;
            gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGBA, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, img);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glEnable(GL_DEPTH_TEST);
            glMatrixMode(GL_PROJECTION); glLoadIdentity();
            gluPerspective(60, (double)W / H, 0.5, 100);
            glMatrixMode(GL_MODELVIEW);
            t0 = now();
            for (f = 0; f < frames; f++) {
                glClearColor(0.3f, 0.3f, 0.35f, 1);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glLoadIdentity();
                glRotatef(f * 3.0f, 0, 1, 0);
                glBegin(GL_QUADS);             /* sol qui fuit à l'infini */
                glColor3f(1, 1, 1);
                glTexCoord2f(0, 0); glVertex3f(-20, -1, -40);
                glTexCoord2f(20, 0); glVertex3f(20, -1, -40);
                glTexCoord2f(20, 20); glVertex3f(20, -1, 0);
                glTexCoord2f(0, 20); glVertex3f(-20, -1, 0);
                glColor3f(1, 0.6f, 0.6f);      /* un cube texturé */
                glTexCoord2f(0, 0); glVertex3f(-1, -1, -4);
                glTexCoord2f(2, 0); glVertex3f(1, -1, -4);
                glTexCoord2f(2, 2); glVertex3f(1, 1, -4);
                glTexCoord2f(0, 2); glVertex3f(-1, 1, -4);
                glColor3f(0.6f, 1, 0.6f);
                glTexCoord2f(0, 0); glVertex3f(1, -1, -4);
                glTexCoord2f(2, 0); glVertex3f(1, -1, -6);
                glTexCoord2f(2, 2); glVertex3f(1, 1, -6);
                glTexCoord2f(0, 2); glVertex3f(1, 1, -4);
                glEnd();
                glFinish();
            }
            printf("texpersp : %d images %dx%d, trilinéaire : %.2f img/s\n",
                   frames, W, H, frames / (now() - t0));
        } else {
            /* quatre formats, un par quadrant */
            static const float envc[4] = { 1, 0.5f, 0, 1 };
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, envc);
            glClearColor(0.1f, 0.1f, 0.1f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            for (f = 0; f < 4; f++) {
                float x0 = (f % 2) * W / 2.0f, y0 = (f / 2) * H / 2.0f;
                switch (f) {
                case 0:
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
                    break;
                case 1:
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 64, 64, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
                    break;
                case 2:
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 64, 64, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, la);
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
                    break;
                default:
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
                    break;
                }
                glColor4f(0.8f, 0.4f, 0.9f, 1);
                glBegin(GL_TRIANGLES);
                glTexCoord2f(0, 0); glVertex2f(x0, y0);
                glTexCoord2f(1.5f, 0); glVertex2f(x0 + W / 2.0f, y0);
                glTexCoord2f(0, 1.5f); glVertex2f(x0, y0 + H / 2.0f);
                glTexCoord2f(1.5f, 0); glVertex2f(x0 + W / 2.0f, y0);
                glTexCoord2f(1.5f, 1.5f); glVertex2f(x0 + W / 2.0f, y0 + H / 2.0f);
                glTexCoord2f(0, 1.5f); glVertex2f(x0, y0 + H / 2.0f);
                glEnd();
            }
            glFinish();
        }
        glDeleteTextures(1, &id);
    } else if (!strcmp(scene, "texpack")) {
        /* Formats de texels compacts (jeux Mac : Zenerchi charge ses textures
           en GL_BGRA + GL_UNSIGNED_INT_8_8_8_8). Une bande par format, couleur
           unie, GL_REPLACE : la couleur lue doit être exacte. Les valeurs 16
           bits sont choisies pour que l'extension en 8 bits soit sans ambiguïté. */
        static const unsigned long bgra8888[4] = { 0x563412FF, 0x563412FF, 0x563412FF, 0x563412FF };
        static const unsigned long rgba8888r[4] = { 0xFFDEBC9A, 0xFFDEBC9A, 0xFFDEBC9A, 0xFFDEBC9A };
        static const unsigned short argb1555[4] = { 0xFE00, 0xFE00, 0xFE00, 0xFE00 };
        static const unsigned short rgb565[4] = { 0x041F, 0x041F, 0x041F, 0x041F };
        static const unsigned short rgba4444[4] = { 0x8F0F, 0x8F0F, 0x8F0F, 0x8F0F };
        static const unsigned long want[5] = { 0x123456, 0x9ABCDE, 0xFF8400, 0x0082FF, 0x88FF00 };
        static const char *const name[5] = {
            "BGRA UINT_8_8_8_8", "RGBA UINT_8_8_8_8_REV", "BGRA USHORT_1_5_5_5_REV",
            "RGB USHORT_5_6_5", "RGBA USHORT_4_4_4_4",
        };
        GLuint id;
        int f;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        for (f = 0; f < 5; f++) {
            float y0 = f * H / 5.0f;
            switch (f) {
            case 0: glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_BGRA,
                                 GL_UNSIGNED_INT_8_8_8_8, bgra8888); break;
            case 1: glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
                                 GL_UNSIGNED_INT_8_8_8_8_REV, rgba8888r); break;
            case 2: glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_BGRA,
                                 GL_UNSIGNED_SHORT_1_5_5_5_REV, argb1555); break;
            case 3: glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB,
                                 GL_UNSIGNED_SHORT_5_6_5, rgb565); break;
            default: glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
                                  GL_UNSIGNED_SHORT_4_4_4_4, rgba4444); break;
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(0, y0);
            glTexCoord2f(1, 0); glVertex2f(W, y0);
            glTexCoord2f(1, 1); glVertex2f(W, y0 + H / 5.0f);
            glTexCoord2f(0, 1); glVertex2f(0, y0 + H / 5.0f);
            glEnd();
            glFinish();
            check(name[f], W / 2, (int)(y0 + H / 10.0f), want[f]);
        }
        glDeleteTextures(1, &id);
    } else if (!strcmp(scene, "probe2")) {
        /* Sonde n°2 : lignes, points, brouillard, décalage de polygone, unité 1. */
        static const float fogc[4] = { 0.125f, 0.25f, 0.375f, 0.5f };
        static const unsigned char px2[4] = { 255, 128, 64, 255 };
        GLuint id;
        glClear(GL_COLOR_BUFFER_BIT);                                          /* 1 */
        glLineWidth(3.0f); glClear(GL_COLOR_BUFFER_BIT);                       /* 2 */
        glPointSize(5.0f); glClear(GL_COLOR_BUFFER_BIT);                       /* 3 */
        glEnable(GL_LINE_STIPPLE); glClear(GL_COLOR_BUFFER_BIT);               /* 4 */
        glEnable(GL_LINE_SMOOTH); glClear(GL_COLOR_BUFFER_BIT);                /* 5 */
        glEnable(GL_POINT_SMOOTH); glClear(GL_COLOR_BUFFER_BIT);               /* 6 */
        glFogfv(GL_FOG_COLOR, fogc); glClear(GL_COLOR_BUFFER_BIT);             /* 7 */
        glFogi(GL_FOG_MODE, GL_LINEAR); glClear(GL_COLOR_BUFFER_BIT);          /* 8 */
        glPolygonOffset(1.5f, 2.5f); glClear(GL_COLOR_BUFFER_BIT);             /* 9 */
        glEnable(GL_POLYGON_OFFSET_LINE); glClear(GL_COLOR_BUFFER_BIT);        /* 10 */
        glEnable(GL_POLYGON_OFFSET_POINT); glClear(GL_COLOR_BUFFER_BIT);       /* 11 */
        glDisable(GL_LINE_STIPPLE); glDisable(GL_LINE_SMOOTH); glDisable(GL_POINT_SMOOTH);
        glDisable(GL_POLYGON_OFFSET_LINE); glDisable(GL_POLYGON_OFFSET_POINT);
        glActiveTextureARB(GL_TEXTURE1_ARB);
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D); glClear(GL_COLOR_BUFFER_BIT);                 /* 12 */
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
        glClear(GL_COLOR_BUFFER_BIT);                                          /* 13 */
        glActiveTextureARB(GL_TEXTURE0_ARB);
        glClientActiveTextureARB(GL_TEXTURE0_ARB);
        /* primitives tracées (vidage des sommets) */
        glBegin(GL_TRIANGLES);
        glColor3f(1, 1, 1);
        glMultiTexCoord4fARB(GL_TEXTURE0_ARB, 0.1f, 0.2f, 0.3f, 1);
        glMultiTexCoord4fARB(GL_TEXTURE1_ARB, 0.6f, 0.7f, 0.8f, 1);
        glVertex2f(4, 4);
        glMultiTexCoord4fARB(GL_TEXTURE1_ARB, 0.65f, 0.75f, 0.85f, 1);
        glVertex2f(60, 4);
        glMultiTexCoord4fARB(GL_TEXTURE1_ARB, 0.66f, 0.76f, 0.86f, 1);
        glVertex2f(4, 60);
        glEnd();
        glActiveTextureARB(GL_TEXTURE1_ARB); glDisable(GL_TEXTURE_2D); glActiveTextureARB(GL_TEXTURE0_ARB);
        glEnable(GL_FOG);
        glFogf(GL_FOG_START, 0.0f); glFogf(GL_FOG_END, 1.0f);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0);
        glVertex3f(4, 4, -0.25f); glVertex3f(60, 4, -0.5f); glVertex3f(4, 60, -0.75f);
        glEnd();
        glDisable(GL_FOG);
        glLineWidth(1.0f);
        glBegin(GL_LINES); glColor3f(0, 1, 0); glVertex2f(2, 2); glVertex2f(40, 30); glEnd();
        glBegin(GL_LINE_STRIP); glVertex2f(2, 50); glVertex2f(20, 50); glVertex2f(20, 60); glEnd();
        glBegin(GL_LINE_LOOP); glVertex2f(30, 30); glVertex2f(40, 30); glVertex2f(40, 40); glEnd();
        glPointSize(1.0f);
        glBegin(GL_POINTS); glColor3f(0, 0, 1); glVertex2f(50.5f, 10.5f); glVertex2f(55.5f, 12.5f); glEnd();
        glFinish();
    } else if (!strcmp(scene, "mix") || !strcmp(scene, "game")) {
        /* Damier en unité 0, « lightmap » (dégradé 16×16) en unité 1, brouillard
           linéaire ; « mix » ajoute lignes et points, « game » anime un couloir. */
        unsigned char img[64 * 64 * 3], lm[16 * 16];
        static const float fogc[4] = { 0.5f, 0.55f, 0.6f, 1 };
        GLuint t0, t1;
        int x, y, f, frames = !strcmp(scene, "game") ? 60 : 1;
        double tstart;
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++) {
                int on = ((x / 8) + (y / 8)) & 1;
                img[(y * 64 + x) * 3] = on ? 230 : 120;
                img[(y * 64 + x) * 3 + 1] = on ? 200 : 80;
                img[(y * 64 + x) * 3 + 2] = on ? 150 : 40;
            }
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                lm[y * 16 + x] = (unsigned char)(80 + 10 * x + 2 * y);
        glGenTextures(1, &t0);
        glBindTexture(GL_TEXTURE_2D, t0);
        gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 64, 64, GL_RGB, GL_UNSIGNED_BYTE, img);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnable(GL_TEXTURE_2D);
        glActiveTextureARB(GL_TEXTURE1_ARB);
        glGenTextures(1, &t1);
        glBindTexture(GL_TEXTURE_2D, t1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 16, 16, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, lm);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnable(GL_TEXTURE_2D);
        glActiveTextureARB(GL_TEXTURE0_ARB);
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, 2.0f);
        glFogf(GL_FOG_END, 30.0f);
        glFogfv(GL_FOG_COLOR, fogc);
        glEnable(GL_DEPTH_TEST);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        gluPerspective(70, (double)W / H, 0.3, 60);
        glMatrixMode(GL_MODELVIEW);
        tstart = now();
        for (f = 0; f < frames; f++) {
            int k;
            glClearColor(fogc[0], fogc[1], fogc[2], 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glLoadIdentity();
            glTranslatef(0, 0, f * 0.4f);
            glBegin(GL_QUADS);
            glColor3f(1, 1, 1);
            for (k = 0; k < 12; k++) {                  /* couloir : sol, plafond, murs */
                float z0 = -k * 3.0f, z1 = z0 - 3.0f;
                int side;
                for (side = 0; side < 4; side++) {
                    float a0[3], a1[3], a2[3], a3[3];
                    switch (side) {
                    case 0: a0[0]=-2;a0[1]=-1.5f;a1[0]= 2;a1[1]=-1.5f;a2[0]= 2;a2[1]=-1.5f;a3[0]=-2;a3[1]=-1.5f; break;
                    case 1: a0[0]=-2;a0[1]= 1.5f;a1[0]= 2;a1[1]= 1.5f;a2[0]= 2;a2[1]= 1.5f;a3[0]=-2;a3[1]= 1.5f; break;
                    case 2: a0[0]=-2;a0[1]=-1.5f;a1[0]=-2;a1[1]= 1.5f;a2[0]=-2;a2[1]= 1.5f;a3[0]=-2;a3[1]=-1.5f; break;
                    default:a0[0]= 2;a0[1]=-1.5f;a1[0]= 2;a1[1]= 1.5f;a2[0]= 2;a2[1]= 1.5f;a3[0]= 2;a3[1]=-1.5f; break;
                    }
                    a0[2] = z0; a1[2] = z0; a2[2] = z1; a3[2] = z1;
                    glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0, 0); glTexCoord2f(0, 0); glVertex3fv(a0);
                    glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 1, 0); glTexCoord2f(2, 0); glVertex3fv(a1);
                    glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 1, 1); glTexCoord2f(2, 2); glVertex3fv(a2);
                    glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0, 1); glTexCoord2f(0, 2); glVertex3fv(a3);
                }
            }
            glEnd();
            if (!strcmp(scene, "mix")) {
                glDisable(GL_TEXTURE_2D);
                glActiveTextureARB(GL_TEXTURE1_ARB); glDisable(GL_TEXTURE_2D);
                glActiveTextureARB(GL_TEXTURE0_ARB);
                glLineWidth(2.0f);
                glBegin(GL_LINE_LOOP);
                glColor3f(1, 0, 0);
                glVertex3f(-1, -1, -3); glVertex3f(1, -1, -3); glVertex3f(1, 1, -3); glVertex3f(-1, 1, -3);
                glEnd();
                glPointSize(4.0f);
                glBegin(GL_POINTS);
                glColor3f(0, 1, 0);
                for (k = 0; k < 8; k++)
                    glVertex3f(-1.5f + k * 0.4f, 0, -4);
                glEnd();
                glEnable(GL_TEXTURE_2D);
                glActiveTextureARB(GL_TEXTURE1_ARB); glEnable(GL_TEXTURE_2D);
                glActiveTextureARB(GL_TEXTURE0_ARB);
            }
            glFinish();
        }
        if (frames > 1)
            printf("game : %d images %dx%d, couloir multitexture + brouillard : %.2f img/s\n",
                   frames, W, H, frames / (now() - tstart));
    } else if (!strcmp(scene, "comb")) {
        /* GL_COMBINE et quatre unités (Marble Blast, moteur Torque) : une
           bande par montage, textures 1×1 pour que le résultat soit exact.
           Comparé au rendu d'Apple par l'image entière. */
        static const unsigned char ta[4] = { 128, 64, 255, 128 };   /* RGBA */
        static const unsigned char tb[4] = { 128, 255, 0, 255 };
        static const float envc[4] = { 0.25f, 0, 0, 1 };
        GLuint id[2];
        int f, u;
        glGenTextures(2, id);
        for (f = 0; f < 2; f++) {
            glActiveTextureARB(GL_TEXTURE0_ARB + f);
            glBindTexture(GL_TEXTURE_2D, id[f]);
            glTexImage2D(GL_TEXTURE_2D, 0, f ? GL_RGB : GL_RGBA, 1, 1, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, f ? tb : ta);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        for (f = 0; f < 4; f++) {
            float y0 = f * H / 4.0f;
            int nunits = f + 1;
            for (u = 0; u < 4; u++) {
                glActiveTextureARB(GL_TEXTURE0_ARB + u);
                if (u >= nunits) {
                    glDisable(GL_TEXTURE_2D);
                    continue;
                }
                glBindTexture(GL_TEXTURE_2D, id[u & 1]);
                glEnable(GL_TEXTURE_2D);
                glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, envc);
                if (u == 0) {
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
                    continue;
                }
                glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
                if (u == 1) {                   /* INTERPOLATE, échelle RGB 2 */
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_CONSTANT);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_PRIMARY_COLOR);
                    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);
                    glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 2.0f);
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
                } else if (u == 2) {            /* ADD_SIGNED avec la couleur */
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_ADD_SIGNED);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
                } else {                        /* SUBTRACT d'une constante */
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_SUBTRACT);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_CONSTANT);
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
                }
            }
            glColor4f(1, 0.5f, 0.25f, 0.5f);
            glBegin(GL_QUADS);
            glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0.5f, 0.5f);
            glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0.5f, 0.5f);
            glMultiTexCoord2fARB(GL_TEXTURE2_ARB, 0.5f, 0.5f);
            glMultiTexCoord2fARB(GL_TEXTURE3_ARB, 0.5f, 0.5f);
            glVertex2f(0, y0);
            glVertex2f(W, y0);
            glVertex2f(W, y0 + H / 4.0f);
            glVertex2f(0, y0 + H / 4.0f);
            glEnd();
            glFinish();
            printf("  bande %d (%d unités) = %06lx\n", f, nunits,
                   px(W / 2, (int)(y0 + H / 8.0f)));
        }
        for (u = 3; u >= 0; u--) {
            glActiveTextureARB(GL_TEXTURE0_ARB + u);
            glDisable(GL_TEXTURE_2D);
        }
        glActiveTextureARB(GL_TEXTURE0_ARB);
        glDeleteTextures(2, id);
    } else if (!strcmp(scene, "stencil")) {
        /* Stencil (GLTEST_STENCIL=1) : masque, comptage de recouvrement, zfail,
           masque d'écriture. Pixels témoins au centre des zones, jamais sur une arête. */
        glClearColor(0, 0, 0, 1);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glEnable(GL_STENCIL_TEST);
        /* 1. masque : un carré écrit 1 dans le stencil, couleur fermée */
        glColorMask(0, 0, 0, 0);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glRectf(W / 4.0f, H / 4.0f, 3 * W / 4.0f, 3 * H / 4.0f);
        glColorMask(1, 1, 1, 1);
        /* rouge partout où le stencil vaut 1, vert partout où il vaut 0 */
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilFunc(GL_EQUAL, 1, 0xFF);
        glColor3f(1, 0, 0); glRectf(0, 0, W, H);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
        glColor3f(0, 1, 0); glRectf(0, 0, W, H);
        glFinish();
        check("dans le masque : rouge", W / 2, H / 2, 0xFF0000);
        check("hors du masque : vert", W / 8, H / 8, 0x00FF00);
        /* 2. comptage : deux carrés qui se recouvrent, INCR ; bleu où le compte vaut 2 */
        glClear(GL_STENCIL_BUFFER_BIT);
        glColorMask(0, 0, 0, 0);
        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
        glRectf(0, 0, 5 * W / 8.0f, 5 * H / 8.0f);
        glRectf(3 * W / 8.0f, 3 * H / 8.0f, W, H);
        glColorMask(1, 1, 1, 1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilFunc(GL_LEQUAL, 2, 0xFF);           /* 2 <= stencil */
        glColor3f(0, 0, 1); glRectf(0, 0, W, H);
        glFinish();
        check("recouvrement : bleu", W / 2, H / 2, 0x0000FF);
        check("un seul carré : inchangé (vert)", W / 8, H / 8, 0x00FF00);
        /* 3. zfail : un plan proche écrit la profondeur, un plan lointain échoue en
              profondeur et INVERSE le stencil ; jaune où le stencil vaut 0xFF */
        glClear(GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glColorMask(0, 0, 0, 0);
        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glBegin(GL_QUADS);                            /* proche : moitié gauche */
        glVertex3f(0, 0, 0.5f); glVertex3f(W / 2.0f, 0, 0.5f);
        glVertex3f(W / 2.0f, H, 0.5f); glVertex3f(0, H, 0.5f);
        glEnd();
        glStencilOp(GL_KEEP, GL_INVERT, GL_KEEP);
        glBegin(GL_QUADS);                            /* lointain : partout */
        glVertex3f(0, 0, -0.5f); glVertex3f(W, 0, -0.5f);
        glVertex3f(W, H, -0.5f); glVertex3f(0, H, -0.5f);
        glEnd();
        glDisable(GL_DEPTH_TEST);
        glColorMask(1, 1, 1, 1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilFunc(GL_EQUAL, 0xFF, 0xFF);
        glColor3f(1, 1, 0); glRectf(0, 0, W, H);
        glFinish();
        check("zfail à gauche : jaune", W / 4, H / 2, 0xFFFF00);
        check("zpass à droite : inchangé (bleu)", 9 * W / 16, H / 2, 0x0000FF);
        /* 4. masque d'écriture : REPLACE 0xFF sous le masque 0x0F donne 0x0F */
        glClear(GL_STENCIL_BUFFER_BIT);
        glColorMask(0, 0, 0, 0);
        glStencilMask(0x0F);
        glStencilFunc(GL_ALWAYS, 0xFF, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glRectf(0, 0, W, H);
        glStencilMask(0xFF);
        glColorMask(1, 1, 1, 1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilFunc(GL_EQUAL, 0x0F, 0xFF);
        glColor3f(1, 0, 1); glRectf(0, 0, W, H);
        glFinish();
        check("masque d'écriture 0x0F : magenta", W / 2, H / 2, 0xFF00FF);
        glDisable(GL_STENCIL_TEST);
    } else if (!strcmp(scene, "stencilprobe")) {
        /* Sonde du stencil : un réglage par glClear (le traceur vide l'état GL à
           chaque effacement avec POMPPC_GLTRACE_STATE=1 ; on diffe les vidages). */
        int i;
        glClearColor(0, 0, 0, 1);
        for (i = 0; i < 12; i++) {
            switch (i) {
            case 0: break;                                              /*  1 référence */
            case 1: glEnable(GL_STENCIL_TEST); break;                   /*  2 */
            case 2: glStencilFunc(GL_EQUAL, 0, 0xFFFFFFFF); break;      /*  3 fonction */
            case 3: glStencilFunc(GL_EQUAL, 0x5A, 0xFFFFFFFF); break;   /*  4 référence */
            case 4: glStencilFunc(GL_EQUAL, 0x5A, 0x3C); break;         /*  5 masque de valeur */
            case 5: glStencilMask(0xA5); break;                         /*  6 masque d'écriture */
            case 6: glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP); break;   /*  7 fail */
            case 7: glStencilOp(GL_REPLACE, GL_INCR, GL_KEEP); break;   /*  8 zfail */
            case 8: glStencilOp(GL_REPLACE, GL_INCR, GL_INVERT); break; /*  9 zpass */
            case 9: glClearStencil(0x77); break;                        /* 10 valeur d'effacement */
            case 10: glStencilOp(GL_REPLACE, GL_INCR_WRAP, GL_DECR_WRAP); break; /* 11 */
            default: glDisable(GL_STENCIL_TEST); break;                 /* 12 */
            }
            glClear(GL_COLOR_BUFFER_BIT);
        }
    } else if (!strcmp(scene, "combprobe")) {
        /* Sonde GL_COMBINE et unités 2–3 : un réglage par glClear (le traceur
           vide l'état GL à chaque effacement avec POMPPC_GLTRACE_STATE=1). */
        static const GLenum steps[][3] = {        /* unité, paramètre, valeur */
            { 0, 0, 0 },                                            /*  1 référence */
            { 0, GL_TEXTURE_ENV_MODE, GL_COMBINE },                 /*  2 */
            { 0, GL_COMBINE_RGB, GL_INTERPOLATE },                  /*  3 */
            { 0, GL_COMBINE_ALPHA, GL_ADD_SIGNED },                 /*  4 */
            { 0, GL_SOURCE0_RGB, GL_CONSTANT },                     /*  5 */
            { 0, GL_SOURCE1_RGB, GL_PRIMARY_COLOR },                /*  6 */
            { 0, GL_SOURCE2_RGB, GL_PREVIOUS },                     /*  7 */
            { 0, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_ALPHA },         /*  8 */
            { 0, GL_OPERAND1_RGB, GL_SRC_ALPHA },                   /*  9 */
            { 0, GL_OPERAND2_RGB, GL_ONE_MINUS_SRC_COLOR },         /* 10 */
            { 0, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR },              /* 11 */
            { 0, GL_SOURCE1_ALPHA, GL_CONSTANT },                   /* 12 */
            { 0, GL_SOURCE2_ALPHA, GL_TEXTURE },                    /* 13 */
            { 0, GL_OPERAND0_ALPHA, GL_ONE_MINUS_SRC_ALPHA },       /* 14 */
            { 0, GL_OPERAND1_ALPHA, GL_ONE_MINUS_SRC_ALPHA },       /* 15 */
            { 0, GL_OPERAND2_ALPHA, GL_ONE_MINUS_SRC_ALPHA },       /* 16 */
            { 0, GL_RGB_SCALE, 4 },                                 /* 17 */
            { 0, GL_ALPHA_SCALE, 2 },                               /* 18 */
            { 1, GL_COMBINE_RGB, GL_DOT3_RGB },                     /* 19 */
            { 2, GL_TEXTURE_2D, 1 },                                /* 20 */
            { 3, GL_COMBINE_RGB, GL_SUBTRACT },                     /* 21 */
            { 2, GL_COMBINE_RGB, GL_DOT3_RGBA },                    /* 22 */
        };
        int i;
        glClearColor(0, 0, 0, 1);
        for (i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); i++) {
            glActiveTextureARB(GL_TEXTURE0_ARB + steps[i][0]);
            if (steps[i][1] == GL_TEXTURE_2D)
                glEnable(GL_TEXTURE_2D);
            else if (steps[i][1] == GL_RGB_SCALE || steps[i][1] == GL_ALPHA_SCALE)
                glTexEnvf(GL_TEXTURE_ENV, steps[i][1], (float)steps[i][2]);
            else if (steps[i][1])
                glTexEnvi(GL_TEXTURE_ENV, steps[i][1], steps[i][2]);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glActiveTextureARB(GL_TEXTURE0_ARB);
    } else if (!strcmp(scene, "texprobe")) {
        /* Sonde texture : réglages un par un, chacun suivi d'un glClear que le
           traceur vide (état GL + objet texture de l'unité 0). */
        static const unsigned char tex[4 * 4 * 4] = {
            255,0,0,255,   0,255,0,255,   0,0,255,255,   255,255,0,255,
            0,255,255,255, 255,0,255,255, 17,34,51,68,   85,102,119,136,
            1,2,3,4,       5,6,7,8,       9,10,11,12,    13,14,15,16,
            200,100,50,25, 25,50,100,200, 128,64,32,16,  16,32,64,128,
        };
        static const float envc[4] = { 0.0625f, 0.1875f, 0.3125f, 0.4375f };
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);                                           /* 1 */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glClear(GL_COLOR_BUFFER_BIT);                                           /* 2 */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glClear(GL_COLOR_BUFFER_BIT);                                           /* 3 */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glClear(GL_COLOR_BUFFER_BIT);                                           /* 4 */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glClear(GL_COLOR_BUFFER_BIT);                                           /* 5 */
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glClear(GL_COLOR_BUFFER_BIT);                                           /* 6 */
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
        glClear(GL_COLOR_BUFFER_BIT);                                           /* 7 */
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, envc);
        glClear(GL_COLOR_BUFFER_BIT);                                           /* 8 */
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glMatrixMode(GL_TEXTURE); glLoadIdentity(); glMatrixMode(GL_MODELVIEW);
        glBegin(GL_QUADS);
        glColor4f(1, 1, 1, 1);
        glTexCoord4f(0.125f, 0.25f, 0.375f, 1.0f); glVertex2f(4, 4);
        glTexCoord4f(0.875f, 0.25f, 0.625f, 1.0f); glVertex2f(60, 4);
        glTexCoord4f(0.875f, 0.75f, 0.0625f, 1.0f); glVertex2f(60, 60);
        glTexCoord4f(0.125f, 0.75f, 0.9375f, 1.0f); glVertex2f(4, 60);
        glEnd();
        glClear(GL_DEPTH_BUFFER_BIT);                                           /* 9 */
        glFinish();
    } else if (!strcmp(scene, "varray")) {
        /* Sonde des tableaux de sommets (axe 1) : glDrawArrays puis
           glDrawElements, avec une projection en PERSPECTIVE et une modèle-vue
           non triviale. Les coordonnées d'objet sont choisies pour être
           reconnaissables dans un vidage : si le tampon remis au pilote les
           contient telles quelles, la géométrie arrive NON transformée.
           Repère : translation (0.25, 0.125, -2) puis glFrustum(-1,1,-1,1,1,10),
           donc x_ndc = x_œil / 2 et y_ndc = y_œil / 2. */
        static const float qa[6 * 3] = {          /* quad A, deux triangles */
            -2.0625f, -1.125f, 0.0f,  -0.4375f, -1.125f, 0.0f,  -0.4375f, 0.875f, 0.0f,
            -2.0625f, -1.125f, 0.0f,  -0.4375f,  0.875f, 0.0f,  -2.0625f, 0.875f, 0.0f,
        };
        static const float ca[6 * 3] = {
            1, 0, 0,  1, 0, 0,  1, 0, 0,  1, 0, 0,  1, 0, 0,  1, 0, 0,
        };
        static const float qb[4 * 3] = {          /* quad B, indexé */
            -0.0625f, -1.125f, 0.0f,   1.5625f, -1.125f, 0.0f,
             1.5625f,  0.875f, 0.0f,  -0.0625f,  0.875f, 0.0f,
        };
        static const float cb[4 * 3] = { 0, 1, 0,  0, 1, 0,  0, 1, 0,  0, 1, 0 };
        static const GLushort ib[6] = { 0, 1, 2, 0, 2, 3 };
        float sx = W / 64.0f, sy = H / 64.0f;

        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glFrustum(-1, 1, -1, 1, 1, 10);
        glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
        glTranslatef(0.25f, 0.125f, -2.0f);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, qa);
        glColorPointer(3, GL_FLOAT, 0, ca);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glVertexPointer(3, GL_FLOAT, 0, qb);
        glColorPointer(3, GL_FLOAT, 0, cb);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, ib);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glFinish();

        /* y est compté depuis le HAUT ; y_œil = +1 tombe à la ligne 16 sur 64. */
        check("quad A (glDrawArrays) rouge", (int)(16 * sx), (int)(32 * sy), 0xFF0000);
        check("quad B (glDrawElements) vert", (int)(48 * sx), (int)(32 * sy), 0x00FF00);
        check("entre les deux quads : fond", (int)(32 * sx), (int)(32 * sy), 0x0000FF);
        check("au-dessus des quads : fond", (int)(32 * sx), (int)(8 * sy), 0x0000FF);
    } else if (!strcmp(scene, "tclprobe")) {
        /* Sonde des codes du descripteur de sortie de sommet (docs/re/
           descripteur-de-sommet.md). UN triangle, tous les attributs
           DISTINCTS et reconnaissables dans le vidage de EndPrimitiveBuffer.
           Ne vérifie aucun pixel : c'est la trace qui est le résultat. */
        static const float pos[3][3] = { {1,2,3}, {4,5,6}, {7,8,9} };
        static const float nrm[3][3] = { {0.1f,0.2f,0.3f}, {0.4f,0.5f,0.6f},
                                         {0.7f,0.8f,0.9f} };
        static const float col[3][4] = { {0.11f,0.22f,0.33f,0.44f},
                                         {0.51f,0.52f,0.53f,0.54f},
                                         {0.61f,0.62f,0.63f,0.64f} };
        static const float sec[3][3] = { {0.71f,0.72f,0.73f},
                                         {0.74f,0.75f,0.76f},
                                         {0.77f,0.78f,0.79f} };
        static const float t0c[3][2] = { {0.5f,0.25f}, {1.5f,1.25f}, {2.5f,2.25f} };
        static const float t1c[3][2] = { {0.75f,0.125f}, {1.75f,1.125f}, {2.75f,2.125f} };
        static const float fogc[3]   = { 0.9f, 1.9f, 2.9f };
        static const GLushort idx[3] = { 0, 1, 2 };
        static const float clip[3][3] = { {-5,-1,0}, {5,-1,0}, {0,5,0} };
        const char *mode = getenv("TCLP_MODE") ? getenv("TCLP_MODE") : "imm";
        const float *P = getenv("TCLP_CLIP") ? &clip[0][0] : &pos[0][0];
        GLuint tex = 0, lst = 0;
        int i;

        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        if (getenv("TCLP_PERSP"))
            glFrustum(-1, 1, -1, 1, 1, 100);
        glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
        if (!getenv("TCLP_CLIP"))
            glTranslatef(10, 20, 30);       /* modèle-vue reconnaissable */
        if (getenv("TCLP_TEXMAT")) {
            glMatrixMode(GL_TEXTURE); glLoadIdentity();
            glTranslatef(100, 200, 0);
            glMatrixMode(GL_MODELVIEW);
        }
        if (getenv("TCLP_TEX")) {
            static unsigned char t[4 * 4 * 4];
            for (i = 0; i < 4 * 4 * 4; i++) t[i] = (unsigned char)(i * 4);
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, t);
            glEnable(GL_TEXTURE_2D);
        }
        if (getenv("TCLP_TEXGEN")) {
            static const GLfloat ps[4] = { 1, 0, 0, 0.5f };
            static const GLfloat pt[4] = { 0, 1, 0, 0.25f };
            glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
            glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
            glTexGenfv(GL_S, GL_OBJECT_PLANE, ps);
            glTexGenfv(GL_T, GL_OBJECT_PLANE, pt);
            glEnable(GL_TEXTURE_GEN_S); glEnable(GL_TEXTURE_GEN_T);
        }
        if (getenv("TCLP_LIGHT")) {
            static const GLfloat dir[4] = { 0, 0, 1, 0 };   /* directionnelle */
            static const GLfloat dif[4] = { 1, 1, 1, 1 };
            static const GLfloat amb[4] = { 0.25f, 0.25f, 0.25f, 1 };
            glLightfv(GL_LIGHT0, GL_POSITION, dir);
            glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
            glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
            glEnable(GL_LIGHT0);
            glEnable(GL_LIGHTING);
            if (getenv("TCLP_COLORMAT")) {
                glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
                glEnable(GL_COLOR_MATERIAL);
            }
            if (getenv("TCLP_TWOSIDE"))
                glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);
        }
        if (getenv("TCLP_MATERIAL")) {
            static const GLfloat amb[4] = { 0.31f, 0.32f, 0.33f, 0.34f };
            static const GLfloat dif[4] = { 0.41f, 0.42f, 0.43f, 0.44f };
            static const GLfloat spe[4] = { 0.81f, 0.82f, 0.83f, 0.84f };
            static const GLfloat emi[4] = { 0.91f, 0.92f, 0.93f, 0.94f };
            glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, amb);
            glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, dif);
            glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spe);
            glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, emi);
        }
        if (getenv("TCLP_CULL")) {
            glCullFace(getenv("TCLP_CULLFRONT") ? GL_FRONT : GL_BACK);
            glEnable(GL_CULL_FACE);
        }
        if (getenv("TCLP_FOG")) {
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glEnable(GL_FOG);
            if (getenv("TCLP_FOGCOORD"))
                glFogi(0x8450 /* GL_FOG_COORDINATE_SOURCE */,
                       0x8451 /* GL_FOG_COORDINATE */);
        }
        if (getenv("TCLP_SECCOL"))
            glEnable(0x81F9 /* GL_COLOR_SUM */);
        if (getenv("TCLP_UNFILLED"))
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        if (!strcmp(mode, "list")) {
            lst = glGenLists(1);
            glNewList(lst, GL_COMPILE);
        }
        if (!strcmp(mode, "arrays") || !strcmp(mode, "elements")) {
            glEnableClientState(GL_VERTEX_ARRAY);
            glVertexPointer(3, GL_FLOAT, 0, P);
            glEnableClientState(GL_NORMAL_ARRAY);
            glNormalPointer(GL_FLOAT, 0, nrm);
            glEnableClientState(GL_COLOR_ARRAY);
            glColorPointer(4, GL_FLOAT, 0, col);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, t0c);
            glClientActiveTextureARB(GL_TEXTURE1_ARB);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, t1c);
            glClientActiveTextureARB(GL_TEXTURE0_ARB);
            glEnableClientState(0x845E /* GL_SECONDARY_COLOR_ARRAY_EXT */);
            glSecondaryColorPointerEXT(3, GL_FLOAT, 0, sec);
            glEnableClientState(0x8457 /* GL_FOG_COORDINATE_ARRAY_EXT */);
            glFogCoordPointerEXT(GL_FLOAT, 0, fogc);
            if (!strcmp(mode, "arrays"))
                glDrawArrays(GL_TRIANGLES, 0, 3);
            else
                glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
            glDisableClientState(0x8457);
            glDisableClientState(0x845E);
            glClientActiveTextureARB(GL_TEXTURE1_ARB);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glClientActiveTextureARB(GL_TEXTURE0_ARB);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glDisableClientState(GL_COLOR_ARRAY);
            glDisableClientState(GL_NORMAL_ARRAY);
            glDisableClientState(GL_VERTEX_ARRAY);
        } else {
            glBegin(GL_TRIANGLES);
            for (i = 0; i < 3; i++) {
                int j = getenv("TCLP_BACKFACE") ? 2 - i : i;
                if (getenv("TCLP_EDGE"))
                    glEdgeFlag(i == 1 ? GL_FALSE : GL_TRUE);
                glNormal3fv(nrm[j]);
                glColor4fv(col[j]);
                glSecondaryColor3fvEXT(sec[j]);
                glFogCoordfEXT(fogc[j]);
                glTexCoord2fv(t0c[j]);
                glMultiTexCoord2fARB(GL_TEXTURE1_ARB, t1c[j][0], t1c[j][1]);
                glVertex3fv(P + j * 3);
            }
            glEnd();
        }
        if (!strcmp(mode, "list")) {
            glEndList();
            glCallList(lst);
            glCallList(lst);            /* deux fois : la 2ᵉ est « compilée » */
        }
        glFinish();
        printf("tclprobe : mode=%s persp=%d light=%d texgen=%d cull=%d clip=%d\n",
               mode, getenv("TCLP_PERSP") ? 1 : 0, getenv("TCLP_LIGHT") ? 1 : 0,
               getenv("TCLP_TEXGEN") ? 1 : 0, getenv("TCLP_CULL") ? 1 : 0,
               getenv("TCLP_CLIP") ? 1 : 0);
        if (tex) glDeleteTextures(1, &tex);
        if (lst) glDeleteLists(lst, 1);
    } else if (!strcmp(scene, "lightprobe")) {
        /* Sonde de l'éclairage (docs/re/etat-tcl.md §1) : un réglage GL par
           glClear. Toutes les valeurs sont des multiples distincts de 1/32 pour
           être retrouvées par recherche dans le vidage autant que par diff. */
        static const float amb[4]  = { 0.0625f, 0.125f,  0.1875f, 0.25f   };
        static const float dif[4]  = { 0.3125f, 0.375f,  0.4375f, 0.5f    };
        static const float spc[4]  = { 0.5625f, 0.625f,  0.6875f, 0.75f   };
        static const float pos[4]  = { 1.5f,    2.5f,    3.5f,    1.0f    };
        static const float dirl[4] = { 4.5f,    5.5f,    6.5f,    0.0f    };
        static const float sdir[3] = { 0.25f,   0.5f,    0.75f            };
        static const float scn[4]  = { 0.8125f, 0.875f,  0.9375f, 1.0f    };
        static const float l7d[4]  = { 0.03125f,0.0625f, 0.09375f,0.125f  };
        static const float l7a[4]  = { 0.15625f,0.1875f, 0.21875f,0.25f   };
        glClearColor(0, 0, 0, 1);
        pstep("référence");
        glEnable(GL_LIGHTING);
        pstep("glEnable(GL_LIGHTING)");
        glEnable(GL_LIGHT0);
        pstep("glEnable(GL_LIGHT0)");
        glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
        pstep("glLightfv(L0, GL_AMBIENT, .0625 .125 .1875 .25)");
        glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
        pstep("glLightfv(L0, GL_DIFFUSE, .3125 .375 .4375 .5)");
        glLightfv(GL_LIGHT0, GL_SPECULAR, spc);
        pstep("glLightfv(L0, GL_SPECULAR, .5625 .625 .6875 .75)");
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        pstep("glLightfv(L0, GL_POSITION, 1.5 2.5 3.5 1) modèle-vue identité");
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glTranslatef(10, 20, 30);
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        glPopMatrix();
        pstep("glLightfv(L0, GL_POSITION, 1.5 2.5 3.5 1) sous translate(10,20,30)");
        glLightfv(GL_LIGHT0, GL_POSITION, dirl);
        pstep("glLightfv(L0, GL_POSITION, 4.5 5.5 6.5 0) directionnelle");
        glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, sdir);
        pstep("glLightfv(L0, GL_SPOT_DIRECTION, .25 .5 .75) identité");
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glScalef(2, 4, 8);
        glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, sdir);
        glPopMatrix();
        pstep("glLightfv(L0, GL_SPOT_DIRECTION, .25 .5 .75) sous scale(2,4,8)");
        glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 60.0f);
        pstep("glLightf(L0, GL_SPOT_CUTOFF, 60)");
        glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 30.0f);
        pstep("glLightf(L0, GL_SPOT_CUTOFF, 30)");
        glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, 12.0f);
        pstep("glLightf(L0, GL_SPOT_EXPONENT, 12)");
        glLightf(GL_LIGHT0, GL_CONSTANT_ATTENUATION, 3.0f);
        pstep("glLightf(L0, GL_CONSTANT_ATTENUATION, 3)");
        glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 5.0f);
        pstep("glLightf(L0, GL_LINEAR_ATTENUATION, 5)");
        glLightf(GL_LIGHT0, GL_QUADRATIC_ATTENUATION, 7.0f);
        pstep("glLightf(L0, GL_QUADRATIC_ATTENUATION, 7)");
        glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 180.0f);
        pstep("glLightf(L0, GL_SPOT_CUTOFF, 180)");
        glEnable(GL_LIGHT5);
        pstep("glEnable(GL_LIGHT5)");
        glLightfv(GL_LIGHT5, GL_DIFFUSE, dif);
        pstep("glLightfv(L5, GL_DIFFUSE, .3125 .375 .4375 .5)");
        glEnable(GL_LIGHT7);
        pstep("glEnable(GL_LIGHT7)");
        glLightfv(GL_LIGHT7, GL_DIFFUSE, l7d);
        pstep("glLightfv(L7, GL_DIFFUSE, .03125 .0625 .09375 .125)");
        glLightfv(GL_LIGHT7, GL_AMBIENT, l7a);
        pstep("glLightfv(L7, GL_AMBIENT, .15625 .1875 .21875 .25)");
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, scn);
        pstep("glLightModelfv(GL_LIGHT_MODEL_AMBIENT, .8125 .875 .9375 1)");
        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
        pstep("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1)");
        glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);
        pstep("glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 1)");
        glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);
        pstep("glLightModeli(COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR)");
        glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SINGLE_COLOR);
        pstep("glLightModeli(COLOR_CONTROL, GL_SINGLE_COLOR)");
        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);
        pstep("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 0)");
        glDisable(GL_LIGHT7);
        pstep("glDisable(GL_LIGHT7)");
        glDisable(GL_LIGHT5); glDisable(GL_LIGHT0); glDisable(GL_LIGHTING);
        pstep("glDisable(LIGHT5, LIGHT0, LIGHTING)");
        glFinish();
    } else if (!strcmp(scene, "matprobe")) {
        /* Sonde du matériau, de GL_COLOR_MATERIAL et des valeurs COURANTES
           (docs/re/etat-tcl.md §2). Les deux objets matériau sont vidés par le
           traceur (clear-matf / clear-matb, pointeurs GS+0x4a70/0x4a74). */
        static const float mam[4] = { 0.03125f, 0.0625f, 0.09375f, 0.125f  };
        static const float mdi[4] = { 0.15625f, 0.1875f, 0.21875f, 0.25f   };
        static const float msp[4] = { 0.28125f, 0.3125f, 0.34375f, 0.375f  };
        static const float mem[4] = { 0.40625f, 0.4375f, 0.46875f, 0.5f    };
        static const float bam[4] = { 0.53125f, 0.5625f, 0.59375f, 0.625f  };
        static const float bdi[4] = { 0.65625f, 0.6875f, 0.71875f, 0.75f   };
        static const float bsp[4] = { 0.78125f, 0.8125f, 0.84375f, 0.875f  };
        static const float bem[4] = { 0.90625f, 0.9375f, 0.96875f, 1.0f    };
        static const float scol[3] = { 0.5625f, 0.6875f, 0.8125f };
        glClearColor(0, 0, 0, 1);
        pstep("référence");
        glMaterialfv(GL_FRONT, GL_AMBIENT, mam);
        pstep("glMaterialfv(FRONT, AMBIENT, .03125 .0625 .09375 .125)");
        glMaterialfv(GL_FRONT, GL_DIFFUSE, mdi);
        pstep("glMaterialfv(FRONT, DIFFUSE, .15625 .1875 .21875 .25)");
        glMaterialfv(GL_FRONT, GL_SPECULAR, msp);
        pstep("glMaterialfv(FRONT, SPECULAR, .28125 .3125 .34375 .375)");
        glMaterialfv(GL_FRONT, GL_EMISSION, mem);
        pstep("glMaterialfv(FRONT, EMISSION, .40625 .4375 .46875 .5)");
        glMaterialf(GL_FRONT, GL_SHININESS, 37.0f);
        pstep("glMaterialf(FRONT, SHININESS, 37)");
        glMaterialfv(GL_BACK, GL_AMBIENT, bam);
        pstep("glMaterialfv(BACK, AMBIENT, .53125 .5625 .59375 .625)");
        glMaterialfv(GL_BACK, GL_DIFFUSE, bdi);
        pstep("glMaterialfv(BACK, DIFFUSE, .65625 .6875 .71875 .75)");
        glMaterialfv(GL_BACK, GL_SPECULAR, bsp);
        pstep("glMaterialfv(BACK, SPECULAR, .78125 .8125 .84375 .875)");
        glMaterialfv(GL_BACK, GL_EMISSION, bem);
        pstep("glMaterialfv(BACK, EMISSION, .90625 .9375 .96875 1)");
        glMaterialf(GL_BACK, GL_SHININESS, 23.0f);
        pstep("glMaterialf(BACK, SHININESS, 23)");
        glEnable(GL_COLOR_MATERIAL);
        pstep("glEnable(GL_COLOR_MATERIAL)");
        glColorMaterial(GL_FRONT, GL_SPECULAR);
        pstep("glColorMaterial(GL_FRONT, GL_SPECULAR)");
        glColorMaterial(GL_BACK, GL_EMISSION);
        pstep("glColorMaterial(GL_BACK, GL_EMISSION)");
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
        pstep("glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE)");
        glDisable(GL_COLOR_MATERIAL);
        pstep("glDisable(GL_COLOR_MATERIAL)");
        glColor4f(0.0625f, 0.1875f, 0.3125f, 0.4375f);
        pstep("glColor4f(.0625 .1875 .3125 .4375)");
        glNormal3f(0.09375f, 0.15625f, 0.21875f);
        pstep("glNormal3f(.09375 .15625 .21875)");
        glSecondaryColor3fvEXT(scol);
        pstep("glSecondaryColor3fvEXT(.5625 .6875 .8125)");
        glFogCoordfEXT(0.34375f);
        pstep("glFogCoordfEXT(.34375)");
        glTexCoord4f(0.28125f, 0.46875f, 0.59375f, 0.71875f);
        pstep("glTexCoord4f(.28125 .46875 .59375 .71875) unité 0");
        glMultiTexCoord4fARB(GL_TEXTURE3_ARB, 0.03125f, 0.09375f, 0.15625f, 0.90625f);
        pstep("glMultiTexCoord4fARB(TEXTURE3, .03125 .09375 .15625 .90625)");
        glFinish();
    } else if (!strcmp(scene, "mtxprobe")) {
        /* Sonde des matrices et des piles (docs/re/etat-tcl.md §4). */
        static const float M[16] = { 1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16 };
        static const float N[16] = { 2,0,0,0, 0,2,0,0, 0,0,2,0,
                                     0.125f, 0.25f, 0.375f, 1 };
        static const float P[16] = { 0.0625f,0,0,0, 0,0.0625f,0,0, 0,0,0.0625f,0,
                                     0.5625f, 0.6875f, 0.8125f, 1 };
        glClearColor(0, 0, 0, 1);
        pstep("référence");
        glMatrixMode(GL_MODELVIEW); glLoadMatrixf(M);
        pstep("glMatrixMode(MODELVIEW); glLoadMatrixf(1..16)");
        glMatrixMode(GL_PROJECTION); glLoadMatrixf(N);
        pstep("glMatrixMode(PROJECTION); glLoadMatrixf(N)");
        glMatrixMode(GL_TEXTURE); glLoadMatrixf(M);
        pstep("glMatrixMode(TEXTURE) unité 0 ; glLoadMatrixf(1..16)");
        glActiveTextureARB(GL_TEXTURE1_ARB);
        glMatrixMode(GL_TEXTURE); glLoadMatrixf(N);
        pstep("unité 1 : glMatrixMode(TEXTURE) ; glLoadMatrixf(N)");
        glActiveTextureARB(GL_TEXTURE2_ARB);
        glMatrixMode(GL_TEXTURE); glLoadMatrixf(P);
        pstep("unité 2 : glMatrixMode(TEXTURE) ; glLoadMatrixf(P)");
        glActiveTextureARB(GL_TEXTURE0_ARB);
        glMatrixMode(GL_COLOR); glLoadMatrixf(M);
        pstep("glMatrixMode(GL_COLOR) ; glLoadMatrixf(1..16)");
        glMatrixMode(GL_MODELVIEW);
        pstep("glMatrixMode(GL_MODELVIEW) seul");
        glPushMatrix();
        pstep("glPushMatrix() (modèle-vue)");
        glLoadIdentity();
        pstep("glLoadIdentity()");
        glPushMatrix(); glTranslatef(0.5f, 0.25f, 0.125f);
        pstep("glPushMatrix() ; glTranslatef(.5 .25 .125)");
        glPopMatrix();
        pstep("glPopMatrix()");
        glPopMatrix();
        pstep("glPopMatrix()");
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
        pstep("projection et modèle-vue remises à l'identité");
        glFinish();
    } else if (!strcmp(scene, "tgprobe")) {
        /* Sonde TexGen (docs/re/etat-tcl.md §5). */
        static const float sp[4] = { 0.0625f, 0.125f, 0.1875f, 0.25f };
        static const float tp[4] = { 0.3125f, 0.375f, 0.4375f, 0.5f  };
        static const float rp[4] = { 0.5625f, 0.625f, 0.6875f, 0.75f };
        static const float qp[4] = { 0.8125f, 0.875f, 0.9375f, 1.0f  };
        static const float ep[4] = { 0.5f, 0.25f, 0.125f, 0.0625f };
        glClearColor(0, 0, 0, 1);
        pstep("référence");
        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        pstep("glTexGeni(S, MODE, GL_OBJECT_LINEAR)");
        glTexGenfv(GL_S, GL_OBJECT_PLANE, sp);
        pstep("glTexGenfv(S, OBJECT_PLANE, .0625 .125 .1875 .25)");
        glTexGenfv(GL_S, GL_EYE_PLANE, sp);
        pstep("glTexGenfv(S, EYE_PLANE, .0625 .125 .1875 .25) identité");
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glTranslatef(10, 20, 30);
        glTexGenfv(GL_S, GL_EYE_PLANE, ep);
        glPopMatrix();
        pstep("glTexGenfv(S, EYE_PLANE, .5 .25 .125 .0625) sous translate(10,20,30)");
        glEnable(GL_TEXTURE_GEN_S);
        pstep("glEnable(GL_TEXTURE_GEN_S)");
        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
        pstep("glTexGeni(S, MODE, GL_SPHERE_MAP)");
        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP_ARB);
        pstep("glTexGeni(S, MODE, GL_REFLECTION_MAP_ARB)");
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        pstep("glTexGeni(T, MODE, GL_OBJECT_LINEAR)");
        glTexGenfv(GL_T, GL_OBJECT_PLANE, tp);
        pstep("glTexGenfv(T, OBJECT_PLANE, .3125 .375 .4375 .5)");
        glTexGenfv(GL_T, GL_EYE_PLANE, tp);
        pstep("glTexGenfv(T, EYE_PLANE, .3125 .375 .4375 .5)");
        glEnable(GL_TEXTURE_GEN_T);
        pstep("glEnable(GL_TEXTURE_GEN_T)");
        glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        pstep("glTexGeni(R, MODE, GL_OBJECT_LINEAR)");
        glTexGenfv(GL_R, GL_OBJECT_PLANE, rp);
        pstep("glTexGenfv(R, OBJECT_PLANE, .5625 .625 .6875 .75)");
        glTexGenfv(GL_R, GL_EYE_PLANE, rp);
        pstep("glTexGenfv(R, EYE_PLANE, .5625 .625 .6875 .75)");
        glTexGeni(GL_Q, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        pstep("glTexGeni(Q, MODE, GL_OBJECT_LINEAR)");
        glTexGenfv(GL_Q, GL_OBJECT_PLANE, qp);
        pstep("glTexGenfv(Q, OBJECT_PLANE, .8125 .875 .9375 1)");
        glTexGenfv(GL_Q, GL_EYE_PLANE, qp);
        pstep("glTexGenfv(Q, EYE_PLANE, .8125 .875 .9375 1)");
        glEnable(GL_TEXTURE_GEN_R); glEnable(GL_TEXTURE_GEN_Q);
        pstep("glEnable(GL_TEXTURE_GEN_R) ; glEnable(GL_TEXTURE_GEN_Q)");
        glActiveTextureARB(GL_TEXTURE2_ARB);
        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_ARB);
        pstep("unité 2 : glTexGeni(S, MODE, GL_NORMAL_MAP_ARB)");
        glTexGenfv(GL_S, GL_OBJECT_PLANE, qp);
        pstep("unité 2 : glTexGenfv(S, OBJECT_PLANE, .8125 .875 .9375 1)");
        glEnable(GL_TEXTURE_GEN_S);
        pstep("unité 2 : glEnable(GL_TEXTURE_GEN_S)");
        glDisable(GL_TEXTURE_GEN_S);
        glActiveTextureARB(GL_TEXTURE0_ARB);
        glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T);
        glDisable(GL_TEXTURE_GEN_R); glDisable(GL_TEXTURE_GEN_Q);
        pstep("toutes les générations désactivées");
        glFinish();
    } else if (!strcmp(scene, "xformprobe")) {
        /* Sonde viewport / profondeur / faces / normalisation / plans de
           découpe / brouillard / points (docs/re/etat-tcl.md §3, §6, §7, §8, §9). */
        static const double p0[4] = { 0.125, 0.25, 0.375, 0.5 };
        static const double p3[4] = { 0.0625, 0.125, 0.1875, 0.25 };
        static const double p5[4] = { 0.5, 0.25, 0.125, 0.0625 };
        static const float fogc[4] = { 0.0625f, 0.1875f, 0.3125f, 0.4375f };
        static const float patt[3] = { 0.5f, 0.25f, 0.125f };
        pp_f  ppf  = (pp_f) gl_sym("glPointParameterfARB", "glPointParameterfEXT");
        pp_fv ppfv = (pp_fv)gl_sym("glPointParameterfvARB", "glPointParameterfvEXT");
        glClearColor(0, 0, 0, 1);
        pstep("référence");
        glViewport(3, 5, 17, 19);
        pstep("glViewport(3, 5, 17, 19)");
        glDepthRange(0.25, 0.75);
        pstep("glDepthRange(0.25, 0.75)");
        glDepthRange(0.0, 1.0);
        pstep("glDepthRange(0, 1)");
        glEnable(GL_CULL_FACE);
        pstep("glEnable(GL_CULL_FACE)");
        glCullFace(GL_FRONT);
        pstep("glCullFace(GL_FRONT)");
        glCullFace(GL_FRONT_AND_BACK);
        pstep("glCullFace(GL_FRONT_AND_BACK)");
        glCullFace(GL_BACK);
        pstep("glCullFace(GL_BACK)");
        glFrontFace(GL_CW);
        pstep("glFrontFace(GL_CW)");
        glFrontFace(GL_CCW);
        pstep("glFrontFace(GL_CCW)");
        glDisable(GL_CULL_FACE);
        pstep("glDisable(GL_CULL_FACE)");
        glEnable(GL_NORMALIZE);
        pstep("glEnable(GL_NORMALIZE)");
        glEnable(GL_RESCALE_NORMAL);
        pstep("glEnable(GL_RESCALE_NORMAL)");
        glDisable(GL_NORMALIZE); glDisable(GL_RESCALE_NORMAL);
        pstep("glDisable(GL_NORMALIZE) ; glDisable(GL_RESCALE_NORMAL)");
        glShadeModel(GL_FLAT);
        pstep("glShadeModel(GL_FLAT)");
        glShadeModel(GL_SMOOTH);
        pstep("glShadeModel(GL_SMOOTH)");
        glClipPlane(GL_CLIP_PLANE0, p0);
        pstep("glClipPlane(CLIP_PLANE0, .125 .25 .375 .5) identité");
        glEnable(GL_CLIP_PLANE0);
        pstep("glEnable(GL_CLIP_PLANE0)");
        glClipPlane(GL_CLIP_PLANE3, p3);
        pstep("glClipPlane(CLIP_PLANE3, .0625 .125 .1875 .25) identité");
        glEnable(GL_CLIP_PLANE3);
        pstep("glEnable(GL_CLIP_PLANE3)");
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glTranslatef(10, 20, 30);
        glClipPlane(GL_CLIP_PLANE5, p5);
        glPopMatrix();
        pstep("glClipPlane(CLIP_PLANE5, .5 .25 .125 .0625) sous translate(10,20,30)");
        glEnable(GL_CLIP_PLANE5);
        pstep("glEnable(GL_CLIP_PLANE5)");
        glDisable(GL_CLIP_PLANE0); glDisable(GL_CLIP_PLANE3);
        glDisable(GL_CLIP_PLANE5);
        pstep("tous les plans de découpe désactivés");
        glFogi(GL_FOG_MODE, GL_EXP2);
        pstep("glFogi(GL_FOG_MODE, GL_EXP2)");
        glFogf(GL_FOG_DENSITY, 0.1875f);
        pstep("glFogf(GL_FOG_DENSITY, .1875)");
        glFogf(GL_FOG_START, 0.3125f);
        pstep("glFogf(GL_FOG_START, .3125)");
        glFogf(GL_FOG_END, 0.4375f);
        pstep("glFogf(GL_FOG_END, .4375)");
        glFogfv(GL_FOG_COLOR, fogc);
        pstep("glFogfv(GL_FOG_COLOR, .0625 .1875 .3125 .4375)");
        glFogi(GL_FOG_MODE, GL_LINEAR);
        pstep("glFogi(GL_FOG_MODE, GL_LINEAR)");
        glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FOG_COORDINATE_EXT);
        pstep("glFogi(FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE)");
        glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FRAGMENT_DEPTH_EXT);
        pstep("glFogi(FOG_COORDINATE_SOURCE, GL_FRAGMENT_DEPTH)");
        glEnable(GL_FOG);
        pstep("glEnable(GL_FOG)");
        glDisable(GL_FOG);
        pstep("glDisable(GL_FOG)");
        glPointSize(5.5f);
        pstep("glPointSize(5.5)");
        if (ppfv) ppfv(GL_POINT_DISTANCE_ATTENUATION_ARB, patt);
        pstep(ppfv ? "glPointParameterfv(POINT_DISTANCE_ATTENUATION, .5 .25 .125)"
                   : "(glPointParameterfv absent)");
        if (ppf) ppf(GL_POINT_SIZE_MIN_ARB, 0.75f);
        pstep(ppf ? "glPointParameterf(POINT_SIZE_MIN, .75)" : "(absent)");
        if (ppf) ppf(GL_POINT_SIZE_MAX_ARB, 19.5f);
        pstep(ppf ? "glPointParameterf(POINT_SIZE_MAX, 19.5)" : "(absent)");
        if (ppf) ppf(GL_POINT_FADE_THRESHOLD_SIZE_ARB, 3.25f);
        pstep(ppf ? "glPointParameterf(POINT_FADE_THRESHOLD_SIZE, 3.25)" : "(absent)");
        glLineWidth(3.25f);
        pstep("glLineWidth(3.25)");
        glPolygonMode(GL_FRONT, GL_LINE);
        pstep("glPolygonMode(GL_FRONT, GL_LINE)");
        glPolygonMode(GL_BACK, GL_POINT);
        pstep("glPolygonMode(GL_BACK, GL_POINT)");
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glViewport(0, 0, W, H);
        pstep("glPolygonMode(FRONT_AND_BACK, GL_FILL) ; glViewport(0,0,W,H)");
        glFinish();
    } else if (!strcmp(scene, "state")) {
        /* Sonde d'état : un réglage GL à la fois, suivi d'un glClear que le
           plugin traceur (POMPPC_GLTRACE_STATE=1) vide en entier. Les valeurs
           sont choisies pour être reconnaissables dans le vidage. */
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 1 base */
        glClearColor(0.125f, 0.25f, 0.375f, 0.5f); glClearDepth(0.625);
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 2 */
        glEnable(GL_DEPTH_TEST); glClear(GL_COLOR_BUFFER_BIT);          /* 3 */
        glDepthFunc(GL_GEQUAL); glClear(GL_COLOR_BUFFER_BIT);           /* 4 */
        glDepthMask(GL_FALSE); glClear(GL_COLOR_BUFFER_BIT);            /* 5 */
        glColorMask(1, 0, 1, 0); glClear(GL_COLOR_BUFFER_BIT);          /* 6 */
        glEnable(GL_BLEND); glClear(GL_COLOR_BUFFER_BIT);               /* 7 */
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 8 */
        glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.3125f);
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 9 */
        glEnable(GL_SCISSOR_TEST); glScissor(3, 5, 17, 19);
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 10 */
        glShadeModel(GL_FLAT); glClear(GL_COLOR_BUFFER_BIT);            /* 11 */
        glEnable(GL_CULL_FACE); glCullFace(GL_FRONT);
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 12 */
        glEnable(GL_TEXTURE_2D); glClear(GL_COLOR_BUFFER_BIT);          /* 13 */
        glEnable(GL_FOG); glClear(GL_COLOR_BUFFER_BIT);                 /* 14 */
        glEnable(GL_STENCIL_TEST); glClear(GL_COLOR_BUFFER_BIT);        /* 15 */
        glEnable(GL_COLOR_LOGIC_OP); glClear(GL_COLOR_BUFFER_BIT);      /* 16 */
        glDisable(GL_DITHER); glClear(GL_COLOR_BUFFER_BIT);             /* 17 */
        glEnable(GL_POLYGON_STIPPLE); glClear(GL_COLOR_BUFFER_BIT);     /* 18 */
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 19 */
        glEnable(GL_POLYGON_OFFSET_FILL); glClear(GL_COLOR_BUFFER_BIT); /* 20 */
        glEnable(GL_POLYGON_SMOOTH); glClear(GL_COLOR_BUFFER_BIT);      /* 21 */
        glViewport(7, 9, 23, 29); glClear(GL_COLOR_BUFFER_BIT);         /* 22 */
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 23 */
        glDisable(GL_TEXTURE_2D); glEnable(GL_TEXTURE_1D);
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 24 */
        glDisable(GL_TEXTURE_1D); glEnable(GL_TEXTURE_RECTANGLE_EXT);
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 25 */
        glDisable(GL_TEXTURE_RECTANGLE_EXT); glEnable(GL_TEXTURE_3D);
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 26 */
        glDisable(GL_TEXTURE_3D); glEnable(GL_TEXTURE_CUBE_MAP);
        glClear(GL_COLOR_BUFFER_BIT);                                   /* 27 */
        glFinish();
    } else if (!strcmp(scene, "depthrt")) {
        /* Aller-retour de la PROFONDEUR entre l'hôte et le tampon invité :
           on dessine (hôte), puis glReadPixels force le repli, donc une
           relecture de la profondeur dans le tampon d'Apple, que glReadPixels
           reconvertit en flottant. La valeur imprimée doit être celle de la
           géométrie, avec comme sans tampon de stencil. */
        float d[4];
        glClearColor(0, 0, 0.25f, 1);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glBegin(GL_QUADS);                 /* z objet 0.2 -> z fenêtre 0.4 */
        glColor3f(1, 0.5f, 0);
        glVertex3f(0, 0, 0.2f); glVertex3f(W, 0, 0.2f);
        glVertex3f(W, H / 2.0f, 0.2f); glVertex3f(0, H / 2.0f, 0.2f);
        glEnd();
        glFinish();
        /* glReadPixels compte y depuis le BAS : la ligne 1 de GL est la
           dernière ligne du tampon (effacée), H−2 la première (dessinée). */
        glReadPixels(1, 1, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d[0]);
        glReadPixels(1, H - 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d[1]);
        printf("  profondeur relue : dessinée %.6f (attendu 0.400000), "
               "effacée %.6f (attendu 1.000000)\n", d[1], d[0]);
        if (!(d[1] > 0.398f && d[1] < 0.402f)) failures++;
        if (!(d[0] > 0.998f)) failures++;
        /* Sens INVERSE : le logiciel d'Apple écrit la profondeur, puis l'hôte
           doit la relire pour ses propres tests. On sort du domaine par
           glLogicOp, on dessine, on y revient. */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_COLOR_LOGIC_OP); glLogicOp(GL_COPY);
        glBegin(GL_QUADS);                 /* logiciel, z fenêtre 0.4 */
        glColor3f(1, 0, 0);
        glVertex3f(0, 0, 0.2f); glVertex3f(W, 0, 0.2f);
        glVertex3f(W, H, 0.2f); glVertex3f(0, H, 0.2f);
        glEnd();
        glDisable(GL_COLOR_LOGIC_OP);
        glBegin(GL_QUADS);                 /* hôte, PLUS LOIN : doit être rejeté */
        glColor3f(0, 1, 0);
        glVertex3f(0, 0, -0.2f); glVertex3f(W, 0, -0.2f);
        glVertex3f(W, H / 2.0f, -0.2f); glVertex3f(0, H / 2.0f, -0.2f);
        glEnd();
        glBegin(GL_QUADS);                 /* hôte, PLUS PRÈS : doit passer */
        glColor3f(0, 0, 1);
        glVertex3f(0, H / 2.0f, 0.6f); glVertex3f(W, H / 2.0f, 0.6f);
        glVertex3f(W, H, 0.6f); glVertex3f(0, H, 0.6f);
        glEnd();
        glFinish();
        {
            unsigned long a = px(W / 2, 2), b = px(W / 2, H - 2);
            printf("  %s profondeur logiciel -> hôte : plus loin rejeté (%06lx)\n",
                   a == 0xFF0000 ? "ok  " : "FAIL", a);
            if (a != 0xFF0000) failures++;
            printf("  %s profondeur logiciel -> hôte : plus près accepté (%06lx)\n",
                   b == 0x0000FF ? "ok  " : "FAIL", b);
            if (b != 0x0000FF) failures++;
        }
        glDisable(GL_DEPTH_TEST);
    } else if (!strcmp(scene, "lit")) {
        /* Éclairage : directionnelle, ponctuelle avec atténuation, spot,
           spéculaire, color material, deux faces et normalize sous une matrice
           d'échelle. Six cases de 64×64 ; chaque case est un quadrilatère plan
           dont la normale varie, pour que l'angle d'incidence change d'une case
           à l'autre. Les pixels témoins sont STRUCTURELS (un dégradé, un ordre
           entre deux points) : l'exactitude, elle, est jugée par la comparaison
           d'image entière avec le rendu d'Apple. */
        float amb[4] = { 0.1f, 0.1f, 0.1f, 1 };
        int cx, cy, tris = getenv("GLTEST_LIT_TRIS") != 0;
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_LIGHTING);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
        for (cy = 0; cy < 2; cy++) for (cx = 0; cx < 3; cx++) {
            int c = cy * 3 + cx;
            float x0 = cx * 64.0f, y0 = cy * 64.0f;
            float dif[4] = { 0.9f, 0.7f, 0.4f, 1 }, spc[4] = { 1, 1, 1, 1 };
            float zero[4] = { 0, 0, 0, 1 };
            float lamb[4] = { 0.05f, 0.05f, 0.05f, 1 };
            float pos[4], dir[3] = { 0, 0, -1 };
            int i, j;
            glDisable(GL_LIGHT0); glDisable(GL_LIGHT1);
            glDisable(GL_COLOR_MATERIAL);
            glDisable(GL_NORMALIZE);
            glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 0);
            glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 0);
            glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, lamb);
            glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, dif);
            glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, zero);
            glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero);
            glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 0);
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glLightfv(GL_LIGHT0, GL_AMBIENT, lamb);
            glLightfv(GL_LIGHT0, GL_DIFFUSE, spc);
            glLightfv(GL_LIGHT0, GL_SPECULAR, zero);
            glLightf(GL_LIGHT0, GL_CONSTANT_ATTENUATION, 1);
            glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 0);
            glLightf(GL_LIGHT0, GL_QUADRATIC_ATTENUATION, 0);
            glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 180);
            glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, 0);
            pos[0] = 0.4f; pos[1] = 0.3f; pos[2] = 1; pos[3] = 0;
            switch (c) {
            case 0:                                   /* directionnelle pure */
                break;
            case 1:                                   /* ponctuelle + atténuation */
                pos[0] = x0 + 32; pos[1] = y0 + 32; pos[2] = 40; pos[3] = 1;
                glLightf(GL_LIGHT0, GL_CONSTANT_ATTENUATION, 0.25f);
                glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 0.01f);
                glLightf(GL_LIGHT0, GL_QUADRATIC_ATTENUATION, 0.0003f);
                break;
            case 2:                                   /* spot */
                pos[0] = x0 + 32; pos[1] = y0 + 32; pos[2] = 50; pos[3] = 1;
                glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 25);
                glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, 8);
                glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, dir);
                break;
            case 3:                                   /* spéculaire, observateur local */
                pos[0] = x0 + 32; pos[1] = y0 + 32; pos[2] = 60; pos[3] = 1;
                glLightfv(GL_LIGHT0, GL_SPECULAR, spc);
                glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spc);
                glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 24);
                glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 1);
                break;
            case 4:                                   /* color material */
                glEnable(GL_COLOR_MATERIAL);
                glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
                break;
            default:                                  /* deux faces + normalize */
                glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);
                glEnable(GL_NORMALIZE);
                glMaterialfv(GL_BACK, GL_DIFFUSE, spc);
                break;
            }
            glLightfv(GL_LIGHT0, GL_POSITION, pos);
            glEnable(GL_LIGHT0);
            if (c == 5)
                glScalef(3, 3, 3);                    /* normales à normaliser */
            /* Maillage 8×8 : les normales varient, l'éclairage aussi.
               GLTEST_LIT_TRIS=1 dessine des TRIANGLES au lieu de quads : un
               quadrilatère n'a pas de découpe définie par OpenGL, et quand la
               valeur aux sommets varie brutalement (le bord d'un cône de spot),
               deux rendus qui coupent la diagonale autrement donnent des images
               différentes — c'est la seule divergence > 2/255 mesurée sur cette
               scène, et elle disparaît avec des triangles. */
            glBegin(tris ? GL_TRIANGLES : GL_QUADS);
            for (j = 0; j < 8; j++) for (i = 0; i < 8; i++) {
                float u = (i - 3.5f) / 8.0f, v = (j - 3.5f) / 8.0f;
                float s = c == 5 ? (1 / 3.0f) : 1.0f;
                float ax = x0 + i * 8.0f, ay = y0 + j * 8.0f;
                float q[4][2];
                int k;
                static const int oq[4] = { 0, 1, 2, 3 }, ot[6] = { 0, 1, 2, 0, 2, 3 };
                glNormal3f(u, v, 0.8f);
                glColor3f(0.9f, 0.3f + i / 16.0f, 0.2f + j / 16.0f);
                if (c == 5) {           /* face arrière : sens inversé */
                    q[0][0] = ax;     q[0][1] = ay;
                    q[1][0] = ax;     q[1][1] = ay + 8;
                    q[2][0] = ax + 8; q[2][1] = ay + 8;
                    q[3][0] = ax + 8; q[3][1] = ay;
                } else {
                    q[0][0] = ax;     q[0][1] = ay;
                    q[1][0] = ax + 8; q[1][1] = ay;
                    q[2][0] = ax + 8; q[2][1] = ay + 8;
                    q[3][0] = ax;     q[3][1] = ay + 8;
                }
                for (k = 0; k < (tris ? 6 : 4); k++) {
                    int z = tris ? ot[k] : oq[k];
                    glVertex3f(q[z][0] * s, q[z][1] * s, 0);
                }
            }
            glEnd();
            if (c == 5)
                glLoadIdentity();
        }
        glDisable(GL_LIGHTING);
        glFinish();
        {   /* témoins structurels, vrais des deux côtés */
            unsigned long a = px(20, 20), b = px(44, 44);
            printf("  %s directionnelle : dégradé (%06lx -> %06lx)\n",
                   a != b ? "ok  " : "FAIL", a, b);
            if (a == b) failures++;
            a = px(96, 32); b = px(70, 6);
            printf("  %s ponctuelle : centre plus clair que le bord (%06lx > %06lx)\n",
                   (a >> 16) > (b >> 16) ? "ok  " : "FAIL", a, b);
            if (!((a >> 16) > (b >> 16))) failures++;
            a = px(160, 32); b = px(134, 6);
            printf("  %s spot : cône éclairé, dehors sombre (%06lx > %06lx)\n",
                   (a >> 16) > (b >> 16) ? "ok  " : "FAIL", a, b);
            if (!((a >> 16) > (b >> 16))) failures++;
            a = px(32, 96);
            printf("  %s spéculaire : point brillant (%06lx)\n",
                   (a & 255) > 40 ? "ok  " : "FAIL", a);
            if (!((a & 255) > 40)) failures++;
            a = px(96, 96); b = px(120, 120);
            printf("  %s color material : couleur suivie (%06lx != %06lx)\n",
                   a != b ? "ok  " : "FAIL", a, b);
            if (a == b) failures++;
            a = px(160, 96);
            printf("  %s deux faces + normalize : face arrière éclairée (%06lx)\n",
                   (a >> 16) > 20 ? "ok  " : "FAIL", a);
            if (!((a >> 16) > 20)) failures++;
        }
    } else if (!strcmp(scene, "texgen")) {
        /* OBJECT_LINEAR, EYE_LINEAR, SPHERE_MAP, et une matrice de texture. */
        unsigned char img[32 * 32 * 4];
        GLuint id;
        int x, y, c;
        /* dégradé lisse : un écart de coordonnée se voit alors PROPORTIONNEL
           dans l'image, au lieu d'être amplifié par un saut de texel */
        for (y = 0; y < 32; y++) for (x = 0; x < 32; x++) {
            img[(y * 32 + x) * 4 + 0] = (unsigned char)(x * 8);
            img[(y * 32 + x) * 4 + 1] = (unsigned char)(y * 8);
            img[(y * 32 + x) * 4 + 2] = (unsigned char)(255 - x * 4 - y * 4);
            img[(y * 32 + x) * 4 + 3] = 255;
        }
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
        glEnable(GL_TEXTURE_2D);
        /* GL_SPHERE_MAP part de la normale d'ŒIL : sans normalisation, chaque
           rendu est libre de sa propre convention. On normalise pour que la
           comparaison porte sur la formule, pas sur la longueur des normales. */
        glEnable(GL_NORMALIZE);
        glClearColor(0, 0, 0.25f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        for (c = 0; c < 4; c++) {
            float sp[4] = { 1 / 32.0f, 0, 0, 0 }, tp[4] = { 0, 1 / 32.0f, 0, 0 };
            float x0 = (c % 2) * 64.0f, y0 = (c / 2) * 64.0f;
            glMatrixMode(GL_TEXTURE); glLoadIdentity(); glMatrixMode(GL_MODELVIEW);
            glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T);
            if (c == 0) {                  /* OBJECT_LINEAR */
                glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
                glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
                glTexGenfv(GL_S, GL_OBJECT_PLANE, sp);
                glTexGenfv(GL_T, GL_OBJECT_PLANE, tp);
                glEnable(GL_TEXTURE_GEN_S); glEnable(GL_TEXTURE_GEN_T);
            } else if (c == 1) {           /* EYE_LINEAR sous une modèle-vue */
                glLoadIdentity(); glTranslatef(17, 23, 0);
                glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
                glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
                glTexGenfv(GL_S, GL_EYE_PLANE, sp);
                glTexGenfv(GL_T, GL_EYE_PLANE, tp);
                glEnable(GL_TEXTURE_GEN_S); glEnable(GL_TEXTURE_GEN_T);
                glLoadIdentity();
            } else if (c == 2) {           /* SPHERE_MAP */
                glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
                glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
                glEnable(GL_TEXTURE_GEN_S); glEnable(GL_TEXTURE_GEN_T);
            } else {                       /* coordonnées explicites + matrice */
                glMatrixMode(GL_TEXTURE);
                glLoadIdentity(); glTranslatef(0.25f, 0.5f, 0); glScalef(2, 3, 1);
                glMatrixMode(GL_MODELVIEW);
            }
            glBegin(GL_QUADS);
            {
                int i, j;
                for (j = 0; j < 4; j++) for (i = 0; i < 4; i++) {
                    float ax = x0 + i * 16.0f, ay = y0 + j * 16.0f;
                    float n = (i + j) / 6.0f - 0.5f;
                    glNormal3f(n, n, 0.7f);
                    glTexCoord2f(i / 4.0f, j / 4.0f); glVertex2f(ax, ay);
                    glTexCoord2f((i + 1) / 4.0f, j / 4.0f); glVertex2f(ax + 16, ay);
                    glTexCoord2f((i + 1) / 4.0f, (j + 1) / 4.0f); glVertex2f(ax + 16, ay + 16);
                    glTexCoord2f(i / 4.0f, (j + 1) / 4.0f); glVertex2f(ax, ay + 16);
                }
            }
            glEnd();
        }
        glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T);
        glDisable(GL_NORMALIZE);
        glDisable(GL_TEXTURE_2D);
        glFinish();
        {
            int c2;
            for (c2 = 0; c2 < 4; c2++) {
                static const char *nm[4] = { "OBJECT_LINEAR", "EYE_LINEAR",
                                             "SPHERE_MAP", "matrice de texture" };
                int x0 = (c2 % 2) * 64, y0 = (c2 / 2) * 64, ok = 0, i;
                unsigned long first = px(x0 + 2, y0 + 2);
                for (i = 3; i < 60 && !ok; i++)
                    if (px(x0 + i, y0 + i) != first) ok = 1;
                printf("  %s texgen %-20s : motif présent (%06lx)\n",
                       ok ? "ok  " : "FAIL", nm[c2], first);
                if (!ok) failures++;
            }
        }
    } else if (!strcmp(scene, "clip")) {
        /* Plan de découpe utilisateur, et un triangle qui traverse le plan
           PROCHE : c'est le découpage que GLEngine ne fait plus. */
        double eq[4] = { -1, 0, 0, 40 };          /* garde x <= 40 (coordonnées œil) */
        glClearColor(0, 0, 0.25f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glClipPlane(GL_CLIP_PLANE0, eq);
        glEnable(GL_CLIP_PLANE0);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0.2f, 0.2f);
        glVertex2f(4, 4); glVertex2f(120, 4); glVertex2f(4, 60);
        glEnd();
        glDisable(GL_CLIP_PLANE0);
        /* triangle qui traverse le plan proche, en perspective */
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glFrustum(-1, 1, -1, 1, 1, 20);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glViewport(0, H / 2, W, H / 2);
        glBegin(GL_TRIANGLES);
        glColor3f(0.2f, 1, 0.2f);
        glVertex3f(-0.6f, -0.6f, -6.0f);
        glVertex3f(0.6f, -0.6f, -6.0f);
        glVertex3f(0.0f, 0.9f, 0.5f);          /* DERRIÈRE l'œil */
        glEnd();
        glViewport(0, 0, W, H);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glOrtho(0, W, H, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glFinish();
        {
            unsigned long a = px(10, 10), b = px(70, 10);
            int i, green = 0;
            printf("  %s découpe : gardé à gauche (%06lx)\n",
                   (a >> 16) > 240 ? "ok  " : "FAIL", a);
            if (!((a >> 16) > 240)) failures++;
            printf("  %s découpe : coupé à droite (%06lx)\n",
                   (b >> 16) < 8 && (b & 255) > 48 ? "ok  " : "FAIL", b);
            if (!((b >> 16) < 8 && (b & 255) > 48)) failures++;
            /* le viewport du second dessin est la moitié HAUTE en repère GL,
               c'est-à-dire les lignes 0..H/2−1 du tampon */
            for (i = 0; i < H / 2; i++)
                if (((px(W / 2, i) >> 8) & 255) > 180) green++;
            printf("  %s plan proche : triangle dessiné (%d lignes vertes)\n",
                   green > 4 ? "ok  " : "FAIL", green);
            if (green <= 4) failures++;
        }
    } else if (!strcmp(scene, "fogz")) {
        /* Brouillard LINEAR puis EXP en perspective, calculé à partir de |z œil|
           (GL_FRAGMENT_DEPTH) : c'est l'hôte qui le fait sur le chemin brut. */
        float fc[4] = { 0.2f, 0.4f, 0.8f, 1 };
        int pass;
        glClearColor(0.2f, 0.4f, 0.8f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_FOG);
        glFogfv(GL_FOG_COLOR, fc);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glFrustum(-1, 1, -1, 1, 1, 40);
        glMatrixMode(GL_MODELVIEW);
        for (pass = 0; pass < 2; pass++) {
            int i;
            if (pass == 0) {
                glFogi(GL_FOG_MODE, GL_LINEAR);
                glFogf(GL_FOG_START, 4);
                glFogf(GL_FOG_END, 28);
            } else {
                glFogi(GL_FOG_MODE, GL_EXP);
                glFogf(GL_FOG_DENSITY, 0.08f);
            }
            glViewport(0, pass ? 0 : H / 2, W, H / 2);
            glLoadIdentity();
            glBegin(GL_QUADS);
            for (i = 0; i < 12; i++) {
                float z = -3.0f - i * 2.0f, s = 0.9f;
                glColor3f(1, 0.9f, 0.1f);
                glVertex3f(-s, -0.9f, z); glVertex3f(s, -0.9f, z);
                glVertex3f(s, 0.9f, z);   glVertex3f(-s, 0.9f, z);
            }
            glEnd();
        }
        glDisable(GL_FOG);
        glViewport(0, 0, W, H);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glOrtho(0, W, H, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glFinish();
        {
            /* le quadrilatère le plus proche est jaune, le plus lointain est
               noyé dans la couleur du brouillard (vrai des deux côtés) */
            unsigned long near_l = px(W / 2, H / 4), near_e = px(W / 2, 3 * H / 4);
            printf("  %s LINEAR : le proche reste jaune (%06lx)\n",
                   (near_l >> 16) > 150 ? "ok  " : "FAIL", near_l);
            if (!((near_l >> 16) > 150)) failures++;
            printf("  %s EXP : le proche reste jaune (%06lx)\n",
                   (near_e >> 16) > 120 ? "ok  " : "FAIL", near_e);
            if (!((near_e >> 16) > 120)) failures++;
        }
    } else if (!strcmp(scene, "bigstrip")) {
        /* Plus de sommets qu'un lot de GLEngine : une bande de 1000 sommets, un
           éventail, un polygone et une boucle de lignes de plus de 192 sommets.
           C'est le cas où GLEngine doit COUPER la primitive : si la coupure
           perdait ou répétait un sommet, l'image le montrerait. */
        int i, n = 1000;
        double PI = 3.14159265358979323846;
        glClearColor(0, 0, 0.25f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_TRIANGLE_STRIP);
        for (i = 0; i < n; i++) {
            float t = i / (float)(n - 1), x = 2 + t * (W - 4);
            float y = (i & 1) ? 4.0f : 40.0f;
            glColor3f(t, 1 - t, (i & 1) ? 1.0f : 0.0f);
            glVertex2f(x, y);
        }
        glEnd();
        glBegin(GL_TRIANGLE_FAN);
        glColor3f(1, 1, 0); glVertex2f(W / 2.0f, 80);
        for (i = 0; i <= 300; i++) {
            double a = i * 2 * PI / 300;
            glColor3f((float)(i % 3) / 2.0f, 0.5f, 1);
            glVertex2f(W / 2.0f + 34 * (float)__builtin_cos(a),
                       80 + 34 * (float)__builtin_sin(a));
        }
        glEnd();
        glBegin(GL_POLYGON);
        for (i = 0; i < 250; i++) {
            double a = i * 2 * PI / 250;
            glColor3f(0.2f, 1, 0.4f);
            glVertex2f(W / 2.0f + 20 * (float)__builtin_cos(a),
                       160 + 20 * (float)__builtin_sin(a));
        }
        glEnd();
        glBegin(GL_LINE_LOOP);
        for (i = 0; i < 400; i++) {
            double a = i * 2 * PI / 400;
            glColor3f(1, 1, 1);
            glVertex2f(W / 2.0f + 45 * (float)__builtin_cos(a),
                       160 + 45 * (float)__builtin_sin(a));
        }
        glEnd();
        glFinish();
        {
            unsigned long a = px(4, 20), b = px(W - 6, 20);
            /* le dégradé va du vert (début) au rouge (fin) : si la coupure de
               GLEngine perdait la fin de la bande, b serait le fond */
            printf("  %s bande : début vert (%06lx)\n",
                   ((a >> 8) & 255) > 200 ? "ok  " : "FAIL", a);
            if (!(((a >> 8) & 255) > 200)) failures++;
            printf("  %s bande : fin rouge atteinte (%06lx)\n",
                   (b >> 16) > 200 ? "ok  " : "FAIL", b);
            if (!((b >> 16) > 200)) failures++;
            a = px(W / 2, 80);
            printf("  %s éventail : centre jaune (%06lx)\n",
                   (a >> 16) > 200 && ((a >> 8) & 255) > 180 ? "ok  " : "FAIL", a);
            if (!((a >> 16) > 200 && ((a >> 8) & 255) > 180)) failures++;
        }
        check("polygone : centre", W / 2, 160, 0x33FF66);
        {
            unsigned long a = px(W / 2, 160 - 45);
            printf("  %s boucle de lignes : fermée (%06lx)\n",
                   a == 0xFFFFFF ? "ok  " : "FAIL", a);
            if (a != 0xFFFFFF) failures++;
        }
    } else if (!strcmp(scene, "dlist")) {
        /* Liste d'affichage et glDrawElements : GLEngine les déroule tous les
           deux dans Begin/EndPrimitiveBuffer. */
        static const float v[8 * 3] = {
            8, 8, 0,   56, 8, 0,   56, 56, 0,   8, 56, 0,
            72, 8, 0,  120, 8, 0,  120, 56, 0,  72, 56, 0 };
        static const float c[8 * 3] = {
            1, 0, 0,  0, 1, 0,  0, 0, 1,  1, 1, 0,
            1, 0, 1,  0, 1, 1,  1, 1, 1,  0.5f, 0.5f, 0.5f };
        static const unsigned short idx[12] = { 0,1,2, 0,2,3, 4,5,6, 4,6,7 };
        GLuint list;
        glClearColor(0, 0, 0.25f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        list = glGenLists(1);
        glNewList(list, GL_COMPILE);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0.5f, 0); glVertex2f(8, 72); glVertex2f(56, 72); glVertex2f(8, 120);
        glColor3f(0, 0.5f, 1); glVertex2f(72, 72); glVertex2f(120, 72); glVertex2f(72, 120);
        glEnd();
        glEndList();
        glCallList(list);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, v);
        glColorPointer(3, GL_FLOAT, 0, c);
        glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_SHORT, idx);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDeleteLists(list, 1);
        glFinish();
        {
            unsigned long a = px(12, 76), b = px(76, 76);
            printf("  %s liste d'affichage : orange (%06lx)\n",
                   (a >> 16) > 240 && (a & 255) < 8 ? "ok  " : "FAIL", a);
            if (!((a >> 16) > 240 && (a & 255) < 8)) failures++;
            printf("  %s liste d'affichage (2) : bleu (%06lx)\n",
                   (b & 255) > 240 && (b >> 16) < 8 ? "ok  " : "FAIL", b);
            if (!((b & 255) > 240 && (b >> 16) < 8)) failures++;
        }
        {
            unsigned long a = px(30, 30), b = px(96, 30);
            printf("  %s glDrawElements : deux quads (%06lx / %06lx)\n",
                   (a && b) ? "ok  " : "FAIL", a, b);
            if (!(a && b)) failures++;
        }
    } else if (!strcmp(scene, "mixte")) {
        /* Alternance, dans la MÊME image et avec la même profondeur, de dessins
           dans le domaine et hors domaine (glPolygonMode(GL_LINE) et un
           glDrawPixels) : le repli par lot d'état doit donner une image exacte. */
        unsigned char pix[16 * 16 * 3];
        int i, k;
        for (i = 0; i < 16 * 16; i++) {
            pix[i * 3 + 0] = (unsigned char)(i & 255);
            pix[i * 3 + 1] = 200;
            pix[i * 3 + 2] = (unsigned char)(255 - (i & 255));
        }
        glClearColor(0, 0, 0.25f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        for (k = 0; k < 4; k++) {
            float y0 = k * 32.0f;
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);      /* dans le domaine */
            glBegin(GL_TRIANGLES);
            glColor3f(1, 0.3f, 0.1f);
            glVertex3f(4, y0 + 4, 0.2f); glVertex3f(60, y0 + 4, 0.2f);
            glVertex3f(4, y0 + 28, 0.2f);
            glEnd();
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);      /* HORS domaine */
            glBegin(GL_TRIANGLES);
            glColor3f(0.2f, 1, 0.4f);
            glVertex3f(68, y0 + 4, 0.1f); glVertex3f(124, y0 + 4, 0.1f);
            glVertex3f(68, y0 + 28, 0.1f);
            glEnd();
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);      /* de nouveau dedans */
            glBegin(GL_QUADS);
            glColor3f(0.9f, 0.9f, 0.2f);
            glVertex3f(70, y0 + 16, 0.5f); glVertex3f(122, y0 + 16, 0.5f);
            glVertex3f(122, y0 + 26, 0.5f); glVertex3f(70, y0 + 26, 0.5f);
            glEnd();
        }
        glRasterPos2f(4, 130);                              /* HORS domaine */
        glDrawPixels(16, 16, GL_RGB, GL_UNSIGNED_BYTE, pix);
        glBegin(GL_TRIANGLES);                              /* et de nouveau dedans */
        glColor3f(0.4f, 0.4f, 1);
        glVertex3f(40, 116, 0.3f); glVertex3f(124, 116, 0.3f); glVertex3f(40, 156, 0.3f);
        glEnd();
        glDisable(GL_DEPTH_TEST);
        glFinish();
        {
            unsigned long a = px(10, 10);
            printf("  %s rempli (domaine) : rouge (%06lx)\n",
                   (a >> 16) > 240 && ((a >> 8) & 255) < 100 ? "ok  " : "FAIL", a);
            if (!((a >> 16) > 240 && ((a >> 8) & 255) < 100)) failures++;
            a = px(100, 21);
            printf("  %s bande jaune devant l'arête (%06lx)\n",
                   (a >> 16) > 200 && (a & 255) < 100 ? "ok  " : "FAIL", a);
            if (!((a >> 16) > 200 && (a & 255) < 100)) failures++;
            a = px(96, 4);
            printf("  %s polygonmode ligne : arête tracée (%06lx)\n",
                   a != 0x000040 ? "ok  " : "FAIL", a);
            if (a == 0x000040) failures++;
            a = px(60, 130);
            printf("  %s triangle après glDrawPixels (%06lx)\n",
                   (a & 255) > 200 ? "ok  " : "FAIL", a);
            if (!((a & 255) > 200)) failures++;
        }
    } else if (!strcmp(scene, "fusion")) {
        /* BEAUCOUP de primitives COURTES et CONSÉCUTIVES : c'est exactement ce
           que Marble Blast envoie (7,7 sommets par dessin), et ce que le plugin
           recolle en un seul DRAW_RAW indexé (POMPPC_GL_MERGE=0 pour couper).
           La scène vérifie que la conversion en triangles indexés ne change
           RIEN : ombrage lisse, ombrage PLAT (le sommet provoquant n'est pas le
           même selon le mode), alternance d'orientation dans un ruban sous
           GL_CULL_FACE, ordre de dessin sous mélange, et coupures de la fusion
           par un changement d'état. Taille attendue : 160x160.
           Les couleurs unies passent par glColor3ub : elles font l'aller-retour
           8 bits sans arrondi, donc les pixels témoins sont exacts. */
        int c, i;
        unsigned char timg[8 * 8 * 4];
        GLuint tid = 0;
        float xm;
#define FX0(k) ((float)((k) * 20 + 2))
#define FX1(k) ((float)((k) * 20 + 18))
        for (i = 0; i < 8 * 8; i++) {
            timg[i * 4 + 0] = 255; timg[i * 4 + 1] = 0;
            timg[i * 4 + 2] = 255; timg[i * 4 + 3] = 255;
        }
        glGenTextures(1, &tid);
        glBindTexture(GL_TEXTURE_2D, tid);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, timg);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        /* 64/255 plutôt que 0,25 : 0,25 × 255 = 63,75, et le rendu d'Apple
           tronque là où le GPU de l'hôte arrondit — un écart de 1/255 qui
           interdirait des pixels témoins exacts. */
        glClearColor(0, 0, 64.0f / 255.0f, 1);  /* fond 0x000040 */
        glClear(GL_COLOR_BUFFER_BIT);

        /* ── bande 1 (y 2..18) : huit RUBANS courts, ombrage lisse ── */
        for (c = 0; c < 8; c++) {
            float x0 = FX0(c), x1 = FX1(c);
            glBegin(GL_TRIANGLE_STRIP);
            if (c < 6) {                        /* couleur unie : pixel témoin exact */
                glColor3ub(FCOL(c));
                glVertex2f(x0, 2); glVertex2f(x0, 18);
                glVertex2f(x1, 2); glVertex2f(x1, 18);
            } else {                            /* dégradé : jugé par le diff */
                glColor3ub(255, 0, 0); glVertex2f(x0, 2);
                glColor3ub(0, 255, 0); glVertex2f(x0, 18);
                glColor3ub(0, 0, 255); glVertex2f(x1, 2);
                glColor3ub(255, 255, 0); glVertex2f(x1, 18);
            }
            glEnd();
        }
        /* ── bande 2 (y 20..36) : huit ÉVENTAILS courts ── */
        for (c = 0; c < 8; c++) {
            float x0 = FX0(c), x1 = FX1(c);
            glBegin(GL_TRIANGLE_FAN);
            glColor3ub(FCOL((c + 2) % 6));
            glVertex2f(x0, 20); glVertex2f(x0, 36);
            glVertex2f(x1, 36); glVertex2f(x1, 20);
            glEnd();
        }
        /* ── bande 3 (y 38..54) : quatre QUADS puis quatre BANDES DE QUADS ── */
        for (c = 0; c < 4; c++) {
            float x0 = FX0(c), x1 = FX1(c);
            glBegin(GL_QUADS);
            glColor3ub(FCOL((c + 4) % 6));
            glVertex2f(x0, 38); glVertex2f(x0, 54);
            glVertex2f(x1, 54); glVertex2f(x1, 38);
            glEnd();
        }
        for (c = 4; c < 8; c++) {
            float x0 = FX0(c), x1 = FX1(c);
            xm = (x0 + x1) / 2;
            glBegin(GL_QUAD_STRIP);
            glColor3ub(FCOL((c + 1) % 6));
            glVertex2f(x0, 38); glVertex2f(x0, 54);
            glVertex2f(xm, 38); glVertex2f(xm, 54);
            glVertex2f(x1, 38); glVertex2f(x1, 54);
            glEnd();
        }
        /* ── bande 4 (y 56..72) : huit POLYGONES convexes de 5 sommets ── */
        for (c = 0; c < 8; c++) {
            float x0 = FX0(c), x1 = FX1(c);
            xm = (x0 + x1) / 2;
            glBegin(GL_POLYGON);
            glColor3ub(FCOL((c + 3) % 6));
            glVertex2f(x0, 58); glVertex2f(xm, 56); glVertex2f(x1, 58);
            glVertex2f(x1, 70); glVertex2f(x0, 70);
            glEnd();
        }
        /* ── bande 5 (y 74..90) : OMBRAGE PLAT, toutes les couleurs de sommet
              différentes. C'est le sommet PROVOQUANT que l'on vérifie, et il
              n'est pas le même selon le mode (dernier sommet du triangle pour
              TRIANGLES/STRIP/FAN, 4ᵉ du quadrilatère pour QUADS, i+3 pour
              QUAD_STRIP, PREMIER sommet pour POLYGON). ── */
        glShadeModel(GL_FLAT);
        {   /* cellule 0 : ruban */
            float x0 = FX0(0), x1 = FX1(0);
            glBegin(GL_TRIANGLE_STRIP);
            glColor3ub(255, 0, 0);   glVertex2f(x0, 74);
            glColor3ub(0, 255, 0);   glVertex2f(x0, 90);
            glColor3ub(0, 0, 255);   glVertex2f(x1, 74);
            glColor3ub(255, 255, 0); glVertex2f(x1, 90);
            glEnd();
        }
        {   /* cellule 1 : éventail */
            float x0 = FX0(1), x1 = FX1(1);
            glBegin(GL_TRIANGLE_FAN);
            glColor3ub(255, 0, 255);   glVertex2f(x0, 74);
            glColor3ub(0, 255, 255);   glVertex2f(x0, 90);
            glColor3ub(255, 255, 255); glVertex2f(x1, 90);
            glColor3ub(128, 128, 128); glVertex2f(x1, 74);
            glEnd();
        }
        {   /* cellule 2 : quadrilatère — le 4ᵉ sommet colore les DEUX triangles */
            float x0 = FX0(2), x1 = FX1(2);
            glBegin(GL_QUADS);
            glColor3ub(255, 0, 0);   glVertex2f(x0, 74);
            glColor3ub(0, 255, 0);   glVertex2f(x0, 90);
            glColor3ub(0, 0, 255);   glVertex2f(x1, 90);
            glColor3ub(255, 128, 0); glVertex2f(x1, 74);
            glEnd();
        }
        {   /* cellule 3 : bande de quads — un provoquant par quadrilatère */
            float x0 = FX0(3), x1 = FX1(3);
            xm = (x0 + x1) / 2;
            glBegin(GL_QUAD_STRIP);
            glColor3ub(255, 0, 255); glVertex2f(x0, 74);
            glColor3ub(255, 0, 0);   glVertex2f(x0, 90);
            glColor3ub(128, 64, 32); glVertex2f(xm, 74);
            glColor3ub(0, 255, 0);   glVertex2f(xm, 90);
            glColor3ub(64, 64, 64);  glVertex2f(x1, 74);
            glColor3ub(0, 0, 255);   glVertex2f(x1, 90);
            glEnd();
        }
        {   /* cellule 4 : polygone — c'est le PREMIER sommet qui colore tout */
            float x0 = FX0(4), x1 = FX1(4);
            xm = (x0 + x1) / 2;
            glBegin(GL_POLYGON);
            glColor3ub(255, 255, 0);   glVertex2f(x0, 76);
            glColor3ub(0, 255, 0);     glVertex2f(xm, 74);
            glColor3ub(0, 0, 255);     glVertex2f(x1, 76);
            glColor3ub(255, 0, 255);   glVertex2f(x1, 90);
            glColor3ub(255, 255, 255); glVertex2f(x0, 90);
            glEnd();
        }
        glShadeModel(GL_SMOOTH);
        /* ── bande 6 (y 92..108) : GL_CULL_FACE sur des RUBANS. Un triangle sur
              deux d'un ruban est retourné pour garder l'orientation : si les
              indices ne reproduisaient pas cette alternance, un triangle sur
              deux disparaîtrait. ── */
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);                    /* les rubans sont FACE AVANT */
        for (c = 0; c < 4; c++) {
            float x0 = FX0(c), x1 = FX1(c);
            xm = (x0 + x1) / 2;
            glBegin(GL_TRIANGLE_STRIP);
            glColor3ub(FCOL(c));
            glVertex2f(x0, 92); glVertex2f(x0, 108);
            glVertex2f(xm, 92); glVertex2f(xm, 108);
            glVertex2f(x1, 92); glVertex2f(x1, 108);
            glEnd();
        }
        glCullFace(GL_FRONT);                   /* les mêmes : tout doit disparaître */
        for (c = 4; c < 8; c++) {
            float x0 = FX0(c), x1 = FX1(c);
            xm = (x0 + x1) / 2;
            glBegin(GL_TRIANGLE_STRIP);
            glColor3ub(FCOL(c % 6));
            glVertex2f(x0, 92); glVertex2f(x0, 108);
            glVertex2f(xm, 92); glVertex2f(xm, 108);
            glVertex2f(x1, 92); glVertex2f(x1, 108);
            glEnd();
        }
        glDisable(GL_CULL_FACE);
        /* ── bande 7 (y 110..126) : MÉLANGE avec recouvrement. La fusion ne doit
              jamais réordonner : en additif le recouvrement s'ajoute, et en
              « source par-dessus » c'est le DERNIER dessiné qui gagne. ── */
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);            /* additif */
        for (c = 0; c < 4; c++) {               /* barre rouge, x 2..78 */
            glBegin(GL_QUADS);
            glColor3ub(255, 0, 0);
            glVertex2f(FX0(c) - 2, 110); glVertex2f(FX0(c) - 2, 117);
            glVertex2f(FX1(c) + 2, 117); glVertex2f(FX1(c) + 2, 110);
            glEnd();
        }
        for (c = 2; c < 6; c++) {               /* barre bleue, x 40..120 */
            glBegin(GL_TRIANGLE_STRIP);
            glColor3ub(0, 0, 255);
            glVertex2f(FX0(c) - 2, 110); glVertex2f(FX0(c) - 2, 117);
            glVertex2f(FX1(c) + 2, 110); glVertex2f(FX1(c) + 2, 117);
            glEnd();
        }
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   /* alpha = 1 : le dernier gagne */
        for (c = 0; c < 4; c++) {
            glBegin(GL_TRIANGLE_FAN);
            glColor4ub(255, 0, 0, 255);
            glVertex2f(FX0(c) - 2, 119); glVertex2f(FX0(c) - 2, 126);
            glVertex2f(FX1(c) + 2, 126); glVertex2f(FX1(c) + 2, 119);
            glEnd();
        }
        for (c = 2; c < 6; c++) {
            glBegin(GL_QUADS);
            glColor4ub(0, 255, 0, 255);
            glVertex2f(FX0(c) - 2, 119); glVertex2f(FX0(c) - 2, 126);
            glVertex2f(FX1(c) + 2, 126); glVertex2f(FX1(c) + 2, 119);
            glEnd();
        }
        glDisable(GL_BLEND);
        /* ── bande 8 (y 128..144) : changements d'état INTERCALÉS. Chacun coupe
              la fusion (il part en commande avant le dessin suivant) ; l'image
              doit rester celle d'un rendu sans fusion. ── */
        for (c = 0; c < 2; c++) {               /* sans rien : rouge, en place */
            glBegin(GL_TRIANGLE_STRIP);
            glColor3ub(255, 0, 0);
            glVertex2f(FX0(c), 128); glVertex2f(FX0(c), 140);
            glVertex2f(FX1(c), 128); glVertex2f(FX1(c), 140);
            glEnd();
        }
        glPushMatrix();
        glTranslatef(0, 4, 0);                  /* SET_MATRIX : coupe la fusion */
        for (c = 2; c < 4; c++) {               /* vert, décalé de 4 pixels vers le bas */
            glBegin(GL_TRIANGLE_FAN);
            glColor3ub(0, 255, 0);
            glVertex2f(FX0(c), 128); glVertex2f(FX0(c), 140);
            glVertex2f(FX1(c), 140); glVertex2f(FX1(c), 128);
            glEnd();
        }
        glPopMatrix();
        glEnable(GL_TEXTURE_2D);                /* format de sommet changé : coupe aussi */
        for (c = 4; c < 6; c++) {
            glBegin(GL_QUADS);
            glColor3ub(255, 255, 255);
            glTexCoord2f(0, 0); glVertex2f(FX0(c), 128);
            glTexCoord2f(0, 1); glVertex2f(FX0(c), 140);
            glTexCoord2f(1, 1); glVertex2f(FX1(c), 140);
            glTexCoord2f(1, 0); glVertex2f(FX1(c), 128);
            glEnd();
        }
        glDisable(GL_TEXTURE_2D);
        for (c = 6; c < 8; c++) {               /* bleu, en place */
            glBegin(GL_TRIANGLE_STRIP);
            glColor3ub(0, 0, 255);
            glVertex2f(FX0(c), 128); glVertex2f(FX0(c), 140);
            glVertex2f(FX1(c), 128); glVertex2f(FX1(c), 140);
            glEnd();
        }
        glFinish();

        {   /* pixels témoins */
            static const unsigned long sol[6] = {
                0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF };
            char nm[64];
            for (c = 0; c < 6; c++) {
                sprintf(nm, "ruban lisse %d", c);
                check(nm, c * 20 + 10, 10, sol[c]);
                sprintf(nm, "éventail lisse %d", c);
                check(nm, c * 20 + 10, 28, sol[(c + 2) % 6]);
                sprintf(nm, "polygone lisse %d", c);
                check(nm, c * 20 + 10, 64, sol[(c + 3) % 6]);
            }
            for (c = 0; c < 4; c++) {
                sprintf(nm, "quad lisse %d", c);
                check(nm, c * 20 + 10, 46, sol[(c + 4) % 6]);
            }
            for (c = 4; c < 8; c++) {
                sprintf(nm, "bande de quads lisse %d", c);
                check(nm, c * 20 + 6, 46, sol[(c + 1) % 6]);
                sprintf(nm, "bande de quads lisse %d (2e)", c);
                check(nm, c * 20 + 14, 46, sol[(c + 1) % 6]);
            }
            /* ombrage plat : chaque triangle prend la couleur de SON provoquant */
            check("plat ruban, 1er triangle", 5, 77, 0x0000FF);
            check("plat ruban, 2e triangle", 15, 87, 0xFFFF00);
            check("plat éventail, 1er triangle", 25, 87, 0xFFFFFF);
            check("plat éventail, 2e triangle", 35, 77, 0x808080);
            check("plat quad, moitié 0-1-2", 45, 77, 0xFF8000);
            check("plat quad, moitié 2-3", 55, 87, 0xFF8000);
            check("plat bande de quads, quad 0", 66, 82, 0x00FF00);
            check("plat bande de quads, quad 1", 74, 82, 0x0000FF);
            check("plat polygone, haut", 90, 80, 0xFFFF00);
            check("plat polygone, bas", 90, 88, 0xFFFF00);
            /* alternance d'orientation : les QUATRE triangles du ruban sont là */
            for (c = 0; c < 4; c++) {
                sprintf(nm, "cull arrière, ruban %d gauche", c);
                check(nm, c * 20 + 5, 100, sol[c]);
                sprintf(nm, "cull arrière, ruban %d droite", c);
                check(nm, c * 20 + 15, 100, sol[c]);
            }
            for (c = 4; c < 8; c++) {
                sprintf(nm, "cull avant, ruban %d éliminé", c);
                check(nm, c * 20 + 10, 100, 0x000040);
            }
            /* mélange : additif (recouvrement = somme), puis le dernier gagne */
            check("additif : rouge seul", 10, 113, 0xFF0040);
            check("additif : recouvrement", 50, 113, 0xFF00FF);
            check("additif : bleu seul", 110, 113, 0x0000FF);
            check("ordre : rouge seul", 10, 122, 0xFF0000);
            check("ordre : le dernier gagne (vert)", 50, 122, 0x00FF00);
            check("ordre : vert seul", 110, 122, 0x00FF00);
            /* coupures de la fusion par un changement d'état */
            check("avant translation : rouge en place", 10, 134, 0xFF0000);
            check("après translation : décalé (fond)", 50, 129, 0x000040);
            check("après translation : vert décalé", 50, 143, 0x00FF00);
            check("texture activée : magenta", 90, 134, 0xFF00FF);
            check("texture coupée : bleu", 130, 134, 0x0000FF);
        }
        glDeleteTextures(1, &tid);
#undef FX0
#undef FX1
    } else if (!strcmp(scene, "fill")) {
        /* Remplissage : 40 grands triangles qui se recouvrent, test de
           profondeur et mélange, 30 images. Mesure le coût par pixel. */
        int frames = 30, f, i;
        double t0 = now();
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (f = 0; f < frames; f++) {
            glClearColor(0.2f, 0.2f, 0.2f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glBegin(GL_TRIANGLES);
            for (i = 0; i < 40; i++) {
                float z = (i - 20) / 25.0f, o = (float)((i * 37 + f * 3) % W);
                glColor4f((i % 3) / 2.0f, (i % 5) / 4.0f, (i % 7) / 6.0f, 0.6f);
                glVertex3f(o - W, 0, z); glVertex3f(o + W, 0, z); glVertex3f(o, H, z);
            }
            glEnd();
            glFinish();
        }
        printf("fill : %d images %dx%d, 40 grands triangles : %.2f img/s\n",
               frames, W, H, frames / (now() - t0));
    } else if (!strcmp(scene, "spin")) {
        int frames = 60, f, i;
        double t0;
        glEnable(GL_DEPTH_TEST);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glFrustum(-1, 1, -1, 1, 1.5, 10);
        glMatrixMode(GL_MODELVIEW);
        t0 = now();
        for (f = 0; f < frames; f++) {
            glClearColor(0.1f, 0.1f, 0.2f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glLoadIdentity();
            glTranslatef(0, 0, -4);
            glRotatef(f * 6.0f, 1, 1, 0);
            glBegin(GL_TRIANGLES);
            for (i = 0; i < 400; i++) {
                float a = i * 0.0157f, b = a + 0.0157f;
                glColor3f(1, (i % 20) / 20.0f, 0); glVertex3f(0, 0, 1);
                glColor3f(0, 1, (i % 7) / 7.0f); glVertex3f((float)__builtin_cos(a), (float)__builtin_sin(a), 0);
                glColor3f(0, 0, 1); glVertex3f((float)__builtin_cos(b), (float)__builtin_sin(b), 0);
            }
            glEnd();
            glFinish();
        }
        printf("spin : %d images %dx%d, 400 triangles chacune : %.2f img/s\n",
               frames, W, H, frames / (now() - t0));
    }

    {
        FILE *fp = fopen(out, "wb");
        fprintf(fp, "P6\n%d %d\n255\n", W, H);
        int x;
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                fwrite(buf + y * ROWB + x * 4 + 1, 1, 3, fp);
        fclose(fp);
    }
    CGLSetCurrentContext(0);
    CGLDestroyContext(ctx);
    printf("%s (%d échec(s))\n", failures ? "ÉCHEC" : "OK", failures);
    return failures ? 1 : 0;
    }
}
