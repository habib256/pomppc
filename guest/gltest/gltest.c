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

int main(int argc, char **argv)
{
    const char *scene = argc > 1 ? argv[1] : "tri";
    const char *out = argc > 4 ? argv[4] : "gltest.ppm";
    CGLPixelFormatAttribute attrs[16];
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
