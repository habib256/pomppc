// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// settings_test — FrontendSettings : aller-retour, défauts, fichier abîmé.
#include "FrontendSettings.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

static std::string tmpPath(const char* tag) {
    return std::string("/tmp/pomppc-settings-") + tag + "-" + std::to_string(getpid()) + ".conf";
}

int main() {
    // Fichier absent : load rend faux, défauts intacts.
    {
        FrontendSettings s;
        assert(!s.load(tmpPath("absent")));
        assert(s.os == "os9" && s.sound && s.pad && s.fitView && s.winW == 0);
    }
    // Aller-retour complet.
    {
        const std::string p = tmpPath("rt");
        FrontendSettings a;
        a.os = "tiger"; a.sound = false; a.pad = false; a.grabbed = false;
        a.preferTablet = false; a.fitView = false; a.zoom = 1.5f;
        a.showLibrary = false; a.showBilan = false; a.showJournal = true;
        a.fullscreen = true; a.winX = -40; a.winY = 25; a.winW = 1440; a.winH = 900;
        a.sharedTiger = "/Users/a/Mon Dossier = 1"; a.sharedOs9 = "/Volumes/Été/x";
        assert(a.save(p));
        FrontendSettings b;
        assert(b.load(p));
        assert(b.os == "tiger" && !b.sound && !b.pad && !b.grabbed && !b.preferTablet);
        assert(!b.fitView && b.zoom == 1.5f && !b.showLibrary && !b.showBilan && b.showJournal);
        assert(b.fullscreen && b.winX == -40 && b.winY == 25 && b.winW == 1440 && b.winH == 900);
        assert(b.sharedTiger == "/Users/a/Mon Dossier = 1" && b.sharedOs9 == "/Volumes/Été/x");
        std::remove(p.c_str());
    }
    // Fichier abîmé ou d'une autre version : on garde ce qui est lisible.
    {
        const std::string p = tmpPath("bad");
        {
            std::ofstream o(p);
            o << "# commentaire\n"
                 "os = beos\n"            // valeur inconnue : défaut gardé
                 "son = peut-etre\n"      // illisible : défaut gardé
                 "manette = 0\n"
                 "zoom = 9\n"             // borné à 2
                 "cle_future = 42\n"      // clé inconnue : ignorée
                 "ligne sans egal\n"
                 "fenetre_l = 100\n"      // géométrie absurde : oubliée
                 "fenetre_h = 50\n";
        }
        FrontendSettings s;
        assert(s.load(p));
        assert(s.os == "os9" && s.sound && !s.pad && s.zoom == 1.0f);  // sans disposition : zoom oublié
        assert(s.winW == 0 && s.winH == 0);
        std::remove(p.c_str());
    }
    // Réglages d'avant la disposition 2 : l'invité seul, à sa taille, une fois.
    {
        const std::string p = tmpPath("v1");
        { std::ofstream o(p); o << "ludotheque = 1\nbilan = 1\njournal = 1\nvue_ajustee = 0\nmanette = 0\n"
                                   "fenetre_l = 1280\nfenetre_h = 900\n"; }
        FrontendSettings s;
        assert(s.load(p));
        assert(!s.showLibrary && !s.showBilan && !s.showJournal && s.fitView && s.autoFit);
        assert(s.winW == 0 && !s.pad);
        FrontendSettings n;
        n.showJournal = true; n.autoFit = false; n.winW = 800; n.winH = 600;
        assert(n.save(p));
        FrontendSettings r;
        assert(r.load(p) && r.showJournal && !r.autoFit && r.winW == 800);
        std::remove(p.c_str());
    }
    std::puts("FrontendSettings : aller-retour, défauts et fichier abîmé OK");
    return 0;
}
