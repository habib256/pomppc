// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
#include "FrontendSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

bool parseBool(const std::string& v, bool& out) {
    if (v == "1" || v == "oui" || v == "true") { out = true; return true; }
    if (v == "0" || v == "non" || v == "false") { out = false; return true; }
    return false;
}

bool parseInt(const std::string& v, int& out) {
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || *end != '\0' || n < -100000 || n > 100000) return false;
    out = (int)n;
    return true;
}

}  // namespace

bool FrontendSettings::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq));
        const std::string v = trim(line.substr(eq + 1));
        if (k == "os") {
            if (v == "os9" || v == "tiger") os = v;
        } else if (k == "son") parseBool(v, sound);
        else if (k == "manette") parseBool(v, pad);
        else if (k == "clavier_invite") parseBool(v, grabbed);
        else if (k == "souris_absolue") parseBool(v, preferTablet);
        else if (k == "vue_ajustee") parseBool(v, fitView);
        else if (k == "zoom") {
            char* end = nullptr;
            float z = std::strtof(v.c_str(), &end);
            if (!v.empty() && *end == '\0' && std::isfinite(z))
                zoom = std::clamp(z, 0.5f, 2.0f);
        } else if (k == "ludotheque") parseBool(v, showLibrary);
        else if (k == "bilan") parseBool(v, showBilan);
        else if (k == "journal") parseBool(v, showJournal);
        else if (k == "plein_ecran") parseBool(v, fullscreen);
        else if (k == "fenetre_x") parseInt(v, winX);
        else if (k == "fenetre_y") parseInt(v, winY);
        else if (k == "fenetre_l") parseInt(v, winW);
        else if (k == "fenetre_h") parseInt(v, winH);
    }
    // Une géométrie absurde (fichier édité à la main) retombe sur le défaut.
    if (winW < 320 || winH < 240 || winW > 16384 || winH > 16384) winW = winH = 0;
    return true;
}

bool FrontendSettings::save(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << "# Réglages d'ImGuiDock (POMPPC), écrits à l'arrêt du frontend.\n"
               "# Modifiables à la main ; la disposition des fenêtres est dans imgui.ini.\n"
            << "os = " << os << '\n'
            << "son = " << sound << '\n'
            << "manette = " << pad << '\n'
            << "clavier_invite = " << grabbed << '\n'
            << "souris_absolue = " << preferTablet << '\n'
            << "vue_ajustee = " << fitView << '\n'
            << "zoom = " << zoom << '\n'
            << "ludotheque = " << showLibrary << '\n'
            << "bilan = " << showBilan << '\n'
            << "journal = " << showJournal << '\n'
            << "plein_ecran = " << fullscreen << '\n';
        if (winW > 0 && winH > 0)
            out << "fenetre_x = " << winX << '\n'
                << "fenetre_y = " << winY << '\n'
                << "fenetre_l = " << winW << '\n'
                << "fenetre_h = " << winH << '\n';
        out.close();
        if (!out) { std::remove(tmp.c_str()); return false; }
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}
