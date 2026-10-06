/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * tlbtest.c - invalidations du TLB vues de l'invite : preuve de x-tlb-precise
 * (patches/tcg/0031, docs/tcg-g4.md section 32).
 *
 * x-tlb-precise ne vide plus le TLB logiciel entier a chaque tlbie, a chaque
 * changement de registre de segment ni a chaque ecriture de BAT : il retire
 * les entrees concernees.  Ce qui peut casser se voit donc dans l'invite par
 * une page qui garde son ancienne traduction :
 *   A. mprotect : lecture seule, ecriture -> faute (adresse exacte), retour
 *      en ecriture ; PROT_NONE -> faute en lecture ;
 *   B. munmap / mmap a la meme adresse : la nouvelle page est nulle, l'ancien
 *      contenu ne doit jamais reapparaitre ;
 *   C. fork et copie sur ecriture : le fils ecrit, le pere ne doit pas voir
 *      ses ecritures, et reciproquement (deux espaces, memes adresses) ;
 *   D. tubes entre processus (copyin/copyout du noyau par la fenetre de
 *      segment) : blocs a motif, sommes de controle des deux cotes ;
 *   E. fichier : ecriture puis relecture a des decalages varies (tampons du
 *      noyau, pages de cache).
 * Tout ce qui est observe passe dans une empreinte FNV : elle doit etre
 * IDENTIQUE avec et sans la propriete (et avec le verificateur).
 * Puis des bancs (hors empreinte) : aller-retour par tubes entre deux
 * processus, mmap/munmap, fork+exit, lectures de fichier.
 *   tlbtest [N]   N = echelle (defaut 1)
 *   tlbtest banc  bancs seulement
 */
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned long long fnv = 1469598103934665603ULL;
static long P;
static int errors;

static void h(const void *p, size_t n)
{
    const unsigned char *c = p;
    while (n--) { fnv ^= *c++; fnv *= 1099511628211ULL; }
}

static void out(const char *fmt, unsigned long a, unsigned long b, unsigned long c)
{
    char buf[256];
    int n = snprintf(buf, sizeof buf, fmt, a, b, c);
    fputs(buf, stdout);
    h(buf, n);
}

static unsigned long rng = 12345;
static unsigned long rnd(void)
{
    rng = rng * 1103515245UL + 12345UL;
    return (rng >> 8) & 0xffffff;
}

static sigjmp_buf jb;
static volatile unsigned long fault_addr;
static void onfault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    fault_addr = (unsigned long)si->si_addr;
    siglongjmp(jb, sig);
}

static unsigned sum(const unsigned char *p, size_t n)
{
    unsigned s = 0;
    while (n--) s = s * 31 + *p++;
    return s;
}

static void fill(unsigned char *p, size_t n, unsigned seed)
{
    size_t i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)(seed + i * 7 + (i >> 9));
}

/* A. protections et fautes */
static void test_a(unsigned char *base, int npages, int rounds)
{
    int r, k, sig;
    for (r = 0; r < rounds; r++) {
        int pg = rnd() % npages;
        unsigned char *p = base + (long)pg * P;
        long off = (rnd() % (P / 4)) * 4;
        mprotect(p, P, PROT_READ);
        fault_addr = 0;
        if ((sig = sigsetjmp(jb, 1)) == 0) {
            *(volatile unsigned *)(p + off) = 0xdeadbeef;
            out("A %lu: ecriture sur lecture seule SANS FAUTE (%lu)\n", r, pg, 0);
            errors++;
        } else {
            out("A %lu: faute %lu a +%lx", r, sig, fault_addr - (unsigned long)base);
            out(" (page %lu)\n", pg, 0, 0);
        }
        mprotect(p, P, PROT_NONE);
        if ((sig = sigsetjmp(jb, 1)) == 0) {
            k = *(volatile unsigned char *)(p + off);
            out("A %lu: lecture sur PROT_NONE SANS FAUTE (%lu)\n", r, k, 0);
            errors++;
        } else {
            out("A %lu: faute lecture %lu a +%lx\n", r, sig, fault_addr - (unsigned long)base);
        }
        mprotect(p, P, PROT_READ | PROT_WRITE);
        *(volatile unsigned *)(p + off) = 0x1000000u * (r & 0xff) + pg;
        out("A %lu: relu %08lx somme %08lx\n", r, *(volatile unsigned *)(p + off),
            sum(base, (size_t)npages * P));
    }
}

/* B. munmap/mmap a la meme adresse */
static void test_b(unsigned char *base, int npages, int rounds)
{
    int r, i;
    for (r = 0; r < rounds; r++) {
        int pg = rnd() % npages, n = 1 + rnd() % 4;
        unsigned char *p = base + (long)pg * P, *q;
        if (pg + n > npages) n = npages - pg;
        fill(p, (size_t)n * P, r);
        munmap(p, (size_t)n * P);
        q = mmap(p, (size_t)n * P, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
        if (q != p) {
            out("B %lu: mmap rend une autre adresse\n", r, 0, 0);
            errors++;
            continue;
        }
        for (i = 0; i < n * P; i++) {
            if (q[i]) {
                out("B %lu: ANCIEN CONTENU visible a +%lx (%02lx)\n", r, (unsigned long)(i), q[i]);
                errors++;
                break;
            }
        }
        fill(q, (size_t)n * P, r + 1000);
        out("B %lu: %lu pages, somme %08lx\n", r, n, sum(base, (size_t)npages * P));
    }
}

/* C. fork et copie sur ecriture */
static void test_c(unsigned char *base, int npages, int rounds)
{
    int r, st, fd[2];
    for (r = 0; r < rounds; r++) {
        unsigned before = sum(base, (size_t)npages * P), child_sum = 0;
        pid_t pid;
        pipe(fd);
        pid = fork();
        if (pid == 0) {
            int i;
            unsigned s;
            close(fd[0]);
            for (i = 0; i < npages; i += 2) fill(base + (long)i * P, P, 77 + r);
            s = sum(base, (size_t)npages * P);
            write(fd[1], &s, sizeof s);
            _exit(0);
        }
        close(fd[1]);
        read(fd[0], &child_sum, sizeof child_sum);
        close(fd[0]);
        waitpid(pid, &st, 0);
        if (sum(base, (size_t)npages * P) != before) {
            out("C %lu: le PERE voit les ecritures du fils\n", r, 0, 0);
            errors++;
        }
        fill(base + (long)(r % npages) * P, P, 300 + r);
        out("C %lu: fils %08lx pere %08lx\n", r, child_sum, sum(base, (size_t)npages * P));
    }
}

/* D. tubes entre processus (copyin/copyout) */
static void test_d(int rounds)
{
    int r, st, a[2], b[2];
    static unsigned char buf[65536], back[65536];
    pid_t pid;
    pipe(a); pipe(b);
    pid = fork();
    if (pid == 0) {
        static unsigned char cb[65536];
        int n;
        close(a[1]); close(b[0]);
        while ((n = read(a[0], cb, sizeof cb)) > 0) {
            int i;
            for (i = 0; i < n; i++) cb[i] ^= 0x5a;
            write(b[1], cb, n);
        }
        _exit(0);
    }
    close(a[0]); close(b[1]);
    for (r = 0; r < rounds; r++) {
        int n = 1 + rnd() % 8000, got = 0, k, i, bad = 0;
        fill(buf, n, r * 3);
        write(a[1], buf, n);
        while (got < n && (k = read(b[0], back + got, n - got)) > 0) got += k;
        for (i = 0; i < n; i++) bad += back[i] != (buf[i] ^ 0x5a);
        if (bad) errors++;
        out("D %lu: %lu octets, %lu faux\n", r, n, bad);
    }
    close(a[1]);
    waitpid(pid, &st, 0);
    close(b[0]);
}

/* E. fichier */
static void test_e(int rounds)
{
    static unsigned char buf[131072], rd[131072];
    const char *fn = "/tmp/tlbtest.dat";
    int fd = open(fn, O_RDWR | O_CREAT | O_TRUNC, 0600), r;
    fill(buf, sizeof buf, 9);
    write(fd, buf, sizeof buf);
    for (r = 0; r < rounds; r++) {
        long off = rnd() % (sizeof buf - 9000), n = 1 + rnd() % 9000;
        lseek(fd, off, SEEK_SET);
        memset(rd, 0, n);
        read(fd, rd, n);
        if (memcmp(rd, buf + off, n)) errors++;
        out("E %lu: +%lx somme %08lx\n", r, off, sum(rd, n));
        if (r % 3 == 0) {
            fill(buf + off, n, r);
            lseek(fd, off, SEEK_SET);
            write(fd, buf + off, n);
        }
    }
    close(fd);
    unlink(fn);
}

static long ms_since(struct timeval *t0)
{
    struct timeval t1;
    gettimeofday(&t1, 0);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_usec - t0->tv_usec) / 1000;
}

static void bench(long n)
{
    struct timeval t0;
    int a[2], b[2], st, i;
    char c = 0;
    pid_t pid;
    unsigned char *m;
    /* aller-retour : deux processus, changement de contexte a chaque octet */
    pipe(a); pipe(b);
    pid = fork();
    if (pid == 0) {
        close(a[1]); close(b[0]);
        while (read(a[0], &c, 1) == 1) write(b[1], &c, 1);
        _exit(0);
    }
    close(a[0]); close(b[1]);
    gettimeofday(&t0, 0);
    for (i = 0; i < 20000 * n; i++) { write(a[1], &c, 1); read(b[0], &c, 1); }
    printf("banc tube %ld allers-retours : %ld ms\n", 20000 * n, ms_since(&t0));
    close(a[1]); waitpid(pid, &st, 0); close(b[0]);
    /* mmap / toucher 16 pages / munmap : tlbie a chaque page rendue */
    gettimeofday(&t0, 0);
    for (i = 0; i < 2000 * n; i++) {
        int k;
        m = mmap(0, 16 * P, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        for (k = 0; k < 16; k++) m[k * P] = k;
        munmap(m, 16 * P);
    }
    printf("banc mmap %ld x 16 pages : %ld ms\n", 2000 * n, ms_since(&t0));
    /* fork + exit */
    gettimeofday(&t0, 0);
    for (i = 0; i < 200 * n; i++) {
        pid = fork();
        if (pid == 0) _exit(0);
        waitpid(pid, &st, 0);
    }
    printf("banc fork %ld : %ld ms\n", 200 * n, ms_since(&t0));
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    int N = 1, npages = 64;
    unsigned char *base;

    P = getpagesize();
    if (argc > 1 && !strcmp(argv[1], "banc")) {
        bench(argc > 2 ? atol(argv[2]) : 1);
        return 0;
    }
    if (argc > 1) N = atoi(argv[1]);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = onfault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    base = mmap(0, (size_t)npages * P, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    fill(base, (size_t)npages * P, 1);
    test_a(base, npages, 200 * N);
    test_b(base, npages, 200 * N);
    test_c(base, npages, 40 * N);
    test_d(300 * N);
    test_e(300 * N);
    printf("erreurs %d\n", errors);
    printf("empreinte %016llx\n", fnv);
    bench(1);
    return errors != 0;
}
