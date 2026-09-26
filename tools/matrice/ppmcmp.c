/* ppmcmp.c — comparaison d'images PPM (P6, 8 bits) pour la matrice de jeux
 * (tools/matrice/, docs/matrice-jeux.md). Sans dépendance : l'hôte n'a ni
 * numpy ni PIL.
 *
 *   ppmcmp ECRAN.ppm X Y IMAGE.ppm…
 *       compare chaque IMAGE au rectangle de même taille pris en (X, Y) dans
 *       ECRAN (capture `screendump` de la VM) ; une ligne par image :
 *       « chemin moy max pct16 » (écart moyen par composante, écart maximal,
 *       % de pixels dont une composante s'écarte de plus de 16).
 *   ppmcmp -r A.ppm B.ppm
 *       compare deux images de même taille (rejeu contre référence) :
 *       « moy max pct16 ».
 *   ppmcmp -s IMAGE.ppm
 *       statistiques d'une image : « l h moyenne ecart_type couleurs16 »
 *       (couleurs16 = nombre de teintes distinctes sur 4 bits par composante :
 *       une image noire, unie ou vide en a une ou deux).
 *   ppmcmp -c ECRAN.ppm X Y L H SORTIE.ppm
 *       découpe un rectangle (pour ranger la capture à côté du rejeu).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { int w, h; unsigned char *p; } Img;

static int tok(FILE *f)
{
    int c, v = 0;
    do {
        c = fgetc(f);
        if (c == '#')
            while (c != '\n' && c != EOF) c = fgetc(f);
    } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
    if (c < '0' || c > '9') return -1;
    while (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); c = fgetc(f); }
    return v;
}

static int load(const char *path, Img *im)
{
    FILE *f = fopen(path, "rb");
    int mx;
    if (!f) { perror(path); return 0; }
    if (fgetc(f) != 'P' || fgetc(f) != '6') { fprintf(stderr, "%s : pas un P6\n", path); fclose(f); return 0; }
    im->w = tok(f); im->h = tok(f); mx = tok(f);
    if (im->w <= 0 || im->h <= 0 || mx != 255) { fprintf(stderr, "%s : en-tête\n", path); fclose(f); return 0; }
    im->p = malloc((size_t)im->w * im->h * 3);
    if (!im->p || fread(im->p, 3, (size_t)im->w * im->h, f) != (size_t)im->w * im->h) {
        fprintf(stderr, "%s : tronqué\n", path); fclose(f); return 0;
    }
    fclose(f);
    return 1;
}

/* écart entre b (w×h) et a pris en (x, y) ; hors de a = écart maximal */
static void diff(const Img *a, int x, int y, const Img *b, double *moy, int *mx, double *pct)
{
    long long sum = 0, over = 0, n = (long long)b->w * b->h;
    int m = 0, i, j, k;
    for (j = 0; j < b->h; j++)
        for (i = 0; i < b->w; i++) {
            const unsigned char *pb = b->p + ((size_t)j * b->w + i) * 3;
            int ax = x + i, ay = y + j, o = 0;
            if (ax < 0 || ay < 0 || ax >= a->w || ay >= a->h) {
                sum += 3 * 255; m = 255; over++;
                continue;
            }
            const unsigned char *pa = a->p + ((size_t)ay * a->w + ax) * 3;
            for (k = 0; k < 3; k++) {
                int d = abs((int)pa[k] - (int)pb[k]);
                sum += d;
                if (d > m) m = d;
                if (d > 16) o = 1;
            }
            over += o;
        }
    *moy = n ? (double)sum / (3.0 * n) : 0;
    *mx = m;
    *pct = n ? 100.0 * over / n : 0;
}

int main(int argc, char **argv)
{
    Img a, b;
    double moy, pct;
    int mx, i;
    if (argc == 3 && !strcmp(argv[1], "-s")) {
        static unsigned char seen[4096];
        long long n, s = 0;
        double s2 = 0, m;
        int nc = 0;
        size_t q;
        if (!load(argv[2], &a)) return 2;
        n = (long long)a.w * a.h;
        for (q = 0; q < (size_t)n; q++) {
            unsigned char *p = a.p + q * 3;
            int l = (p[0] + p[1] + p[2]) / 3, c = (p[0] >> 4) << 8 | (p[1] >> 4) << 4 | (p[2] >> 4);
            s += l; s2 += (double)l * l;
            if (!seen[c]) { seen[c] = 1; nc++; }
        }
        m = (double)s / n;
        printf("%d %d %.2f %.2f %d\n", a.w, a.h, m, sqrt(s2 / n - m * m > 0 ? s2 / n - m * m : 0), nc);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "-r")) {
        if (!load(argv[2], &a) || !load(argv[3], &b)) return 2;
        if (a.w != b.w || a.h != b.h) { printf("taille %dx%d contre %dx%d\n", a.w, a.h, b.w, b.h); return 1; }
        diff(&a, 0, 0, &b, &moy, &mx, &pct);
        printf("%.3f %d %.3f\n", moy, mx, pct);
        return 0;
    }
    if (argc == 8 && !strcmp(argv[1], "-c")) {
        int x = atoi(argv[3]), y = atoi(argv[4]), w = atoi(argv[5]), h = atoi(argv[6]), j;
        FILE *f;
        if (!load(argv[2], &a)) return 2;
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > a.w || y + h > a.h) { fprintf(stderr, "hors image\n"); return 1; }
        f = fopen(argv[7], "wb");
        if (!f) { perror(argv[7]); return 2; }
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (j = 0; j < h; j++) fwrite(a.p + ((size_t)(y + j) * a.w + x) * 3, 3, w, f);
        fclose(f);
        return 0;
    }
    if (argc < 5) {
        fprintf(stderr, "usage : ppmcmp ECRAN.ppm X Y IMAGE.ppm… | -r A B | -s IMAGE | -c ECRAN X Y L H SORTIE\n");
        return 2;
    }
    if (!load(argv[1], &a)) return 2;
    for (i = 4; i < argc; i++) {
        if (!load(argv[i], &b)) continue;
        diff(&a, atoi(argv[2]), atoi(argv[3]), &b, &moy, &mx, &pct);
        printf("%s %.3f %d %.3f\n", argv[i], moy, mx, pct);
        free(b.p);
    }
    return 0;
}
