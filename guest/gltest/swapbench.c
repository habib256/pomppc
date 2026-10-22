/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * swapbench.c — l'ÉCHANGE seul, à cadence élevée, en fenêtre et en plein écran
 * (22/10/2026, levier B de docs/plugin-allegement.md). À lancer dans la session
 * graphique (job swapwin) :
 *
 *   swapbench fen   N [L H]   une fenêtre GLUT, une image = un effacement et
 *                             un triangle, N échanges : ms/image ;
 *   swapbench move  N [L H]   idem, la fenêtre est déplacée toutes les 60 images
 *                             (glutPositionWindow) : après chaque déplacement
 *                             et 30 images, le centre de l'ANCIEN rectangle ne
 *                             doit plus être à notre couleur (une écriture
 *                             directe au vieil endroit, après que le
 *                             WindowServer y a repeint le bureau, y resterait),
 *                             le centre du nouveau doit l'être ;
 *   swapbench cover N [L H]   deux fenêtres qui se recouvrent, chacune sa
 *                             couleur ; dans la zone commune, la mémoire vidéo
 *                             ne doit montrer que la fenêtre DE DEVANT
 *                             (relevé à chaque image après 30 images) ;
 *   swapbench pop   N [L H]   comme cover, mais la seconde fenêtre passe,
 *                             toutes les 45 images, devant, derrière
 *                             (glutPushWindow), devant (glutPopWindow), cachée
 *                             — sans rien déplacer : 25 images après, la
 *                             zone commune doit montrer la fenêtre de devant
 *                             si elle est là (jamais la nôtre par-dessus), la
 *                             nôtre sinon ;
 *   swapbench fs    N [L H]   plein écran CGL (capture de l'écran, mode L×H,
 *                             CGLSetFullScreen) : ms/image, puis le centre de
 *                             l'écran doit être à notre couleur.
 *
 * SWB_WORK=k : k × 100 000 tours d'une boucle de calcul par image (le travail
 * du jeu : au-delà de la latence de l'hôte, le temps par image mesure alors
 * ce que l'échange coûte au G4) ; SWB_TEXDEL=1 : une texture 16×16 créée,
 * employée puis détruite à chaque image (comme Marble Blast).
 *
 * La mémoire vidéo est lue par CGDisplayBaseAddress (lecture seule). Rend 0 si
 * les relevés sont bons. Les temps : gettimeofday, après 30 images de mise en
 * route.
 */
#include <Carbon/Carbon.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <GLUT/glut.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#define WARM 30

static int W = 320, H = 240, frames = 600, frame, mode;     /* 0 fen, 1 move, 2 cover */
static int win[2], nwin = 1, shown = 1, toggled_at = -1, absent;
static double t0;
static int failures, moves, seen_bad, sampled;
static int old_x = -1, old_y = -1, moved_at = -1;
static const float col[2][3] = { { 1, 0, 1 }, { 0, 1, 0 } };    /* magenta, vert */
static const unsigned long colpix[2] = { 0xff00ff, 0x00ff00 };
static long work;
static int texdel;
static volatile unsigned long sink;

static void game_work(void)
{
    long i;
    unsigned long a = 1;
    for (i = 0; i < work * 100000L; i++)
        a = a * 1103515245UL + 12345UL;
    sink = a;
}

static void tex_churn(void)
{
    static unsigned char texels[16 * 16 * 4];
    GLuint t;
    if (!texdel)
        return;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_TRIANGLES);
    glTexCoord2f(0, 0); glVertex2f(0.8f, 0.8f);
    glTexCoord2f(1, 0); glVertex2f(1, 0.8f);
    glTexCoord2f(0, 1); glVertex2f(0.8f, 1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    /* SWB_TEXDEL=2 : détruite encore liée à l'unité 0 — fait planter le plugin
       (us_fill → tex_cp_fill → tex_surface sur l'objet libéré), y compris sans
       aucun levier : défaut antérieur, relevé le 22/10. =1 : déliée d'abord. */
    if (texdel != 2)
        glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &t);
}

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Pixel xRGB de l'écran principal en (x, y) global, origine en haut à gauche ;
   ~0 hors écran ou si l'écran n'est pas en 32 bits. */
static unsigned long vram_px(int x, int y)
{
    CGDirectDisplayID d = CGMainDisplayID();
    unsigned char *base = CGDisplayBaseAddress(d);
    unsigned long rb = CGDisplayBytesPerRow(d);
    if (!base || CGDisplayBitsPerPixel(d) != 32 || x < 0 || y < 0 ||
        x >= (int)CGDisplayPixelsWide(d) || y >= (int)CGDisplayPixelsHigh(d))
        return ~0UL;
    return *(volatile unsigned long *)(base + y * rb + x * 4) & 0xffffff;
}

static void draw(int k, float a)
{
    glClearColor(col[k][0], col[k][1], col[k][2], 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    /* un petit triangle dans un coin : l'image n'est pas vide, le centre reste
       à la couleur du fond */
    glBegin(GL_TRIANGLES);
    glColor3f(1, 1, a);
    glVertex2f(-1, -1); glVertex2f(-0.8f, -1); glVertex2f(-1, -0.8f);
    glEnd();
    tex_churn();
    game_work();
}

static void finish(void)
{
    double dt = now() - t0;
    int n = frames - WARM;
    printf("swapbench %s : %d images %dx%d en %.3f s : %.3f ms/image\n",
           mode == 0 ? "fen" : mode == 1 ? "move" : mode == 2 ? "cover" : "pop", n, W, H, dt,
           dt * 1000 / n);
    if (mode == 1)
        printf("move : %d déplacements, %d relevés mauvais\n", moves, seen_bad);
    if (mode == 2)
        printf("cover : %d relevés, %d où la fenêtre de derrière se montrait\n",
               sampled, seen_bad);
    if (mode == 3)
        printf("pop : %d relevés, %d où la fenêtre de derrière se montrait par-dessus, "
               "%d où elle manquait une fois découverte\n", sampled, seen_bad, absent);
    printf("%s (%d échec(s))\n", failures ? "ÉCHEC" : "OK", failures);
    fflush(stdout);
    exit(failures ? 1 : 0);
}

static void idle(void)
{
    int k;
    if (frame == WARM)
        t0 = now();
    if (frame >= frames)
        finish();
    for (k = 0; k < nwin; k++) {
        glutSetWindow(win[k]);
        draw(k, (frame & 63) / 63.0f);
        glutSwapBuffers();
    }
    frame++;
    if (mode == 1 && frame > WARM) {
        int x = glutGet(GLUT_WINDOW_X), y = glutGet(GLUT_WINDOW_Y);
        if (frame % 60 == 0) {
            old_x = x; old_y = y;
            moved_at = frame;
            moves++;
            glutSetWindow(win[0]);
            glutPositionWindow(x == 40 ? 400 : 40, 60 + (moves % 3) * 40);
        } else if (moved_at > 0 && frame == moved_at + 30) {
            unsigned long po = vram_px(old_x + W / 2, old_y + H / 2);
            unsigned long pn = vram_px(x + W / 2, y + H / 2);
            int bad = po == colpix[0] || pn != colpix[0];
            if (bad) {
                seen_bad++;
                failures++;
                printf("move %d : ancien centre (%d,%d) %06lx, nouveau (%d,%d) %06lx\n",
                       moves, old_x + W / 2, old_y + H / 2, po, x + W / 2, y + H / 2, pn);
            }
        }
    }
    if (mode == 3 && frame > WARM) {
        int x1, y1;
        unsigned long px;
        /* quatre phases de 45 images : devant, derrière (glutPushWindow : ordre
           seul, rien ne bouge), devant (glutPopWindow), cachée */
        if (frame % 45 == 0) {
            int ph = (frame / 45) % 4;
            glutSetWindow(win[1]);
            if (ph == 0) {
                glutShowWindow();
                glutPopWindow();
            } else if (ph == 1) {
                glutPushWindow();
            } else if (ph == 2) {
                glutPopWindow();
            } else {
                glutHideWindow();
            }
            shown = ph == 0 || ph == 2;     /* devant nous */
            toggled_at = frame;
        } else if (toggled_at > 0 && frame > toggled_at + 25) {
            glutSetWindow(win[1]);
            x1 = glutGet(GLUT_WINDOW_X); y1 = glutGet(GLUT_WINDOW_Y);
            px = vram_px(x1 + 20, y1 + 20);
            sampled++;
            if (shown && px == colpix[0]) {
                seen_bad++;
                if (seen_bad <= 4)
                    printf("pop : image %d, (%d,%d) = %06lx par-dessus la fenêtre de devant\n",
                           frame, x1 + 20, y1 + 20, px);
            }
            if (!shown && px != colpix[0])
                absent++;
        }
        if (frame == frames - 1 && seen_bad)
            failures++;
    }
    if (mode == 2 && frame > WARM) {
        /* zone commune : fenêtre 1 (devant) en (x1, y1), fenêtre 0 en (x0, y0) */
        int x1, y1;
        unsigned long px;
        glutSetWindow(win[1]);
        x1 = glutGet(GLUT_WINDOW_X); y1 = glutGet(GLUT_WINDOW_Y);
        px = vram_px(x1 + 20, y1 + 20);
        sampled++;
        if (px == colpix[0]) {
            seen_bad++;
            if (seen_bad <= 4)
                printf("cover : image %d, (%d,%d) = %06lx (fenêtre de derrière)\n",
                       frame, x1 + 20, y1 + 20, px);
        }
        if (frame == frames - 1 && (seen_bad || px != colpix[1])) {
            failures++;
            printf("cover : fin, (%d,%d) = %06lx (attendu %06lx), %d relevés mauvais\n",
                   x1 + 20, y1 + 20, px, colpix[1], seen_bad);
        }
    }
}

static void display(void)
{
}

static int run_fs(void)
{
    CGDirectDisplayID display = CGMainDisplayID();
    CFDictionaryRef original = CGDisplayCurrentMode(display), m;
    boolean_t exact;
    CGLPixelFormatObj pf = NULL;
    CGLContextObj ctx = NULL;
    GLint count;
    CGLError error;
    int pass = 0, captured = 0, i;
    unsigned long px;
    double t = 0;
    ProcessSerialNumber psn = { 0, kCurrentProcess };
    CGLPixelFormatAttribute attrs[] = { kCGLPFAFullScreen, kCGLPFADoubleBuffer,
        kCGLPFAColorSize, 32, kCGLPFADepthSize, 24, kCGLPFADisplayMask, 0, 0 };
    attrs[7] = (CGLPixelFormatAttribute)CGDisplayIDToOpenGLDisplayMask(display);
    CFRetain(original);
    SetFrontProcess(&psn);
    m = CGDisplayBestModeForParameters(display, 32, W, H, &exact);
    if (!m || !exact || CGDisplayCapture(display)) goto done;
    captured = 1;
    if (CGDisplaySwitchToMode(display, m)) goto done;
    HideMenuBar();
    CGDisplayHideCursor(display);
    error = CGLChoosePixelFormat(attrs, &pf, &count);
    if (error || !pf) { printf("pixel format: %d\n", error); goto done; }
    error = CGLCreateContext(pf, NULL, &ctx);
    if (error) { printf("context: %d\n", error); goto done; }
    CGLSetCurrentContext(ctx);
    error = CGLSetFullScreen(ctx);
    if (error) { printf("CGLSetFullScreen: %d\n", error); goto done; }
    printf("GL_RENDERER = %s ; %lux%lu\n", glGetString(GL_RENDERER),
           (unsigned long)CGDisplayPixelsWide(display), (unsigned long)CGDisplayPixelsHigh(display));
    glViewport(0, 0, W, H);
    for (i = 0; i < frames; i++) {
        if (i == WARM)
            t = now();
        draw(0, (i & 63) / 63.0f);
        CGLFlushDrawable(ctx);
    }
    t = now() - t;
    printf("swapbench fs : %d images %dx%d en %.3f s : %.3f ms/image\n",
           frames - WARM, W, H, t, t * 1000 / (frames - WARM));
    glFinish();
    CGLFlushDrawable(ctx);
    CGLFlushDrawable(ctx);
    glFinish();
    usleep(100000);
    px = vram_px(W / 2, H / 2);
    printf("fs : centre %06lx attendu %06lx\n", px, colpix[0]);
    pass = px == colpix[0] && !glGetError();
    CGLClearDrawable(ctx);
done:
    CGLSetCurrentContext(NULL);
    if (ctx) CGLDestroyContext(ctx);
    if (pf) CGLDestroyPixelFormat(pf);
    if (captured) {
        CGDisplaySwitchToMode(display, original);
        CGDisplayShowCursor(display);
        ShowMenuBar();
        CGDisplayRelease(display);
    }
    CFRelease(original);
    printf("%s\n", pass ? "OK (0 échec(s))" : "ÉCHEC (1 échec(s))");
    return pass ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *m = argc > 1 ? argv[1] : "fen";
    if (getenv("SWB_WORK")) work = atol(getenv("SWB_WORK"));
    if (getenv("SWB_TEXDEL")) texdel = atoi(getenv("SWB_TEXDEL"));
    if (argc > 2) frames = atoi(argv[2]);
    if (argc > 4) { W = atoi(argv[3]); H = atoi(argv[4]); }
    if (frames <= WARM + 1) frames = WARM + 2;
    if (!strcmp(m, "fs"))
        return run_fs();
    mode = !strcmp(m, "move") ? 1 : !strcmp(m, "cover") ? 2 : !strcmp(m, "pop") ? 3 : 0;
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
    glutInitWindowSize(W, H);
    glutInitWindowPosition(40, 60);
    win[0] = glutCreateWindow("POMPPC swapbench");
    glutDisplayFunc(display);
    if (mode == 2 || mode == 3) {
        glutInitWindowPosition(40 + W / 2, 60 + H / 2);
        win[1] = glutCreateWindow("POMPPC swapbench 2");
        glutDisplayFunc(display);
        nwin = 2;
    }
    printf("GL_RENDERER = %s\n", glGetString(GL_RENDERER));
    fflush(stdout);
    glutIdleFunc(idle);
    glutMainLoop();
    return 0;
}
