#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Archive synthétique seulement ; original propriétaire facultatif hors dépôt."""
from dataclasses import replace
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/guest/cmr-track-projection.py'
spec = importlib.util.spec_from_file_location('cmr_track_projection', TOOL)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
LINE = b'TEX t2, fragment.texcoord[2], texture[2], %zt2;\n'
T3 = b'TEX t3, fragment.texcoord[3], texture[3], %zt3;\n'


def fixture(line=LINE):
    # Aucun programme du jeu : seules deux instructions de texture minimales.
    data = bytearray(2048 * 54)
    struct.pack_into('<4sIII20s', data, 0, b'BIGF', 53, 2048, 2048, b'Administrator')
    payload = b'!!ARBfp1.0\n# test synthetique\n' + line + T3 + b'END\n'
    for index in range(53):
        name = 'Track.xpu' if index == 30 else 'Test%d.xpu' % index
        struct.pack_into('<16sII', data, 36 + index * 24, name.encode(), len(payload), index * 2048)
        begin = 2048 + index * 2048
        data[begin:begin + len(payload)] = payload
    data = bytes(data)
    entry = module.parse_bigf(data)[30]
    pos = entry.offset + payload.index(b'TEX') + 2
    patched = data[:pos] + b'P' + data[pos + 1:]
    profile = module.Profile(len(data), module.sha256(data), module.sha256(patched),
                             module.sha256(payload), module.sha256(patched[entry.offset:entry.offset + entry.size]), pos)
    return data, patched, profile


class ProjectionTests(unittest.TestCase):
    def test_one_byte_only_and_other_programs(self):
        data, expected, profile = fixture()
        patched, report = module.transform(data, profile=profile)
        self.assertEqual(patched, expected)
        self.assertEqual([i for i, (a,b) in enumerate(zip(data, patched)) if a != b], [profile.changed_offset])
        self.assertEqual(patched[profile.changed_offset], ord('P'))
        self.assertEqual(report['changed_bytes'], 1)
        self.assertEqual(module.parse_bigf(data), module.parse_bigf(patched))
        for entry in module.parse_bigf(data):
            if entry.name != 'Track.xpu':
                self.assertEqual(data[entry.offset:entry.offset+entry.size], patched[entry.offset:entry.offset+entry.size])
        self.assertEqual(patched.count(T3), 53)  # T3 et tous les autres programmes restent exacts.
        self.assertEqual(patched.count(LINE), 52)

    def test_tabs_and_restore(self):
        data, expected, profile = fixture(LINE.replace(b'TEX t2,', b'\tTEX\tt2 , '))
        self.assertEqual(module.transform(data, profile=profile)[0], expected)
        restored, report = module.transform(data, restore=True, profile=profile)
        self.assertEqual(restored, data)
        self.assertEqual(report['changed_bytes'], 0)

    def test_unknown_mutated_and_already_patched_refused(self):
        data, expected, profile = fixture()
        with self.assertRaisesRegex(module.ArchiveError, 'taille'):
            module.transform(data[:-1], profile=profile)
        bad = bytearray(data);bad[-1] = 1
        with self.assertRaisesRegex(module.ArchiveError, 'SHA-256'):
            module.transform(bytes(bad), profile=profile)
        with self.assertRaisesRegex(module.ArchiveError, 'SHA-256'):
            module.transform(expected, profile=profile)
        with self.assertRaises(module.ArchiveError):
            module.transform(data)  # Synthétique jamais accepté comme archive réelle.

    def test_malformed_directory(self):
        data, _, _ = fixture()
        variants = []
        for offset, value in [(0,b'BAD!'),(4,struct.pack('<I',0xffffffff)),
                              (8,struct.pack('<I',0)),(12,struct.pack('<I',1)),
                              (36+20,struct.pack('<I',1)),
                              (36+16,struct.pack('<I',0xffffffff))]:
            bad = bytearray(data);bad[offset:offset+len(value)]=value;variants.append(bytes(bad))
        duplicate = bytearray(data);duplicate[60:76]=duplicate[36:52];variants.append(bytes(duplicate))
        overlap = bytearray(data);struct.pack_into('<I',overlap,60+20,0);variants.append(bytes(overlap))
        badname = bytearray(data);badname[36:52]=b'A'*16;variants.append(bytes(badname))
        badheader = bytearray(data);badheader[2048]=0;variants.append(bytes(badheader))
        for bad in variants:
            with self.subTest(bad=module.sha256(bad)):
                with self.assertRaises(module.ArchiveError): module.parse_bigf(bad)
        with self.assertRaises(module.ArchiveError): module.parse_bigf(b'BIGF')

    def test_missing_target_multiple_instructions_and_bad_posthash(self):
        data, _, profile = fixture()
        bad = bytearray(data);pos=36+30*24
        bad[pos:pos+16]=b'Other.xpu'+b'\0'*7
        with self.assertRaisesRegex(module.ArchiveError, 'Track.xpu'):
            module.transform(bytes(bad),profile=replace(profile,original=module.sha256(bad)))
        data, _, profile = fixture(LINE+LINE)
        with self.assertRaisesRegex(module.ArchiveError, 'unique'):
            module.transform(data,profile=profile)
        data, _, profile = fixture(LINE.replace(b'TEX',b'TXP'))
        with self.assertRaisesRegex(module.ArchiveError, 'unique'):
            module.transform(data,profile=profile)
        data, _, profile = fixture()
        with self.assertRaisesRegex(module.ArchiveError, 'apres'):
            module.transform(data,profile=replace(profile,patched='0'*64))
        with self.assertRaisesRegex(module.ArchiveError, 'position'):
            module.transform(data,profile=replace(profile,changed_offset=profile.changed_offset+1))

    def test_exclusive_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp)/'new.big';module.write_new(out,b'one')
            self.assertEqual(out.read_bytes(),b'one')
            with self.assertRaises(FileExistsError): module.write_new(out,b'two')
            self.assertEqual(out.read_bytes(),b'one')
            link=Path(tmp)/'link.big';link.symlink_to(out)
            with self.assertRaises(FileExistsError): module.write_new(link,b'two')
            self.assertEqual(out.read_bytes(),b'one')

    @unittest.skipUnless(os.environ.get('POMPPC_CMR_ORIGINAL'),'original propriétaire local facultatif')
    def test_real_archive_cli(self):
        original=Path(os.environ['POMPPC_CMR_ORIGINAL'])
        data=original.read_bytes()
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp)/'shaders.big';restore=Path(tmp)/'restored.big'
            run=lambda *args: subprocess.run([sys.executable,str(TOOL),str(original),*map(str,args)],capture_output=True,text=True)
            r=run(out);self.assertEqual(r.returncode,0,r.stderr)
            patched=out.read_bytes()
            self.assertEqual(module.sha256(patched),module.KNOWN.patched)
            self.assertEqual([i for i,(a,b) in enumerate(zip(data,patched)) if a!=b],[163262])
            self.assertEqual(original.read_bytes(),data)
            self.assertNotEqual(run(out).returncode,0)
            self.assertNotEqual(run(original).returncode,0)
            self.assertEqual(run(restore,'--restore').returncode,0)
            self.assertEqual(restore.read_bytes(),data)
            r=subprocess.run([sys.executable,str(TOOL),str(out),str(Path(tmp)/'twice.big')],capture_output=True,text=True)
            self.assertNotEqual(r.returncode,0)
            self.assertFalse((Path(tmp)/'twice.big').exists())


if __name__=='__main__':
    unittest.main()
