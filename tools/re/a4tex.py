# GPL3 - Copyleft VERHILLE Arnaud
# A4, volet textures (30/09/2026, docs/protocole-v23-textures.md) : trafic de texels par image
# dans un vidage POMPPC_GL_DUMP de la matrice.
# Usage : python3 tools/re/a4tex.py DOSSIER_DE_VIDAGE... — lancé depuis la racine du dépôt.
# Un vidage de matrice est autonome : sa première image réémet toutes les textures liées, les
# autres textures (marquées sales) repartent à leur premier usage. Par clé (texture, face,
# niveau), un « renvoi » est un niveau déjà envoyé dans le vidage et renvoyé (vrai trafic de
# régime) ; un « premier envoi » apparaît pour la première fois après la réémission
# (contamination du vidage surtout : borne haute du régime).
import collections
import os
import re
import struct
import sys

PROTO = open('patches/qgpu/qgpu_proto.h').read()
OPS = {int(m.group(2), 16): m.group(1)
       for m in re.finditer(r'#define QGPU_OP_(\w+)\s+0x([0-9A-Fa-f]+)', PROTO)}


def texel_bytes(fmt, typ):
    if typ == 0x1401:
        return {0x1908: 4, 0x80E1: 4, 0x1907: 3, 0x80E0: 3, 0x190A: 2}.get(fmt, 1)
    if typ in (0x8035, 0x8367, 0x1406, 0x1405):
        return 4
    return 2


def size(name, a):
    """Octets de texels portés par la commande (ce que l'invité a recopié dans BAR0)."""
    if name == 'TEX_IMAGE':
        return a[2] * a[3] * 4
    if name == 'TEX_IMAGE3':
        w, h, d, fmt, typ, row, img = a[3], a[4], a[5], a[7], a[8], a[10], a[11]
        if 0x83F0 <= fmt <= 0x83F3 and typ == 0:
            return ((w + 3) // 4) * ((h + 3) // 4) * (8 if fmt <= 0x83F1 else 16)
        b = texel_bytes(fmt, typ)
        row = row or w * b
        return (img or row * h) * (d - 1) + row * (h - 1) + w * b
    if name == 'TEX_SUBIMAGE':
        return a[6] * a[7] * a[8] * texel_bytes(a[9], a[10])
    return 0


def commands(path):
    raw = open(path, 'rb').read()
    h = struct.unpack('>16I', raw[:64])
    words = struct.unpack('>%dI' % (h[3] // 4), raw[64:64 + h[3]])
    q = 0
    while q < len(words):
        l = words[q] & 0xffff
        if not l:
            break
        yield h[1], OPS.get(words[q] >> 16), words[q + 1:q + l]
        q += l


def analyse(d):
    seen = {}
    frames = set()
    f0 = None
    rep = [0, 0]
    first = [0, 0]
    resident = [0, 0]
    for fn in sorted(f for f in os.listdir(d) if f.endswith('.bin')):
        for frame, nm, a in commands(os.path.join(d, fn)):
            if f0 is None:
                f0 = frame
            frames.add(frame)
            if nm in ('TEX_IMAGE', 'TEX_IMAGE3', 'TEX_SUBIMAGE'):
                k = (a[0], a[1], a[2]) if nm != 'TEX_IMAGE' else (a[0], 0, a[1])
                sz = size(nm, a)
                if frame == f0:
                    resident[0] += 1
                    resident[1] += sz
                    seen[k] = 1
                elif k in seen:
                    rep[0] += 1
                    rep[1] += sz
                else:
                    first[0] += 1
                    first[1] += sz
                    seen[k] = 1
            elif nm == 'TEX_DESTROY':
                for kk in [x for x in seen if x[0] == a[0]]:
                    del seen[kk]
    n = max(1, len(frames) - 1)
    return n, resident, rep, first


if __name__ == '__main__':
    print('%-40s %6s | %-24s | %-22s | %s' % ('vidage', 'images', 'réémission (niveaux, Kio)',
                                              'renvois / image', 'premiers envois / image'))
    for d in sys.argv[1:]:
        n, res, rep, first = analyse(d)
        print('%-40s %6d | %6d niv. %9.0f Kio | %5.1f niv. %7.1f Kio | %5.1f niv. %7.1f Kio' % (
            d, n, res[0], res[1] / 1024, rep[0] / n, rep[1] / n / 1024,
            first[0] / n, first[1] / n / 1024))
