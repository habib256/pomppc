/*
 * rtt.c — rendu vers texture à la manière d'IndirectX (Colin McRae) : une
 * fenêtre CACHÉE (CreateNewWindow sans ShowWindow) sert de drawable à un
 * contexte AGL « cible de rendu », et le contexte principal l'échantillonne
 * par aglSurfaceTexture. docs/re/cmr-rendu-vers-texture.md.
 *
 *   rtt [2d|rect] [sortie.ppm]
 *
 * Source 64×48 (64×64 en 2D : GL 1.x exige des puissances de 2) : moitié HAUTE rouge, moitié basse verte, 16 colonnes de
 * gauche bleues (effacements à ciseaux). Le contexte principal (fenêtre
 * visible 128×96) dessine un quad plein écran, t = 0 en BAS de son écran, et
 * relit son tampon arrière (glReadPixels) :
 *   (a) orientation : aglSurfaceTexture rend la ligne du HAUT de la source en
 *       t = 0 (mémoire de la fenêtre, de haut en bas) — donc le BAS de l'image
 *       principale est rouge ; les colonnes bleues sont à gauche ;
 *   (b) tampon AVANT : la source redessinée en jaune SANS échange ne change
 *       pas la texture ;
 *   (c) après aglSwapBuffers de la source, la texture est jaune.
 * « rect » : même chose avec GL_TEXTURE_RECTANGLE_EXT (coordonnées en texels).
 * Code de sortie 0 si tous les témoins sont bons. GLTEST_REQUIRE=<texte> :
 * exige ce texte dans GL_RENDERER (POMPPC). Référence : POMPPC_GL_DISABLE=1.
 *
 *   gcc-4.0 -arch ppc -O1 -isysroot /Developer/SDKs/MacOSX10.4u.sdk -o rtt rtt.c \
 *       -framework AGL -framework OpenGL -framework Carbon
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Carbon/Carbon.h>
#include <AGL/agl.h>
#include <OpenGL/gl.h>

#ifndef GL_TEXTURE_RECTANGLE_EXT
#define GL_TEXTURE_RECTANGLE_EXT 0x84F5
#endif

enum { W = 128, H = 96, SW = 64 };
static int SH = 48;                     /* 2d : 64 (une texture 2D doit être en puissances de 2) */
static unsigned char img[W * H * 4];
static int failures;

/* 0xRRGGBB, y depuis le HAUT de l'image principale */
static unsigned long px(int x, int y)
{
    const unsigned char *p = img + ((H - 1 - y) * W + x) * 4;   /* glReadPixels : bas d'abord */
    return ((unsigned long)p[0] << 16) | ((unsigned long)p[1] << 8) | p[2];
}

static void temoin(const char *what, int x, int y, unsigned long want)
{
    unsigned long got = px(x, y);
    int bon = got == want;
    printf("  %s %-40s (%3d,%3d) = %06lx%s\n", bon ? "ok  " : "FAIL", what, x, y, got,
           bon ? "" : " (attendu autre)");
    if (!bon)
        failures++;
}

static void source_frame(AGLContext b, float r, float g, float bl, int split)
{
    aglSetCurrentContext(b);
    glViewport(0, 0, SW, SH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(r, g, bl, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (split) {
        glEnable(GL_SCISSOR_TEST);
        glClearColor(1, 0, 0, 1); glScissor(0, SH / 2, SW, SH - SH / 2); glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(0, 1, 0, 1); glScissor(0, 0, SW, SH / 2); glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(0, 0, 1, 1); glScissor(0, 0, 16, SH); glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
    glFlush();
}

static void main_frame(AGLContext a, GLenum target, GLuint tex)
{
    float s1 = target == GL_TEXTURE_RECTANGLE_EXT ? SW : 1;
    float t1 = target == GL_TEXTURE_RECTANGLE_EXT ? SH : 1;
    aglSetCurrentContext(a);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClearColor(0.5f, 0.5f, 0.5f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(target, tex);
    glEnable(target);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);   glVertex2f(-1, -1);
    glTexCoord2f(s1, 0);  glVertex2f(1, -1);
    glTexCoord2f(s1, t1); glVertex2f(1, 1);
    glTexCoord2f(0, t1);  glVertex2f(-1, 1);
    glEnd();
    glDisable(target);
    glFinish();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, img);
}

static int write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int y, x;
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            unsigned long c = px(x, y);
            unsigned char rgb[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    GLint attrs[] = { AGL_RGBA, AGL_DOUBLEBUFFER, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8,
                      AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_DEPTH_SIZE, 16, AGL_NONE };
    int rect = argc > 1 && !strcmp(argv[1], "rect");
    Rect rb;
    const char *out = argc > 2 ? argv[2] : "rtt.ppm";
    GLenum target = rect ? GL_TEXTURE_RECTANGLE_EXT : GL_TEXTURE_2D;
    Rect ra = { 60, 40, 60 + H, 40 + W };
    WindowRef wa, wb;
    AGLPixelFormat pf;
    AGLContext a, b;
    GLuint tex;
    const char *req = getenv("GLTEST_REQUIRE"), *rnd;
    int mid = W / 2;

    if (!rect)
        SH = 64;
    rb.top = 0; rb.left = 0; rb.bottom = SH; rb.right = SW;

    if (CreateNewWindow(kDocumentWindowClass, kWindowNoAttributes, &ra, &wa) != noErr ||
        CreateNewWindow(kDocumentWindowClass, kWindowNoAttributes, &rb, &wb) != noErr) {
        printf("FAIL CreateNewWindow\n");
        return 3;
    }
    ShowWindow(wa);                     /* la source, elle, n'est JAMAIS montrée */
    pf = aglChoosePixelFormat(NULL, 0, attrs);
    if (!pf) {
        printf("FAIL aglChoosePixelFormat (%s)\n", aglErrorString(aglGetError()));
        return 2;
    }
    a = aglCreateContext(pf, NULL);
    b = aglCreateContext(pf, a);
    aglDestroyPixelFormat(pf);
    if (!a || !b) {
        printf("FAIL aglCreateContext\n");
        return 3;
    }
    aglSetDrawable(a, GetWindowPort(wa));
    aglSetDrawable(b, GetWindowPort(wb));
    aglSetCurrentContext(a);
    rnd = (const char *)glGetString(GL_RENDERER);
    printf("GL_RENDERER = %s ; cible %s\n", rnd ? rnd : "(nul)", rect ? "rectangle" : "2D");
    if (req && *req && (!rnd || !strstr(rnd, req))) {
        printf("GLTEST_REQUIRE : GL_RENDERER « %s » ne contient pas « %s »\n", rnd ? rnd : "", req);
        return 6;
    }

    /* la source : haut rouge, bas vert, colonnes de gauche bleues ; échangée */
    source_frame(b, 0, 0, 0, 1);
    aglSwapBuffers(b);

    aglSetCurrentContext(a);
    glGenTextures(1, &tex);
    glBindTexture(target, tex);
    aglSurfaceTexture(a, target, GL_RGBA8, b);
    printf("  aglSurfaceTexture : erreur AGL %s, GL 0x%x\n", aglErrorString(aglGetError()),
           (unsigned)glGetError());
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    main_frame(a, target, tex);
    temoin("(a) bas (t = 0) = HAUT de la source : rouge", mid + 20, H - 10, 0xFF0000);
    temoin("(a) haut (t = 1) = bas de la source : vert", mid + 20, 10, 0x00FF00);
    temoin("(a) gauche : colonnes bleues", 8, H / 2 + 20, 0x0000FF);
    write_ppm(out);

    /* (b) la source redessinée en jaune, PAS échangée : texture inchangée */
    source_frame(b, 1, 1, 0, 0);
    main_frame(a, target, tex);
    temoin("(b) sans échange : toujours rouge en bas", mid + 20, H - 10, 0xFF0000);
    /* (c) échange de la source : la texture suit */
    aglSwapBuffers(b);
    main_frame(a, target, tex);
    temoin("(c) après échange de la source : jaune", mid + 20, H - 10, 0xFFFF00);
    temoin("(c) après échange de la source : jaune (haut)", mid + 20, 10, 0xFFFF00);
    aglSwapBuffers(a);

    aglSetCurrentContext(NULL);
    aglDestroyContext(b);
    aglDestroyContext(a);
    DisposeWindow(wb);
    DisposeWindow(wa);
    printf("%s (%d échec(s))\n", failures ? "ÉCHEC" : "OK", failures);
    return failures ? 1 : 0;
}
