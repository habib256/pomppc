#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""tlbbbanc.py DOSSIER… — bancs de tools/tcg/tlbbdev.sh (jobs tlbbtest:BANC, fptest:BANC
banc-l, fptest:BANC, t2bench ; docs/tcg-g4.md §41.5) : une ligne par démarrage (passes),
puis les médianes par bras (préfixe du nom du dossier jusqu'au dernier « - »)."""
import collections, glob, os, re, statistics, sys

COLS = ('tlbbtest', 'banc-l', 'sommets', 'chaîne', 'fcmpu', 'd3inter')


def lit(d):
    r = collections.defaultdict(list)
    for f in glob.glob(os.path.join(d, '*-tlbbtest/out/tlbbtest.txt')):
        r['tlbbtest'] += [int(x) for x in re.findall(r'appels : (\d+) ms', open(f).read())]
    for f in glob.glob(os.path.join(d, '*-fptest/out/fptest.txt')):
        t = open(f).read()
        r['banc-l'] += [int(x) for x in re.findall(r'banc-l : produits .* (\d+) ms', t)]
        r['sommets'] += [int(x) for x in re.findall(r'banc : sommets .* (\d+) ms', t)]
        r['chaîne'] += [int(x) for x in re.findall(r'banc : chaine .* (\d+) ms', t)]
        r['fcmpu'] += [int(x) for x in re.findall(r'banc : fcmpu .* (\d+) ms', t)]
    for f in glob.glob(os.path.join(d, '*-t2bench/out/*.txt')):
        r['d3inter'] += [float(x) for x in re.findall(r'passe \d+ rc=0 ([0-9.]+) s', open(f).read())]
    return r


def main():
    bras = collections.defaultdict(lambda: collections.defaultdict(list))
    print('| démarrage | ' + ' | '.join(COLS) + ' |')
    print('|---' * (len(COLS) + 1) + '|')
    for d in sys.argv[1:]:
        r = lit(d)
        nom = os.path.basename(d.rstrip('/'))
        b = nom.rsplit('-', 1)[0]
        for k in COLS:
            bras[b][k] += r[k]
        print('| %s | ' % nom + ' | '.join(' / '.join(('%g' % v) for v in r[k]) or '-' for k in COLS) + ' |')
    for b, r in bras.items():
        print('| médiane %s | ' % b + ' | '.join(('%g' % statistics.median(r[k])) if r[k] else '-'
                                              for k in COLS) + ' |')


if __name__ == '__main__':
    main()
