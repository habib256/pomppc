#!/usr/bin/env python3
"""Ratés du cache de sauts d'une campagne matab en mode preuve (RETVERIFY=1, tcg/0042).

    jcmiss.py bench/tcg/ab/CAMPAGNE [...]

Lit la dernière ligne « ret-verify: » de chaque <mode>-<k>/run_tiger.log (cumul depuis le
démarrage) : recherches = vérifiés + ratés (boucle, helper, en ligne) ; ratés jusqu'à la table
de hachage = ratés de la boucle et du helper ; conflits = bloc trouvé ensuite alors que la case
tenait un autre bloc vivant.
"""
import re
import sys
from pathlib import Path

num = r"(\d+)"
for camp in sys.argv[1:]:
    print(f"## {camp}")
    print("| bras | cases/page | recherches | appels du helper | ratés jusqu'au qht | dont conflit | case vide | divergences | 2e voie |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for log in sorted(Path(camp).glob("*-*/run_tiger.log")):
        txt = log.read_text(errors="replace")
        lines = [l for l in txt.splitlines() if l.startswith("ret-verify: boucle")]
        if not lines:
            print(f"| {log.parent.name} | — | pas de rapport |"); continue
        l = lines[-1]
        src = dict((m[0], (int(m[1]), int(m[2]), int(m[3])))
                   for m in re.findall(r"(boucle|helper|en-ligne) (\d+) vérifiés / (\d+) ratés, (\d+) divergences", l))
        look = sum(h + m for h, m, _ in src.values())
        div = sum(d for *_, d in src.values())
        helper = src["helper"][0] + src["helper"][1]
        qht = src["boucle"][1] + src["helper"][1]
        per = re.search(r"entrées(?:, (\d+) par page)?\)", l)
        found = re.search(r"case vide, conflit, drapeaux\) : (\d+) / (\d+) / (\d+)", l)
        w2 = [x for x in txt.splitlines() if "seconde voie" in x]
        w2 = re.search(r"(\d+) trouvés", w2[-1]).group(1) if w2 else "—"
        pct = lambda x: f"{x/1e6:.1f} M ({100*x/look:.2f} %)"
        print(f"| {log.parent.name} | {per.group(1) or 64} | {look/1e9:.3f} G | {pct(helper)} | {pct(qht)} | "
              f"{int(found.group(2))/1e6:.1f} M | {int(found.group(1))/1e6:.1f} M | {div} | {int(w2)/1e6:.1f} M |")
