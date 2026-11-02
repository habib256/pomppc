// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// Le dossier partagé : décisions, aller et retour par une vraie image HFS+, sans ImGui ni VM.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utime.h>

#include "SetupFlow.h"
#include "SharedFolder.h"

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void write(const std::string& p, const std::string& t) { std::ofstream(p) << t; }
static std::string read(const std::string& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
// Une date passée, comme un fichier qu'on n'a pas touché depuis longtemps.
static void age(const std::string& p) {
    struct utimbuf t{1700000000, 1700000000};
    utime(p.c_str(), &t);
}

int main() {
    using A = SyncAction;
    //                 manifeste image  chgé-i mac    chgé-m suppr
    CHECK(decideSync(false, true,  false, false, false, true) == A::CopyToMac);    // nouveau dans l'invité
    CHECK(decideSync(false, true,  false, true,  false, true) == A::KeepBoth);     // nouveau des deux côtés
    CHECK(decideSync(true,  true,  false, true,  false, true) == A::None);         // personne n'a touché
    CHECK(decideSync(true,  true,  false, true,  true,  true) == A::None);         // le Mac seul : il a raison
    CHECK(decideSync(true,  true,  true,  true,  false, true) == A::CopyToMac);    // l'invité seul
    CHECK(decideSync(true,  true,  true,  true,  true,  true) == A::KeepBoth);     // les deux
    CHECK(decideSync(true,  true,  true,  false, false, true) == A::CopyToMac);    // changé ici, supprimé là
    CHECK(decideSync(true,  false, false, true,  false, true) == A::MoveToTrash);  // supprimé dans l'invité
    CHECK(decideSync(true,  false, false, true,  false, false) == A::None);        // arrêt brutal : on ne supprime pas
    CHECK(decideSync(true,  false, false, true,  true,  true) == A::None);         // changé sur le Mac : il reste
    CHECK(decideSync(true,  false, false, false, false, true) == A::None);         // parti des deux côtés

    CHECK(sharedVolumeName("/Users/a/Echanges") == "Echanges");
    CHECK(sharedVolumeName("/Users/a/Echanges/") == "Echanges");
    CHECK(sharedVolumeName("/Users/a/a:b") == "ab");
    CHECK(sharedVolumeName("/").find("Mac") == 0);
    CHECK(sharedVolumeName("/x/" + std::string(40, 'n')).size() == 27);
    CHECK(sharedVolumeName("/x/" + std::string(26, 'n') + "é").size() == 26);   // pas au milieu d'un caractère

    {
        const std::string q =
            "{\"return\": [{\"device\": \"ide0-hd0\", \"inserted\": {\"file\": \"a}b\\\"[\"}, \"removable\": false},"
            "{\"qdev\": \"/machine/peripheral/autre/autre.0/legacy[0]\", \"tray_open\": true},"
            "{\"inserted\": {\"x\": {\"y\": 1}}, \"qdev\": \"/machine/peripheral/moi/moi.0/legacy[0]\", \"tray_open\": false}], \"id\": 3}";
        CHECK(usbDiskEjected(q, "autre"));
        CHECK(!usbDiskEjected(q, "moi"));     // l'éjection d'un autre disque ne compte pas
        CHECK(!usbDiskEjected(q, "absent"));
        CHECK(!usbDiskEjected("", "moi"));
    }

    char tmpl[] = "/tmp/pomppc-shared-XXXXXX";
    const std::string root = fs::canonical(mkdtemp(tmpl)).string();
    const std::string folder = root + "/Mon Dossier", data = root + "/data";
    fs::create_directories(folder + "/sous");
    fs::create_directories(data);

    CHECK(sharedFolderProblem(folder, data).empty());
    CHECK(!sharedFolderProblem(root + "/absent", data).empty());
    CHECK(!sharedFolderProblem("/", data).empty());
    CHECK(!sharedFolderProblem(root, data).empty());             // contient les données de POMPPC
    CHECK(sharedFolderProblem(data + "/x", root + "/autre").size() > 0);   // n'existe pas

#ifdef __APPLE__
    setenv("POMPPC_TRASH", (root + "/trash").c_str(), 1);
    const std::string mnt = data + "/m", img = data + "/shared.dmg", man = data + "/shared.manifest";

    auto prepare = [&] {
        for (const char* n : {"garde.txt", "change.txt", "supprime.txt", "les-deux.txt", "mac-seul.txt", "mac-supprime.txt"})
            write(folder + "/" + n, std::string("origine ") + n);
        write(folder + "/été.txt", "accents");
        write(folder + "/sous/dedans.txt", "dedans");
        fs::create_directories(folder + "/sous/vide");
        write(folder + "/.DS_Store", "finder");
    };
    prepare();
    for (const auto& e : fs::recursive_directory_iterator(folder)) if (e.is_regular_file()) age(e.path().string());
    std::string err;
    CHECK(buildSharedDisk(folder, mnt, img, man, &err));
    CHECK(fs::exists(img) && fs::exists(man) && !fs::exists(mnt));
    CHECK(sharedManifestFolder(man) == folder);
    CHECK(read(folder + "/garde.txt") == "origine garde.txt");

    // Ce que ferait l'invité, écrit dans l'image comme lui.
    CHECK(SetupFlow::runTool({"hdiutil", "attach", "-quiet", "-nobrowse", "-mountpoint", mnt, img}));
    CHECK(fs::exists(mnt + "/sous/dedans.txt") && fs::is_directory(mnt + "/sous/vide"));
    write(mnt + "/change.txt", "changé par l'invité, plus long");
    write(mnt + "/les-deux.txt", "version de l'invité plus longue");
    write(mnt + "/nouveau.txt", "né dans l'invité");
    write(mnt + "/mac-seul.txt", std::string("origine mac-seul.txt"));
    age(mnt + "/mac-seul.txt");
    fs::create_directories(mnt + "/nouveau dossier/plus bas");
    write(mnt + "/nouveau dossier/plus bas/f.txt", "profond");
    write(mnt + "/mac-neuf.txt", "invité");
    fs::remove(mnt + "/supprime.txt");
    fs::remove(mnt + "/sous/dedans.txt");
    fs::remove(mnt + "/sous/vide");
    write(mnt + "/.DS_Store", "finder de l'invité");
    write(mnt + "/._change.txt", "double");
    fs::create_directories(mnt + "/.Trashes");
    fs::create_directories(mnt + "/Trash");                                // réservé à la racine
    fs::create_directories(mnt + "/sous/Trash");                           // ailleurs : à l'utilisateur
    write(mnt + "/sous/Trash/garde.txt", "à moi");
    CHECK(SetupFlow::runTool({"hdiutil", "detach", "-quiet", mnt}));

    // Ce que ferait l'utilisateur sur le Mac pendant ce temps.
    write(folder + "/les-deux.txt", "version du mac");
    write(folder + "/mac-seul.txt", "le mac a changé ceci");
    write(folder + "/mac-neuf.txt", "mac");
    write(folder + "/nouveau-mac.txt", "pas dans l'image");
    fs::remove(folder + "/mac-supprime.txt");

    SharedSyncReport rep;
    CHECK(syncSharedDisk(folder, mnt, img, man, true, "Tiger", &rep, &err));
    CHECK(!fs::exists(img) && !fs::exists(man) && !fs::exists(mnt));

    CHECK(read(folder + "/garde.txt") == "origine garde.txt");
    CHECK(read(folder + "/change.txt") == "changé par l'invité, plus long");
    CHECK(read(folder + "/nouveau.txt") == "né dans l'invité");
    CHECK(read(folder + "/nouveau dossier/plus bas/f.txt") == "profond");
    CHECK(read(folder + "/mac-seul.txt") == "le mac a changé ceci");       // le Mac seul : gardé
    CHECK(read(folder + "/les-deux.txt") == "version du mac");
    CHECK(read(folder + "/les-deux (from Tiger).txt") == "version de l'invité plus longue");
    CHECK(read(folder + "/mac-neuf.txt") == "mac");
    CHECK(read(folder + "/mac-neuf (from Tiger).txt") == "invité");
    CHECK(read(folder + "/nouveau-mac.txt") == "pas dans l'image");
    CHECK(read(folder + "/.DS_Store") == "finder");                        // celui du Mac n'est pas écrasé
    CHECK(read(folder + "/été.txt") == "accents");                         // accents : pas vus comme nouveaux
    CHECK(!fs::exists(folder + "/._change.txt") && !fs::exists(folder + "/.Trashes") && !fs::exists(folder + "/Trash"));
    CHECK(read(folder + "/sous/Trash/garde.txt") == "à moi");
    CHECK(!fs::exists(folder + "/mac-supprime.txt"));                      // supprimé sur le Mac : il ne revient pas
    CHECK(!fs::exists(folder + "/supprime.txt") && fs::exists(root + "/trash/supprime.txt"));   // Corbeille
    CHECK(!fs::exists(folder + "/sous/dedans.txt") && fs::exists(root + "/trash/dedans.txt"));
    CHECK(!fs::exists(folder + "/sous/vide") && fs::is_directory(folder + "/sous"));
    std::printf("%s\n", rep.summary().c_str());
    CHECK(rep.copied == 4 && rep.conflicts == 2 && rep.trashed == 3 && rep.failed == 0);   // change, nouveau, f, garde
    CHECK(rep.summary().find("4 files updated") == 0);
    CHECK(SharedSyncReport{}.summary() == "No changes.");

    // Arrêt brutal : les changements reviennent, rien n'est supprimé.
    for (const auto& e : fs::recursive_directory_iterator(folder)) if (e.is_regular_file()) age(e.path().string());
    CHECK(buildSharedDisk(folder, mnt, img, man, &err));
    CHECK(SetupFlow::runTool({"hdiutil", "attach", "-quiet", "-nobrowse", "-mountpoint", mnt, img}));
    fs::remove(mnt + "/garde.txt");
    write(mnt + "/apres.txt", "avant la panne");
    CHECK(SetupFlow::runTool({"hdiutil", "detach", "-quiet", mnt}));
    SharedSyncReport brutal;
    CHECK(syncSharedDisk(folder, mnt, img, man, false, "Tiger", &brutal, &err));
    CHECK(fs::exists(folder + "/garde.txt") && read(folder + "/apres.txt") == "avant la panne");
    CHECK(brutal.trashed == 0 && brutal.copied == 1);

    // L'image reste quand on ne peut pas finir : rien n'est perdu.
    CHECK(buildSharedDisk(folder, mnt, img, man, &err));
    SharedSyncReport none;
    fs::rename(folder, folder + ".parti");
    CHECK(!syncSharedDisk(folder, mnt, img, man, true, "Tiger", &none, &err) && !err.empty());
    CHECK(fs::exists(img) && fs::exists(man));
    fs::rename(folder + ".parti", folder);
    CHECK(syncSharedDisk(folder, mnt, img, man, true, "Tiger", &none, &err));

    // Un manifeste tronqué n'autorise aucune déduction.
    CHECK(buildSharedDisk(folder, mnt, img, man, &err));
    { std::ofstream t(man, std::ios::app); t << "tronque\tf\n"; }
    SharedSyncReport bad;
    CHECK(!syncSharedDisk(folder, mnt, img, man, true, "Tiger", &bad, &err));
    CHECK(fs::exists(img) && fs::exists(folder + "/garde.txt"));
    fs::remove(img);
    fs::remove(man);

    CHECK(!buildSharedDisk(root + "/absent", mnt, img, man, &err) && !err.empty());
    CHECK(!fs::exists(img));
#endif

    fs::remove_all(root);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
