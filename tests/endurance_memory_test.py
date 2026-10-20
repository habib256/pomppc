#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Preuves du vidage RAM : erreurs de collecte et transfert HMP réel sans Tiger."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "endurance"))
import memoire
import endurance
from hmp import hmp

MTREE = "    0000000000000000-0000000001ffffff (prio 0, ram): ppc_core99.ram\n"


class CaptureTest(unittest.TestCase):
    def test_collecte_survit_au_screendump_en_echec(self):
        appels = []
        def commande(mon, cmd):
            appels.append(cmd)
            if cmd.startswith("screendump") or (cmd == "info registers -a" and "cont" in appels):
                raise OSError("écran indisponible")
            return ""
        def capture(mon, dossier):
            self.assertEqual(appels, ["stop", "info registers -a"])
            return {"complet": True, "duree_s": 0.1, "octets_ecrits": 32,
                    "octets_attendus": 32}
        with tempfile.TemporaryDirectory() as d, patch.object(
                endurance, "hmp", side_effect=commande), patch.object(
                endurance.memoire, "capture", side_effect=capture), patch.object(
                endurance.time, "sleep"):
            info = endurance.collecte("mon", d, None, "panique")
            self.assertTrue(info["memoire"]["complet"])
            self.assertIn("erreur_collecte", info)
            self.assertIn("cont", appels)
            self.assertTrue(Path(d, "memoire.json").exists())
            self.assertIn("complète (ram.bin)", Path(d, "incident.md").read_text())

    def test_region_repetee(self):
        self.assertEqual(memoire.plage_ram(MTREE * 2), (0, 32 * 1024 * 1024))

    def test_regions_invalides(self):
        for texte in ("", MTREE.replace("ram):", "rom):"),
                      MTREE + MTREE.replace("1ffffff", "3ffffff"),
                      MTREE.replace("0000000000000000-", "0000000000001000-")):
            with self.subTest(texte=texte), self.assertRaises(ValueError):
                memoire.plage_ram(texte)

    def test_erreurs_transfert(self):
        for erreur in ("court", "moniteur", "delai"):
            with self.subTest(erreur=erreur), tempfile.TemporaryDirectory() as d:
                def commande(mon, cmd, **kw):
                    if cmd == "info mtree":
                        return MTREE
                    if erreur == "delai":
                        raise TimeoutError("délai")
                    Path(d, "ram.bin").write_bytes(b"incomplet")
                    return "Error: failed" if erreur == "moniteur" else ""
                with patch.object(memoire, "hmp", side_effect=commande), patch.object(
                        memoire.shutil, "disk_usage", return_value=shutil._ntuple_diskusage(0, 0, 1 << 40)):
                    info = memoire.capture("mon", d)
                self.assertFalse(info["complet"])
                self.assertIn("erreur", info)

    def test_pas_de_place(self):
        with tempfile.TemporaryDirectory() as d, patch.object(
                memoire, "hmp", return_value=MTREE) as mon, patch.object(
                memoire.shutil, "disk_usage", return_value=shutil._ntuple_diskusage(0, 0, 0)):
            self.assertFalse(memoire.capture("mon", d)["complet"])
            self.assertEqual(mon.call_count, 1)

    def test_ancien_vidage_preserve(self):
        with tempfile.TemporaryDirectory() as d, patch.object(
                memoire, "hmp", return_value=MTREE) as mon:
            Path(d, "ram.bin").write_bytes(b"preuve precedente")
            self.assertFalse(memoire.capture("mon", d)["complet"])
            self.assertEqual(mon.call_count, 1)
            self.assertEqual(Path(d, "ram.bin").read_bytes(), b"preuve precedente")

    @unittest.skipUnless(shutil.which("qemu-system-ppc"), "QEMU PPC absent")
    def test_qemu_reel(self):
        # Aucun disque ni système invité ; CPU arrêté dès le départ. Le fichier
        # chargé en RAM permet de vérifier le contenu, pas seulement sa taille.
        with tempfile.TemporaryDirectory(prefix="endurance-") as d:
            mon = os.path.join(d, "mon.sock")
            temoin = Path(d, "temoin.bin")
            temoin.write_bytes(bytes(range(256)))
            dest = Path(d, 'incident avec espaces "cites"')
            dest.mkdir()
            with subprocess.Popen([
                    "qemu-system-ppc", "-M", "mac99", "-m", "32", "-S",
                    "-display", "none", "-serial", "none", "-nic", "none",
                    "-monitor", "unix:%s,server=on,wait=off" % mon,
                    "-device", "loader,file=%s,addr=0x10000,force-raw=on" % temoin],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as proc:
                try:
                    for _ in range(100):
                        if os.path.exists(mon) or proc.poll() is not None:
                            break
                        time.sleep(0.05)
                    self.assertIsNone(proc.poll(), proc.stderr.read().decode() if proc.poll() is not None else "")
                    info = memoire.capture(mon, str(dest))
                    self.assertTrue(info["complet"], info)
                    self.assertEqual(info["octets_ecrits"], 32 * 1024 * 1024)
                    with open(dest / "ram.bin", "rb") as f:
                        f.seek(0x10000)
                        self.assertEqual(f.read(256), temoin.read_bytes())
                    self.assertIn("paused", hmp(mon, "info status"))
                finally:
                    proc.terminate()
                    proc.wait(timeout=10)


if __name__ == "__main__":
    unittest.main()
