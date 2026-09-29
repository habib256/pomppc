/* GPL3 - Copyleft VERHILLE Arnaud
 *
 * stallmeter — mesure, DANS l'invité, les arrêts des vCPU vus par la base de
 * temps (29/09/2026, chantier « gel au chargement de DOOM 3 »).
 *
 * La base de temps du G4 émulé suit l'horloge de l'hôte : un vCPU arrêté par
 * QEMU (BQL, section exclusive, hôte chargé) voit mftb sauter d'un coup. Les
 * verrous tournants du noyau de Tiger paniquent au bout de LockTimeOut =
 * 0x5f49c1 tops = 250 ms (tbfrequency 24 979 204) : tout arrêt d'un vCPU
 * TENANT un verrou au-delà de 250 ms fait paniquer l'autre qui l'attend.
 *
 * N fils (défaut 2, un par vCPU) bouclent sur mftb et notent chaque saut
 * au-dessus de 20 ms ; un saut vient soit d'un arrêt du vCPU (QEMU), soit
 * d'une préemption par l'ordonnanceur de Tiger — d'où `nice -20` en root
 * et deux fils : un arrêt de QEMU sur UN vCPU ne touche qu'un fil.
 *
 *   stallmeter SECONDES [FILS]    → une ligne par saut ≥ 20 ms, bilan à la fin
 *   SM_SLEEP=1 stallmeter …       → fils endormis 1 ms entre deux lectures :
 *                                   presque rien pris au jeu mesuré, et le
 *                                   saut compte alors aussi la latence de réveil
 *
 * Compilation dans l'invité : gcc -O2 -o stallmeter stallmeter.c -lpthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <unistd.h>

static unsigned long long tbfreq;
static volatile int fini;
static int nfils = 2;
static int dort;                 /* SM_SLEEP=1 : usleep(1 ms) entre deux lectures */

static unsigned long long mftb64(void)
{
    unsigned long hi, lo, hi2;
    do {
        __asm__ volatile("mftbu %0" : "=r"(hi));
        __asm__ volatile("mftb %0"  : "=r"(lo));
        __asm__ volatile("mftbu %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((unsigned long long)hi << 32) | lo;
}

typedef struct {
    int id;
    unsigned long long max_ms_x10;
    unsigned long n20, n100, n250, n1000;
} Fil;

static Fil fils[8];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static void *boucle(void *arg)
{
    Fil *f = (Fil *)arg;
    unsigned long long seuil = tbfreq / 50;         /* 20 ms */
    unsigned long long a = mftb64(), b, d;
    while (!fini) {
        if (dort)
            usleep(1000);
        b = mftb64();
        d = b - a;
        if (d >= seuil) {
            unsigned long long ms10 = d * 10000ULL / tbfreq;
            struct timeval tv;
            gettimeofday(&tv, 0);
            f->n20++;
            if (ms10 >= 1000) f->n100++;
            if (ms10 >= 2500) f->n250++;
            if (ms10 >= 10000) f->n1000++;
            if (ms10 > f->max_ms_x10) f->max_ms_x10 = ms10;
            pthread_mutex_lock(&mu);
            printf("%ld.%03d fil %d saut %llu.%llu ms\n", (long)tv.tv_sec, (int)(tv.tv_usec / 1000),
                   f->id, ms10 / 10, ms10 % 10);
            fflush(stdout);
            pthread_mutex_unlock(&mu);
            b = mftb64();
        }
        a = b;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 60, i;
    union { unsigned int i; unsigned long long q; } u;
    unsigned long v = 0;
    size_t l = sizeof(u);
    pthread_t t[8];
    if (argc > 2) nfils = atoi(argv[2]);
    dort = getenv("SM_SLEEP") && getenv("SM_SLEEP")[0] == '1';
    if (nfils < 1 || nfils > 8) nfils = 2;
    u.q = 0;
    /* hw.tbfrequency : 4 ou 8 octets selon le noyau ; tampon de 8, taille rendue */
    if (sysctlbyname("hw.tbfrequency", &u, &l, 0, 0) == 0)
        v = l == 4 ? u.i : (unsigned long)u.q;
    if (!v) {
        fprintf(stderr, "hw.tbfrequency illisible\n");
        return 1;
    }
    tbfreq = v;
    printf("stallmeter : %d s, %d fils%s, tbfrequency %lu\n", secs, nfils, dort ? " endormis" : "", v);
    fflush(stdout);
    for (i = 0; i < nfils; i++) {
        fils[i].id = i;
        pthread_create(&t[i], 0, boucle, &fils[i]);
    }
    sleep(secs);
    fini = 1;
    for (i = 0; i < nfils; i++)
        pthread_join(t[i], 0);
    for (i = 0; i < nfils; i++)
        printf("bilan fil %d : max %llu.%llu ms ; sauts >=20 ms %lu, >=100 ms %lu, >=250 ms %lu, >=1 s %lu\n",
               i, fils[i].max_ms_x10 / 10, fils[i].max_ms_x10 % 10,
               fils[i].n20, fils[i].n100, fils[i].n250, fils[i].n1000);
    return 0;
}
