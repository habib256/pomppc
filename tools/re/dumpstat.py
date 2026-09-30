#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""dumpstat.py DOSSIER_DE_VIDAGE [--toutes] — volumes par image d'un vidage
POMPPC_GL_DUMP (fichiers NNNNNN.bin, en-tête `struct dump_hdr` de 64 octets :
magic, image, base, octets de commandes, vtx_off/len, idx_off/len,
arena_off/len). Par image :
  - octets de texture : TEX_IMAGE (w×h×4), TEX_IMAGE3 et TEX_SUBIMAGE
    (octets par tranche × profondeur, ou octets par ligne × h si la tranche
    vaut 0) ;
  - octets de géométrie : zones de sommets et d'indices de chaque soumission
    (DRAW_RAW, chemin hérité) + BUF_SUBDATA (miroirs des VBO, DRAW_NATIVE) ;
  - octets de commandes, dessins, SET_STATE, SET_MATRIX, SET_LIGHT…
La PREMIÈRE image du vidage est à part : le vidage est autonome, il réémet
tout l'état et toutes les textures (dump_submit) ; elle n'est pas dans la
moyenne. Les opcodes viennent de patches/qgpu/qgpu_proto.h (lancé depuis la
racine d'un dépôt ou par chemin absolu)."""
import os
import re
import struct
import sys
from collections import defaultdict

ICI = os.path.dirname(os.path.abspath(__file__))
PROTO = os.path.join(ICI, "..", "..", "patches", "qgpu", "qgpu_proto.h")
ops = {}
for m in re.finditer(r'#define QGPU_OP_(\w+)\s+0x([0-9A-Fa-f]+)', open(PROTO).read()):
    ops[int(m.group(2), 16)] = m.group(1)

DESSINS = ("DRAW_TRIANGLES", "DRAW_TRIANGLES_TEX", "DRAW_TRIANGLES_TEX2", "DRAW_LINES",
           "DRAW_POINTS", "DRAW_TRIANGLES_TEXN", "DRAW_TRIANGLES_SEC", "DRAW_RAW",
           "DRAW_RAW_BUF", "DRAW_NATIVE")


def une(path, acc):
    d = open(path, "rb").read()
    h = struct.unpack(">16I", d[:64])
    if h[0] != 0x50514431:
        return None
    frame, ncmd = h[1], h[3]
    a = acc[frame]
    words = struct.unpack(">%dI" % (ncmd // 4), d[64:64 + ncmd])
    a["soumissions"] += 1
    a["commandes"] += ncmd
    a["geometrie"] += h[5] + h[7]
    q = 0
    while q < len(words):
        hd = words[q]
        op, l = hd >> 16, hd & 0xffff
        if not l:
            break
        x = words[q + 1:q + l]
        n = ops.get(op, hex(op))
        a["op:" + n] += 1
        a["mots:" + n] += l
        if n == "TEX_IMAGE" and len(x) >= 4:
            a["texture"] += x[2] * x[3] * 4
        elif n == "TEX_IMAGE3" and len(x) >= 11:
            a["texture"] += (x[11] * x[5]) if x[11] else x[10] * x[4] * max(1, x[5])
        elif n == "TEX_SUBIMAGE" and len(x) >= 14:
            a["texture"] += (x[13] * x[8]) if x[13] else x[12] * x[7] * max(1, x[8])
        elif n == "BUF_SUBDATA" and len(x) >= 4:
            a["geometrie"] += x[3]
        if n in DESSINS:
            a["dessins"] += 1
        q += l
    return frame


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    dossier = sys.argv[1]
    acc = defaultdict(lambda: defaultdict(int))
    for f in sorted(os.listdir(dossier)):
        if f.endswith(".bin"):
            une(os.path.join(dossier, f), acc)
    if not acc:
        sys.exit("aucun vidage dans %s" % dossier)
    images = sorted(acc)
    prem, reste = images[0], images[1:]
    cles = ["texture", "geometrie", "commandes", "soumissions", "dessins",
            "op:SET_STATE", "op:SET_MATRIX", "op:SET_LIGHT", "op:SET_MATERIAL", "op:SET_CURRENT",
            "op:SET_TEXGEN", "op:SET_CLIP_PLANE", "op:VIEWPORT", "op:DEPTH_RANGE",
            "op:PROG_ENV", "op:PROG_LOCAL", "op:PROG_BIND", "op:GLSL_UNIFORMS",
            "op:TEX_IMAGE", "op:TEX_IMAGE3", "op:TEX_SUBIMAGE", "op:TEX_PARAM",
            "op:BUF_SUBDATA", "op:COPY_TEX", "op:DRAW_NATIVE", "op:DRAW_RAW", "op:DRAW_RAW_BUF"]
    print("vidage %s : images %d..%d (%d), la première (autonome) à part" %
          (dossier, prem, images[-1], len(images)))
    print("%-18s %14s %14s %14s" % ("", "1re image", "moyenne/image", "max/image"))
    for k in cles:
        v0 = acc[prem].get(k, 0)
        vs = [acc[i].get(k, 0) for i in reste] or [0]
        if not v0 and not any(vs):
            continue
        print("%-18s %14d %14.0f %14d" % (k, v0, sum(vs) / len(vs), max(vs)))
    tous = defaultdict(int)
    for i in reste:
        for k, v in acc[i].items():
            if k.startswith("mots:"):
                tous[k[5:]] += v
    if reste:
        print("\nmots de commandes par image, par opcode (hors 1re) :")
        for k, v in sorted(tous.items(), key=lambda x: -x[1])[:15]:
            print("  %-22s %10.0f" % (k, v / len(reste)))


if __name__ == "__main__":
    main()
