// POMPPC — Dear ImGui + GLFW/OpenGL3 shell around a QEMU PowerPC Mac.
// The QEMU display arrives over D-Bus (QemuBridge); we upload it as a texture
// and render it inside an ImGui window, exactly like pom68k does with its
// in-process framebuffer. Input is forwarded back over D-Bus; the menu bar
// drives the machine over QMP.
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "QemuBridge.h"

namespace fs = std::filesystem;

// ImGui key → QEMU key-number (PS/2 set-1 scancode; extended = 0x80|low).
struct KeyMap { ImGuiKey k; uint32_t num; };
static const KeyMap kKeys[] = {
    {ImGuiKey_Escape,0x01},{ImGuiKey_1,0x02},{ImGuiKey_2,0x03},{ImGuiKey_3,0x04},
    {ImGuiKey_4,0x05},{ImGuiKey_5,0x06},{ImGuiKey_6,0x07},{ImGuiKey_7,0x08},
    {ImGuiKey_8,0x09},{ImGuiKey_9,0x0A},{ImGuiKey_0,0x0B},{ImGuiKey_Minus,0x0C},
    {ImGuiKey_Equal,0x0D},{ImGuiKey_Backspace,0x0E},{ImGuiKey_Tab,0x0F},
    {ImGuiKey_Q,0x10},{ImGuiKey_W,0x11},{ImGuiKey_E,0x12},{ImGuiKey_R,0x13},
    {ImGuiKey_T,0x14},{ImGuiKey_Y,0x15},{ImGuiKey_U,0x16},{ImGuiKey_I,0x17},
    {ImGuiKey_O,0x18},{ImGuiKey_P,0x19},{ImGuiKey_LeftBracket,0x1A},
    {ImGuiKey_RightBracket,0x1B},{ImGuiKey_Enter,0x1C},{ImGuiKey_LeftCtrl,0x1D},
    {ImGuiKey_A,0x1E},{ImGuiKey_S,0x1F},{ImGuiKey_D,0x20},{ImGuiKey_F,0x21},
    {ImGuiKey_G,0x22},{ImGuiKey_H,0x23},{ImGuiKey_J,0x24},{ImGuiKey_K,0x25},
    {ImGuiKey_L,0x26},{ImGuiKey_Semicolon,0x27},{ImGuiKey_Apostrophe,0x28},
    {ImGuiKey_GraveAccent,0x29},{ImGuiKey_LeftShift,0x2A},{ImGuiKey_Backslash,0x2B},
    {ImGuiKey_Z,0x2C},{ImGuiKey_X,0x2D},{ImGuiKey_C,0x2E},{ImGuiKey_V,0x2F},
    {ImGuiKey_B,0x30},{ImGuiKey_N,0x31},{ImGuiKey_M,0x32},{ImGuiKey_Comma,0x33},
    {ImGuiKey_Period,0x34},{ImGuiKey_Slash,0x35},{ImGuiKey_RightShift,0x36},
    {ImGuiKey_LeftAlt,0x38},{ImGuiKey_Space,0x39},{ImGuiKey_CapsLock,0x3A},
    {ImGuiKey_F1,0x3B},{ImGuiKey_F2,0x3C},{ImGuiKey_F3,0x3D},{ImGuiKey_F4,0x3E},
    {ImGuiKey_F5,0x3F},{ImGuiKey_F6,0x40},{ImGuiKey_F7,0x41},{ImGuiKey_F8,0x42},
    {ImGuiKey_F9,0x43},{ImGuiKey_F10,0x44},{ImGuiKey_F11,0x57},{ImGuiKey_F12,0x58},
    // Extended keys (0x80 | set-1 low byte).
    {ImGuiKey_RightCtrl,0x9D},{ImGuiKey_RightAlt,0xB8},{ImGuiKey_LeftSuper,0xDB},
    {ImGuiKey_RightSuper,0xDC},{ImGuiKey_UpArrow,0xC8},{ImGuiKey_DownArrow,0xD0},
    {ImGuiKey_LeftArrow,0xCB},{ImGuiKey_RightArrow,0xCD},{ImGuiKey_Delete,0xD3},
    {ImGuiKey_Home,0xC7},{ImGuiKey_End,0xCF},{ImGuiKey_PageUp,0xC9},
    {ImGuiKey_PageDown,0xD1},{ImGuiKey_Insert,0xD2},{ImGuiKey_KeypadEnter,0x9C},
};

static void glfwErr(int e, const char* d) { std::fprintf(stderr, "GLFW %d: %s\n", e, d); }

// A tidy display name for a CD image ("DOOM_II.cdr" → "DOOM II").
static std::string prettyName(const std::string& path) {
    std::string n = fs::path(path).stem().string();
    std::replace(n.begin(), n.end(), '_', ' ');
    return n;
}

// Available CD images for the ludothèque (disks/cdr/ + disks/).
static std::vector<std::string> listCdImages(const std::string& root) {
    std::vector<std::string> out;
    const char* exts[] = {".iso", ".cdr", ".img", ".toast"};
    for (const std::string& dir : {root + "/disks/cdr", root + "/disks"}) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            if (e.path().filename() == "games.iso") continue;  // auto-mounted master
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            for (const char* x : exts)
                if (ext == x) { out.push_back(e.path().string()); break; }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Build a config for a given launcher (shared env). `sound`/`pad` enable the
// engine layer's existing PulseAudio Screamer + usb-host gamepad passthrough.
static QemuBridge::Config makeConfig(const std::string& launcher,
                                     const std::string& runtimeDir, bool sound,
                                     bool pad) {
    QemuBridge::Config cfg;
    cfg.launcher = launcher;
    cfg.runtimeDir = runtimeDir;
    if (!sound) cfg.env.push_back("NOSOUND=1");
    if (!pad) cfg.env.push_back("NOPAD=1");
    if (const char* res = std::getenv("RES")) cfg.env.push_back(std::string("RES=") + res);
    if (const char* smp = std::getenv("SMP")) cfg.env.push_back(std::string("SMP=") + smp);
    return cfg;
}

int main(int argc, char** argv) {
    fs::path exeDir = fs::weakly_canonical(fs::path(argv[0])).parent_path();
    // Default boot = Mac OS 9. The "OS" menu switches to Mac OS X and back.
    std::string defLauncher =
        (argc > 1) ? argv[1] : (exeDir / ".." / ".." / "run_os9.sh").string();
    defLauncher = fs::weakly_canonical(defLauncher).string();
    if (!fs::exists(defLauncher)) {
        std::fprintf(stderr, "launcher not found: %s\n", defLauncher.c_str());
        return 1;
    }
    std::string root = fs::path(defLauncher).parent_path().string();
    std::string runtimeDir = root + "/.run";
    fs::create_directories(runtimeDir);
    std::string tigerSh = root + "/run_tiger.sh";
    std::string os9Sh = root + "/run_os9.sh";

    std::string curLauncher = defLauncher;   // boots Mac OS 9 by default
    bool sound = true;    // on by default (PulseAudio Screamer); RAM capped ≤768
    bool pad = true;      // usb-host passthrough; the run script self-guards
    auto bridge = std::make_unique<QemuBridge>();
    std::string err;
    if (!bridge->start(makeConfig(curLauncher, runtimeDir, sound, pad), &err)) {
        std::fprintf(stderr, "QemuBridge start failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "QemuBridge: launched %s\n", curLauncher.c_str());

    glfwSetErrorCallback(glfwErr);
    if (!glfwInit()) { std::fprintf(stderr, "GLFW init failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window =
        glfwCreateWindow(1280, 900, "POMPPC — PowerPC Macintosh (QEMU)", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    std::vector<uint32_t> fb;
    int guestW = 0, guestH = 0;
    bool grabbed = true;    // keyboard → guest (toggle in the Machine menu)
    bool paused = false;
    float zoom = 1.0f;
    bool showLibrary = true;      // ludothèque window
    std::string currentCd;        // game CD currently inserted in gamecd

    auto relaunch = [&](const std::string& launcher) {
        bridge->stop();
        bridge = std::make_unique<QemuBridge>();
        std::string e;
        if (!bridge->start(makeConfig(launcher, runtimeDir, sound, pad), &e))
            std::fprintf(stderr, "relaunch failed: %s\n", e.c_str());
        else
            curLauncher = launcher;
        guestW = guestH = 0;
        paused = false;
        currentCd.clear();
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();

        int w = 0, h = 0;
        if (bridge->latchFrame(fb, w, h)) {
            guestW = w; guestH = h;
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            // QEMU's mac99 scanout is PIXMAN_x8r8g8b8: the high byte is
            // padding, not alpha, and is commonly zero. An RGBA texture made
            // ImGui blend the whole guest display as transparent. Store RGB
            // so OpenGL supplies an opaque alpha value when sampling.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_BGRA,
                         GL_UNSIGNED_BYTE, fb.data());
        }

        // ── Menu bar → machine control over QMP ──────────────────────────
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Machine")) {
                if (ImGui::MenuItem("Redémarrer")) bridge->reset();
                if (ImGui::MenuItem(paused ? "Reprendre" : "Pause")) {
                    paused = !paused;
                    bridge->setPaused(paused);
                }
                ImGui::Separator();
                ImGui::MenuItem("Clavier → invité", nullptr, &grabbed);
                ImGui::Separator();
                if (ImGui::MenuItem("Quitter")) glfwSetWindowShouldClose(window, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Jeux")) {
                ImGui::MenuItem("Ludothèque", nullptr, &showLibrary);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("OS")) {
                bool onTiger = (curLauncher == tigerSh);
                if (ImGui::MenuItem("Mac OS X 10.4 (Tiger)", nullptr, onTiger, !onTiger))
                    relaunch(tigerSh);
                bool onOs9 = (curLauncher == os9Sh);
                if (ImGui::MenuItem("Mac OS 9.2.2", nullptr, onOs9, !onOs9))
                    relaunch(os9Sh);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Périphériques")) {
                ImGui::TextDisabled("(prend effet au redémarrage)");
                if (ImGui::MenuItem("Son (PulseAudio)", nullptr, &sound))
                    relaunch(curLauncher);
                if (ImGui::MenuItem("Manette USB", nullptr, &pad))
                    relaunch(curLauncher);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Vue")) {
                if (ImGui::MenuItem("50 %"))  zoom = 0.5f;
                if (ImGui::MenuItem("100 %")) zoom = 1.0f;
                if (ImGui::MenuItem("150 %")) zoom = 1.5f;
                if (ImGui::MenuItem("200 %")) zoom = 2.0f;
                ImGui::EndMenu();
            }
            ImGui::TextDisabled("  %dx%d  %s  %s", guestW, guestH,
                                bridge->running() ? "en marche" : "arrêté",
                                paused ? "(pause)" : "");
            ImGui::EndMainMenuBar();
        }

        // ── Screen window ────────────────────────────────────────────────
        ImGui::SetNextWindowPos(ImVec2(20, 40), ImGuiCond_FirstUseEver);
        ImGui::Begin("Écran", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        if (guestW > 0) {
            ImVec2 drawSz(guestW * zoom, guestH * zoom);
            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(intptr_t)tex, drawSz);
            bool hovered = ImGui::IsItemHovered();

            if (hovered) {
                // Map cursor from the (possibly scaled) image to guest pixels.
                float lx = (io.MousePos.x - origin.x) / zoom;
                float ly = (io.MousePos.y - origin.y) / zoom;
                if (bridge->mouseIsAbsolute())
                    bridge->mouseAbs((int)lx, (int)ly);
                else if (io.MouseDelta.x || io.MouseDelta.y)
                    bridge->mouseRel((int)(io.MouseDelta.x / zoom),
                                     (int)(io.MouseDelta.y / zoom));
                if (io.MouseWheel > 0) bridge->mouseWheel(3);
                if (io.MouseWheel < 0) bridge->mouseWheel(4);
            }
            for (int b = 0; b < 3; ++b) {
                int qb = (b == 1) ? 2 : (b == 2 ? 1 : 0);  // ImGui M/R ↔ qemu R/M
                if (ImGui::IsMouseClicked(b) && hovered) bridge->mouseButton(qb, true);
                if (ImGui::IsMouseReleased(b)) bridge->mouseButton(qb, false);
            }
        } else {
            ImGui::TextUnformatted("En attente de l'affichage QEMU…");
        }
        ImGui::End();

        // ── Ludothèque : insert a game CD into the guest at runtime ───────
        if (showLibrary) {
            ImGui::SetNextWindowSize(ImVec2(340, 440), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(980, 60), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Ludothèque", &showLibrary)) {
                ImGui::TextDisabled("Insère un CD de jeu à chaud (lecteur gamecd)");
                if (currentCd.empty())
                    ImGui::TextDisabled("CD inséré : aucun");
                else
                    ImGui::Text("CD inséré : %s", prettyName(currentCd).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Éjecter")) {
                    bridge->ejectCd();
                    currentCd.clear();
                }
                ImGui::Separator();
                ImGui::BeginChild("games");
                for (const std::string& g : listCdImages(root)) {
                    bool sel = (g == currentCd);
                    if (ImGui::Selectable(prettyName(g).c_str(), sel,
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (bridge->changeCd(g)) currentCd = g;
                    }
                }
                ImGui::EndChild();
            }
            ImGui::End();
        }

        // Clipboard bridge: offer host clipboard to the guest (throttled poll),
        // and paste any text the guest copied onto the host clipboard.
        static int clipTick = 0;
        if (++clipTick >= 30) {
            clipTick = 0;
            if (const char* hc = glfwGetClipboardString(window))
                if (hc[0]) bridge->publishLocalClipboard(hc);
        }
        std::string gclip;
        if (bridge->takeGuestClipboard(gclip))
            glfwSetClipboardString(window, gclip.c_str());

        // Keyboard → guest (skip while an ImGui text field wants input).
        if (grabbed && !io.WantTextInput) {
            for (const auto& e : kKeys) {
                if (ImGui::IsKeyPressed(e.k, false)) bridge->keyPress(e.num);
                if (ImGui::IsKeyReleased(e.k)) bridge->keyRelease(e.num);
            }
        }

        ImGui::Render();
        int fbw, fbh;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    bridge->stop();
    glDeleteTextures(1, &tex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
