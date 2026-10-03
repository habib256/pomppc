#!/usr/bin/env python3
"""Compile UN fichier objet d'un arbre QEMU modifié avec la ligne de commande
d'un arbre déjà construit (même version, mêmes options), sans toucher à ce
dernier : les -I relatifs au répertoire de construction restent ceux de
l'arbre construit (en-têtes générés : config-*.h, qapi, trace, decodetree),
les -I vers les sources passent à l'arbre modifié.  Faible charge : un seul
cc1, nice 19.  Pour vérifier qu'un patch compile tant qu'un `make` complet
n'est pas permis (docs/tcg-g4.md §28).

  tools/tcg/cc1.py <arbre modifié> <arbre construit> <source relative> \
                   [<motif de la cible : ppc64-softmmu|ppc-softmmu|system>] [-o objet]
"""
import json, os, shlex, subprocess, sys

src_tree, built, rel = sys.argv[1:4]
want = sys.argv[4] if len(sys.argv) > 4 and not sys.argv[4].startswith('-') else 'ppc64-softmmu'
out = None
if '-o' in sys.argv:
    out = sys.argv[sys.argv.index('-o') + 1]
bdir = os.path.join(built, 'build')
cc = json.load(open(os.path.join(bdir, 'compile_commands.json')))
cands = [e for e in cc if os.path.normpath(os.path.join(e['directory'], e['file'])) ==
         os.path.normpath(os.path.join(built, rel))]
cands = [e for e in cands if want in e.get('output', e['command'])] or cands
if not cands:
    sys.exit('pas de ligne de compilation pour ' + rel)
e = cands[0]
args = shlex.split(e['command'])


def remap(p):
    """chemin d'en-têtes : sources de l'arbre construit -> arbre modifié"""
    full = os.path.normpath(p if os.path.isabs(p) else os.path.join(bdir, p))
    b, bd = os.path.normpath(built), os.path.normpath(bdir)
    if (full == b or full.startswith(b + os.sep)) and \
       not (full == bd or full.startswith(bd + os.sep)):
        return os.path.join(src_tree, os.path.relpath(full, b))
    return full

new = []
skip = False
for i, a in enumerate(args):
    if skip:
        skip = False
        continue
    if a in ('-o', '-MF', '-MQ'):
        skip = True
        continue
    if a.startswith('-MD') or a == '-MMD':
        continue
    for pre in ('-I', '-iquote'):
        if a.startswith(pre) and len(a) > len(pre):
            a = pre + remap(a[len(pre):])
            break
    else:
        if a == '-iquote' or a == '-I':
            pass
    if a.endswith(rel) and not a.startswith('-'):
        a = os.path.join(src_tree, rel)
    new.append(a)
# arguments séparés (-iquote X, -I X)
fixed = []
it = iter(range(len(new)))
i = 0
while i < len(new):
    a = new[i]
    if a in ('-iquote', '-I', '-isystem') and i + 1 < len(new):
        p = new[i + 1]
        p = remap(p)
        fixed += [a, p]
        i += 2
        continue
    fixed.append(a)
    i += 1
# les en-têtes de l'arbre modifié d'abord (include/, tcg/<hôte>/, host/include/…)
fixed += ['-o', out or '/dev/null']
cmd = ['nice', '-n', '19'] + fixed
r = subprocess.run(cmd, cwd=bdir)
sys.exit(r.returncode)
