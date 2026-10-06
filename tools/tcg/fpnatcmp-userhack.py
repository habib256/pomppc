#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# fpnatcmp-userhack.py [ARBRE] — banc linux-user de fpnatcmp-user.sh (tcg/0033) : neutralise, dans une COPIE de l'arbre patché, le
# code « système seulement » des patches 0001/0008/0011/0021-0025 pour que
# qemu-ppc se construise, et lance l'invité avec MSR[FE0] = MSR[FE1] = 0 comme
# Tiger (linux-user les pose par défaut). N'est JAMAIS appliqué à l'arbre du
# binaire système. Idempotent.
import os, sys
Q = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else '~/src/qemu-fpu')

def ed(path, old, new):
    p = os.path.join(Q, path)
    s = open(p).read()
    if new in s:
        return
    if old not in s:
        print('%s : ancre absente (déjà fait ?)' % path); return
    open(p, 'w').write(s.replace(old, new, 1))

ed('accel/tcg/translator.c',
   "    const int sh = TARGET_PAGE_BITS - TB_JMP_PAGE_BITS;  /* runtime in common code */\n",
   """#ifdef CONFIG_USER_ONLY
    g_assert_not_reached();
}

static G_GNUC_UNUSED void ret_inline_user_unused(DisasContextBase *db,
                                                  TCGv_i64 pc, TCGv_i32 flags,
                                                  bool verify)
{
    const int sh = 12;
#else
    const int sh = TARGET_PAGE_BITS - TB_JMP_PAGE_BITS;  /* runtime in common code */
#endif
""")
ed('accel/tcg/translator.c', '#error "x-ret-inline: system mode only"\n',
   '/* user hack: x-ret-inline jamais allumé ici */\n')
ed('target/ppc/cpu.h',
   "#define TLB_NEED_SR_CHECK      0x4  /* segment registers changed (x-sr-tlb) */\n",
   "#define TLB_NEED_SR_CHECK      0x4  /* segment registers changed (x-sr-tlb) */\n"
   "#endif /* user hack (banc linux-user) */\n")
ed('target/ppc/cpu.h',
   "    uint64_t sr_verify_runs, sr_verify_entries, sr_verify_bad;\n#endif\n",
   "    uint64_t sr_verify_runs, sr_verify_entries, sr_verify_bad;\n"
   "/* user hack: fin déplacée */\n")
p = os.path.join(Q, 'accel/tcg/user-exec-stub.c')
s = open(p).read()
if 'pomppc_jc_scan_stat' not in s:
    open(p, 'a').write('\n/* user hack (banc linux-user, tcg/0033) */\n'
                       'void pomppc_jc_scan_stat(CPUState *cpu);\n'
                       'void pomppc_jc_scan_stat(CPUState *cpu) { }\n')
ed('target/ppc/cpu_init.c',
   "        msr |= (target_ulong)1 << MSR_FE0; /* Allow floating point exceptions */\n"
   "        msr |= (target_ulong)1 << MSR_FE1;\n",
   "        /* user hack: MSR[FE0] = MSR[FE1] = 0, comme sous Tiger */\n")
ed('target/ppc/fpu_helper.c',
   "#ifdef CONFIG_USER_ONLY\n    return true;\n#else\n    return (env->msr & ((1U << MSR_FE0) | (1U << MSR_FE1))) != 0;\n#endif\n",
   "    /* user hack: MSR[FE] comme en système (Tiger : 0) */\n"
   "    return (env->msr & ((1U << MSR_FE0) | (1U << MSR_FE1))) != 0;\n")
# linux-user sort par _exit : pas d'atexit ; bilan tous les 2^16 contrôles
p = os.path.join(Q, 'target/ppc/fpu_helper.c')
s = open(p).read()
s = s.replace("if ((qatomic_fetch_inc(&fpn_total) & ((1u << 24) - 1)) == 0) {",
              "if ((qatomic_fetch_inc(&fpn_total) & ((1u << 16) - 1)) == 0) {")
open(p, 'w').write(s)
print('ok')
