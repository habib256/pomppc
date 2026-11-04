#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Copie locale verifiee de shaders.big : Track.xpu TEX t2 -> TXP t2.

Ne modifie jamais l'entree ni un fichier de sortie existant. Aucun acces VM.
--restore recopie l'ORIGINAL connu conserve, vers une nouvelle sortie.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys


class ArchiveError(ValueError):
    pass


@dataclass(frozen=True)
class Entry:
    name: str
    offset: int
    size: int


@dataclass(frozen=True)
class Profile:
    size: int
    original: str
    patched: str
    track_original: str
    track_patched: str
    opcode_offset: int


KNOWN = Profile(
    215040,
    '9825b610f58ab69b26dfe9c57d1423b373f20f0b0c6d89632a07d4750a9e5256',
    'f9fe55f4310998c1bb548d9abf633a171073e9950315621b627758b4e813e4f6',
    '993f14fb517fcc7806cf2bf23c4574f2d83aa35d815bfdcbf8cfa486488cea8d',
    '56af056d75ef1255e2f205affa9644b92ab4efe720f5513d16ad8d87d9e1404e',
    163260,
)
# Une seule instruction, ancree sur la ligne ; les templates %zt2 sont
# remplaces par IndirectX a l'execution, jamais par cet outil.
TRACK_T2 = re.compile(
    rb'(?m)^[ \t]*(?P<opcode>TEX)[ \t]+t2[ \t]*,[ \t]*'
    rb'fragment\.texcoord\[2\][ \t]*,[ \t]*texture\[2\][ \t]*,[ \t]*'
    rb'%zt2[ \t]*;[ \t]*\r?$'
)
TRACK_T2_PROJECTED = re.compile(TRACK_T2.pattern.replace(b'TEX', b'TXP'))


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def parse_bigf(data):
    """Variante BIGF de cette edition : entete36, noms16, taille/offset LE."""
    if len(data) < 36:
        raise ArchiveError('entete BIGF tronque')
    magic, count, alignment, payload = struct.unpack_from('<4sIII', data)
    if magic != b'BIGF' or count != 53 or alignment != 2048 or payload != 2048:
        raise ArchiveError('variante BIGF inconnue (53 entrees, alignement/donnees 2048 requis)')
    if data[16:36] != b'Administrator' + b'\0' * 7:
        raise ArchiveError('identifiant BIGF inconnu')
    if 36 + count * 24 > payload or payload > len(data):
        raise ArchiveError('repertoire BIGF hors limites')
    entries, names = [], set()
    for index in range(count):
        pos = 36 + index * 24
        raw_name = data[pos:pos + 16]
        name, separator, padding = raw_name.partition(b'\0')
        if not separator or any(padding) or not re.fullmatch(rb'[A-Za-z0-9_.-]+', name):
            raise ArchiveError('nom BIGF invalide a entree %d' % index)
        name = name.decode('ascii')
        if name.casefold() in names:
            raise ArchiveError('nom BIGF duplique : ' + name)
        names.add(name.casefold())
        size, relative = struct.unpack_from('<II', data, pos + 16)
        offset = payload + relative
        if not size or relative % alignment or offset + size > len(data):
            raise ArchiveError('entree BIGF hors limites/alignment : ' + name)
        header = b'!!ARBvp1.0' if name.endswith('.xvu') else b'!!ARBfp1.0'
        if not name.endswith(('.xvu', '.xpu')) or not data[offset:offset + size].startswith(header):
            raise ArchiveError('source ARB inattendue : ' + name)
        entries.append(Entry(name, offset, size))
    ordered = sorted(entries, key=lambda entry: entry.offset)
    for before, after in zip(ordered, ordered[1:]):
        if before.offset + before.size > after.offset:
            raise ArchiveError('entrees BIGF chevauchees')
    return entries


def transform(data, restore=False, profile=KNOWN):
    """Valide tout avant de produire des octets ; profile interne pour tests synthetiques."""
    if len(data) != profile.size:
        raise ArchiveError('taille inconnue : %d (attendu %d)' % (len(data), profile.size))
    entries = parse_bigf(data)
    if sha256(data) != profile.original:
        raise ArchiveError('SHA-256 original inconnu ; aucune modification autorisee')
    targets = [entry for entry in entries if entry.name == 'Track.xpu']
    if len(targets) != 1:
        raise ArchiveError('Track.xpu unique absent du repertoire')
    track = targets[0]
    source = data[track.offset:track.offset + track.size]
    if sha256(source) != profile.track_original:
        raise ArchiveError('empreinte de Track.xpu inattendue')
    matches = list(TRACK_T2.finditer(source))
    if len(matches) != 1:
        raise ArchiveError('instruction TEX t2 unique absente de Track.xpu')
    opcode = track.offset + matches[0].start('opcode')
    if opcode != profile.opcode_offset or data[opcode:opcode + 3] != b'TEX':
        raise ArchiveError('position de correction inattendue')
    patched = data[:opcode] + b'TXP' + data[opcode + 3:]
    projected = list(TRACK_T2_PROJECTED.finditer(patched[track.offset:track.offset + track.size]))
    if (len(projected) != 1 or projected[0].start('opcode') != matches[0].start('opcode') or
            patched[opcode:opcode + 3] != b'TXP'):
        raise ArchiveError('projection TXP absente ou opcode invalide apres correction')
    changed = [index for index, (before, after) in enumerate(zip(data, patched)) if before != after]
    if changed != [opcode + 1, opcode + 2]:
        raise ArchiveError('deux changements E->X et X->P exactement requis')
    if len(patched) != len(data) or sha256(patched) != profile.patched:
        raise ArchiveError('empreinte apres correction inattendue')
    if sha256(patched[track.offset:track.offset + track.size]) != profile.track_patched:
        raise ArchiveError('empreinte Track.xpu corrige inattendue')
    # Deux octets, meme longueur ; controler aussi les autres entrees.
    if patched[:opcode] != data[:opcode] or patched[opcode + 3:] != data[opcode + 3:]:
        raise ArchiveError('difference hors opcode')
    for entry in entries:
        if entry != track and patched[entry.offset:entry.offset + entry.size] != data[entry.offset:entry.offset + entry.size]:
            raise ArchiveError('autre programme modifie : ' + entry.name)
    output = data if restore else patched
    return output, {
        'operation': 'restore-original' if restore else 'Track.xpu TEX t2 -> TXP t2',
        'input_sha256': profile.original,
        'output_sha256': sha256(output),
        'bytes': len(output), 'entries': len(entries), 'entry': track.name,
        'entry_offset': track.offset, 'entry_bytes': track.size,
        'opcode_before': 'TEX', 'opcode_after': 'TEX' if restore else 'TXP',
        'opcode_offset': opcode,
        'changed_offsets': [] if restore else changed,
        'changed_bytes': 0 if restore else 2,
    }


def write_new(path, data):
    """Creation exclusive : jamais d'ecrasement ni de suivi d'un lien sortie."""
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0)
    fd = os.open(path, flags, 0o644)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if Path(path).read_bytes() != data:
            raise ArchiveError('relecture sortie differente')
    except BaseException:
        os.unlink(path)  # Seulement le fichier que CET appel vient de creer.
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path, help='shaders.big.original conserve, SHA-256 connu')
    parser.add_argument('output', type=Path, help='nouveau fichier ; doit etre absent')
    parser.add_argument('--restore', action='store_true', help='copier l original connu sans correctif')
    args = parser.parse_args(argv)
    try:
        if not stat.S_ISREG(args.original.stat().st_mode):
            raise ArchiveError('entree non reguliere')
        if args.original.stat().st_size != KNOWN.size:
            raise ArchiveError('taille archive inconnue')
        if args.original.resolve() == args.output.resolve():
            raise ArchiveError('entree et sortie identiques interdites')
        result, report = transform(args.original.read_bytes(), args.restore)
        write_new(args.output, result)
    except (ArchiveError, OSError) as error:
        parser.exit(1, 'Refus : %s\n' % error)
    report['input'] = str(args.original)
    report['output'] = str(args.output)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
