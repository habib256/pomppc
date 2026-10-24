#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""tlbb-mut.py ARBRE [MUTANT…] — contre-épreuves de patches/tcg/0052 (x-tlb-base,
docs/tcg-g4.md §41) dans la VM de dev.

ARBRE : une copie de l'arbre QEMU (commitée, construite dans build/ : jamais ~/src/qemu ni
l'arbre de travail). Pour chaque mutant : une substitution dans un fichier source, `ninja
qemu-system-ppc`, le binaire rangé dans build/mut-<nom>, le fichier rendu par `git checkout`.
Puis tools/tcg/tlbbnu.sh (mode superviseur sans système : helper au milieu d'un bloc,
PTE + tlbie, MMIO) avec x-tlb-base, et la VM de dev (tools/tcg/tlbbdev.sh, une à la fois)
avec x-tlb-base et x-tlb-base-verify, job tlbbtest (420 s au plus ; la référence en met
80) : erreurs (écarts à l'attendu calculé en C), empreinte (contre celle du bras de
référence), divergences du vérificateur, et si le job n'a pas abouti (VM figée ou QEMU
tombé).
Sortie : une ligne par mutant (et bench/tcg/tlbb/mut-<nom>/ du dépôt principal).
"""
import os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.expanduser(sys.argv[1])
REF = '028069d6e50d8d0b'        # empreinte de tlbbtest (modes 1-4), bras ref
X86 = 'tcg/x86_64/tcg-target.c.inc'
TR = 'target/ppc/translate.c'

MUTANTS = {
    # borne un octet trop loin : un accès qui déborde d'un octet sur la page suivante passe
    'borne+1': (X86, "TARGET_PAGE_SIZE - memop_size(mop), 0);",
                "TARGET_PAGE_SIZE - memop_size(mop) + 1, 0);"),
    # borne sans la taille : tout accès qui commence dans la page passe (à cheval compris)
    'borne-sans-taille': (X86, "TARGET_PAGE_SIZE - memop_size(mop), 0);",
                          "TARGET_PAGE_SIZE - 1, 0);"),
    # test de couverture retiré : tout chargement couvert va droit par l'adend gardé
    'sans-test': (X86, "tcg_out_opc(s, OPC_JCC_long + JCC_JA, 0, 0, 0);",
                  "tcg_out_opc(s, OPC_JCC_long + JCC_JO, 0, 0, 0);"),
    # le chemin lent du premier chargement ne pose pas le poison (negp : valeur d'avant)
    'sans-poison': (X86, "tcg_out_movi(s, TCG_TYPE_I32, l->tlbb_negp, TARGET_PAGE_SIZE);",
                    "(void)0;"),
    # la page gardée décalée d'une page (negp = -(page + 0x1000))
    'page+1': (X86, "tcg_out_modrm(s, OPC_GRP3_Ev + P_REXW, EXT3_NEG, negp);",
               "tcg_out_modrm(s, OPC_GRP3_Ev + P_REXW, EXT3_NEG, negp);\n"
               "        tgen_arithi(s, ARITH_SUB + P_REXW, negp, 0x1000, 0);"),
    # adend : le pointeur de l'entrée du TLB au lieu de son addend
    'adend-entree': (X86, "        tcg_out_ld(s, TCG_TYPE_PTR, ix, TCG_REG_L0,\n"
                          "                   offsetof(CPUTLBEntry, addend));",
                     "        tcg_out_ld(s, TCG_TYPE_PTR, ix, TCG_REG_L0,\n"
                     "                   offsetof(CPUTLBEntry, addend));\n"
                     "        if (ix != TCG_REG_L0) {\n"
                     "            tcg_out_mov(s, TCG_TYPE_PTR, ix, TCG_REG_L0);\n"
                     "        }"),
    # hors ligne (adresse non couverte) : retour sans le chargement
    'hors-ligne-vide': (X86, "    ld = prepare_host_addr(s, &h, l->addr_reg, l->oi, true);\n"
                             "    tcg_out_qemu_ld_direct(s, l->datalo_reg, -1, h, l->type,\n"
                             "                           get_memop(l->oi));",
                        "    ld = prepare_host_addr(s, &h, l->addr_reg, l->oi, true);"),
    # frontal : la base survit aux étiquettes et branchements
    'etiquettes': (TR, """            if (def->flags & (TCG_OPF_BB_END | TCG_OPF_COND_BRANCH)) {
                tlbb_kill_all(ctx);
                break;
            }
            for (int i = 0; i < def->nb_oargs; i++) {""", """            if (0) {
                tlbb_kill_all(ctx);
                break;
            }
            for (int i = 0; i < def->nb_oargs; i++) {"""),
    # frontal : la base survit à l'écriture de son registre (attendu équivalent : le test
    # porte sur l'adresse elle-même)
    'base-ecrite': (TR, """                    if (op->args[i] == temp_arg(tcgv_tl_temp(cpu_gpr[r])) &&
                        ctx->tlbb[r].first) {""", """                    if (0 &&
                        ctx->tlbb[r].first) {"""),
    # frontal : la base survit aux appels de helper (en espace utilisateur, aucun helper
    # ne change la traduction en cours de bloc : attendu non vu ici, voir tlbbnu)
    'appels': (TR, """        case INDEX_op_call:
            if (!tlbb_call_ok(op)) {""", """        case INDEX_op_call:
            if (0) {"""),
}


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)


def main():
    names = sys.argv[2:] or list(MUTANTS)
    for name in names:
        f, old, new = MUTANTS[name]
        p = os.path.join(TREE, f)
        s = open(p).read()
        if s.count(old) != 1:
            print('%-20s substitution introuvable (%d)' % (name, s.count(old)))
            continue
        open(p, 'w').write(s.replace(old, new))
        r = sh('cd %s/build && systemd-run --user --scope -q -p MemoryMax=20G '
               'ninja -j8 qemu-system-ppc' % TREE)
        sh('git -C %s checkout -- %s' % (TREE, f))
        if r.returncode:
            print('%-20s construction : échec\n%s' % (name, r.stdout[-2000:]))
            continue
        b = os.path.join(TREE, 'build', 'mut-' + name)
        shutil.copy(os.path.join(TREE, 'build', 'qemu-system-ppc'), b)
        nu = subprocess.run([os.path.join(HERE, 'tlbbnu.sh'), b, 'x-tlb-base=on'],
                            capture_output=True, text=True).stdout
        nu = re.findall(r'tlbbnu : .*', nu)
        env = dict(os.environ, QEMU_BIN=b, TIMEOUT='420')
        subprocess.run([os.path.join(HERE, 'tlbbdev.sh'), 'mut-' + name,
                        'x-tlb-base=on,x-tlb-base-verify=on', 'tlbbtest'],
                       env=env, capture_output=True, text=True)
        root = sh('git -C %s rev-parse --path-format=absolute --git-common-dir' % HERE
                  ).stdout.strip().rsplit('/.git', 1)[0]
        d = os.path.join(root, 'bench', 'tcg', 'tlbb', 'mut-' + name)
        txt = ''
        try:
            txt = open(os.path.join(d, '1-tlbbtest', 'out', 'tlbbtest.txt')).read()
        except OSError:
            pass
        q = ''
        try:
            q = open(os.path.join(d, 'qemu.log'), errors='replace').read()
        except OSError:
            pass
        err = re.findall(r'^erreurs (\d+)', txt, re.M)
        fp = re.findall(r'^empreinte ([0-9a-f]+)', txt, re.M)
        m4 = re.findall(r'^mode4 : .* (\d+) erreurs', txt, re.M)
        div = re.findall(r'tlb-base-verify: (\d+) chargements couverts vérifiés, (\d+) '
                         r'divergences', q)
        crash = 'oui' if not txt.strip() or not err else 'non'
        print('%-20s erreurs %s (mode4 %s), empreinte %s, vérificateur %s, '
              'tlbbtest inachevé : %s ; %s' % (
                  name, err[-1] if err else '-', m4[-1] if m4 else '-',
                  ('= ref' if fp and fp[-1] == REF else (fp[-1] if fp else '-')),
                  ('%s vérifiés / %s divergences' % div[-1]) if div else '-', crash,
                  nu[-1] if nu else 'tlbbnu : -'),
              flush=True)


if __name__ == '__main__':
    main()
