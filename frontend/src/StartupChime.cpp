/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
#include "StartupChime.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>

void ChimeSettings::load(const std::string& path) {
    std::ifstream in(path);
    int flag;
    float level;
    std::string name;
    if (in >> flag >> level >> std::quoted(name) &&
        (flag == 0 || flag == 1) && std::isfinite(level)) {
        enabled = flag != 0;
        volume = std::clamp(level, 0.0f, 1.0f);
        file = name;
    }
}

bool ChimeSettings::save(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    std::ofstream out(tmp);
    out << enabled << ' ' << volume << ' ' << std::quoted(file) << '\n';
    out.close();
    if (!out || std::rename(tmp.c_str(), path.c_str()) != 0) return false;
    return true;
}

#ifndef __APPLE__
// Do not pretend to have played a sound on an unsupported host.
struct ChimePlayer::Impl {};
ChimePlayer::ChimePlayer() : impl_(new Impl) {}
ChimePlayer::~ChimePlayer() = default;
bool ChimePlayer::play(const std::string&, float, std::string& error) {
    error = "lecture du carillon disponible sur hôte macOS uniquement";
    return false;
}
void ChimePlayer::stop() {}
void ChimePlayer::volume(float) {}
#endif
