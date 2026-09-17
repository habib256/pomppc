/*
 * POMPPCQFB_info.c — descripteur kmod du kext, ce que Xcode 2.x génère
 * automatiquement pour tout projet « Kernel Extension ». Sans lui, le Mach-O
 * n'a pas de symbole _kmod_info et kextload refuse le kext :
 *   « Executable file doesn't contain kernel extension code » (vu en vrai).
 *
 * _start/_stop viennent de libkmodc++.a (elles exécutent les constructeurs
 * statiques C++ puis appellent _realmain/_antimain si non nuls).
 */
#include <mach/mach_types.h>
#include <mach/kmod.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t  *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;

KMOD_EXPLICIT_DECL(net.pomppc.POMPPCQFB, "0.1", _start, _stop)
