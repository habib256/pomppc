// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// DiskConfig — le choix des disques d'une VM : ce que liste et valide le panneau Disks.
//
// Sans ImGui ni QEMU, pour être testé seul (frontend/tests/disks_test.cpp). Les lanceurs
// reçoivent le résultat par l'environnement (scripts/disques.sh).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct DiskEntry {
    std::string name;       // nom de fichier dans disks/
    uint64_t bytes = 0;     // taille vue par l'invité, lue dans l'en-tête qcow2
    bool ssd = false;       // <disque>.type
};

struct DiskSelection {
    std::string boot;                 // nom de fichier ; vide = le disque par défaut
    std::vector<std::string> extras;  // noms de fichier
    bool operator==(const DiskSelection& o) const { return boot == o.boot && extras == o.extras; }
    bool operator!=(const DiskSelection& o) const { return !(*this == o); }
};

// Les images qcow2 de <root>/disks, triées. Un autre format n'est pas listé : les lanceurs
// n'attachent que du qcow2.
std::vector<DiskEntry> listDisks(const std::string& root);

// Disques en plus de celui de démarrage que la machine accepte : 4 emplacements IDE, moins celui
// de démarrage, le lecteur CD et, sous OS 9, le CD de jeux s'il existe.
int maxExtraDisks(const std::string& root, bool tiger);

// Retire ce qui n'existe plus, les doublons et le surplus. Disque de démarrage absent : celui par
// défaut, même s'il n'existe pas encore (le lanceur décide alors).
DiskSelection normalizeSelection(const DiskSelection& in, const std::vector<DiskEntry>& disks,
                                 const std::string& defaultBoot, int maxExtras);

// VAR=valeur pour le lanceur. Rien pour un disque de démarrage vide.
std::vector<std::string> diskEnv(const std::string& root, bool tiger, const DiskSelection& sel);

std::string joinNames(const std::vector<std::string>& names);        // a;b
std::vector<std::string> splitNames(const std::string& text);

// Lettres, chiffres, espace, - et _ : ce nom part dans un chemin et un shell. Sans extension.
bool validDiskName(const std::string& name);
// Crée <root>/disks/<name>.qcow2, sans jamais écraser. Rend faux et remplit err en cas d'échec.
bool createDisk(const std::string& root, const std::string& name, int gigabytes, bool ssd,
                std::string* err);
bool setDiskSsd(const std::string& root, const std::string& fileName, bool ssd);
std::string formatDiskSize(uint64_t bytes);
