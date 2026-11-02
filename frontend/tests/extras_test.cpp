// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// Fichiers déposés, mémoire donnée à l'invité, versions, sans ImGui ni VM.
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "AppExtras.h"
#include "SetupFlow.h"

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main() {
    char tmpl[] = "/tmp/pomppc-extras-XXXXXX";
    const std::string root = mkdtemp(tmpl);

    CHECK(isDiscImage("/a/b.iso") && isDiscImage("x.CDR") && isDiscImage("y.Toast") && isDiscImage("z.dmg"));
    CHECK(!isDiscImage("a.txt") && !isDiscImage("iso") && !isDiscImage("a.iso.txt"));

    std::ofstream(root + "/d.iso") << "image";
    std::ofstream(root + "/n.txt") << "note";
    fs::create_directories(root + "/dossier.iso");
    CHECK(classifyDrop({}) == DropKind::Nothing);
    CHECK(classifyDrop({root + "/d.iso"}) == DropKind::DiscImage);
    CHECK(classifyDrop({root + "/n.txt"}) == DropKind::Files);
    CHECK(classifyDrop({root + "/d.iso", root + "/n.txt"}) == DropKind::Files);
    CHECK(classifyDrop({root + "/dossier.iso"}) == DropKind::Files);   // un dossier, pas une image

    std::string err;
    std::ofstream(root + "/dossier.iso/dedans.txt") << "dedans";
#ifdef __APPLE__
    const std::string disk = root + "/From Mac.dmg", mnt = root + "/mnt";
    CHECK(buildExchangeDisk({root + "/n.txt", root + "/dossier.iso"}, mnt, disk, &err));
    CHECK(!fs::exists(mnt) && fs::exists(root + "/n.txt"));              // le fichier de l'utilisateur reste
    CHECK(fs::file_size(disk) >= 64ULL * 1000 * 1000);                   // de la place pour répondre
    {   // le disque se relit, avec ses fichiers
        CHECK(SetupFlow::runTool({"hdiutil", "attach", "-quiet", "-nobrowse", "-mountpoint", mnt, disk}));
        CHECK(fs::exists(mnt + "/n.txt") && fs::exists(mnt + "/dossier.iso/dedans.txt"));
        SetupFlow::runTool({"hdiutil", "detach", "-quiet", mnt});
    }
    CHECK(!buildExchangeDisk({root + "/absent.txt"}, mnt, root + "/d2.dmg", &err) && !err.empty());
    CHECK(!fs::exists(root + "/d2.dmg"));
#endif

    CHECK(effectiveMemoryMb(1024, true) == 768);
    CHECK(effectiveMemoryMb(1024, false) == 1024);
    CHECK(effectiveMemoryMb(512, true) == 512);

    CHECK(compareVersions("0.4", "0.4.0") == 0);
    CHECK(compareVersions("0.4", "0.4.1") == -1);
    CHECK(compareVersions("v0.5", "0.4.9") == 1);
    CHECK(compareVersions("0.10", "0.9") == 1);
    CHECK(compareVersions("dev", "0.4") == 0);
    CHECK(compareVersions("", "0.4") == 0);
    CHECK(compareVersions("0.4-beta", "0.4") == 0);

    CHECK(parseLatestTag("{\"url\":\"x\",\"tag_name\":\"0.4\",\"name\":\"y\"}") == "0.4");
    CHECK(parseLatestTag("{\"tag_name\" : \"v1.2\"}") == "v1.2");
    CHECK(parseLatestTag("{\"message\":\"Not Found\"}").empty());
    CHECK(parseLatestTag("{\"tag_name\":").empty());

    BackgroundTask t;
    std::string r, e;
    CHECK(!t.poll(&r, &e));
    t.start([](std::string* err2) { *err2 = "non"; return std::string("oui"); });
    for (int i = 0; i < 400 && t.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(t.poll(&r, &e) && r == "oui" && e == "non");
    CHECK(!t.poll(&r, &e));                                            // une seule fois

    fs::remove_all(root);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
