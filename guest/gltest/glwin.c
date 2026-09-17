/*
 * glwin.c — test OpenGL FENÊTRÉ pour Tiger (GLUT) : double tampon, échanges,
 * profondeur, dans une vraie fenêtre du WindowServer.
 *
 *   glwin [images] [largeur hauteur] [sortie.ppm] [pause_s]
 *
 * Anime une scène (éventails lissés, test de profondeur) pendant `images`
 * images, mesure le débit, puis dessine une image témoin, la relit dans le
 * tampon arrière (glReadPixels), vérifie des pixels, l'écrit en PPM, échange,
 * et laisse la fenêtre affichée `pause_s` secondes (capture d'écran côté hôte).
 * Code de sortie 0 si les pixels témoins sont bons.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/time.h>
#include <GLUT/glut.h>
#include <OpenGL/gl.h>

static int W = 320, H = 240, frames = 120, frame, pause_s = 4;
static const char *out = "glwin.ppm";
static double t0;
static int failures;

static double now(void)
{
    struct timeval tv; gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void scene(float angle)
{
    int i;
    glClearColor(0.05f, 0.05f, 0.15f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-1.33, 1.33, -1, 1, 1.5, 20);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glTranslatef(0, 0, -4);
    glRotatef(angle, 0.3f, 1, 0.2f);
    for (i = 0; i < 6; i++) {
        int k;
        glPushMatrix();
        glRotatef(i * 60.0f, 0, 0, 1);
        glTranslatef(0, 0.6f, (i % 2) ? 0.3f : -0.3f);
        glBegin(GL_TRIANGLE_FAN);
        glColor3f(1, 1, 1); glVertex3f(0, 0, 0.2f);
        for (k = 0; k <= 24; k++) {
            float a = k * 6.2831853f / 24;
            glColor3f(0.5f + 0.5f * cos(a + i), 0.5f + 0.5f * sin(a * 2 + i), (i % 3) / 2.0f);
            glVertex3f(0.5f * cos(a), 0.5f * sin(a), 0);
        }
        glEnd();
        glPopMatrix();
    }
}

/* Image témoin : repère orthographique en pixels, origine en bas à gauche. */
static void witness(void)
{
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0);                       /* rouge loin, moitié gauche */
    glVertex3f(10, 10, -0.5f); glVertex3f(W / 2, 10, -0.5f); glVertex3f(10, H - 10, -0.5f);
    glColor3f(0, 1, 0);                       /* vert proche, recouvre en partie */
    glVertex3f(40, 40, 0.5f); glVertex3f(W - 10, 40, 0.5f); glVertex3f(40, H - 10, 0.5f);
    glEnd();
}

static unsigned long pixel(const unsigned char *img, int x, int y)   /* y depuis le bas */
{
    const unsigned char *p = img + (y * W + x) * 3;
    return ((unsigned long)p[0] << 16) | (p[1] << 8) | p[2];
}

static void check(const unsigned char *img, const char *what, int x, int y, unsigned long want)
{
    unsigned long got = pixel(img, x, y);
    printf("  %s %-24s (%3d,%3d) = %06lx\n", got == want ? "ok  " : "FAIL", what, x, y, got);
    if (got != want)
        failures++;
}

static void finish(void)
{
    double dt = now() - t0;
    unsigned char *img = malloc(W * H * 3);
    FILE *f;
    int y;

    printf("animation : %d images %dx%d en %.2f s : %.1f img/s\n", frames, W, H, dt, frames / dt);
    witness();
    glFinish();
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, img);
    check(img, "fond bleu", 5, 5, 0x0000FF);
    check(img, "rouge (loin, seul)", 20, 20, 0xFF0000);
    check(img, "vert devant le rouge", 50, 50, 0x00FF00);
    check(img, "vert seul", W - 30, 45, 0x00FF00);
    f = fopen(out, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (y = H - 1; y >= 0; y--)
            fwrite(img + y * W * 3, 1, W * 3, f);
        fclose(f);
    }
    glutSwapBuffers();
    printf("%s (%d échec(s))\n", failures ? "ÉCHEC" : "OK", failures);
    fflush(stdout);
    sleep(pause_s);
    exit(failures ? 1 : 0);
}

static void display(void)
{
    if (frame == 0) {
        printf("GL_VENDOR   = %s\nGL_RENDERER = %s\nGL_VERSION  = %s\n",
               glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
        fflush(stdout);
        t0 = now();
    }
    if (frame >= frames) {
        finish();
        return;
    }
    scene(frame * 4.0f);
    glutSwapBuffers();
    frame++;
}

static void idle(void)
{
    glutPostRedisplay();
}

int main(int argc, char **argv)
{
    glutInit(&argc, argv);
    if (argc > 1) frames = atoi(argv[1]);
    if (argc > 3) { W = atoi(argv[2]); H = atoi(argv[3]); }
    if (argc > 4) out = argv[4];
    if (argc > 5) pause_s = atoi(argv[5]);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH);
    glutInitWindowSize(W, H);
    glutInitWindowPosition(40, 60);
    glutCreateWindow("POMPPC glwin");
    glEnable(GL_DEPTH_TEST);
    glShadeModel(GL_SMOOTH);
    glutDisplayFunc(display);
    glutIdleFunc(idle);
    glutMainLoop();
    return 0;
}
