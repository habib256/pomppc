// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// AppExtras — ce que le frontal décide sans ImGui ni QEMU, testé seul (frontend/tests/extras_test.cpp) :
// fichiers déposés sur la fenêtre, mémoire réellement donnée à l'invité, versions de POMPPC.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

bool isDiscImage(const std::string& path);   // .iso .cdr .toast .img .dmg

// Une seule image déposée se branche telle quelle ; tout le reste part sur un disque « From Mac ».
enum class DropKind { Nothing, DiscImage, Files };
DropKind classifyDrop(const std::vector<std::string>& paths);

bool isCompressedImage(const std::string& path);   // .dmg
bool convertDiscImage(const std::string& dmg, const std::string& out, std::string* err);

// Crée un disque HFS+ écrivable avec ces fichiers et de la place en plus, branché ensuite en USB :
// l'invité le monte sans pilote, et ce qu'il y écrit se relit sur le Mac. Les fichiers restent.
bool buildExchangeDisk(const std::vector<std::string>& paths, const std::string& mountDir,
                       const std::string& out, std::string* err);

// [C] run_tiger.sh : le Screamer exige moins de 1 Go, le lanceur plafonne alors la mémoire.
int effectiveMemoryMb(int wantedMb, bool sound);

// -1, 0 ou 1. « 0.4 » == « 0.4.0 », et une version illisible ne se compare à rien (0).
int compareVersions(const std::string& a, const std::string& b);
std::string parseLatestTag(const std::string& githubJson);   // vide si introuvable

// Une tâche longue hors du fil de l'interface. poll() rend vrai une fois, à la fin.
class BackgroundTask {
public:
    ~BackgroundTask();
    bool busy() const { return running_.load(); }
    void start(std::function<std::string(std::string* err)> work);
    bool poll(std::string* result, std::string* err);

private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    bool pending_ = false;
    std::string result_, err_;
};
