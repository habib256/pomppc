/* regbench.c - noyaux courts pour lire le « regime » de vitesse d'un demarrage
 * (docs/tcg-g4.md §14) : chacun vise ~1-2 s sur le G4 emule, chacun sollicite
 * un chemin different de TCG :
 *   call  appels/retours (bl/blr : helper_lookup_tb_ptr)
 *   mem   lectures pseudo-aleatoires sur 64 Mo (TLB logiciel de QEMU)
 *   fp    chaine de fmadds (flottant scalaire, helpers softfloat)
 *   sys   appels systeme getppid (sc, rfi, mtmsr : verrou global de QEMU)
 *   ctx   ping-pong par tube entre deux processus (changements de contexte :
 *         mtsrin, x-sr-tlb, reveils inter-vCPU)
 *   copy  memcpy de 1 Mo (lmw/stmw, lvx/stvx)
 * Sortie : une ligne « noyau ms » par noyau et par tour (NTOUR tours). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>

static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec * 1e3 + t.tv_usec / 1e3; }

volatile unsigned sink;
__attribute__((noinline)) unsigned f1(unsigned x) { return x * 2654435761u + 1; }
__attribute__((noinline)) unsigned f2(unsigned x) { return f1(x) ^ (x >> 3); }

static void k_call(void) { unsigned x = 1, i; for (i = 0; i < 30000000; i++) x = f2(x); sink = x; }

static unsigned *big;
#define BIGW (16u << 20)            /* 64 Mo */
static void k_mem(void) {
  unsigned x = 12345, s = 0, i;
  for (i = 0; i < 20000000; i++) { x = x * 1103515245u + 12345u; s += big[(x >> 6) & (BIGW - 1)]; }
  sink = s;
}
static void k_fp(void) {
  volatile float a = 1.0001f, b = 0.9999f; float c = 0.5f; int i;
  for (i = 0; i < 30000000; i++) c = c * a + b * 0.25f - c * 0.0001f;
  sink = (unsigned)c;
}
static void k_sys(void) { int i; unsigned s = 0; for (i = 0; i < 1500000; i++) s += getppid(); sink = s; }
static void k_ctx(void) {
  int a[2], b[2], i; char c = 0;
  pipe(a); pipe(b);
  pid_t p = fork();
  if (p == 0) { for (i = 0; i < 100000; i++) { read(a[0], &c, 1); write(b[1], &c, 1); } _exit(0); }
  for (i = 0; i < 100000; i++) { write(a[1], &c, 1); read(b[0], &c, 1); }
  waitpid(p, 0, 0); close(a[0]); close(a[1]); close(b[0]); close(b[1]);
}
static char *src, *dst;
static void k_copy(void) { int i; for (i = 0; i < 1500; i++) { memcpy(dst, src, 1 << 20); src[i & 1023]++; } }

int main(int argc, char **argv) {
  int ntour = argc > 1 ? atoi(argv[1]) : 3, t, k;
  struct { const char *n; void (*f)(void); } K[] = {
    {"call", k_call}, {"mem", k_mem}, {"fp", k_fp}, {"sys", k_sys}, {"ctx", k_ctx}, {"copy", k_copy}};
  unsigned i;
  big = malloc(BIGW * 4); for (i = 0; i < BIGW; i++) big[i] = i;
  src = malloc(1 << 20); dst = malloc(1 << 20); memset(src, 1, 1 << 20);
  for (t = 0; t < ntour; t++)
    for (k = 0; k < 6; k++) {
      double t0 = now(); K[k].f(); printf("%s %.0f\n", K[k].n, now() - t0); fflush(stdout);
    }
  return 0;
}
