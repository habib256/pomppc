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
# idem pour les bilans AltiVec (x-vfp-native-verify, tcg/0034)
p = os.path.join(Q, 'target/ppc/int_helper.c')
s = open(p).read()
for c in ("vfn_n", "vfnc_total"):
    s = s.replace("if ((qatomic_fetch_inc(&%s) & ((1u << 26) - 1)) == 0) {" % c,
                  "if ((qatomic_fetch_inc(&%s) & ((1u << 16) - 1)) == 0) {" % c)
open(p, 'w').write(s)
# --- série jusqu'à 0050 (tcg/0051, docs/tcg-g4.md §40) : configurer avec --disable-werror ---
# fin du bloc système de cpu.h : déplacée par 0031-0043 après ms_last
ed('target/ppc/cpu.h',
   "    uint64_t ms_last;\n#endif\n\n    /* Other registers */",
   "    uint64_t ms_last;\n/* user hack: fin déplacée */\n\n    /* Other registers */")
# pomppc_jc_scan_stat (tcg/0011) appelée par translate-all.c en mode utilisateur aussi
ed('accel/tcg/internal-common.h',
   "void pomppc_jc_scan_stat(CPUState *cpu);       /* tcg/0011 */\n#endif\n",
   "#endif\nvoid pomppc_jc_scan_stat(CPUState *cpu);       /* tcg/0011, user hack */\n")
# helper_mem_stat (tcg/0031) : défini dans mmu_helper.c, absent du mode utilisateur
p = os.path.join(Q, 'target/ppc/user_only_helper.c')
s = open(p).read()
if 'helper_mem_stat' not in s:
    open(p, 'a').write('\n/* user hack (banc linux-user, tcg/0031) */\n'
                       '#include "exec/helper-proto.h"\n'
                       'void helper_mem_stat(CPUPPCState *env, target_ulong addr, uint32_t arg) { }\n')
# bilan final de x-fp-verify à la sortie (compteurs exacts de x-lfs-known, lfsk-mut.py)
ed('linux-user/exit.c', "void preexit_cleanup(CPUArchState *env, int code)\n{\n",
   "/* user hack (banc linux-user, tcg/0051) : bilan final de x-fp-verify */\n"
   "void ppc_fpn_report_user(void);\n\n"
   "void preexit_cleanup(CPUArchState *env, int code)\n{\n    ppc_fpn_report_user();\n")
ed('target/ppc/fpu_helper.c',
   "void helper_fpn_verify(CPUPPCState *env, uint64_t a, uint64_t b, uint64_t c,",
   "/* user hack (banc linux-user, tcg/0051) : bilan final, appelé par preexit_cleanup */\n"
   "void ppc_fpn_report_user(void);\nvoid ppc_fpn_report_user(void)\n{\n"
   "    if (qatomic_read(&fpn_atexit)) {\n        fprintf(stderr, \"FINAL \");\n"
   "        fpn_report();\n    }\n}\n\n"
   "void helper_fpn_verify(CPUPPCState *env, uint64_t a, uint64_t b, uint64_t c,")
# bilan final de x-vfp-native-verify et de x-vfp-known (tcg/0053), s'ils sont là
p = os.path.join(Q, 'target/ppc/int_helper.c')
if 'ppc_vfpk_check' in open(p).read():
    ed('linux-user/exit.c', "void ppc_fpn_report_user(void);\n\n",
       "void ppc_fpn_report_user(void);\nvoid ppc_vfn_report_user(void);       /* tcg/0053 */\n\n")
    ed('linux-user/exit.c', "{\n    ppc_fpn_report_user();\n",
       "{\n    ppc_fpn_report_user();\n    ppc_vfn_report_user();\n")
    ed('target/ppc/int_helper.c', "void helper_vfn_save(CPUPPCState *env, uint32_t regs)\n",
       "/* user hack (banc linux-user, tcg/0053) : bilan final, appelé par preexit_cleanup */\n"
       "void ppc_vfn_report_user(void);\nvoid ppc_vfn_report_user(void)\n{\n"
       "    fprintf(stderr, \"FINAL \");\n    if (qatomic_read(&vfn_atexit)) {\n"
       "        vfn_report();\n    } else {\n        vfpk_report();\n    }\n}\n\n"
       "void helper_vfn_save(CPUPPCState *env, uint32_t regs)\n")
print('ok')
