/*
 * vfptest.c - equivalence de vaddfp/vsubfp/vmaddfp/vnmsubfp entre les helpers
 * d'origine et le chemin rapide a 4 voies (propriete x-vfp-fast,
 * patches/tcg/0003-ppc-vfp-fast.patch, docs/tcg-g4.md section 9).
 *
 * Execute les VRAIES instructions AltiVec dans l'invite Tiger, VSCR[NJ] a 0
 * puis a 1, sur :
 *   1. un catalogue de 40 valeurs limites croise sur une voie (40^2 pour
 *      add/sub, 40^3 pour les FMA), les trois autres voies normales ;
 *   2. N vecteurs aleatoires (defaut 2^22) : voies normales, extremes,
 *      zeros, denormaux, infinis, NaN, annulations exactes ;
 * et hache les resultats. La sortie doit etre IDENTIQUE octet pour octet
 * entre x-vfp-fast=off et =on (empreinte FNV en derniere ligne).
 * Plus vperm (patches/tcg/0004, x-vperm-fast) : chaque octet de controle a
 * chaque position, et des vecteurs aleatoires (dont r = a = c).
 * Un banc (BANC=N) : N x (2 vmaddfp + vaddfp + vsubfp) dependants, puis
 * N x 4 vperm dependants, temps en ms.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef unsigned int u32;
typedef unsigned long long u64;

static u64 fnv = 1469598103934665603ULL;
static void h(const void *p, size_t n)
{
    const unsigned char *c = p;
    while (n--) { fnv ^= *c++; fnv *= 1099511628211ULL; }
}
static void out(const char *s)
{
    fputs(s, stdout);
    h(s, strlen(s));
}

typedef union { vector float v; u32 u[4]; } V;

static u32 hh[4];
static void acc(const V *r)
{
    int i;
    for (i = 0; i < 4; i++) {
        hh[i] = (hh[i] ^ r->u[i]) * 16777619u;
    }
}

static void run4(const V *a, const V *b, const V *c)
{
    V r;
    r.v = vec_add(a->v, b->v); acc(&r);
    r.v = vec_sub(a->v, b->v); acc(&r);
    r.v = vec_madd(a->v, c->v, b->v); acc(&r);
    r.v = vec_nmsub(a->v, c->v, b->v); acc(&r);
}

static u64 st = 0x5eed;
static u32 rnd(void)
{
    st = st * 6364136223846793005ULL + 1442695040888963407ULL;
    return (u32)(st >> 32);
}
static u32 mkf(u32 s, u32 e, u32 f) { return s << 31 | (e & 0xff) << 23 | (f & 0x7fffff); }
static u32 lane(void)
{
    u32 r = rnd(), f = rnd() & 0x7fffff, s = r & 1, k = (r >> 8) % 100, e = r >> 16;
    if (k < 55) return mkf(s, 112 + e % 32, f);
    if (k < 70) return mkf(s, 1 + e % 254, f);
    if (k < 76) return mkf(s, 1 + e % 6, f);
    if (k < 82) return mkf(s, 249 + e % 6, f);
    if (k < 88) return s << 31;
    if (k < 92) return mkf(s, 0, f | 1);
    if (k < 95) return mkf(s, 0xff, 0);
    if (k < 98) return mkf(s, 0xff, f | 0x400000);
    return mkf(s, 0xff, (f & 0x3fffff) | 1);
}

static const u32 CAT[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x80800000,
    0x00800001, 0x01000000, 0x3f800000, 0xbf800000, 0x3f800001, 0x3f7fffff,
    0x3f000000, 0x33800000, 0x33000000, 0x4b800000, 0x4b7fffff, 0x7f7fffff,
    0xff7fffff, 0x7f000000, 0x1f800000, 0x1f000000, 0x20000000, 0x5f800000,
    0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001, 0x3eaaaaab, 0xbeaaaaab,
    0x40490fdb, 0x3dcccccd, 0x2d000000, 0x52000000, 0x00200000, 0x3fc00000,
    0x3fffffff, 0x0d800000, 0x71800000, 0x7f7ffff0,
};
#define NCAT (sizeof CAT / sizeof CAT[0])

int main(int argc, char **argv)
{
    char line[256];
    long nrand = getenv("NVEC") ? atol(getenv("NVEC")) : (1L << 22), n;
    long banc = argc > 1 ? atol(argv[1]) : 0;
    unsigned nj, x, y, z, i;
    V a, b, c;
    struct timeval t0, t1;

    setvbuf(stdout, 0, _IONBF, 0);
    for (nj = 0; nj < 2; nj++) {
        vector unsigned short vs = (vector unsigned short)vec_splat_u32(0);
        V m;
        m.u[0] = m.u[1] = m.u[2] = 0; m.u[3] = nj ? 0x00010000 : 0;
        vs = (vector unsigned short)m.v;
        vec_mtvscr(vs);
        memset(hh, 0, sizeof hh);
        for (x = 0; x < NCAT; x++) {
            for (y = 0; y < NCAT; y++) {
                for (z = 0; z < NCAT; z++) {
                    for (i = 1; i < 4; i++) {
                        a.u[i] = mkf(0, 120 + i, 0x123456 * i);
                        b.u[i] = mkf(1, 118 + i, 0x654321 * i);
                        c.u[i] = mkf(0, 126, 0x0f0f0f * i);
                    }
                    a.u[0] = CAT[x]; b.u[0] = CAT[y]; c.u[0] = CAT[z];
                    run4(&a, &b, &c);
                }
            }
        }
        snprintf(line, sizeof line, "NJ=%u catalogue : %u triplets, h=%08x%08x%08x%08x\n",
                 nj, (unsigned)(NCAT * NCAT * NCAT), hh[0], hh[1], hh[2], hh[3]);
        out(line);
        memset(hh, 0, sizeof hh);
        gettimeofday(&t0, 0);
        for (n = 0; n < nrand; n++) {
            for (i = 0; i < 4; i++) {
                a.u[i] = lane(); b.u[i] = lane(); c.u[i] = lane();
                if ((rnd() & 7) == 0) {
                    float p = *(float *)&a.u[i] * *(float *)&c.u[i];
                    p = -p;
                    b.u[i] = *(u32 *)&p;
                } else if ((rnd() & 7) == 0) {
                    b.u[i] = a.u[i] ^ ((rnd() & 1) ? 0x80000000u : 1u);
                }
            }
            run4(&a, &b, &c);
        }
        gettimeofday(&t1, 0);
        snprintf(line, sizeof line, "NJ=%u aleatoire : %ld vecteurs x 4 instructions, h=%08x%08x%08x%08x\n",
                 nj, nrand, hh[0], hh[1], hh[2], hh[3]);
        out(line);
        fprintf(stderr, "(NJ=%u : %ld s)\n", nj, (long)(t1.tv_sec - t0.tv_sec));
    }
    /* vperm (patches/tcg/0004, x-vperm-fast) : chaque octet de controle a
     * chaque position, puis des vecteurs aleatoires */
    {
        typedef union { vector unsigned char v; u32 u[4]; unsigned char b[16]; } B;
        B pa, pb, pc, pr;
        unsigned pos, val, k;
        memset(hh, 0, sizeof hh);
        for (pos = 0; pos < 16; pos++) {
            for (val = 0; val < 256; val++) {
                for (k = 0; k < 4; k++) {
                    for (i = 0; i < 4; i++) {
                        pa.u[i] = rnd(); pb.u[i] = rnd(); pc.u[i] = rnd();
                    }
                    pc.b[pos] = val;
                    pr.v = vec_perm(pa.v, pb.v, pc.v);
                    acc((V *)&pr);
                }
            }
        }
        for (n = 0; n < nrand; n++) {
            for (i = 0; i < 4; i++) {
                pa.u[i] = rnd(); pb.u[i] = rnd(); pc.u[i] = rnd();
            }
            pr.v = vec_perm(pa.v, pb.v, pc.v);
            acc((V *)&pr);
            pa.v = vec_perm(pa.v, pb.v, pa.v);     /* r = a = c */
            acc((V *)&pa);
        }
        snprintf(line, sizeof line, "vperm : %u + %ld x 2 vecteurs, h=%08x%08x%08x%08x\n",
                 16 * 256 * 4, nrand, hh[0], hh[1], hh[2], hh[3]);
        out(line);
    }
    snprintf(line, sizeof line, "empreinte %016llx\n", fnv);
    fputs(line, stdout);
    if (banc > 0) {
        vector unsigned char p0 = vec_lvsl(3, (unsigned char *)0);
        vector unsigned char x0 = (vector unsigned char)vec_splat_u8(7);
        vector unsigned char x1 = (vector unsigned char)vec_splat_u8(9);
        typedef union { vector unsigned char v; u32 u[4]; } B;
        B rr;
        gettimeofday(&t0, 0);
        for (n = 0; n < banc; n++) {
            x0 = vec_perm(x0, x1, p0);
            x1 = vec_perm(x1, x0, p0);
            x0 = vec_perm(x0, x1, p0);
            x1 = vec_perm(x1, x0, p0);
        }
        gettimeofday(&t1, 0);
        rr.v = vec_xor(x0, x1);
        printf("banc : %ld x 4 vperm en %ld ms (%08x)\n", banc,
               (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000),
               rr.u[0]);
    }
    if (banc > 0) {
        vector float va = (vector float){ 0.5f, 0.25f, 0.75f, 0.125f };
        vector float vb = (vector float){ 1.0f, 2.0f, 3.0f, 4.0f };
        vector float vc = (vector float){ 0.5f, 0.75f, 0.25f, 0.625f };
        vector float acc1 = va, acc2 = vb;
        V r;
        vec_mtvscr((vector unsigned short)vec_splat_u32(0));
        gettimeofday(&t0, 0);
        for (n = 0; n < banc; n++) {
            acc1 = vec_madd(acc1, vc, vb);
            acc2 = vec_add(acc2, va);
            acc1 = vec_madd(acc1, va, vc);
            acc2 = vec_sub(acc2, vc);
        }
        gettimeofday(&t1, 0);
        r.v = vec_add(acc1, acc2);
        printf("banc : %ld x (2 vmaddfp + vaddfp + vsubfp) en %ld ms (%08x)\n", banc,
               (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000),
               r.u[0]);
    }
    return 0;
}
