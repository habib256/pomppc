#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""sampleplug.py SAMPLE.txt [SAMPLE2.txt …] [N] [--ms X] [--enfants F,G…] — résume un
ou plusieurs `sample <pid>` pris DANS Tiger (jeu + plugin GL) : fil le plus chargé
(le fil principal du jeu : le plus chargé de ceux qui passent par le plugin) de chaque relevé, les relevés AGRÉGÉS (sommes
d'échantillons), puis
  1. les postes du plugin (liste POSTES) : inclusif, part du fil, propre ;
     avec --ms X (ms/image de la scène), la part convertie en ms/image
     (part × X : le fil principal est celui qui fait l'image) ;
  2. les groupes (GROUPES : textures, transport…), inclusifs, sans double compte ;
  3. --enfants F : les enfants directs de F (inclusifs) ;
  4. le propre (« self ») des N premières fonctions.
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
    "upload_bands", "convert_level", "upload_surftex", "tex_complete",
    "tex_params_ok", "tex_prm_sig", "tex_lv0_sig", "intern_tex", "find_tex", "tex_cp",
    "unit_mask", "unit_drvtex", "prog_state", "prog_domain_ok", "geom_format",
    "va_gen_sizes", "vd_key_of", "wl_take", "send_state", "compute_state", "compute_geom_state",
    "compute_v8_state", "state_units", "state_progs", "send_polygon_stipple",
    "geom_send_all", "geom_send_matrices", "geom_send_lights", "geom_send_texgen",
    "prog_sync", "sync_to_host", "geom_draw_native", "va_plan_build",
    "va_sources_ok", "va_scan_idx", "geom_dispatch_publish", "pomppc_lazy_update",
    "reserve", "flush", "submit_cur", "arena_alloc", "pomppc_qgpu_submit", "pomppc_qgpu_kick",
    "IOConnectCallScalarMethod", "io_connect_method_scalarI_scalarO",
    "stats_frame", "pthread_mutex_lock", "pthread_mutex_unlock", "__pthread_self", "getenv",
    "draw_probe", "cube_probe", "__memcpy", "memcpy", "__bzero", "memset",
]

POSTES_SET = set(POSTES) - {"pthread_mutex_lock", "pthread_mutex_unlock", "__pthread_self",
                            "getenv", "__memcpy", "memcpy", "__bzero", "memset",
                            "IOConnectCallScalarMethod"}

# Groupes : un échantillon compte une fois s'il passe par l'une des fonctions.
GROUPES = {
    "textures (téléversement : upload_texture, upload_bands, upload_surftex, tex_cp)":
        ("upload_texture", "upload_bands", "upload_surftex", "tex_cp", "convert_level"),
    "état (send_state + geom_send_all)": ("send_state", "geom_send_all"),
    "transport (flush, soumission)": ("flush", "submit_cur", "pomppc_qgpu_submit",
                                      "pomppc_qgpu_kick", "IOConnectCallScalarMethod",
                                      "io_connect_method_scalarI_scalarO"),
}


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


def resume(noeuds, incl, propre, groupes, enfants, suivis):
    """Accumule dans les dictionnaires passés (plusieurs relevés s'additionnent)."""
    pile = []

    def depile(jusqua):
        while pile and pile[-1][0] >= jusqua:
            p, f, n, s = pile.pop()
            propre[f] += n - s

    for prof, n, f in noeuds:
        depile(prof)
        if pile:
            pile[-1][3] += n
            if pile[-1][1] in suivis:
                enfants[pile[-1][1]][f] += n
        noms = [x[1] for x in pile]
        if f not in noms:
            incl[f] += n
        for g, membres in GROUPES.items():
            if f in membres and not any(x in membres for x in noms):
                groupes[g] += n
        pile.append([prof, f, n, 0])
    depile(-1)


def propre_sous(noeuds, racine, prefixes, somme):
    """Propre des fonctions dont le nom commence par un des `prefixes`, sous
    les nœuds `racine` (lot 5 : part de GLEngine sous glDrawElements)."""
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


def main():
    args = sys.argv[1:]
    ms = None
    suivis = []
    fichiers = []
    n_top = 30
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--ms":
            ms = float(args[i + 1].replace(",", "."))
            i += 2
            continue
        if a == "--enfants":
            suivis += args[i + 1].split(",")
            i += 2
            continue
        if a.isdigit():
            n_top = int(a)
        else:
            fichiers.append(a)
        i += 1
    if not fichiers:
        sys.exit(__doc__)
    incl, propre, groupes = defaultdict(int), defaultdict(int), defaultdict(int)
    enfants = defaultdict(lambda: defaultdict(int))
    g = defaultdict(int)
    total = 0
    for path in fichiers:
        fils = lit(path)
        if not fils:
            sys.exit("aucun fil dans %s" % path)
        # le fil qui fait l'image : le plus chargé de ceux qui passent par le
        # plugin (Warcraft III dessine hors de son fil le plus chargé)
        gl = [t for t in fils if any(x[2] in POSTES_SET for x in t[2])]
        nom, n, noeuds = max(gl or fils, key=lambda t: t[1])
        total += n
        print("%s : fil %s, %d échantillons" % (path, nom, n))
        resume(noeuds, incl, propre, groupes, enfants, set(suivis))
        propre_sous(noeuds, "gleDrawArraysOrElements_VBO_Exec", ("gle", "_gle", "glDraw"), g)
    print("fil principal, %d relevé(s) : %d échantillons%s" % (
        len(fichiers), total, (", %.1f ms/image" % ms) if ms else ""))
    g = {f: n for f, n in g.items() if not f.startswith(("gld", "glr"))}
    print("GLEngine, propre sous gleDrawArraysOrElements_VBO_Exec : %d (%.1f %%) %s"
          % (sum(g.values()), 100.0 * sum(g.values()) / total,
             ", ".join("%s %d" % x for x in sorted(g.items(), key=lambda x: -x[1])[:8])))

    def ligne(f, n, pr=None):
        s = "%-44s %6d %5.1f%%" % (f[:44], n, 100.0 * n / total)
        if ms:
            s += " %6.2f" % (ms * n / total)
        if pr is not None:
            s += " %6d" % pr
        if ms and pr is not None:
            s += " %6.2f" % (ms * pr / total)
        return s

    print("\n%-44s %6s %6s%s %6s%s" % ("poste", "incl", "%", " ms/im" if ms else "", "propre",
                                       " ms/im" if ms else ""))
    for f in POSTES:
        if incl.get(f):
            print(ligne(f, incl[f], propre.get(f, 0)))
    print("\ngroupes (inclusifs, sans double compte) :")
    for k in GROUPES:
        print(ligne(k, groupes.get(k, 0)))
    for f in suivis:
        print("\nenfants de %s (inclusif %d, propre %d) :" % (f, incl.get(f, 0), propre.get(f, 0)))
        for e, n in sorted(enfants[f].items(), key=lambda x: -x[1]):
            print("  " + ligne(e, n))
    print("\npropre, %d premières :" % n_top)
    for f, n in sorted(propre.items(), key=lambda x: -x[1])[:n_top]:
        s = "%6d %5.1f%%" % (n, 100.0 * n / total)
        if ms:
            s += " %6.2f" % (ms * n / total)
        print("%s  %s" % (s, f))


if __name__ == "__main__":
    main()
