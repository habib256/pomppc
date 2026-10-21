#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""regions.py — phase 0 du traducteur de second niveau (docs/plan-traducteur-rapide.md §5) :
quelle part du temps vCPU tient dans les N régions les plus chaudes ?

Deux sources, combinables :

  * un profil perf de QEMU lancé avec -perfmap (matrice.py --sample-hote, perf record) :
    --perf PERF.DATA --map /tmp/perf-<pid>.map. La carte de QEMU a une ligne par
    instruction invité traduite (adresse hôte, taille, guest-0x<pc>) : elle donne le
    découpage en blocs, la taille du code hôte par instruction, et chaque échantillon
    des fils « CPU n/TCG » est rangé dans son bloc (chemins lents de fin de bloc compris).
  * le greffon tools/tcg/tbedges.c : --tb PREFIXE (lit PREFIXE.blocks.csv et
    PREFIXE.edges.csv ; --tb-base PREFIXE0 soustrait un instantané antérieur) :
    exécutions par bloc et enchaînements bloc → bloc.

Sans --tb : répartition par bloc et approximations (fenêtres alignées de 256 o / 1 Kio /
4 Kio, comme la première estimation du 21/10). Avec --tb : formation des régions
(graphe des enchaînements observés, boucles comprises, bornées : --max-insns,
--max-pages, --max-blocks), poids = temps perf du bloc si --perf, sinon instructions
exécutées ; parts dans les 50/200/1000 régions les plus chaudes, transitions de bloc
restées dans une région, sorties indirectes.

  regions.py --perf perf.data --map /tmp/perf-123.map
  regions.py --tb /tmp/e.2 --tb-base /tmp/e.1 [--perf … --map …] [--json sortie.json]
"""
import argparse
import bisect
import collections
import csv
import json
import re
import subprocess
import sys

PAGE = 4096

# --- carte perf de QEMU ----------------------------------------------------------------


class Tb:
    __slots__ = ("hstart", "hend", "pc", "n", "sizes")

    def __init__(self, hstart, pc):
        self.hstart, self.hend, self.pc, self.n, self.sizes = hstart, hstart, pc, 0, []


def load_map(path):
    """Blocs de la carte -perfmap, dans l'ordre d'écriture. Nouveau bloc quand
    l'adresse hôte ou l'adresse invité n'enchaînent pas sur la ligne précédente."""
    tbs, cur = [], None
    with open(path, "rb") as f:
        for line in f:
            p = line.split()
            if len(p) != 3 or not p[2].startswith(b"guest-0x"):
                cur = None
                continue
            h, sz, g = int(p[0], 16), int(p[1], 16), int(p[2][8:], 16)
            if cur is None or h != cur.hend or g != cur.pc + 4 * cur.n:
                cur = Tb(h, g)
                tbs.append(cur)
            cur.n += 1
            cur.sizes.append(sz)
            cur.hend = h + sz
    return tbs


class HostIndex:
    """Adresse hôte → bloc. Le dernier écrit gagne (tampon réutilisé après tb_flush)."""

    def __init__(self, tbs):
        last = {}
        for t in tbs:
            last[t.hstart] = t
        self.starts = sorted(last)
        self.tbs = [last[s] for s in self.starts]
        self.collisions = len(tbs) - len(last)

    def find(self, ip):
        i = bisect.bisect_right(self.starts, ip) - 1
        if i < 0:
            return None
        t = self.tbs[i]
        nxt = self.starts[i + 1] if i + 1 < len(self.starts) else t.hend + 4096
        # chemins lents et sorties après la dernière instruction : jusqu'au bloc suivant
        if ip < nxt and ip - t.hstart < 65536:
            return t
        return None


LIGNE = re.compile(r"^\s*(?P<comm>.+?)\s+(?P<ip>[0-9a-f]+)\s+\((?P<dso>[^)]*)\)\s*$")


def load_perf(data, hidx, mapname):
    """Échantillons des fils vCPU : (blocs → échantillons, total vCPU, hors JIT par dso)."""
    per_tb = collections.Counter()
    halves = (collections.Counter(), collections.Counter())
    total = jit = unresolved = 0
    p = subprocess.Popen(["perf", "script", "-i", data, "-F", "comm,ip,dso"],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                         errors="replace")
    for l in p.stdout:
        m = LIGNE.match(l)
        if not m or "/TCG" not in m.group("comm"):
            continue
        total += 1
        dso = m.group("dso")
        ip = int(m.group("ip"), 16)
        if dso.startswith("/tmp/perf-") or dso == "[unknown]" or dso == mapname:
            t = hidx.find(ip)
            if t is None:
                if dso.startswith("/tmp/perf-"):
                    unresolved += 1
                continue
            jit += 1
            per_tb[(t.pc, t.n)] += 1
            halves[jit & 1][(t.pc, t.n)] += 1
    p.wait()
    load_perf.halves = halves
    return per_tb, total, jit, unresolved


def split_half(halves, key, ks, vtotal):
    """Part des K plus chauds SANS le biais d'échantillonnage : classement sur une moitié
    des échantillons (un sur deux), part mesurée sur l'autre, et inversement."""
    res = {}
    for k in ks:
        s = 0.0
        for a, b in ((0, 1), (1, 0)):
            agg_a, agg_b = collections.Counter(), collections.Counter()
            for x, c in halves[a].items():
                agg_a[key(x)] += c
            for x, c in halves[b].items():
                agg_b[key(x)] += c
            top = [x for x, _ in agg_a.most_common(k)]
            s += sum(agg_b[x] for x in top) / max(sum(agg_b.values()), 1)
        res[k] = s / 2
    return res


# --- greffon tbedges -------------------------------------------------------------------


def read_tb(prefix):
    blocks = {}
    with open(prefix + ".blocks.csv") as f:
        rows = (l for l in f if not l.startswith("#"))
        for r in csv.DictReader(rows):
            blocks[int(r["slot"])] = dict(pc=int(r["pc"], 16), phys=int(r["phys"], 16),
                                          n=int(r["insns"]), lastw=int(r["lastw"], 16),
                                          execs=int(r["execs"]))
    edges = collections.Counter()
    with open(prefix + ".edges.csv") as f:
        for r in csv.DictReader(f):
            edges[(int(r["src"]), int(r["dst"]))] += int(r["count"])
    return blocks, edges


def ring_time(prefix):
    with open(prefix + ".blocks.csv") as f:
        m = re.search(r"t_wall=([0-9.]+)", f.readline())
    return float(m.group(1)) if m else 0.0


def pick_ring(prefix, t0, t1):
    import glob
    snaps = sorted((ring_time(p[:-len(".blocks.csv")]), p[:-len(".blocks.csv")])
                   for p in glob.glob(prefix + ".r*.blocks.csv"))
    if len(snaps) < 2:
        sys.exit("moins de deux instantanés %s.r*" % prefix)
    if t0 is None or t1 is None:
        return snaps[-1][1], snaps[-2][1]
    base = [p for t, p in snaps if t <= t0]
    end = [p for t, p in snaps if t >= t1]
    return (end[0] if end else snaps[-1][1]), (base[-1] if base else snaps[0][1])


def exit_kind(w):
    op = w >> 26
    if op == 19:
        xo = (w >> 1) & 0x3ff
        if xo == 16:
            return "bclr"
        if xo == 528:
            return "bcctr"
        if xo in (50, 18):
            return "rfi"
        if xo == 150:
            return "isync"
        return "cr"
    if op == 17:
        return "sc"
    if op in (16, 18):
        return "direct"
    return "fallthrough"   # bloc coupé (page, taille, instruction qui termine le bloc)


# --- formation des régions -------------------------------------------------------------


def form_regions(blocks, edges, weight, a):
    """Graines par poids décroissant ; croissance en largeur le long des arêtes
    fréquentes (≥ a.min_edge de la sortie du bloc, cible ≥ a.min_rel de la graine),
    sans voler un bloc déjà pris, bornée en instructions, pages et blocs."""
    succ = collections.defaultdict(list)
    out_tot = collections.Counter()
    for (s, d), c in edges.items():
        succ[s].append((c, d))
        out_tot[s] += c
    for s in succ:
        succ[s].sort(reverse=True)
    owner = {}
    regions = []
    order = sorted(blocks, key=lambda b: -weight.get(b, 0))
    for seed in order:
        if seed in owner or weight.get(seed, 0) <= 0:
            continue
        sexe = max(blocks[seed]["execs"], 1)
        reg = [seed]
        owner[seed] = len(regions)
        insns = blocks[seed]["n"]
        pages = {blocks[seed]["pc"] // PAGE, (blocks[seed]["pc"] + 4 * blocks[seed]["n"] - 1) // PAGE}
        q = collections.deque([seed])
        full = False
        while q:
            b = q.popleft()
            tot = out_tot[b] or 1
            for c, d in succ[b]:
                if d in owner or d not in blocks:
                    continue
                if c < a.min_edge * tot or blocks[d]["execs"] < a.min_rel * sexe:
                    continue
                bd = blocks[d]
                np_ = pages | {bd["pc"] // PAGE, (bd["pc"] + 4 * bd["n"] - 1) // PAGE}
                if (insns + bd["n"] > a.max_insns or len(np_) > a.max_pages
                        or len(reg) >= a.max_blocks):
                    full = True
                    continue
                owner[d] = len(regions)
                reg.append(d)
                insns += bd["n"]
                pages = np_
                q.append(d)
        regions.append(dict(blocks=reg, insns=insns, pages=len(pages), full=full,
                            w=sum(weight.get(x, 0) for x in reg)))
    return regions, owner


def report_regions(blocks, edges, weight, wtotal, label, a, out):
    regions, owner = form_regions(blocks, edges, weight, a)
    regions.sort(key=lambda r: -r["w"])
    trans = sum(edges.values()) or 1
    res = {"poids": label, "regions": len(regions)}
    print("\nRégions (poids : %s ; bornes %d instr., %d pages, %d blocs ; arête ≥ %.0f %%, "
          "cible ≥ %.1f %% de la graine)" % (label, a.max_insns, a.max_pages, a.max_blocks,
                                            100 * a.min_edge, 100 * a.min_rel))
    print("| N régions | part du poids total | blocs | instr. moy. | pages moy. | bornées |"
          " transitions internes / toutes |")
    print("|---|---|---|---|---|---|---|")
    for k in (50, 200, 1000, 5000):
        top = regions[:k]
        if not top:
            continue
        ids = {i for i, r in enumerate(regions[:k])}
        rid = {}
        for i, r in enumerate(top):
            for b in r["blocks"]:
                rid[b] = i
        internal = sum(c for (s, d), c in edges.items() if s in rid and rid.get(d) == rid[s])
        w = sum(r["w"] for r in top)
        nb = sum(len(r["blocks"]) for r in top)
        print("| %d | %.1f %% | %d | %.0f | %.2f | %d | %.1f %% |" % (
            k, 100 * w / wtotal, nb, sum(r["insns"] for r in top) / len(top),
            sum(r["pages"] for r in top) / len(top), sum(r["full"] for r in top),
            100 * internal / trans))
        res["top%d" % k] = dict(part=w / wtotal, blocs=nb, internes=internal / trans)
    out.setdefault("regions", []).append(res)
    return regions


# --- main ------------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--perf")
    ap.add_argument("--map")
    ap.add_argument("--tb")
    ap.add_argument("--tb-base")
    ap.add_argument("--ring", help="préfixe des instantanés en anneau (interval=) : "
                    "prend les deux qui encadrent --t0/--t1 (secondes epoch), sinon les deux derniers")
    ap.add_argument("--t0", type=float)
    ap.add_argument("--t1", type=float)
    ap.add_argument("--max-insns", type=int, default=1024)
    ap.add_argument("--max-pages", type=int, default=4)
    ap.add_argument("--max-blocks", type=int, default=128)
    ap.add_argument("--min-edge", type=float, default=0.05)
    ap.add_argument("--min-rel", type=float, default=0.001)
    ap.add_argument("--json")
    a = ap.parse_args()
    out = {}

    per_tb, vtotal, mapinfo = None, None, {}
    if a.perf:
        if not a.map:
            sys.exit("--perf demande --map /tmp/perf-<pid>.map")
        tbs = load_map(a.map)
        hidx = HostIndex(tbs)
        per_tb, vtotal, jit, unres = load_perf(a.perf, hidx, a.map)
        print("carte : %d traductions (%d adresses hôte réutilisées)" % (len(tbs), hidx.collisions))
        print("échantillons vCPU %d, dans le code généré %d (%.1f %%), non résolus %d" % (
            vtotal, jit, 100 * jit / max(vtotal, 1), unres))
        out.update(vcpu=vtotal, jit=jit)
        # taille du code hôte par instruction invité, pondérée par le temps
        size = {}
        for t in hidx.tbs:
            size[(t.pc, t.n)] = sum(t.sizes)
        wb = sum(per_tb[k] * size.get(k, 0) / k[1] for k in per_tb)
        wl = sum(per_tb[k] * k[1] for k in per_tb)
        print("code hôte par instruction invité (pondéré par le temps) : %.1f octets ; "
              "bloc moyen pondéré : %.1f instructions" % (wb / max(jit, 1), wl / max(jit, 1)))
        out.update(octets_par_insn=wb / max(jit, 1), bloc_moyen=wl / max(jit, 1))
        ranked = sorted(per_tb.values(), reverse=True)
        ks = (50, 200, 1000, 5000, 20000)
        sh = split_half(load_perf.halves, lambda x: x, ks, vtotal)
        jf = jit / vtotal
        print("\n| N blocs les plus chauds | part du temps vCPU | sans biais (moitiés croisées) |")
        print("|---|---|---|")
        for k in ks:
            s = sum(ranked[:k])
            print("| %d | %.1f %% | %.1f %% |" % (k, 100 * s / vtotal, 100 * sh[k] * jf))
            out["tb_top%d" % k] = s / vtotal
            out["tb_top%d_sb" % k] = sh[k] * jf
        print("(%d blocs distincts échantillonnés)" % len(ranked))
        for win in (256, 1024, 4096):
            agg = collections.Counter()
            for (pc, n), c in per_tb.items():
                agg[pc // win] += c
            r = sorted(agg.values(), reverse=True)
            shw = split_half(load_perf.halves, lambda x, w=win: x[0] // w, (50, 200, 1000), vtotal)
            print("fenêtres de %d o : 50 → %.1f %%, 200 → %.1f %%, 1000 → %.1f %% du vCPU "
                  "(sans biais : %.1f / %.1f / %.1f %%)" % (
                      win, 100 * sum(r[:50]) / vtotal, 100 * sum(r[:200]) / vtotal,
                      100 * sum(r[:1000]) / vtotal, 100 * shw[50] * jf, 100 * shw[200] * jf,
                      100 * shw[1000] * jf))
            out["fen%d_top1000" % win] = sum(r[:1000]) / vtotal

    if a.ring:
        a.tb, a.tb_base = pick_ring(a.ring, a.t0, a.t1)
        print("instantanés : %s − %s" % (a.tb, a.tb_base))
    if a.tb:
        blocks, edges = read_tb(a.tb)
        if a.tb_base:
            b0, e0 = read_tb(a.tb_base)
            for s, b in blocks.items():
                if s in b0:
                    b["execs"] -= b0[s]["execs"]
            for k, c in e0.items():
                edges[k] -= c
            edges = collections.Counter({k: c for k, c in edges.items() if c > 0})
        blocks = {s: b for s, b in blocks.items() if b["execs"] > 0}
        tot_insn = sum(b["execs"] * b["n"] for b in blocks.values())
        tot_exec = sum(b["execs"] for b in blocks.values())
        print("\ngreffon : %d blocs exécutés, %.3g exécutions, %.3g instructions, %d arêtes, "
              "%.2f instructions par bloc exécuté" % (len(blocks), tot_exec, tot_insn, len(edges),
                                                    tot_insn / max(tot_exec, 1)))
        kinds = collections.Counter()
        for (s, d), c in edges.items():
            if s in blocks:
                kinds[exit_kind(blocks[s]["lastw"])] += c
        tk = sum(kinds.values()) or 1
        print("transitions par sortie du bloc source : " + ", ".join(
            "%s %.1f %%" % (k, 100 * v / tk) for k, v in kinds.most_common()))
        out.update(blocs=len(blocks), transitions={k: v / tk for k, v in kinds.items()})
        # cibles indirectes : nombre de cibles distinctes par site bclr/bcctr
        nt = collections.defaultdict(collections.Counter)
        for (s, d), c in edges.items():
            if s in blocks and exit_kind(blocks[s]["lastw"]) in ("bclr", "bcctr"):
                nt[s][d] += c
        cover = {1: 0, 2: 0, 3: 0}
        itot = sum(sum(v.values()) for v in nt.values()) or 1
        for s, v in nt.items():
            r = sorted(v.values(), reverse=True)
            for k in cover:
                cover[k] += sum(r[:k])
        print("sorties indirectes prédites par les 1/2/3 cibles les plus vues du site : "
              + " / ".join("%.1f %%" % (100 * cover[k] / itot) for k in (1, 2, 3)))
        out["indirect_pred"] = {k: cover[k] / itot for k in cover}
        # poids
        winsn = {s: b["execs"] * b["n"] for s, b in blocks.items()}
        report_regions(blocks, edges, winsn, tot_insn, "instructions exécutées", a, out)
        if per_tb is not None:
            # temps perf → blocs du greffon, par (pc, longueur) puis par pc seul,
            # réparti entre homonymes au prorata des instructions exécutées
            by_key = collections.defaultdict(list)
            by_pc = collections.defaultdict(list)
            for s, b in blocks.items():
                by_key[(b["pc"], b["n"])].append(s)
                by_pc[b["pc"]].append(s)
            wt = collections.Counter()
            lost = 0
            for (pc, n), c in per_tb.items():
                cand = by_key.get((pc, n)) or by_pc.get(pc)
                if not cand:
                    lost += c
                    continue
                z = sum(winsn[x] for x in cand) or len(cand)
                for x in cand:
                    wt[x] += c * (winsn[x] or 1) / z
            print("\ntemps perf rattaché aux blocs du greffon : %.1f %% du code généré "
                  "(le reste : blocs jamais vus par le greffon)" % (
                      100 * (1 - lost / max(sum(per_tb.values()), 1))))
            report_regions(blocks, edges, wt, vtotal, "temps vCPU (perf, part de TOUT le vCPU)", a, out)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()
