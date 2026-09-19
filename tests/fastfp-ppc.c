/*
 * fastfp-ppc.c — test différentiel au niveau des INSTRUCTIONS PowerPC.
 *
 * Programme PowerPC 32 bits autonome (ni libc ni libm : appels système bruts)
 * qui exécute les vraies instructions flottantes et imprime, pour chacune, le
 * résultat ET le FPSCR lu par `mffs`. Exécuté sous `qemu-ppc` (cible
 * ppc-linux-user) une fois sans la propriété x-fast-fp et une fois avec, ses
 * deux sorties doivent être identiques à FX, FR et FI près — c'est le contrat
 * de docs/flottant-rapide.md § 3, vérifié sur les instructions elles-mêmes et
 * pas seulement sur softfloat.
 *
 * Comparée à celle d'un QEMU NON patché, la sortie du mode exact doit être
 * identique octet pour octet.
 *
 * Voir tests/fastfp-ppc.sh, qui compile, exécute et compare. Prérequis :
 * powerpc-linux-gnu-gcc et un build ppc-linux-user (que le dépôt ne produit
 * pas : ../configure --target-list=ppc-linux-user dans un build à part).
 *
 * Quatre phases, pilotées par g_prep :
 *   0 — FPSCR remis à 0 avant chaque instruction : XX = 0, l'amorçage ne doit
 *       JAMAIS s'armer, les deux modes doivent coïncider y compris sur FX/FI ;
 *   1 — FPSCR = XX avant chaque instruction : amorçage armé à tous les coups ;
 *   2 — FPSCR laissé s'accumuler : le cas réaliste (XX se pose tout seul) ;
 *   3 — XE armé, opérations exactes seulement (armer XE alors que XX est déjà
 *       posé pose FEX et déroute au mtfsf : l'état n'est pas atteignable).
 */
typedef unsigned int u32;
typedef unsigned long long u64;
#define FP_FX (1u<<31)
#define FP_XX (1u<<25)
#define FP_FR (1u<<18)
#define FP_FI (1u<<17)
#define FP_XE (1u<<3)

static long sys3(long n, long a, long b, long c)
{
    register long r0 __asm__("r0") = n;
    register long r3 __asm__("r3") = a;
    register long r4 __asm__("r4") = b;
    register long r5 __asm__("r5") = c;
    __asm__ volatile("sc" : "+r"(r3), "+r"(r0)
                     : "r"(r4), "r"(r5)
                     : "memory","r6","r7","r8","r9","r10","r11","r12","cr0");
    return r3;
}
static void out(const char *s, unsigned n) { sys3(4, 1, (long)s, n); }
static void die(int c) { sys3(1, c, 0, 0); for (;;) {} }

static char buf[65536];
static unsigned blen;
static void flush(void) { if (blen) { out(buf, blen); blen = 0; } }
static void pc(char c) { if (blen >= sizeof(buf) - 1) flush(); buf[blen++] = c; }
static void phex(u64 v, int digits)
{
    static const char h[] = "0123456789abcdef";
    int i;
    for (i = digits - 1; i >= 0; i--) pc(h[(v >> (4 * i)) & 15]);
}
static void pstr(const char *s) { while (*s) pc(*s++); }

union du { double d; u64 u; };

static u32 rd_fpscr(void)
{
    union du t;
    __asm__ volatile("mffs %0" : "=f"(t.d));
    return (u32)t.u;
}
static void wr_fpscr(u32 v)
{
    union du t;
    t.u = 0xfff8000000000000ULL | (u64)v;
    __asm__ volatile("mtfsf 0xff,%0" :: "f"(t.d));
}

#define OP2(name, insn) \
static double name(double a, double b) \
{ double r; __asm__ volatile(insn " %0,%1,%2" : "=f"(r) : "f"(a), "f"(b)); return r; }
OP2(o_fadds, "fadds")
OP2(o_fsubs, "fsubs")
OP2(o_fdivs, "fdivs")
OP2(o_fadd,  "fadd")
OP2(o_fsub,  "fsub")
OP2(o_fdiv,  "fdiv")
static double o_fmuls(double a, double b)
{ double r; __asm__ volatile("fmuls %0,%1,%2" : "=f"(r) : "f"(a), "f"(b)); return r; }
static double o_fmul(double a, double b)
{ double r; __asm__ volatile("fmul %0,%1,%2" : "=f"(r) : "f"(a), "f"(b)); return r; }
#ifdef HAVE_FSQRT
static double o_fsqrts(double a)
{ double r; __asm__ volatile("fsqrts %0,%1" : "=f"(r) : "f"(a)); return r; }
static double o_fsqrt(double a)
{ double r; __asm__ volatile("fsqrt %0,%1" : "=f"(r) : "f"(a)); return r; }
#endif
#define OP3(name, insn) \
static double name(double a, double b, double c) \
{ double r; __asm__ volatile(insn " %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(b), "f"(c)); return r; }
OP3(o_fmadds,  "fmadds")
OP3(o_fmsubs,  "fmsubs")
OP3(o_fnmadds, "fnmadds")
OP3(o_fnmsubs, "fnmsubs")
OP3(o_fmadd,   "fmadd")
OP3(o_fmsub,   "fmsub")

static const u64 vals[] = {
    0x0000000000000000ULL, 0x8000000000000000ULL, 0x3ff0000000000000ULL,
    0xbff0000000000000ULL, 0x4000000000000000ULL, 0x3fe0000000000000ULL,
    0x3ff0000000000001ULL, 0x3ff0000020000000ULL, 0x3810000000000000ULL,
    0x380fffffc0000000ULL, 0x36a0000000000000ULL, 0x47efffffe0000000ULL,
    0xc7efffffe0000000ULL, 0x47f0000000000000ULL, 0x0010000000000000ULL,
    0x0000000000000001ULL, 0x7fefffffffffffffULL, 0x7ff0000000000000ULL,
    0xfff0000000000000ULL, 0x7ff8000000000000ULL, 0x7ff0000000000001ULL,
    0x4170000000000000ULL, 0x4170000010000000ULL, 0x3e60000000000000ULL,
    0x400921fb54442d18ULL, 0x4009220000000000ULL, 0x3fb999999999999aULL,
    0x3fb99999a0000000ULL, 0xc17000000c000000ULL, 0x41cdcd6500000000ULL,
};
#define NV ((int)(sizeof(vals) / sizeof(vals[0])))

/* prep : 0 = FPSCR remis à 0 avant chaque op ; 1 = FPSCR = XX avant chaque op ;
 *        2 = FPSCR laissé tel quel (XX s'accumule) ; 3 = FPSCR = XE. */
static u32 g_prep;
static void prep(void)
{
    if (g_prep == 0) wr_fpscr(0);
    else if (g_prep == 1) wr_fpscr(FP_XX);
    else if (g_prep == 3) wr_fpscr(FP_XE);
}
static void rec(const char *tag, int i, int j, int k, double r)
{
    union du t;
    t.d = r;
    pstr(tag); pc(' '); phex(i, 2); pc(' '); phex(j, 2); pc(' '); phex(k, 2);
    pc(' '); phex(t.u, 16); pc(' '); phex(rd_fpscr(), 8); pc('\n');
}

#define SWEEP2(tag, fn) do {                                    \
    int i, j;                                                   \
    for (i = 0; i < NV; i++) for (j = 0; j < NV; j++) {         \
        union du a, b; a.u = vals[i]; b.u = vals[j];            \
        prep(); rec(tag, i, j, 0, fn(a.d, b.d));                \
    } } while (0)

#define SWEEP3(tag, fn) do {                                    \
    int i, j, k;                                                \
    for (i = 0; i < NV; i++) for (j = 0; j < NV; j++)           \
        for (k = 0; k < NV; k += 3) {                           \
        union du a, b, c; a.u = vals[i]; b.u = vals[j]; c.u = vals[k]; \
        prep(); rec(tag, i, j, k, fn(a.d, b.d, c.d));           \
    } } while (0)


/* --- comparaisons et manipulation du FPSCR (gen_reset_fpstatus les emploie) --- */
static u32 do_fcmpu(double a, double b)
{
    u32 cr;
    __asm__ volatile("fcmpu 1,%1,%2\n\t" "mfcr %0" : "=r"(cr) : "f"(a), "f"(b) : "cr1");
    return cr;
}
static u32 do_fcmpo(double a, double b)
{
    u32 cr;
    __asm__ volatile("fcmpo 1,%1,%2\n\t" "mfcr %0" : "=r"(cr) : "f"(a), "f"(b) : "cr1");
    return cr;
}
static u32 do_mcrfs(int f)
{
    u32 cr;
    switch (f) {
    case 0: __asm__ volatile("mcrfs 2,0\n\t" "mfcr %0" : "=r"(cr) ::"cr2"); break;
    case 1: __asm__ volatile("mcrfs 2,1\n\t" "mfcr %0" : "=r"(cr) ::"cr2"); break;
    case 2: __asm__ volatile("mcrfs 2,2\n\t" "mfcr %0" : "=r"(cr) ::"cr2"); break;
    default:__asm__ volatile("mcrfs 2,5\n\t" "mfcr %0" : "=r"(cr) ::"cr2"); break;
    }
    return cr;
}
static void misc_sweep(void)
{
    int i, j;
    for (i = 0; i < NV; i++) {
        for (j = 0; j < NV; j++) {
            union du a, b; a.u = vals[i]; b.u = vals[j];
            prep();
            { u32 cr = do_fcmpu(a.d, b.d);
              pstr("fcmpu "); phex(i,2); pc(' '); phex(j,2); pc(' ');
              phex(cr,8); pc(' '); phex(rd_fpscr(),8); pc('\n'); }
            prep();
            { u32 cr = do_fcmpo(a.d, b.d);
              pstr("fcmpo "); phex(i,2); pc(' '); phex(j,2); pc(' ');
              phex(cr,8); pc(' '); phex(rd_fpscr(),8); pc('\n'); }
        }
    }
    /* mffs / mtfsf / mtfsb0 / mtfsb1 / mtfsfi / mcrfs : tous passent par
     * gen_reset_fpstatus(), que le patch 0002 met en ligne. */
    for (i = 0; i < NV; i++) {
        union du a, b; a.u = vals[i]; b.u = vals[(i + 7) % NV];
        prep();
        (void)o_fdivs(a.d, b.d);               /* salit le FPSCR */
        pstr("mffs "); phex(i,2); pc(' '); phex(rd_fpscr(),8); pc('\n');
        for (j = 0; j < 4; j++) {
            u32 cr = do_mcrfs(j);
            pstr("mcrfs "); phex(i,2); pc(' '); phex(j,2); pc(' ');
            phex(cr,8); pc(' '); phex(rd_fpscr(),8); pc('\n');
        }
        __asm__ volatile("mtfsb0 6");          /* efface XX */
        pstr("mtfsb0 "); phex(i,2); pc(' '); phex(rd_fpscr(),8); pc('\n');
        (void)o_fdivs(a.d, b.d);
        pstr("apres0 "); phex(i,2); pc(' '); phex(rd_fpscr(),8); pc('\n');
        __asm__ volatile("mtfsb1 6");          /* repose XX */
        pstr("mtfsb1 "); phex(i,2); pc(' '); phex(rd_fpscr(),8); pc('\n');
        (void)o_fmuls(a.d, b.d);
        pstr("apres1 "); phex(i,2); pc(' '); phex(rd_fpscr(),8); pc('\n');
        __asm__ volatile("mtfsfi 7,0");
        pstr("mtfsfi "); phex(i,2); pc(' '); phex(rd_fpscr(),8); pc('\n');
    }
}

static void sweep(void)
{
    SWEEP2("fadds", o_fadds);
    SWEEP2("fsubs", o_fsubs);
    SWEEP2("fmuls", o_fmuls);
    SWEEP2("fdivs", o_fdivs);
    SWEEP2("fadd",  o_fadd);
    SWEEP2("fsub",  o_fsub);
    SWEEP2("fmul",  o_fmul);
    SWEEP2("fdiv",  o_fdiv);
    SWEEP3("fmadds",  o_fmadds);
    SWEEP3("fmsubs",  o_fmsubs);
    SWEEP3("fnmadds", o_fnmadds);
    SWEEP3("fnmsubs", o_fnmsubs);
    SWEEP3("fmadd",   o_fmadd);
    SWEEP3("fmsub",   o_fmsub);
#ifdef HAVE_FSQRT
    { int i; for (i = 0; i < NV; i++) { union du a; a.u = vals[i];
        prep(); rec("fsqrts", i, 0, 0, o_fsqrts(a.d)); } }
    { int i; for (i = 0; i < NV; i++) { union du a; a.u = vals[i];
        prep(); rec("fsqrt", i, 0, 0, o_fsqrt(a.d)); } }
#endif
}

void _start(void)
{
    u32 p;
    for (p = 0; p < 3; p++) {
        g_prep = p;
        pstr("=== phase "); phex(p, 1); pc('\n');
        if (p == 2) wr_fpscr(0);
        sweep();
        misc_sweep();
        flush();
    }
    /* XE armé, opérations EXACTES seulement : armer XE alors que XX est
     * déjà posé pose FEX et déroute immédiatement (mtfsf), donc l'état
     * XX=1 & XE=1 n'est pas atteignable sans trappe. */
    flush();
    pstr("=== phase 3\n");
    {
        union du a, b;
        int i;
        for (i = 0; i < 8; i++) {
            a.u = 0x3ff0000000000000ULL; b.u = 0x4000000000000000ULL;
            wr_fpscr(FP_XE);
            rec("xe_fadds", i, 0, 0, o_fadds(a.d, b.d));
            wr_fpscr(FP_XE);
            rec("xe_fmuls", i, 0, 0, o_fmuls(a.d, b.d));
        }
        wr_fpscr(0);
    }
    flush();
    die(0);
}
