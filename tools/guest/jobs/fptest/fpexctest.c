/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 VERHILLE Arnaud
 *
 * Tiger/PowerPC : exceptions flottantes precises, PC du chemin lent.
 * Compiler DANS Tiger : gcc -std=gnu99 -O2 -Wall -o fpexctest fpexctest.c
 * Comparer froid eteint/allume, meme binaire et memes options CPU.
 *
 * [B — logiciel] Apple xnu-792.24.17, bsd/dev/ppc/unix_signal.c,
 * sigreturn : restaure les etats PPC puis active les exceptions precises si
 * SIGFPE a un gestionnaire. osfmk/ppc/status.c, thread_enable_fpe, pose FE0/FE1.
 * osfmk/ppc/proc_reg.h, MSR_IMPORT_BITS, autorise aussi leur importation.
 * https://github.com/apple-oss-distributions/xnu/tree/xnu-792.24.17
 * [D] QEMU v11.1.2 + POMPPC : les helpers FP doivent conserver le PC de
 * l'instruction invitee. Ce test de non-regression n'est pas une mesure G4.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#if defined(__DARWIN_UNIX03) && __DARWIN_UNIX03
#define UC_PC(u) ((u)->uc_mcontext->__ss.__srr0)
#define UC_MSR(u) ((u)->uc_mcontext->__ss.__srr1)
#define UC_FPSCR(u) ((u)->uc_mcontext->__fs.__fpscr)
#else
#define UC_PC(u) ((u)->uc_mcontext->ss.srr0)
#define UC_MSR(u) ((u)->uc_mcontext->ss.srr1)
#define UC_FPSCR(u) ((u)->uc_mcontext->fs.fpscr)
#endif

static volatile uintptr_t wanted_pc, resume_pc;
static volatile sig_atomic_t hits, errors, last_delta, last_code;
static volatile uint32_t last_fpscr, last_msr;

static void handler(int sig, siginfo_t *info, void *context)
{
    ucontext_t *u = context;
    if (sig == SIGUSR1) {
        UC_MSR(u) |= 0x900;
        return;
    }
    if (sig != SIGFPE || !resume_pc) {
        _exit(90);
    }
    hits++;
    last_delta = (int32_t)((uintptr_t)UC_PC(u) - wanted_pc);
    last_code = info->si_code;
    last_fpscr = UC_FPSCR(u);
    last_msr = UC_MSR(u) & 0x900;
    if (last_delta != 0 || last_msr != 0x900) {
        errors++;
    }
    /* Si le PC est inattendu, garder sa pile et rejouer avec les exceptions
     * desactivees ; ne pas sauter dans l'epilogue d'une autre fonction. */
    if (last_delta == 0) {
        UC_PC(u) = resume_pc;
    }
    UC_FPSCR(u) = 0;
}

static void fpscr(uint32_t control)
{
    uint64_t bits = control;
    __asm__ volatile("lfd 0,%0\n\tmtfsf 255,0" : : "m"(bits) : "fr0", "memory");
}

#define OP(name, instruction) \
extern const unsigned char pc_##name[], after_##name[]; \
__attribute__((noinline)) static void op_##name(const uint64_t *args) \
{ \
    __asm__ volatile("lfd 1,0(%0)\n\tlfd 2,8(%0)\n" \
        ".globl _pc_" #name "\n_pc_" #name ":\n\t" instruction "\n" \
        ".globl _after_" #name "\n_after_" #name ":\n\tnop" \
        : : "r"(args) : "fr0", "fr1", "fr2", "cr0", "memory"); \
}
OP(adds, "fadds 0,1,2")
OP(add, "fadd 0,1,2")
OP(divs, "fdivs 0,1,2")
OP(cmpo, "fcmpo 0,1,2")
OP(rsp, "frsp 0,1")

struct trial {
    const char *name;
    void (*op)(const uint64_t *);
    const unsigned char *pc, *resume;
    uint32_t enable;
    uint64_t args[2];
};

int main(int argc, char **argv)
{
    const struct trial trials[] = {
        {"fadds-snan", op_adds, pc_adds, after_adds, 0x80,
            {UINT64_C(0x7ff0000000000001), UINT64_C(0x3ff0000000000000)}},
        {"fadd-snan", op_add, pc_add, after_add, 0x80,
            {UINT64_C(0x7ff0000000000001), UINT64_C(0x3ff0000000000000)}},
        {"fdivs-zero", op_divs, pc_divs, after_divs, 0x10,
            {UINT64_C(0x3ff0000000000000), 0}},
        {"fcmpo-qnan", op_cmpo, pc_cmpo, after_cmpo, 0x80,
            {UINT64_C(0x7ff8000000000000), UINT64_C(0x3ff0000000000000)}},
        {"frsp-snan", op_rsp, pc_rsp, after_rsp, 0x80,
            {UINT64_C(0x7ff0000000000001), 0}},
    };
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 128;
    unsigned t, i, total_errors = 0;
    struct sigaction action;
    if (!n || n > 10000) return 2;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGFPE, &action, NULL) || sigaction(SIGUSR1, &action, NULL)) return 2;
    alarm(60);
    for (t = 0; t < sizeof(trials) / sizeof(trials[0]); t++) {
        const struct trial *p = &trials[t];
        hits = errors = last_delta = last_code = 0;
        last_msr = last_fpscr = 0;
        wanted_pc = (uintptr_t)p->pc;
        resume_pc = (uintptr_t)p->resume;
        for (i = 0; i < n; i++) {
            sig_atomic_t before = hits;
            fpscr(p->enable);
            if (raise(SIGUSR1)) return 2;
            p->op(p->args);
            fpscr(0);
            if (hits != before + 1) errors++;
        }
        printf("%s n=%u signaux=%u erreurs=%u pc_delta=%d fpscr=%08x msr_fe=%03x code=%d\n",
               p->name, n, (unsigned)hits, (unsigned)errors, (int)last_delta,
               (unsigned)last_fpscr, (unsigned)last_msr, (int)last_code);
        total_errors += errors;
    }
    alarm(0);
    printf("exceptions FP : %u cas, erreurs %u\n", n * t, total_errors);
    return total_errors != 0;
}
