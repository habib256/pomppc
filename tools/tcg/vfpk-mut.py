#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""vfpk-mut.py ARBRE-USER [MUTANT…] — contre-épreuves de patches/tcg/0053 (x-vfp-known,
docs/tcg-g4.md §42) sur le banc linux-user.

ARBRE-USER : la copie linux-user de l'arbre (tools/tcg/fpnatcmp-userhack.py), construite dans
build/. Pour chaque mutant : une substitution dans un fichier source, `ninja qemu-ppc`, le
binaire rangé dans OUT/mut-<nom>, le fichier rendu tel quel. Puis (tools/tcg/vfpkuser.sh
« un ») vfptest k 512 (graine 0), kx 256 (graine 1) et kp 16384, bras vfpk (sans vérificateur) et
ver (x-vfp-native-verify, x-vfp-native-cmp-verify) : lignes d'empreinte différentes de celles
du binaire non muté, divergences du vérificateur, sorties de bloc. Une ligne par mutant.
Variables : OUT (défaut $TMPDIR/vfpkmut), TMO (secondes par passage, défaut 900).
"""
import os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.expanduser(sys.argv[1])
OUT = os.environ.get('OUT', os.path.join(os.environ.get('TMPDIR', '/tmp'), 'vfpkmut'))
TMO = int(os.environ.get('TMO', '900'))
X86 = 'tcg/x86_64/tcg-target.c.inc'
IH = 'target/ppc/int_helper.c'
VM = 'target/ppc/translate/vmx-impl.c.inc'
TC = 'tcg/tcg.c'

MUTANTS = {
    # le talon n'appelle jamais la vérification (R/X sans effet)
    'talon-sans-controle': (X86, "    if (l->fp_sel & PPC_VFPK_EXITS) {\n        tcg_out_mov(s, TCG_TYPE_PTR, tcg_target_call_iarg_regs[0], TCG_AREG0);",
                            "    if (0) {\n        tcg_out_mov(s, TCG_TYPE_PTR, tcg_target_call_iarg_regs[0], TCG_AREG0);"),
    # la vérification ne sort jamais pour vD
    'sans-sortie-vd': (IH, "            if (!vfp_zon(r->u32[i])) {", "            if (0) {"),
    # la vérification ne sort jamais pour la porte
    'sans-sortie-porte': (IH, "    if (!out && (sel & PPC_VFPK_X) &&", "    if (0 && !out && (sel & PPC_VFPK_X) &&"),
    # test de vD laxiste : les dénormaux acceptés
    'zon-laxiste': (IH, "            if (!vfp_zon(r->u32[i])) {",
                    "            if (((r->u32[i] >> 23) & 0xff) == 0xff) {"),
    # la sortie de bloc refait l'instruction (pas de nip + 4)
    'sans-nip4': (IH, "    cpu_restore_state(cs, ra);\n    env->nip += 4;", "    cpu_restore_state(cs, ra);\n"),
    # les globales ne sont pas rangées avant une op qui peut sortir
    'sans-sync': (TC, "            if (op->args[0] & PPC_VFPK_EXITS) {\n                la_global_sync(s, nb_globals);",
                  "            if (0) {\n                la_global_sync(s, nb_globals);"),
    # l'émetteur saute le test de vB quand vA est connu
    'test-b-par-a': (X86, "    bool test_a = !(sel & PPC_VFPK_A), test_b = !(sel & PPC_VFPK_B);\n    bool test_c = fma && !(sel & PPC_VFPK_C);\n    bool any_ok = false;\n    TCGLabelQemuLdst *l;",
                     "    bool test_a = !(sel & PPC_VFPK_A), test_b = !(sel & PPC_VFPK_A);\n    bool test_c = fma && !(sel & PPC_VFPK_C);\n    bool any_ok = false;\n    TCGLabelQemuLdst *l;"),
    # l'émetteur ne teste jamais la porte
    'porte-jamais': (X86, "    for (int w = 0; w < 2 && !(sel & PPC_VFPK_G); w++) {", "    for (int w = 0; w < 0; w++) {"),
    # le consommateur n'oblige pas son producteur (R jamais posé)
    'sans-rely': (VM, "        p->args[0] |= PPC_VFPK_R;", "        p->args[0] |= 0;"),
    # ni le garant de la porte (X jamais posé)
    'sans-rely-porte': (VM, "        ctx->vfpk_gate_prod->args[0] |= PPC_VFPK_X;", "        ctx->vfpk_gate_prod->args[0] |= 0;"),
    # vsldoi propage quel que soit le décalage
    'vsldoi-tout': (VM, "if ((insn & 0x3f) == 44 && ((insn >> 6) & 3) == 0) {", "if ((insn & 0x3f) == 44) {"),
    # vor propage même quand vA != vB
    'vor-tout': (VM, "    case 1156:                  /* vor: vmr when vA = vB */\n        if (a == b) {",
                 "    case 1156:                  /* vor: vmr when vA = vB */\n        if (1) {"),
    # vxor donne un zéro même quand vA != vB
    'vxor-tout': (VM, "    case 1220:                  /* vxor vD,vA,vA: zeros */\n        if (a == b) {",
                  "    case 1220:                  /* vxor vD,vA,vA: zeros */\n        if (1) {"),
    # vspltis* donne un zéro quelle que soit la constante
    'vspltis-tout': (VM, "        if (a == 0) {\n            nsrc = 0;", "        if (1) {\n            nsrc = 0;"),
    # vspltw : source vA (le champ UIM) au lieu de vB
    'vspltw-a': (VM, "    case 652:                   /* vspltw */\n        src[0] = b;",
                 "    case 652:                   /* vspltw */\n        src[0] = a;"),
    # propagation : un seul producteur gardé
    'un-producteur': (VM, "                    p[np++] = q;", "                    if (np == 0) {\n                        p[np++] = q;\n                    }"),
    # un rangement dans env ne retire rien
    'rangement-ignore': (VM, "                vfpk_drop(ctx, (intptr_t)op->args[2], size);", "                (void)size;"),
    # tout appel de helper ne retire que vD
    'appels-surs': (VM, "            if ((insn >> 26) == 4 && vfpk_safe_call(op)) {", "            if (1) {"),
    # un helper de la liste blanche ne retire même pas vD
    'liste-sans-vd': (VM, "                vfpk_drop(ctx, avr_full_offset(d), 16);\n            } else {\n                all = true;",
                      "                (void)d;\n            } else {\n                all = true;"),
    # les autres sortes de ppc_vfp (vcmp*fp, vcf*, vct*) ne retirent pas vD
    'cmp-garde-vd': (VM, "                vfpk_drop(ctx, (intptr_t)op->args[1], 16);", "                (void)op;"),
}


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build(name):
    r = run(['ninja', '-C', os.path.join(TREE, 'build'), 'qemu-ppc'])
    if r.returncode:
        print(name, ': construction impossible', r.stdout[-2000:], file=sys.stderr)
        return None
    dst = os.path.join(OUT, 'mut-' + name)
    shutil.copy2(os.path.join(TREE, 'build', 'qemu-ppc'), dst)
    return dst


def apply(m):
    path = os.path.join(TREE, m[0])
    s = open(path).read()
    if s.count(m[1]) != 1:
        raise SystemExit('%s : ancre trouvée %d fois dans %s' % (m, s.count(m[1]), m[0]))
    open(path, 'w').write(s.replace(m[1], m[2]))
    return path, s


def passes(q, tag):
    """[(nom, lignes d'empreinte, bilan)] pour k 512 0, kx 256 1, kp 16384 0, bras vfpk et ver."""
    res = []
    for arm in ('vfpk', 'ver'):
        for args in (('k', '512', '0'), ('kx', '256', '1'), ('kp', '16384', '0')):
            od = os.path.join(OUT, '%s-%s-%s' % (tag, arm, args[0]))
            os.makedirs(od, exist_ok=True)
            env = dict(os.environ, OUT=od, ARM=arm)
            try:
                r = run(['bash', os.path.join(HERE, 'vfpkuser.sh'), q, 'un'] + list(args),
                        env=env, timeout=TMO)
                out = r.stdout
            except subprocess.TimeoutExpired:
                out = 'DELAI DEPASSE\n'
            lines = [l for l in out.splitlines() if l.startswith(('K', 'P', 'total', 'DELAI'))]
            bil = ''
            m = re.findall(r'FINAL vfp-native-verify: (\d+) opérations.*?, (\d+) divergences', out)
            if m:
                bil = 'vérifiées %s divergences %s' % m[-1]
            m = re.findall(r'x-vfp-known : (\d+) talons relus, (\d+) sorties', out)
            if m:
                bil += ' talons %s sorties %s' % m[-1]
            res.append(('%s/%s' % (arm, args[0]), lines, bil))
    return res


def main():
    os.makedirs(OUT, exist_ok=True)
    names = sys.argv[2:] or list(MUTANTS)
    q0 = build('aucun')
    ref = passes(q0, 'aucun')
    for n, lines, bil in ref:
        print('aucun %-9s %s | %s' % (n, lines[-1] if lines else '?', bil), flush=True)
    for name in names:
        path, orig = apply(MUTANTS[name])
        try:
            q = build(name)
        finally:
            open(path, 'w').write(orig)
        if not q:
            continue
        res = passes(q, name)
        parts = []
        for (n, lines, bil), (_, l0, _) in zip(res, ref):
            diff = sum(1 for a, b in zip(lines, l0) if a != b) + abs(len(lines) - len(l0))
            parts.append('%s: %d lignes %s' % (n, diff, bil))
        print('%-20s %s' % (name, ' ; '.join(parts)), flush=True)
    build('aucun')


if __name__ == '__main__':
    main()
