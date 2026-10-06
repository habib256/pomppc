#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""tlbpproof.py ARBRE [N] [mut] — preuve hôte de x-tlb-precise (patches/tcg/0031,
docs/tcg-g4.md §32) sur le code de l'arbre QEMU, extrait tel quel.

Extrait de target/ppc/mmu_helper.c les prédicats tlbp_match_seg/cls/range et
ppc_tlbie_set_add, et de accel/tcg/cputlb.c la boucle de balayage par classes de
pomppc_tlb_flush_match ; les compile contre une petite table de TLB simulée
(adressage direct par numéro de page modulo n, n = 64..8192, plus 8 victimes) et
compare, sur N tirages, à la définition :
  - classe : après le balayage au pas de 16, il ne reste AUCUNE entrée dont la
    page a ses bits 12..15 dans le masque, et toutes les autres sont là ;
  - segment, plage : mêmes vérifications avec le balayage complet ;
  - ppc_tlbie_set_add : classes = OU des pages, pages dans l'ordre, débordement
    au-delà de PPC_TLBIE_PAGES (8) marqué n = 9.
`mut` : rejoue avec des mutants du code extrait (chacun doit échouer).
"""
import os, re, random, subprocess, sys, tempfile

tree = os.path.expanduser(sys.argv[1])
N = int(sys.argv[2]) if len(sys.argv) > 2 else 200000
MUT = len(sys.argv) > 3 and sys.argv[3] == "mut"


def func(src, name):
    """le texte d'une fonction C (de sa ligne de signature à l'accolade fermante)"""
    m = re.search(r"^(static [^\n]*\b%s\(.*?^\})" % re.escape(name), src, re.S | re.M)
    if not m:
        sys.exit("fonction %s introuvable" % name)
    return m.group(1)


mmu = open(os.path.join(tree, "target/ppc/mmu_helper.c")).read()
tlb = open(os.path.join(tree, "accel/tcg/cputlb.c")).read()
funcs = "\n\n".join(func(mmu, f) for f in
                    ("tlbp_match_seg", "tlbp_match_cls", "tlbp_match_range", "ppc_tlbie_set_add"))
m = re.search(r"(        /\* the index of a page is its number modulo n \(n >= 64\) \*/\n"
              r"        for \(int c = 0; c < 16; c\+\+\) \{.*?\n        \}\n)", tlb, re.S)
if not m:
    sys.exit("boucle de balayage introuvable dans cputlb.c")
loop = m.group(1)

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
typedef uint64_t vaddr;
typedef uint32_t target_ulong;
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_MASK ((vaddr)-1 << TARGET_PAGE_BITS)
#define PPC_TLBIE_PAGES 8
struct PPCTlbieSet { uint16_t cls; uint8_t n; uint32_t pg[PPC_TLBIE_PAGES]; };
typedef struct { uint64_t addr_read; } CPUTLBEntry;
typedef bool (*PomppcTlbMatch)(vaddr page, uint64_t arg);
static vaddr pomppc_entry_page(CPUTLBEntry *e)
{ return e->addr_read == (uint64_t)-1 ? (vaddr)-1 : e->addr_read & TARGET_PAGE_MASK; }
static int used;
#define tlb_n_used_entries_dec(cpu, i) (used--)
%(funcs)s

static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static CPUTLBEntry tab[8192];

/* balayage par classes, code extrait de pomppc_tlb_flush_match */
static size_t scan_cls(size_t n, PomppcTlbMatch match, uint64_t arg, uint16_t cls)
{
    size_t dropped = 0;
    struct { CPUTLBEntry *table; } F = { tab }, *f = &F;
    int mmu_idx = 0; void *cpu = 0; (void)cpu; (void)mmu_idx;
%(loop)s
    return dropped;
}

int main(int argc, char **argv)
{
    long N = atol(argv[1]), bad = 0, cases = 0;
    for (long t = 0; t < N; t++) {
        size_t n = (size_t)64 << (rnd() %% 8);          /* 64 .. 8192 */
        int kind = rnd() %% 3;
        uint64_t arg; uint16_t cls = 0;
        memset(tab, -1, sizeof(tab));
        for (size_t k = 0; k < n; k++) {
            if (rnd() %% 3) {                            /* entrée valide à sa place */
                uint64_t pg = ((rnd() << 4) & 0xffff0) | (k & 15);
                pg = (pg & ~(uint64_t)(n - 1) & 0xfffff) | k;
                tab[k].addr_read = (pg << 12) | (rnd() & 0x3f) * 0;
            }
        }
        if (kind == 0) { arg = rnd() & 0xffff; }
        else if (kind == 1) { arg = rnd() & 0xffff; cls = arg; }
        else { uint64_t a = rnd() & 0xfffff000, b = a + ((rnd() %% 2048) << 12);
               uint64_t e = tab[rnd() %% n].addr_read;
               if (e != (uint64_t)-1 && (rnd() & 1)) {   /* bornes sur des entrées */
                   b = e & TARGET_PAGE_MASK;
                   a = b - (((rnd() %% 64) << 12) & b);
                   if (rnd() & 1) a = b;
               }
               if (b > 0xffffffffull) b = 0xfffff000; arg = (a << 32) | (b + 0xfff); }
        CPUTLBEntry before[8192]; memcpy(before, tab, n * sizeof(tab[0]));
        PomppcTlbMatch mt = kind == 0 ? tlbp_match_seg : kind == 1 ? tlbp_match_cls : tlbp_match_range;
        scan_cls(n, mt, arg, cls);
        for (size_t k = 0; k < n; k++) {
            uint64_t a = before[k].addr_read;
            vaddr pg = a & TARGET_PAGE_MASK;
            bool should;
            if (a == (uint64_t)-1) continue;
            cases++;
            if (kind == 0) should = (arg >> ((pg >> 28) & 15)) & 1;
            else if (kind == 1) should = (arg >> ((pg >> 12) & 15)) & 1;
            else should = pg >= (arg >> 32) && pg <= (uint32_t)arg;
            if (should != (tab[k].addr_read == (uint64_t)-1)) {
                if (bad++ < 5) printf("ÉCART genre %%d n %%zu page %%llx\n", kind, n, (unsigned long long)pg);
            }
        }
        /* ppc_tlbie_set_add contre le modèle */
        {
            struct PPCTlbieSet s; uint16_t c = 0; int m = rnd() %% 12; uint32_t p[16]; int np = 0, over = 0;
            memset(&s, 0, sizeof(s));
            for (int i = 0; i < m; i++) {
                uint32_t ea = rnd() & 0xfffff000;
                if (i && (rnd() & 3) == 0) ea = p[np ? np - 1 : 0] << 12;
                ppc_tlbie_set_add(&s, ea);
                c |= 1 << ((ea >> 12) & 15);
                if (np < 8) { if (np == 0 || p[np - 1] != ea >> 12) p[np++] = ea >> 12; }
                else over = 1;
            }
            cases++;
            if (s.cls != c || (over ? s.n != 9 : (s.n != np || memcmp(s.pg, p, np * 4)))) {
                if (bad++ < 5) printf("ÉCART tlbie_set_add\n");
            }
        }
    }
    printf("tlbpproof : %%ld tirages, %%ld cas vérifiés, %%ld écarts\n", N, cases, bad);
    return bad != 0;
}
'''

MUTANTS = [
    ("pas de 16 décalé", lambda f, l: (f, l.replace("j = c; j < n; j += 16", "j = (c + 1) & 15; j < n; j += 16"))),
    ("classe par les bits 13..16", lambda f, l: (f.replace("(page >> TARGET_PAGE_BITS) & 15", "(page >> (TARGET_PAGE_BITS + 1)) & 15"), l)),
    ("segment par les bits 27..30", lambda f, l: (f.replace("(page >> 28) & 15", "(page >> 27) & 15"), l)),
    ("plage sans sa dernière page", lambda f, l: (f.replace("page <= (uint32_t)r", "page < ((uint32_t)r & ~0xfffu)"), l)),
    ("pages de tlbie sans débordement", lambda f, l: (f.replace("s->n = PPC_TLBIE_PAGES + 1;", ";"), l)),
]


def run(fsrc, lsrc, n):
    d = tempfile.mkdtemp(prefix="tlbpproof-")
    c = os.path.join(d, "t.c")
    open(c, "w").write(HARNESS % {"funcs": fsrc, "loop": lsrc})
    exe = os.path.join(d, "t")
    subprocess.check_call(["gcc", "-O2", "-w", "-o", exe, c])
    p = subprocess.run([exe, str(n)], capture_output=True, text=True)
    return p.returncode, p.stdout.strip().splitlines()[-1] if p.stdout.strip() else ""


rc, last = run(funcs, loop, N)
print(last)
if rc:
    sys.exit(1)
if MUT:
    det = 0
    for name, f in MUTANTS:
        ff, ll = f(funcs, loop)
        if (ff, ll) == (funcs, loop):
            print("  mutant « %s » : NON APPLIQUÉ (motif absent)" % name)
            continue
        r, l = run(ff, ll, max(2000, N // 20))
        det += r != 0
        print("  mutant « %s » : %s (%s)" % (name, "détecté" if r else "NON DÉTECTÉ", l))
    print("mutants détectés : %d / %d" % (det, len(MUTANTS)))
