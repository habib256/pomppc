/* fpcheck.c — test différentiel déterministe du flottant PowerPC (G4 7400).
 *
 * But : prouver que deux QEMU (ou deux réglages de CPU : `-cpu g4` contre
 * `-cpu g4,x-fast-fp=on`) calculent EXACTEMENT la même chose — mêmes bits de
 * résultat, mêmes bits de FPSCR — à FI et FR près, qui décrivent la DERNIÈRE
 * opération et ne sont pas collants.
 *
 * Sortie : une ligne de texte par section, comparable par `diff` :
 *
 *   section  n_ops  h_résultats  h_fpscr_sans(FI|FR)  h_fpscr_sans(FI|FR|FX)  h_fpscr_strict
 *
 * Les quatre colonnes de hachage permettent de localiser un écart :
 *   - h_résultats seul qui bouge          → le calcul diffère (grave) ;
 *   - h_fpscr_strict seul                 → FI/FR ou FX (toléré, voir README) ;
 *   - h_fpscr_sans(FI|FR) qui bouge mais
 *     pas h_fpscr_sans(FI|FR|FX)          → seul FX diffère (nuance connue) ;
 *   - h_fpscr_sans(FI|FR|FX) qui bouge    → un bit collant diffère (grave).
 *
 * TOUTES les opérations passent par de l'assembleur en ligne `volatile` : gcc 4.0
 * replie les constantes, fusionne mul+add en fmadd et réordonne — on ne lui
 * laisse pas le choix de l'instruction ni de l'ordre.
 *
 * Le G4 7400 n'a PAS de `fsqrt` (ni `fsqrts`, ni `fcfid`/`fctid`) : sqrt est dans
 * libm. `frsqrte` (estimation) le remplace ici.
 *
 * Usage : fpcheck [N]                 N = itérations aléatoires par section (défaut 4000)
 *         fpcheck --vidage SECTION    détaille cette section opération par opération
 *                                     (résultats et FPSCR en clair, préfixés « D »)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;
typedef unsigned int u32;

/* ------------------------------------------------------------------ FPSCR */
/* Numérotation PowerPC (bit 0 = poids fort du mot de 32 bits). */
#define B_FX     0x80000000u   /* 0  : exception cumulée (sommet) */
#define B_FEX    0x40000000u   /* 1  : dérivé */
#define B_VX     0x20000000u   /* 2  : dérivé */
#define B_OX     0x10000000u   /* 3  */
#define B_UX     0x08000000u   /* 4  */
#define B_ZX     0x04000000u   /* 5  */
#define B_XX     0x02000000u   /* 6  : inexact CUMULÉ (collant) */
#define B_VXSNAN 0x01000000u   /* 7  */
#define B_VXISI  0x00800000u   /* 8  */
#define B_VXIDI  0x00400000u   /* 9  */
#define B_VXZDZ  0x00200000u   /* 10 */
#define B_VXIMZ  0x00100000u   /* 11 */
#define B_VXVC   0x00080000u   /* 12 */
#define B_FR     0x00040000u   /* 13 : arrondi de la DERNIÈRE op (non collant) */
#define B_FI     0x00020000u   /* 14 : inexact de la DERNIÈRE op (non collant) */
#define B_FPRF   0x0001F000u   /* 15-19 */
#define B_VXSOFT 0x00000400u   /* 21 */
#define B_VXSQRT 0x00000200u   /* 22 */
#define B_VXCVI  0x00000100u   /* 23 */
#define B_VE     0x00000080u   /* 24 */
#define B_OE     0x00000040u   /* 25 */
#define B_UE     0x00000020u   /* 26 */
#define B_ZE     0x00000010u   /* 27 */
#define B_XE     0x00000008u   /* 28 */
#define B_NI     0x00000004u   /* 29 */
/* RN = bits 30-31 : 0 au plus proche, 1 vers zéro, 2 vers +inf, 3 vers -inf */

#define M_FIFR   (B_FI | B_FR)
#define M_FIFRFX (B_FI | B_FR | B_FX)

typedef union { double d; u64 u; } du_t;
typedef union { float f; u32 u; } fu_t;

static unsigned fpscr_get(void)
{
    du_t x;
    __asm__ __volatile__("mffs %0" : "=f"(x.d));
    return (unsigned)(x.u & 0xFFFFFFFFULL);
}

static void fpscr_set(unsigned v)
{
    du_t x;
    x.u = (u64)v;                       /* mtfsf ne regarde que les 32 bits bas */
    __asm__ __volatile__("mtfsf 255,%0" : : "f"(x.d));
}

/* ------------------------------------------------- instructions sous test */
/* fadd/fsub/fmul/fdiv et leurs formes « s » : frD,frA,frB */
#define OP2(mn, x, y) \
    ({ double r_; __asm__ __volatile__(mn " %0,%1,%2" : "=f"(r_) : "f"(x), "f"(y)); r_; })
/* fmadd frD,frA,frC,frB  =  frA*frC + frB : on passe (a, b, c) pour a*b+c */
#define OP3(mn, a, b, c) \
    ({ double r_; __asm__ __volatile__(mn " %0,%1,%2,%3" : "=f"(r_) : "f"(a), "f"(b), "f"(c)); r_; })
#define OP1(mn, x) \
    ({ double r_; __asm__ __volatile__(mn " %0,%1" : "=f"(r_) : "f"(x)); r_; })

/* fsel frD,frA,frC,frB : frA >= 0 ? frC : frB */
#define OPSEL(a, c, b) \
    ({ double r_; __asm__ __volatile__("fsel %0,%1,%2,%3" : "=f"(r_) : "f"(a), "f"(c), "f"(b)); r_; })

static u64 do_fctiw(double x, int trunc)
{
    du_t r;
    if (trunc) __asm__ __volatile__("fctiwz %0,%1" : "=f"(r.d) : "f"(x));
    else       __asm__ __volatile__("fctiw  %0,%1" : "=f"(r.d) : "f"(x));
    return r.u;
}

static unsigned do_fcmp(double a, double b, int ordered)
{
    unsigned cr;
    if (ordered) __asm__ __volatile__("fcmpo cr0,%1,%2\n\tmfcr %0"
                                      : "=r"(cr) : "f"(a), "f"(b) : "cr0");
    else         __asm__ __volatile__("fcmpu cr0,%1,%2\n\tmfcr %0"
                                      : "=r"(cr) : "f"(a), "f"(b) : "cr0");
    return (cr >> 28) & 0xFu;
}

/* conversions par la mémoire : stfs arrondit au simple, lfs élargit */
static u32 do_stfs(double x)
{
    fu_t f;
    __asm__ __volatile__("stfs %1,%0" : "=m"(f.f) : "f"(x));
    return f.u;
}

static double do_lfs(u32 bits)
{
    fu_t f; double d;
    f.u = bits;
    __asm__ __volatile__("lfs %0,%1" : "=f"(d) : "m"(f.f));
    return d;
}

/* les quatre façons d'effacer XX au milieu d'une rafale */
#define CLR_MTFSB0 0
#define CLR_MTFSF  1
#define CLR_MTFSFI 2
#define CLR_MCRFS  3

static unsigned clear_xx(int how)
{
    unsigned cr = 0;
    du_t z;
    switch (how) {
    case CLR_MTFSB0:                    /* efface XX (bit 6) et RIEN d'autre */
        __asm__ __volatile__("mtfsb0 6");
        break;
    case CLR_MTFSF:                     /* champ 1 (UX ZX XX VXSNAN) remis à 0 */
        z.u = 0;
        __asm__ __volatile__("mtfsf 64,%0" : : "f"(z.d));
        break;
    case CLR_MTFSFI:
        __asm__ __volatile__("mtfsfi 1,0");
        break;
    default:                            /* mcrfs copie le champ 1 dans CR0 ET l'efface */
        __asm__ __volatile__("mcrfs 0,1\n\tmfcr %0" : "=r"(cr) : : "cr0");
        cr = (cr >> 28) & 0xFu;
        break;
    }
    return cr;
}

/* --------------------------------------------------------------- hachage */
#define FNV_OFF 14695981039346656037ULL
#define FNV_PRM 1099511628211ULL

static u64 hmix(u64 h, u64 v)
{
    h ^= v;
    h *= FNV_PRM;
    h ^= h >> 29;
    return h;
}

static u64 hstr(const char *s)
{
    u64 h = FNV_OFF;
    while (*s) { h ^= (unsigned char)*s++; h *= FNV_PRM; }
    return h;
}

/* ------------------------------------------------------- générateur d'opérandes */
static u64 g_rs;

static void prng_seed(u64 s) { g_rs = s ? s : 0x9E3779B97F4A7C15ULL; }

static u64 rnd(void)
{
    g_rs ^= g_rs << 13;
    g_rs ^= g_rs >> 7;
    g_rs ^= g_rs << 17;
    return g_rs;
}

enum {
    G_BITS = 0,   /* motif 64 bits quelconque : NaN, dénormaux, infinis à foison */
    G_NORM,       /* normaux d'exposant borné */
    G_SING,       /* exactement représentable en simple (y compris NaN/inf/dénormaux) */
    G_HALF,       /* mi-chemin (et voisins) pour l'arrondi vers le simple */
    G_INT,        /* voisinage des bornes de fctiw */
    G_DENORM,     /* dénormaux double et simple */
    G_MIX,        /* mélange déterministe des précédents */
    G_SINGN       /* float32 ZÉRO OU NORMAL, tel que le rend un lfs de code « float » */
};

static double gen_op(int g)
{
    du_t x; fu_t f;
    u64 r = rnd();

    switch (g) {
    case G_BITS:
        x.u = r;
        return x.d;
    case G_NORM: {
        int e = 1023 + (int)((r >> 52) % 121u) - 60;
        x.u = ((r & 1ULL) << 63) | ((u64)e << 52) | (r & 0x000FFFFFFFFFFFFFULL);
        return x.d;
    }
    case G_SING:
        f.u = (u32)(r >> 20);
        return (double)f.f;             /* lfs : élargissement exact */
    case G_SINGN: {
        /* C'EST le cas qui arme le chemin rapide float64r32_* du patch :
         * float32 « zéro ou normal » (exposant 1..254), comme en sort un lfs.
         * Un octet sur 64 rend +0 ou -0, admis par le chemin rapide. */
        u32 e = (u32)((r >> 40) % 256u);
        if (e == 0 || e == 255) e = ((r >> 3) & 63) ? 1u + (u32)((r >> 48) % 254u) : 0u;
        f.u = (u32)((r & 1ULL) << 31) | (e << 23) | (e ? (u32)((r >> 8) & 0x7FFFFFu) : 0u);
        return (double)f.f;
    }
    case G_HALF: {
        /* mantisse simple + un demi-ulp exactement, ou juste au-dessus/dessous */
        static const u64 TAIL[4] = { 0x10000000ULL, 0x10000001ULL,
                                     0x0FFFFFFFULL, 0x18000000ULL };
        int e = 1023 + (int)((r >> 52) % 40u) - 20;
        x.u = ((r & 1ULL) << 63) | ((u64)e << 52)
            | (r & 0x000FFFFFE0000000ULL) | TAIL[(r >> 4) & 3];
        return x.d;
    }
    case G_INT: {
        static const double B[8] = { 0.0, 0.5, 1.5, 2.5, 2147483647.0,
                                     2147483648.0, -2147483648.0, -2147483649.0 };
        double b = B[(r >> 32) & 7];
        long k = (long)(r & 0xFFFFu) - 32768L;
        return b + (double)k / 256.0;   /* hors section mesurée : pas de FPSCR à protéger */
    }
    case G_DENORM:
        if (r & 1) {                    /* dénormal double */
            x.u = ((r >> 1) & 0x8000000000000000ULL) | ((r >> 8) & 0x000FFFFFFFFFFFFFULL);
            if ((x.u & 0x000FFFFFFFFFFFFFULL) == 0) x.u |= 1;
            return x.d;
        }
        f.u = (u32)((r >> 24) & 0x807FFFFFu);   /* dénormal simple */
        if ((f.u & 0x007FFFFFu) == 0) f.u |= 1;
        return (double)f.f;
    default: {
        int k = (int)(r % 6u);
        return gen_op(k);
    }
    }
}

/* ---------------------------------------------------------- cas limites */
static const u64 EDGE[] = {
    0x0000000000000000ULL, /* +0 */
    0x8000000000000000ULL, /* -0 */
    0x0000000000000001ULL, /* plus petit dénormal double */
    0x800FFFFFFFFFFFFFULL, /* -plus grand dénormal double */
    0x0010000000000000ULL, /* DBL_MIN */
    0x0010000000000001ULL, /* DBL_MIN + 1 ulp */
    0x7FEFFFFFFFFFFFFFULL, /* DBL_MAX */
    0xFFEFFFFFFFFFFFFFULL, /* -DBL_MAX */
    0x3FF0000000000000ULL, /* 1.0 */
    0xBFF0000000000000ULL, /* -1.0 */
    0x3FE0000000000000ULL, /* 0.5 */
    0x4000000000000000ULL, /* 2.0 */
    0x4008000000000000ULL, /* 3.0 */
    0x3FF0000000000001ULL, /* 1.0 + 1 ulp double */
    0x7FF0000000000000ULL, /* +inf */
    0xFFF0000000000000ULL, /* -inf */
    0x7FF8000000000000ULL, /* QNaN par défaut */
    0xFFF8000000000000ULL, /* -QNaN */
    0x7FFDEADBEEF12345ULL, /* QNaN à charge utile */
    0x7FF0000000000001ULL, /* SNaN (charge utile minimale) */
    0x7FF4000000000000ULL, /* SNaN */
    0xFFF0000000000001ULL, /* -SNaN */
    0x3810000000000000ULL, /* FLT_MIN en double (2^-126) */
    0x380FFFFFFFFFFFFFULL, /* juste sous FLT_MIN : soupassement du simple */
    0x36A0000000000000ULL, /* 2^-149 : plus petit dénormal simple */
    0x3690000000000000ULL, /* 2^-150 : mi-chemin vers zéro en simple */
    0x47EFFFFFE0000000ULL, /* FLT_MAX en double */
    0x47EFFFFFF0000000ULL, /* mi-chemin FLT_MAX/inf : débordement du simple */
    0x47F0000000000000ULL, /* 2^128 : débordement du simple */
    0x3FF0000010000000ULL, /* 1 + 2^-24 : mi-chemin simple exact (pair) */
    0x3FF0000010000001ULL, /* juste au-dessus du mi-chemin */
    0x3FF000000FFFFFFFULL, /* juste au-dessous */
    0x3FF0000030000000ULL, /* 1 + 3*2^-24 : mi-chemin (impair) */
    0x41DFFFFFFFC00000ULL, /* 2147483647.0 */
    0x41E0000000000000ULL, /* 2147483648.0 */
    0xC1E0000000000000ULL, /* -2147483648.0 */
    0xC1E0000000100000ULL, /* -2147483649.0 */
    0x3FE0000000000000ULL, /* 0.5 (bis, pour fctiw) */
    0x3FF8000000000000ULL, /* 1.5 */
    0x4004000000000000ULL, /* 2.5 */
    0xBFF8000000000000ULL, /* -1.5 */
    0x7FE0000000000000ULL, /* 2^1023 */
    0x0020000000000000ULL, /* 2^-1021 */
    0x4330000000000000ULL, /* 2^52 */
    0x433FFFFFFFFFFFFFULL, /* 2^53 - 1 ulp */
    0x7E37E43C8800759CULL  /* 1e300 */
};
#define NEDGE ((int)(sizeof(EDGE) / sizeof(EDGE[0])))

/* ------------------------------------------------------------- sections */
static const char *g_name;
static unsigned long g_ops;
static u64 g_hres, g_hm1, g_hm2, g_hs;

/* Vidage d'UNE section (`--vidage NOM`) : chaque résultat et chaque lecture du
 * FPSCR sort en clair, préfixée « D ». Sert à isoler la PREMIÈRE opération
 * fautive quand un hachage diffère entre deux modes — un `diff` des deux
 * vidages donne le rang, et les lignes OPER donnent les opérandes. */
static const char *g_dump;

static int dumping(void) { return g_dump && g_name && strcmp(g_dump, g_name) == 0; }

static void sect_begin(const char *name)
{
    g_name = name;
    g_ops = 0;
    g_hres = g_hm1 = g_hm2 = g_hs = FNV_OFF;
    prng_seed(hstr(name));
}

static void sect_end(void)
{
    printf("%-26s %9lu %016llx %016llx %016llx %016llx\n",
           g_name, g_ops, g_hres, g_hm1, g_hm2, g_hs);
    fflush(stdout);
}

static void rec_u64(u64 v)
{
    if (dumping()) printf("D %-24s %7lu R %016llx\n", g_name, g_ops, v);
    g_hres = hmix(g_hres, v);
    g_ops++;
}

static void rec_u32(u32 v) { rec_u64((u64)v); }
static void rec_d(double v) { du_t x; x.d = v; rec_u64(x.u); }

/* le FPSCR courant, haché dans les trois colonnes (masquée, masquée+FX, stricte) */
static void rec_fp(void)
{
    unsigned f = fpscr_get();
    if (dumping()) printf("D %-24s %7lu F %08x\n", g_name, g_ops, f);
    g_hm1 = hmix(g_hm1, (u64)(f & ~M_FIFR));
    g_hm2 = hmix(g_hm2, (u64)(f & ~M_FIFRFX));
    g_hs  = hmix(g_hs,  (u64)f);
}

/* --------------------------------------------------- tampons d'opérandes */
#define NMAX 8192
static int NR = 4000;
static double OA[NMAX], OB[NMAX], OC[NMAX];

/* Les opérandes sont fabriqués AVANT la boucle mesurée : le générateur fait des
 * lfs et des conversions qui n'ont rien à faire entre deux instructions dont on
 * observe le FPSCR. */
static void fill(int gen)
{
    int i;
    for (i = 0; i < NR; i++) {
        OA[i] = gen_op(gen);
        OB[i] = gen_op(gen);
        OC[i] = gen_op(gen);
    }
    if (dumping()) {
        du_t a, b, c;
        int n = NR < 24 ? NR : 24;
        for (i = 0; i < n; i++) {
            a.d = OA[i]; b.d = OB[i]; c.d = OC[i];
            printf("D %-24s OPER %4d %016llx %016llx %016llx\n",
                   g_name, i, a.u, b.u, c.u);
        }
    }
}

enum { BURST = 0, PEROP = 1 };    /* mffs après chaque op, ou en fin de rafale */
enum { KEEP = 0, RESET = 1 };     /* FPSCR remis à `base` à chaque itération ? */

/* fadd/fsub/fmul/fdiv, et leurs formes « s » */
static void run_bin(const char *name, unsigned base, int gen, int perop, int reset, int s)
{
    int i;
    sect_begin(name);
    fill(gen);
    fpscr_set(base);
    for (i = 0; i < NR; i++) {
        double a = OA[i], b = OB[i];
        if (reset) fpscr_set(base);
        if (s) {
            rec_d(OP2("fadds", a, b)); if (perop) rec_fp();
            rec_d(OP2("fsubs", a, b)); if (perop) rec_fp();
            rec_d(OP2("fmuls", a, b)); if (perop) rec_fp();
            rec_d(OP2("fdivs", a, b)); if (perop) rec_fp();
        } else {
            rec_d(OP2("fadd", a, b)); if (perop) rec_fp();
            rec_d(OP2("fsub", a, b)); if (perop) rec_fp();
            rec_d(OP2("fmul", a, b)); if (perop) rec_fp();
            rec_d(OP2("fdiv", a, b)); if (perop) rec_fp();
        }
        if (!perop && (i & 63) == 63) rec_fp();
    }
    rec_fp();
    sect_end();
}

/* fmadd/fmsub/fnmadd/fnmsub, et leurs formes « s » */
static void run_fma(const char *name, unsigned base, int gen, int perop, int reset, int s)
{
    int i;
    sect_begin(name);
    fill(gen);
    fpscr_set(base);
    for (i = 0; i < NR; i++) {
        double a = OA[i], b = OB[i], c = OC[i];
        if (reset) fpscr_set(base);
        if (s) {
            rec_d(OP3("fmadds",   a, b, c)); if (perop) rec_fp();
            rec_d(OP3("fmsubs",   a, b, c)); if (perop) rec_fp();
            rec_d(OP3("fnmadds",  a, b, c)); if (perop) rec_fp();
            rec_d(OP3("fnmsubs",  a, b, c)); if (perop) rec_fp();
        } else {
            rec_d(OP3("fmadd",   a, b, c)); if (perop) rec_fp();
            rec_d(OP3("fmsub",   a, b, c)); if (perop) rec_fp();
            rec_d(OP3("fnmadd",  a, b, c)); if (perop) rec_fp();
            rec_d(OP3("fnmsub",  a, b, c)); if (perop) rec_fp();
        }
        if (!perop && (i & 63) == 63) rec_fp();
    }
    rec_fp();
    sect_end();
}

/* fres, frsqrte : ESTIMATIONS, précision définie par l'implémentation. */
static void run_est(const char *name, unsigned base, int gen, int reset)
{
    int i;
    sect_begin(name);
    fill(gen);
    fpscr_set(base);
    for (i = 0; i < NR; i++) {
        if (reset) fpscr_set(base);
        rec_d(OP1("fres",    OA[i])); rec_fp();
        rec_d(OP1("frsqrte", OB[i])); rec_fp();
    }
    rec_fp();
    sect_end();
}

/* frsp : arrondi du double vers le simple */
static void run_frsp(const char *name, unsigned base, int gen, int reset)
{
    int i;
    sect_begin(name);
    fill(gen);
    fpscr_set(base);
    for (i = 0; i < NR; i++) {
        if (reset) fpscr_set(base);
        rec_d(OP1("frsp", OA[i])); rec_fp();
        rec_d(OP1("frsp", OB[i])); rec_fp();
        rec_d(OP1("frsp", OC[i])); rec_fp();
    }
    rec_fp();
    sect_end();
}

/* fctiw / fctiwz. `full` : hache les 64 bits du FPR (les 32 hauts sont
 * ARCHITECTURALEMENT INDÉFINIS — section informative). */
static void run_fctiw(const char *name, unsigned base, int gen, int reset, int full)
{
    int i;
    sect_begin(name);
    fill(gen);
    fpscr_set(base);
    for (i = 0; i < NR; i++) {
        u64 r;
        if (reset) fpscr_set(base);
        r = do_fctiw(OA[i], 0); if (full) rec_u64(r); else rec_u32((u32)r); rec_fp();
        r = do_fctiw(OA[i], 1); if (full) rec_u64(r); else rec_u32((u32)r); rec_fp();
        r = do_fctiw(OB[i], 0); if (full) rec_u64(r); else rec_u32((u32)r); rec_fp();
        r = do_fctiw(OB[i], 1); if (full) rec_u64(r); else rec_u32((u32)r); rec_fp();
    }
    rec_fp();
    sect_end();
}

/* fcmpu / fcmpo : champ CR + FPSCR (FPRF, VXSNAN, VXVC) */
static void run_fcmp(const char *name, unsigned base, int gen, int reset)
{
    int i;
    sect_begin(name);
    fill(gen);
    fpscr_set(base);
    for (i = 0; i < NR; i++) {
        if (reset) fpscr_set(base);
        rec_u32(do_fcmp(OA[i], OB[i], 0)); rec_fp();
        rec_u32(do_fcmp(OA[i], OB[i], 1)); rec_fp();
        rec_u32(do_fcmp(OB[i], OA[i], 0)); rec_fp();
        rec_u32(do_fcmp(OC[i], OC[i], 1)); rec_fp();
    }
    rec_fp();
    sect_end();
}

/* fsel, fabs, fneg, fnabs, fmr : ne touchent pas le FPSCR (c'est aussi ce qu'on vérifie) */
static void run_nonarith(const char *name, unsigned base, int gen)
{
    int i;
    sect_begin(name);
    fill(gen);
    fpscr_set(base);
    for (i = 0; i < NR; i++) {
        rec_d(OPSEL(OA[i], OB[i], OC[i]));
        rec_d(OP1("fabs",  OA[i]));
        rec_d(OP1("fneg",  OB[i]));
        rec_d(OP1("fnabs", OC[i]));
        rec_d(OP1("fmr",   OA[i]));
        rec_fp();
    }
    rec_fp();
    sect_end();
}

/* conversions par la mémoire : stfs (double → simple) puis lfs (simple → double) */
static void run_lfsstfs(const char *name, unsigned base, int gen)
{
    int i;
    sect_begin(name);
    fill(gen);
    fpscr_set(base);
    for (i = 0; i < NR; i++) {
        u32 s = do_stfs(OA[i]);
        rec_u32(s); rec_fp();
        rec_d(do_lfs(s)); rec_fp();
        s = do_stfs(OB[i]);
        rec_u32(s); rec_fp();
        rec_d(do_lfs(s)); rec_fp();
    }
    rec_fp();
    sect_end();
}

/* Cas limites : produit croisé complet de la table EDGE. */
static void run_edge_bin(const char *name, unsigned base)
{
    int i, j;
    du_t a, b;
    sect_begin(name);
    for (i = 0; i < NEDGE; i++) {
        a.u = EDGE[i];
        for (j = 0; j < NEDGE; j++) {
            b.u = EDGE[j];
            fpscr_set(base);
            rec_d(OP2("fadd",  a.d, b.d)); rec_fp();
            rec_d(OP2("fsub",  a.d, b.d)); rec_fp();
            rec_d(OP2("fmul",  a.d, b.d)); rec_fp();
            rec_d(OP2("fdiv",  a.d, b.d)); rec_fp();
            rec_d(OP2("fadds", a.d, b.d)); rec_fp();
            rec_d(OP2("fmuls", a.d, b.d)); rec_fp();
            rec_d(OP2("fdivs", a.d, b.d)); rec_fp();
        }
    }
    sect_end();
}

static void run_edge_fma(const char *name, unsigned base)
{
    int i, j;
    du_t a, b, c;
    sect_begin(name);
    for (i = 0; i < NEDGE; i++) {
        a.u = EDGE[i];
        for (j = 0; j < NEDGE; j++) {
            b.u = EDGE[j];
            c.u = EDGE[(i + j) % NEDGE];
            fpscr_set(base);
            rec_d(OP3("fmadd",   a.d, b.d, c.d)); rec_fp();
            rec_d(OP3("fmsub",   a.d, b.d, c.d)); rec_fp();
            rec_d(OP3("fnmadd",  a.d, b.d, c.d)); rec_fp();
            rec_d(OP3("fnmsub",  a.d, b.d, c.d)); rec_fp();
            rec_d(OP3("fmadds",  a.d, b.d, c.d)); rec_fp();
            rec_d(OP3("fnmsubs", a.d, b.d, c.d)); rec_fp();
        }
    }
    sect_end();
}

static void run_edge_un(const char *name, unsigned base)
{
    int i;
    du_t a;
    sect_begin(name);
    for (i = 0; i < NEDGE; i++) {
        a.u = EDGE[i];
        fpscr_set(base);
        rec_d(OP1("frsp",    a.d));       rec_fp();
        rec_u32((u32)do_fctiw(a.d, 0));   rec_fp();
        rec_u32((u32)do_fctiw(a.d, 1));   rec_fp();
        rec_d(OP1("fres",    a.d));       rec_fp();
        rec_d(OP1("frsqrte", a.d));       rec_fp();
        rec_u32(do_stfs(a.d));            rec_fp();
    }
    sect_end();
}

static void run_edge_cmp(const char *name, unsigned base)
{
    int i, j;
    du_t a, b;
    sect_begin(name);
    for (i = 0; i < NEDGE; i++) {
        a.u = EDGE[i];
        for (j = 0; j < NEDGE; j++) {
            b.u = EDGE[j];
            fpscr_set(base);
            rec_u32(do_fcmp(a.d, b.d, 0)); rec_fp();
            rec_u32(do_fcmp(a.d, b.d, 1)); rec_fp();
            rec_d(OPSEL(a.d, b.d, a.d));   rec_fp();
        }
    }
    sect_end();
}

/* XX effacé AU MILIEU d'une rafale, par les quatre instructions possibles. */
static void run_clear_xx(const char *name, int how)
{
    int i;
    sect_begin(name);
    fill(G_MIX);
    fpscr_set(0);                        /* RN=0, tout propre */
    for (i = 0; i < NR; i++) {
        double a = OA[i], b = OB[i];
        rec_d(OP2("fadd", a, b)); rec_fp();
        rec_d(OP2("fmul", a, b)); rec_fp();
        if ((i & 3) == 0) {
            rec_u32(clear_xx(how));      /* CR pour mcrfs, 0 sinon */
            rec_fp();                    /* XX doit être retombé à 0 */
        }
        rec_d(OP2("fdiv", a, b)); rec_fp();
        rec_d(OP3("fmadd", a, b, OC[i])); rec_fp();
    }
    rec_fp();
    sect_end();
}

/* « FX effacé seul » : XX reste à 1, FX est remis à 0 par mtfsb0 0.
 * Le QEMU d'aujourd'hui repose FX à CHAQUE opération inexacte ; le vrai
 * matériel (et le mode rapide) ne le pose que sur la transition 0→1 de XX.
 * Section ISOLÉE : un écart ici ne contamine aucune autre ligne. */
static void run_fx_alone(const char *name)
{
    int i;
    sect_begin(name);
    fill(G_NORM);
    fpscr_set(0);
    for (i = 0; i < NR; i++) {
        double a = OA[i], b = OB[i];
        rec_d(OP2("fadd", a, b)); rec_fp();
        __asm__ __volatile__("mtfsb0 0");      /* FX ← 0, XX intact */
        rec_fp();
        rec_d(OP2("fmul", a, b)); rec_fp();    /* FX reposé ou non ? */
        rec_d(OP2("fdiv", a, b)); rec_fp();
        __asm__ __volatile__("mtfsb0 0");
        rec_fp();
    }
    rec_fp();
    sect_end();
}

/* ---- sections taillées pour le mode « flottant rapide » (docs/flottant-rapide.md) ----
 * L'amorçage du patch ne s'arme que si (FPSCR & (XX|XE|OE|UE)) == XX ET que
 * l'arrondi est au plus proche ; le chemin rapide float64r32_* exige en plus
 * des opérandes exactement float32 « zéro ou normal ». Sans ces sections, le
 * test ne prouverait rien du mode rapide. */

/* Transition XX 0→1 : une suite DÉLIBÉRÉMENT exacte (puissances de deux), puis
 * la première opération inexacte. XX (et FX) doivent basculer au MÊME rang dans
 * les deux modes — c'est tout l'argument d'exactitude de l'amorçage. */
static void run_transition(const char *name)
{
    static const double EX[8] = { 1.0, 2.0, 4.0, 0.5, 3.0, 8.0, 0.25, 16.0 };
    int i, k;
    sect_begin(name);
    for (i = 0; i < NR; i++) {
        double a = EX[i & 7], b = EX[(i >> 3) & 7], q;
        fpscr_set(0);                       /* XX = 0 : tout doit rester en logiciel */
        rec_fp();
        for (k = 0; k < 4; k++) {           /* exactes : XX doit rester à 0 */
            rec_d(OP2("fadd", a, b)); rec_fp();
            rec_d(OP2("fmul", a, b)); rec_fp();
            rec_d(OP2("fsub", a, b)); rec_fp();
            rec_d(OP2("fdiv", a, b)); rec_fp();
            rec_d(OP2("fadds", a, b)); rec_fp();
            rec_d(OP3("fmadd", a, b, a)); rec_fp();
        }
        q = OP2("fdiv", a, EX[4]);          /* x/3 : PREMIÈRE inexacte, XX 0→1 ici */
        rec_d(q); rec_fp();
        rec_d(OP2("fmul", q, q));  rec_fp();   /* à partir d'ici l'amorçage est armé */
        rec_d(OP2("fadd", q, a));  rec_fp();
        rec_d(OP2("fdivs", a, EX[4])); rec_fp();
        rec_d(OP2("fadd", a, b));  rec_fp();   /* exacte, mais XX reste à 1 */
    }
    rec_fp();
    sect_end();
}

/* mtfsb0 XX en cours de route → retour au logiciel exact ; mtfsb1 XX → ré-armé.
 * Vérifie le « ça se désarme et se ré-arme tout seul » du § 2.3 du patch. */
static void run_bascule(const char *name)
{
    int i;
    sect_begin(name);
    fill(G_SINGN);
    fpscr_set(0);
    for (i = 0; i < NR; i++) {
        double a = OA[i], b = OB[i], c = OC[i];
        __asm__ __volatile__("mtfsb1 6"); g_ops++; rec_fp();   /* XX ← 1 : armé */
        rec_d(OP2("fdivs", a, b)); rec_fp();
        rec_d(OP2("fmuls", a, b)); rec_fp();
        __asm__ __volatile__("mtfsb0 6"); g_ops++; rec_fp();   /* XX ← 0 : désarmé */
        rec_d(OP2("fdivs", a, b)); rec_fp();                   /* exact de nouveau */
        rec_d(OP2("fadds", a, b)); rec_fp();
        __asm__ __volatile__("mtfsb1 6"); g_ops++; rec_fp();   /* ré-armé */
        rec_d(OP3("fmadds", a, b, c)); rec_fp();
        rec_d(OP3("fmadd",  a, b, c)); rec_fp();
        __asm__ __volatile__("mtfsb0 6"); g_ops++;
        __asm__ __volatile__("mtfsb0 0"); g_ops++;             /* XX et FX à 0 */
        rec_fp();
    }
    rec_fp();
    sect_end();
}

/* Débordement (OX), soupassement (UX), résultats dénormaux : le chemin rapide
 * doit repartir au logiciel dès que |résultat| <= FLT_MIN ou qu'il déborde. */
static const u64 EXTR[] = {
    0x47EFFFFFE0000000ULL, /* FLT_MAX */
    0xC7EFFFFFE0000000ULL, /* -FLT_MAX */
    0x3810000000000000ULL, /* FLT_MIN (2^-126) */
    0x380FFFFFE0000000ULL, /* plus grand dénormal simple */
    0x36A0000000000000ULL, /* 2^-149 : plus petit dénormal simple */
    0x7FEFFFFFFFFFFFFFULL, /* DBL_MAX */
    0x0010000000000000ULL, /* DBL_MIN */
    0x0000000000000001ULL, /* plus petit dénormal double */
    0x41E0000000000000ULL, /* 2^31 */
    0x3E30000000000000ULL, /* 2^-28 */
    0x4090000000000000ULL, /* 1024 */
    0x3F50000000000000ULL, /* 2^-10 */
    0x3FF0000000000000ULL, /* 1.0 */
    0x3FF0000010000000ULL, /* 1 + 2^-24 (non représentable en simple) */
    0xBFF8000000000000ULL, /* -1.5 */
    0x4059000000000000ULL  /* 100.0 */
};
#define NEXTR ((int)(sizeof(EXTR) / sizeof(EXTR[0])))

static void run_ox_ux(const char *name, unsigned base)
{
    int i, j;
    du_t a, b;
    sect_begin(name);
    for (i = 0; i < NEXTR; i++) {
        a.u = EXTR[i];
        for (j = 0; j < NEXTR; j++) {
            b.u = EXTR[j];
            fpscr_set(base);
            rec_d(OP2("fmuls", a.d, b.d)); rec_fp();   /* FLT_MAX*FLT_MAX → OX */
            rec_d(OP2("fdivs", a.d, b.d)); rec_fp();   /* FLT_MIN/2^30 → UX */
            rec_d(OP2("fadds", a.d, b.d)); rec_fp();
            rec_d(OP2("fsubs", a.d, b.d)); rec_fp();
            rec_d(OP2("fmul",  a.d, b.d)); rec_fp();
            rec_d(OP2("fdiv",  a.d, b.d)); rec_fp();
            rec_d(OP3("fmadds", a.d, b.d, a.d)); rec_fp();
            rec_d(OP3("fmadd",  a.d, b.d, b.d)); rec_fp();
            rec_d(OP1("frsp",  a.d)); rec_fp();
        }
    }
    sect_end();
}

/* Rafale longue sans lecture : mffs seulement à la fin (le cas des jeux). */
static void run_rafale(const char *name, unsigned base)
{
    int i, k;
    sect_begin(name);
    fill(G_NORM);
    fpscr_set(base);
    for (i = 0; i < NR; i++) {
        double a = OA[i], b = OB[i], c = OC[i];
        for (k = 0; k < 4; k++) {
            rec_d(OP2("fadd", a, b));
            rec_d(OP2("fmul", b, c));
            rec_d(OP3("fmadd", a, b, c));
            rec_d(OP2("fdivs", a, c));
            rec_d(OP1("frsp", a));
        }
    }
    rec_fp();                            /* UNE seule lecture, à la toute fin */
    sect_end();
}

/* mtfsb1/mtfsb0/mtfsfi/mtfsf/mcrfs manipulés pour eux-mêmes, FPSCR lu à chaque pas. */
static void run_fpscr_ops(const char *name)
{
    int i;
    du_t z;
    sect_begin(name);
    fill(G_MIX);
    fpscr_set(0);
    for (i = 0; i < NR; i++) {
        unsigned cr;
        rec_d(OP2("fadd", OA[i], OB[i])); rec_fp();
        __asm__ __volatile__("mtfsb1 6"); rec_fp(); g_ops++;   /* XX ← 1 */
        __asm__ __volatile__("mtfsb0 6"); rec_fp(); g_ops++;   /* XX ← 0 */
        __asm__ __volatile__("mtfsb1 29"); rec_fp(); g_ops++;  /* NI ← 1 */
        __asm__ __volatile__("mtfsb0 29"); rec_fp(); g_ops++;
        __asm__ __volatile__("mtfsfi 7,0"); rec_fp(); g_ops++;   /* champ 7 : XE NI RN ← 0 */
        z.u = (u64)(0x02000000u | (unsigned)(i & 3));          /* XX + RN */
        __asm__ __volatile__("mtfsf 255,%0" : : "f"(z.d)); rec_fp(); g_ops++;
        __asm__ __volatile__("mcrfs 0,0\n\tmfcr %0" : "=r"(cr) : : "cr0");
        rec_u32((cr >> 28) & 0xFu); rec_fp();
        __asm__ __volatile__("mcrfs 1,1\n\tmfcr %0" : "=r"(cr) : : "cr1");
        rec_u32((cr >> 24) & 0xFu); rec_fp();
        rec_d(OP2("fdiv", OA[i], OC[i])); rec_fp();
    }
    rec_fp();
    sect_end();
}

/* --------------------------------------------------------------- AltiVec */
#ifdef __ALTIVEC__
#ifndef __APPLE_ALTIVEC__
#include <altivec.h>
#endif
#define HAVE_AV 1

typedef vector unsigned int vu_t;
typedef union { vu_t v; u32 w[4]; } uv_t;

#define VSCR_NJ 0x00010000u             /* 1 = mode non-Java (dénormaux aplatis) */
#define VSCR_SAT 0x00000001u

#define V2(mn, a, b) \
    ({ vu_t r_; __asm__ __volatile__(mn " %0,%1,%2" : "=v"(r_) : "v"(a), "v"(b)); r_; })
/* vmaddfp vD,vA,vC,vB = vA*vC + vB */
#define V3(mn, a, c, b) \
    ({ vu_t r_; __asm__ __volatile__(mn " %0,%1,%2,%3" : "=v"(r_) : "v"(a), "v"(c), "v"(b)); r_; })
#define V1(mn, a) \
    ({ vu_t r_; __asm__ __volatile__(mn " %0,%1" : "=v"(r_) : "v"(a)); r_; })
#define VC(mn, a, imm) \
    ({ vu_t r_; __asm__ __volatile__(mn " %0,%1," #imm : "=v"(r_) : "v"(a)); r_; })

static u32 vscr_get(void)
{
    uv_t u;
    __asm__ __volatile__("mfvscr %0" : "=v"(u.v));
    return u.w[3];
}

static void vscr_set(u32 w)
{
    uv_t u;
    u.w[0] = u.w[1] = u.w[2] = 0; u.w[3] = w;
    __asm__ __volatile__("mtvscr %0" : : "v"(u.v));
}

static void rec_v(vu_t v)
{
    uv_t u; int k;
    u.v = v;
    for (k = 0; k < 4; k++) g_hres = hmix(g_hres, (u64)u.w[k]);
    g_ops++;
}

/* VSCR haché dans la colonne « résultats » (les ops vectorielles ne touchent
 * pas le FPSCR : les colonnes FPSCR doivent rester figées, c'est le test). */
static void rec_vscr(void) { g_hres = hmix(g_hres, (u64)vscr_get()); }

static vu_t mkvec(int gen)
{
    uv_t u; int k;
    for (k = 0; k < 4; k++) {
        u64 r = rnd();
        switch (gen) {
        case G_BITS:   u.w[k] = (u32)(r >> 17); break;
        case G_DENORM: u.w[k] = (u32)((r >> 13) & 0x807FFFFFu) | 1u; break;
        default: {     /* normaux simples d'exposant borné */
            u32 e = 127u + (u32)((r >> 40) % 41u) - 20u;
            u.w[k] = (u32)((r & 1ULL) << 31) | (e << 23) | (u32)((r >> 24) & 0x7FFFFFu);
            break; }
        }
    }
    return u.v;
}

static void run_av(const char *name, u32 vscr, int gen)
{
    int i;
    sect_begin(name);
    fpscr_set(0);
    for (i = 0; i < NR; i++) {
        vu_t a = mkvec(gen), b = mkvec(gen), c = mkvec(gen);
        vscr_set(vscr);
        rec_v(V2("vaddfp",    a, b)); rec_vscr();
        rec_v(V2("vsubfp",    a, b)); rec_vscr();
        rec_v(V3("vmaddfp",   a, b, c)); rec_vscr();
        rec_v(V3("vnmsubfp",  a, b, c)); rec_vscr();
        rec_v(V1("vrefp",     a));    rec_vscr();
        rec_v(V1("vrsqrtefp", b));    rec_vscr();
        rec_v(V2("vmaxfp",    a, b)); rec_vscr();
        rec_v(V2("vminfp",    a, b)); rec_vscr();
        rec_v(V2("vcmpeqfp",  a, b)); rec_vscr();
        rec_v(V2("vcmpgtfp",  a, b)); rec_vscr();
        rec_v(V2("vcmpgefp",  a, b)); rec_vscr();
        rec_v(V2("vcmpbfp",   a, b)); rec_vscr();
        rec_v(VC("vcfsx",  a, 0));  rec_vscr();
        rec_v(VC("vcfsx",  a, 8));  rec_vscr();
        rec_v(VC("vcfux",  b, 1));  rec_vscr();
        rec_v(VC("vcfux",  b, 23)); rec_vscr();
        rec_v(VC("vctsxs", a, 0));  rec_vscr();
        rec_v(VC("vctsxs", a, 4));  rec_vscr();
        rec_v(VC("vctuxs", b, 0));  rec_vscr();
        rec_v(VC("vctuxs", b, 31)); rec_vscr();
        rec_fp();                        /* doit rester immobile */
    }
    rec_fp();
    sect_end();
}
#else
#define HAVE_AV 0
#endif

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv)
{
    unsigned f0;

    {
        int i;
        for (i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--vidage") && i + 1 < argc) {
                g_dump = argv[++i];
                NR = 6;                 /* un vidage doit rester lisible */
            } else {
                NR = atoi(argv[i]);
                if (NR < 1) NR = 1;
                if (NR > NMAX) NR = NMAX;
            }
        }
    }
    f0 = fpscr_get();

    printf("# fpcheck v1 — flottant PowerPC, test différentiel déterministe\n");
    printf("# iterations_par_section=%d cas_limites=%d extremes=%d altivec=%d%s%s\n",
           NR, NEDGE, NEXTR, HAVE_AV, g_dump ? " vidage=" : "", g_dump ? g_dump : "");
    printf("# fpscr_a_l_entree=%08x\n", f0);
    printf("# colonnes: section n_ops h_res h_fpscr_sans_FI_FR h_fpscr_sans_FI_FR_FX h_fpscr_strict\n");
    printf("# les sections en -info portent sur des bits ARCHITECTURALEMENT INDEFINIS\n");

    /* --- scalaire double : les quatre modes d'arrondi, XX = 0 au départ --- */
    run_bin("d-bin-rn0-bits",   0, G_BITS, PEROP, RESET, 0);
    run_bin("d-bin-rn1-bits",   1, G_BITS, PEROP, RESET, 0);
    run_bin("d-bin-rn2-bits",   2, G_BITS, PEROP, RESET, 0);
    run_bin("d-bin-rn3-bits",   3, G_BITS, PEROP, RESET, 0);
    run_bin("d-bin-rn0-norm",   0, G_NORM, PEROP, RESET, 0);
    run_bin("d-bin-rn0-sing",   0, G_SING, PEROP, RESET, 0);
    run_bin("d-bin-rn0-half",   0, G_HALF, PEROP, RESET, 0);
    run_bin("d-bin-rn0-denorm", 0, G_DENORM, PEROP, RESET, 0);

    /* --- XX déjà posé : c'est LÀ que le mode rapide doit s'engager --- */
    run_bin("d-bin-xx1-rn0",    B_XX | 0, G_MIX, PEROP, RESET, 0);
    run_bin("d-bin-xx1-rn1",    B_XX | 1, G_MIX, PEROP, RESET, 0);
    run_bin("d-bin-xx1-rn2",    B_XX | 2, G_MIX, PEROP, RESET, 0);
    run_bin("d-bin-xx1-rn3",    B_XX | 3, G_MIX, PEROP, RESET, 0);
    run_bin("d-bin-xx1-norm",   B_XX | 0, G_NORM, PEROP, KEEP, 0);
    run_bin("d-bin-xx1-denorm", B_XX | 0, G_DENORM, PEROP, KEEP, 0);
    /* XE = 1 : le mode rapide doit se désengager (aucune trappe : MSR[FE0/FE1]=0) */
    run_bin("d-bin-xx1-xe1",    B_XX | B_XE, G_MIX, PEROP, RESET, 0);
    run_bin("d-bin-xe1-xx0",    B_XE, G_MIX, PEROP, RESET, 0);
    run_bin("d-bin-ni1-rn0",    B_NI, G_DENORM, PEROP, RESET, 0);

    /* --- scalaire simple --- */
    run_bin("s-bin-rn0-sing",   0, G_SING, PEROP, RESET, 1);
    run_bin("s-bin-rn1-sing",   1, G_SING, PEROP, RESET, 1);
    run_bin("s-bin-rn2-sing",   2, G_SING, PEROP, RESET, 1);
    run_bin("s-bin-rn3-sing",   3, G_SING, PEROP, RESET, 1);
    run_bin("s-bin-xx1-sing",   B_XX, G_SING, PEROP, RESET, 1);
    run_bin("s-bin-xx1-xe1",    B_XX | B_XE, G_SING, PEROP, RESET, 1);
    /* opérandes non représentables en simple : résultat « indéfini » (PEM) */
    run_bin("s-bin-rn0-bits-info",   0, G_BITS, PEROP, RESET, 1);
    run_bin("s-bin-rn0-denorm-info", 0, G_DENORM, PEROP, RESET, 1);

    /* --- multiplication-accumulation --- */
    run_fma("d-fma-rn0-bits",   0, G_BITS, PEROP, RESET, 0);
    run_fma("d-fma-rn1-bits",   1, G_BITS, PEROP, RESET, 0);
    run_fma("d-fma-rn2-bits",   2, G_BITS, PEROP, RESET, 0);
    run_fma("d-fma-rn3-bits",   3, G_BITS, PEROP, RESET, 0);
    run_fma("d-fma-rn0-norm",   0, G_NORM, PEROP, RESET, 0);
    run_fma("d-fma-xx1-rn0",    B_XX, G_MIX, PEROP, KEEP, 0);
    run_fma("d-fma-xx1-xe1",    B_XX | B_XE, G_MIX, PEROP, RESET, 0);
    run_fma("s-fma-rn0-sing",   0, G_SING, PEROP, RESET, 1);
    run_fma("s-fma-rn1-sing",   1, G_SING, PEROP, RESET, 1);
    run_fma("s-fma-rn2-sing",   2, G_SING, PEROP, RESET, 1);
    run_fma("s-fma-rn3-sing",   3, G_SING, PEROP, RESET, 1);
    run_fma("s-fma-xx1-sing",   B_XX, G_SING, PEROP, KEEP, 1);
    run_fma("s-fma-rn0-bits-info", 0, G_BITS, PEROP, RESET, 1);

    /* --- estimations (pas de fsqrt sur le 7400) --- */
    run_est("est-rn0-norm",   0, G_NORM, RESET);
    run_est("est-rn0-bits",   0, G_BITS, RESET);
    run_est("est-rn1-norm",   1, G_NORM, RESET);
    run_est("est-xx1-rn0",    B_XX, G_MIX, KEEP);
    run_est("est-denorm",     0, G_DENORM, RESET);

    /* --- frsp --- */
    run_frsp("frsp-rn0-half",  0, G_HALF, RESET);
    run_frsp("frsp-rn1-half",  1, G_HALF, RESET);
    run_frsp("frsp-rn2-half",  2, G_HALF, RESET);
    run_frsp("frsp-rn3-half",  3, G_HALF, RESET);
    run_frsp("frsp-rn0-bits",  0, G_BITS, RESET);
    run_frsp("frsp-rn0-denorm", 0, G_DENORM, RESET);
    run_frsp("frsp-xx1-rn0",   B_XX, G_MIX, KEEP);

    /* --- fctiw / fctiwz --- */
    run_fctiw("fctiw-rn0", 0, G_INT, RESET, 0);
    run_fctiw("fctiw-rn1", 1, G_INT, RESET, 0);
    run_fctiw("fctiw-rn2", 2, G_INT, RESET, 0);
    run_fctiw("fctiw-rn3", 3, G_INT, RESET, 0);
    run_fctiw("fctiw-bits",     0, G_BITS, RESET, 0);
    run_fctiw("fctiw-xx1",      B_XX, G_INT, KEEP, 0);
    run_fctiw("fctiw-64b-info", 0, G_INT, RESET, 1);

    /* --- comparaisons --- */
    run_fcmp("fcmp-rn0-bits", 0, G_BITS, RESET);
    run_fcmp("fcmp-rn0-norm", 0, G_NORM, RESET);
    run_fcmp("fcmp-xx1",      B_XX, G_MIX, KEEP);

    /* --- instructions non arithmétiques (FPSCR intact attendu) --- */
    run_nonarith("fsel-fabs-fneg", 0, G_BITS);
    run_nonarith("fsel-xx1",       B_XX, G_MIX);

    /* --- conversions par la mémoire --- */
    run_lfsstfs("lfs-stfs-sing",      0, G_SING);
    run_lfsstfs("lfs-stfs-half",      0, G_HALF);
    run_lfsstfs("lfs-stfs-bits-info", 0, G_BITS);

    /* --- cas limites : produit croisé --- */
    run_edge_bin("edge-bin-rn0", 0);
    run_edge_bin("edge-bin-rn1", 1);
    run_edge_bin("edge-bin-rn2", 2);
    run_edge_bin("edge-bin-rn3", 3);
    run_edge_bin("edge-bin-xx1", B_XX);
    run_edge_bin("edge-bin-xe1", B_XX | B_XE);
    run_edge_fma("edge-fma-rn0", 0);
    run_edge_fma("edge-fma-rn3", 3);
    run_edge_fma("edge-fma-xx1", B_XX);
    run_edge_un("edge-un-rn0", 0);
    run_edge_un("edge-un-rn1", 1);
    run_edge_un("edge-un-rn2", 2);
    run_edge_un("edge-un-rn3", 3);
    run_edge_un("edge-un-xx1", B_XX);
    run_edge_cmp("edge-cmp-rn0", 0);

    /* --- le chemin rapide du patch « flottant rapide » (docs/flottant-rapide.md) --- */
    /* opérandes float32 zéro-ou-normaux (ce que rend lfs) : la SEULE façon
     * d'emprunter float64r32_* rapide. XX=1 + RN=0 = amorçage armé. */
    run_bin("s-bin-xx1-singn",     B_XX,        G_SINGN, PEROP, KEEP,  1);
    run_bin("s-bin-rn0-singn",     0,           G_SINGN, PEROP, RESET, 1);
    run_bin("s-bin-xx1-singn-rn1", B_XX | 1,    G_SINGN, PEROP, KEEP,  1);
    run_bin("d-bin-xx1-singn",     B_XX,        G_SINGN, PEROP, KEEP,  0);
    run_bin("d-bin-rn0-singn",     0,           G_SINGN, PEROP, RESET, 0);
    run_fma("s-fma-xx1-singn",     B_XX,        G_SINGN, PEROP, KEEP,  1);
    run_fma("d-fma-xx1-singn",     B_XX,        G_SINGN, PEROP, KEEP,  0);
    run_bin("s-bin-rafale-singn",  B_XX,        G_SINGN, BURST, KEEP,  1);
    run_est("est-xx1-singn",       B_XX,        G_SINGN, KEEP);
    run_frsp("frsp-xx1-singn",     B_XX,        G_SINGN, KEEP);
    /* OE=1 ou UE=1 avec XX=1 : softfloat re-biaise, l'amorçage NE DOIT PAS s'armer */
    run_bin("d-bin-xx1-oe1",       B_XX | B_OE, G_SINGN, PEROP, RESET, 0);
    run_bin("d-bin-xx1-ue1",       B_XX | B_UE, G_SINGN, PEROP, RESET, 0);
    run_bin("s-bin-xx1-oe1",       B_XX | B_OE, G_SINGN, PEROP, RESET, 1);
    run_bin("s-bin-xx1-ue1",       B_XX | B_UE, G_SINGN, PEROP, RESET, 1);
    run_bin("d-bin-xx1-oe1ue1",    B_XX | B_OE | B_UE, G_SINGN, PEROP, RESET, 0);
    /* transition XX 0→1, et bascule mtfsb0/mtfsb1 en cours de rafale */
    run_transition("transition-xx");
    run_bascule("xx-bascule");
    /* débordements, soupassements, résultats dénormaux */
    run_ox_ux("ox-ux-rn0", 0);
    run_ox_ux("ox-ux-xx1", B_XX);
    run_ox_ux("ox-ux-xx1-oe1", B_XX | B_OE);
    run_ox_ux("ox-ux-xx1-ue1", B_XX | B_UE);
    run_ox_ux("ox-ux-xx1-rn3", B_XX | 3);

    /* --- état du FPSCR manipulé en cours de route --- */
    run_clear_xx("xx-clr-mtfsb0", CLR_MTFSB0);
    run_clear_xx("xx-clr-mtfsf",  CLR_MTFSF);
    run_clear_xx("xx-clr-mtfsfi", CLR_MTFSFI);
    run_clear_xx("xx-clr-mcrfs",  CLR_MCRFS);
    run_fx_alone("fx-efface-seul");
    run_fpscr_ops("fpscr-ops");

    /* --- rafales : mffs seulement à la fin --- */
    run_rafale("rafale-rn0", 0);
    run_rafale("rafale-xx1", B_XX);
    run_rafale("rafale-rn2", 2);
    run_bin("d-bin-rafale-mix", 0, G_MIX, BURST, KEEP, 0);
    run_fma("d-fma-rafale-mix", 0, G_MIX, BURST, KEEP, 0);
    run_bin("s-bin-rafale-sing", 0, G_SING, BURST, KEEP, 1);

#if HAVE_AV
    /* --- AltiVec : mode Java (NJ=0) et non-Java (NJ=1) --- */
    run_av("av-java-norm",      0,       G_NORM);
    run_av("av-java-bits",      0,       G_BITS);
    run_av("av-java-denorm",    0,       G_DENORM);
    run_av("av-nonjava-norm",   VSCR_NJ, G_NORM);
    run_av("av-nonjava-bits",   VSCR_NJ, G_BITS);
    run_av("av-nonjava-denorm", VSCR_NJ, G_DENORM);
#else
    printf("# altivec absent de cette compilation (-faltivec manquant)\n");
#endif

    printf("# fpscr_a_la_sortie=%08x\n", fpscr_get());
    printf("# fin\n");
    return 0;
}
