// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
#include "AppExtras.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "SetupFlow.h"

namespace fs = std::filesystem;

bool isDiscImage(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".iso" || ext == ".cdr" || ext == ".toast" || ext == ".img" || ext == ".dmg";
}

DropKind classifyDrop(const std::vector<std::string>& paths) {
    if (paths.empty()) return DropKind::Nothing;
    std::error_code ec;
    if (paths.size() == 1 && isDiscImage(paths[0]) && fs::is_regular_file(paths[0], ec))
        return DropKind::DiscImage;
    return DropKind::Files;
}

bool isCompressedImage(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".dmg";
}

bool convertDiscImage(const std::string& dmg, const std::string& out, std::string* err) {
    const std::string base = out.substr(0, out.find_last_of('.'));
    std::error_code ec;
    fs::remove(base + ".cdr", ec);
    if (!SetupFlow::runTool({"hdiutil", "convert", dmg, "-quiet", "-format", "UDTO", "-o", base})) {
        *err = "POMPPC couldn't convert this .dmg.";
        return false;
    }
    fs::rename(base + ".cdr", out, ec);
    if (ec) { *err = "POMPPC couldn't save the disc."; return false; }
    return true;
}

bool buildExchangeDisk(const std::vector<std::string>& paths, const std::string& mountDir,
                       const std::string& out, std::string* err) {
#ifdef __APPLE__
    std::error_code ec;
    uintmax_t bytes = 0;
    for (const std::string& p : paths) {
        if (fs::is_directory(p, ec)) {
            for (const auto& e : fs::recursive_directory_iterator(p, ec))
                if (e.is_regular_file(ec)) bytes += e.file_size(ec);
        } else {
            const uintmax_t size = fs::file_size(p, ec);
            if (ec) {
                *err = "POMPPC couldn't read " + fs::path(p).filename().string() + ".";
                return false;
            }
            bytes += size;
        }
    }
    // Les fichiers, l'en-tête du système de fichiers, et de la place pour répondre.
    const uintmax_t mb = bytes / (1024 * 1024) + bytes / (1024 * 1024) / 8 + 64;
    fs::remove(out, ec);
    if (!SetupFlow::runTool({"hdiutil", "create", "-quiet", "-size", std::to_string(mb) + "m", "-fs", "HFS+",
                             "-layout", "NONE", "-volname", "From Mac", "-ov", out})) {
        *err = "POMPPC couldn't create the disk.";
        return false;
    }
    fs::create_directories(mountDir, ec);
    if (!SetupFlow::runTool({"hdiutil", "attach", "-quiet", "-nobrowse", "-mountpoint", mountDir, out})) {
        *err = "POMPPC couldn't prepare the disk.";
        fs::remove(out, ec);
        return false;
    }
    bool ok = true;
    for (const std::string& p : paths) {
        if (!SetupFlow::runTool({"cp", "-R", p, mountDir + "/"})) {
            *err = "POMPPC couldn't copy " + fs::path(p).filename().string() + ".";
            ok = false;
            break;
        }
    }
    SetupFlow::runTool({"hdiutil", "detach", "-quiet", mountDir});
    fs::remove(mountDir, ec);
    if (!ok) fs::remove(out, ec);
    return ok;
#else
    (void)paths; (void)mountDir; (void)out;
    *err = "Dropping files isn't available on this host yet.";
    return false;
#endif
}

int effectiveMemoryMb(int wantedMb, bool sound) {
    return sound ? std::min(wantedMb, 768) : wantedMb;
}

int compareVersions(const std::string& a, const std::string& b) {
    auto parse = [](std::string s, std::vector<long>* out) {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        if (s.empty()) return false;
        std::stringstream in(s);
        std::string part;
        while (std::getline(in, part, '.')) {
            if (part.empty() || !std::all_of(part.begin(), part.end(), ::isdigit)) return false;
            out->push_back(std::strtol(part.c_str(), nullptr, 10));
        }
        return true;
    };
    std::vector<long> x, y;
    if (!parse(a, &x) || !parse(b, &y)) return 0;
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const long l = i < x.size() ? x[i] : 0, r = i < y.size() ? y[i] : 0;
        if (l != r) return l < r ? -1 : 1;
    }
    return 0;
}

std::string parseLatestTag(const std::string& json) {
    const std::string key = "\"tag_name\"";
    size_t p = json.find(key);
    if (p == std::string::npos) return "";
    p = json.find('"', p + key.size());
    if (p == std::string::npos) return "";
    const size_t e = json.find('"', p + 1);
    return e == std::string::npos ? "" : json.substr(p + 1, e - p - 1);
}

BackgroundTask::~BackgroundTask() {
    if (thread_.joinable()) thread_.join();
}

void BackgroundTask::start(std::function<std::string(std::string*)> work) {
    if (thread_.joinable()) thread_.join();
    result_.clear();
    err_.clear();
    pending_ = true;
    running_.store(true);
    thread_ = std::thread([this, work = std::move(work)] {
        result_ = work(&err_);
        running_.store(false);
    });
}

bool BackgroundTask::poll(std::string* result, std::string* err) {
    if (!pending_ || running_.load()) return false;
    if (thread_.joinable()) thread_.join();
    pending_ = false;
    *result = result_;
    *err = err_;
    return true;
}
