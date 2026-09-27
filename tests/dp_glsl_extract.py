#!/usr/bin/env python3
"""Extrait des sources de DarkPlaces (Nexuiz 2.5.2) son shader GLSL intégré et
une liste de permutations, pour que tests/qgpu_core_test.c les fasse compiler
et lier par le backend GL de l'hôte (protocole v21, run_v21_dp).

    tests/dp_glsl_extract.py <enginesource.zip | gl_rmain.c> <dossier>

Écrit <dossier>/default.glsl (le texte de `builtinshaderstring`) et
<dossier>/permutations.txt : une ligne par programme, « V F|préambule » où V/F
disent si le programme a un shader de sommets / de fragments et où le
préambule (les #define que DarkPlaces place devant le texte, « \\n » littéraux)
est celui de R_GLSL_CompilePermutation : mode, puis bits de permutation.

Rien de DarkPlaces n'est versionné dans le dépôt : les sources sont dans
.run/jeux/Nexuiz/sources/ (hors git), et le test est ignoré sans elles.
"""
import re
import sys
import zipfile


def c_strings(body):
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    return ''.join(bytes(p, 'latin-1').decode('unicode_escape') for p in parts)


def table(src, name):
    i = src.index(name)
    j = src.index('};', i)
    return [c_strings(m) for m in re.findall(r'\{("(?:[^"\\]|\\.)*"|NULL)\s*,', src[i:j])]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    path, out = sys.argv[1], sys.argv[2]
    if path.endswith('.zip'):
        with zipfile.ZipFile(path) as z:
            src = z.read('darkplaces/gl_rmain.c').decode('latin-1')
    else:
        src = open(path, encoding='latin-1').read()
    i = src.index('static const char *builtinshaderstring =')
    body = c_strings(src[i:src.index(';\n', i)])
    open(out + '/default.glsl', 'w').write(body)

    # shaderpermutationinfo : {"#define …", " nom"}
    i = src.index('shaderpermutationinfo_t shaderpermutationinfo[SHADERPERMUTATION_COUNT] =')
    perm = [c_strings(m) for m in re.findall(r'\{\s*("(?:[^"\\]|\\.)*")\s*,', src[i:src.index('};', i)])]
    # shadermodeinfo : {vs, gs, fs, pretext, nom}
    i = src.index('shadermodeinfo_t shadermodeinfo[SHADERMODE_COUNT] =')
    modes = []
    for m in re.finditer(r'\{\s*("[^"]*"|NULL)\s*,\s*("[^"]*"|NULL)\s*,\s*("[^"]*"|NULL)\s*,\s*("(?:[^"\\]|\\.)*")',
                         src[i:src.index('};', i)]):
        modes.append((m.group(1) != 'NULL', m.group(3) != 'NULL', c_strings(m.group(4))))
    assert len(perm) == 18 and len(modes) == 13, (len(perm), len(modes))

    def pre(mode, bits):
        s = modes[mode][2]
        for k in range(len(perm)):
            s += perm[k] if bits & (1 << k) else '\n'
        return s

    # Les combinaisons que DarkPlaces choisit lui-même (R_SetupGenericShader,
    # R_SetupSurfaceShader, R_BlendView) — pas toutes les 2^18 : certaines ne
    # compilent nulle part (identificateurs non déclarés dans le texte), et
    # DarkPlaces ne les demande jamais. Bits : 0 diffuse, 1 vertextextureblend
    # / viewtint, 2 colormapping / saturation, 3 fog / gammaramps, 4 cubefilter,
    # 5 glow / bloom, 6 specular / postprocessing, 7 exactspecularmath,
    # 8 reflection, 9 offsetmapping, 10 reliefmapping, 11 shadowmaprect,
    # 12 shadowmapcube, 13 shadowmap2d, 14 pcf, 15 pcf2, 16 shadowsampler,
    # 17 vsdct. USEPOSTPROCESSING (r_glsl_postprocess, 0 par défaut) est
    # laissé de côté : son texte multiplie un int par un float, ce que GLSL
    # 1.10 refuse partout.
    import itertools
    D, VTB, CM, FOG, CUBE, GLOW, SPEC, EXACT, REFL, OFS, RELIEF = [1 << k for k in range(11)]
    SRECT, SCUBE, S2D, PCF, PCF2, SSAMP, VSDCT = [1 << k for k in range(11, 18)]

    def subsets(opts, most):
        for n in range(most + 1):
            for c in itertools.combinations(opts, n):
                yield sum(c)

    combos = set()
    GENERIC, POST, DEPTH, FLAT, VCOL, LMAP, DMODEL, DTAN, LDIR, LSRC, REFR, WATER, SHOW = range(13)
    combos |= {(GENERIC, 0), (GENERIC, D)}
    for extra in (0, CM, GLOW, VTB):
        for ex in (0, EXACT):
            combos.add((GENERIC, D | SPEC | extra | ex))
    combos |= {(DEPTH, 0), (SHOW, 0)}
    for b in subsets([VTB, CM, FOG, GLOW], 4):
        combos.add((POST, b))
    for ofs in (0, OFS | RELIEF):
        combos |= {(REFR, ofs), (WATER, ofs)}
        for b in subsets([VTB, GLOW, FOG, CM, REFL], 2):
            combos |= {(FLAT, b | ofs), (VCOL, b | ofs), (LMAP, b | ofs)}
            combos |= {(LDIR, b | ofs), (LDIR, b | ofs | D), (LDIR, b | ofs | D | SPEC)}
            for m in (DMODEL, DTAN):
                combos |= {(m, b | ofs | D), (m, b | ofs | D | SPEC), (m, b | ofs | D | SPEC | EXACT)}
        for b in subsets([VTB, CUBE, FOG, CM], 1):
            for sh in (0, SRECT, SRECT | PCF, SRECT | PCF2, S2D, S2D | SSAMP, SCUBE, SCUBE | VSDCT,
                       SRECT | VSDCT | PCF):
                combos |= {(LSRC, b | ofs | D | sh), (LSRC, b | ofs | D | SPEC | sh)}
    with open(out + '/permutations.txt', 'w') as f:
        for mode, bits in sorted(combos):
            v, fr, _ = modes[mode]
            f.write('%d %d|%s\n' % (v, fr, pre(mode, bits).replace('\n', '\\n')))
    print('%d octets de texte, %d permutations' % (len(body), len(combos)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
