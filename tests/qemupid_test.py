#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sélection du PID de QEMU : le shell parent qui cite le disque ne doit jamais être retenu."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "matrice"))
import qemupid  # noqa: E402

QEMU = "/Users/x/src/qemu/build/qemu-system-ppc"
LIGNE = "-M mac99,via=pmu -drive file=/Users/x/bench/disques/actuel/disks/tiger.qcow2,format=qcow2"


class Choix(unittest.TestCase):
    def test_shell_parent_avant_qemu(self):
        # ordre du cas réel : le shell a le PID le plus bas et cite QEMU, mac99 et le disque
        ps = ("100 /bin/bash bash -c DISK=/b/disks/tiger.qcow2 %s %s\n"
              "101 /usr/bin/env env DISK=/b/disks/tiger.qcow2 ./run_tiger.sh -M mac99\n"
              "200 %s %s %s\n" % (QEMU, LIGNE, QEMU, QEMU, LIGNE))
        self.assertEqual(qemupid.choisit(ps), [200])

    def test_shell_seul(self):
        ps = "100 bash bash -c '%s %s'\n" % (QEMU, LIGNE)
        self.assertEqual(qemupid.choisit(ps), [])

    def test_copie_renommee(self):
        ps = "300 /tmp/qret qret %s\n" % LIGNE
        self.assertEqual(qemupid.choisit(ps), [300])

    def test_autre_disque_ou_machine(self):
        ps = ("400 %s %s -M mac99 -drive file=/d/os9.qcow2\n"
              "401 %s %s -M g3beige -drive file=/d/disks/tiger.qcow2\n" % (QEMU, QEMU, QEMU, QEMU))
        self.assertEqual(qemupid.choisit(ps), [])

    def test_lignes_courtes(self):
        self.assertEqual(qemupid.choisit("\n12\nabc d e\n"), [])


class Profil(unittest.TestCase):
    def ecrit(self, texte):
        f = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
        self.addCleanup(os.unlink, f.name)
        f.write(texte)
        f.close()
        return f.name

    def test_profil_bash_refuse(self):
        self.assertFalse(qemupid.profil_de_qemu(self.ecrit("Analysis\nProcess:  bash [12]\n")))

    def test_profil_qemu_accepte(self):
        self.assertTrue(qemupid.profil_de_qemu(self.ecrit("Process:  qemu-system-ppc [12]\n")))

    def test_profil_perf_accepte(self):
        self.assertTrue(qemupid.profil_de_qemu(self.ecrit("# Samples: 1K\n")))

    def test_profil_absent_refuse(self):
        self.assertFalse(qemupid.profil_de_qemu("/nonexistent/profil.txt"))


if __name__ == "__main__":
    unittest.main()
