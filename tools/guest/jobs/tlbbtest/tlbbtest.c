/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * tlbbtest - chargements en forme D sur une meme base (x-tlb-base, patches/tcg/0052,
 * docs/tcg-g4.md section 41), vus de l'invite.
 *
 * Memoire : 4 pages consecutives a V-0x1000 : [-0x1000,0) PROT_NONE, [0,0x1000) la page 3
 * d'un fichier, [0x1000,0x2000) sa page 1 (deux mmap MAP_FIXED : pas la page physique
 * suivante), [0x2000,0x3000) PROT_NONE. Octet de la page de fichier f, decalage o :
 * motif(f, o). Chaque essai est un bloc ecrit dans un tampon executable (un bloc de
 * traduction : pas de branchement avant blr) :
 *   mode 1 (couverture) : un premier chargement A d'une base r3, puis deux chargements B et
 *     B' de la meme base, de toutes tailles et formes (lbz lhz lha lwz lfs lfd, formes u),
 *     a des deplacements autour de chaque bord de page vu de la base ; chaque resultat
 *     range par r4 (stw, stfs, stfd : les rangements ne tuent rien).
 *   mode 2 (base ecrite) : A sur r3, addi r3,r3,X (ou lwzu), puis B sur r3.
 *   mode 3 (registres) : six bases a la fois, A puis deux B chacune, entrelaces (plus de
 *     paires que de registres preserves de l'hote : debordements de la pile de TCG).
 *   mode 4 (etiquettes) : A, stmw (x-lmw-inline : branchement et etiquettes TCG), B.
 * Attendu calcule en C : valeur (motif ; lfs/lfd relus par stfs/stfd, exacts), ou faute
 * (SIGBUS/SIGSEGV) a l'instruction meme (srr0) si l'acces touche une page PROT_NONE ;
 * chaque faute (decalage de l'instruction, DAR - V) entre aussi dans l'empreinte.
 * Sortie : erreurs (ecarts a l'attendu) et empreinte FNV, identiques avec et sans la
 * propriete. BANC=N : bancs (hors empreinte).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ucontext.h>

typedef unsigned int u32;
typedef unsigned long long u64;

#if defined(__DARWIN_UNIX03) && __DARWIN_UNIX03
#define UC_PC(uc) ((uc)->uc_mcontext->__ss.__srr0)
#define UC_DAR(uc) ((uc)->uc_mcontext->__es.__dar)
#else
#define UC_PC(uc) ((uc)->uc_mcontext->ss.srr0)
#define UC_DAR(uc) ((uc)->uc_mcontext->es.dar)
#endif

#define PG 0x1000
static unsigned char *V;            /* page 0 de la zone */
static u32 *kbuf;                   /* le bloc */
static u64 fnv = 1469598103934665603ULL;
static long nerr, ntest, nfault, ndirect;

static void h(const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n--) {
        fnv ^= *b++;
        fnv *= 1099511628211ULL;
    }
}

static unsigned char motif(int f, int o)
{
    return (unsigned char)(f * 0x5b + o * 7 + (o >> 8) * 13 + 1);
}

/* contenu attendu de V + x, -1 si la page est PROT_NONE */
static int octet(long x)
{
    if (x >= 0 && x < PG)
        return motif(3, (int)x);
    if (x >= PG && x < 2 * PG)
        return motif(1, (int)(x - PG));
    return -1;
}

static sigjmp_buf jb;
static volatile unsigned long f_pc, f_dar;
static void onfault(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    (void)si;
    f_pc = UC_PC(uc);
    f_dar = UC_DAR(uc);
    siglongjmp(jb, sig);
}

/* ---- l'assembleur ---- */
enum { LWZ = 32, LWZU, LBZ, LBZU, STW, LHZ = 40, LHZU, LHA, LHAU, LFS = 48, LFSU, LFD, LFDU,
       STFS = 52, STFD = 54, ADDI = 14 };
static int kn;
static void D(int op, int rt, int ra, int d)
{
    kbuf[kn++] = (u32)op << 26 | (u32)rt << 21 | (u32)ra << 16 | ((u32)d & 0xffff);
}
static void blr(void) { kbuf[kn++] = 0x4e800020; }
static void flush(void)
{
    int i;
    for (i = 0; i < kn; i += 8)
        __asm__ volatile("dcbst 0,%0" :: "r"(kbuf + i) : "memory");
    __asm__ volatile("sync");
    for (i = 0; i < kn; i += 8)
        __asm__ volatile("icbi 0,%0" :: "r"(kbuf + i) : "memory");
    __asm__ volatile("sync; isync");
}

/* les formes : opcode, taille, flottant, forme mise a jour */
typedef struct { int op, size, fp, upd; const char *nom; } Forme;
static const Forme F[] = {
    { LBZ, 1, 0, 0, "lbz" }, { LHZ, 2, 0, 0, "lhz" }, { LHA, 2, 0, 0, "lha" },
    { LWZ, 4, 0, 0, "lwz" }, { LFS, 4, 1, 0, "lfs" }, { LFD, 8, 2, 0, "lfd" },
    { LBZU, 1, 0, 1, "lbzu" }, { LHAU, 2, 0, 1, "lhau" }, { LWZU, 4, 0, 1, "lwzu" },
    { LFSU, 4, 1, 1, "lfsu" }, { LFDU, 8, 2, 1, "lfdu" },
};
#define NF (int)(sizeof(F) / sizeof(F[0]))

/* chargement de rt (r5..r12 ou f1..f8) puis rangement en out[slot] (8 octets) */
static void ld_st(const Forme *f, int r, int ra, int d, int slot)
{
    D(f->op, r, ra, d);
    if (f->fp == 1)
        D(STFS, r, 4, slot * 8);
    else if (f->fp == 2)
        D(STFD, r, 4, slot * 8);
    else
        D(STW, r, 4, slot * 8);
}

/* la valeur attendue (8 octets) d'un chargement en V + x, ou -1 : faute */
static int attendu(const Forme *f, long x, unsigned char *e)
{
    int i, b;
    long v = 0;
    memset(e, 0, 8);
    for (i = 0; i < f->size; i++) {
        b = octet(x + i);
        if (b < 0)
            return -1;
        v = (v << 8) | b;
    }
    if (f->fp) {                    /* stfs / stfd : les octets tels quels */
        for (i = 0; i < f->size; i++)
            e[i] = (unsigned char)octet(x + i);
        return 0;
    }
    if (f->op == LHA || f->op == LHAU)
        v = (short)v;
    {
        u32 w = (u32)v;
        memcpy(e, &w, 4);           /* stw, gros-boutiste comme l'invite */
    }
    return 0;
}

/*
 * Un essai : bloc deja assemble (kn mots), base r3 = V + b, nslots resultats ; insn[k] :
 * indice (dans kbuf) du chargement du resultat k, x[k] : son adresse - V, fo[k] : sa forme.
 */
static void essai(long b, int nslots, const int *insn, const long *x, const Forme **fo,
                  const char *nom)
{
    static u64 out[16];
    void (*fn)(void *, void *) = (void (*)(void *, void *))(void *)kbuf;
    unsigned char e[8];
    int k, sig, fk = -1;
    long rec[4];

    flush();
    memset(out, 0x5a, sizeof(out));
    for (k = 0; k < nslots; k++) {
        if (attendu(fo[k], x[k], e) < 0) {
            fk = k;
            break;
        }
    }
    ntest++;
    if ((sig = sigsetjmp(jb, 1)) == 0) {
        fn(V + b, out);
        if (fk >= 0) {
            nerr++;
            if (nerr < 30)
                printf("ERREUR %s b %ld : pas de faute au resultat %d (x %ld)\n", nom, b, fk,
                       x[fk]);
        }
    } else {
        nfault++;
        rec[0] = sig;
        rec[1] = (long)((u32 *)f_pc - kbuf);
        rec[2] = (long)(f_dar - (unsigned long)V);
        rec[3] = fk;
        h(rec, sizeof(rec));
        if (fk < 0 || rec[1] != insn[fk]) {
            nerr++;
            if (nerr < 30)
                printf("ERREUR %s b %ld : faute sig %d a l'instruction %ld (attendue %d, "
                       "resultat %d) dar V%+ld\n", nom, b, sig, rec[1],
                       fk >= 0 ? insn[fk] : -1, fk, rec[2]);
        }
    }
    for (k = 0; k < nslots && (fk < 0 || k < fk); k++) {
        attendu(fo[k], x[k], e);
        if (memcmp(&out[k], e, fo[k]->fp == 2 ? 8 : 4)) {
            nerr++;
            if (nerr < 30)
                printf("ERREUR %s b %ld resultat %d (%s, x %ld) : %08x%08x au lieu de "
                       "%02x%02x%02x%02x%02x%02x%02x%02x\n", nom, b, k, fo[k]->nom, x[k],
                       (u32)(out[k] >> 32), (u32)out[k], e[0], e[1], e[2], e[3], e[4], e[5],
                       e[6], e[7]);
        }
    }
    h(out, sizeof(out));
}

static const long BASES[] = { 0, 1, 2, 3, 4, 5, 7, 8, 0x7f8, 0x800, 0xff0, 0xff8, 0xff9, 0xffa,
                              0xffb, 0xffc, 0xffd, 0xffe, 0xfff, 0x1000, 0x1001, 0x1003,
                              0x1004, 0x1ff0, 0x1ff8, 0x1ffc, 0x1fff };
#define NB (int)(sizeof(BASES) / sizeof(BASES[0]))

/* deplacements d'un essai : autour des bords de page vus de la base */
static int deps(long b, int *d)
{
    int n = 0, k, e;
    for (k = -1; k <= 2; k++) {
        long bord = k * PG - (b & (PG - 1));
        for (e = -9; e <= 9; e++) {
            long v = bord + e;
            if (v >= -32768 && v <= 32767 && b + v >= -PG && b + v + 8 <= 3 * PG)
                d[n++] = (int)v;
        }
    }
    for (e = 0; e <= 8; e++)
        d[n++] = e;
    return n;
}

static void mode1(void)
{
    int bi, a, fb, i, j, n, d[160];
    for (bi = 0; bi < NB; bi++) {
        long b = BASES[bi];
        n = deps(b, d);
        for (a = 0; a < 6; a++) {               /* premier chargement : non mis a jour */
            static const int DA[] = { 0, 4, -4, 8 };
            for (i = 0; i < 4; i++) {
                int da = DA[i];
                if (octet(b + da) < 0 || octet(b + da + F[a].size - 1) < 0)
                    continue;       /* le premier ne doit pas fauter */
                for (fb = 0; fb < NF; fb++) {
                    for (j = 0; j < n; j++) {
                        int insn[3];
                        long x[3];
                        const Forme *fo[3];
                        int d2 = d[(j * 7 + 3) % n];
                        kn = 0;
                        insn[0] = kn; x[0] = b + da; fo[0] = &F[a];
                        ld_st(&F[a], 5, 3, da, 0);
                        insn[1] = kn; x[1] = b + d[j]; fo[1] = &F[fb];
                        ld_st(&F[fb], 6, 3, d[j], 1);
                        /* B' : forme lwz, sur la base telle qu'elle est (mise a jour : ea) */
                        insn[2] = kn; fo[2] = &F[3];
                        x[2] = (F[fb].upd ? b + d[j] : b) + d2;
                        if (x[2] < -PG || x[2] + 8 > 3 * PG) {
                            x[2] -= d2;     /* rester dans la zone (4 pages) */
                            d2 = 0;
                        }
                        ld_st(&F[3], 7, 3, d2, 2);
                        blr();
                        essai(b, 3, insn, x, fo, "mode1");
                    }
                }
            }
        }
    }
}

static void mode2(void)
{
    int bi, j, n, d[160], dx;
    static const int DX[] = { 0, 4, -4, 0x1000, -0x1000, 0xffc, 0x1004 };
    for (bi = 0; bi < NB; bi++) {
        long b = BASES[bi];
        if (octet(b) < 0 || octet(b + 3) < 0)
            continue;
        for (dx = 0; dx < 7; dx++) {
            long nb = b + DX[dx];
            n = deps(nb, d);
            for (j = 0; j < n; j++) {
                int insn[2];
                long x[2];
                const Forme *fo[2];
                if (nb + d[j] < -PG || nb + d[j] + 8 > 3 * PG)
                    continue;
                kn = 0;
                insn[0] = kn; x[0] = b; fo[0] = &F[3];
                ld_st(&F[3], 5, 3, 0, 0);
                D(ADDI, 3, 3, DX[dx]);
                insn[1] = kn; x[1] = nb + d[j]; fo[1] = &F[3];
                ld_st(&F[3], 6, 3, d[j], 1);
                blr();
                essai(b, 2, insn, x, fo, "mode2");
            }
        }
    }
}

/*
 * mode 4 (etiquettes) : A sur r3, puis stmw r31,120(r4) (x-lmw-inline : un branchement et
 * deux etiquettes TCG au milieu du bloc), puis B sur r3 : la base meurt a l'etiquette.
 */
static void mode4(void)
{
    int bi, j, n, d[160], fb;
    for (bi = 0; bi < NB; bi++) {
        long b = BASES[bi];
        if (octet(b) < 0 || octet(b + 3) < 0)
            continue;
        n = deps(b, d);
        for (fb = 0; fb < 6; fb++) {
            for (j = 0; j < n; j++) {
                int insn[2];
                long x[2];
                const Forme *fo[2];
                kn = 0;
                insn[0] = kn; x[0] = b; fo[0] = &F[3];
                ld_st(&F[3], 5, 3, 0, 0);
                kbuf[kn++] = 0xbfe40078;            /* stmw r31,120(r4) */
                insn[1] = kn; x[1] = b + d[j]; fo[1] = &F[fb];
                ld_st(&F[fb], 6, 3, d[j], 1);
                blr();
                essai(b, 2, insn, x, fo, "mode4");
            }
        }
    }
}

/* six bases : r3 pointe sur un tableau de six adresses (lues par lwz : sur r3 aussi) */
static void mode3(long rounds)
{
    static u32 tab[8];
    static u64 out[16];
    static const int R[6] = { 8, 9, 10, 11, 12, 3 };
    void (*fn)(void *, void *) = (void (*)(void *, void *))(void *)kbuf;
    unsigned char e[8];
    long t;
    int k, i;
    unsigned long s = 7;
    for (t = 0; t < rounds; t++) {
        long b[6];
        int dd[6][3], slot = 0, sig;
        kn = 0;
        for (k = 0; k < 6; k++) {
            s = s * 1103515245UL + 12345UL;
            b[k] = (long)((s >> 8) % (2 * PG - 16));
            for (i = 0; i < 3; i++) {
                s = s * 1103515245UL + 12345UL;
                dd[k][i] = (int)((s >> 8) % 24) - 12;
                if (b[k] + dd[k][i] < 0 || b[k] + dd[k][i] + 4 > 2 * PG)
                    dd[k][i] = 0;
            }
            tab[k] = (u32)(unsigned long)(V + b[k]);
        }
        for (k = 0; k < 5; k++)             /* r3 en dernier : il porte le tableau */
            D(LWZ, R[k], 3, 4 * k);
        D(LWZ, 3, 3, 20);
        for (i = 0; i < 3; i++)
            for (k = 0; k < 6; k++) {
                D(LWZ, 5 + (slot & 1) * 2, R[k], dd[k][i]);
                D(STW, 5 + (slot & 1) * 2, 4, 8 * (slot % 16));
                slot++;
            }
        blr();
        flush();
        memset(out, 0x5a, sizeof(out));
        ntest++;
        if ((sig = sigsetjmp(jb, 1)) != 0) {
            nerr++;
            printf("ERREUR mode3 : faute %d\n", sig);
            continue;
        }
        fn(tab, out);
        /* les 16 derniers rangements (slots 2..17 : 0 et 1 recrits) */
        for (slot = 2; slot < 18; slot++) {
            k = slot % 6; i = slot / 6;
            attendu(&F[3], b[k] + dd[k][i], e);
            if (memcmp(&out[slot % 16], e, 4)) {
                nerr++;
                if (nerr < 30)
                    printf("ERREUR mode3 tour %ld base %d i %d\n", t, k, i);
            }
        }
        h(out, sizeof(out));
    }
}

/* bancs : une structure de 8 mots lue par 8 lwz sur la meme base (hors empreinte) */
static long ms_since(struct timeval *t0)
{
    struct timeval t;
    gettimeofday(&t, 0);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_usec - t0->tv_usec) / 1000;
}

static void banc(long n)
{
    static u64 out[16];
    void (*fn)(void *, void *) = (void (*)(void *, void *))(void *)kbuf;
    struct timeval t0;
    long i;
    int k;
    kn = 0;
    for (k = 0; k < 8; k++)
        D(LWZ, 5 + k, 3, 4 * k);
    for (k = 0; k < 8; k++)
        D(LFS, 1 + k, 3, 32 + 4 * k);
    D(STW, 5, 4, 0);
    D(STFS, 1, 4, 8);
    blr();
    flush();
    gettimeofday(&t0, 0);
    for (i = 0; i < n; i++)
        fn(V + 64 + (i & 0x3f) * 8, out);
    printf("banc 8 lwz + 8 lfs sur une base, %ld appels : %ld ms\n", n, ms_since(&t0));
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    char path[] = "/tmp/tlbbtestXXXXXX";
    unsigned char *z, pg[PG];
    int fd, f, o;

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = onfault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    kbuf = mmap(0, 65536, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    fd = mkstemp(path);
    for (f = 0; f < 4; f++) {
        for (o = 0; o < PG; o++)
            pg[o] = motif(f, o);
        write(fd, pg, PG);
    }
    unlink(path);
    z = mmap(0, 4 * PG, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    V = z + PG;
    if (kbuf == MAP_FAILED || z == MAP_FAILED ||
        mmap(V, PG, PROT_READ, MAP_FIXED | MAP_SHARED, fd, 3 * PG) == MAP_FAILED ||
        mmap(V + PG, PG, PROT_READ, MAP_FIXED | MAP_SHARED, fd, 1 * PG) == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    if (argc > 2 && !strcmp(argv[1], "banc")) {
        banc(atol(argv[2]));
        return 0;
    }
    mode1();
    printf("mode1 : %ld essais, %ld fautes, %ld erreurs ; empreinte %016llx\n", ntest, nfault,
           nerr, fnv);
    mode2();
    printf("mode2 : %ld essais, %ld fautes, %ld erreurs ; empreinte %016llx\n", ntest, nfault,
           nerr, fnv);
    mode3(argc > 1 ? atol(argv[1]) : 20000);
    printf("mode3 : %ld essais, %ld fautes, %ld erreurs ; empreinte %016llx\n", ntest, nfault,
           nerr, fnv);
    mode4();
    printf("mode4 : %ld essais, %ld fautes, %ld erreurs ; empreinte %016llx\n", ntest, nfault,
           nerr, fnv);
    printf("erreurs %ld\nempreinte %016llx\n", nerr, fnv);
    (void)ndirect;
    return nerr != 0;
}
