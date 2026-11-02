// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
#include "SetupFlow.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "DiskConfig.h"

extern char** environ;
namespace fs = std::filesystem;

namespace {

const char* kTigerPage = "https://macintoshgarden.org/apps/mac-osx-mac-os-10-ppc";
const char* kTigerFile = "tiger-10-4-6-ppc.iso";
const char* kTigerMd5 = "4764d50fecb9f9370b626fc146f09954";
const char* kOs9Page = "https://macintoshgarden.org/apps/mac-os-922-universal";
const char* kOs9File = "macos-922-uni.zip";
const char* kOs9Md5 = "5ba031dfd678a74b9dee414af93ea514";

const char* stepName(SetupFlow::Step s) {
    switch (s) {
        case SetupFlow::Step::System: return "system";
        case SetupFlow::Step::Disk: return "disk";
        case SetupFlow::Step::Disc: return "disc";
        case SetupFlow::Step::Installing: return "installing";
        case SetupFlow::Step::InstallShort: return "install-short";
        case SetupFlow::Step::UpdateOffer: return "update-offer";
        case SetupFlow::Step::Updating: return "updating";
        case SetupFlow::Step::Driver: return "driver";
        case SetupFlow::Step::Finished: return "finished";
        case SetupFlow::Step::Off: break;
    }
    return "off";
}

bool endsWith(std::string s, const std::string& suffix) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string firstWord(const std::string& s) {
    return s.substr(0, s.find_first_of(" \t\r\n"));
}

}  // namespace

SetupFlow::SetupFlow(std::string root, std::string runtimeDir)
    : root_(std::move(root)), runtimeDir_(std::move(runtimeDir)) {}

SetupFlow::~SetupFlow() {
    cancelJob();
    if (job_ && job_->thread.joinable()) job_->thread.join();
}

bool SetupFlow::wantsVm() const {
    return step_ == Step::Installing || step_ == Step::Updating || step_ == Step::Driver ||
           step_ == Step::Finished;
}

void SetupFlow::save() const {
    if (step_ == Step::Off) return;
    std::ofstream out(runtimeDir_ + "/setup.state");
    out << stepName(step_) << ' ' << (tiger_ ? "tiger" : "os9") << '\n';
}

bool SetupFlow::load() {
    std::ifstream in(runtimeDir_ + "/setup.state");
    std::string name, os;
    if (!(in >> name >> os)) return false;
    tiger_ = os != "os9";
    for (Step s : {Step::System, Step::Disk, Step::Disc, Step::Installing, Step::InstallShort,
                   Step::UpdateOffer, Step::Updating, Step::Driver, Step::Finished}) {
        if (name == stepName(s)) { step_ = s; return true; }
    }
    return false;
}

void SetupFlow::begin() { go(Step::System); }

void SetupFlow::go(Step s) {
    step_ = s;
    problem_.clear();
    md5Mismatch_ = false;
    save();
}

void SetupFlow::end() {
    step_ = Step::Off;
    std::error_code ec;
    fs::remove(runtimeDir_ + "/setup.state", ec);
}

int SetupFlow::stepNumber() const {
    switch (step_) {
        case Step::System: case Step::Disk: return 1;
        case Step::Disc: return 2;
        case Step::Installing: case Step::InstallShort: return 3;
        case Step::UpdateOffer: case Step::Updating: return 4;
        case Step::Driver: return 5;
        default: return stepCount();
    }
}

std::string SetupFlow::diskPath() const {
    return root_ + (tiger_ ? "/disks/tiger.qcow2" : "/disks/os9.qcow2");
}
std::string SetupFlow::discPath() const {
    return root_ + (tiger_ ? "/images/MacOSX.4.iso" : "/disks/os9-install.iso");
}
std::string SetupFlow::discPage() const { return tiger_ ? kTigerPage : kOs9Page; }
std::string SetupFlow::discFile() const { return tiger_ ? kTigerFile : kOs9File; }
std::string SetupFlow::discMd5() const { return tiger_ ? kTigerMd5 : kOs9Md5; }
std::string SetupFlow::downloadedDisc() const {
    const char* forced = std::getenv("POMPPC_DOWNLOADS");
    const char* home = std::getenv("HOME");
    return (forced ? std::string(forced) : std::string(home ? home : "") + "/Downloads") + "/" + discFile();
}

std::vector<std::string> SetupFlow::sizeChoices() const {
    if (tiger_) return {"16", "32", "64", "128"};
    return {"8", "2", "16", "32"};
}

std::vector<std::string> SetupFlow::vmEnv() const {
    switch (step_) {
        case Step::Installing:
            // Un cœur : le chemin stable, celui de l'installation d'origine (scripts/10-install.sh).
            if (tiger_) return {"POMPPC_INSTALL_CD=" + discPath(), "POMPPC_NO_REBOOT=1", "SMP=1"};
            return {"POMPPC_OS9_MODE=install", "POMPPC_NO_REBOOT=1"};
        case Step::Updating:
            return {"POMPPC_CD=" + updatePath(), "POMPPC_NO_REBOOT=1"};
        default:
            return {};
    }
}

bool SetupFlow::diskLooksInstalled() const {
    std::string out;
    if (!runTool({"du", "-k", diskPath()}, &out)) return false;
    const long kb = std::strtol(out.c_str(), nullptr, 10);
    return kb > (tiger_ ? 1024L * 1024L : 200L * 1024L);
}

void SetupFlow::onVmExit() {
    std::error_code ec;
    switch (step_) {
        case Step::Installing:
            if (!diskLooksInstalled()) go(Step::InstallShort);
            else if (tiger_) go(Step::UpdateOffer);
            else go(Step::Finished);
            break;
        case Step::Updating:
            // L'invité a redémarré pour finir le combo : la tablette de 10.4.11 vaut désormais.
            fs::remove(marginPath(), ec);
            fs::remove(updatePath(), ec);
            go(Step::Driver);
            break;
        default:
            break;
    }
}

bool SetupFlow::createNewDisk(int gigabytes, bool ssd, std::string* err) {
    const std::string name = tiger_ ? "tiger" : "os9";
    if (gigabytes < minimumGb()) {
        *err = osName() + " needs a disk of at least " + std::to_string(minimumGb()) + " GB.";
        return false;
    }
    if (!createDisk(root_, name, gigabytes, ssd, err)) return false;
    // Avant le combo, Tiger n'a pas la marge de tablette de 10.4.11 : le pointeur serait décalé
    // (docs/tablette-tiger-10.4.11.md). onVmExit la rétablit une fois le combo passé.
    if (tiger_) std::ofstream(marginPath()) << "0\n";
    std::error_code ec;
    go(fs::exists(discPath(), ec) ? Step::Installing : Step::Disc);
    return true;
}

bool SetupFlow::importDisk(const std::string& path, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    if (!in.read(magic, 4) || magic[0] != 'Q' || magic[1] != 'F' || magic[2] != 'I') {
        *err = "This file is not a .qcow2 disk.";
        return false;
    }
    if (!runTool({"cp", "-c", path, diskPath()}) && !runTool({"cp", path, diskPath()})) {
        *err = "Copying the disk failed.";
        return false;
    }
    go(Step::Finished);
    return true;
}

// ── Outils ──────────────────────────────────────────────────────────────────

bool SetupFlow::runTool(const std::vector<std::string>& args, std::string* output, int* pidOut) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    int fds[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (output) {
        if (pipe(fds) != 0) return false;
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
    }
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (output) close(fds[1]);
    if (rc != 0) {
        if (output) close(fds[0]);
        return false;
    }
    if (pidOut) *pidOut = pid;
    if (output) {
        char buf[4096];
        ssize_t n;
        output->clear();
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) output->append(buf, (size_t)n);
        close(fds[0]);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool SetupFlow::downloadReady(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && fs::file_size(path, ec) > 0 &&
           !fs::exists(path + ".download", ec) && !fs::exists(path + ".crdownload", ec) &&
           !fs::exists(path + ".part", ec);
}

bool SetupFlow::placeMedia(const std::string& file, const std::string& out, bool move, std::string* err) {
    std::error_code ec;
    fs::create_directories(fs::path(out).parent_path(), ec);
    fs::remove(out, ec);
    if (endsWith(file, ".zip")) {
        // La plus grosse image de l'archive : celle du système, pas un fichier d'accompagnement.
        const std::string dir = runtimeDir_ + "/extraction";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        if (!runTool({"unzip", "-q", "-o", file, "-d", dir})) {
            *err = "POMPPC couldn't open this archive.";
            fs::remove_all(dir, ec);
            return false;
        }
        fs::path best;
        uintmax_t bestSize = 0;
        for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string p = e.path().string();
            if (p.find("/__MACOSX/") != std::string::npos) continue;
            if (!endsWith(p, ".iso") && !endsWith(p, ".toast") && !endsWith(p, ".cdr") &&
                !endsWith(p, ".img") && !endsWith(p, ".dmg")) continue;
            const uintmax_t size = e.file_size(ec);
            if (size >= bestSize) { best = e.path(); bestSize = size; }
        }
        bool ok = false;
        if (best.empty()) *err = "POMPPC found no disc image in this file.";
        else ok = placeMedia(best.string(), out, true, err);
        fs::remove_all(dir, ec);
        return ok;
    }
    if (endsWith(file, ".dmg")) {
        const std::string base = out.substr(0, out.find_last_of('.'));
        fs::remove(base + ".cdr", ec);
        if (!runTool({"hdiutil", "convert", file, "-quiet", "-format", "UDTO", "-o", base})) {
            *err = "POMPPC couldn't convert this .dmg.";
            return false;
        }
        fs::rename(base + ".cdr", out, ec);
        if (ec) { *err = "POMPPC couldn't save the disc."; return false; }
        return true;
    }
    if (move) {
        fs::rename(file, out, ec);
        if (ec) {   // autre volume
            ec.clear();
            fs::copy_file(file, out, ec);
            if (ec) { *err = "POMPPC couldn't save the disc."; return false; }
            fs::remove(file, ec);
        }
        return true;
    }
    fs::create_symlink(file, out, ec);
    if (ec) { *err = "POMPPC couldn't use this file."; return false; }
    return true;
}

// ── Travaux longs ───────────────────────────────────────────────────────────

void SetupFlow::startJob(const std::string& text, const std::string& watchFile, uint64_t watchBytes,
                         std::function<void(Job&)> work) {
    finishJob();
    job_ = std::make_unique<Job>();
    job_->text = text;
    job_->watchFile = watchFile;
    job_->watchBytes = watchBytes;
    Job* j = job_.get();
    j->thread = std::thread([j, work = std::move(work)] {
        work(*j);
        j->running.store(false);
    });
}

void SetupFlow::poll() {
    if (!job_ || job_->running.load() || job_->applied) return;
    if (job_->thread.joinable()) job_->thread.join();
    job_->applied = true;
    if (job_->next != Step::Off) go(job_->next);
    else problem_ = job_->error;
    md5Mismatch_ = job_->md5Bad;
}

void SetupFlow::finishJob() {
    if (job_ && job_->thread.joinable()) job_->thread.join();
    job_.reset();
}

std::string SetupFlow::busyText() const { return job_ ? job_->text : std::string(); }

float SetupFlow::progress() const {
    if (!busy() || job_->watchFile.empty() || job_->watchBytes == 0) return -1.0f;   // fixés au départ
    std::error_code ec;
    const uintmax_t now = fs::file_size(job_->watchFile, ec);
    if (ec) return 0.0f;
    return std::min(1.0f, (float)now / (float)job_->watchBytes);
}

void SetupFlow::cancelJob() {
    if (!busy()) return;
    job_->cancelled.store(true);
    const int pid = job_->pid.load();
    if (pid > 0) kill(pid, SIGTERM);
}

bool SetupFlow::takeChosen(std::string* path) {
    if (!job_ || busy() || !job_->hasChosen) return false;   // chemin vide : annulé
    *path = job_->chosen;
    job_->hasChosen = false;
    return true;
}

void SetupFlow::chooseFile(const std::string& prompt) {
    startJob("Waiting for your choice…", "", 0, [prompt](Job& j) {
        // POMPPC_SETUP_CHOOSE : le fichier d'une épreuve scriptée, sans fenêtre de macOS.
        if (const char* forced = std::getenv("POMPPC_SETUP_CHOOSE")) {
            j.chosen = forced;
        } else {
            std::string out;
            if (runTool({"osascript", "-e", "on run argv",
                         "-e", "return POSIX path of (choose file with prompt (item 1 of argv))",
                         "-e", "end run", "--", prompt}, &out)) {
                out.erase(out.find_last_not_of("\r\n") + 1);
                j.chosen = out;
            }
        }
        j.hasChosen = true;
    });
}

void SetupFlow::placeDisc(const std::string& file, bool move) {
    const std::string out = discPath();
    startJob("Preparing the installation disc…", "", 0, [this, file, out, move](Job& j) {
        if (placeMedia(file, out, move, &j.error)) j.next = Step::Installing;
    });
}

void SetupFlow::checkAndPlaceDownload() {
    const std::string file = downloadedDisc(), sum = discMd5(), out = discPath();
    startJob("Checking the download…", "", 0, [this, file, sum, out](Job& j) {
        std::string got;
        runTool({"md5", "-q", file}, &got);
        if (j.cancelled.load()) return;
        if (firstWord(got) != sum) { j.md5Bad = true; return; }
        if (placeMedia(file, out, true, &j.error)) j.next = Step::Installing;
    });
}

void SetupFlow::acceptMismatch() {
    md5Mismatch_ = false;
    placeDisc(downloadedDisc(), true);
}

void SetupFlow::downloadUpdate() {
    const std::string dmg = root_ + "/disks/MacOSXUpdCombo10.4.11PPC.dmg", iso = updatePath();
    const std::string url = updateUrl, sha = updateSha1;
    startJob("Downloading the 10.4.11 update from Apple…", dmg + ".tmp", kUpdateBytes, [this, dmg, iso, url, sha](Job& j) {
        std::error_code ec;
        if (!fs::exists(dmg, ec)) {
            const std::string tmp = dmg + ".tmp";
            std::vector<char*> argv;
            const std::vector<std::string> args = {"curl", "-fsSL", "--retry", "3", "-o", tmp, url};
            for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            pid_t pid = -1;
            if (posix_spawnp(&pid, "curl", nullptr, nullptr, argv.data(), environ) != 0) {
                j.error = "POMPPC couldn't start the download.";
                return;
            }
            j.pid.store(pid);
            int status = 0;
            waitpid(pid, &status, 0);
            j.pid.store(-1);
            if (j.cancelled.load()) { fs::remove(tmp, ec); return; }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                fs::remove(tmp, ec);
                j.error = "The download didn't finish. Check your connection and try again.";
                return;
            }
            std::string got;
            runTool({"shasum", "-a", "1", tmp}, &got);
            if (firstWord(got) != sha) {
                fs::remove(tmp, ec);
                j.error = "The downloaded file doesn't match Apple's checksum. Try again.";
                return;
            }
            fs::rename(tmp, dmg, ec);
        }
        if (!placeMedia(dmg, iso, false, &j.error)) return;
        fs::remove(dmg, ec);
        j.next = Step::Updating;
    });
}
