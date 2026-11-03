// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
#include "DiskConfig.h"

#ifdef _WIN32
#include "WinHost.h"
#else
#include <spawn.h>
#include <sys/wait.h>
#endif

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#ifndef _WIN32
extern char** environ;
#endif
namespace fs = std::filesystem;

namespace {

// En-tête qcow2 : « QFI\xfb » à 0, taille virtuelle en 64 bits gros-boutiste à 24.
bool readQcow2Size(const fs::path& p, uint64_t* bytes) {
    std::ifstream in(p, std::ios::binary);
    unsigned char h[32];
    if (!in.read(reinterpret_cast<char*>(h), sizeof(h))) return false;
    if (h[0] != 'Q' || h[1] != 'F' || h[2] != 'I' || h[3] != 0xfb) return false;
    uint64_t v = 0;
    for (int i = 24; i < 32; ++i) v = (v << 8) | h[i];
    *bytes = v;
    return true;
}

}  // namespace

std::vector<DiskEntry> listDisks(const std::string& root) {
    std::vector<DiskEntry> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root + "/disks", ec)) {
        if (e.path().extension() != ".qcow2") continue;
        DiskEntry d;
        if (!readQcow2Size(e.path(), &d.bytes)) continue;
        d.name = e.path().filename().string();
        std::ifstream type(e.path().string() + ".type");
        std::string t;
        d.ssd = (type >> t) && t == "ssd";
        out.push_back(d);
    }
    std::sort(out.begin(), out.end(),
              [](const DiskEntry& a, const DiskEntry& b) { return a.name < b.name; });
    return out;
}

int maxExtraDisks(const std::string& root, bool tiger) {
    int used = 2;  // démarrage, lecteur CD
    std::error_code ec;
    if (!tiger && fs::exists(root + "/disks/games.iso", ec)) used++;
    return 4 - used;
}

DiskSelection normalizeSelection(const DiskSelection& in, const std::vector<DiskEntry>& disks,
                                 const std::string& defaultBoot, int maxExtras) {
    auto known = [&](const std::string& n) {
        return std::any_of(disks.begin(), disks.end(),
                           [&](const DiskEntry& d) { return d.name == n; });
    };
    DiskSelection out;
    // Jamais un autre disque à la place du défaut absent : OS 9 démarrerait sur celui de Tiger.
    out.boot = known(in.boot) ? in.boot : defaultBoot;
    for (const std::string& n : in.extras) {
        if ((int)out.extras.size() >= maxExtras) break;
        if (!known(n) || n == out.boot) continue;
        if (std::find(out.extras.begin(), out.extras.end(), n) != out.extras.end()) continue;
        out.extras.push_back(n);
    }
    return out;
}

std::vector<std::string> diskEnv(const std::string& root, bool tiger, const DiskSelection& sel) {
    std::vector<std::string> env;
    if (sel.boot.empty()) return env;
    const std::string dir = root + "/disks/";
    env.push_back(std::string(tiger ? "DISK=" : "OS9_DISK=") + dir + sel.boot);
    std::string extras;
    for (const std::string& n : sel.extras) {
        if (!extras.empty()) extras += ';';
        extras += dir + n;
    }
    // Toujours posée : une valeur héritée ne doit pas rattacher un disque décoché.
    env.push_back("POMPPC_EXTRA_DISKS=" + extras);
    return env;
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) {
        if (!out.empty()) out += ';';
        out += n;
    }
    return out;
}

std::vector<std::string> splitNames(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(';', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

bool validDiskName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] == ' ' || name[0] == '-' || name.back() == ' ')
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == ' ' || c == '-' || c == '_';
    });
}

bool setDiskSsd(const std::string& root, const std::string& fileName, bool ssd) {
    const std::string path = root + "/disks/" + fileName + ".type";
    std::error_code ec;
    if (!ssd) {
        fs::remove(path, ec);
        return !ec;
    }
    std::ofstream out(path);
    out << "ssd\n";
    return static_cast<bool>(out);
}

bool createDisk(const std::string& root, const std::string& name, int gigabytes, bool ssd,
                std::string* err) {
    if (name.empty()) {
        *err = "Enter a name for the new disk.";
        return false;
    }
    if (!validDiskName(name)) {
        *err = "Use only letters, digits, spaces, - and _ in the name.";
        return false;
    }
    if (gigabytes < 1 || gigabytes > 2048) {
        *err = "The size must be between 1 and 2048 GB.";
        return false;
    }
    const std::string file = name + ".qcow2";
    const std::string path = root + "/disks/" + file;
    std::error_code ec;
    if (fs::exists(path, ec)) {
        *err = "A disk with this name already exists.";
        return false;
    }
    // config.env met qemu-img dans le PATH, celui du paquet comme celui d'un arbre de sources.
    const std::string size = std::to_string(gigabytes) + "G";
#ifdef _WIN32
    // qemu-img.exe : à côté de POMPPC.exe, où CreateProcess cherche d'abord.
    const std::string shortPath = winhost::qemuPath(path);
    if (shortPath.empty()) {
        *err = "POMPPC can't create a disk here because of the characters in this path: " + path;
        return false;
    }
    if (!winhost::runTool({"qemu-img", "create", "-q", "-f", "qcow2", shortPath, size}, nullptr, nullptr)) {
        *err = "qemu-img could not create the disk.";
        return false;
    }
#else
    const std::string config = root + "/config.env";
    const char* script = "source \"$1\" && qemu-img create -q -f qcow2 \"$2\" \"$3\"";
    const char* argv[] = {"bash", "-c", script, "bash", config.c_str(), path.c_str(), size.c_str(),
                          nullptr};
    pid_t pid = -1;
    if (posix_spawnp(&pid, "bash", nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0) {
        *err = "Could not start qemu-img.";
        return false;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        *err = "qemu-img could not create the disk.";
        return false;
    }
#endif
    if (ssd && !setDiskSsd(root, file, true)) {
        *err = "The disk was created, but its type could not be saved.";
        return false;
    }
    return true;
}

std::string formatDiskSize(uint64_t bytes) {
    char text[32];
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    std::snprintf(text, sizeof(text), gb == static_cast<uint64_t>(gb) ? "%.0f GB" : "%.1f GB", gb);
    return text;
}
