#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""lfsk-mut.py ARBRE-USER [MUTANT…] — contre-épreuves de patches/tcg/0051 (x-lfs-known,
docs/tcg-g4.md §40) sur le banc linux-user.

ARBRE-USER : la copie linux-user de l'arbre (tools/tcg/fpnatcmp-userhack.py), commitée,
construite dans build-user/. Pour chaque mutant : une substitution dans un fichier source,
`ninja qemu-ppc`, le binaire rangé dans build-user/mut-<nom>, le fichier rendu par
`git checkout`. Puis, bras lfsk (tools/tcg/lfskuser.sh « un ») :
  - fptest l sur les quatre plages des cas spéciaux (zéros et dénormaux des deux signes,
    infinis et NaN des deux signes, avec les normaux voisins) : écarts comptés, et (sous
    x-fp-verify, bilan final du banc linux-user) mots hors ligne et sorties de bloc contre
    l'attendu exact (4 et 3 par mot anormal : le classement du vrai émetteur) ;
  - fptest k 4096 (graines 0 et 7) contre l'empreinte du binaire non muté, et sous
    x-fp-verify : divergences du vérificateur.
Sortie : une ligne par mutant. Variables : OUT (dossier, défaut $TMPDIR/lfskmut), NP (tranches
parallèles du mode l, défaut 8).
"""
import os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.expanduser(sys.argv[1])
OUT = os.environ.get('OUT', os.path.join(os.environ.get('TMPDIR', '/tmp'), 'lfskmut'))
NP = os.environ.get('NP', '8')
X86 = 'tcg/x86_64/tcg-target.c.inc'
FPU = 'target/ppc/fpu_helper.c'
TR = 'target/ppc/translate/fp-impl.c.inc'

MUTANTS = {
    # test de classement de l'émetteur x86_64 retiré : tout mot converti par l'hôte
    'sans-test': (X86, "tgen_arithi(s, ARITH_CMP, t, (int32_t)0xfe000000, 0);",
                  "tgen_arithi(s, ARITH_CMP, t, (int32_t)0xffffffff, 0);"),
    # les infinis acceptés (borne <= au lieu de <)
    'infinis': (X86, "ok = pfp_jshort(s, JCC_JB);", "ok = pfp_jshort(s, JCC_JBE);"),
    # les dénormaux acceptés (second test : t >= 0xff000000 au lieu de t == 0xff000000)
    'denormaux': (X86, """        tgen_arithi(s, ARITH_CMP, t, (int32_t)0xff000000, 0);
        pfp_jslow(s, l, JCC_JNE);""", """        tgen_arithi(s, ARITH_CMP, t, (int32_t)0xff000000, 0);
        pfp_jslow(s, l, JCC_JB);"""),
    # décalage faux dans le lea (exposant décalé d'un cran)
    'lea': (X86, "OPC_LEA, t, -1, xa, 1, -0x01000000", "OPC_LEA, t, -1, xa, 1, -0x00800000"),
    # le zéro part au talon (attendu équivalent : DOUBLE(0) est fpi_zon, pas de sortie)
    'zero-talon': (X86, """        tgen_arithi(s, ARITH_CMP, t, (int32_t)0xff000000, 0);
        pfp_jslow(s, l, JCC_JNE);""", """        tgen_arithi(s, ARITH_CMP, t, (int32_t)0xff000000, 0);
        pfp_jslow(s, l, JCC_JMP);"""),
    # le talon ne sort jamais du bloc
    'sans-sortie': (FPU, "if ((sel & FPK_R) && !fpi_zon(r)) {\n        fpk_exit(env, sel, r, env->fpscr, ra);",
                    "if (0) {\n        fpk_exit(env, sel, r, env->fpscr, ra);"),
    # le talon sort toujours (attendu équivalent pour les résultats ; le compteur le voit)
    'sortie-toujours': (FPU, "if ((sel & FPK_R) && !fpi_zon(r)) {\n        fpk_exit(env, sel, r, env->fpscr, ra);",
                        "if (!fpi_zon(r)) {\n        fpk_exit(env, sel, r, env->fpscr, ra);"),
    # la sortie de bloc n'écrit pas frT (talon : r = 0 rangé)
    'sortie-sans-frt': (FPU, "fpk_exit(env, sel, r, env->fpscr, ra);\n    }\n    return int128_make128(r, env->fpscr);",
                        "fpk_exit(env, sel, *cpu_fpr_ptr(env, (sel >> 8) & 0xff), env->fpscr, ra);\n    }\n    return int128_make128(r, env->fpscr);"),
    # le talon convertit par l'hôte (le sNaN est calmé)
    'talon-hote': (FPU, "uint64_t r = helper_todouble((uint32_t)w);\n\n    qatomic_inc(&lfsk_slow);",
                   "uint64_t r;\n    float rf;\n    double rd;\n    uint32_t rw = w;\n    memcpy(&rf, &rw, 4);\n    rd = rf;\n    memcpy(&r, &rd, 8);\n\n    qatomic_inc(&lfsk_slow);"),
    # forme mise à jour : rA écrit après l'op (la sortie de bloc le perd)
    'maj-apres': (TR, """    if (update) {
        tcg_gen_mov_tl(cpu_gpr[ra], ea);
    }
    tcg_gen_ppc_fp32(r, nf, w, w, w, w, FPI_LFS | rt << 8,
                     ppc_fp32_native_slow, 0, 0);
    top = tcg_last_op();""", """    tcg_gen_ppc_fp32(r, nf, w, w, w, w, FPI_LFS | rt << 8,
                     ppc_fp32_native_slow, 0, 0);
    top = tcg_last_op();
    if (update) {
        tcg_gen_mov_tl(cpu_gpr[ra], ea);
    }"""),
    # le consommateur ne pose pas FPK_R sur le lfs : son producteur noté est l'op précédente
    # du même lfs... impossible sans corrompre ; à la place : lfs ne note pas de producteur et
    # frT reste connu de l'instruction d'avant (fpk_prod[rt] inchangé)
    'prod-perdu': (TR, "    ctx->fpk_prod[rt] = top;\n    ctx->fpk_handled = true;",
                   "    if (!((ctx->fpk_valid >> rt) & 1) || !ctx->fpk_prod[rt]) {\n        ctx->fpk_prod[rt] = top;\n    }\n    ctx->fpk_handled = true;"),
    # lfs producteur sans être connu (fpk_valid non posé) : attendu équivalent (vitesse seule)
    'pas-producteur': (TR, "    ctx->fpk_valid |= 1u << rt;\n    ctx->fpk_prod[rt] = top;",
                       "    ctx->fpk_valid &= ~(1u << rt);\n    ctx->fpk_prod[rt] = top;"),
}


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build(name):
    b = os.path.join(TREE, 'build-user')
    r = run(['nice', '-n', '19', 'ninja', '-C', b, 'qemu-ppc'])
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:])
        sys.exit('construction %s : échec' % name)
    dst = os.path.join(b, 'mut-' + name)
    shutil.copy2(os.path.join(b, 'qemu-ppc'), dst)
    return dst


def apply(m):
    f, old, new = MUTANTS[m]
    p = os.path.join(TREE, f)
    s = open(p).read()
    if s.count(old) != 1:
        sys.exit('mutant %s : ancre trouvée %d fois dans %s' % (m, s.count(old), f))
    open(p, 'w').write(s.replace(old, new))
    return f


# les 33 554 430 mots anormaux (dénormaux et infinis, NaN des deux signes) et leurs voisins
NANORM = 33554430
RANGES = [('0', '1000000'), ('7f000000', '80000000'), ('80000000', '81000000'),
          ('ff000000', '100000000')]


def test(q, tag):
    env = dict(os.environ, OUT=os.path.join(OUT, tag))
    os.makedirs(env['OUT'], exist_ok=True)
    ecarts, hl, so, div = 0, 0, 0, 0
    envl = dict(env, OPTS='x-fp-verify=on')
    for lo, hi in RANGES:
        r = run(['nice', '-n', '19', 'bash', os.path.join(HERE, 'lfskuser.sh'), q, 'un', 'l', lo, hi],
                env=envl, timeout=3600)
        m = re.search(r'(\d+) ecarts', r.stdout)
        ecarts += int(m.group(1)) if m else 10**9
        div += r.stderr.count('DIVERGENCE')
        # bilan final (preexit_cleanup du banc linux-user) : le classement exact du vrai
        # émetteur — chaque mot anormal hors ligne quatre fois, trois sorties de bloc
        f = [l for l in r.stderr.splitlines() if l.startswith('FINAL') and 'x-lfs-known' in l]
        m = re.search(r'x-lfs-known : (\d+) mots hors ligne, (\d+) sorties', f[-1]) if f else None
        hl += int(m.group(1)) if m else 10**12
        so += int(m.group(2)) if m else 10**12
    ks = []
    for seed in ('0', '7'):
        r = run(['nice', '-n', '19', 'bash', os.path.join(HERE, 'lfskuser.sh'), q, 'un', 'k', '4096', seed],
                env=env, timeout=3600)
        ks.append(r.stdout)
        env2 = dict(env, OPTS='x-fp-verify=on,x-fp-native-cmp-verify=on')
        r = run(['nice', '-n', '19', 'bash', os.path.join(HERE, 'lfskuser.sh'), q, 'un', 'k', '4096', seed],
                env=env2, timeout=3600)
        div += r.stderr.count('DIVERGENCE')
        last = [l for l in r.stderr.splitlines() if 'x-lfs-known' in l]
        if last:
            m = re.search(r'x-lfs-known : (\d+) mots hors ligne, (\d+) sorties', last[-1])
            ks.append('lfsk %s/%s' % m.groups() if m else '')
    return ecarts, ks, div, (hl, so)


def main():
    os.makedirs(OUT, exist_ok=True)
    names = sys.argv[2:] or list(MUTANTS)
    ref_q = os.path.join(TREE, 'build-user', 'mut-aucun')
    if not os.path.exists(ref_q):
        build('aucun')
    e0, k0, d0, c0 = test(ref_q, 'aucun')
    print('aucun              : mode l %d écarts, hors ligne/sorties %d/%d (attendu %d/%d) ; k : %s ; '
          'vérificateur %d divergences' % (e0, c0[0], c0[1], 4 * NANORM, 3 * NANORM,
          ' | '.join(x.strip().splitlines()[-1] if x.strip() else '' for x in k0), d0), flush=True)
    for m in names:
        f = apply(m)
        try:
            q = build(m)
        finally:
            run(['git', '-C', TREE, 'checkout', '--', f])
        e, k, d, c = test(q, m)
        kdiff = sum(1 for a, b in zip(k0[0::2], k[0::2])
                    for x, y in zip(a.splitlines(), b.splitlines()) if x != y)
        cnt = c != (4 * NANORM, 3 * NANORM)
        caught = e > 0 or kdiff > 0 or d > 0
        print('%-18s : mode l %d écarts, hors ligne/sorties %d/%d ; k : %d lignes d\'empreinte '
              'différentes ; vérificateur %d divergences ; k %s -> %s' %
              (m, e, c[0], c[1], kdiff, d, ' '.join(k[1::2]),
               'ATTRAPÉ' if caught else 'vu au compteur seul' if cnt else 'non attrapé'), flush=True)
    build('aucun')


if __name__ == '__main__':
    main()
