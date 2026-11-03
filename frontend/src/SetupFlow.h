// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// SetupFlow — l'assistant de premier lancement : où on en est et ce que chaque étape demande.
//
// Sans ImGui ni QEMU, pour être testé seul (frontend/tests/setup_test.cpp). L'état est écrit à
// chaque étape dans .run/setup.state : quitter l'app ne fait rien perdre. SetupUi le dessine.
//
// Les étapes avec VM finissent seules : -no-reboot fait sortir QEMU quand l'invité redémarre.
//
// L'assistant ouvre la page du système sur Macintosh Garden et attend le fichier : il ne le
// télécharge pas lui-même. Mac OS reste sous droit d'auteur d'Apple, le téléchargement reste un
// geste de l'utilisateur (docs/publier-macos.md). Le combo 10.4.11, qu'Apple distribue encore
// gratuitement, est téléchargé directement.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class SetupFlow {
public:
    enum class Step {
        Off,          // pas d'assistant en cours
        System,       // Tiger ou OS 9
        Disk,         // nouveau disque, ou disque existant
        Disc,         // le CD d'installation
        Installing,   // VM : l'installateur (-no-reboot)
        InstallShort, // l'installateur a fini sans rien installer
        UpdateOffer,  // Tiger : proposer le combo
        Updating,     // VM : le combo (-no-reboot)
        Driver,       // VM : le pilote, jusqu'à ce que le device le voie
        Finished,     // VM : prêt
    };

    SetupFlow(std::string root, std::string runtimeDir);
    ~SetupFlow();

    Step step() const { return step_; }
    bool active() const { return step_ != Step::Off; }
    bool wantsVm() const;        // cette étape se joue dans la VM
    bool tiger() const { return tiger_; }
    void setTiger(bool tiger) { tiger_ = tiger; save(); }
    const std::string& problem() const { return problem_; }
    void clearProblem() { problem_.clear(); }

    bool load();                 // reprend un assistant interrompu ; faux s'il n'y en a pas
    void begin();                // Step::System
    void go(Step s);             // change d'étape et l'écrit
    void end();                  // fin, ou abandon : efface l'état

    // Numéro de l'étape et nombre total, pour l'affichage. Tiger : 5 ; OS 9 : 3.
    int stepNumber() const;
    int stepCount() const { return tiger_ ? 5 : 3; }
    std::string osName() const { return tiger_ ? "Mac OS X 10.4 Tiger" : "Mac OS 9.2.2"; }
    std::string launcher() const { return root_ + (tiger_ ? "/run_tiger.sh" : "/run_os9.sh"); }

    std::string diskPath() const;
    std::string discPath() const;     // où le lanceur attend le CD d'installation
    std::string updatePath() const { return root_ + "/disks/combo-10.4.11.iso"; }
    std::string marginPath() const { return diskPath() + ".tablet-margin"; }
    std::string discPage() const;     // la page de Macintosh Garden
    std::string discFile() const;     // le nom du fichier sur cette page
    std::string discMd5() const;      // le MD5 publié par le site
    std::string downloadedDisc() const;   // ce fichier dans Téléchargements
    std::vector<std::string> sizeChoices() const;   // en Go ; le premier est conseillé
    int minimumGb() const { return tiger_ ? 5 : 1; }

    // Variables pour le lanceur à cette étape.
    std::vector<std::string> vmEnv() const;
    // QEMU est sorti proprement pendant Installing ou Updating : l'invité a redémarré.
    void onVmExit();
    // À chaque image : applique le résultat d'un travail fini, dans le fil de l'interface.
    void poll();
    // Le disque semble installé : assez de données écrites (Tiger : 1 Go ; OS 9 : 200 Mio).
    bool diskLooksInstalled() const;

    bool createNewDisk(int gigabytes, bool ssd, std::string* err);   // puis Step::Disc
    bool importDisk(const std::string& path, std::string* err);      // puis Step::Finished

    // ── Travaux longs, hors du fil de l'interface ──────────────────────────
    bool busy() const { return job_ && job_->running.load(); }
    std::string busyText() const;
    float progress() const;      // 0 à 1, ou négatif si inconnu
    void cancelJob();
    // Rend une fois le chemin choisi dans la fenêtre de macOS ; vide si annulé ou encore en cours.
    bool takeChosen(std::string* path);

    void chooseFile(const std::string& prompt);    // la fenêtre de fichiers de macOS
    void placeDisc(const std::string& file, bool move);   // puis Step::Installing
    void checkAndPlaceDownload();                  // MD5, puis placeDisc ; sinon md5Mismatch()
    bool md5Mismatch() const { return md5Mismatch_; }
    void acceptMismatch();
    void downloadUpdate();                         // chez Apple, SHA-1 contrôlé ; puis Step::Updating
    void skipUpdate() { go(Step::Driver); }
    static bool downloadReady(const std::string& path);   // complet, navigateur fini

    // Outils, aussi utilisés par les tests.
    static bool runTool(const std::vector<std::string>& args, std::string* output = nullptr,
                        int* pidOut = nullptr);
    bool placeMedia(const std::string& file, const std::string& out, bool move, std::string* err);
    // Un .dmg en image brute : hdiutil sur macOS, qemu-img ailleurs (le paquet le met au PATH).
    static bool convertDmg(const std::string& dmg, const std::string& out, std::string* err);
    // Une page ou un dossier dans l'application de l'hôte, sans attendre qu'elle se ferme.
    static void openExternal(const std::string& target);

    static constexpr const char* kUpdateUrl =
        "https://download.info.apple.com/Mac_OS_X/061-3461.20071114.8Uy45/MacOSXUpdCombo10.4.11PPC.dmg";
    // [B] Apple publie ce fichier et son SHA-1 : https://support.apple.com/kb/dl170
    static constexpr const char* kUpdateSha1 = "3d403bfa769424c61a3cfac173f8527658f9d4af";
    static constexpr uint64_t kUpdateBytes = 189552989;
    std::string updateSha1 = kUpdateSha1;   // remplaçable par un test
    std::string updateUrl = kUpdateUrl;     // idem

private:
    struct Job {
        std::thread thread;
        std::atomic<bool> running{true};
        std::atomic<bool> cancelled{false};
        std::atomic<int> pid{-1};
        std::string text;
        std::string watchFile;         // sa taille donne la progression
        uint64_t watchBytes = 0;
        std::string chosen;
        bool hasChosen = false;
        // Résultat : appliqué par poll(), jamais depuis le fil du travail.
        Step next = Step::Off;
        std::string error;
        bool md5Bad = false;
        bool applied = false;
    };
    void startJob(const std::string& text, const std::string& watchFile, uint64_t watchBytes,
                  std::function<void(Job&)> work);
    void finishJob();
    void save() const;

    std::string root_, runtimeDir_;
    Step step_ = Step::Off;
    bool tiger_ = true;
    bool md5Mismatch_ = false;
    std::string problem_;
    std::unique_ptr<Job> job_;
};
