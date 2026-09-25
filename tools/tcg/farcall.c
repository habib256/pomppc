/*
 * farcall.c — banc hôte (Apple Silicon) : un appel indirect (BLR) depuis du
 * code généré vers une fonction du binaire coûte-t-il plus cher quand le code
 * généré est LOIN (> 4 Gio) du binaire ? (docs/tcg-g4.md §14, les deux régimes)
 *
 * Imite ce que TCG émet : NB blocs de code, chacun charge l'adresse d'un
 * « helper » (8 fonctions du binaire) par MOVZ/MOVK ×4 — la MÊME suite
 * d'instructions dans les deux cas —, BLR, puis branche au bloc suivant.
 * Seule l'adresse du tampon change : juste après le texte (« près ») ou
 * au-delà de 4 Gio (« loin »). Imprime ns par appel, 5 tours alternés.
 *
 *   cc -O2 -o farcall tools/tcg/farcall.c && ./farcall [NB] [TOURS] [LOIN] [HELPERS]
 * LOIN : adresse demandée pour le tampon « loin » (défaut 0x300000000) ;
 * HELPERS : les helpers sont recopiés dans un tampon JIT à cette adresse (les
 * écarts imprimés sont alors mesurés depuis lui).
 */
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#define H(n) __attribute__((noinline)) uint64_t h##n(uint64_t x) { return x * 3 + n; }
H(0) H(1) H(2) H(3) H(4) H(5) H(6) H(7)
static uint64_t (*hs[8])(uint64_t) = {h0, h1, h2, h3, h4, h5, h6, h7};

static uint32_t *emit_movi(uint32_t *p, int rd, uint64_t v) {
  *p++ = 0xd2800000 | ((v & 0xffff) << 5) | rd;                  /* movz */
  *p++ = 0xf2a00000 | (((v >> 16) & 0xffff) << 5) | rd;          /* movk lsl 16 */
  *p++ = 0xf2c00000 | (((v >> 32) & 0xffff) << 5) | rd;          /* movk lsl 32 */
  *p++ = 0xf2e00000 | (((v >> 48) & 0xffff) << 5) | rd;          /* movk lsl 48 */
  return p;
}

/* fonction générée : x0 = nombre de tours de la chaîne de blocs */
static void *build(void *hint, int nb, int stride, void **base_out) {
  size_t size = (size_t)nb * stride + 65536;
  void *buf = mmap(hint, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
  if (buf == MAP_FAILED) { perror("mmap"); exit(1); }
  *base_out = buf;
  pthread_jit_write_protect_np(0);
  uint32_t *p = buf;
  /* prologue */
  *p++ = 0xa9be7bfd;              /* stp x29, x30, [sp, #-32]! */
  *p++ = 0xa90153f3;              /* stp x19, x20, [sp, #16] */
  *p++ = 0xaa0003f3;              /* mov x19, x0 (tours) */
  *p++ = 0xd2800014;              /* mov x20, #0 (accumulateur) */
  uint32_t *loop = p;
  uint32_t *blk0 = (uint32_t *)((char *)buf + 4096);
  /* b blk0 */
  *p = 0x14000000 | (((blk0 - p)) & 0x3ffffff); p++;
  uint32_t *ret = p;
  *p++ = 0xa94153f3;              /* ldp x19, x20, [sp, #16] */
  *p++ = 0xa8c27bfd;              /* ldp x29, x30, [sp], #32 */
  *p++ = 0xd65f03c0;              /* ret */
  for (int i = 0; i < nb; i++) {
    uint32_t *q = (uint32_t *)((char *)blk0 + (size_t)i * stride);
    *q++ = 0xaa1403e0;            /* mov x0, x20 */
    q = emit_movi(q, 16, (uint64_t)(uintptr_t)hs[(i * 5) & 7]);
    *q++ = 0xd63f0200;            /* blr x16 */
    *q++ = 0xaa0003f4;            /* mov x20, x0 */
    if (i + 1 < nb) {
      uint32_t *nx = (uint32_t *)((char *)blk0 + (size_t)(i + 1) * stride);
      *q = 0x14000000 | ((nx - q) & 0x3ffffff); q++;   /* b bloc suivant */
    } else {
      *q++ = 0xf1000673;          /* subs x19, x19, #1 */
      /* b.ne loop : trop loin pour b.cond ? on passe par un saut court */
      *q++ = 0x54000041;          /* b.ne +8 */
      *q = 0x14000000 | ((ret - q) & 0x3ffffff); q++;  /* b ret */
      *q = 0x14000000 | ((loop - q) & 0x3ffffff); q++; /* b loop */
    }
  }
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(buf, size);
  return buf;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

int main(int argc, char **argv) {
  int nb = argc > 1 ? atoi(argv[1]) : 4096, tours = argc > 2 ? atoi(argv[2]) : 5;
  int stride = 256;
  uintptr_t text = (uintptr_t)&main;
  void *nb_, *fb_;
  void *nearh = (void *)((text + 512ull * 1024 * 1024) & ~((1ull << 30) - 1));
  void *farh = (void *)0x300000000ull;
  if (argc > 3) farh = (void *)(uintptr_t)strtoull(argv[3], 0, 0);
  if (argc > 4) {  /* helpers recopiés dans un tampon JIT à cette adresse */
    uint32_t *hb = mmap((void *)(uintptr_t)strtoull(argv[4], 0, 0), 16384,
                        PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    pthread_jit_write_protect_np(0);
    for (int k = 0; k < 8; k++) {
      uint32_t *q = hb + k * 16;
      q[0] = 0x8b000401;                 /* add x1, x0, x0, lsl #1 */
      q[1] = 0x91000020 | (k << 10);     /* add x0, x1, #k */
      q[2] = 0xd65f03c0;                 /* ret */
      hs[k] = (uint64_t (*)(uint64_t))(void *)q;
    }
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(hb, 16384);
    printf("helpers recopiés en %p\n", (void *)hb);
    text = (uintptr_t)hb;
  }
  void *nearf = build(nearh, nb, stride, &nb_);
  void *farf = build(farh, nb, stride, &fb_);
  printf("texte %p ; près %p (%+.2f Gio) ; loin %p (%+.2f Gio) ; %d blocs\n",
         (void *)text, nb_, ((intptr_t)nb_ - (intptr_t)text) / 1073741824.0,
         fb_, ((intptr_t)fb_ - (intptr_t)text) / 1073741824.0, nb);
  uint64_t (*fn)(uint64_t) = nearf, (*ff)(uint64_t) = farf;
  long n = 200000000L / nb;
  fn(1000); ff(1000);
  for (int t = 0; t < tours; t++) {
    double a = now(); fn(n); double b = now(); ff(n); double c = now();
    printf("tour %d : près %.3f ns/appel  loin %.3f ns/appel  loin/près %.3f\n", t,
           (b - a) / (n * (double)nb), (c - b) / (n * (double)nb), (c - b) / (b - a));
  }
  return 0;
}
