/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * POMPPCFsqrt.kext — annonce `fsqrt` aux programmes de Tiger.
 *
 * La libm de Tiger (`_sqrt`, libSystem) lit _cpu_capabilities dans la commpage
 * (0xFFFF8020) et ne prend l'instruction `fsqrt` que si kHasFsqrt (0x20000000)
 * est levé — ce que le noyau ne fait que sur G5. Sinon elle appelle sa racine
 * logicielle (`___sqrt`, ~228 ns par appel sous TCG contre ~23 pour `fsqrt`,
 * mêmes résultats sur 2,3 M valeurs : tools/guest/jobs/sqrttest). QEMU exécute
 * `fsqrt` sur son modèle G4. Ce kext lève le bit dans la variable du noyau et
 * dans la commpage ; le déchargement le rebaisse.
 *
 * Dépend de com.apple.kernel (symboles hors KPI : __cpu_capabilities,
 * _commPagePtr32). Refuse de charger si la commpage ne porte pas la même
 * valeur que la variable (disposition inattendue).
 */
#include <mach/mach_types.h>
#include <mach/kmod.h>
#include <libkern/OSTypes.h>

#define kHasFsqrt          0x20000000
#define COMM_CPU_CAPS_OFF  0x020      /* _COMM_PAGE_CPU_CAPABILITIES - _COMM_PAGE_BASE_ADDRESS */

extern int _cpu_capabilities;         /* __cpu_capabilities du noyau */
extern char *commPagePtr32;           /* adresse noyau de la commpage 32 bits */
extern void printf(const char *, ...);

static int leve;                      /* le bit était baissé et c'est nous qui l'avons levé */

static volatile UInt32 *mot(void)
{
    return commPagePtr32 ? (volatile UInt32 *)(commPagePtr32 + COMM_CPU_CAPS_OFF) : 0;
}

kern_return_t fsqrt_start(kmod_info_t *ki, void *d)
{
    volatile UInt32 *m = mot();
    if (!m || *m != (UInt32)_cpu_capabilities) {
        printf("POMPPCFsqrt: commpage %p = %08x, _cpu_capabilities = %08x : refus\n",
               m, m ? (unsigned)*m : 0, (unsigned)_cpu_capabilities);
        return KERN_FAILURE;
    }
    if (!(*m & kHasFsqrt)) {
        _cpu_capabilities |= kHasFsqrt;
        *m |= kHasFsqrt;
        leve = 1;
    }
    printf("POMPPCFsqrt: _cpu_capabilities %08x (kHasFsqrt levé)\n", (unsigned)*m);
    return KERN_SUCCESS;
}

kern_return_t fsqrt_stop(kmod_info_t *ki, void *d)
{
    volatile UInt32 *m = mot();
    if (leve && m) {
        _cpu_capabilities &= ~kHasFsqrt;
        *m &= ~kHasFsqrt;
        leve = 0;
    }
    printf("POMPPCFsqrt: _cpu_capabilities %08x\n", m ? (unsigned)*m : 0);
    return KERN_SUCCESS;
}

__private_extern__ int _kext_apple_cc = __APPLE_CC__;
KMOD_EXPLICIT_DECL(net.pomppc.POMPPCFsqrt, "0.1", fsqrt_start, fsqrt_stop)
