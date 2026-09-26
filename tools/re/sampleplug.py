#!/usr/bin/env python3
"""sampleplug.py SAMPLE.txt [N] — résume un `sample <pid>` pris DANS Tiger
(jeu + plugin GL) : fil le plus chargé (le fil principal du jeu), puis
  1. les postes du plugin (liste POSTES) : inclusif, part du fil, propre ;
  2. le propre (« self ») des N premières fonctions.
Inclusif : une fonction récursive n'est comptée qu'une fois par pile. Propre
d'un nœud : son compte moins la somme de ses enfants. Le self % localise, il
ne valide pas (docs/metrologie-boot.md)."""
import re
import sys
from collections import defaultdict

POSTES = [
    "my_glDrawElements", "gleDrawArraysOrElements_VBO_Exec", "gleExecuteVertexArrayRange",
    "gldUpdateDispatch", "pomppc_geom_dispatch", "geom_render_array", "geom_draw_client",
    "geom_ok", "geom_texture_ok", "geom_raster_ok", "accel_ok_for", "texture_ok",
    "texture_unit_ok", "texturing_on", "unit_textured", "texture_uploadable", "upload_texture",
    "tex_params_ok", "tex_prm_sig", "tex_lv0_sig", "intern_tex", "find_tex", "tex_cp",
    "tex_complete", "unit_mask", "unit_drvtex", "prog_state", "prog_domain_ok", "geom_format",
    "va_gen_sizes", "vd_key_of", "wl_take", "send_state", "compute_state", "compute_geom_state",
    "geom_send_all", "prog_sync", "sync_to_host", "geom_draw_native", "va_plan_build",
    "va_sources_ok", "va_scan_idx", "geom_dispatch_publish", "pomppc_lazy_update", "flush",
    "stats_frame", "pthread_mutex_lock", "pthread_mutex_unlock", "__pthread_self", "getenv",
    "draw_probe", "cube_probe",
]


def lit(path):
    lignes = open(path, errors="replace").read().split("\n")
    row = re.compile(r"^(?P<pre>[ +!:|]*)(?P<n>\d+) (?P<name>.+?)(?:  \(in [^)]+\))?"
                     r"(?: \+ [^\[]*)?(?: \[[^\]]*\])?\s*$")
    fils = []
    cur = None
    arbre = False
    for l in lignes:
        if l.startswith("Call graph:"):
            arbre = True
            continue
        if arbre and (l.startswith("Total number in stack") or l.startswith("Sort by top of stack")
                      or l.startswith("Binary Images")):
            arbre = False
        if not arbre or not l.strip():
            continue
        m = row.match(l)
        if not m:
            continue
        prof = len(m.group("pre"))
        nom = m.group("name").strip()
        if re.match(r"Thread_[0-9a-f]+", nom) and prof <= 4:
            cur = [nom, int(m.group("n")), []]
            fils.append(cur)
            continue
        if cur is not None:
            cur[2].append((prof, int(m.group("n")), nom))
    return fils


def resume(noeuds):
    incl = defaultdict(int)
    propre = defaultdict(int)
    pile = []

    def depile(jusqua):
        while pile and pile[-1][0] >= jusqua:
            p, f, n, s = pile.pop()
            propre[f] += n - s

    for prof, n, f in noeuds:
        depile(prof)
        if pile:
            pile[-1][3] += n
        if f not in [x[1] for x in pile]:
            incl[f] += n
        pile.append([prof, f, n, 0])
    depile(-1)
    return incl, propre


def propre_sous(noeuds, racine, prefixes):
    """Propre des fonctions dont le nom commence par un des `prefixes`, sous
    les nœuds `racine` (lot 5 : part de GLEngine sous glDrawElements)."""
    somme = defaultdict(int)
    pile = []                           # [prof, nom, n, enfants, sous_racine]

    def depile(jusqua):
        while pile and pile[-1][0] >= jusqua:
            p, f, n, s, sous = pile.pop()
            if sous and f.startswith(prefixes):
                somme[f] += n - s

    for prof, n, f in noeuds:
        depile(prof)
        sous = bool(pile) and (pile[-1][4] or pile[-1][1] == racine)
        if pile:
            pile[-1][3] += n
        pile.append([prof, f, n, 0, sous or f == racine])
    depile(-1)
    return somme


def main():
    args = sys.argv[1:]
    path = args[0]
    n_top = int(args[1]) if len(args) > 1 else 30
    fils = lit(path)
    if not fils:
        sys.exit("aucun fil dans %s" % path)
    nom, total, noeuds = max(fils, key=lambda t: t[1])
    incl, propre = resume(noeuds)
    print("fil %s : %d échantillons" % (nom, total))
    g = propre_sous(noeuds, "gleDrawArraysOrElements_VBO_Exec", ("gle", "_gle", "glDraw"))
    g = {f: n for f, n in g.items() if not f.startswith(("gld", "glr"))}
    print("GLEngine, propre sous gleDrawArraysOrElements_VBO_Exec : %d (%.1f %%) %s"
          % (sum(g.values()), 100.0 * sum(g.values()) / total,
             ", ".join("%s %d" % x for x in sorted(g.items(), key=lambda x: -x[1]))))
    print("\n%-36s %6s %6s %6s" % ("poste", "incl", "%", "propre"))
    for f in POSTES:
        if incl.get(f):
            print("%-36s %6d %5.1f%% %6d" % (f, incl[f], 100.0 * incl[f] / total, propre.get(f, 0)))
    print("\npropre, %d premières :" % n_top)
    for f, n in sorted(propre.items(), key=lambda x: -x[1])[:n_top]:
        print("%6d %5.1f%%  %s" % (n, 100.0 * n / total, f))


if __name__ == "__main__":
    main()
