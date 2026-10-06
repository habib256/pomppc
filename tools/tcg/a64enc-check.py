#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""a64enc-check.py ARBRE-QEMU [--cc CLANG] [--objdump OBJDUMP]

Relecture mécanique des encodages A64 « bruts » des émetteurs POMPPC de
tcg/aarch64/tcg-target.c.inc (tcg/0014, 0016, 0022, et ceux de 0037/0038,
docs/parite-arm64-0031-0035.md) : chaque constante #define PFP_*/PVF_* est
lue DANS L'ARBRE, complétée de registres concrets comme le font pfp_f3(),
pvf_3(), pvf_2()…, puis comparée au mot que produit l'assembleur de clang
pour l'instruction attendue (référence indépendante), et désassemblée.

Clang : celui du Mac (Xcode, `clang -arch arm64`) ou, sur le PC, celui du
NDK Android (`--target=aarch64-linux-android`, trouvé tout seul s'il est dans
~/android-sdk/ndk/*). Ne prouve que l'encodage, pas la sémantique.
Sortie : une ligne par instruction, « OK » ou « ÉCART », code 1 si un écart.
"""
import glob, os, re, shutil, subprocess, sys, tempfile

# (constante ou expression des #define, champs d, n, m, a, référence attendue)
# d/n/m : numéros de registre (V ou X/W selon l'instruction) posés comme le
# fait l'émetteur : d | n << 5 | m << 16 (| a << 10 pour fmadd).
CASES = [
    # --- tcg/0014/0016 (déjà prouvés sur le M4 : témoins de l'outil) ---
    ("PFP_FADDS", 3, 0, 1, None, "fadd s3, s0, s1"),
    ("PFP_FADDS | PFP_FTYPE_D", 3, 0, 1, None, "fadd d3, d0, d1"),
    ("PFP_FSUBS", 3, 0, 1, None, "fsub s3, s0, s1"),
    ("PFP_FMULS", 3, 0, 2, None, "fmul s3, s0, s2"),
    ("0x1e624000", 0, 0, None, None, "fcvt s0, d0"),          # pfp_fcvt_sd
    ("0x1e22c000", 3, 3, None, None, "fcvt d3, s3"),          # pfp_fcvt_ds
    ("0x9e670000", 0, 5, None, None, "fmov d0, x5"),          # pfp_fmov_dx
    ("0x9e660000", 5, 0, None, None, "fmov x5, d0"),          # pfp_fmov_xd
    # --- tcg/0037 (x-fp-native-cmp, NON compilé sur le PC) ---
    ("PFP_FDIVS", 3, 0, 1, None, "fdiv s3, s0, s1"),
    ("PFP_FDIVS | PFP_FTYPE_D", 3, 0, 1, None, "fdiv d3, d0, d1"),
    ("PFP_FCVTNS_XD", 5, 0, None, None, "fcvtns x5, d0"),
    ("PFP_FCVTZS_XD", 5, 0, None, None, "fcvtzs x5, d0"),
    ("0x93407c00", 7, 5, None, None, "sxtw x7, w5"),          # tcg_out_ext32s
    # --- tcg/0022 (témoins) ---
    ("PVF_ADD4S", 4, 0, 0, None, "add v4.4s, v0.4s, v0.4s"),
    ("PVF_CMHS4S", 16, 4, 6, None, "cmhs v16.4s, v4.4s, v6.4s"),
    ("PVF_CMHI4S", 5, 7, 4, None, "cmhi v5.4s, v7.4s, v4.4s"),
    ("PVF_AND16B", 16, 16, 17, None, "and v16.16b, v16.16b, v17.16b"),
    ("PVF_ORR16B", 3, 17, 18, None, "orr v3.16b, v17.16b, v18.16b"),
    ("PVF_FNEG4S", 4, 1, None, None, "fneg v4.4s, v1.4s"),
    ("PVF_CMEQ0_4S", 5, 4, None, None, "cmeq v5.4s, v4.4s, #0"),
    ("PVF_NOT16B", 16, 16, None, None, "mvn v16.16b, v16.16b"),
    ("PVF_UMAXV4S", 16, 16, None, None, "umaxv s16, v16.4s"),
    ("PVF_UMOVWS0", 9, 16, None, None, "mov w9, v16.s[0]"),
    # --- tcg/0038 (x-vfp-native-cmp, NON compilé sur le PC) ---
    ("PVF_FCMEQ4S", 3, 0, 1, None, "fcmeq v3.4s, v0.4s, v1.4s"),
    ("PVF_FCMGE4S", 3, 0, 1, None, "fcmge v3.4s, v0.4s, v1.4s"),
    ("PVF_FCMGT4S", 3, 0, 1, None, "fcmgt v3.4s, v0.4s, v1.4s"),
    ("PVF_FCMGT4S", 18, 6, 2, None, "fcmgt v18.4s, v6.4s, v2.4s"),
    ("PVF_FCMGE4S", 18, 6, 2, None, "fcmge v18.4s, v6.4s, v2.4s"),
    ("PVF_FMUL4S", 2, 1, 19, None, "fmul v2.4s, v1.4s, v19.4s"),
    ("PVF_SCVTF4S", 3, 1, None, None, "scvtf v3.4s, v1.4s"),
    ("PVF_UCVTF4S", 3, 1, None, None, "ucvtf v3.4s, v1.4s"),
    ("PVF_FCVTZS4S", 3, 2, None, None, "fcvtzs v3.4s, v2.4s"),
    ("PVF_FCVTZU4S", 3, 2, None, None, "fcvtzu v3.4s, v2.4s"),
]


def defines(src):
    d = {}
    for m in re.finditer(r'^#define\s+(P[FV][FP]_\w+)\s+(\(?0x[0-9a-fA-F]+u?l*l*\)?|\(1ull << \d+\))',
                         src, re.M):
        d[m.group(1)] = int(eval(re.sub(r'u?ll?\b', '', m.group(2))))
    return d


def find_tools(cc, od):
    if not cc:
        if sys.platform == 'darwin':
            cc, od = 'clang', 'objdump'
        else:
            ndk = sorted(glob.glob(os.path.expanduser(
                '~/android-sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin')))
            if ndk:
                cc = ndk[-1] + '/clang'
                od = od or ndk[-1] + '/llvm-objdump'
            else:
                cc = shutil.which('clang')
    od = od or shutil.which('llvm-objdump') or 'objdump'
    if not cc:
        sys.exit("aucun clang capable d'assembler de l'A64 (--cc)")
    return cc, od


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    tree, cc, od = args[0], None, None
    if '--cc' in args:
        cc = args[args.index('--cc') + 1]
    if '--objdump' in args:
        od = args[args.index('--objdump') + 1]
    cc, od = find_tools(cc, od)
    src = open(os.path.join(tree, 'tcg/aarch64/tcg-target.c.inc')).read()
    d = defines(src)
    words, missing = [], []
    for expr, rd, rn, rm, ra, ref in CASES:
        names = re.findall(r'P[FV][FP]_\w+', expr)
        if any(n not in d for n in names):
            missing.append(expr)
            words.append(None)
            continue
        w = eval(expr, {}, d)
        w |= rd | rn << 5
        if rm is not None:
            w |= rm << 16
        if ra is not None:
            w |= ra << 10
        words.append(w)
    tmp = tempfile.mkdtemp()
    s = os.path.join(tmp, 'ref.s')
    o = os.path.join(tmp, 'ref.o')
    with open(s, 'w') as f:
        f.write('.text\n')
        for c in CASES:
            f.write('    %s\n' % c[5])
        f.write('.data\n')
        for w in words:
            f.write('    .word 0x%08x\n' % (w or 0))
    tgt = ['-arch', 'arm64'] if sys.platform == 'darwin' else ['--target=aarch64-linux-android']
    subprocess.check_call([cc] + tgt + ['-c', s, '-o', o])
    # Mach-O (Mac) : section __text ; ELF (NDK) : .text.  Octets bruts : par
    # défaut chez le llvm-objdump du NDK, sur demande (--show-raw-insn) ailleurs.
    sec = '__text' if sys.platform == 'darwin' else '.text'
    for extra in ([], ['--show-raw-insn']):
        dis = subprocess.run([od, '-d'] + extra + ['-j', sec, o],
                             capture_output=True, text=True).stdout
        ref = [int(m.group(1), 16) for m in
               re.finditer(r'^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s', dis, re.M)]
        if len(ref) != len(CASES):
            # objdump d'Apple ou GNU : octets séparés « 20 18 20 1e »
            ref = []
            for m in re.finditer(r'^\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} ){4})', dis, re.M):
                b = bytes.fromhex(m.group(1).replace(' ', ''))
                ref.append(int.from_bytes(b, 'little'))
        if len(ref) == len(CASES):
            break
    if len(ref) != len(CASES):
        print(dis)
        sys.exit("désassemblage illisible : %d mots pour %d instructions" % (len(ref), len(CASES)))
    bad = 0
    for (expr, *_x, txt), w, r in zip(CASES, words, ref):
        if w is None:
            print('ABSENT  %-28s (constante introuvable dans l\'arbre)' % expr)
            bad += 1
            continue
        ok = w == r
        bad += not ok
        print('%-6s  %-26s %08x  réf %08x  %s' % ('OK' if ok else 'ÉCART', expr, w, r, txt))
    print('%d encodages relus, %d écarts' % (len(CASES), bad))
    shutil.rmtree(tmp)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
