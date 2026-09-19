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
    setvbuf(stdout, NULL, _IOLBF, 0);    /* un plantage garde ce qui précède */
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
    } else if (!strcmp(scene, "v15")) {
        /* Relevé, fonction par fonction, de ce que la chaîne TIENT vraiment —
           c'est la matière de docs/re/version-extensions.md et la condition de
           la tâche 4.1 (« rien n'est annoncé qui ne soit tenu »). Chaque sous-
           test fait l'appel, note l'erreur GL, dessine une case de 24×24 et
           compare le pixel du centre à ce qu'OpenGL exige. Le verdict est
           imprimé ; la scène ne compte AUCUN échec, c'est un relevé. */
        int cell = 0;
        GLuint tid[4];
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glGenTextures(4, tid);
#define V15_BEGIN(nm) do { const char *v15n = nm; GLenum v15e; unsigned long v15g; \
                           int v15x = (cell % 5) * 25 + 4, v15y = (cell / 5) * 25 + 4; \
                           cell++; while (glGetError() != GL_NO_ERROR) { }
#define V15_QUAD() do { glBegin(GL_QUADS); \
              glVertex2f((float)v15x, (float)v15y); \
              glVertex2f((float)v15x + 20, (float)v15y); \
              glVertex2f((float)v15x + 20, (float)v15y + 20); \
              glVertex2f((float)v15x, (float)v15y + 20); glEnd(); } while (0)
#define V15_END(want) glFinish(); v15e = glGetError(); \
                      v15g = px(v15x + 10, v15y + 10); \
                      printf("  %-28s err 0x%-4x pixel %06lx attendu %06lx : %s\n", \
                             v15n, (unsigned)v15e, v15g, (unsigned long)(want), \
                             (v15e == GL_NO_ERROR && v15g == (unsigned long)(want)) \
                             ? "TENU" : "NON TENU"); } while (0)

        /* ── 1.2 : textures 3D ── */
        V15_BEGIN("1.2 texture 3D")
        {
            typedef void (*t3_f)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei,
                                 GLint, GLenum, GLenum, const GLvoid *);
            t3_f t3 = (t3_f)gl_sym("glTexImage3D", "glTexImage3DEXT");
            static GLubyte d3[2 * 2 * 2 * 3];
            int i2;
            for (i2 = 0; i2 < 8; i2++) {
                d3[i2 * 3 + 0] = 255; d3[i2 * 3 + 1] = 128; d3[i2 * 3 + 2] = 0;
            }
            glBindTexture(0x806F /* GL_TEXTURE_3D */, tid[0]);
            /* d3 est SERRÉ (lignes de 6 octets) : avec l'alignement de 4 par
               défaut, OpenGL lit des lignes de 8 et le texel (1,1,1) tombe
               après le tableau — le noir était alors la bonne réponse (vu en
               vrai une fois les textures 3D tenues, 19/09/2026). */
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            if (t3) t3(0x806F, 0, GL_RGB, 2, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, d3);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexParameteri(0x806F, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(0x806F, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glEnable(0x806F);
            glColor3ub(255, 255, 255);
            glTexCoord3f(0.5f, 0.5f, 0.5f);
            V15_QUAD();
            glDisable(0x806F);
        }
        V15_END(0xFF8000);

        /* ── 1.3 : cartes de cube ── */
        V15_BEGIN("1.3 cube map")
        {
            static GLubyte f6[4 * 3];
            int i2, f;
            for (i2 = 0; i2 < 4; i2++) {
                f6[i2 * 3 + 0] = 0; f6[i2 * 3 + 1] = 255; f6[i2 * 3 + 2] = 128;
            }
            glBindTexture(0x8513 /* GL_TEXTURE_CUBE_MAP */, tid[1]);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);       /* f6 est serré, cf. 3D */
            for (f = 0; f < 6; f++)
                glTexImage2D(0x8515 + f, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, f6);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexParameteri(0x8513, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(0x8513, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glEnable(0x8513);
            glColor3ub(255, 255, 255);
            glTexCoord3f(0.0f, 0.0f, 1.0f);
            V15_QUAD();
            glDisable(0x8513);
        }
        V15_END(0x00FF80);

        /* ── 1.3 : textures compressées (S3TC) ── */
        V15_BEGIN("1.3 compression S3TC")
        {
            typedef void (*ct_f)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint,
                                 GLsizei, const GLvoid *);
            ct_f ct = (ct_f)gl_sym("glCompressedTexImage2D", "glCompressedTexImage2DARB");
            int i2;
            /* Bloc DXT1 uni : deux couleurs 565 identiques (rouge pur). Le
               tableau fait 256 octets alors que le bloc en fait 8, parce que le
               rasteriseur d'Apple, qui ne sait pas décompresser, lit la texture
               comme si elle était brute et déborde : avec un tableau trop
               court, l'image dépendrait de la mémoire du processus, et la
               comparaison n'aurait aucun sens. */
            static GLubyte blk[256];
            blk[0] = 0x00; blk[1] = 0xF8; blk[2] = 0x00; blk[3] = 0xF8;
            for (i2 = 4; i2 < 256; i2++) blk[i2] = 0x40;
            glBindTexture(GL_TEXTURE_2D, tid[2]);
            if (ct) ct(GL_TEXTURE_2D, 0, 0x83F0 /* COMPRESSED_RGB_S3TC_DXT1 */,
                       4, 4, 0, 8, blk);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glEnable(GL_TEXTURE_2D);
            glColor3ub(255, 255, 255);
            glTexCoord2f(0.5f, 0.5f);
            V15_QUAD();
            glDisable(GL_TEXTURE_2D);
        }
        V15_END(0xFF0000);

        /* ── 1.4 : mélange à facteurs séparés ── */
        V15_BEGIN("1.4 blend func separate")
        {
            typedef void (*bfs_f)(GLenum, GLenum, GLenum, GLenum);
            bfs_f bfs = (bfs_f)gl_sym("glBlendFuncSeparate", "glBlendFuncSeparateEXT");
            glColor3ub(64, 64, 64);
            V15_QUAD();                      /* destination connue */
            glEnable(GL_BLEND);
            if (bfs) bfs(GL_ONE, GL_ONE, GL_ZERO, GL_ZERO);
            glColor4ub(32, 32, 32, 255);
            V15_QUAD();
            glDisable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ZERO);
        }
        V15_END(0x606060);

        /* ── 1.4 : couleur secondaire ── */
        V15_BEGIN("1.4 couleur secondaire")
        {
            typedef void (*sc_f)(GLfloat, GLfloat, GLfloat);
            sc_f sc = (sc_f)gl_sym("glSecondaryColor3f", "glSecondaryColor3fEXT");
            glEnable(0x8458 /* GL_COLOR_SUM */);
            if (sc) sc(128 / 255.0f, 0, 0);
            glColor3ub(0, 128, 0);
            V15_QUAD();
            glDisable(0x8458);
            if (sc) sc(0, 0, 0);
        }
        V15_END(0x808000);

        /* ── 1.4 : paramètres de point (taille bornée) ── */
        V15_BEGIN("1.4 point parameters")
        {
            pp_f ppf = (pp_f)gl_sym("glPointParameterf", "glPointParameterfARB");
            pp_fv ppv = (pp_fv)gl_sym("glPointParameterfv", "glPointParameterfvARB");
            static const GLfloat att[3] = { 1, 0, 0 };
            if (ppv) ppv(GL_POINT_DISTANCE_ATTENUATION_ARB, att);
            if (ppf) ppf(GL_POINT_SIZE_MIN_ARB, 9.0f);
            glPointSize(1);
            glColor3ub(0, 255, 255);
            glBegin(GL_POINTS);
            glVertex2f((float)v15x + 10.5f, (float)v15y + 10.5f);
            glEnd();
            if (ppf) ppf(GL_POINT_SIZE_MIN_ARB, 1.0f);
            glPointSize(1);
        }
        V15_END(0x00FFFF);

        /* ── 1.4 : mipmaps automatiques (GL_GENERATE_MIPMAP) ── */
        V15_BEGIN("1.4 mipmaps automatiques")
        {
            /* Niveau 0 : damier 4×4 noir et blanc. Avec les mipmaps engendrés,
               un quadrilatère très minifié doit rendre le GRIS moyen ; sans, on
               échantillonnerait le damier et on verrait du noir ou du blanc. */
            static GLubyte ck[16 * 16 * 3];
            int u2, v2;
            for (v2 = 0; v2 < 16; v2++)
                for (u2 = 0; u2 < 16; u2++) {
                    GLubyte c2 = ((u2 + v2) & 1) ? 255 : 0;
                    ck[(v2 * 16 + u2) * 3 + 0] = c2;
                    ck[(v2 * 16 + u2) * 3 + 1] = c2;
                    ck[(v2 * 16 + u2) * 3 + 2] = c2;
                }
            glBindTexture(GL_TEXTURE_2D, tid[3]);
            glTexParameteri(GL_TEXTURE_2D, 0x8191 /* GL_GENERATE_MIPMAP */, GL_TRUE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, ck);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glEnable(GL_TEXTURE_2D);
            glColor3ub(255, 255, 255);
            /* 128 texels pour 20 pixels : très minifié, donc un niveau de
               mipmap élevé — le damier doit devenir gris uniforme. Sans
               mipmaps engendrés, on échantillonnerait le damier lui-même. */
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f((float)v15x, (float)v15y);
            glTexCoord2f(8, 0); glVertex2f((float)v15x + 20, (float)v15y);
            glTexCoord2f(8, 8); glVertex2f((float)v15x + 20, (float)v15y + 20);
            glTexCoord2f(0, 8); glVertex2f((float)v15x, (float)v15y + 20);
            glEnd();
            glDisable(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, 0x8191, GL_FALSE);
        }
        V15_END(0x7F7F7F);

        /* ── 1.5 : objets tampon de sommets ── */
        V15_BEGIN("1.5 VBO (glBindBuffer)")
        {
            typedef void (*gb_f)(GLsizei, GLuint *);
            typedef void (*bb_f)(GLenum, GLuint);
            typedef void (*bd_f)(GLenum, long, const GLvoid *, GLenum);
            gb_f gb = (gb_f)gl_sym("glGenBuffers", "glGenBuffersARB");
            bb_f bb = (bb_f)gl_sym("glBindBuffer", "glBindBufferARB");
            bd_f bd = (bd_f)gl_sym("glBufferData", "glBufferDataARB");
            gb_f db = (gb_f)gl_sym("glDeleteBuffers", "glDeleteBuffersARB");
            GLuint vb = 0;
            GLfloat vv[8];
            vv[0] = (float)v15x;      vv[1] = (float)v15y;
            vv[2] = (float)v15x + 20; vv[3] = (float)v15y;
            vv[4] = (float)v15x + 20; vv[5] = (float)v15y + 20;
            vv[6] = (float)v15x;      vv[7] = (float)v15y + 20;
            if (gb && bb && bd) {
                gb(1, &vb);
                bb(0x8892 /* GL_ARRAY_BUFFER */, vb);
                bd(0x8892, (long)sizeof(vv), vv, 0x88E4 /* GL_STATIC_DRAW */);
                glEnableClientState(GL_VERTEX_ARRAY);
                glVertexPointer(2, GL_FLOAT, 0, (const GLvoid *)0);
                glColor3ub(255, 0, 255);
                glDrawArrays(GL_QUADS, 0, 4);
                glDisableClientState(GL_VERTEX_ARRAY);
                bb(0x8892, 0);
                if (db) db(1, &vb);
            }
        }
        V15_END(0xFF00FF);

        /* ── 1.4 : répétition en miroir ── */
        V15_BEGIN("1.4 GL_MIRRORED_REPEAT")
        {
            static GLubyte ramp[2 * 3] = { 255, 0, 0, 0, 0, 255 };
            glBindTexture(GL_TEXTURE_2D, tid[2]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, ramp);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x8370 /* MIRRORED_REPEAT */);
            glEnable(GL_TEXTURE_2D);
            glColor3ub(255, 255, 255);
            glTexCoord2f(1.25f, 0.5f);       /* miroir : équivaut à s = 0.75 → bleu */
            V15_QUAD();
            glDisable(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        }
        V15_END(0x0000FF);

        /* ── 1.4 : position de rastérisation en coordonnées fenêtre ── */
        V15_BEGIN("1.4 glWindowPos + glDrawPixels")
        {
            typedef void (*wp_f)(GLint, GLint);
            wp_f wp = (wp_f)gl_sym("glWindowPos2i", "glWindowPos2iARB");
            static GLubyte im[8 * 8 * 3];
            int i2;
            for (i2 = 0; i2 < 64; i2++) {
                im[i2 * 3 + 0] = 0; im[i2 * 3 + 1] = 200; im[i2 * 3 + 2] = 255;
            }
            if (wp) wp(v15x + 6, H - (v15y + 14));   /* fenêtre : y vers le HAUT */
            glDrawPixels(8, 8, GL_RGB, GL_UNSIGNED_BYTE, im);
        }
        V15_END(0x00C8FF);

        /* ── 1.2 : GL_CLAMP_TO_EDGE ── */
        V15_BEGIN("1.2 GL_CLAMP_TO_EDGE")
        {
            static GLubyte ramp[2 * 3] = { 255, 0, 0, 0, 255, 0 };
            glBindTexture(GL_TEXTURE_2D, tid[2]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, ramp);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /* CLAMP_TO_EDGE */);
            glEnable(GL_TEXTURE_2D);
            glColor3ub(255, 255, 255);
            glTexCoord2f(3.0f, 0.5f);        /* borné : dernier texel → vert */
            V15_QUAD();
            glDisable(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        }
        V15_END(0x00FF00);

        /* ── 1.3 : multitexture, 4 unités ── */
        V15_BEGIN("1.3 multitexture 4 unites")
        {
            static GLubyte g1[3] = { 128, 128, 128 };
            int u2;
            for (u2 = 0; u2 < 4; u2++) {
                glActiveTextureARB(GL_TEXTURE0_ARB + u2);
                glBindTexture(GL_TEXTURE_2D, tid[2]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, g1);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
                glEnable(GL_TEXTURE_2D);
                glMultiTexCoord2fARB(GL_TEXTURE0_ARB + u2, 0.5f, 0.5f);
            }
            glColor3ub(255, 255, 255);
            V15_QUAD();                      /* 0.5^4 = 0.0625 → 16 */
            for (u2 = 3; u2 >= 0; u2--) {
                glActiveTextureARB(GL_TEXTURE0_ARB + u2);
                glDisable(GL_TEXTURE_2D);
            }
            glActiveTextureARB(GL_TEXTURE0_ARB);
        }
        V15_END(0x101010);

        /* ── 1.3 : multitexture, 8 unités (au-delà du chemin accéléré : c'est le
           repli sur le rendu d'Apple qui doit tenir) ── */
        V15_BEGIN("1.3 multitexture 8 unites")
        {
            static GLubyte g1[3] = { 128, 128, 128 };
            int u2;
            for (u2 = 0; u2 < 8; u2++) {
                glActiveTextureARB(GL_TEXTURE0_ARB + u2);
                glBindTexture(GL_TEXTURE_2D, tid[2]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, g1);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
                glEnable(GL_TEXTURE_2D);
                glMultiTexCoord2fARB(GL_TEXTURE0_ARB + u2, 0.5f, 0.5f);
            }
            glColor3ub(255, 255, 255);
            V15_QUAD();                      /* 0.5^8 · 255 = 0,996 → 1 */
            for (u2 = 7; u2 >= 0; u2--) {
                glActiveTextureARB(GL_TEXTURE0_ARB + u2);
                glDisable(GL_TEXTURE_2D);
            }
            glActiveTextureARB(GL_TEXTURE0_ARB);
        }
        V15_END(0x010101);

        /* ── limite annoncée : une texture de 4096 de large (au-delà du chemin
           accéléré, qui s'arrête à QGPU_MAX_TEX_DIM = 2048) ── */
        V15_BEGIN("limite texture 4096 de large")
        {
            static GLubyte wide[4096 * 3];
            int i2;
            for (i2 = 0; i2 < 4096; i2++) {
                wide[i2 * 3 + 0] = 0; wide[i2 * 3 + 1] = 128; wide[i2 * 3 + 2] = 255;
            }
            glBindTexture(GL_TEXTURE_2D, tid[3]);
            glTexParameteri(GL_TEXTURE_2D, 0x8191, GL_FALSE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4096, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, wide);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glEnable(GL_TEXTURE_2D);
            glColor3ub(255, 255, 255);
            glTexCoord2f(0.5f, 0.5f);
            V15_QUAD();
            glDisable(GL_TEXTURE_2D);
        }
        V15_END(0x0080FF);

        /* ── 1.2 : glDrawRangeElements ── */
        V15_BEGIN("1.2 glDrawRangeElements")
        {
            typedef void (*dre_f)(GLenum, GLuint, GLuint, GLsizei, GLenum, const GLvoid *);
            dre_f dre = (dre_f)gl_sym("glDrawRangeElements", "glDrawRangeElementsEXT");
            GLfloat vv[8];
            static const GLuint ii[4] = { 0, 1, 2, 3 };
            vv[0] = (float)v15x;      vv[1] = (float)v15y;
            vv[2] = (float)v15x + 20; vv[3] = (float)v15y;
            vv[4] = (float)v15x + 20; vv[5] = (float)v15y + 20;
            vv[6] = (float)v15x;      vv[7] = (float)v15y + 20;
            if (dre) {
                glEnableClientState(GL_VERTEX_ARRAY);
                glVertexPointer(2, GL_FLOAT, 0, vv);
                glColor3ub(200, 200, 0);
                dre(GL_QUADS, 0, 3, 4, GL_UNSIGNED_INT, ii);
                glDisableClientState(GL_VERTEX_ARRAY);
            }
        }
        V15_END(0xC8C800);

        /* ── 1.3 : matrices transposées ── */
        V15_BEGIN("1.3 glLoadTransposeMatrix")
        {
            typedef void (*lt_f)(const GLfloat *);
            lt_f lt = (lt_f)gl_sym("glLoadTransposeMatrixf", "glLoadTransposeMatrixfARB");
            static const GLfloat idm[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            if (lt) lt(idm);
            glColor3ub(0, 200, 200);
            V15_QUAD();
            glPopMatrix();
        }
        V15_END(0x00C8C8);

        /* ── 1.4 : coordonnée de brouillard explicite ── */
        V15_BEGIN("1.4 glFogCoord")
        {
            typedef void (*fc_f)(GLfloat);
            fc_f fc = (fc_f)gl_sym("glFogCoordf", "glFogCoordfEXT");
            GLfloat fogc[4] = { 0, 0, 1, 1 };
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogf(GL_FOG_START, 0);
            glFogf(GL_FOG_END, 1);
            glFogfv(GL_FOG_COLOR, fogc);
            glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FOG_COORDINATE_EXT);
            glEnable(GL_FOG);
            if (fc) fc(1.0f);               /* f = 0 → couleur du brouillard pure */
            glColor3ub(255, 0, 0);
            V15_QUAD();
            glDisable(GL_FOG);
            glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FRAGMENT_DEPTH_EXT);
        }
        V15_END(0x0000FF);
#undef V15_BEGIN
#undef V15_QUAD
#undef V15_END
        glDeleteTextures(4, tid);
        glFinish();
    } else if (!strcmp(scene, "blendc")) {
        /* Mélange à couleur constante et équations minimum/maximum (v8).
           Six bandes verticales sur un fond connu ; les couleurs sont choisies
           exactes en 8 bits pour que les témoins des bandes MIN et MAX (où
           aucun arrondi n'intervient) soient comparables au bit près. */
        typedef void (*bc_f)(GLfloat, GLfloat, GLfloat, GLfloat);
        typedef void (*be_f)(GLenum);
        bc_f blend_color = (bc_f)gl_sym("glBlendColor", "glBlendColorEXT");
        be_f blend_eq = (be_f)gl_sym("glBlendEquation", "glBlendEquationEXT");
        static const struct { GLenum src, dst, eq; } bands[6] = {
            { 0x8001 /* CONSTANT_COLOR */,       GL_ZERO, 0x8006 },
            { 0x8002 /* 1-CONSTANT_COLOR */,     GL_ZERO, 0x8006 },
            { 0x8003 /* CONSTANT_ALPHA */,       GL_ZERO, 0x8006 },
            { 0x8004 /* 1-CONSTANT_ALPHA */,     GL_ZERO, 0x8006 },
            { GL_ZERO, GL_ZERO, 0x8007 /* GL_MIN */ },
            { GL_ZERO, GL_ZERO, 0x8008 /* GL_MAX */ },
        };
        int i;
        float bw = W / 6.0f;
        printf("glBlendColor=%p glBlendEquation=%p\n", (void *)blend_color, (void *)blend_eq);
        glClearColor(64 / 255.0f, 128 / 255.0f, 192 / 255.0f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        if (blend_color) blend_color(0.5f, 0.25f, 0.75f, 0.5f);
        for (i = 0; i < 6; i++) {
            glBlendFunc(bands[i].src, bands[i].dst);
            if (blend_eq) blend_eq(bands[i].eq);
            glColor3ub(192, 64, 128);
            glBegin(GL_QUADS);
            glVertex2f(i * bw, 8); glVertex2f((i + 1) * bw, 8);
            glVertex2f((i + 1) * bw, (float)H - 8); glVertex2f(i * bw, (float)H - 8);
            glEnd();
        }
        if (blend_eq) blend_eq(0x8006);
        glDisable(GL_BLEND);
        glFinish();
        {
            int x4 = (int)(4.5f * bw), x5 = (int)(5.5f * bw);
            /* min / max, canal par canal, entre (192,64,128) et (64,128,192) :
               les facteurs sont IGNORÉS, c'est la lettre de la spécification. */
            check("GL_MIN", x4, H / 2, 0x404080);
            check("GL_MAX", x5, H / 2, 0xC080C0);
            check("fond intact (haut)", W / 2, 2, 0x4080C0);
        }
    } else if (!strcmp(scene, "logicop")) {
        /* Opérations logiques (v8). Six carrés, une opération chacun, sur un
           fond dont chaque octet est connu : la sortie est exacte au bit près,
           il n'y a aucun arrondi dans cette fonction. */
        static const struct { GLenum op; const char *n; } ops[6] = {
            { GL_XOR, "XOR" }, { GL_INVERT, "INVERT" }, { GL_AND, "AND" },
            { GL_OR, "OR" }, { GL_COPY_INVERTED, "COPY_INVERTED" }, { GL_NAND, "NAND" },
        };
        static const unsigned char S[3] = { 0xF0, 0x3C, 0xA5 };
        static const unsigned char D[3] = { 0x5A, 0xFF, 0x0F };
        int i;
        float bw = W / 6.0f;
        glClearColor(D[0] / 255.0f, D[1] / 255.0f, D[2] / 255.0f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_COLOR_LOGIC_OP);
        for (i = 0; i < 6; i++) {
            glLogicOp(ops[i].op);
            glColor3ub(S[0], S[1], S[2]);
            glBegin(GL_QUADS);
            glVertex2f(i * bw, 8); glVertex2f((i + 1) * bw, 8);
            glVertex2f((i + 1) * bw, (float)H - 8); glVertex2f(i * bw, (float)H - 8);
            glEnd();
        }
        glDisable(GL_COLOR_LOGIC_OP);
        glFinish();
        {
            int k;
            unsigned long want[6];
            for (k = 0; k < 6; k++) {
                unsigned long r, g2, b;
                switch (ops[k].op) {
                case GL_XOR:  r = S[0] ^ D[0]; g2 = S[1] ^ D[1]; b = S[2] ^ D[2]; break;
                case GL_INVERT: r = ~D[0]; g2 = ~D[1]; b = ~D[2]; break;
                case GL_AND:  r = S[0] & D[0]; g2 = S[1] & D[1]; b = S[2] & D[2]; break;
                case GL_OR:   r = S[0] | D[0]; g2 = S[1] | D[1]; b = S[2] | D[2]; break;
                case GL_COPY_INVERTED: r = ~S[0]; g2 = ~S[1]; b = ~S[2]; break;
                default:      r = ~(S[0] & D[0]); g2 = ~(S[1] & D[1]); b = ~(S[2] & D[2]); break;
                }
                want[k] = ((r & 0xFF) << 16) | ((g2 & 0xFF) << 8) | (b & 0xFF);
                check(ops[k].n, (int)((k + 0.5f) * bw), H / 2, want[k]);
            }
            check("fond intact (haut)", W / 2, 2, 0x5AFF0F);
        }
    } else if (!strcmp(scene, "polymode")) {
        /* Modes de polygone (v8). Un quadrilatère et un triangle en fil de fer,
           puis en points, puis les deux faces en modes différents. Le point à
           regarder est la DIAGONALE du quadrilatère : elle ne doit pas être
           tracée (c'est le contour, pas la décomposition en triangles). */
        /* 64/255 et non 0.25 : 0.25·255 = 63,75, que le rendu d'Apple arrondit
           à 63 et l'hôte à 64 — un écart de 1/255 qui n'apprend rien. */
        glClearColor(0, 0, 64 / 255.0f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glColor3ub(255, 255, 0);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        /* Coordonnées en .5, c'est-à-dire au CENTRE des pixels. Une arête posée
           sur un y ENTIER tombe exactement sur la frontière entre deux lignes,
           et deux rasteriseurs conformes ont le droit d'en choisir chacun une —
           c'est ce qui sépare l'hôte du rendu d'Apple dans la scène « mixte ».
           Ici on sort du cas ambigu, et les deux images doivent coïncider. */
        glBegin(GL_QUADS);
        glVertex2f(8.5f, 8.5f); glVertex2f((float)W / 2 - 8.5f, 8.5f);
        glVertex2f((float)W / 2 - 8.5f, (float)H / 2 - 8.5f);
        glVertex2f(8.5f, (float)H / 2 - 8.5f);
        glEnd();
        glColor3ub(0, 255, 255);
        glBegin(GL_TRIANGLES);
        glVertex2f((float)W / 2 + 8.5f, 8.5f); glVertex2f((float)W - 8.5f, 8.5f);
        glVertex2f((float)W / 2 + 8.5f, (float)H / 2 - 8.5f);
        glEnd();
        /* points : taille 1, pour que la comparaison avec le rendu d'Apple ne
           dépende pas de la façon dont chacun centre un gros point. */
        glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
        glColor3ub(255, 0, 255);
        glBegin(GL_POLYGON);
        glVertex2f(8.5f, (float)H / 2 + 8.5f);
        glVertex2f((float)W / 2 - 8.5f, (float)H / 2 + 8.5f);
        glVertex2f((float)W / 2 - 8.5f, (float)H - 8.5f);
        glVertex2f(8.5f, (float)H - 8.5f);
        glEnd();
        /* une face pleine, l'autre en fil de fer : le quadrilatère est arrière */
        glPolygonMode(GL_FRONT, GL_FILL);
        glPolygonMode(GL_BACK, GL_LINE);
        glColor3ub(0, 255, 0);
        glBegin(GL_QUADS);                  /* sens horaire = face arrière en CCW */
        glVertex2f((float)W / 2 + 8.5f, (float)H / 2 + 8.5f);
        glVertex2f((float)W / 2 + 8.5f, (float)H - 8.5f);
        glVertex2f((float)W - 8.5f, (float)H - 8.5f);
        glVertex2f((float)W - 8.5f, (float)H / 2 + 8.5f);
        glEnd();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glFinish();
        {
            int cx = W / 4, cy = H / 4;
            /* (cx,cy) est EXACTEMENT sur la diagonale de la décomposition du
               quadrilatère en deux triangles : c'est le témoin de l'écart que
               la v8 supprime sur le chemin brut. */
            check("diagonale NON tracee", cx, cy, 0x000040);
            check("diagonale NON tracee (2)", cx + 1, cy + 1, 0x000040);
            check("interieur du quad, hors diagonale", cx + 8, cy - 8, 0x000040);
        }
    } else if (!strcmp(scene, "stipple")) {
        /* Pointillés (v8). À gauche : un motif de polygone en bandes
           HORIZONTALES de 16 lignes — il dit le sens vertical (c'est la couture
           entre la ligne yw=0 de glPolygonStipple, en bas, et la ligne 0 des
           surfaces du protocole, en haut). À droite : un motif en colonnes de
           8 — il dit le sens horizontal (bit de poids fort = x = 0). En bas :
           des segments pointillés. */
        GLubyte rows[128], cols[128];
        int i, r;
        for (r = 0; r < 32; r++)
            for (i = 0; i < 4; i++) {
                rows[r * 4 + i] = (r < 16) ? 0xFF : 0x00;
                cols[r * 4 + i] = 0xF0;      /* 8 pixels allumés, 8 éteints */
            }
        glClearColor(0, 0, 64 / 255.0f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_POLYGON_STIPPLE);
        glPolygonStipple(rows);
        glColor3ub(255, 128, 0);
        glBegin(GL_QUADS);
        glVertex2f(0, 0); glVertex2f((float)W / 2, 0);
        glVertex2f((float)W / 2, (float)H * 3 / 4); glVertex2f(0, (float)H * 3 / 4);
        glEnd();
        glPolygonStipple(cols);
        glColor3ub(0, 255, 128);
        glBegin(GL_QUADS);
        glVertex2f((float)W / 2, 0); glVertex2f((float)W, 0);
        glVertex2f((float)W, (float)H * 3 / 4); glVertex2f((float)W / 2, (float)H * 3 / 4);
        glEnd();
        glDisable(GL_POLYGON_STIPPLE);
        /* pointillé de ligne : segments indépendants puis un ruban */
        glEnable(GL_LINE_STIPPLE);
        glLineStipple(2, 0x00FF);
        glColor3ub(255, 255, 255);
        glBegin(GL_LINES);
        glVertex2f(4, (float)H - 12); glVertex2f((float)W - 4, (float)H - 12);
        glEnd();
        glLineStipple(1, 0x0F0F);
        glBegin(GL_LINE_STRIP);
        glVertex2f(4, (float)H - 5); glVertex2f((float)W / 2, (float)H - 5);
        glVertex2f((float)W - 4, (float)H - 5);
        glEnd();
        glDisable(GL_LINE_STIPPLE);
        glFinish();
    } else if (!strcmp(scene, "occl")) {
        /* Requêtes d'occlusion (v8 / OpenGL 1.5). Le compte est vérifié EXACT :
           un rectangle dont l'aire est connue, puis le même coupé de moitié par
           les ciseaux, puis le même entièrement caché par la profondeur. Le
           rendu d'Apple seul n'écrit rien dans la variable de sortie (bouchon
           gldGetQueryInfo) : la scène accepte donc aussi « 0 partout », ce qui
           est précisément la preuve que le repli logiciel NE TIENT PAS cette
           fonction. */
        typedef void (*gen_f)(GLsizei, GLuint *);
        typedef void (*bq_f)(GLenum, GLuint);
        typedef void (*eq_f)(GLenum);
        typedef void (*gq_f)(GLuint, GLenum, GLuint *);
        gen_f gen = (gen_f)gl_sym("glGenQueries", "glGenQueriesARB");
        bq_f beg = (bq_f)gl_sym("glBeginQuery", "glBeginQueryARB");
        eq_f end = (eq_f)gl_sym("glEndQuery", "glEndQueryARB");
        gq_f getq = (gq_f)gl_sym("glGetQueryObjectuiv", "glGetQueryObjectuivARB");
        gen_f del = (gen_f)gl_sym("glDeleteQueries", "glDeleteQueriesARB");
        GLuint q = 0, got[3], avail[3];
        unsigned long want[3];
        int i, quarter = W / 4, half = H / 2;
        const char *names[3] = { "visible en entier", "coupe par les ciseaux",
                                 "cache par la profondeur" };
        want[0] = (unsigned long)(2 * quarter) * half;
        want[1] = want[0] / 2;
        want[2] = 0;
        for (i = 0; i < 3; i++) { got[i] = 0; avail[i] = 0; }
        glClearColor(0, 0, 64 / 255.0f, 1);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (!gen || !beg || !end || !getq) {
            printf("occl : points d'entree absents\n");
        } else {
            /* un mur opaque à mi-profondeur, sur toute l'image */
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glColor3ub(32, 32, 32);
            glBegin(GL_QUADS);
            glVertex3f(0, 0, 0); glVertex3f((float)W, 0, 0);
            glVertex3f((float)W, (float)H, 0); glVertex3f(0, (float)H, 0);
            glEnd();
            glDisable(GL_DEPTH_TEST);
            for (i = 0; i < 3; i++) {
                /* glOrtho(0,W,H,0,−1,1) NIE z : un z positif donne une
                   profondeur plus PETITE, donc plus près de l'observateur. Le
                   troisième cas doit donc passer DERRIÈRE le mur (z < 0). */
                float z = (i == 2) ? -0.5f : 0.5f;
                gen(1, &q);
                if (i == 1) { glEnable(GL_SCISSOR_TEST); glScissor(quarter, half, quarter, half); }
                if (i == 2) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); }
                beg(0x8914 /* GL_SAMPLES_PASSED */, q);
                glColor3ub(255, 200, 0);
                glBegin(GL_QUADS);
                glVertex3f((float)quarter, 0, z);
                glVertex3f((float)(3 * quarter), 0, z);
                glVertex3f((float)(3 * quarter), (float)half, z);
                glVertex3f((float)quarter, (float)half, z);
                glEnd();
                end(0x8914);
                getq(q, 0x8867 /* GL_QUERY_RESULT_AVAILABLE */, &avail[i]);
                getq(q, 0x8866 /* GL_QUERY_RESULT */, &got[i]);
                if (i == 1) glDisable(GL_SCISSOR_TEST);
                if (i == 2) glDisable(GL_DEPTH_TEST);
                if (del) del(1, &q);
                printf("occl %-26s : %u echantillons (attendu %lu), disponible %u\n",
                       names[i], got[i], want[i], avail[i]);
            }
            if (got[0] == 0 && got[1] == 0) {
                printf("occl : aucune requete tenue (rendu d'Apple seul) — attendu\n");
            } else {
                for (i = 0; i < 3; i++) {
                    int ok = (got[i] == want[i]);
                    printf("  %s requete %-26s = %u\n", ok ? "ok  " : "FAIL", names[i], got[i]);
                    if (!ok) failures++;
                }
            }
        }
        glFinish();
    } else if (!strcmp(scene, "v8probe")) {
        /* Sonde des états du pipeline fixe que la v8 ajoute (mélange constant,
           minimum/maximum, opération logique, pointillés, modes de polygone) :
           un réglage GL par glClear, le traceur vide l'état de GLEngine à
           chaque effacement (POMPPC_GLTRACE_STATE=1) et on diffe les vidages
           avec tools/re/diffstate.py. Même méthode que « stencilprobe ». */
        typedef void (*bc_f)(GLfloat, GLfloat, GLfloat, GLfloat);
        typedef void (*be_f)(GLenum);
        bc_f blend_color = (bc_f)gl_sym("glBlendColor", "glBlendColorEXT");
        be_f blend_eq = (be_f)gl_sym("glBlendEquation", "glBlendEquationEXT");
        GLubyte stipA[128], stipB[128];
        int i;
        for (i = 0; i < 128; i++) {
            /* Octets tous distincts : le diff nomme alors l'octet du motif,
               pas seulement « quelque chose a bougé ». */
            stipA[i] = (GLubyte)(0x40 + i);
            stipB[i] = (GLubyte)(0xBF - i);
        }
        glClearColor(0, 0, 0, 1);
        pstep("1 reference");
        if (blend_color) blend_color(0.125f, 0.25f, 0.375f, 0.5f);
        pstep("2 glBlendColor(.125 .25 .375 .5)");
        if (blend_color) blend_color(0.75f, 0.875f, 0.0625f, 0.25f);
        pstep("3 glBlendColor(.75 .875 .0625 .25)");
        glBlendFunc(0x8001 /* GL_CONSTANT_COLOR */, 0x8004 /* GL_ONE_MINUS_CONSTANT_ALPHA */);
        pstep("4 glBlendFunc(CONSTANT_COLOR, ONE_MINUS_CONSTANT_ALPHA)");
        if (blend_eq) blend_eq(0x8007 /* GL_MIN */);
        pstep("5 glBlendEquation(GL_MIN)");
        if (blend_eq) blend_eq(0x8008 /* GL_MAX */);
        pstep("6 glBlendEquation(GL_MAX)");
        glLogicOp(GL_XOR);
        pstep("7 glLogicOp(GL_XOR)");
        glLogicOp(GL_NAND);
        pstep("8 glLogicOp(GL_NAND)");
        glEnable(GL_COLOR_LOGIC_OP);
        pstep("9 glEnable(GL_COLOR_LOGIC_OP)");
        glLineStipple(3, 0xA5A5);
        pstep("10 glLineStipple(3, 0xA5A5)");
        glLineStipple(97, 0x1234);
        pstep("11 glLineStipple(97, 0x1234)");
        glEnable(GL_LINE_STIPPLE);
        pstep("12 glEnable(GL_LINE_STIPPLE)");
        glPolygonStipple(stipA);
        pstep("13 glPolygonStipple(0x40..0xBF)");
        glPolygonStipple(stipB);
        pstep("14 glPolygonStipple(0xBF..0x40)");
        glEnable(GL_POLYGON_STIPPLE);
        pstep("15 glEnable(GL_POLYGON_STIPPLE)");
        glPolygonMode(GL_FRONT, GL_LINE);
        pstep("16 glPolygonMode(FRONT, LINE)");
        glPolygonMode(GL_BACK, GL_POINT);
        pstep("17 glPolygonMode(BACK, POINT)");
        glEnable(GL_POLYGON_OFFSET_LINE);
        pstep("18 glEnable(GL_POLYGON_OFFSET_LINE)");
        glEnable(GL_POLYGON_OFFSET_POINT);
        pstep("19 glEnable(GL_POLYGON_OFFSET_POINT)");
        glPolygonOffset(2.5f, 3.25f);
        pstep("20 glPolygonOffset(2.5, 3.25)");
        glFinish();
        /* Relecture par glGet : elle dit ce que GLEngine croit avoir retenu,
           et sert de contrôle croisé des offsets relevés par le diff. */
        {
            GLfloat bc[4] = { -1, -1, -1, -1 };
            GLint li[4] = { 0, 0, 0, 0 };
            glGetFloatv(0x8005 /* GL_BLEND_COLOR */, bc);
            printf("GL_BLEND_COLOR = %g %g %g %g\n", bc[0], bc[1], bc[2], bc[3]);
            glGetIntegerv(GL_LOGIC_OP_MODE, li);
            printf("GL_LOGIC_OP_MODE = 0x%x\n", (unsigned)li[0]);
            glGetIntegerv(GL_LINE_STIPPLE_REPEAT, li);
            printf("GL_LINE_STIPPLE_REPEAT = %d\n", (int)li[0]);
            glGetIntegerv(GL_LINE_STIPPLE_PATTERN, li);
            printf("GL_LINE_STIPPLE_PATTERN = 0x%x\n", (unsigned)li[0]);
            glGetIntegerv(GL_POLYGON_MODE, li);
            printf("GL_POLYGON_MODE = 0x%x 0x%x\n", (unsigned)li[0], (unsigned)li[1]);
            printf("glGetError = 0x%x\n", (unsigned)glGetError());
        }
    } else if (!strcmp(scene, "qprobe")) {
        /* Requêtes d'occlusion : les points d'entrée existent-ils dans
           libGL/GLEngine, et que se passe-t-il si on les appelle alors que
           GL_ARB_occlusion_query n'est PAS annoncée ? (Le tableau de bits du
           bloc de configuration, docs/re/capacites-glengine.md §3.2, ne porte
           pas le bit 17 chez le GLDriver d'Apple.) */
        typedef void (*gen_f)(GLsizei, GLuint *);
        typedef void (*bq_f)(GLenum, GLuint);
        typedef void (*eq_f)(GLenum);
        typedef void (*gq_f)(GLuint, GLenum, GLuint *);
        typedef GLboolean (*isq_f)(GLuint);
        gen_f gen = (gen_f)gl_sym("glGenQueries", "glGenQueriesARB");
        bq_f beg = (bq_f)gl_sym("glBeginQuery", "glBeginQueryARB");
        eq_f end = (eq_f)gl_sym("glEndQuery", "glEndQueryARB");
        gq_f getq = (gq_f)gl_sym("glGetQueryObjectuiv", "glGetQueryObjectuivARB");
        isq_f isq = (isq_f)gl_sym("glIsQuery", "glIsQueryARB");
        gen_f del = (gen_f)gl_sym("glDeleteQueries", "glDeleteQueriesARB");
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        GLuint q = 0, avail = 0, n = 0;
        printf("glGenQueries=%p glBeginQuery=%p glEndQuery=%p glGetQueryObjectuiv=%p "
               "glIsQuery=%p glDeleteQueries=%p\n",
               (void *)gen, (void *)beg, (void *)end, (void *)getq, (void *)isq, (void *)del);
        printf("ARB_occlusion_query annoncee : %s\n",
               (ext && strstr(ext, "GL_ARB_occlusion_query")) ? "oui" : "non");
        if (!gen || !beg || !end || !getq) {
            printf("qprobe : points d'entree absents, rien a tester\n");
        } else {
            gen(1, &q);
            printf("glGenQueries -> %u (err 0x%x)\n", q, (unsigned)glGetError());
            printf("glIsQuery(%u) = %d\n", q, isq ? (int)isq(q) : -1);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            beg(0x8914 /* GL_SAMPLES_PASSED */, q);
            printf("glBeginQuery -> err 0x%x\n", (unsigned)glGetError());
            glColor3f(1, 1, 1);
            glBegin(GL_TRIANGLES);
            glVertex2f(0, 0); glVertex2f((float)W, 0); glVertex2f(0, (float)H);
            glEnd();
            end(0x8914);
            printf("glEndQuery -> err 0x%x\n", (unsigned)glGetError());
            getq(q, 0x8867 /* GL_QUERY_RESULT_AVAILABLE */, &avail);
            printf("disponible = %u (err 0x%x)\n", avail, (unsigned)glGetError());
            getq(q, 0x8866 /* GL_QUERY_RESULT */, &n);
            printf("echantillons = %u (err 0x%x) ; attendu ~%d (moitie de %dx%d)\n",
                   n, (unsigned)glGetError(), W * H / 2, W, H);
            if (del) del(1, &q);
            printf("glDeleteQueries -> err 0x%x\n", (unsigned)glGetError());
        }
        glFinish();
    } else if (!strcmp(scene, "caps")) {
        /* Ce que l'application VOIT : version, extensions, limites. C'est la
           preuve de la tâche 4.1 — et la liste se relit telle quelle. */
        static const struct { GLenum e; const char *n; int nv; } gi[] = {
            { 0x0D33, "GL_MAX_TEXTURE_SIZE", 1 },
            { 0x84E2, "GL_MAX_TEXTURE_UNITS", 1 },
            { 0x8872, "GL_MAX_TEXTURE_IMAGE_UNITS", 1 },
            { 0x8871, "GL_MAX_TEXTURE_COORDS", 1 },
            { 0x8073, "GL_MAX_3D_TEXTURE_SIZE", 1 },
            { 0x851C, "GL_MAX_CUBE_MAP_TEXTURE_SIZE", 1 },
            { 0x84F8, "GL_MAX_RECTANGLE_TEXTURE_SIZE", 1 },
            { 0x0D31, "GL_MAX_LIGHTS", 1 },
            { 0x0D32, "GL_MAX_CLIP_PLANES", 1 },
            { 0x80E8, "GL_MAX_ELEMENTS_VERTICES", 1 },
            { 0x80E9, "GL_MAX_ELEMENTS_INDICES", 1 },
            { 0x0D50, "GL_SUBPIXEL_BITS", 1 },
            { 0x0D56, "GL_DEPTH_BITS", 1 },
            { 0x0D57, "GL_STENCIL_BITS", 1 },
            { 0x80A8, "GL_SAMPLE_BUFFERS", 1 },
            { 0x80A9, "GL_SAMPLES", 1 },
            { 0x0B12, "GL_POINT_SIZE_RANGE", 2 },
            { 0x846D, "GL_ALIASED_POINT_SIZE_RANGE", 2 },
            { 0x0B22, "GL_LINE_WIDTH_RANGE", 2 },
            { 0x846E, "GL_ALIASED_LINE_WIDTH_RANGE", 2 },
            { 0x84FF, "GL_MAX_TEXTURE_MAX_ANISOTROPY", 1 },
            { 0x84FD, "GL_MAX_TEXTURE_LOD_BIAS", 1 },
            { 0x86A2, "GL_NUM_COMPRESSED_TEXTURE_FORMATS", 1 },
        };
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        int i, nl = 0;
        printf("== VERSION == %s\n", (const char *)glGetString(GL_VERSION));
        printf("== VENDOR   == %s\n", (const char *)glGetString(GL_VENDOR));
        printf("== RENDERER == %s\n", (const char *)glGetString(GL_RENDERER));
        printf("== EXTENSIONS ==\n");
        if (ext) {
            const char *p = ext;
            while (*p) {
                const char *q2 = p;
                while (*q2 && *q2 != ' ') q2++;
                printf("  %.*s\n", (int)(q2 - p), p);
                nl++;
                p = *q2 ? q2 + 1 : q2;
            }
        }
        printf("  (%d extensions)\n", nl);
        printf("== LIMITES ==\n");
        for (i = 0; i < (int)(sizeof(gi) / sizeof(gi[0])); i++) {
            GLfloat f[4] = { 0, 0, 0, 0 };
            while (glGetError() != GL_NO_ERROR) { }
            glGetFloatv(gi[i].e, f);
            if (glGetError() != GL_NO_ERROR)
                printf("  %-34s : (refuse)\n", gi[i].n);
            else if (gi[i].nv == 2)
                printf("  %-34s : %g %g\n", gi[i].n, f[0], f[1]);
            else
                printf("  %-34s : %g\n", gi[i].n, f[0]);
        }
        glFinish();
    } else if (!strcmp(scene, "entry")) {
        /* Un appel par fonction de chaque version annoncée, résolu par la
           liaison normale du processus : la question n'est pas « la chaîne
           dit-elle 1.5 » mais « l'appel passe-t-il, sans erreur GL ni
           plantage ». Le rendu est vérifié à l'image par la scène « entry »
           elle-même (un quadrilatère par fonction). */
        struct ent { const char *a, *b; void *p; };
        static struct ent es[] = {
            { "glTexImage3D", "glTexImage3DEXT", 0 },              /* 1.2 */
            { "glDrawRangeElements", "glDrawRangeElementsEXT", 0 },/* 1.2 */
            { "glBlendColor", "glBlendColorEXT", 0 },              /* 1.2 imaging / 1.4 */
            { "glBlendEquation", "glBlendEquationEXT", 0 },        /* 1.2 imaging / 1.4 */
            { "glActiveTexture", "glActiveTextureARB", 0 },        /* 1.3 */
            { "glMultiTexCoord2f", "glMultiTexCoord2fARB", 0 },    /* 1.3 */
            { "glCompressedTexImage2D", "glCompressedTexImage2DARB", 0 }, /* 1.3 */
            { "glLoadTransposeMatrixf", "glLoadTransposeMatrixfARB", 0 }, /* 1.3 */
            { "glSampleCoverage", "glSampleCoverageARB", 0 },      /* 1.3 */
            { "glBlendFuncSeparate", "glBlendFuncSeparateEXT", 0 },/* 1.4 */
            { "glFogCoordf", "glFogCoordfEXT", 0 },                /* 1.4 */
            { "glSecondaryColor3f", "glSecondaryColor3fEXT", 0 },  /* 1.4 */
            { "glPointParameterf", "glPointParameterfARB", 0 },    /* 1.4 */
            { "glMultiDrawArrays", "glMultiDrawArraysEXT", 0 },    /* 1.4 */
            { "glWindowPos2i", "glWindowPos2iARB", 0 },            /* 1.4 */
            { "glBindBuffer", "glBindBufferARB", 0 },              /* 1.5 */
            { "glGenBuffers", "glGenBuffersARB", 0 },              /* 1.5 */
            { "glBufferData", "glBufferDataARB", 0 },              /* 1.5 */
            { "glMapBuffer", "glMapBufferARB", 0 },                /* 1.5 */
            { "glGenQueries", "glGenQueriesARB", 0 },              /* 1.5 */
            { "glBeginQuery", "glBeginQueryARB", 0 },              /* 1.5 */
        };
        int i, miss = 0;
        for (i = 0; i < (int)(sizeof(es) / sizeof(es[0])); i++) {
            es[i].p = gl_sym(es[i].a, es[i].b);
            printf("  %-26s %s\n", es[i].a, es[i].p ? "present" : "ABSENT");
            if (!es[i].p) miss++;
        }
        printf("entry : %d point(s) d'entree absent(s) sur %d\n", miss,
               (int)(sizeof(es) / sizeof(es[0])));
        /* Appels réels, chacun suivi d'un carré de 16x16 dont la couleur dit
           « appel passé sans erreur GL » (vert) ou « erreur » (rouge). */
        glClearColor(0, 0, 0.25f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        {
            static const GLuint ix[3] = { 0, 1, 2 };
            static const GLfloat vp[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
            static GLubyte t3[2 * 2 * 2 * 4];
            static const GLfloat idm[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
            GLuint buf = 0, qid = 0;
            float sc[3] = { 1, 0, 0 };
            int col = 0;
            while (glGetError() != GL_NO_ERROR) { }
/* Macro variadique : le corps d'appel contient des virgules de plus haut
   niveau (listes d'initialisation), que seule `...` protège. */
#define CALLED(name, ...) do { \
            void *fp_ = gl_sym(name, name "ARB"); \
            if (!fp_) fp_ = gl_sym(name, name "EXT"); \
            if (fp_) { __VA_ARGS__; } \
            { GLenum e_ = glGetError(); int cx_ = (col % 8) * 16, cy_ = (col / 8) * 16; \
              glColor3f(fp_ && e_ == GL_NO_ERROR ? 0.0f : 1.0f, \
                        fp_ && e_ == GL_NO_ERROR ? 1.0f : 0.0f, 0.0f); \
              glBegin(GL_QUADS); \
              glVertex2f((float)cx_ + 1, (float)cy_ + 1); \
              glVertex2f((float)cx_ + 15, (float)cy_ + 1); \
              glVertex2f((float)cx_ + 15, (float)cy_ + 15); \
              glVertex2f((float)cx_ + 1, (float)cy_ + 15); \
              glEnd(); \
              printf("  appel %-26s %s (err 0x%x)\n", name, fp_ ? "ok" : "ABSENT", (unsigned)e_); \
              col++; } \
        } while (0)
            CALLED("glBlendColor", ((void (*)(GLfloat, GLfloat, GLfloat, GLfloat))fp_)(0.25f, 0.5f, 0.75f, 1.0f));
            CALLED("glBlendEquation", ((void (*)(GLenum))fp_)(0x8006));
            CALLED("glActiveTexture", ((void (*)(GLenum))fp_)(GL_TEXTURE0_ARB));
            CALLED("glMultiTexCoord2f", ((void (*)(GLenum, GLfloat, GLfloat))fp_)(GL_TEXTURE0_ARB, 0.5f, 0.5f));
            CALLED("glLoadTransposeMatrixf", ((void (*)(const GLfloat *))fp_)(idm));
            CALLED("glSampleCoverage", ((void (*)(GLfloat, GLboolean))fp_)(1.0f, GL_FALSE));
            CALLED("glBlendFuncSeparate", ((void (*)(GLenum, GLenum, GLenum, GLenum))fp_)(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO));
            CALLED("glFogCoordf", ((void (*)(GLfloat))fp_)(0.5f));
            CALLED("glSecondaryColor3f", ((void (*)(GLfloat, GLfloat, GLfloat))fp_)(sc[0], sc[1], sc[2]));
            CALLED("glPointParameterf", ((void (*)(GLenum, GLfloat))fp_)(0x8126 /* GL_POINT_SIZE_MIN */, 1.0f));
            CALLED("glWindowPos2i", ((void (*)(GLint, GLint))fp_)(2, 2));
            CALLED("glGenBuffers", ((void (*)(GLsizei, GLuint *))fp_)(1, &buf));
            CALLED("glBindBuffer", ((void (*)(GLenum, GLuint))fp_)(0x8892 /* GL_ARRAY_BUFFER */, buf));
            CALLED("glBufferData", ((void (*)(GLenum, long, const GLvoid *, GLenum))fp_)(0x8892, 64, 0, 0x88E4 /* GL_STATIC_DRAW */));
            /* Délier : avec un GL_ARRAY_BUFFER lié, un pointeur de tableau de
               sommets est un OFFSET dans le tampon (OpenGL 1.5). Laisser le
               nôtre lié faisait lire GLEngine à une adresse absurde au premier
               glDrawRangeElements — l'application est fautive, mais autant ne
               pas l'être ici. */
            CALLED("glBindBuffer", ((void (*)(GLenum, GLuint))fp_)(0x8892, 0));
            CALLED("glGenQueries", ((void (*)(GLsizei, GLuint *))fp_)(1, &qid));
            CALLED("glBeginQuery", ((void (*)(GLenum, GLuint))fp_)(0x8914, qid));
            CALLED("glEndQuery", ((void (*)(GLenum))fp_)(0x8914));
            CALLED("glDrawRangeElements", glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(3, GL_FLOAT, 0, vp); ((void (*)(GLenum, GLuint, GLuint, GLsizei, GLenum, const GLvoid *))fp_)(GL_TRIANGLES, 0, 2, 3, GL_UNSIGNED_INT, ix); glDisableClientState(GL_VERTEX_ARRAY));
            CALLED("glTexImage3D", ((void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *))fp_)(0x806F /* GL_TEXTURE_3D */, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, t3));
#undef CALLED
        }
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
            /* L'arête horizontale de ce triangle est à y = 4 ENTIER : elle tombe
               exactement sur la frontière entre les lignes 3 et 4, et deux
               rasteriseurs conformes ont chacun le droit d'en choisir une.
               GLEngine prend la 4, l'hôte la 3 (mesuré). On regarde donc les
               deux : ce que la scène veut prouver, c'est que le mode LIGNE a
               tracé une arête là où le mode PLEIN aurait rempli. */
            a = px(96, 4) != 0x000040 ? px(96, 4) : px(96, 3);
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
    } else if (!strcmp(scene, "sepspec")) {
        /* Couleur spéculaire séparée (OpenGL 1.2) : lumière directionnelle à
           diffuse noire et spéculaire blanche, matériau à spéculaire blanche
           (brillance 0 : le terme vaut 1 face à la lumière), texture 1×1 NOIRE
           en MODULATE. Couleur unique : (diffuse + spéculaire) × texture = noir.
           Séparée : diffuse × texture + spéculaire = blanc. */
        static const GLfloat zero[4] = { 0, 0, 0, 1 }, one[4] = { 1, 1, 1, 1 };
        static const GLfloat dir[4] = { 0, 0, 1, 0 };
        static const unsigned char black[4] = { 0, 0, 0, 255 };
        GLuint id;
        int k;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        glLightfv(GL_LIGHT0, GL_POSITION, dir);
        glLightfv(GL_LIGHT0, GL_AMBIENT, zero);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, zero);
        glLightfv(GL_LIGHT0, GL_SPECULAR, one);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, zero);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, zero);
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, one);
        glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 0.0f);
        glClearColor(0.2f, 0.2f, 0.2f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        for (k = 0; k < 2; k++) {
            float x0 = k ? (float)W / 2 : 0;
            glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL,
                          k ? GL_SEPARATE_SPECULAR_COLOR : GL_SINGLE_COLOR);
            glBegin(GL_QUADS);
            glNormal3f(0, 0, 1);
            glTexCoord2f(0, 0); glVertex2f(x0, 0);
            glTexCoord2f(1, 0); glVertex2f(x0 + W / 2, 0);
            glTexCoord2f(1, 1); glVertex2f(x0 + W / 2, H);
            glTexCoord2f(0, 1); glVertex2f(x0, H);
            glEnd();
        }
        glFinish();
        check("couleur unique : noir", W / 4, H / 2, 0x000000);
        check("spéculaire séparée : blanc", 3 * W / 4, H / 2, 0xffffff);
        glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SINGLE_COLOR);
        glDisable(GL_LIGHTING);
        glDisable(GL_LIGHT0);
        glDisable(GL_TEXTURE_2D);
    } else if (!strcmp(scene, "texlod")) {
        /* Niveaux et bornes de LOD (OpenGL 1.2) sur une texture 2D, que le rendu
           d'Apple tient aussi : chaque cas se compare à lui (job gpu) ET à la
           valeur attendue. Chaîne 4×4 rouge, 2×2 verte, 1×1 bleue ;
           NEAREST_MIPMAP_NEAREST. Un quad de 64 px grossit (λ < 0), un quad de
           2 px donne λ = 1, un quad de 1 px λ = 2. */
        static unsigned char red[16 * 4], green[4 * 4], blue[4];
        GLuint id[2];
        int i;
        for (i = 0; i < 16; i++) { red[i * 4] = 255; red[i * 4 + 3] = 255; }
        for (i = 0; i < 4; i++) { green[i * 4 + 1] = 255; green[i * 4 + 3] = 255; }
        blue[2] = 255; blue[3] = 255;
        glGenTextures(2, id);
        glBindTexture(GL_TEXTURE_2D, id[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
        glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
        glTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
#define LQ(x0, y0, sz) do { glBegin(GL_QUADS); \
            glTexCoord2f(0, 0); glVertex2f(x0, y0); glTexCoord2f(1, 0); glVertex2f((x0) + (sz), y0); \
            glTexCoord2f(1, 1); glVertex2f((x0) + (sz), (y0) + (sz)); \
            glTexCoord2f(0, 1); glVertex2f(x0, (y0) + (sz)); glEnd(); } while (0)
        LQ(0, 0, 64);                                   /* grossi : niveau 0 */
        LQ(70, 0, 2);                                   /* λ = 1 : niveau 1 */
        LQ(80, 0, 1);                                   /* λ = 2 : niveau 2 */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
        LQ(0, 70, 64);                                  /* grossi : niveau de base 1 */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
        LQ(90, 0, 1);                                   /* λ = 2, borné au niveau 1 */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, 2.0f);
        LQ(70, 70, 64);                                 /* λ < 0 porté à 2 : niveau 2 */
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, -1000.0f);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 0.4f);
        LQ(100, 0, 1);                                  /* λ = 2 borné à 0,4 : niveau 0 */
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 1000.0f);
        /* chaîne PARTIELLE (niveaux 0 et 1) : complète seulement avec MAX_LEVEL 1 */
        glBindTexture(GL_TEXTURE_2D, id[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
        glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
        LQ(110, 0, 2);                                  /* λ = 1 : niveau 1 */
#undef LQ
        glFinish();
        check("grossi : niveau 0", 32, 32, 0xff0000);
        check("λ=1 : niveau 1", 70, 0, 0x00ff00);
        check("λ=2 : niveau 2", 80, 0, 0x0000ff);
        check("BASE_LEVEL 1, grossi", 32, 100, 0x00ff00);
        check("MAX_LEVEL 1 à λ=2", 90, 0, 0x00ff00);
        check("MIN_LOD 2, grossi", 100, 100, 0x0000ff);
        check("MAX_LOD 0,4 à λ=2", 100, 0, 0xff0000);
        check("chaîne partielle + MAX_LEVEL 1", 110, 0, 0x00ff00);
        glDisable(GL_TEXTURE_2D);
    } else if (!strcmp(scene, "tex3d")) {
        /* Textures 3D (OpenGL 1.2, protocole v10). Le rendu d'Apple ne les
           échantillonne pas : les valeurs attendues sont calculées depuis les
           texels, R = 30·x, G = 100·y, B = 60·z + 30 (niveau 0, 8×2×4), et la
           chaîne de mipmaps est complète (4×1×2, 2×1×1, 1×1×1). À jouer avec
           et sans POMPPC_GL_GEOM=0 : les deux chemins de géométrie. */
        static unsigned char l0[4][2][8][4], l1[2][1][4][4], l2[1][1][2][4], l3[4];
        int x, yy, z;
        GLuint id;
        for (z = 0; z < 4; z++)
            for (yy = 0; yy < 2; yy++)
                for (x = 0; x < 8; x++) {
                    l0[z][yy][x][0] = (unsigned char)(30 * x);
                    l0[z][yy][x][1] = (unsigned char)(100 * yy);
                    l0[z][yy][x][2] = (unsigned char)(60 * z + 30);
                    l0[z][yy][x][3] = 255;
                }
        for (z = 0; z < 2; z++)
            for (x = 0; x < 4; x++) {
                l1[z][0][x][0] = (unsigned char)(200 - 40 * x);
                l1[z][0][x][1] = (unsigned char)(10 + 100 * z);
                l1[z][0][x][2] = 20; l1[z][0][x][3] = 255;
            }
        memset(l2, 0x40, sizeof(l2)); memset(l3, 0x80, sizeof(l3));
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_3D, id);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 8, 2, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, l0);
        glTexImage3D(GL_TEXTURE_3D, 1, GL_RGBA, 4, 1, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, l1);
        glTexImage3D(GL_TEXTURE_3D, 2, GL_RGBA, 2, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, l2);
        glTexImage3D(GL_TEXTURE_3D, 3, GL_RGBA, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, l3);
        {
            GLint mx = 0;
            GLenum err = glGetError();
            glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &mx);
            printf("GL_MAX_3D_TEXTURE_SIZE = %d ; glTexImage3D : erreur GL 0x%x\n", mx, err);
            if (err || mx < 8)
                failures++;
        }
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_3D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
#define QUAD3(r0) do { glBegin(GL_QUADS); \
            glTexCoord3f(0, 0, r0); glVertex2f(0, 0); glTexCoord3f(1, 0, r0); glVertex2f(W, 0); \
            glTexCoord3f(1, 1, r0); glVertex2f(W, H); glTexCoord3f(0, 1, r0); glVertex2f(0, H); \
            glEnd(); glFinish(); } while (0)
        /* centre du texel (x, y) de la tranche : pixel (W·(x+½)/8, H·(y+½)/2) */
#define TX(xx) ((int)(W * ((xx) + 0.5f) / 8))
#define TY(yv) ((int)(H * ((yv) + 0.5f) / 2))
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        QUAD3(0.3f);                            /* tranche 1 : B = 90 */
        check("r=0,3 texel (3,0,1)", TX(3), TY(0), 0x5a005a);
        check("r=0,3 texel (5,1,1)", TX(5), TY(1), 0x96645a);
        QUAD3(0.9f);                            /* tranche 3 : B = 210 */
        check("r=0,9 texel (0,1,3)", TX(0), TY(1), 0x0064d2);
        check("r=0,9 texel (7,0,3)", TX(7), TY(0), 0xd200d2);
        QUAD3(1.3f);                            /* REPEAT : 5,2 → tranche 1 */
        check("REPEAT r=1,3 → tranche 1", TX(2), TY(0), 0x3c005a);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        QUAD3(1.3f);                            /* CLAMP_TO_EDGE : tranche 3 */
        check("CLAMP_TO_EDGE r=1,3 → tranche 3", TX(2), TY(0), 0x3c00d2);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        QUAD3(0.5f);                            /* entre les tranches 1 et 2 : B = 120 */
        {
            unsigned long c = px(TX(4), TY(0));
            int b = (int)(c & 255), r = (int)(c >> 16);
            printf("  %s %-28s (%3d,%3d) = %06lx\n",
                   (b >= 118 && b <= 122 && r >= 116 && r <= 124) ? "ok  " : "FAIL",
                   "linéaire entre tranches 1-2", TX(4), TY(0), c);
            if (!(b >= 118 && b <= 122 && r >= 116 && r <= 124))
                failures++;
        }
        /* mipmaps : un quad de 4×1 pixels couvrant la texture → λ = 1, niveau 1
           (4×1×2), r = 0,3 → tranche 0 : texel x → (200 − 40x, 10, 20) */
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glTexCoord3f(0, 0, 0.3f); glVertex2f(0, 0); glTexCoord3f(1, 0, 0.3f); glVertex2f(4, 0);
        glTexCoord3f(1, 1, 0.3f); glVertex2f(4, 1); glTexCoord3f(0, 1, 0.3f); glVertex2f(0, 1);
        glEnd();
        glFinish();
        check("mipmap niveau 1, texel 0", 0, 0, 0xc80a14);
        check("mipmap niveau 1, texel 2", 2, 0, 0x780a14);
#undef QUAD3
#undef TX
#undef TY
        glDisable(GL_TEXTURE_3D);
    } else if (!strcmp(scene, "tex13")) {
        /* Répétitions et compression d'OpenGL 1.3/1.4, relayées à l'hôte v10 :
           GL_CLAMP_TO_BORDER et GL_CLAMP avec la couleur de bordure,
           GL_MIRRORED_REPEAT, S3TC (DXT1, DXT1 à alpha, DXT3, DXT5, mipmaps,
           sous-image, carte de cube, format générique compressé par GLEngine).
           Chaque cas est un carré de 16 px à coordonnée de texture CONSTANTE
           (le texel échantillonné est exact), sauf les mipmaps. Valeurs
           attendues : la règle d'OpenGL, et pour les couleurs interpolées de
           S3TC, l'arrondi du décodeur de l'hôte (qgpu-core.c, dxt_block),
           choisi exact quand c'est possible. Le rendu d'Apple échoue presque
           partout : il ignore la bordure et le miroir, et ne décode pas S3TC
           reçu par glCompressedTexImage2D. */
        typedef void (*cti_f)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
                              const GLvoid *);
        typedef void (*ctsi_f)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                               GLsizei, const GLvoid *);
        cti_f cti = (cti_f)gl_sym("glCompressedTexImage2D", "glCompressedTexImage2DARB");
        ctsi_f ctsi = (ctsi_f)gl_sym("glCompressedTexSubImage2D", "glCompressedTexSubImage2DARB");
        static const GLfloat bc[4] = { 0.2f, 0.6f, 1.0f, 1.0f };      /* → 3399ff */
        static const unsigned char q4[2 * 2 * 4] = { 255, 0, 0, 255,  0, 255, 0, 255,
                                                     0, 0, 255, 255,  255, 255, 255, 255 };
        static const unsigned char wh[4] = { 255, 255, 255, 255 };
        static const unsigned char rb[2 * 4] = { 255, 0, 0, 255,  0, 0, 255, 255 };
        /* DXT1, c0 rouge > c1 bleu : quatre couleurs, texels 0,1,2,3 par ligne */
        static const unsigned char d1[8] = { 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 };
        /* DXT1 à alpha, c0 (r5 = 4) < c1 (r5 = 28) : trois couleurs + transparent */
        static const unsigned char d1a[8] = { 0x00, 0x20, 0x00, 0xE0, 0xE4, 0xE4, 0xE4, 0xE4 };
        /* DXT3 blanc, alphas 15, 0, 8, 3 par ligne */
        static const unsigned char d3[16] = { 0x0F, 0x38, 0x0F, 0x38, 0x0F, 0x38, 0x0F, 0x38,
                                              0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 };
        /* DXT5 blanc, a0 = 252 > a1 = 0, codes 0, 1, 2, 7 par ligne */
        static const unsigned char d5[16] = { 0xFC, 0x00, 0x88, 0x8E, 0xE8, 0x88, 0x8E, 0xE8,
                                              0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 };
        static unsigned char mip[4][4 * 8], blk[8], mag[4 * 4 * 3];
        static const unsigned short mipc[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
        GLuint id[13];
        int i, l;
        if (!cti || !ctsi) {
            printf("  glCompressedTexImage2D absent\n");
            return 1;
        }
        glGenTextures(13, id);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
#define CQ(x0, y0, s, t) do { glBegin(GL_QUADS); glTexCoord2f(s, t); \
            glVertex2f(x0, y0); glVertex2f((x0) + 16, y0); \
            glVertex2f((x0) + 16, (y0) + 16); glVertex2f(x0, (y0) + 16); glEnd(); } while (0)
#define T2(k, filt, wrap) do { glBindTexture(GL_TEXTURE_2D, id[k]); \
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt); \
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt); \
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap); \
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap); \
            glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, bc); } while (0)
        /* ── rangée 0 : bordure et miroir ── */
        T2(0, GL_NEAREST, 0x812D);                          /* CLAMP_TO_BORDER */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, q4);
        CQ(0, 0, -0.5f, 0.25f);
        CQ(20, 0, 0.25f, 0.25f);
        T2(1, GL_LINEAR, 0x812D);                           /* 1×1 blanc, linéaire */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, wh);
        CQ(40, 0, 0.0f, 0.5f);
        T2(2, GL_LINEAR, GL_CLAMP);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, wh);
        CQ(60, 0, 0.0f, 0.5f);
        T2(3, GL_LINEAR, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, wh);
        CQ(80, 0, 0.0f, 0.5f);
        T2(4, GL_NEAREST, 0x8370);                          /* MIRRORED_REPEAT */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rb);
        CQ(100, 0, 1.25f, 0.5f);
        CQ(120, 0, -0.25f, 0.5f);
        CQ(140, 0, 2.25f, 0.5f);
        printf("  [tex13] rangée 1\n");
        /* ── rangée 1 : DXT1 quatre couleurs ; DXT1 à alpha, test d'alpha ── */
        T2(5, GL_NEAREST, GL_REPEAT);
        cti(GL_TEXTURE_2D, 0, 0x83F0, 4, 4, 0, 8, d1);
        for (i = 0; i < 4; i++)
            CQ(20 * i, 40, (i + 0.5f) / 4, 0.5f);
        glDisable(GL_TEXTURE_2D);
        glColor3ub(0x40, 0x40, 0x40);
        glRecti(80, 40, 156, 56);                           /* fond des cas 1.4–1.7 */
        glColor3ub(255, 255, 255);
        glEnable(GL_TEXTURE_2D);
        T2(6, GL_NEAREST, GL_REPEAT);
        cti(GL_TEXTURE_2D, 0, 0x83F1, 4, 4, 0, 8, d1a);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.5f);
        for (i = 0; i < 4; i++)
            CQ(80 + 20 * i, 40, (i + 0.5f) / 4, 0.5f);
        glDisable(GL_ALPHA_TEST);
        printf("  [tex13] rangée 2\n");
        /* ── rangée 2 : DXT3 et DXT5 blancs, mélangés sur le noir ── */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        T2(7, GL_NEAREST, GL_REPEAT);
        cti(GL_TEXTURE_2D, 0, 0x83F2, 4, 4, 0, 16, d3);
        for (i = 0; i < 4; i++)
            CQ(20 * i, 80, (i + 0.5f) / 4, 0.5f);
        T2(8, GL_NEAREST, GL_REPEAT);
        cti(GL_TEXTURE_2D, 0, 0x83F3, 4, 4, 0, 16, d5);
        for (i = 0; i < 4; i++)
            CQ(80 + 20 * i, 80, (i + 0.5f) / 4, 0.5f);
        glDisable(GL_BLEND);
        printf("  [tex13] rangée 3\n");
        /* ── rangée 3 : mipmaps DXT1 8×8 rouge, 4×4 vert, 2×2 bleu, 1×1 blanc ── */
        for (l = 0; l < 4; l++)
            for (i = 0; i < 4; i++) {
                mip[l][i * 8 + 0] = mip[l][i * 8 + 2] = (unsigned char)(mipc[l] & 0xFF);
                mip[l][i * 8 + 1] = mip[l][i * 8 + 3] = (unsigned char)(mipc[l] >> 8);
            }
        T2(9, GL_NEAREST, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        for (l = 0; l < 4; l++)
            cti(GL_TEXTURE_2D, l, 0x83F0, 8 >> l, 8 >> l, 0, l ? 8 : 32, mip[l]);
        if (getenv("TEX13_STEP")) { glFinish(); printf("  [tex13] mips chargées\n"); }
#define LQ(x0, y0, sz) do { glBegin(GL_QUADS); \
            glTexCoord2f(0, 0); glVertex2f(x0, y0); glTexCoord2f(1, 0); glVertex2f((x0) + (sz), y0); \
            glTexCoord2f(1, 1); glVertex2f((x0) + (sz), (y0) + (sz)); \
            glTexCoord2f(0, 1); glVertex2f(x0, (y0) + (sz)); glEnd(); } while (0)
        LQ(0, 120, 16);                                     /* grossi : niveau 0 */
        LQ(20, 120, 4);                                     /* λ = 1 */
        LQ(40, 120, 2);                                     /* λ = 2 */
        LQ(60, 120, 1);                                     /* λ = 3 */
#undef LQ
        if (getenv("TEX13_STEP")) { glFinish(); printf("  [tex13] mips dessinées\n"); }
        /* format générique : GLEngine compresse lui-même (DXT1) */
        for (i = 0; i < 16; i++) {
            mag[i * 3] = 255; mag[i * 3 + 1] = 0; mag[i * 3 + 2] = 255;
        }
        T2(10, GL_NEAREST, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x84ED /* COMPRESSED_RGB */, 4, 4, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, mag);
        CQ(80, 120, 0.5f, 0.5f);
        if (getenv("TEX13_STEP")) { glFinish(); printf("  [tex13] générique dessinée\n"); }
        /* sous-image compressée : 8×8 rouge, bloc (4,0) remplacé par du bleu */
        T2(11, GL_NEAREST, GL_REPEAT);
        cti(GL_TEXTURE_2D, 0, 0x83F0, 8, 8, 0, 32, mip[0]);
        CQ(100, 120, 5.5f / 8, 1.5f / 8);                   /* dessiné AVANT : rouge */
        glFinish();
        blk[0] = 0x1F; blk[1] = 0x00; blk[2] = 0x1F; blk[3] = 0x00;
        ctsi(GL_TEXTURE_2D, 0, 4, 0, 4, 4, 0x83F0, 8, blk);
        CQ(120, 120, 5.5f / 8, 1.5f / 8);
        CQ(140, 120, 1.5f / 8, 1.5f / 8);
        glDisable(GL_TEXTURE_2D);
        printf("  [tex13] rangée 4\n");
        /* ── rangée 4 : carte de cube DXT1, face +X rouge, les autres bleues ── */
        glBindTexture(GL_TEXTURE_CUBE_MAP, id[12]);
        for (i = 0; i < 6; i++) {
            unsigned short c = i ? 0x001F : 0xF800;
            blk[0] = blk[2] = (unsigned char)(c & 0xFF);
            blk[1] = blk[3] = (unsigned char)(c >> 8);
            cti(0x8515 + i, 0, 0x83F0, 4, 4, 0, 8, blk);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_CUBE_MAP);
        glBegin(GL_QUADS);
        glTexCoord3f(1, 0.1f, 0.2f);
        glVertex2f(0, 160); glVertex2f(16, 160); glVertex2f(16, 176); glVertex2f(0, 176);
        glTexCoord3f(0.1f, 0.2f, 1);
        glVertex2f(20, 160); glVertex2f(36, 160); glVertex2f(36, 176); glVertex2f(20, 176);
        glEnd();
        glDisable(GL_TEXTURE_CUBE_MAP);
#undef CQ
#undef T2
        glFinish();
        printf("  erreur GL 0x%x\n", glGetError());
        check("CLAMP_TO_BORDER : bordure", 8, 8, 0x3399ff);
        check("CLAMP_TO_BORDER : dedans", 28, 8, 0xff0000);
        check("TO_BORDER linéaire au bord", 48, 8, 0x99ccff);
        check("GL_CLAMP linéaire au bord", 68, 8, 0x99ccff);
        check("CLAMP_TO_EDGE (témoin)", 88, 8, 0xffffff);
        check("MIRRORED s=1,25", 108, 8, 0x0000ff);
        check("MIRRORED s=-0,25", 128, 8, 0xff0000);
        check("MIRRORED s=2,25", 148, 8, 0xff0000);
        check("DXT1 texel 0", 8, 48, 0xff0000);
        check("DXT1 texel 1", 28, 48, 0x0000ff);
        check("DXT1 texel 2 (2/3)", 48, 48, 0xaa0055);
        check("DXT1 texel 3 (1/3)", 68, 48, 0x5500aa);
        check("DXT1A texel 0", 88, 48, 0x210000);
        check("DXT1A texel 1", 108, 48, 0xe70000);
        check("DXT1A texel 2 (1/2)", 128, 48, 0x840000);
        check("DXT1A texel 3 transparent", 148, 48, 0x404040);
        check("DXT3 alpha 15", 8, 88, 0xffffff);
        check("DXT3 alpha 0", 28, 88, 0x000000);
        check("DXT3 alpha 8", 48, 88, 0x888888);
        check("DXT3 alpha 3", 68, 88, 0x333333);
        check("DXT5 alpha a0", 88, 88, 0xfcfcfc);
        check("DXT5 alpha a1", 108, 88, 0x000000);
        check("DXT5 alpha code 2", 128, 88, 0xd8d8d8);
        check("DXT5 alpha code 7", 148, 88, 0x242424);
        check("DXT1 mip grossi : niveau 0", 8, 128, 0xff0000);
        check("DXT1 mip λ=1 : 4×4", 20, 120, 0x00ff00);
        check("DXT1 mip λ=2 : 2×2", 40, 120, 0x0000ff);
        check("DXT1 mip λ=3 : 1×1", 60, 120, 0xffffff);
        check("COMPRESSED_RGB générique", 88, 128, 0xff00ff);
        check("sous-image : avant", 108, 128, 0xff0000);
        check("sous-image : après", 128, 128, 0x0000ff);
        check("sous-image : bloc intact", 148, 128, 0xff0000);
        check("cube DXT1 face +X", 8, 168, 0xff0000);
        check("cube DXT1 face +Z", 28, 168, 0x0000ff);
    } else if (!strcmp(scene, "cube")) {
        /* Cartes de cube (OpenGL 1.3, protocole v10). Le rendu d'Apple ne les
           tient pas : valeurs attendues de la table 3.21 d'OpenGL, dont le GPU
           hôte a déjà confirmé l'orientation (run_v10 (e)). Texture A : une
           couleur par face ; texture B : face +X à quatre texels distincts. */
        static const unsigned long fc[6] = { 0xff0000, 0x00ff00, 0x0000ff,
                                             0xffff00, 0xff00ff, 0x00ffff };
        static const float dir[6][3] = { { 1, .1f, .2f }, { -1, .2f, .1f }, { .1f, 1, .2f },
                                         { .2f, -1, .1f }, { .1f, .2f, 1 }, { .2f, .1f, -1 } };
        static const unsigned long px4[4] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };
        unsigned char face[4 * 4];
        GLuint id[2];
        int f, i, k;
        glGenTextures(2, id);
        for (k = 0; k < 2; k++) {
            glBindTexture(GL_TEXTURE_CUBE_MAP, id[k]);
            for (f = 0; f < 6; f++) {
                for (i = 0; i < 4; i++) {
                    unsigned long c = k ? (f ? 0 : px4[i]) : fc[f];
                    face[i * 4] = (unsigned char)(c >> 16);
                    face[i * 4 + 1] = (unsigned char)(c >> 8);
                    face[i * 4 + 2] = (unsigned char)c;
                    face[i * 4 + 3] = 255;
                }
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, face);
            }
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        {
            GLint mx = 0;
            GLenum err = glGetError();
            glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &mx);
            printf("GL_MAX_CUBE_MAP_TEXTURE_SIZE = %d ; faces : erreur GL 0x%x\n", mx, err);
            if (err || mx < 2)
                failures++;
        }
        glEnable(GL_TEXTURE_CUBE_MAP);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glClearColor(0, 0, 0, 1);
#define BAND(y0, y1, dx, dy, dz) do { glBegin(GL_QUADS); \
            glTexCoord3f(dx, dy, dz); glVertex2f(0, y0); glTexCoord3f(dx, dy, dz); glVertex2f(W, y0); \
            glTexCoord3f(dx, dy, dz); glVertex2f(W, y1); glTexCoord3f(dx, dy, dz); glVertex2f(0, y1); \
            glEnd(); } while (0)
        glBindTexture(GL_TEXTURE_CUBE_MAP, id[0]);
        glClear(GL_COLOR_BUFFER_BIT);
        for (f = 0; f < 6; f++)
            BAND(H * f / 6.0f, H * (f + 1) / 6.0f, dir[f][0], dir[f][1], dir[f][2]);
        glFinish();
        {
            static const char *nm[6] = { "face +X", "face -X", "face +Y", "face -Y",
                                         "face +Z", "face -Z" };
            for (f = 0; f < 6; f++)
                check(nm[f], W / 2, (int)(H * (f + 0.5f) / 6), fc[f]);
        }
        glBindTexture(GL_TEXTURE_CUBE_MAP, id[1]);
        glClear(GL_COLOR_BUFFER_BIT);
        BAND(0, H / 4.0f, 1, 0.5f, 0.5f);
        BAND(H / 4.0f, H / 2.0f, 1, 0.5f, -0.5f);
        BAND(H / 2.0f, 3 * H / 4.0f, 1, -0.5f, 0.5f);
        BAND(3 * H / 4.0f, H, 1, -0.5f, -0.5f);
        glFinish();
        check("orientation +X : (0,0)", W / 2, H / 8, 0xff0000);
        check("orientation +X : (1,0)", W / 2, 3 * H / 8, 0x00ff00);
        check("orientation +X : (0,1)", W / 2, 5 * H / 8, 0x0000ff);
        check("orientation +X : (1,1)", W / 2, 7 * H / 8, 0xffffff);
#undef BAND
        /* génération GL_NORMAL_MAP : normale −X → face −X (verte) */
        glBindTexture(GL_TEXTURE_CUBE_MAP, id[0]);
        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_ARB);
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_ARB);
        glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_ARB);
        glEnable(GL_TEXTURE_GEN_S); glEnable(GL_TEXTURE_GEN_T); glEnable(GL_TEXTURE_GEN_R);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glNormal3f(-1, 0, 0);
        glVertex2f(0, 0); glVertex2f(W, 0); glVertex2f(W, H); glVertex2f(0, H);
        glEnd();
        glFinish();
        check("GL_NORMAL_MAP, normale -X", W / 2, H / 2, 0x00ff00);
        /* génération GL_REFLECTION_MAP : oeil en (x, y, 0) avec x ≫ y, normale
           +X → r = u − 2n(n·u) = (−x, y, 0)/|u| : face −X (verte). NORMAL_MAP
           ou la position seule donneraient +X (rouge). */
        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, 0x8512 /* REFLECTION_MAP */);
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, 0x8512);
        glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, 0x8512);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glNormal3f(1, 0, 0);
        glVertex2f(W * 0.75f, 0); glVertex2f(W, 0);
        glVertex2f(W, H / 16.0f); glVertex2f(W * 0.75f, H / 16.0f);
        glEnd();
        glFinish();
        check("GL_REFLECTION_MAP, normale +X", W * 7 / 8, H / 32, 0x00ff00);
        glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T); glDisable(GL_TEXTURE_GEN_R);
        glDisable(GL_TEXTURE_CUBE_MAP);
    } else if (!strcmp(scene, "tex14")) {
        /* OpenGL 1.4 au pixel (docs/re/opengl-1.4.md) : couleur secondaire,
           biais de LOD de texture et d'unité, textures de profondeur et
           comparaison d'ombre, stencil à enveloppement (GLTEST_STENCIL=1),
           sources croisées (crossbar). Valeurs choisies exactes sur 8 bits.
           À jouer avec et sans POMPPC_GL_GEOM=0 (les deux chemins). */
        typedef void (*sc3_f)(GLfloat, GLfloat, GLfloat);
        typedef void (*scp_f)(GLint, GLenum, GLsizei, const GLvoid *);
        typedef void (*at_f)(GLenum);
        sc3_f sc3 = (sc3_f)gl_sym("glSecondaryColor3f", "glSecondaryColor3fEXT");
        scp_f scp = (scp_f)gl_sym("glSecondaryColorPointer", "glSecondaryColorPointerEXT");
        at_f at = (at_f)gl_sym("glActiveTexture", "glActiveTextureARB");
        static const unsigned char blk[4] = { 0, 0, 0, 255 };
        static const unsigned char red[4] = { 255, 0, 0, 255 };
        static const unsigned char grn[4] = { 0, 255, 0, 255 };
        static const unsigned char cyn[4] = { 153, 255, 255, 255 };
        static unsigned char m0[4 * 4 * 4], m1[2 * 2 * 4], m2[4];
        static const GLfloat dz[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
        static const GLfloat one[4] = { 1, 1, 1, 1 }, amb[4] = { 0.2f, 0.2f, 0.2f, 1 };
        static const GLfloat zero[4] = { 0, 0, 0, 1 };
        GLfloat vx[4 * 2], sec[4 * 3], mx = -1;
        GLint sbits = 0;
        GLuint id[6];
        int i;
        if (!sc3 || !scp || !at) {
            printf("  points d'entrée 1.4 absents\n");
            return 1;
        }
        for (i = 0; i < 16; i++) { m0[i * 4] = 255; m0[i * 4 + 3] = 255; }
        for (i = 0; i < 4; i++) { m1[i * 4 + 1] = 255; m1[i * 4 + 3] = 255; }
        m2[2] = 255; m2[3] = 255;
        glGenTextures(6, id);
        glClearColor(0, 0, 0, 1);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
#define SQ(x0, y0) glRecti(x0, y0, (x0) + 16, (y0) + 16)
#define TQ(x0, y0, s, t, r) do { glBegin(GL_QUADS); glTexCoord3f(s, t, r); \
            glVertex2f(x0, y0); glVertex2f((x0) + 16, y0); \
            glVertex2f((x0) + 16, (y0) + 16); glVertex2f(x0, (y0) + 16); glEnd(); } while (0)
#define LQ(x0, y0, sz) do { glBegin(GL_QUADS); \
            glTexCoord2f(0, 0); glVertex2f(x0, y0); glTexCoord2f(1, 0); glVertex2f((x0) + (sz), y0); \
            glTexCoord2f(1, 1); glVertex2f((x0) + (sz), (y0) + (sz)); \
            glTexCoord2f(0, 1); glVertex2f(x0, (y0) + (sz)); glEnd(); } while (0)
        /* ── rangée 0 : couleur secondaire ── */
        glEnable(0x8458 /* GL_COLOR_SUM */);
        glColor3f(0, 0.4f, 0);
        sc3(0.6f, 0, 0);
        SQ(0, 0);                                           /* valeur courante */
        for (i = 0; i < 4; i++) {
            sec[i * 3] = 0.2f; sec[i * 3 + 1] = 0; sec[i * 3 + 2] = 0.4f;
        }
        vx[0] = 20; vx[1] = 0; vx[2] = 36; vx[3] = 0; vx[4] = 36; vx[5] = 16; vx[6] = 20; vx[7] = 16;
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(0x845E /* SECONDARY_COLOR_ARRAY */);
        glVertexPointer(2, GL_FLOAT, 0, vx);
        scp(3, GL_FLOAT, 0, sec);
        glDrawArrays(GL_QUADS, 0, 4);                       /* tableau */
        glDisableClientState(0x845E);
        glDisableClientState(GL_VERTEX_ARRAY);
        glBindTexture(GL_TEXTURE_2D, id[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blk);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glColor3f(1, 1, 1);
        sc3(0.2f, 0.4f, 0.6f);
        TQ(40, 0, 0.5f, 0.5f, 0);                           /* après la texture */
        glDisable(GL_TEXTURE_2D);
        glDisable(0x8458);
        glColor3f(0, 0.4f, 0);
        sc3(0.6f, 0, 0);
        SQ(60, 0);                                          /* COLOR_SUM coupée */
        glEnable(0x8458);
        glEnable(GL_LIGHTING);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, one);
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero);
        glNormal3f(0, 0, 1);
        SQ(80, 0);                                          /* éclairée : ignorée */
        glDisable(GL_LIGHTING);
        glDisable(0x8458);
        sc3(0, 0, 0);
        /* ── rangée 1 : biais de LOD (chaîne 4×4 rouge, 2×2 verte, 1×1 bleue) ── */
        glBindTexture(GL_TEXTURE_2D, id[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, m0);
        glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, m1);
        glTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, m2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glGetFloatv(0x84FD /* MAX_TEXTURE_LOD_BIAS */, &mx);
        LQ(0, 40, 4);                                       /* λ = 0 : niveau 0 */
        glTexParameterf(GL_TEXTURE_2D, 0x8501, 1.0f);
        LQ(20, 40, 4);                                      /* +1 texture */
        glTexParameterf(GL_TEXTURE_2D, 0x8501, 0.0f);
        glTexEnvf(0x8500, 0x8501, 2.0f);
        LQ(40, 40, 4);                                      /* +2 unité */
        glTexParameterf(GL_TEXTURE_2D, 0x8501, 1.0f);
        glTexEnvf(0x8500, 0x8501, 1.0f);
        LQ(60, 40, 4);                                      /* +1 +1 */
        glTexEnvf(0x8500, 0x8501, 0.0f);
        glTexParameterf(GL_TEXTURE_2D, 0x8501, -1.0f);
        LQ(80, 40, 2);                                      /* λ = 1, −1 texture */
        glTexParameterf(GL_TEXTURE_2D, 0x8501, 0.0f);
        glTexEnvf(0x8500, 0x8501, -2.0f);
        LQ(100, 40, 1);                                     /* λ = 2, −2 unité */
        glTexEnvf(0x8500, 0x8501, 0.0f);
        LQ(120, 40, 1);                                     /* λ = 2, sans biais */
        /* ── rangée 2 : texture de profondeur 2×2 : 0,2 0,4 / 0,6 0,8 ── */
        glBindTexture(GL_TEXTURE_2D, id[2]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 2, 2, 0, GL_DEPTH_COMPONENT,
                     GL_FLOAT, dz);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        TQ(0, 80, 0.25f, 0.25f, 0.5f);                      /* D = 0,2 en luminance */
        TQ(20, 80, 0.75f, 0.75f, 0.5f);                     /* D = 0,8 */
        glTexParameteri(GL_TEXTURE_2D, 0x884C, 0x884E);     /* COMPARE_R_TO_TEXTURE */
        TQ(40, 80, 0.25f, 0.25f, 0.5f);                     /* 0,5 ≤ 0,2 : 0 */
        TQ(60, 80, 0.25f, 0.75f, 0.5f);                     /* 0,5 ≤ 0,6 : 1 */
        glTexParameteri(GL_TEXTURE_2D, 0x884D, GL_GEQUAL);
        TQ(80, 80, 0.25f, 0.25f, 0.5f);                     /* 0,5 ≥ 0,2 : 1 */
        glTexParameteri(GL_TEXTURE_2D, 0x884D, GL_LEQUAL);
        glTexParameteri(GL_TEXTURE_2D, 0x884C, GL_NONE);
        glTexParameteri(GL_TEXTURE_2D, 0x884B, GL_ALPHA);   /* DEPTH_TEXTURE_MODE */
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.7f);
        glColor3f(0, 0, 1);
        TQ(100, 80, 0.75f, 0.75f, 0.5f);                    /* A = 0,8 : passe, bleu */
        TQ(120, 80, 0.25f, 0.25f, 0.5f);                    /* A = 0,2 : rejeté */
        glDisable(GL_ALPHA_TEST);
        glColor3f(1, 1, 1);
        glTexParameteri(GL_TEXTURE_2D, 0x884B, GL_INTENSITY);
        glTexParameteri(GL_TEXTURE_2D, 0x884C, 0x884E);
        TQ(140, 80, 0.25f, 0.75f, 0.5f);                    /* I = (0,5 ≤ 0,6) = 1 */
        glDisable(GL_TEXTURE_2D);
        /* ── rangée 3 : stencil à enveloppement ── */
        glGetIntegerv(GL_STENCIL_BITS, &sbits);
        if (sbits >= 8) {
            glEnable(GL_STENCIL_TEST);
            glColorMask(0, 0, 0, 0);
            glStencilFunc(GL_ALWAYS, 0, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, 0x8508 /* DECR_WRAP */);
            SQ(0, 120);                                     /* 0 → 255 */
            glStencilFunc(GL_ALWAYS, 255, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            SQ(20, 120);
            glStencilOp(GL_KEEP, GL_KEEP, 0x8507 /* INCR_WRAP */);
            SQ(20, 120);                                    /* 255 → 0 */
            glColorMask(1, 1, 1, 1);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glStencilFunc(GL_EQUAL, 255, 0xFF);
            glColor3f(1, 0, 0);
            SQ(0, 120);
            glStencilFunc(GL_EQUAL, 0, 0xFF);
            glColor3f(0, 1, 0);
            SQ(20, 120);
            glDisable(GL_STENCIL_TEST);
        }
        /* ── rangée 4 : crossbar ── */
        at(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, id[4]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, grn);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, 0x8570 /* COMBINE */);
        glTexEnvi(GL_TEXTURE_ENV, 0x8571 /* COMBINE_RGB */, GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, 0x8580 /* SOURCE0_RGB */, 0x8578 /* PREVIOUS */);
        at(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, id[3]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, 0x8570);
        glTexEnvi(GL_TEXTURE_ENV, 0x8571, GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, 0x8580, GL_TEXTURE1);
        TQ(0, 160, 0.5f, 0.5f, 0);                          /* unité 0 lit l'unité 1 */
        at(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, id[5]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, cyn);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        at(GL_TEXTURE0);
        glTexEnvi(GL_TEXTURE_ENV, 0x8571, GL_MODULATE);
        glTexEnvi(GL_TEXTURE_ENV, 0x8580, GL_TEXTURE0);
        glTexEnvi(GL_TEXTURE_ENV, 0x8581 /* SOURCE1_RGB */, GL_TEXTURE1);
        TQ(20, 160, 0.5f, 0.5f, 0);                         /* rouge × (0,6 1 1) */
        glTexEnvi(GL_TEXTURE_ENV, 0x8581, 0x8578);
        glTexEnvi(GL_TEXTURE_ENV, 0x8580, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        at(GL_TEXTURE1);
        glDisable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        at(GL_TEXTURE0);
        glDisable(GL_TEXTURE_2D);
        /* ── rangée 5 : paramètres de point, mélange au carré, glMultiDrawArrays ── */
        {
            typedef void (*ppf_f)(GLenum, GLfloat);
            typedef void (*ppfv_f)(GLenum, const GLfloat *);
            typedef void (*mda_f)(GLenum, const GLint *, const GLsizei *, GLsizei);
            ppf_f ppf = (ppf_f)gl_sym("glPointParameterf", "glPointParameterfARB");
            ppfv_f ppfv = (ppfv_f)gl_sym("glPointParameterfv", "glPointParameterfvARB");
            mda_f mda = (mda_f)gl_sym("glMultiDrawArrays", "glMultiDrawArraysEXT");
            /* œil = fenêtre (modèle-vue identité) : le point en (120, 160) est à
               d = 200 ; c = 1/10000 → facteur sqrt(1/4) = 1/2 */
            static const GLfloat att[3] = { 0, 0, 1.0f / 10000.0f };
            static const GLfloat att1[3] = { 1, 0, 0 };
            static GLfloat mv[8 * 2];
            static const GLint first[2] = { 0, 4 };
            static const GLsizei cnt[2] = { 4, 4 };
            glColor3f(1, 1, 1);
            glPointSize(16);
            if (ppfv) ppfv(0x8129 /* POINT_DISTANCE_ATTENUATION */, att);
            glBegin(GL_POINTS); glVertex2f(120, 160); glEnd();     /* taille 8 */
            if (ppf) ppf(0x8127 /* POINT_SIZE_MAX */, 4.0f);
            glBegin(GL_POINTS); glVertex2f(160, 120); glEnd();     /* 8 borné à 4 */
            if (ppf) ppf(0x8127, 64.0f);
            if (ppf) ppf(0x8126 /* POINT_SIZE_MIN */, 12.0f);
            glBegin(GL_POINTS); glVertex2f(200, 80); glEnd();      /* d≈216 : porté à 12 */
            if (ppf) ppf(0x8126, 0.0f);
            if (ppfv) ppfv(0x8129, att1);
            glPointSize(1);
            /* mélange au carré (1.4) : source 0,8 → 0,64 ; destination 0,8 → 0,64 */
            glColor3f(0.8f, 0.8f, 0.8f);
            SQ(180, 160);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_COLOR, GL_ZERO);
            glColor3f(0.8f, 0.8f, 0.8f);
            SQ(160, 160);                                   /* 0,8 × 0,8 */
            glBlendFunc(GL_ZERO, GL_DST_COLOR);
            SQ(180, 160);                                   /* fond 0,8 × 0,8 */
            glBlendFunc(GL_ONE, GL_ZERO);
            glDisable(GL_BLEND);
            /* glMultiDrawArrays : deux carrés d'un appel */
            for (i = 0; i < 2; i++) {
                float x0 = 200 + 20 * i;
                mv[i * 8 + 0] = x0; mv[i * 8 + 1] = 160; mv[i * 8 + 2] = x0 + 16;
                mv[i * 8 + 3] = 160; mv[i * 8 + 4] = x0 + 16; mv[i * 8 + 5] = 176;
                mv[i * 8 + 6] = x0; mv[i * 8 + 7] = 176;
            }
            glColor3f(1, 0, 1);
            glEnableClientState(GL_VERTEX_ARRAY);
            glVertexPointer(2, GL_FLOAT, 0, mv);
            if (mda) mda(GL_QUADS, first, cnt, 2);
            glDisableClientState(GL_VERTEX_ARRAY);
            glColor3f(1, 1, 1);
        }
#undef SQ
#undef TQ
#undef LQ
        glFinish();
        printf("  erreur GL 0x%x, GL_MAX_TEXTURE_LOD_BIAS = %g, stencil %d bits\n",
               glGetError(), mx, sbits);
        if (mx < 2.0f) {
            printf("  FAIL GL_MAX_TEXTURE_LOD_BIAS < 2 (1.4 exige au moins 2)\n");
            failures++;
        }
        check("secondaire courante", 8, 8, 0x996600);
        check("secondaire en tableau", 28, 8, 0x336666);
        check("secondaire après texture", 48, 8, 0x336699);
        check("COLOR_SUM coupée", 68, 8, 0x006600);
        check("éclairée : secondaire ignorée", 88, 8, 0x333333);
        check("λ=0 sans biais : niveau 0", 1, 41, 0xff0000);
        check("biais texture +1", 21, 41, 0x00ff00);
        check("biais unité +2", 41, 41, 0x0000ff);
        check("biais texture +1 unité +1", 61, 41, 0x0000ff);
        check("λ=1, biais texture −1", 80, 40, 0xff0000);
        check("λ=2, biais unité −2", 100, 40, 0xff0000);
        check("λ=2 sans biais (témoin)", 120, 40, 0x0000ff);
        check("profondeur 0,2 en luminance", 8, 88, 0x333333);
        check("profondeur 0,8 en luminance", 28, 88, 0xcccccc);
        check("ombre LEQUAL 0,5≤0,2 : 0", 48, 88, 0x000000);
        check("ombre LEQUAL 0,5≤0,6 : 1", 68, 88, 0xffffff);
        check("ombre GEQUAL 0,5≥0,2 : 1", 88, 88, 0xffffff);
        check("mode ALPHA, A=0,8 passe", 108, 88, 0x0000ff);
        check("mode ALPHA, A=0,2 rejeté", 128, 88, 0x000000);
        check("mode INTENSITY + ombre : 1", 148, 88, 0xffffff);
        if (sbits >= 8) {
            check("DECR_WRAP 0 → 255", 8, 128, 0xff0000);
            check("INCR_WRAP 255 → 0", 28, 128, 0x00ff00);
        } else {
            printf("  (pas de stencil : GLTEST_STENCIL=1 pour les cas d'enveloppement)\n");
        }
        check("crossbar : REPLACE TEXTURE1", 8, 168, 0x00ff00);
        check("crossbar : TEXTURE0 × TEXTURE1", 28, 168, 0x990000);
        /* point de 8 centré en (120,160) : pixels 116..123 ; de 4 en (160,120) :
           158..161 ; de 12 en (200,80) : 194..205 */
        check("point atténué : dedans (+3)", 123, 160, 0xffffff);
        check("point atténué : dehors (+5)", 125, 160, 0x000000);
        check("POINT_SIZE_MAX 4 : dedans", 161, 120, 0xffffff);
        check("POINT_SIZE_MAX 4 : dehors", 163, 120, 0x000000);
        check("POINT_SIZE_MIN 12 : dedans (+5)", 205, 80, 0xffffff);
        check("mélange SRC_COLOR (source²)", 168, 168, 0xa3a3a3);
        check("mélange DST_COLOR (destination²)", 188, 168, 0xa3a3a3);
        check("glMultiDrawArrays : 1er", 208, 168, 0xff00ff);
        check("glMultiDrawArrays : 2e", 228, 168, 0xff00ff);
    } else if (!strcmp(scene, "gl15")) {
        /* OpenGL 1.5 au pixel : les huit fonctions de comparaison d'ombre
           (EXT_shadow_funcs), et les objets tampon au-delà du simple dessin
           (glMapBuffer en écriture et en lecture, glBufferSubData,
           glGetBufferSubData, tampon d'indices, décalages dans le tampon).
           Les requêtes d'occlusion ont leur scène (« occl »). */
        typedef void (*gb_f)(GLsizei, GLuint *);
        typedef void (*bb_f)(GLenum, GLuint);
        typedef void (*bd_f)(GLenum, GLsizeiptr, const GLvoid *, GLenum);
        typedef void (*bsd_f)(GLenum, GLintptr, GLsizeiptr, const GLvoid *);
        typedef void *(*mb_f)(GLenum, GLenum);
        typedef GLboolean (*ub_f)(GLenum);
        typedef void (*gbp_f)(GLenum, GLenum, GLint *);
        gb_f gen = (gb_f)gl_sym("glGenBuffers", "glGenBuffersARB");
        gb_f del = (gb_f)gl_sym("glDeleteBuffers", "glDeleteBuffersARB");
        bb_f bind = (bb_f)gl_sym("glBindBuffer", "glBindBufferARB");
        bd_f data = (bd_f)gl_sym("glBufferData", "glBufferDataARB");
        bsd_f sub = (bsd_f)gl_sym("glBufferSubData", "glBufferSubDataARB");
        bsd_f getsub = (bsd_f)gl_sym("glGetBufferSubData", "glGetBufferSubDataARB");
        mb_f map = (mb_f)gl_sym("glMapBuffer", "glMapBufferARB");
        ub_f unmap = (ub_f)gl_sym("glUnmapBuffer", "glUnmapBufferARB");
        gbp_f gbp = (gbp_f)gl_sym("glGetBufferParameteriv", "glGetBufferParameterivARB");
        static const GLfloat dz[4] = { 0.25f, 0.75f, 0.25f, 0.75f };
        static const GLenum fn[8] = { GL_NEVER, GL_ALWAYS, GL_LESS, GL_LEQUAL,
                                      GL_GREATER, GL_GEQUAL, GL_EQUAL, GL_NOTEQUAL };
        /* r = 0,5 contre D = 0,25 (colonne 0) : NEVER, ALWAYS, <, <=, >, >=, ==, != */
        static const unsigned long w25[8] = { 0, 0xffffff, 0, 0, 0xffffff, 0xffffff, 0, 0xffffff };
        static const unsigned long w75[8] = { 0, 0xffffff, 0xffffff, 0xffffff, 0, 0, 0, 0xffffff };
        static const GLushort idx[6] = { 0, 1, 2, 0, 2, 3 };
        GLfloat quad[4 * 5], back[4 * 5];
        GLuint tid, bo[2];
        GLint sz = -1;
        int i, k, okget = 1;
        char nm[64];
        if (!gen || !bind || !data || !sub || !getsub || !map || !unmap || !gbp || !del) {
            printf("  points d'entrée des objets tampon absents\n");
            return 1;
        }
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        /* ── rangée 0 et 1 : les huit fonctions, D = 0,25 puis 0,75 ── */
        glGenTextures(1, &tid);
        glBindTexture(GL_TEXTURE_2D, tid);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 2, 2, 0, GL_DEPTH_COMPONENT,
                     GL_FLOAT, dz);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, 0x884C, 0x884E);     /* COMPARE_R_TO_TEXTURE */
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glEnable(GL_TEXTURE_2D);
        for (k = 0; k < 2; k++)
            for (i = 0; i < 8; i++) {
                float x0 = 20 * i, y0 = 40 * k, s = k ? 0.75f : 0.25f;
                glTexParameteri(GL_TEXTURE_2D, 0x884D, fn[i]);
                glBegin(GL_QUADS);
                glTexCoord3f(s, 0.25f, 0.5f);
                glVertex2f(x0, y0); glVertex2f(x0 + 16, y0);
                glVertex2f(x0 + 16, y0 + 16); glVertex2f(x0, y0 + 16);
                glEnd();
            }
        glDisable(GL_TEXTURE_2D);
        /* ── rangée 2 : objets tampon ── */
        gen(2, bo);
        bind(0x8892 /* ARRAY_BUFFER */, bo[0]);
        data(0x8892, sizeof(quad), 0, 0x88E4 /* STATIC_DRAW */);
        gbp(0x8892, 0x8764 /* BUFFER_SIZE */, &sz);
        {   /* écriture par glMapBuffer : x, y, r, g, b par sommet */
            GLfloat *m = (GLfloat *)map(0x8892, 0x88B9 /* WRITE_ONLY */);
            static const float xy[4][2] = { { 0, 80 }, { 16, 80 }, { 16, 96 }, { 0, 96 } };
            for (i = 0; i < 4; i++) {
                quad[i * 5] = xy[i][0]; quad[i * 5 + 1] = xy[i][1];
                quad[i * 5 + 2] = 1; quad[i * 5 + 3] = 0.6f; quad[i * 5 + 4] = 0.2f;
            }
            if (m)
                memcpy(m, quad, sizeof(quad));
            printf("  glMapBuffer(WRITE_ONLY) : %s, glUnmapBuffer : %d\n", m ? "ok" : "nul",
                   (int)unmap(0x8892));
        }
        bind(0x8893 /* ELEMENT_ARRAY_BUFFER */, bo[1]);
        data(0x8893, sizeof(idx), idx, 0x88E4);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(2, GL_FLOAT, 20, (const GLvoid *)0);
        glColorPointer(3, GL_FLOAT, 20, (const GLvoid *)8);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const GLvoid *)0);   /* ffff99→33 */
        /* glBufferSubData : décale le carré de 20 en x et le passe en bleu */
        for (i = 0; i < 4; i++) {
            quad[i * 5] += 20;
            quad[i * 5 + 2] = 0; quad[i * 5 + 3] = 0.2f; quad[i * 5 + 4] = 1;
        }
        sub(0x8892, 0, sizeof(quad), quad);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const GLvoid *)0);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        /* relecture : glGetBufferSubData, puis glMapBuffer(READ_ONLY) */
        memset(back, 0, sizeof(back));
        getsub(0x8892, 0, sizeof(back), back);
        for (i = 0; i < 20; i++)
            if (back[i] != quad[i]) okget = 0;
        {
            GLfloat *m = (GLfloat *)map(0x8892, 0x88B8 /* READ_ONLY */);
            int okmap = m && !memcmp(m, quad, sizeof(quad));
            unmap(0x8892);
            printf("  BUFFER_SIZE %d, glGetBufferSubData %s, glMapBuffer(READ_ONLY) %s, "
                   "erreur GL 0x%x\n", sz, okget ? "exact" : "FAUX", okmap ? "exact" : "FAUX",
                   glGetError());
            if (sz != (GLint)sizeof(quad) || !okget || !okmap) {
                printf("  FAIL relecture des objets tampon\n");
                failures++;
            }
        }
        bind(0x8892, 0);
        bind(0x8893, 0);
        del(2, bo);
        glFinish();
        for (k = 0; k < 2; k++)
            for (i = 0; i < 8; i++) {
                snprintf(nm, sizeof(nm), "ombre fonction %d, D = %s", i, k ? "0,75" : "0,25");
                check(nm, 20 * i + 8, 40 * k + 8, k ? w75[i] : w25[i]);
            }
        check("VBO : glMapBuffer + indices", 8, 88, 0xff9933);
        check("VBO : glBufferSubData", 28, 88, 0x0033ff);
    } else if (!strcmp(scene, "tcprobe")) {
        /* Sonde : quelles façons de donner les coordonnées de texture font
           jeter la géométrie brute par GLEngine ? Texture 2D blanche en
           REPLACE sur fond noir : blanc = dessiné, noir = jeté. */
        typedef void (*mtc3_f)(GLenum, GLfloat, GLfloat, GLfloat);
        mtc3_f mtc3 = (mtc3_f)gl_sym("glMultiTexCoord3f", "glMultiTexCoord3fARB");
        static const unsigned char wh[4] = { 255, 255, 255, 255 };
        static GLfloat va[4 * 2], ta[4 * 4];
        GLuint id;
        int i, k;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, wh);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
#define PQ(x0, TC) do { glBegin(GL_QUADS); TC; glVertex2f(x0, 0); glVertex2f((x0) + 16, 0); \
            glVertex2f((x0) + 16, 16); glVertex2f(x0, 16); glEnd(); } while (0)
        PQ(0, glTexCoord2f(0.5f, 0.5f));
        PQ(20, glTexCoord3f(0.5f, 0.5f, 0.5f));
        PQ(40, glTexCoord4f(0.5f, 0.5f, 0.5f, 1.0f));
        PQ(60, glTexCoord1f(0.5f));
        PQ(80, if (mtc3) mtc3(GL_TEXTURE0, 0.5f, 0.5f, 0.5f));
        PQ(100, glTexCoord3f(0.5f, 0.5f, 0.0f));
#undef PQ
        /* tableaux : taille 2, 3, 4 */
        for (k = 2; k <= 4; k++) {
            float x0 = 120 + 20 * (k - 2);
            va[0] = x0; va[1] = 0; va[2] = x0 + 16; va[3] = 0;
            va[4] = x0 + 16; va[5] = 16; va[6] = x0; va[7] = 16;
            for (i = 0; i < 16; i++)
                ta[i] = (i % 4 == 3) ? 1.0f : 0.5f;
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glVertexPointer(2, GL_FLOAT, 0, va);
            glTexCoordPointer(k, GL_FLOAT, 16, ta);
            glDrawArrays(GL_QUADS, 0, 4);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glDisableClientState(GL_VERTEX_ARRAY);
        }
        /* glTexCoord3f AVANT glBegin (valeur courante), sommets sans coordonnée */
        glTexCoord3f(0.5f, 0.5f, 0.5f);
        glBegin(GL_QUADS);
        glVertex2f(180, 0); glVertex2f(196, 0); glVertex2f(196, 16); glVertex2f(180, 16);
        glEnd();
        glFinish();
        check("immédiat glTexCoord2f", 8, 8, 0xffffff);
        check("immédiat glTexCoord3f", 28, 8, 0xffffff);
        check("immédiat glTexCoord4f", 48, 8, 0xffffff);
        check("immédiat glTexCoord1f", 68, 8, 0xffffff);
        check("immédiat glMultiTexCoord3f", 88, 8, 0xffffff);
        check("immédiat glTexCoord3f r=0", 108, 8, 0xffffff);
        check("tableau de taille 2", 128, 8, 0xffffff);
        check("tableau de taille 3", 148, 8, 0xffffff);
        check("tableau de taille 4", 168, 8, 0xffffff);
        check("glTexCoord3f courant hors glBegin", 188, 8, 0xffffff);
        glDisable(GL_TEXTURE_2D);
    } else if (!strcmp(scene, "matbegin")) {
        /* glMaterial ENTRE glBegin et glEnd. Au chemin brut, GLEngine bascule
           alors la primitive vers le rendu logiciel d'un AUTRE renderer
           (_gleForceToSoftwareTCL → _gleSwitchToNonRevertRenderer), quel que
           soit cfg+0x7a : elle est perdue pour la surface de l'hôte. Défaut
           connu (docs/re/opengl-1.4.md §3.3) ; exact au chemin hérité et sous
           Apple. Éclairage ambiant seul (modèle 1,1,1) : la couleur est
           l'ambiante du matériau. */
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        {
            static const GLfloat lm[4] = { 1, 1, 1, 1 }, z[4] = { 0, 0, 0, 1 };
            static const GLfloat ra[4] = { 1, 0, 0, 1 }, ga[4] = { 0, 1, 0, 1 };
            glEnable(GL_LIGHTING);
            glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm);
            glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, z);
            glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, ga);
            glNormal3f(0, 0, 1);
            glBegin(GL_QUADS);
            glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, ra);
            glVertex2f(0, 40); glVertex2f(16, 40); glVertex2f(16, 56); glVertex2f(0, 56);
            glEnd();
            glBegin(GL_QUADS);                                  /* la suivante, normale */
            glVertex2f(20, 40); glVertex2f(36, 40); glVertex2f(36, 56); glVertex2f(20, 56);
            glEnd();
            glDisable(GL_LIGHTING);
            glFinish();
            check("glMaterial entre glBegin/glEnd", 8, 48, 0xff0000);
            check("primitive suivante", 28, 48, 0xff0000);
        }
    } else if (!strcmp(scene, "ptprobe")) {
        /* Sonde : deux points de taille 16 au chemin hérité (POMPPC_GL_GEOM=0),
           sans puis avec atténuation (facteur 1/2 à d = 200) — le vidage des
           sommets dit si GLEngine range une taille dérivée dans le sommet. */
        typedef void (*ppfv_f)(GLenum, const GLfloat *);
        ppfv_f ppfv = (ppfv_f)gl_sym("glPointParameterfv", "glPointParameterfvARB");
        static const GLfloat att[3] = { 0, 0, 1.0f / 10000.0f }, one[3] = { 1, 0, 0 };
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glPointSize(16);
        glBegin(GL_POINTS); glVertex2f(120, 160); glEnd();
        glFinish();
        if (ppfv) ppfv(0x8129, att);
        glBegin(GL_POINTS); glVertex2f(120, 160); glEnd();
        glFinish();
        if (ppfv) ppfv(0x8129, one);
    } else if (!strcmp(scene, "v14probe")) {
        /* Sonde d'OpenGL 1.4 (relevé seulement). Première partie, un réglage
           par glClear (POMPPC_GLTRACE_STATE=1, diff par tools/re/diffstate.py) :
           GL_COLOR_SUM, couleur secondaire courante, biais de LOD d'unité
           (GL_TEXTURE_FILTER_CONTROL) sur les unités 0 et 1. Puis ce que
           GLEngine ACCEPTE (erreur GL après chaque appel) : INCR_WRAP et
           DECR_WRAP, une source croisée (GL_TEXTURE1 dans l'unité 0), une
           texture de profondeur et ses paramètres de comparaison, le biais de
           texture, GENERATE_MIPMAP. La texture reste liée à l'unité 0 : le
           vidage à l'échange (POMPPC_GL_T3DDUMP=1) montre son objet. */
        typedef void (*sc3_f)(GLfloat, GLfloat, GLfloat);
        typedef void (*at_f)(GLenum);
        sc3_f sc3 = (sc3_f)gl_sym("glSecondaryColor3f", "glSecondaryColor3fEXT");
        at_f at = (at_f)gl_sym("glActiveTexture", "glActiveTextureARB");
        static const GLfloat dep[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        GLfloat f4[4];
        GLint i4[4];
        GLuint id;
        glClearColor(0, 0, 0, 1);
        f4[0] = f4[1] = f4[2] = -1;
        glGetFloatv(0x8126, f4);
        glGetFloatv(0x8127, f4 + 1);
        glGetFloatv(0x8128, f4 + 2);
        printf("valeurs initiales : POINT_SIZE_MIN %g, MAX %g, FADE %g\n", f4[0], f4[1], f4[2]);
        glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, f4);
        printf("GL_ALIASED_POINT_SIZE_RANGE %g %g\n", f4[0], f4[1]);
        pstep("1 reference");
        glEnable(0x8458 /* GL_COLOR_SUM */);
        pstep("2 glEnable(GL_COLOR_SUM)");
        printf("  erreur GL 0x%x\n", glGetError());
        if (sc3) sc3(0.125f, 0.25f, 0.375f);
        pstep("3 glSecondaryColor3f(.125 .25 .375)");
        glTexEnvf(0x8500 /* TEXTURE_FILTER_CONTROL */, 0x8501 /* LOD_BIAS */, 1.5f);
        pstep("4 unite 0 : TEXTURE_LOD_BIAS 1.5");
        printf("  erreur GL 0x%x\n", glGetError());
        if (at) at(GL_TEXTURE1);
        glTexEnvf(0x8500, 0x8501, 2.5f);
        if (at) at(GL_TEXTURE0);
        pstep("5 unite 1 : TEXTURE_LOD_BIAS 2.5");
        printf("  erreur GL 0x%x\n", glGetError());
        glDisable(0x8458);
        pstep("6 glDisable(GL_COLOR_SUM)");
        {
            typedef void (*ppf_f)(GLenum, GLfloat);
            typedef void (*ppfv_f)(GLenum, const GLfloat *);
            ppf_f ppf = (ppf_f)gl_sym("glPointParameterf", "glPointParameterfARB");
            ppfv_f ppfv = (ppfv_f)gl_sym("glPointParameterfv", "glPointParameterfvARB");
            static const GLfloat att[3] = { 0.5f, 0.25f, 0.125f };
            if (ppf) ppf(0x8126 /* POINT_SIZE_MIN */, 3.5f);
            pstep("7 POINT_SIZE_MIN 3.5");
            if (ppf) ppf(0x8127 /* POINT_SIZE_MAX */, 7.25f);
            pstep("8 POINT_SIZE_MAX 7.25");
            if (ppf) ppf(0x8128 /* POINT_FADE_THRESHOLD_SIZE */, 2.5f);
            pstep("9 POINT_FADE_THRESHOLD_SIZE 2.5");
            if (ppfv) ppfv(0x8129 /* POINT_DISTANCE_ATTENUATION */, att);
            pstep("10 POINT_DISTANCE_ATTENUATION .5 .25 .125");
            f4[0] = f4[1] = -1;
            glGetFloatv(0x8127, f4);
            glGetFloatv(0x8126, f4 + 1);
            printf("POINT_SIZE_MAX relu %g, MIN %g, erreur GL 0x%x\n", f4[0], f4[1], glGetError());
        }
        f4[0] = -1;
        glGetFloatv(0x84FD /* MAX_TEXTURE_LOD_BIAS */, f4);
        printf("GL_MAX_TEXTURE_LOD_BIAS = %g\n", f4[0]);
        f4[0] = -1;
        glGetTexEnvfv(0x8500, 0x8501, f4);
        printf("biais d'unité relu = %g, erreur GL 0x%x\n", f4[0], glGetError());
        glStencilOp(GL_KEEP, 0x8507 /* INCR_WRAP */, 0x8508 /* DECR_WRAP */);
        printf("glStencilOp(INCR_WRAP, DECR_WRAP) : erreur GL 0x%x\n", glGetError());
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, 0x8570 /* COMBINE */);
        glTexEnvi(GL_TEXTURE_ENV, 0x8580 /* SOURCE0_RGB */, GL_TEXTURE1);
        printf("source croisée GL_TEXTURE1 : erreur GL 0x%x\n", glGetError());
        glTexEnvi(GL_TEXTURE_ENV, 0x8580, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 2, 2, 0, GL_DEPTH_COMPONENT,
                     GL_FLOAT, dep);
        printf("glTexImage2D(DEPTH_COMPONENT, FLOAT) : erreur GL 0x%x\n", glGetError());
        glTexParameteri(GL_TEXTURE_2D, 0x884C /* COMPARE_MODE */, 0x884E);
        printf("COMPARE_MODE = COMPARE_R_TO_TEXTURE : erreur GL 0x%x\n", glGetError());
        glTexParameteri(GL_TEXTURE_2D, 0x884D /* COMPARE_FUNC */, GL_GEQUAL);
        printf("COMPARE_FUNC = GEQUAL : erreur GL 0x%x\n", glGetError());
        glTexParameteri(GL_TEXTURE_2D, 0x884B /* DEPTH_TEXTURE_MODE */, GL_INTENSITY);
        printf("DEPTH_TEXTURE_MODE = INTENSITY : erreur GL 0x%x\n", glGetError());
        glTexParameterf(GL_TEXTURE_2D, 0x8501 /* TEXTURE_LOD_BIAS */, 0.75f);
        printf("TEXTURE_LOD_BIAS = 0.75 : erreur GL 0x%x\n", glGetError());
        glTexParameteri(GL_TEXTURE_2D, 0x8191 /* GENERATE_MIPMAP */, GL_TRUE);
        printf("GENERATE_MIPMAP = TRUE : erreur GL 0x%x\n", glGetError());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        i4[0] = i4[1] = i4[2] = i4[3] = -1;
        glGetTexParameteriv(GL_TEXTURE_2D, 0x884C, i4 + 0);
        glGetTexParameteriv(GL_TEXTURE_2D, 0x884D, i4 + 1);
        glGetTexParameteriv(GL_TEXTURE_2D, 0x884B, i4 + 2);
        glGetTexParameteriv(GL_TEXTURE_2D, 0x8191, i4 + 3);
        printf("relus : COMPARE_MODE %x, COMPARE_FUNC %x, DEPTH_TEXTURE_MODE %x, "
               "GENERATE_MIPMAP %d\n", i4[0], i4[1], i4[2], i4[3]);
        i4[0] = -1;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, i4);
        printf("format interne du niveau 0 : %x, erreur GL 0x%x\n", i4[0], glGetError());
        glEnable(GL_TEXTURE_2D);
        glBegin(GL_QUADS);
        glTexCoord3f(0.25f, 0.25f, 0.4f); glVertex2f(0, 0); glVertex2f(W, 0);
        glVertex2f(W, H); glVertex2f(0, H);
        glEnd();
        glFinish();
        printf("  pixel (r = 0,4 contre D = 0,25, GEQUAL) : %06lx\n", px(W / 2, H / 2));
    } else if (!strcmp(scene, "wrapprobe")) {
        /* Sonde (relevé seulement), selon PROBE_MODE :
             border  : WRAP_S = CLAMP_TO_BORDER, WRAP_T = MIRRORED_REPEAT, couleur
                       de bordure (0,25 ; 0,5 ; 0,75 ; 1) — où GLEngine les range ;
             dxt     : un bloc DXT1 4×4 par glCompressedTexImage2D — ce que le
                       pilote reçoit (format, pas, données) ;
             generic : GL_COMPRESSED_RGB par glTexImage2D (GLEngine compresse),
                       puis relecture par glGetCompressedTexImage ;
             genmip  : idem sur trois niveaux, dessinés minifiés.
           Dans tous les modes : les formats compressés annoncés et les requêtes
           de niveau (docs/re/bordure-et-compression.md).
           La texture reste liée à l'unité 0 : le vidage à l'échange la montre. */
        static const GLfloat bc[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        static const unsigned char blk[8] = { 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 };
        static const unsigned char white[16] = { 255, 255, 255, 255, 255, 255, 255, 255,
                                                 255, 255, 255, 255, 255, 255, 255, 255 };
        static unsigned char white4[4 * 4 * 4];
        const char *mode = getenv("PROBE_MODE") ? getenv("PROBE_MODE") : "border";
        GLuint id;
        memset(white4, 255, sizeof(white4));
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        {
            GLint nf = -1, fl[16], i2;
            glGetIntegerv(0x86A2 /* NUM_COMPRESSED_TEXTURE_FORMATS */, &nf);
            printf("formats compressés annoncés : %d :", nf);
            if (nf > 0 && nf <= 16) {
                glGetIntegerv(0x86A3, fl);
                for (i2 = 0; i2 < nf; i2++)
                    printf(" %x", fl[i2]);
            }
            printf("\n");
        }
        if (!strcmp(mode, "genmip")) {
            /* format générique, trois niveaux : GLEngine compresse chacun */
            glTexImage2D(GL_TEXTURE_2D, 0, 0x84ED, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, white4);
            glTexImage2D(GL_TEXTURE_2D, 1, 0x84ED, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, white4);
            glTexImage2D(GL_TEXTURE_2D, 2, 0x84ED, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white4);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            glEnable(GL_TEXTURE_2D);
            glBegin(GL_QUADS);                                  /* λ = 2 */
            glTexCoord2f(0, 0); glVertex2f(0, 0); glTexCoord2f(1, 0); glVertex2f(1, 0);
            glTexCoord2f(1, 1); glVertex2f(1, 1); glTexCoord2f(0, 1); glVertex2f(0, 1);
            glEnd();
            glFinish();
            printf("GL_COMPRESSED_RGB, mipmaps : dessinées, erreur GL 0x%x\n", glGetError());
        } else if (!strcmp(mode, "generic")) {
            GLint ifmt = 0, cmp = -1, sz = -1;
            glTexImage2D(GL_TEXTURE_2D, 0, 0x84ED /* COMPRESSED_RGB */, 2, 2, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, white);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x86A1 /* TEXTURE_COMPRESSED */, &cmp);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x86A0 /* ..._IMAGE_SIZE */, &sz);
            printf("GL_COMPRESSED_RGB : interne %x, compressée %d, taille %d, erreur GL 0x%x\n",
                   ifmt, cmp, sz, glGetError());
            {   /* ARB_texture_compression : relecture de l'image compressée */
                typedef void (*gcti_f)(GLenum, GLint, GLvoid *);
                gcti_f g = (gcti_f)gl_sym("glGetCompressedTexImage", "glGetCompressedTexImageARB");
                unsigned char buf[64];
                memset(buf, 0xAA, sizeof(buf));
                if (g)
                    g(GL_TEXTURE_2D, 0, buf);
                printf("glGetCompressedTexImage : %s, erreur GL 0x%x, %02x%02x%02x%02x %02x%02x%02x%02x | %02x\n",
                       g ? "appelé" : "absent", glGetError(), buf[0], buf[1], buf[2], buf[3],
                       buf[4], buf[5], buf[6], buf[7], buf[8]);
            }
        } else if (!strcmp(mode, "dxt")) {
            typedef void (*ctx_f)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
                                  const GLvoid *);
            ctx_f cti = (ctx_f)gl_sym("glCompressedTexImage2D", "glCompressedTexImage2DARB");
            if (cti)
                cti(GL_TEXTURE_2D, 0, 0x83F0, 4, 4, 0, 8, blk);
            printf("glCompressedTexImage2D (DXT1) : %s, erreur GL 0x%x\n",
                   cti ? "appelé" : "absent", glGetError());
            {
                GLint ifmt = 0, cmp = -1, sz = -1;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x86A1, &cmp);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x86A0, &sz);
                printf("  interne %x, compressée %d, taille %d\n", ifmt, cmp, sz);
            }
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812D);   /* CLAMP_TO_BORDER */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x8370);   /* MIRRORED_REPEAT */
            glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, bc);
            printf("CLAMP_TO_BORDER, MIRRORED_REPEAT, bordure : erreur GL 0x%x\n", glGetError());
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glTexCoord2f(-1, 0); glVertex2f(0, 0); glTexCoord2f(2, 0); glVertex2f(W, 0);
        glTexCoord2f(2, 1); glVertex2f(W, H); glTexCoord2f(-1, 1); glVertex2f(0, H);
        glEnd();
        glFinish();
        printf("  pixels : gauche %06lx, milieu %06lx\n", px(W / 8, H / 2), px(W / 2, H / 2));
    } else if (!strcmp(scene, "cubeprobe")) {
        /* Sonde (relevé seulement) : carte de cube dont la face f a un niveau 0
           de 4×4 (R = 40·f, G = 255 − 40·f, B = 7), un niveau 1 de 2×2
           (R = 40·f, G = 100, B = 200) et un niveau 2 de 1×1 — pour retrouver
           dans l'objet texture de GLEngine où vivent les six faces. Il faut
           POMPPC_GL_TRYCUBE (cfg+0xc2) : sans lui, GLEngine refuse les faces. */
        static unsigned char l0[16 * 4], l1[4 * 4], l2[4];
        int f, i;
        GLuint id;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_CUBE_MAP, id);
        for (f = 0; f < 6; f++) {
            for (i = 0; i < 16; i++) {
                l0[i * 4] = (unsigned char)(40 * f); l0[i * 4 + 1] = (unsigned char)(255 - 40 * f);
                l0[i * 4 + 2] = 7; l0[i * 4 + 3] = 255;
            }
            for (i = 0; i < 4; i++) {
                l1[i * 4] = (unsigned char)(40 * f); l1[i * 4 + 1] = 100;
                l1[i * 4 + 2] = 200; l1[i * 4 + 3] = 255;
            }
            l2[0] = (unsigned char)f; l2[1] = 1; l2[2] = 2; l2[3] = 255;
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, 4, 4, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, l0);
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 1, GL_RGBA, 2, 2, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, l1);
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 2, GL_RGBA, 1, 1, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, l2);
        }
        printf("faces de cube : erreur GL 0x%x\n", glGetError());
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glEnable(GL_TEXTURE_CUBE_MAP);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glTexCoord3f(1, 0.1f, 0.2f); glVertex2f(0, 0);
        glTexCoord3f(1, 0.1f, 0.2f); glVertex2f(W, 0);
        glTexCoord3f(1, 0.1f, 0.2f); glVertex2f(W, H);
        glTexCoord3f(1, 0.1f, 0.2f); glVertex2f(0, H);
        glEnd();
        glFinish();
        printf("  face +X au centre = %06lx (attendu 00ff07)\n", px(W / 2, H / 2));
        glDisable(GL_TEXTURE_CUBE_MAP);
    } else if (!strcmp(scene, "t3dprobe")) {
        /* Sonde (relevé seulement) : une texture 3D 8×2×4 dont chaque texel
           code ses coordonnées (R = 30·x, G = 100·y, B = 60·z + 30), plus un
           niveau 1 de 4×1×2 — toutes les dimensions distinctes, pour qu'aucun
           champ relevé ne soit ambigu —, et trois modes de répétition DIFFÉRENTS (S REPEAT,
           T CLAMP, R CLAMP_TO_EDGE) pour que chaque paramètre se retrouve sans
           ambiguïté dans les vidages du plugin (POMPPC_GL_T3DDUMP). Il faut
           POMPPC_GL_TRY3D : sans lui, GLEngine refuse glTexImage3D. Tailles en
           puissances de 2 : OpenGL 1.2 rend GL_INVALID_VALUE sinon (vu). */
        unsigned char img[4][2][8][4], lv1[2][1][4][4];
        int x, yy, z;
        GLuint id;
        for (z = 0; z < 4; z++)
            for (yy = 0; yy < 2; yy++)
                for (x = 0; x < 8; x++) {
                    img[z][yy][x][0] = (unsigned char)(30 * x);
                    img[z][yy][x][1] = (unsigned char)(100 * yy);
                    img[z][yy][x][2] = (unsigned char)(60 * z + 30);
                    img[z][yy][x][3] = 255;
                }
        for (z = 0; z < 2; z++)
            for (x = 0; x < 4; x++) {
                lv1[z][0][x][0] = (unsigned char)(200 - 40 * x);
                lv1[z][0][x][1] = (unsigned char)(10 + 100 * z);
                lv1[z][0][x][2] = 20;
                lv1[z][0][x][3] = 255;
            }
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_3D, id);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 8, 2, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
        glTexImage3D(GL_TEXTURE_3D, 1, GL_RGBA, 4, 1, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, lv1);
        printf("glTexImage3D : erreur GL 0x%x\n", glGetError());
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        if (getenv("T3D_LOD")) {
            /* relevé des paramètres de LOD (OpenGL 1.2 et 1.4), valeurs
               distinctives, à comparer au vidage sans T3D_LOD */
            glTexParameterf(GL_TEXTURE_3D, GL_TEXTURE_MIN_LOD, 1.5f);
            glTexParameterf(GL_TEXTURE_3D, GL_TEXTURE_MAX_LOD, 2.5f);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 1);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 3);
            glTexParameterf(GL_TEXTURE_3D, GL_TEXTURE_PRIORITY, 0.25f);
            glTexParameterf(GL_TEXTURE_3D, 0x8501 /* GL_TEXTURE_LOD_BIAS */, 0.75f);
            printf("paramètres de LOD : erreur GL 0x%x\n", glGetError());
        }
        glEnable(GL_TEXTURE_3D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glTexCoord3f(0, 0, 0.3f); glVertex2f(0, 0);
        glTexCoord3f(1, 0, 0.3f); glVertex2f(W, 0);
        glTexCoord3f(1, 1, 0.3f); glVertex2f(W, H);
        glTexCoord3f(0, 1, 0.3f); glVertex2f(0, H);
        glEnd();
        glFinish();
        /* r = 0,3 → tranche z = 1 : texel (3, 0) → 5a005a, texel (5, 1) → 96645a */
        printf("  texel (1,0,1) en (%d,%d) = %06lx ; texel (2,1,1) en (%d,%d) = %06lx\n",
               3 * W / 8, H / 4, px(3 * W / 8, H / 4), 5 * W / 8, 3 * H / 4,
               px(5 * W / 8, 3 * H / 4));
        glDisable(GL_TEXTURE_3D);
    } else if (!strcmp(scene, "texup")) {
        /* Débit de téléversement de textures (tâche 2.5, protocole v10) : une
           texture de TEXUP_SIZE² texels (256 par défaut), modifiée et renvoyée
           à CHAQUE image par glTexSubImage2D, puis dessinée. TEXUP_FMT = rgba
           (octets, défaut), rgb, rgb565, bgra (8_8_8_8_REV : le seul format
           que l'invité recopiait déjà sans convertir). À comparer avec
           POMPPC_GL_TEX3=0 (conversion par l'invité) et POMPPC_GL_DISABLE=1. */
        int frames = 60, f, i, n = getenv("TEXUP_SIZE") ? atoi(getenv("TEXUP_SIZE")) : 256;
        const char *fm = getenv("TEXUP_FMT") ? getenv("TEXUP_FMT") : "rgba";
        GLenum fmt = GL_RGBA, type = GL_UNSIGNED_BYTE;
        int bpp = 4;
        unsigned char *pix;
        unsigned int v = 0;
        GLuint id;
        double t0;
        if (!strcmp(fm, "rgb")) { fmt = GL_RGB; bpp = 3; }
        else if (!strcmp(fm, "rgb565")) { fmt = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5; bpp = 2; }
        else if (!strcmp(fm, "bgra")) { fmt = GL_BGRA; type = GL_UNSIGNED_INT_8_8_8_8_REV; }
        pix = calloc(n * n, bpp);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, n, n, 0, fmt, type, pix);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        t0 = now();
        for (f = 0; f < frames; f++) {
            v = (f * 4) & 0xF8;                /* gris exact aussi en 5_6_5 */
            if (bpp == 2) {
                unsigned short g = (unsigned short)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
                for (i = 0; i < n * n; i++)
                    ((unsigned short *)pix)[i] = g;
            } else {
                memset(pix, (int)v, (size_t)n * n * bpp);
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, n, fmt, type, pix);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(0, 0);
            glTexCoord2f(1, 0); glVertex2f(W, 0);
            glTexCoord2f(1, 1); glVertex2f(W, H);
            glTexCoord2f(0, 1); glVertex2f(0, H);
            glEnd();
            glFinish();
        }
        printf("texup %s %dx%d : %d images, %.2f img/s\n", fm, n, n, frames,
               frames / (now() - t0));
        /* la dernière image : gris v, étendu à 8 bits comme le fait OpenGL */
        {
            unsigned long c = bpp == 2 ? ((v | (v >> 5)) << 16) | ((v | (v >> 6)) << 8) | (v | (v >> 5))
                                       : v * 0x010101UL;
            check("dernière image téléversée", W / 2, H / 2, c);
        }
        free(pix);
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
