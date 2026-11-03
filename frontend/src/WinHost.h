// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// WinHost — ce que le paquet Windows demande à l'hôte, là où les autres hôtes ont bash et POSIX.
// N'est compilé que sous Windows (frontend/CMakeLists.txt).
//
// CHAÎNES : tout le frontal parle UTF-8. POMPPC.exe se déclare en UTF-8 (src/pomppc.manifest,
// activeCodePage, Windows 10 1903 et plus) : getenv, fopen et les flux lisent et écrivent de l'UTF-8.
// [B] MSDN, « Use UTF-8 code pages in Windows apps ». Les appels Win32 de ce fichier sont en W.
#pragma once
#ifdef _WIN32

#include <functional>
#include <string>
#include <vector>

#include "LaunchPlan.h"

namespace winhost {

std::wstring wide(const std::string& utf8);
std::string narrow(const std::wstring& text);

std::string exeDir();          // le dossier de POMPPC.exe, en UTF-8, avec des /
std::string userHome();        // %USERPROFILE%
std::string dataRoot();        // POMPPC_DATA, ou %USERPROFILE%/POMPPC : disks/, images/, .run/
std::string downloadsDir();
std::string tempDir();         // un chemin ASCII : pour les sockets AF_UNIX

// QEMU lit argv en ANSI et ouvre ses fichiers par CreateFileA (block/file-win32.c) : un chemin non
// ASCII peut lui être illisible. Rend la forme courte (8.3), en repartant du dernier parent qui existe
// pour un fichier encore à créer. Vide si elle n'est pas ASCII non plus (formes courtes coupées sur ce
// volume) : l'appelant le dit, plutôt que de laisser QEMU échouer sans un mot.
std::string qemuPath(const std::string& path);

// Le LaunchHost de buildLaunchPlan. `overrides` (KEY=VAL) passe avant l'environnement du processus.
LaunchHost launchHost(const std::vector<std::string>& overrides);

// Lance une commande sans fenêtre et attend sa fin : SetupFlow::runTool. `started` reçoit le PID dès le
// départ, pour terminate(), `out` la sortie standard. Vrai si le code de sortie est 0.
bool runTool(const std::vector<std::string>& args, std::string* out, const std::function<void(int)>& started);
void terminate(int pid);

void openExternal(const std::string& target);        // ShellExecuteW, sans attendre
std::string chooseFile(const std::string& prompt);   // GetOpenFileNameW ; vide si annulé

// Un processus de QEMU : hors de vue, dans un Job KILL_ON_JOB_CLOSE pour qu'il ne survive pas au frontal,
// stdout et stderr dans `logFile`. Les handles rendus appartiennent à l'appelant (CloseHandle).
struct Child {
    void* process = nullptr;
    void* job = nullptr;
    unsigned long pid = 0;
};
bool spawnQemu(const std::string& exe, const std::vector<std::string>& args,
               const std::vector<std::string>& env, const std::string& logFile, Child* child, std::string* err);

// Crée le verrou d'instance, partage 0 : tant que ce handle vit, personne d'autre ne l'ouvre. Rend
// nullptr si un autre le tient. [D] QEMU block/file-win32.c:331-335,365 @ v11.1.2 : sous Windows QEMU
// refuse locking=on et ouvre l'image en partage ; sans ce verrou, deux instances corrompent le qcow2.
// Le fichier n'est JAMAIS supprimé (docs/etat-des-hotes.md) : le handle fait foi, pas son existence.
void* lockInstance(const std::string& path);
void unlockInstance(void* handle);

// Une fois au démarrage : Winsock, et stderr vers .run/app.log si le processus n'a pas de sortie (exe
// de sous-système GUI lancé depuis l'Explorateur).
void startup(const std::string& logFile);

}  // namespace winhost

#endif  // _WIN32
