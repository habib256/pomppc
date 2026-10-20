#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""dumpvidages.py DOSSIER_DE_VIDAGE [--detail] — pourquoi chaque soumission d'un
vidage POMPPC_GL_DUMP est partie (20/10/2026, docs/d3-plugin-x86.md §9).

Pour chaque soumission (fichier NNNNNN.bin, en-tête `struct dump_hdr`) : image,
moitié, remplissage du flux, des sommets, des indices et de l'arène, dernière
commande, et la cause probable du vidage :
  échange    la soumission finit par SURF_PRESENT / une relecture d'échange ;
  sonde      PROG_STRING / GLSL_* seule (soumission synchrone de définition) ;
  relecture  finit par une relecture (SURF/TEX/DEPTH_READBACK, QUERY_RESULT) ;
  indices    zone des indices remplie à plus de 60 % (256 Kio : un seul gros dessin
             de DOOM 3 en prend 190) ;
  flux       flux à moins de 8 Kio de sa taille (256 ou 512 Kio) ;
  arène      arène à moins de 64 Kio de la fin de la moitié ;
  ?          aucune des précédentes (vidage demandé par une procédure).
Puis le bilan par image (hors première image, autonome) : soumissions par
image et par cause. Les tailles de zones se lisent dans l'en-tête (vtx_off =
taille du flux ; idx_off ; arena_off), donc valent aussi sous POMPPC_GL_ZONES."""
import collections
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO = os.path.join(HERE, "..", "..", "patches", "qgpu", "qgpu_proto.h")


def opnames():
    names = {}
    for m in re.finditer(r"#define (QGPU_OP_\w+)\s+0x([0-9A-Fa-f]+)", open(PROTO).read()):
        names.setdefault(int(m.group(2), 16), m.group(1)[8:])
    return names


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    detail = "--detail" in sys.argv
    if not args:
        sys.exit(__doc__)
    d = args[0]
    names = opnames()
    half = 0x800000
    rows = []
    for f in sorted(x for x in os.listdir(d) if x.endswith(".bin")):
        b = open(os.path.join(d, f), "rb").read()
        if len(b) < 64:
            continue
        h = struct.unpack(">16I", b[:64])
        _, frame, base, ncb, vo, vl, io, il, ao, al = h[:10]
        words = struct.unpack(">%dI" % (ncb // 4), b[64:64 + ncb])
        ops, i = [], 0
        while i < len(words):
            ln = words[i] & 0xFFFF
            ops.append(names.get(words[i] >> 16, hex(words[i] >> 16)))
            if not ln:
                break
            i += ln
        last = ops[-1] if ops else "-"
        real = [o for o in ops if o not in ("CTX_BIND",)]
        idx_size = ao - io if ao > io else 0x40000
        if last == "SURF_PRESENT":
            why = "échange"
        elif real and all(o.startswith(("PROG_", "GLSL_")) for o in real):
            why = "sonde"
        elif last in ("SURF_READBACK", "TEX_READBACK", "DEPTH_READBACK", "STENCIL_READBACK",
                      "QUERY_RESULT"):
            why = "relecture"
        elif il > idx_size * 6 // 10:
            why = "indices"
        elif ncb > vo - 0x2000:
            why = "flux"
        elif ao + al > half - 0x10000:
            why = "arène"
        else:
            why = "?"
        rows.append((f, frame, base, ncb, vl, il, al, last, why, ops.count("DRAW_NATIVE")))
    if detail:
        for r in rows:
            print("%s image %d moitié %08x flux %7d sommets %7d indices %7d arène %8d "
                  "dessins %4d  fin %-14s → %s" % (r[0], r[1], r[2], r[3], r[4], r[5], r[6],
                                                    r[9], r[7], r[8]))
    if not rows:
        return
    first = rows[0][1]
    per = collections.defaultdict(collections.Counter)
    for r in rows:
        if r[1] != first:
            per[r[1]][r[8]] += 1
    if not per:
        print("une seule image dans le vidage")
        return
    tot = collections.Counter()
    for c in per.values():
        tot.update(c)
    n = len(per)
    print("%d images (hors la première, autonome) : %.2f soumissions par image" %
          (n, sum(tot.values()) / n))
    for k, v in tot.most_common():
        print("  %-10s %6.2f par image" % (k, v / n))


if __name__ == "__main__":
    main()
