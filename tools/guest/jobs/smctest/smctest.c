/*
 * smctest.c - code modifie, recopie ou remappe dans l'invite, pour prouver que
 * les sorties indirectes de TCG (blr, bctr : helper_lookup_tb_ptr, et la
 * recherche en ligne de x-ret-inline, patches/tcg/0008) et le cache de sauts
 * garde par mmu_idx (x-jc-idx) ne font jamais tourner un bloc perime.
 *
 * Chaque essai rend des valeurs attendues connues d'avance ; la sortie (une
 * empreinte FNV par essai et le nombre d'erreurs) doit etre identique dans
 * tous les modes, erreurs = 0.
 *
 *   A  JIT maison : f = "li r3,K ; blr" dans une page RWX, appelee a chaud
 *      (bctrl/blr), puis K reecrit (dcbst/sync/icbi/isync) : la valeur suit.
 *   B  retour dans du code modifie PENDANT l'appel : un talon appelle un
 *      patcheur C qui reecrit l'instruction situee a l'adresse de retour du
 *      talon ; le blr du patcheur doit trouver le nouveau code.
 *   C  meme adresse virtuelle, deux pages physiques : deux pages d'un fichier
 *      au code different, mappees tour a tour a la meme adresse (MAP_FIXED).
 *   D  deux processus (fork), meme adresse, code different (copie sur
 *      ecriture), qui alternent par un tube : chaque changement de processus
 *      change les registres de segment (x-sr-tlb, x-jc-idx).
 *   E  deux fils : l'un appelle f en boucle, l'autre la reecrit ; apres avoir
 *      lu la generation publiee (puis isync), l'appelant doit voir au moins
 *      cette generation (MTTCG : invalidation vue de l'autre vCPU).
 *   F  fonction C recopiee a des adresses successives d'un tampon et appelee.
 *
 *   smctest [n]        essais (n = echelle, defaut 1)
 *   smctest banc N     banc d'appels/retours indirects (64 cibles, 3 N appels)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>

typedef int (*fn_t)(void);
static unsigned long errors;
static uint64_t fnv = 0xcbf29ce484222325ULL;

static void mix(uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) {
        fnv ^= (v >> (8 * i)) & 0xff;
        fnv *= 0x100000001b3ULL;
    }
}

static void flush_code(void *p, size_t len)
{
    char *a = (char *)((uintptr_t)p & ~31UL), *e = (char *)p + len;
    char *q;
    for (q = a; q < e; q += 32) {
        __asm__ volatile("dcbst 0,%0" : : "r"(q) : "memory");
    }
    __asm__ volatile("sync" ::: "memory");
    for (q = a; q < e; q += 32) {
        __asm__ volatile("icbi 0,%0" : : "r"(q) : "memory");
    }
    __asm__ volatile("sync\n\tisync" ::: "memory");
}

#define LI3(k)  (0x38600000u | ((uint32_t)(k) & 0xffff))
#define BLR     0x4e800020u

static void *rwx(size_t len)
{
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
    return p;
}

static void check(const char *t, int got, int want)
{
    mix((uint32_t)got);
    if (got != want) {
        if (errors < 20) {
            printf("ERREUR %s : %d au lieu de %d\n", t, got, want);
        }
        errors++;
    }
}

static void report(const char *t)
{
    printf("%s empreinte %016llx erreurs %lu\n", t, (unsigned long long)fnv, errors);
    fflush(stdout);
    fnv = 0xcbf29ce484222325ULL;
}

/* ---- A : JIT maison ---------------------------------------------------- */
static void test_a(int n)
{
    uint32_t *c = rwx(4096);
    fn_t f = (fn_t)c;
    int r, i, k;

    for (r = 0; r < 2000 * n; r++) {
        k = (r * 7919) & 0x7fff;
        c[0] = LI3(k);
        c[1] = BLR;
        flush_code(c, 8);
        for (i = 0; i < 200; i++) {
            check("A", f(), k);
        }
    }
    report("A");
}

/* ---- B : retour dans du code modifie pendant l'appel ------------------- */
static uint32_t *stub;
static volatile int patch_to = -1;

static void patcher(void)
{
    if (patch_to >= 0) {
        stub[7] = LI3(patch_to);
        flush_code(&stub[7], 4);
    }
}

static void test_b(int n)
{
    uintptr_t pa = (uintptr_t)patcher;
    fn_t f;
    int r, i, k;

    stub = rwx(4096);
    stub[0] = 0x7c0802a6;                           /* mflr r0 */
    stub[1] = 0x90010008;                           /* stw r0,8(r1) */
    stub[2] = 0x9421ffc0;                           /* stwu r1,-64(r1) */
    stub[3] = 0x3d800000 | (uint32_t)(pa >> 16);    /* lis r12,hi */
    stub[4] = 0x618c0000 | (uint32_t)(pa & 0xffff); /* ori r12,r12,lo */
    stub[5] = 0x7d8903a6;                           /* mtctr r12 */
    stub[6] = 0x4e800421;                           /* bctrl */
    stub[7] = LI3(0);                               /* <- adresse de retour */
    stub[8] = 0x38210040;                           /* addi r1,r1,64 */
    stub[9] = 0x80010008;                           /* lwz r0,8(r1) */
    stub[10] = 0x7c0803a6;                          /* mtlr r0 */
    stub[11] = BLR;
    flush_code(stub, 48);
    f = (fn_t)stub;
    for (r = 0; r < 2000 * n; r++) {
        k = (r * 104729) & 0x7fff;
        patch_to = -1;
        stub[7] = LI3(k);
        flush_code(&stub[7], 4);
        for (i = 0; i < 100; i++) {
            check("B-chaud", f(), k);
        }
        patch_to = k ^ 0x5a5a;
        check("B-pendant", f(), k ^ 0x5a5a);  /* reecrit pendant l'appel */
        patch_to = -1;
        check("B-apres", f(), k ^ 0x5a5a);
    }
    report("B");
}

/* ---- C : meme adresse, deux pages physiques ----------------------------- */
static void test_c(int n)
{
    char path[] = "/tmp/smctest.XXXXXX";
    static uint32_t page[2][1024];
    int fd = mkstemp(path), r, i, which;
    void *x;
    fn_t f;

    if (fd < 0) {
        perror("mkstemp");
        exit(2);
    }
    unlink(path);
    page[0][0] = LI3(1111);
    page[0][1] = BLR;
    page[1][0] = LI3(2222);
    page[1][1] = BLR;
    if (write(fd, page, sizeof(page)) != sizeof(page)) {
        perror("write");
        exit(2);
    }
    x = mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    if (x == MAP_FAILED) {
        perror("mmap C");
        exit(2);
    }
    f = (fn_t)x;
    for (r = 0; r < 3000 * n; r++) {
        which = r & 1;
        if (mmap(x, 4096, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, fd,
                 which * 4096) != x) {
            perror("mmap C fixe");
            exit(2);
        }
        for (i = 0; i < 100; i++) {
            check("C", f(), which ? 2222 : 1111);
        }
    }
    munmap(x, 4096);
    close(fd);
    report("C");
}

/* ---- D : deux processus, meme adresse, code different -------------------- */
static void test_d(int n)
{
    uint32_t *c = rwx(4096);
    fn_t f = (fn_t)c;
    int p2c[2], c2p[2], r, i, st;
    unsigned long cerr = 0;
    char b = 0;
    pid_t pid;

    c[0] = LI3(3333);
    c[1] = BLR;
    flush_code(c, 8);
    for (i = 0; i < 100; i++) {
        check("D-pere", f(), 3333);            /* le code est chaud avant le fork */
    }
    if (pipe(p2c) || pipe(c2p)) {
        perror("pipe");
        exit(2);
    }
    pid = fork();
    if (pid == 0) {
        c[0] = LI3(4444);                      /* copie sur ecriture */
        flush_code(c, 8);
        for (r = 0; r < 5000 * n; r++) {
            if (read(p2c[0], &b, 1) != 1) {
                break;
            }
            for (i = 0; i < 50; i++) {
                if (f() != 4444) {
                    cerr++;
                }
            }
            if (write(c2p[1], &b, 1) != 1) {
                break;
            }
        }
        _exit(cerr > 250 ? 250 : (int)cerr);
    }
    for (r = 0; r < 5000 * n; r++) {
        if (write(p2c[1], &b, 1) != 1 || read(c2p[0], &b, 1) != 1) {
            printf("ERREUR D : tube\n");
            errors++;
            break;
        }
        for (i = 0; i < 50; i++) {
            check("D-pere", f(), 3333);
        }
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("ERREUR D-fils : statut %d\n", st);
        errors++;
    }
    mix((uint32_t)st);
    report("D");
}

/* ---- E : deux fils, l'un reecrit ce que l'autre appelle ------------------ */
static uint32_t *ecode;
static volatile int egen, estop;

static void *e_writer(void *arg)
{
    int g, n = *(int *)arg;
    for (g = 1; g <= 20000 * n; g++) {
        ecode[0] = LI3(g & 0x7fff);
        flush_code(ecode, 4);
        __asm__ volatile("sync" ::: "memory");
        egen = g;
        if ((g & 63) == 0) {
            usleep(100);
        }
    }
    estop = 1;
    return NULL;
}

static void test_e(int n)
{
    pthread_t th;
    fn_t f;
    long calls = 0, bad = 0;
    int g, v, gv;

    ecode = rwx(4096);
    ecode[0] = LI3(0);
    ecode[1] = BLR;
    flush_code(ecode, 8);
    f = (fn_t)ecode;
    pthread_create(&th, NULL, e_writer, &n);
    while (!estop) {
        g = egen;
        __asm__ volatile("isync" ::: "memory");
        v = f();
        calls++;
        /* v = generation g' & 0x7fff avec g' >= g : comparer modulo 2^15 */
        gv = (v - (g & 0x7fff)) & 0x7fff;
        if (gv > 0x3fff) {
            if (bad < 20) {
                printf("ERREUR E : generation %d vue apres %d (memoire %d, rappel %d)\n",
                       v, g, (int)(ecode[0] & 0xffff), f());
            }
            bad++;
        }
    }
    pthread_join(th, NULL);
    check("E-fin", f(), (20000 * n) & 0x7fff);
    errors += bad;
    mix((uint32_t)bad);
    printf("E appels %s\n", calls > 1000 ? "> 1000" : "<= 1000");
    report("E");
}

/* ---- F : fonction recopiee ---------------------------------------------- */
static int __attribute__((noinline)) add_one(int x)
{
    return x + 1;
}

static void test_f(int n)
{
    /* add_one : "addi r3,r3,1 ; blr" a -O2 ; on recopie ses 8 premiers
       octets apres avoir verifie qu'ils se terminent par blr */
    uint32_t *src = (uint32_t *)(void *)add_one;
    char *buf = rwx(65536);
    int r, i, off;
    int (*g)(int);

    if (src[1] != BLR) {
        printf("F : add_one inattendu (%08x %08x), essai saute\n", src[0], src[1]);
        report("F");
        return;
    }
    for (r = 0; r < 4000 * n; r++) {
        off = ((r * 68) % (65536 - 512)) & ~3;
        memcpy(buf + off, src, 8);
        flush_code(buf + off, 8);
        g = (int (*)(int))(buf + off);
        for (i = 0; i < 20; i++) {
            check("F", g(r + i), r + i + 1);
        }
        memset(buf + off, 0, 8);               /* efface : un appel perime planterait */
        flush_code(buf + off, 8);
    }
    report("F");
}

/* ---- banc ----------------------------------------------------------------- */
#define F1(n) static int __attribute__((noinline)) fb##n(int x) { return x * 3 + n; }
#define F8(n) F1(n##0) F1(n##1) F1(n##2) F1(n##3) F1(n##4) F1(n##5) F1(n##6) F1(n##7)
F8(1) F8(2) F8(3) F8(4) F8(5) F8(6) F8(7) F8(8)
#define R8(n) fb##n##0, fb##n##1, fb##n##2, fb##n##3, fb##n##4, fb##n##5, fb##n##6, fb##n##7,
static int (*const tabf[64])(int) = { R8(1) R8(2) R8(3) R8(4) R8(5) R8(6) R8(7) R8(8) };

static int __attribute__((noinline)) level2(int x, int i)
{
    return tabf[(i * 13) & 63](x) + tabf[(i * 7 + 5) & 63](x >> 1);
}

static void banc(long nloop)
{
    struct timeval a, b;
    long i;
    int s = 1;

    gettimeofday(&a, NULL);
    for (i = 0; i < nloop; i++) {
        s = level2(s, (int)i) & 0xffff;
    }
    gettimeofday(&b, NULL);
    printf("banc %ld appels : %ld ms (s=%d)\n", nloop * 3,
           (long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_usec - a.tv_usec) / 1000), s);
}

int main(int argc, char **argv)
{
    int n = 1;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 2 && strcmp(argv[1], "banc") == 0) {
        banc(atol(argv[2]));
        return 0;
    }
    if (argc > 1) {
        n = atoi(argv[1]);
    }
    test_a(n);
    test_b(n);
    test_c(n);
    test_d(n);
    test_e(n);
    test_f(n);
    printf("total erreurs %lu\n", errors);
    return errors != 0;
}
