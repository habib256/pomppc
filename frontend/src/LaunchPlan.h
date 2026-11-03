// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// LaunchPlan — la ligne de commande de QEMU quand il n'y a pas de bash pour exécuter run_tiger.sh ou
// run_os9.sh : le paquet Windows. Sur Linux et macOS le frontal lance toujours ces scripts, et ce
// fichier n'entre que dans frontend/tests/launchplan_test.cpp.
//
// Le paquet porte le QEMU qu'il a été construit avec : ses capacités sont connues à la construction
// (scripts/build_qemu_qfb.sh, packaging/windows/smoke_windows.sh lance la ligne produite ici) et ne
// se sondent pas à chaque démarrage, contrairement aux scripts qui acceptent n'importe quel binaire.
// La seule capacité qui dépend de la machine, le backend GL de qgpu, reste à backend=auto.
//
// Sans API de Windows ici : tout ce qui touche l'hôte passe par LaunchHost, pour rester testable.
#pragma once

#include <functional>
#include <string>
#include <vector>

struct LaunchHost {
    // Variable d'environnement. Rend faux si elle n'existe pas (vide ≠ absente, POMPPC_BOOT_ARGS).
    std::function<bool(const std::string& name, std::string* value)> env;
    std::function<bool(const std::string& path)> fileExists;
    // Première ligne d'un petit fichier de réglage, vide s'il n'existe pas.
    std::function<std::string(const std::string& path)> firstLine;
    // Taille du fichier, 0 s'il n'existe pas : le disque d'OS 9 est-il installé ?
    std::function<unsigned long long(const std::string& path)> fileSize;
    // Le port TCP de 127.0.0.1 est-il libre ?
    std::function<bool(int port)> portFree;
    // Un chemin tel que QEMU saura l'ouvrir. Il lit argv dans la page de code ANSI (block/file-win32.c
    // appelle CreateFileA) : un chemin non ASCII doit passer par sa forme courte. Vide si impossible.
    std::function<std::string(const std::string& path)> qemuPath;
};

struct LaunchInput {
    std::string packageRoot;   // les .exe, share/qemu, patches/smp-mac99, pomppc-guest.iso
    std::string dataRoot;      // disks/, images/, .run/
    std::string qmpPath;       // socket QMP, déjà passé par qemuPath
    bool tiger = true;         // sinon Mac OS 9
};

struct LaunchPlan {
    bool ok = false;
    std::string error;                  // en anglais : s'affiche dans la fenêtre
    std::string exe;                    // qemu-system-ppc.exe du paquet
    std::vector<std::string> args;      // sans argv[0]
    std::vector<std::string> childEnv;  // KEY=VAL à ajouter à l'environnement de QEMU
    // Verrou à tenir pendant toute la vie de QEMU ; vide en SNAPSHOT=1 (écritures jetées).
    std::string lockFile;
    int sshPort = 0;
};

LaunchPlan buildLaunchPlan(const LaunchInput& in, const LaunchHost& host);

// [B] MSDN, « Parsing C++ command-line arguments » : l'inverse de ce que fait le CRT de QEMU à argv.
std::string windowsCommandLine(const std::vector<std::string>& argv);
