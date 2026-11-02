// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// L'assistant de premier lancement : étapes, reprise, médias, téléchargement, sans ImGui ni VM.
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "SetupFlow.h"

namespace fs = std::filesystem;
using Step = SetupFlow::Step;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void wait(SetupFlow& s) {
    for (int i = 0; i < 3000 && s.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    s.poll();
}
static void write(const std::string& p, const std::string& text) { std::ofstream(p) << text; }
static bool hasEnv(const std::vector<std::string>& env, const std::string& v) {
    for (const std::string& e : env) if (e == v) return true;
    return false;
}

int main() {
    char tmpl[] = "/tmp/pomppc-setup-XXXXXX";
    const std::string root = mkdtemp(tmpl);
    const std::string run = root + "/.run";
    fs::create_directories(run);
    fs::create_directories(root + "/disks");
    fs::create_directories(root + "/dl");
    setenv("POMPPC_DOWNLOADS", (root + "/dl").c_str(), 1);
    // qemu-img factice : assez d'en-tête pour que listDisks y voie un qcow2.
    write(root + "/config.env", "qemu-img() { printf 'QFI\\373' > \"$5\"; dd if=/dev/zero bs=1 count=28 >> \"$5\" 2>/dev/null; }\n");

    {   // Étapes, reprise après avoir quitté
        SetupFlow s(root, run);
        CHECK(!s.load() && !s.active());
        s.begin();
        CHECK(s.step() == Step::System && !s.wantsVm());
        s.setTiger(false);
        s.go(Step::Disc);
        CHECK(s.stepCount() == 3 && s.stepNumber() == 2);
        SetupFlow r(root, run);
        CHECK(r.load() && r.step() == Step::Disc && !r.tiger());
        r.setTiger(true);
        CHECK(r.stepCount() == 5);
        r.end();
        SetupFlow e(root, run);
        CHECK(!e.load());
    }
    {   // Disque neuf
        SetupFlow s(root, run);
        s.begin();
        std::string err;
        CHECK(!s.createNewDisk(4, false, &err) && !err.empty());   // trop petit pour Tiger
        CHECK(!fs::exists(s.diskPath()));
        CHECK(s.createNewDisk(16, true, &err));
        CHECK(s.step() == Step::Disc);
        CHECK(fs::exists(s.diskPath()) && fs::exists(s.diskPath() + ".type"));
        std::ifstream m(s.marginPath());
        int margin = -1;
        CHECK((m >> margin) && margin == 0);       // pas de marge de tablette avant le combo
        std::string err2;
        CHECK(!s.createNewDisk(16, false, &err2));  // jamais par-dessus un disque existant
        fs::remove(s.diskPath() + ".type");
        s.setTiger(false);
        CHECK(s.createNewDisk(2, false, &err));
        CHECK(!fs::exists(s.marginPath()));         // OS 9 : pas de tablette USB à régler
        fs::remove(s.diskPath());
        s.end();
    }
    {   // Disque existant
        SetupFlow s(root, run);
        s.begin();
        s.setTiger(false);
        write(root + "/pasun.qcow2", "bonjour tout le monde");
        std::string err;
        CHECK(!s.importDisk(root + "/pasun.qcow2", &err) && !err.empty());
        CHECK(!fs::exists(s.diskPath()));
        write(root + "/vrai.qcow2", "QFI\xfb reste");
        CHECK(s.importDisk(root + "/vrai.qcow2", &err));
        CHECK(s.step() == Step::Finished && fs::exists(s.diskPath()));
        fs::remove(s.diskPath());
        s.end();
    }
    {   // Variables du lanceur, étape par étape
        SetupFlow s(root, run);
        s.begin();
        s.go(Step::Installing);
        auto env = s.vmEnv();
        CHECK(hasEnv(env, "POMPPC_INSTALL_CD=" + s.discPath()) && hasEnv(env, "POMPPC_NO_REBOOT=1") && hasEnv(env, "SMP=1"));
        s.go(Step::Updating);
        env = s.vmEnv();
        CHECK(hasEnv(env, "POMPPC_CD=" + s.updatePath()) && hasEnv(env, "POMPPC_NO_REBOOT=1"));
        s.go(Step::Driver);
        CHECK(s.vmEnv().empty() && s.wantsVm());
        s.setTiger(false);
        s.go(Step::Installing);
        env = s.vmEnv();
        CHECK(hasEnv(env, "POMPPC_OS9_MODE=install") && hasEnv(env, "POMPPC_NO_REBOOT=1"));
        s.end();
    }
    {   // La sortie de QEMU fait avancer
        SetupFlow s(root, run);
        s.begin();
        write(s.diskPath(), "petit");
        s.go(Step::Installing);
        s.onVmExit();
        CHECK(s.step() == Step::InstallShort);        // rien d'installé
        // Disque écrit : 1,5 Go réellement alloués, l'heuristique lit du.
        {
            std::ofstream big(s.diskPath(), std::ios::binary);
            std::vector<char> mio(1024 * 1024, 'x');
            for (int i = 0; i < 1100; ++i) big.write(mio.data(), (std::streamsize)mio.size());
        }
        s.go(Step::Installing);
        s.onVmExit();
        CHECK(s.step() == Step::UpdateOffer);
        write(s.updatePath(), "cd");
        write(s.marginPath(), "0\n");
        s.go(Step::Updating);
        s.onVmExit();
        CHECK(s.step() == Step::Driver);
        CHECK(!fs::exists(s.updatePath()) && !fs::exists(s.marginPath()));
        fs::remove(s.diskPath());
        s.setTiger(false);
        { std::ofstream big(s.diskPath(), std::ios::binary); std::vector<char> mio(1024 * 1024, 'x');
          for (int i = 0; i < 250; ++i) big.write(mio.data(), (std::streamsize)mio.size()); }
        s.go(Step::Installing);
        s.onVmExit();
        CHECK(s.step() == Step::Finished);            // OS 9 : ni combo ni pilote
        fs::remove(s.diskPath());
        s.end();
    }
    {   // Téléchargement du navigateur
        const std::string f = root + "/dl/x.iso";
        CHECK(!SetupFlow::downloadReady(f));
        write(f, "");
        CHECK(!SetupFlow::downloadReady(f));
        write(f, "contenu");
        CHECK(SetupFlow::downloadReady(f));
        for (const char* suffix : {".download", ".crdownload", ".part"}) {
            write(f + suffix, "");
            CHECK(!SetupFlow::downloadReady(f));
            fs::remove(f + suffix);
        }
        fs::remove(f);
    }
    {   // Médias
        SetupFlow s(root, run);
        const std::string out = root + "/images/sortie.iso";
        std::string err;
        write(root + "/mien.iso", "image");
        CHECK(s.placeMedia(root + "/mien.iso", out, false, &err));
        CHECK(fs::is_symlink(out) && fs::exists(root + "/mien.iso"));   // le fichier de l'utilisateur reste
        write(root + "/dep.iso", "image");
        CHECK(s.placeMedia(root + "/dep.iso", out, true, &err));
        CHECK(!fs::is_symlink(out) && !fs::exists(root + "/dep.iso"));
        // Zip : la plus grosse image, hors __MACOSX
        const std::string z = root + "/z";
        fs::create_directories(z + "/sous/__MACOSX");
        write(z + "/sous/grand.iso", std::string(5000, 'a'));
        write(z + "/petit.img", "p");
        write(z + "/sous/__MACOSX/faux.iso", std::string(9000, 'b'));
        write(z + "/lisez.txt", "t");
        CHECK(SetupFlow::runTool({"sh", "-c", "cd '" + z + "' && zip -qr '" + root + "/a.zip' ."}));
        CHECK(s.placeMedia(root + "/a.zip", out, false, &err));
        CHECK(fs::file_size(out) == 5000);
        CHECK(!fs::exists(run + "/extraction"));
        CHECK(SetupFlow::runTool({"sh", "-c", "cd '" + z + "' && zip -qr '" + root + "/v.zip' lisez.txt"}));
        CHECK(!s.placeMedia(root + "/v.zip", out, false, &err) && !err.empty());
#ifdef __APPLE__
        if (SetupFlow::runTool({"hdiutil", "create", "-quiet", "-size", "1m", "-fs", "HFS+", "-volname", "t", root + "/t.dmg"})) {
            CHECK(s.placeMedia(root + "/t.dmg", out, false, &err));
            CHECK(fs::file_size(out) > 0 && !fs::exists(root + "/images/sortie.cdr"));
        }
#endif
    }
    {   // Disque choisi dans le navigateur : MD5 bon, MD5 faux
        SetupFlow s(root, run);
        s.begin();
        fs::create_directories(root + "/images");
        write(s.downloadedDisc(), "faux");
        s.go(Step::Disc);
        s.checkAndPlaceDownload();
        wait(s);
        CHECK(s.md5Mismatch() && s.step() == Step::Disc);
        CHECK(fs::exists(s.downloadedDisc()));           // rien de déplacé sans accord
        s.acceptMismatch();
        wait(s);
        CHECK(s.step() == Step::Installing && fs::exists(s.discPath()));
        fs::remove(s.discPath());
        s.end();
    }
    {   // Combo : SHA-1 faux, puis bon (fichier local servi par curl)
        SetupFlow s(root, run);
        s.begin();
        write(root + "/combo.iso", "contenu du combo");
        s.updateUrl = "file://" + root + "/combo.iso";
        s.go(Step::UpdateOffer);
        s.updateSha1 = "0000";
        s.downloadUpdate();
        wait(s);
        CHECK(s.step() == Step::UpdateOffer && !s.problem().empty());
        CHECK(!fs::exists(root + "/disks/MacOSXUpdCombo10.4.11PPC.dmg.tmp"));
        CHECK(!fs::exists(s.updatePath()));
        std::string sum;
        CHECK(SetupFlow::runTool({"shasum", "-a", "1", root + "/combo.iso"}, &sum));
        s.updateSha1 = sum.substr(0, 40);
        s.downloadUpdate();
        wait(s);
        // Un faux .dmg : hdiutil le refuse, mais la somme est passée et le problème est dit.
        CHECK(s.step() == Step::UpdateOffer && !s.problem().empty());
        fs::remove(root + "/disks/MacOSXUpdCombo10.4.11PPC.dmg");
        s.end();
    }
    fs::remove_all(root);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
