// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
#include "SetupUi.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "SetupFlow.h"
#include "imgui.h"

namespace fs = std::filesystem;

namespace {

const float kPageWidth = 600.0f;

// Où mène la fenêtre de fichiers ouverte par cette page.
enum class Chooser { None, Disk, Disc };

void header(const SetupFlow& setup) {
    static const char* tigerSteps[] = {"Disk", "Disc", "Install", "Update", "3D Driver"};
    static const char* os9Steps[] = {"Disk", "Disc", "Install"};
    const char** names = setup.tiger() ? tigerSteps : os9Steps;
    for (int i = 0; i < setup.stepCount(); ++i) {
        if (i > 0) { ImGui::SameLine(); ImGui::TextDisabled(" > "); ImGui::SameLine(); }
        if (i + 1 == setup.stepNumber()) ImGui::TextUnformatted(names[i]);
        else ImGui::TextDisabled("%s", names[i]);
    }
    ImGui::Separator();
    ImGui::Spacing();
}

void systemPage(SetupFlow& setup, const SetupActions& a) {
    ImGui::TextUnformatted("Welcome to POMPPC");
    ImGui::Spacing();
    ImGui::TextWrapped("POMPPC runs Mac OS on an emulated Power Mac G4. Which system do you want to set up?");
    ImGui::Spacing();
    if (ImGui::Button("Mac OS X 10.4 Tiger")) { setup.setTiger(true); setup.go(SetupFlow::Step::Disk); }
    ImGui::SameLine();
    ImGui::TextDisabled("3D games, Mac OS X applications");
    if (ImGui::Button("Mac OS 9.2.2")) { setup.setTiger(false); setup.go(SetupFlow::Step::Disk); }
    ImGui::SameLine();
    ImGui::TextDisabled("Classic Mac applications and games");
    ImGui::Spacing();
    if (a.cancel) { if (ImGui::Button("Cancel")) a.cancel(); }
    else if (ImGui::Button("Quit")) a.quit();
}

void diskPage(SetupFlow& setup, Chooser& chooser) {
    static int choice = 0, custom = 16, type = 0;
    static std::string error;
    ImGui::Text("%s needs a hard disk.", setup.osName().c_str());
    ImGui::Spacing();
    const auto sizes = setup.sizeChoices();
    ImGui::TextUnformatted("Size of a new disk:");
    for (int i = 0; i < (int)sizes.size(); ++i) {
        std::string label = sizes[i] + " GB" + (i == 0 ? " (recommended)" : "");
        if (ImGui::RadioButton(label.c_str(), choice == i)) choice = i;
    }
    if (ImGui::RadioButton("Other:", choice == (int)sizes.size())) choice = (int)sizes.size();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("GB", &custom);
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("Type", &type, "HD\0SSD\0");
    ImGui::TextDisabled("It only takes the space it really uses on your Mac.");
    ImGui::Spacing();
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    if (ImGui::Button("Create a New Disk")) {
        const int gb = choice < (int)sizes.size() ? std::atoi(sizes[choice].c_str()) : custom;
        if (setup.createNewDisk(gb, type == 1, &error)) error.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Use an Existing Disk")) {
        chooser = Chooser::Disk;
        setup.chooseFile("Choose your " + setup.osName() + " disk (.qcow2).");
    }
    ImGui::SameLine();
    if (ImGui::Button("Back")) setup.go(SetupFlow::Step::System);
}

void discPage(SetupFlow& setup, Chooser& chooser) {
    static bool waiting = false;
    ImGui::Text("POMPPC needs the %s installation disc.", setup.osName().c_str());
    ImGui::Spacing();
    if (setup.md5Mismatch()) {
        waiting = false;
        ImGui::TextWrapped("This file doesn't match the checksum published by Macintosh Garden. It may be "
                           "incomplete or a different version, so the installation may fail.");
        ImGui::Spacing();
        if (ImGui::Button("Use It Anyway")) setup.acceptMismatch();
        ImGui::SameLine();
        if (ImGui::Button("Choose Another File")) {
            setup.go(SetupFlow::Step::Disc);
            chooser = Chooser::Disc;
            setup.chooseFile("Choose your " + setup.osName() + " installation disc.");
        }
        return;
    }
    if (waiting) {
        ImGui::TextWrapped("Macintosh Garden has opened in your browser. In its list of files, find %s and "
                           "click one of its links.", setup.discFile().c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("POMPPC is waiting for the file in your Downloads folder and continues by itself. "
                           "If macOS asks, allow POMPPC to access Downloads.");
        ImGui::Spacing();
        if (ImGui::Button("Choose a File Instead")) {
            waiting = false;
            chooser = Chooser::Disc;
            setup.chooseFile("Choose the file you downloaded.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Open the Page Again")) SetupFlow::runTool({"open", setup.discPage()});
        ImGui::SameLine();
        if (ImGui::Button("Back")) waiting = false;
        static int tick = 0;
        if (++tick % 60 == 0 && SetupFlow::downloadReady(setup.downloadedDisc())) {
            waiting = false;
            setup.checkAndPlaceDownload();
        }
        return;
    }
    ImGui::TextWrapped("Macintosh Garden, an independent archive not linked to Apple, keeps a copy: %s. "
                       "Mac OS remains Apple's copyrighted software, so only download it if you're "
                       "entitled to use it.", setup.discFile().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Open Macintosh Garden")) {
        if (SetupFlow::downloadReady(setup.downloadedDisc())) {
            setup.checkAndPlaceDownload();
        } else {
            SetupFlow::runTool({"open", setup.discPage()});
            waiting = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("I Have the Disc")) {
        chooser = Chooser::Disc;
        setup.chooseFile("Choose your " + setup.osName() + " installation disc (.iso, .toast, .cdr, .dmg or .zip).");
    }
}

void installShortPage(SetupFlow& setup) {
    ImGui::TextWrapped("The installer stopped, but %s doesn't seem to be installed on the disk.",
                       setup.osName().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Start the Installer Again")) setup.go(SetupFlow::Step::Installing);
    ImGui::SameLine();
    if (ImGui::Button("It's Installed, Continue")) {
        setup.go(setup.tiger() ? SetupFlow::Step::UpdateOffer : SetupFlow::Step::Finished);
    }
}

void updatePage(SetupFlow& setup) {
    ImGui::TextWrapped("Tiger needs its last update, 10.4.11, for POMPPC's 3D games and mouse to work as "
                       "tested. Apple offers it for free (181 MB).");
    ImGui::Spacing();
    if (ImGui::Button("Download from Apple")) setup.downloadUpdate();
    ImGui::SameLine();
    if (ImGui::Button("Skip")) setup.skipUpdate();
    ImGui::TextDisabled("Skip it only if this disc already installs 10.4.11.");
}

}  // namespace

void drawSetupPage(SetupFlow& setup, const SetupActions& actions) {
    static Chooser chooser = Chooser::None;
    static std::string chooserError;

    std::string chosen;
    if (setup.takeChosen(&chosen)) {
        const Chooser was = chooser;
        chooser = Chooser::None;
        chooserError.clear();
        if (!chosen.empty()) {
            if (was == Chooser::Disk) setup.importDisk(chosen, &chooserError);
            else if (was == Chooser::Disc) setup.placeDisc(chosen, false);
        }
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos(ImVec2(std::max(24.0f, (avail.x - kPageWidth) * 0.5f), std::max(24.0f, avail.y * 0.14f)));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kPageWidth);
    if (setup.step() != SetupFlow::Step::System) header(setup);
    if (setup.busy()) {
        ImGui::TextUnformatted(setup.busyText().c_str());
        const float p = setup.progress();
        if (p >= 0.0f) {
            ImGui::ProgressBar(p, ImVec2(kPageWidth, 0));
            if (ImGui::Button("Cancel")) setup.cancelJob();
        }
    } else {
        if (!setup.problem().empty()) {
            ImGui::TextWrapped("%s", setup.problem().c_str());
            ImGui::Spacing();
        }
        if (!chooserError.empty()) {
            ImGui::TextWrapped("%s", chooserError.c_str());
            ImGui::Spacing();
        }
        switch (setup.step()) {
            case SetupFlow::Step::System: systemPage(setup, actions); break;
            case SetupFlow::Step::Disk: diskPage(setup, chooser); break;
            case SetupFlow::Step::Disc: discPage(setup, chooser); break;
            case SetupFlow::Step::InstallShort: installShortPage(setup); break;
            case SetupFlow::Step::UpdateOffer: updatePage(setup); break;
            default: break;
        }
        if (setup.step() != SetupFlow::Step::System && !setup.busy()) {
            ImGui::Spacing();
            if (ImGui::Button("Quit Setup")) actions.quit();
            ImGui::SameLine();
            ImGui::TextDisabled("Setup continues here next time.");
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
}

float setupBarHeight(const SetupFlow& setup) {
    if (!setup.wantsVm()) return 0.0f;
    const int lines = setup.step() == SetupFlow::Step::Finished ? 1 : 3;
    return lines * ImGui::GetFrameHeightWithSpacing() + (lines == 1 ? 6.0f : 0.0f);
}

void drawSetupBar(SetupFlow& setup) {
    const float h = setupBarHeight(setup);
    if (h <= 0.0f) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(32, 58, 100, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
    ImGui::BeginChild("setupbar", ImVec2(0, h), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const std::string step = "Step " + std::to_string(setup.stepNumber()) + " of " +
                             std::to_string(setup.stepCount()) + ": ";
    switch (setup.step()) {
        case SetupFlow::Step::Installing:
            if (setup.tiger()) {
                ImGui::TextWrapped("%sInstall Tiger. In the installer, open Disk Utility from the Utilities menu, "
                                   "erase the disk as Mac OS Extended (Journaled), then install on it. The next "
                                   "step starts by itself when the installer restarts.", step.c_str());
            } else {
                ImGui::TextWrapped("%sInstall Mac OS 9. Open Drive Setup to initialize the disk, then run Mac OS "
                                   "Install. Setup continues by itself when you restart.", step.c_str());
            }
            break;
        case SetupFlow::Step::Updating:
            ImGui::TextWrapped("%sUpdate to 10.4.11. If Tiger asks, create your account first. Then open the CD on "
                               "the desktop and double-click MacOSXUpdCombo10.4.11PPC.pkg. The next step starts "
                               "by itself when Tiger restarts.", step.c_str());
            break;
        case SetupFlow::Step::Driver:
            ImGui::TextWrapped("%sInstall the 3D driver. Open the POMPPC_GUEST CD on the desktop, double-click "
                               "Install POMPPC, enter your password and restart. Setup ends by itself once the "
                               "driver is running.", step.c_str());
            break;
        case SetupFlow::Step::Finished: {
            ImGui::Text("%s is ready. Enjoy!", setup.osName().c_str());
            std::error_code ec;
            if (fs::is_regular_file(setup.discPath(), ec) && !fs::is_symlink(setup.discPath(), ec)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete the Installation Disc")) fs::remove(setup.discPath(), ec);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("OK")) setup.end();
            break;
        }
        default: break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
