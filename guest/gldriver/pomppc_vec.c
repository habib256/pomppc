/*
 * pomppc_vec.c — balayage AltiVec des indices d'un dessin (POMPPC_GL_IDXVEC).
 *
 * Le chemin natif (DRAW_NATIVE) doit connaître la plage [vmin, vmax] des
 * indices d'un glDrawElements pour ne recopier dans le miroir hôte que les
 * octets sales que le dessin lit (raw_sync). DOOM 3 envoie ~456 000 indices
 * 32 bits par image : en scalaire, sept à neuf instructions par indice sur le
 * G4 émulé. Ici, quatre indices par vminuw / vmaxuw (huit en 16 bits), que
 * QEMU traduit en instructions SIMD de l'hôte (tcg_gen_gvec_umin/umax).
 *
 * Compilé SEUL avec -faltivec (Makefile) : le reste du plugin ne doit pas
 * voir les mots-clés vector / pixel / bool. Le résultat est exact (min et max
 * entiers) ; POMPPC_GL_IDXVEC=2 le compare au balayage scalaire à chaque
 * dessin (ligne D3X de la note).
 */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <altivec.h>

#include "pomppc_gld.h"

int pomppc_vec_available(void)
{
    int v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname("hw.vectorunit", &v, &len, 0, 0) == 0 && v)
        return 1;
    v = 0;
    len = sizeof(v);
    if (sysctlbyname("hw.optional.altivec", &v, &len, 0, 0) == 0 && v)
        return 1;
    return 0;
}

typedef union {
    vector unsigned int v;
    unsigned int w[4];
} V32;

typedef union {
    vector unsigned short v;
    unsigned short h[8];
} V16;

int pomppc_vec_minmax_u32(const unsigned long *p, long n,
                          unsigned long *mn, unsigned long *mx)
{
    unsigned long lo = 0xffffffffUL, hi = 0, v;
    long i = 0;
    int k;

    if (n <= 0 || !p)
        return 0;
    /* tête : jusqu'à une adresse multiple de 16 (lvx ignore les 4 bits bas) */
    while (i < n && ((unsigned long)(p + i) & 15)) {
        v = p[i++];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (n - i >= 16) {
        vector unsigned int vlo = (vector unsigned int)vec_splat_s32(-1);
        vector unsigned int vhi = (vector unsigned int)vec_splat_s32(0);
        V32 a, b;
        for (; n - i >= 16; i += 16) {
            const unsigned long *q = p + i;
            vector unsigned int x0 = vec_ld(0, (const unsigned int *)q);
            vector unsigned int x1 = vec_ld(16, (const unsigned int *)q);
            vector unsigned int x2 = vec_ld(32, (const unsigned int *)q);
            vector unsigned int x3 = vec_ld(48, (const unsigned int *)q);
            vlo = vec_min(vlo, vec_min(vec_min(x0, x1), vec_min(x2, x3)));
            vhi = vec_max(vhi, vec_max(vec_max(x0, x1), vec_max(x2, x3)));
        }
        a.v = vlo;
        b.v = vhi;
        for (k = 0; k < 4; k++) {
            if (a.w[k] < lo) lo = a.w[k];
            if (b.w[k] > hi) hi = b.w[k];
        }
    }
    /* queue */
    for (; i < n; i++) {
        v = p[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    *mn = lo;
    *mx = hi;
    return 1;
}

int pomppc_vec_minmax_u16(const unsigned short *p, long n,
                          unsigned long *mn, unsigned long *mx)
{
    unsigned long lo = 0xffffUL, hi = 0, v;
    long i = 0;
    int k;

    if (n <= 0 || !p)
        return 0;
    while (i < n && ((unsigned long)(p + i) & 15)) {
        v = p[i++];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (n - i >= 32) {
        vector unsigned short vlo = (vector unsigned short)vec_splat_s16(-1);
        vector unsigned short vhi = (vector unsigned short)vec_splat_s16(0);
        V16 a, b;
        for (; n - i >= 32; i += 32) {
            const unsigned short *q = p + i;
            vector unsigned short x0 = vec_ld(0, q);
            vector unsigned short x1 = vec_ld(16, q);
            vector unsigned short x2 = vec_ld(32, q);
            vector unsigned short x3 = vec_ld(48, q);
            vlo = vec_min(vlo, vec_min(vec_min(x0, x1), vec_min(x2, x3)));
            vhi = vec_max(vhi, vec_max(vec_max(x0, x1), vec_max(x2, x3)));
        }
        a.v = vlo;
        b.v = vhi;
        for (k = 0; k < 8; k++) {
            if (a.h[k] < lo) lo = a.h[k];
            if (b.h[k] > hi) hi = b.h[k];
        }
    }
    for (; i < n; i++) {
        v = p[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    *mn = lo;
    *mx = hi;
    return 1;
}
