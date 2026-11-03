/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
// POMPPC — Dear ImGui + GLFW/OpenGL3 shell around a QEMU PowerPC Mac.
// The QEMU display arrives over D-Bus (QemuBridge); we upload it as a texture
// and render it inside an ImGui window, exactly like pom68k does with its
// in-process framebuffer. Input is forwarded back over D-Bus; the menu bar
// drives the machine over QMP.
//
// F1 : the guest screen is an ImGui window docked in the middle of a
// full-frame DockSpace (menus, Ludothèque, Bilan, Journal around it; layout
// built once with DockBuilder, then saved in .run/imgui2.ini). The guest image
// is always drawn at the largest rectangle keeping the scanout's aspect ratio
// (black bands, never stretched), unless a fixed zoom is picked in Vue.
// Vue ▸ Plein écran (Alt+Enter, Ctrl+Cmd+F or F11 to enter/leave; Escape goes to the guest)
// puts the host window on its monitor at native size with only the guest
// shown. Pointer input is mapped on the rectangle actually drawn.
#ifdef _WIN32
#define GLFW_INCLUDE_GLEXT   // gl.h de Windows s'arrête à GL 1.1 : GL_BGRA, GL_CLAMP_TO_EDGE…
#endif
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#ifdef _WIN32
#include <unistd.h>   // usleep (mingw-w64)
#else
#include <spawn.h>
#endif
#include <fstream>
#include <ctime>
#include <functional>
#include <vector>

#ifndef _WIN32
extern char** environ;
#endif

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder*
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "QemuBridge.h"
#include "StartupChime.h"
#include "FrontendSettings.h"
#include "PointerPolicy.h"
#include "SetupFlow.h"
#include "SetupUi.h"
#include "AppExtras.h"
#include "SharedFolder.h"
#include "WinHost.h"
#include "roboto_font.h"

#ifndef IMGUI_HAS_DOCK
#error "Dear ImGui docking branch required — run ./setup.sh (it fetches the docking tag)"
#endif

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

static bool gClipboardPoll = false;
static void glfwErr(int e, const char* d) {
    // glfwGetClipboardString logs this when the pasteboard has no string.
    if (gClipboardPoll && e == 0x00010009) return;
    std::fprintf(stderr, "GLFW %d: %s\n", e, d);
}

// ── Journal : what the frontend did (launches, CDs, bascules, clics) ─────
static std::deque<std::string> gJournal;
static bool gJournalScroll = false;
static void journal(const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::time_t t = std::time(nullptr);
    char ts[16];
    std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
    gJournal.push_back(std::string(ts) + "  " + msg);
    if (gJournal.size() > 500) gJournal.pop_front();
    gJournalScroll = true;
    std::fprintf(stderr, "[pomppc] %s\n", msg);
}

// Largest rectangle of aspect gw:gh fitting in `avail`, snapped to whole
// points. The limiting side takes the whole extent and the other one is
// derived from the guest ratio (one rounding only), so a 4:3 guest stays 4:3
// to the pixel instead of drifting by two independent roundings.
static ImVec2 fitRect(ImVec2 avail, int gw, int gh) {
    if (gw <= 0 || gh <= 0 || avail.x < 1 || avail.y < 1) return ImVec2(0, 0);
    float ax = std::floor(avail.x), ay = std::floor(avail.y);
    if (ax * gh <= ay * gw)                        // width-limited
        return ImVec2(ax, std::round(ax * gh / gw));
    return ImVec2(std::round(ay * gw / gh), ay);   // height-limited
}

// Monitor under the window's centre (GLFW has no "current monitor" call).
static GLFWmonitor* monitorOf(GLFWwindow* win) {
    int wx, wy, ww, wh;
    glfwGetWindowPos(win, &wx, &wy);
    glfwGetWindowSize(win, &ww, &wh);
    int cx = wx + ww / 2, cy = wy + wh / 2;
    int n = 0;
    GLFWmonitor** mons = glfwGetMonitors(&n);
    for (int i = 0; i < n; ++i) {
        int mx, my;
        glfwGetMonitorPos(mons[i], &mx, &my);
        const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
        if (vm && cx >= mx && cx < mx + vm->width && cy >= my && cy < my + vm->height)
            return mons[i];
    }
    return glfwGetPrimaryMonitor();
}

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
// engine layer's existing Screamer sound (PulseAudio on Linux, CoreAudio on
// macOS) + usb-host gamepad passthrough.
// Fichiers déposés sur la fenêtre : relevés ici, traités dans la boucle principale.
static std::vector<std::string> gDroppedFiles;
static void onFilesDropped(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; ++i) gDroppedFiles.push_back(paths[i]);
}

// Fonctions GL 3 d'agrandissement net : GLFW n'expose que l'OpenGL d'origine dans ses en-têtes.
typedef void (*GlGenFramebuffers)(GLsizei, GLuint*);
typedef void (*GlBindFramebuffer)(GLenum, GLuint);
typedef void (*GlFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (*GlBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static QemuBridge::Config makeConfig(const std::string& launcher,
                                     const std::string& runtimeDir, bool sound,
                                     bool pad, const std::vector<std::string>& diskVars) {
    QemuBridge::Config cfg;
    cfg.launcher = launcher;
    cfg.runtimeDir = runtimeDir;
#ifdef _WIN32
    cfg.packageRoot = winhost::exeDir();
#endif
    if (!sound) cfg.env.push_back("NOSOUND=1");
    if (!pad) cfg.env.push_back("NOPAD=1");
    if (const char* res = std::getenv("RES")) cfg.env.push_back(std::string("RES=") + res);
    if (const char* smp = std::getenv("SMP")) cfg.env.push_back(std::string("SMP=") + smp);
    cfg.env.insert(cfg.env.end(), diskVars.begin(), diskVars.end());
    return cfg;
}

// ── Épreuve scriptée (POMPPC_FE_SCRIPT) ──────────────────────────────────
// Joue une suite d'entrées *dans la file d'ImGui* (même chemin que la vraie
// souris et le vrai clavier : raccourcis, rectangle dessiné, pont D-Bus) et
// prend des captures au bon moment, sans les droits d'Accessibilité qu'un
// clic synthétique macOS exige. Étapes séparées par ';' :
//   wait <s> | move <x> <y> | click <x> <y> (points fenêtre)
//   | guestmove <x> <y> (fractions 0-1 de la vue dessinée, sans clic)
//   | fs (Ctrl+Cmd+F) | altenter | f11 | esc | type <texte: a-z 0-9 espace .>
//   | enter | shot <fichier.png> (screencapture -x, hôte macOS) | quit (sans rien demander)
//   | glshot <fichier.ppm> (l'image de la fenêtre seule, sans droit de capture d'écran)
//   | close (comme le bouton de fermeture : pose la question)
//   | drop <chemin> (comme un fichier déposé sur la fenêtre)
struct ProbeStep { std::string op; std::string arg; float x = 0, y = 0; };
static std::vector<ProbeStep> parseProbe(const char* s) {
    std::vector<ProbeStep> out;
    std::string all(s);
    size_t p = 0;
    while (p <= all.size()) {
        size_t e = all.find(';', p);
        if (e == std::string::npos) e = all.size();
        std::string t = all.substr(p, e - p);
        p = e + 1;
        size_t b = t.find_first_not_of(' ');
        if (b == std::string::npos) continue;
        t = t.substr(b);
        ProbeStep st;
        size_t sp = t.find(' ');
        st.op = t.substr(0, sp);
        st.arg = (sp == std::string::npos) ? "" : t.substr(sp + 1);
        std::sscanf(st.arg.c_str(), "%f %f", &st.x, &st.y);
        out.push_back(st);
    }
    return out;
}
static ImGuiKey probeKey(char c) {
    if (c >= 'a' && c <= 'z') return (ImGuiKey)(ImGuiKey_A + (c - 'a'));
    if (c >= '0' && c <= '9') return (ImGuiKey)(ImGuiKey_0 + (c - '0'));
    if (c == ' ') return ImGuiKey_Space;
    if (c == '.') return ImGuiKey_Period;
    return ImGuiKey_None;
}

#ifdef __APPLE__
#include <pthread/qos.h>
// The AppKit main thread is USER_INTERACTIVE, above QEMU. Its frame copy and
// GL upload then preempt the audio timer, which lives on QEMU's main loop.
// Utility still runs the UI when the machine is idle; under load the guest
// audio timer wins.
static void preferGuestAudio() {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
}
#else
static void preferGuestAudio() {}
#endif

#ifdef __APPLE__
// GLFW's Cocoa menu bar carries "Window ▸ Enter Full Screen" bound to
// Ctrl+Cmd+F (toggleFullScreen:, macOS *Spaces* full screen). The menu eats
// that key equivalent before GLFW sees it, so our Vue ▸ Plein écran shortcut
// would never fire and the dock UI would be stretched into a native full
// screen space. Strip the key equivalent and forbid native full screen on
// our window (the green button then zooms): Plein écran is ours alone.
// Plain Objective-C runtime calls — no Cocoa sources in the build.
#include <objc/message.h>
#include <objc/runtime.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
static int stripFullScreenKey(id menu) {
    using MsgId = id (*)(id, SEL);
    using MsgLong = long (*)(id, SEL);
    using MsgIdL = id (*)(id, SEL, long);
    using MsgSel = SEL (*)(id, SEL);
    using MsgSetId = void (*)(id, SEL, id);
    if (!menu) return 0;
    int n = 0;
    long count = ((MsgLong)objc_msgSend)(menu, sel_registerName("numberOfItems"));
    id empty = ((id (*)(id, SEL, const char*))objc_msgSend)(
        (id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "");
    for (long i = 0; i < count; ++i) {
        id item = ((MsgIdL)objc_msgSend)(menu, sel_registerName("itemAtIndex:"), i);
        if (((MsgSel)objc_msgSend)(item, sel_registerName("action")) ==
            sel_registerName("toggleFullScreen:")) {
            ((MsgSetId)objc_msgSend)(item, sel_registerName("setKeyEquivalent:"), empty);
            ++n;
        }
        n += stripFullScreenKey(((MsgId)objc_msgSend)(item, sel_registerName("submenu")));
    }
    return n;
}
static int disableNativeFullScreen(GLFWwindow* win) {
    id app = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"),
                                             sel_registerName("sharedApplication"));
    int n = stripFullScreenKey(((id (*)(id, SEL))objc_msgSend)(app, sel_registerName("mainMenu")));
    id nswin = (id)glfwGetCocoaWindow(win);
    if (nswin) {
        const unsigned long kPrimary = 1UL << 7, kNone = 1UL << 9;  // NSWindowCollectionBehavior
        unsigned long b = ((unsigned long (*)(id, SEL))objc_msgSend)(
            nswin, sel_registerName("collectionBehavior"));
        b = (b & ~kPrimary) | kNone;
        ((void (*)(id, SEL, unsigned long))objc_msgSend)(
            nswin, sel_registerName("setCollectionBehavior:"), b);
    }
    return n;
}

// glfwGetClipboardString prints GLFW_FORMAT_UNAVAILABLE on an empty pasteboard.
// changeCount is silent; the string is read only when the pasteboard changes.
static std::string macClipboardText() {
    using MsgId = id (*)(id, SEL);
    static long seen = -1;
    id pb = ((MsgId)objc_msgSend)((id)objc_getClass("NSPasteboard"),
                                  sel_registerName("generalPasteboard"));
    if (!pb) return {};
    long change = ((long (*)(id, SEL))objc_msgSend)(pb, sel_registerName("changeCount"));
    if (change == seen) return {};
    seen = change;
    id type = ((id (*)(id, SEL, const char*))objc_msgSend)(
        (id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"),
        "public.utf8-plain-text");
    id str = ((id (*)(id, SEL, id))objc_msgSend)(pb, sel_registerName("stringForType:"), type);
    if (!str) return {};
    const char* utf8 = ((const char* (*)(id, SEL))objc_msgSend)(str, sel_registerName("UTF8String"));
    if (!utf8 || !utf8[0]) return {};
    return utf8;
}
#endif

// Window names shared by the DockBuilder layout and the Begin() calls.
static const char* kWinScreen  = "Screen";
static const char* kWinLibrary = "Library";
static const char* kWinBilan   = "Status";
static const char* kWinJournal = "Log";
static const char* kWinDisks   = "Disks";

// Ctrl+C ou SIGTERM : quitter par la fin de main (réglages écrits, QEMU
// arrêté proprement) au lieu de mourir sur place. Le gestionnaire ne fait que
// lever le drapeau ; la boucle le lit à chaque image.
static volatile std::sig_atomic_t gQuitSignal = 0;
static void onQuitSignal(int) { gQuitSignal = 1; }

// Default layout, built only when imgui2.ini has no node for the dockspace
// (first launch, or after Vue ▸ Disposition par défaut): guest in the
// central node, Ludothèque on the right, Bilan + Journal below.
static void buildDefaultLayout(ImGuiID dsId, const ImGuiViewport* vp) {
    ImGui::DockBuilderRemoveNode(dsId);
    ImGui::DockBuilderAddNode(dsId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dsId, vp->WorkSize);
    ImGuiID center = dsId;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.20f, nullptr, &center);
    ImGuiID bottomRight = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.45f, nullptr, &bottom);
    ImGui::DockBuilderDockWindow(kWinScreen, center);
    ImGui::DockBuilderDockWindow(kWinLibrary, right);
    ImGui::DockBuilderDockWindow(kWinDisks, right);
    ImGui::DockBuilderDockWindow(kWinJournal, bottom);
    ImGui::DockBuilderDockWindow(kWinBilan, bottomRight);
    ImGui::DockBuilderFinish(dsId);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // Le paquet Windows n'a pas de bash : run_tiger.sh et run_os9.sh n'y sont que des noms, qui disent
    // le système à lancer (QemuBridge passe alors par LaunchPlan). Les données vivent dans
    // %USERPROFILE%\POMPPC et le paquet à côté de POMPPC.exe, comme ~/POMPPC et l'AppImage.
    if (argc > 1 && std::string(argv[1]) == "--version") {
        std::printf("%s\n", POMPPC_VERSION_TEXT);
        return 0;
    }
    std::string root = winhost::dataRoot();
    winhost::startup(root + "/.run/app.log");
    {
        std::error_code rootEc;
        for (const char* sub : {"/.run", "/disks", "/images"}) fs::create_directories(fs::u8path(root + sub), rootEc);
    }
    _putenv_s("POMPPC_APP", "1");
    _putenv_s("POMPPC_VERSION", POMPPC_VERSION_TEXT);
    std::string defLauncher = root + "/run_os9.sh";
    if (argc > 1) defLauncher = std::string(argv[1]).find("tiger") != std::string::npos ? root + "/run_tiger.sh" : root + "/run_os9.sh";
    const auto launcherThere = [](const std::string&) { return true; };
#else
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
    const auto launcherThere = [](const std::string& path) { return fs::exists(path); };
#endif
    std::string runtimeDir = root + "/.run";
    fs::create_directories(runtimeDir);
    std::string tigerSh = root + "/run_tiger.sh";
    std::string os9Sh = root + "/run_os9.sh";
    // Docking layout persists next to the QMP socket, whatever the cwd.
    // imgui2.ini depuis que les fenêtres ont des noms anglais (02/11) : un ancien imgui.ini ne les
    // reconnaît pas, et l'écran de l'invité en sortait flottant et minuscule. La disposition
    // repart donc une fois par défaut.
    static std::string iniPath = runtimeDir + "/imgui2.ini";
    const std::string chimeSettingsPath = runtimeDir + "/chime.conf";
    // Réglages d'ImGuiDock : relus ici, écrits à l'arrêt (fin de main).
    const std::string feSettingsPath = runtimeDir + "/imguidock.conf";
    // Une épreuve scriptée (POMPPC_FE_SCRIPT) part des défauts et n'écrit
    // rien : elle doit être reproductible et ne pas écraser ceux de l'usager.
    const bool feKeep = std::getenv("POMPPC_FE_SCRIPT") == nullptr;
    FrontendSettings fe;
    const bool feLoaded = feKeep && fe.load(feSettingsPath);
    const std::string defaultChime = root + "/disks/chimes/powermac3-1-4.2.8.wav";
    ChimeSettings chime;
    chime.file = defaultChime;
    chime.load(chimeSettingsPath);
    if (const char* p = std::getenv("POMPPC_CHIME_FILE")) chime.file = p;
    if (const char* p = std::getenv("POMPPC_CHIME")) chime.enabled = std::string(p) != "0";
    if (const char* p = std::getenv("POMPPC_CHIME_VOLUME")) {
        char* end = nullptr;
        float level = std::strtof(p, &end);
        if (end != p && *end == '\0' && std::isfinite(level))
            chime.volume = std::clamp(level, 0.0f, 1.0f);
    }
    char chimeFile[4096];
    std::snprintf(chimeFile, sizeof(chimeFile), "%s", chime.file.c_str());
    ChimePlayer chimePlayer;
    ChimeTrigger chimeTrigger;
    auto saveChime = [&]() {
        if (!chime.save(chimeSettingsPath)) journal("could not save the chime settings");
    };

    // Un lanceur passé en argument (run_tiger.sh délègue ainsi) prime sur
    // l'OS retenu à l'arrêt précédent.
    std::string curLauncher = defLauncher;   // boots Mac OS 9 by default
    if (argc <= 1 && fe.os == "tiger" && launcherThere(tigerSh)) curLauncher = tigerSh;
    bool sound = fe.sound;  // on by default (Screamer; pa or coreaudio); RAM capped ≤768
    bool pad = fe.pad;      // usb-host passthrough; the run script self-guards
    auto bridge = std::make_unique<QemuBridge>();
    std::string err;
    bool trySession = false;      // Try Without Saving : ce que fait l'invité est jeté à l'arrêt
    // L'assistant : repris s'il était en cours, lancé par l'app s'il n'y a encore aucun disque.
    const bool appMode = std::getenv("POMPPC_APP") != nullptr;
    SetupFlow setup(root, runtimeDir);
    if (!setup.load() && appMode && listDisks(root).empty()) setup.begin();
    if (setup.active()) curLauncher = setup.launcher();
    std::string setupCancelTo;

    // Choix du panneau Disks, par système. DISK ou OS9_DISK posés à la main priment : le panneau
    // ne les écrase pas (voir diskVarsFor).
    auto diskVarsFor = [&](const std::string& launcher) {
        const bool tiger = launcher == tigerSh;
        if (std::getenv(tiger ? "DISK" : "OS9_DISK")) return std::vector<std::string>{};
        DiskSelection& sel = tiger ? fe.tigerDisks : fe.os9Disks;
        sel = normalizeSelection(sel, listDisks(root), tiger ? "tiger.qcow2" : "os9.qcow2",
                                 maxExtraDisks(root, tiger));
        std::vector<std::string> vars = diskEnv(root, tiger, sel);
        if (setup.active()) {
            const std::vector<std::string> step = setup.vmEnv();
            vars.insert(vars.end(), step.begin(), step.end());
        } else {
            // Preferences. Une variable posée à la main prime.
            const int mem = tiger ? fe.memoryTiger : fe.memoryOs9;
            const char* memVar = tiger ? "RAM_MB" : "OS9_RAM";
            if (mem > 0 && !std::getenv(memVar)) vars.push_back(std::string(memVar) + "=" + std::to_string(mem));
            if (tiger && fe.coresTiger > 0 && !std::getenv("SMP")) vars.push_back("SMP=" + std::to_string(fe.coresTiger));
            if (trySession) vars.push_back("SNAPSHOT=1");
        }
        return vars;
    };
    // Un échec ici s'affiche dans la fenêtre : lancée par le Finder, l'app n'a pas de terminal.
    std::string startProblem;
    bool setupVmStarted = false;
    SetupFlow::Step setupLastStep = setup.step();
    if (setup.active() && !setup.wantsVm()) {
        // Pas de VM pour ces étapes : l'assistant la lancera.
    } else if (!bridge->start(makeConfig(curLauncher, runtimeDir, sound, pad, diskVarsFor(curLauncher)), &err)) {
        std::fprintf(stderr, "QemuBridge start failed: %s\n", err.c_str());
        startProblem = err;
        setupVmStarted = setup.active();
    } else {
        setupVmStarted = setup.active();
        journal("QEMU started: %s", curLauncher.c_str());
        chimeTrigger.arm();
    }

    glfwSetErrorCallback(glfwErr);
    if (!glfwInit()) { std::fprintf(stderr, "GLFW init failed\n"); return 1; }
#ifdef __APPLE__
    // macOS only offers 3.2+ core, forward-compatible contexts (a 3.0
    // request fails: "NSGL: The targeted version of macOS does not support
    // OpenGL 3.0 or 3.1") — the frontend never opened a window on the Mac host.
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    if (std::getenv("POMPPC_FE_DISKS")) fe.showDisks = true;   // pour vérifier le panneau par une capture
    const bool feGeom = fe.winW > 0 && fe.winH > 0;
    // La version vient de l'accueil QMP du QEMU réellement lancé (QEMU_BIN
    // peut viser un autre arbre que ~/src/qemu).
    // Le système, pas la machinerie : la version de QEMU est dans le panneau Status.
    auto windowTitle = [&](const std::string& launcher) {
        return std::string("POMPPC — ") +
               (launcher == tigerSh ? "Mac OS X 10.4 Tiger" : "Mac OS 9.2.2");
    };
    GLFWwindow* window =
        glfwCreateWindow(feGeom ? fe.winW : 1024, feGeom ? fe.winH : 790,
                         windowTitle(curLauncher).c_str(), nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    if (feGeom) glfwSetWindowPos(window, fe.winX, fe.winY);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
#ifdef __APPLE__
    journal("Ctrl+Cmd+F given back to the front end (%d \"Enter Full Screen\" Cocoa menu item(s))",
            disableNativeFullScreen(window));
#endif
    std::signal(SIGINT, onQuitSignal);
    std::signal(SIGTERM, onQuitSignal);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    {
        ImGuiIO& io0 = ImGui::GetIO();
        io0.ConfigWindowsMoveFromTitleBarOnly = true;
        io0.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io0.IniFilename = iniPath.c_str();
    }
    ImGui::StyleColorsDark();
    // Police vectorielle embarquée. ImGui 1.92 la rastérise à la densité de l'écran : nette partout.
    {
        ImFontConfig fontConfig;
        fontConfig.FontDataOwnedByAtlas = false;
        ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRobotoFont),
                                                   (int)sizeof(kRobotoFont), 15.0f, &fontConfig);
    }
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    GLuint tex = 0;
    glfwSetDropCallback(window, onFilesDropped);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Default GL_REPEAT made the linear filter blend the first and last
    // rows/columns with the opposite edge once the view is scaled down.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    std::vector<uint32_t> fb;
    int guestW = 0, guestH = 0;
    int texW = 0, texH = 0;          // géométrie actuellement allouée sur le GPU

    // Ludothèque : le listing était refait À CHAQUE FRAME (deux directory_iterator,
    // un stat par entrée, tri + unique, des dizaines d'allocations, 60 fois par
    // seconde, même fenêtre fermée). Mis en cache, rafraîchi à la demande.
    std::vector<std::string> cdImages = listCdImages(root);
    bool cdImagesStale = false;
    bool grabbed = fe.grabbed;    // keyboard → guest (toggle in the Machine menu)
    // Touches actuellement enfoncées côté invité. Sans ce suivi, décocher
    // « Clavier → invité » (ou ouvrir un champ de saisie ImGui) pendant qu'une
    // touche est tenue n'envoyait jamais le relâchement : Shift/Ctrl restaient
    // collés dans l'invité.
    constexpr int kKeyCount = (int)(sizeof(kKeys) / sizeof(kKeys[0]));
    bool keyHeld[kKeyCount] = {};
    bool keysWereLive = false;
    // Même suivi pour les boutons souris (ordre ImGui G/D/M).
    bool mouseHeld[3] = {};
    bool paused = false;
    // Vue : « ajuster » (plus grand rectangle au ratio de l'invité) par
    // défaut ; un zoom fixe 50-200 % reste possible en fenêtre.
    bool fitView = fe.fitView;
    float zoom = fe.zoom;
    bool showLibrary = fe.showLibrary;      // ludothèque window
    bool showBilan = fe.showBilan;
    bool showJournal = fe.showJournal;
    bool resetLayout = false;
    std::string currentCd;        // game CD currently inserted in gamecd

    // Plein écran hôte : la fenêtre GLFW passe sur son moniteur à la
    // résolution native ; on retient la géométrie fenêtrée pour y revenir.
    bool fullscreen = false;
    // Toujours en fenêtre au démarrage (demande de l'utilisateur, 26/10) :
    // Alt+Entrée bascule ensuite. Le plein écran retenu à l'arrêt n'est plus
    // rétabli (fe.fullscreen reste écrit, pour l'information seulement).
    bool toggleFullscreen = false;
    int winX = 0, winY = 0, winW = 1280, winH = 900;
    glfwGetWindowPos(window, &winX, &winY);
    glfwGetWindowSize(window, &winW, &winH);
    if (feLoaded) journal("settings loaded: %s", feSettingsPath.c_str());

    // Rectangle réellement dessiné (points ImGui) : la seule référence de la
    // souris invité. Mis à jour à chaque frame par la vue de l'invité.
    ImVec2 drawOrigin(0, 0), drawSize(0, 0);
    float relAccX = 0, relAccY = 0;  // restes fractionnaires du mode relatif
    int lastGx = -1, lastGy = -1;    // dernière position absolue envoyée
    int lastAbs = -1;                // dernier mode souris vu (-1 : inconnu)
    // Machine ▸ Souris absolue : run_os9.sh ajoute une virtio-tablet pour une
    // souris absolue, mais la souris USB HID de mac99 redevient « courante »
    // dès qu'OS 9 l'interroge ; on rend la tablette courante (mouse_set).
    // Décocher pour les jeux qui veulent des déplacements relatifs.
    bool preferTablet = fe.preferTablet;
    bool pointerPrefChanged = false;
    double nextPointerRetry = 0;
    bool pointerCaptured = false;
    bool suppressPointerDelta = false;

    // Un geste qui couperait l'invité se confirme d'abord (fenêtres dessinées à la fin de l'image).
    bool askQuit = false, quitWhenStopped = false, forceQuit = false, askRestart = false;
    bool stoppedSeen = false;
    std::string restartTitle;
    std::function<void()> restartAction;
    auto osNameOf = [&](const std::string& launcher) {
        return std::string(launcher == tigerSh ? "Tiger" : "Mac OS 9");
    };
    auto confirmRestart = [&](const std::string& title, std::function<void()> action) {
        if (!bridge->running()) { action(); return; }
        restartTitle = title;
        restartAction = std::move(action);
        askRestart = true;
    };
    const std::string appLog = runtimeDir + "/app.log";
    auto openFile = [](const std::string& path) {
#ifdef __APPLE__
        const char* tool = "open";
        const char* args[] = {tool, path.c_str(), nullptr};
        pid_t pid = -1;
        posix_spawnp(&pid, tool, nullptr, nullptr, const_cast<char* const*>(args), environ);
#else
        SetupFlow::openExternal(path);
#endif
    };

    // Un message discret, quelques secondes, en haut de la fenêtre.
    std::string toastText;
    double toastUntil = 0;
    auto showToast = [&](const std::string& text, double seconds = 5.0) {
        toastText = text;
        toastUntil = glfwGetTime() + seconds;
        journal("%s", text.c_str());
    };
    bool showPrefs = false, showAbout = false;

    // Dossier partagé (SharedFolder.h) : une image à la fois, puisqu'une seule VM tourne ici. Tant que
    // l'image existe, ce que l'invité y a écrit n'est peut-être pas revenu : elle n'est jamais écrasée.
    enum class ShareState { Off, Building, Attaching, Attached, Releasing, Syncing, Ejected };
    ShareState shareState = ShareState::Off;
    BackgroundTask shareTask, shareChoose;
    std::string shareFolder, shareProblem, shareLast;
    bool shareBlocked = false;           // un échec : on attend l'utilisateur, sans réessayer en boucle
    bool shareRecovering = false;        // l'image d'un arrêt brutal : une fois rendue, on branche normalement
    double shareNext = 0;
    int shareTries = 0;
    const std::string shareImage = runtimeDir + "/shared-disk.dmg";
    const std::string shareList = runtimeDir + "/shared-disk.list";
    const std::string shareMount = runtimeDir + "/shared-mount";
    const std::string shareId = "pomppcshared";
    auto shareStartSync = [&](const std::string& folder, bool allowDeletes) {
        shareState = ShareState::Syncing;
        const std::string guest = osNameOf(curLauncher);
        shareTask.start([=](std::string* e) {
            SharedSyncReport report;
            syncSharedDisk(folder, shareMount, shareImage, shareList, allowDeletes, guest, &report, e);
            return report.summary();
        });
    };
    // Avant de quitter ou de relancer : QEMU est parti, plus personne n'écrit dans l'image.
    auto shareFinishNow = [&](bool clean) {
        std::string r, e;
        while (shareTask.busy()) usleep(20000);
        shareTask.poll(&r, &e);
        std::error_code ec;
        if (fs::exists(shareImage, ec) && !shareBlocked) {
            const std::string folder = sharedManifestFolder(shareList);
            SharedSyncReport report;
            e.clear();
            if (folder.empty() || !syncSharedDisk(folder, shareMount, shareImage, shareList, clean,
                                                  osNameOf(curLauncher), &report, &e)) {
                shareProblem = e;
                shareBlocked = true;
                journal("shared folder: %s", e.c_str());
            } else {
                journal("shared folder: %s", report.summary().c_str());
            }
        }
        shareState = ShareState::Off;
    };

    auto relaunch = [&](const std::string& launcher) {
        chimePlayer.stop();
        chimeTrigger.pending = false;
        stoppedSeen = false;
        startProblem.clear();
        quitWhenStopped = false;
        const bool cleanStop = !bridge->running() && bridge->exitedCleanly();
        bridge->stop();
        shareFinishNow(cleanStop);
        bridge = std::make_unique<QemuBridge>();
        std::string e;
        if (!bridge->start(makeConfig(launcher, runtimeDir, sound, pad, diskVarsFor(launcher)), &e)) {
            journal("restart failed: %s", e.c_str());
            startProblem = e;
        } else {
            curLauncher = launcher;
            journal("QEMU restarted: %s", launcher.c_str());
            glfwSetWindowTitle(window, windowTitle(launcher).c_str());
            chimeTrigger.arm();
        }
        guestW = guestH = 0;
        texW = texH = 0;                 // la texture GPU sera réallouée
        fb.clear();                      // force un latch complet
        for (bool& k : keyHeld) k = false;
        for (bool& m : mouseHeld) m = false;
        lastAbs = -1;
        nextPointerRetry = 0;
        paused = false;
        currentCd.clear();
        cdImagesStale = true;
    };

    // Draw the guest texture in the current window at the largest rectangle
    // with the guest's aspect ratio (or the fixed zoom), centred, and forward
    // the pointer relative to that rectangle only. The window background is
    // black, so the rest of the content region is the letterbox.
    // Agrandissement net : chaque pixel de l'invité est d'abord multiplié par un entier (au plus
    // proche, FBO), puis l'image est ramenée à sa taille en bilinéaire. Le flou reste limité aux
    // bords des pixels, et les pixels gardent tous la même largeur à un rapport non entier.
    // Lissé : bilinéaire direct. Un rapport entier ou une réduction se passent de FBO.
    GLuint sharpTex = 0, sharpFbo = 0, sharpFboSrc = 0;
    int sharpW = 0, sharpH = 0;
    bool guestTexDirty = true;
    auto shownTexture = [&](ImVec2 sz) -> GLuint {
        int fbw = 0, ww = 0, unused = 0;
        glfwGetFramebufferSize(window, &fbw, &unused);
        glfwGetWindowSize(window, &ww, &unused);
        const float density = ww > 0 ? (float)fbw / (float)ww : 1.0f;
        const float scale = sz.x * density / (float)guestW;
        const bool integer = std::fabs(scale - std::round(scale)) < 0.001f;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        fe.sharpImage ? GL_NEAREST : GL_LINEAR);
        static auto genFb = (GlGenFramebuffers)glfwGetProcAddress("glGenFramebuffers");
        static auto bindFb = (GlBindFramebuffer)glfwGetProcAddress("glBindFramebuffer");
        static auto attach = (GlFramebufferTexture2D)glfwGetProcAddress("glFramebufferTexture2D");
        static auto blit = (GlBlitFramebuffer)glfwGetProcAddress("glBlitFramebuffer");
        if (!fe.sharpImage || integer || scale < 1.0f || !genFb || !bindFb || !attach || !blit) return tex;
        const int factor = std::min((int)std::ceil(scale), 8);
        const int w = guestW * factor, h = guestH * factor;
        if (!sharpFbo) {
            genFb(1, &sharpFbo);
            genFb(1, &sharpFboSrc);
            glGenTextures(1, &sharpTex);
        }
        const bool resized = w != sharpW || h != sharpH;
        if (resized) {
            sharpW = w; sharpH = h;
            glBindTexture(GL_TEXTURE_2D, sharpTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        if (resized || guestTexDirty) {
            const GLenum readFb = 0x8CA8, drawFb = 0x8CA9, colour0 = 0x8CE0;
            bindFb(readFb, sharpFboSrc);
            attach(readFb, colour0, GL_TEXTURE_2D, tex, 0);
            bindFb(drawFb, sharpFbo);
            attach(drawFb, colour0, GL_TEXTURE_2D, sharpTex, 0);
            blit(0, 0, guestW, guestH, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            bindFb(readFb, 0);
            bindFb(drawFb, 0);
            guestTexDirty = false;
        }
        return sharpTex;
    };

    auto guestView = [&](bool allowZoom) {
        ImGuiIO& io = ImGui::GetIO();
        if (guestW <= 0) {
            drawSize = ImVec2(0, 0);
            if (!stoppedSeen && startProblem.empty()) {
                ImGui::TextDisabled("Starting %s…", osNameOf(curLauncher).c_str());
                return;
            }
            // QEMU n'est plus là. Un arrêt voulu n'est pas une panne : on le dit autrement.
            const bool clean = startProblem.empty() && bridge->exitedCleanly();
            const std::string os = osNameOf(curLauncher);
            ImGui::SetCursorPos(ImVec2(24, 24));
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520);
            if (clean) ImGui::Text("%s has shut down.", os.c_str());
            else ImGui::Text("%s couldn't start, or stopped unexpectedly.", os.c_str());
            if (!startProblem.empty()) ImGui::TextDisabled("%s", startProblem.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            std::string again = clean ? "Start " + os : std::string("Try Again");
            if (ImGui::Button(again.c_str())) relaunch(curLauncher);
            std::error_code logEc;
            if (!clean && fs::exists(appLog, logEc)) {
                ImGui::SameLine();
                if (ImGui::Button("Show Log")) openFile(appLog);
            }
            ImGui::SameLine();
            if (ImGui::Button("Quit")) {
                forceQuit = true;
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            ImGui::EndGroup();
            return;
        }
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 sz = (fitView || !allowZoom)
                        ? fitRect(avail, guestW, guestH)
                        : ImVec2(std::round(guestW * zoom), std::round(guestH * zoom));
        if (sz.x < 1 || sz.y < 1) { drawSize = ImVec2(0, 0); return; }
        ImVec2 cur = ImGui::GetCursorPos();
        float ox = std::max(0.0f, std::floor((avail.x - sz.x) * 0.5f));
        float oy = std::max(0.0f, std::floor((avail.y - sz.y) * 0.5f));
        ImGui::SetCursorPos(ImVec2(cur.x + ox, cur.y + oy));
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)(intptr_t)shownTexture(sz), sz);
        drawOrigin = origin;
        drawSize = sz;
        // IsItemHovered = inside the drawn (and visible, clipped) rectangle,
        // with no ImGui window or menu on top: the bands never send events.
        const bool pointerReady = bridge->running() && !paused &&
            (!fullscreen || !bridge->mouseIsAbsolute());
        bool hovered = pointerReady && (pointerCaptured || ImGui::IsItemHovered());
        float sx = (float)guestW / sz.x, sy = (float)guestH / sz.y;
        // Un seul curseur sur l'image (01/11) : en absolu, celui de Tiger suit
        // déjà la souris. ImGuiMouseCursor_None fait passer le backend GLFW en
        // GLFW_CURSOR_HIDDEN à la frame suivante ; hors de l'image (menus,
        // bandeaux), en pause ou VM arrêtée, ImGui remet la flèche et le
        // backend GLFW_CURSOR_NORMAL. Le glfwSetInputMode direct couvre le cas
        // où le backend a oublié son dernier curseur (démarrage souris déjà
        // sur l'image, sortie de capture) et ne masquerait rien.
        if (wantsHiddenHostCursor(hovered, bridge->mouseIsAbsolute(), pointerCaptured)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
        }
        if (hovered) {
            if (bridge->mouseIsAbsolute()) {
                int gx = (int)std::floor((io.MousePos.x - origin.x) * sx);
                int gy = (int)std::floor((io.MousePos.y - origin.y) * sy);
                gx = std::clamp(gx, 0, guestW - 1);
                gy = std::clamp(gy, 0, guestH - 1);
                if (gx != lastGx || gy != lastGy) {
                    bridge->mouseAbs(gx, gy);
                    lastGx = gx; lastGy = gy;
                }
            } else if (!suppressPointerDelta && (io.MouseDelta.x || io.MouseDelta.y)) {
                relAccX += io.MouseDelta.x * sx;
                relAccY += io.MouseDelta.y * sy;
                int dx = (int)relAccX, dy = (int)relAccY;
                relAccX -= dx; relAccY -= dy;
                if (dx || dy) bridge->mouseRel(dx, dy);
            }
            if (io.MouseWheel > 0) bridge->mouseWheel(3);
            if (io.MouseWheel < 0) bridge->mouseWheel(4);
        } else {
            relAccX = relAccY = 0;
            lastGx = lastGy = -1;   // re-send on re-entry
        }
        for (int b = 0; b < 3; ++b) {
            int qb = (b == 1) ? 2 : (b == 2 ? 1 : 0);  // ImGui M/R ↔ qemu R/M
            if (ImGui::IsMouseClicked(b) && hovered) {
                bridge->mouseButton(qb, true);
                mouseHeld[b] = true;
                if (b == 0)
                    journal("host click (%.0f,%.0f) -> guest (%d,%d) %s [rect %.0fx%.0f @ %.0f,%.0f]",
                            io.MousePos.x, io.MousePos.y,
                            (int)std::floor((io.MousePos.x - origin.x) * sx),
                            (int)std::floor((io.MousePos.y - origin.y) * sy),
                            bridge->mouseIsAbsolute() ? "abs" : "rel", sz.x, sz.y,
                            origin.x, origin.y);
            }
            // !IsMouseDown plutôt que IsMouseReleased : à la perte de focus,
            // ImGui efface l'état souris (ClearInputMouse, DownDurationPrev
            // à -1) et IsMouseReleased ne devient jamais vrai.
            if (ImGui::IsMouseReleased(b) || (mouseHeld[b] && !ImGui::IsMouseDown(b))) {
                bridge->mouseButton(qb, false);
                mouseHeld[b] = false;
            }
        }
    };

    // Épreuve scriptée : injectée après le backend GLFW (dont la position de
    // repli passerait sinon devant), une phase par frame (appui, relâchement).
    std::vector<ProbeStep> probe;
    if (const char* ps = std::getenv("POMPPC_FE_SCRIPT")) probe = parseProbe(ps);
    size_t probeIdx = 0;
    int probePhase = 0;
    double probeUntil = 0;
    float probeMx = -1, probeMy = -1;
    std::string probeGlShot;
    std::string probeShot;
    auto probeKeys = [](std::initializer_list<ImGuiKey> ks, bool down) {
        ImGuiIO& pio = ImGui::GetIO();
        for (ImGuiKey k : ks) {
            if (k == ImGuiKey_LeftCtrl) pio.AddKeyEvent(ImGuiMod_Ctrl, down);
            if (k == ImGuiKey_LeftSuper) pio.AddKeyEvent(ImGuiMod_Super, down);
            if (k == ImGuiKey_LeftAlt) pio.AddKeyEvent(ImGuiMod_Alt, down);
            pio.AddKeyEvent(k, down);
        }
    };
    auto probeTick = [&]() {
        if (probeIdx >= probe.size()) return;
        ImGuiIO& pio = ImGui::GetIO();
        if (probeMx >= 0) pio.AddMousePosEvent(probeMx, probeMy);
        double now = glfwGetTime();
        if (now < probeUntil) return;
        const ProbeStep& st = probe[probeIdx];
        bool done = true;
        if (st.op == "wait") {
            if (probePhase == 0) { probeUntil = now + st.x; done = false; }
        } else if (st.op == "guestmove") {
            if (drawSize.x <= 0 || drawSize.y <= 0) return;
            probeMx = drawOrigin.x + std::clamp(st.x, 0.0f, 1.0f) * (drawSize.x - 1);
            probeMy = drawOrigin.y + std::clamp(st.y, 0.0f, 1.0f) * (drawSize.y - 1);
            pio.AddMousePosEvent(probeMx, probeMy);
            journal("position probe: host %.1f,%.1f -> guest %d,%d", probeMx, probeMy,
                    (int)((probeMx - drawOrigin.x) * guestW / drawSize.x),
                    (int)((probeMy - drawOrigin.y) * guestH / drawSize.y));
        } else if (st.op == "move") {
            probeMx = st.x; probeMy = st.y;
            pio.AddMousePosEvent(probeMx, probeMy);
        } else if (st.op == "click") {
            if (probePhase == 0) {
                probeMx = st.x; probeMy = st.y;
                pio.AddMousePosEvent(probeMx, probeMy);
                done = false;
            } else if (probePhase == 1) {
                pio.AddMouseButtonEvent(0, true);
                done = false;
            } else {
                pio.AddMouseButtonEvent(0, false);
            }
        } else if (st.op == "fs" || st.op == "f11" || st.op == "esc" || st.op == "enter" ||
                   st.op == "altenter") {
            bool down = (probePhase == 0);
            if (st.op == "fs") probeKeys({ImGuiKey_LeftCtrl, ImGuiKey_LeftSuper, ImGuiKey_F}, down);
            else if (st.op == "altenter") probeKeys({ImGuiKey_LeftAlt, ImGuiKey_Enter}, down);
            else if (st.op == "f11") probeKeys({ImGuiKey_F11}, down);
            else if (st.op == "esc") probeKeys({ImGuiKey_Escape}, down);
            else probeKeys({ImGuiKey_Enter}, down);
            done = !down;
        } else if (st.op == "type") {
            size_t ci = (size_t)probePhase / 2;
            if (ci < st.arg.size()) {
                ImGuiKey k = probeKey(st.arg[ci]);
                if (k != ImGuiKey_None) probeKeys({k}, probePhase % 2 == 0);
                // Aussi le caractère : un champ d'ImGui lit le texte, pas les touches.
                if (probePhase % 2 == 0) ImGui::GetIO().AddInputCharacter((unsigned char)st.arg[ci]);
                done = false;
            }
        } else if (st.op == "shot") {
            probeShot = st.arg;
        } else if (st.op == "glshot") {
            probeGlShot = st.arg;
        } else if (st.op == "quit") {
            forceQuit = true;
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        } else if (st.op == "close") {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        } else if (st.op == "drop") {
            gDroppedFiles.push_back(st.arg);
        }
        if (done) {
            journal("probe: %s %s", st.op.c_str(), st.arg.c_str());
            ++probeIdx;
            probePhase = 0;
        } else {
            ++probePhase;
        }
    };

    // Tâches de fond : le CD des fichiers déposés, la recherche de mise à jour.
    BackgroundTask dropTask, updateTask;
    struct UsbDisk { std::string id, path, label; bool showOnMac = false; };
    std::vector<UsbDisk> usbDisks, usbReleasing;
    std::string pendingUsb;
    bool pendingUsbReadOnly = false, dropReadOnly = false;
    int usbCounter = 0, usbReleaseTries = 0;
    double usbReleaseNext = 0;
    std::string updateFound;

    // Ajustement de la fenêtre à l'invité : après 0,7 s sans changement de mode, pour ne pas suivre
    // chaque étape du démarrage (640x480, puis le mode du bureau).
    int fitGuestW = 0, fitGuestH = 0, fitLastW = 0, fitLastH = 0, fitSettle = 0;
    double fitSince = 0;
    bool fitDone = false;

    while (true) {
        if (glfwWindowShouldClose(window)) {
            // Fermer la fenêtre couperait l'invité. Un signal ne demande rien : personne n'est là
            // pour répondre.
            if (gQuitSignal || forceQuit || !bridge->checkAlive()) break;
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            askQuit = true;
        }
        glfwPollEvents();
        if (gQuitSignal) glfwSetWindowShouldClose(window, GLFW_TRUE);
        // After the Cocoa event pump, which puts this thread back in the
        // interactive band above QEMU's audio timer.
        preferGuestAudio();
        // Une confirmation à l'écran rend le pointeur : il faut pouvoir y cliquer, en plein écran aussi.
        const bool uiModal = askQuit || askRestart || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        const bool capture = wantsPointerCapture(fullscreen,
            glfwGetWindowAttrib(window, GLFW_FOCUSED), bridge->running(), paused || uiModal,
            bridge->mouseIsAbsolute());
        suppressPointerDelta = capture != pointerCaptured;
        if (suppressPointerDelta) {
            if (capture) showToast("The mouse is captured. Ctrl+Cmd+F, F11 or Alt+Return leaves full screen and releases it.");
            pointerCaptured = capture;
            glfwSetInputMode(window, GLFW_CURSOR, capture ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
            if (glfwRawMouseMotionSupported())
                glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, capture ? GLFW_TRUE : GLFW_FALSE);
#endif
            relAccX = relAccY = 0;
            lastGx = lastGy = -1;
            for (int button = 0; button < 3; ++button) bridge->mouseButton(button, false);
            for (bool& m : mouseHeld) m = false;
            journal("host mouse: %s", capture ? "captured, relative without edges" : "released");
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        probeTick();
        ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();

        // ── Raccourcis plein écran (avalés : jamais transmis à l'invité) ──
        // Alt+Entrée (comme les jeux sous Windows), Ctrl+Cmd+F comme macOS, F11.
        // Échap reste une touche de l'invité, notamment pour les menus des
        // jeux, même en plein écran capturé.
        bool swallow[kKeyCount] = {};
        auto swallowKey = [&](ImGuiKey k) {
            for (int i = 0; i < kKeyCount; ++i) if (kKeys[i].k == k) swallow[i] = true;
        };
        const bool altEnter = io.KeyAlt && !io.KeyCtrl && !io.KeySuper &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        if (altEnter || ImGui::IsKeyPressed(ImGuiKey_F11, false) ||
            (io.KeyCtrl && io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_F, false))) {
            toggleFullscreen = true;
            swallowKey(ImGuiKey_F11);
            swallowKey(ImGuiKey_F);
            swallowKey(ImGuiKey_Enter);
            swallowKey(ImGuiKey_KeypadEnter);
        }
        int w = 0, h = 0, dy0 = 0, dy1 = 0;
        bool resized = false;
        if (bridge->latchFrame(fb, w, h, dy0, dy1, resized)) {
            bridge->checkAlive();
            if (chimeTrigger.frame(bridge->running(), w, h, chime.enabled, chime.volume)) {
                std::string error;
                if (chimePlayer.play(chime.file, chime.volume, error))
                    journal("host chime: %s (volume %.0f %%)", chime.file.c_str(), chime.volume * 100);
                else journal("chime not played: %s", error.c_str());
            }
            if (w != guestW || h != guestH) {
                journal("scanout %dx%d", w, h);
                fitGuestW = w; fitGuestH = h;
                fitSince = glfwGetTime();
                fitDone = false;
            }
            guestW = w; guestH = h;
            guestTexDirty = true;
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            // QEMU's mac99 scanout is PIXMAN_x8r8g8b8: the high byte is
            // padding, not alpha, and is commonly zero. An RGBA texture made
            // ImGui blend the whole guest display as transparent. Store RGB
            // so OpenGL supplies an opaque alpha value when sampling.
            if (resized || w != texW || h != texH) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_BGRA,
                             GL_UNSIGNED_BYTE, fb.data());
                texW = w; texH = h;
            } else if (dy1 > dy0) {
                // Réallouer la texture entière à chaque frame coûtait un
                // upload complet (3 Mo en 1024x768) : on ne pousse que les
                // lignes que QEMU a réellement modifiées.
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dy0, w, dy1 - dy0, GL_BGRA,
                                GL_UNSIGNED_BYTE, fb.data() + (size_t)dy0 * w);
            }
        }

        // QEMU peut mourir de son côté : sans ce test, le frontend affichait
        // indéfiniment la dernière image en annonçant « en marche ».
        // Fichiers déposés. Pendant l'étape du CD de l'assistant, c'est le disque d'installation.
        if (!gDroppedFiles.empty()) {
            std::vector<std::string> files;
            files.swap(gDroppedFiles);
            std::error_code ec;
            const std::string exchange = root + "/Exchange";
            if (setup.step() == SetupFlow::Step::Disc && !setup.busy() && files.size() == 1) {
                setup.placeDisc(files[0], false);
            } else if (!bridge->running()) {
                showToast("Start the system first, then drop your files again.");
            } else if (dropTask.busy()) {
                showToast("POMPPC is still preparing the previous disk.");
            } else if (classifyDrop(files) == DropKind::DiscImage && !isCompressedImage(files[0])) {
                pendingUsb = files[0];
                pendingUsbReadOnly = true;
            } else if (classifyDrop(files) != DropKind::Nothing) {
                fs::create_directories(exchange, ec);
                std::string name = "From Mac";
                for (int n = 2; fs::exists(exchange + "/" + name + ".dmg", ec); ++n)
                    name = "From Mac " + std::to_string(n);
                const std::string out = exchange + "/" + name + ".dmg";
                const std::string mount = runtimeDir + "/exchange-mount";
                if (classifyDrop(files) == DropKind::DiscImage) {
                    // Un .dmg est compressé : le disque de QEMU veut une image brute.
                    showToast("Preparing the disc...", 60.0);
                    const std::string source = files[0], iso = exchange + "/" + name + ".iso";
                    dropReadOnly = true;
                    dropTask.start([source, iso](std::string* e) {
                        return convertDiscImage(source, iso, e) ? iso : std::string();
                    });
                } else {
                    showToast("Preparing a disk with your files...", 60.0);
                    dropReadOnly = false;
                    dropTask.start([files, mount, out](std::string* e) {
                        return buildExchangeDisk(files, mount, out, e) ? out : std::string();
                    });
                }
            }
        }
        {
            std::string built, e;
            if (dropTask.poll(&built, &e)) {
                if (built.empty()) {
                    showToast(e);
                } else {
                    pendingUsb = built;
                    pendingUsbReadOnly = dropReadOnly;
                }
            }
        }
        if (!pendingUsb.empty()) {
            const std::string id = "pomppcusb" + std::to_string(++usbCounter);
            if (usbDisks.size() + (shareState == ShareState::Attached ? 1 : 0) >= 4) {
                showToast("Eject a USB disk first, in the Devices menu.");
            } else if (bridge->attachUsbDisk(pendingUsb, pendingUsbReadOnly, id)) {
                usbDisks.push_back({id, pendingUsb, fs::path(pendingUsb).stem().string()});
                showToast("\"" + usbDisks.back().label + "\" is on the desktop of Mac OS.");
            } else {
                showToast("POMPPC couldn't connect this disk.");
            }
            pendingUsb.clear();
        }
        // Un disque éjecté n'est libre qu'une fois le périphérique parti : quelques essais.
        if (!usbReleasing.empty() && glfwGetTime() >= usbReleaseNext) {
            usbReleaseNext = glfwGetTime() + 0.5;
            if (bridge->releaseUsbDisk(usbReleasing.front().id)) {
                if (usbReleasing.front().showOnMac) openFile(usbReleasing.front().path);
                usbReleasing.erase(usbReleasing.begin());
            } else if (++usbReleaseTries > 20) {
                showToast("POMPPC couldn't release this disk. It will be free when the system stops.");
                usbReleasing.erase(usbReleasing.begin());
                usbReleaseTries = 0;
            }
        }
        if (!bridge->running()) { usbDisks.clear(); usbReleasing.clear(); }
        // Dossier partagé : construire, brancher, attendre l'éjection ou l'arrêt, ramener.
        {
            std::string done, e;
            const std::string wanted = curLauncher == tigerSh ? fe.sharedTiger : fe.sharedOs9;
            std::error_code ec;
            if (shareTask.poll(&done, &e)) {
                if (shareState == ShareState::Building) {
                    if (done.empty()) {
                        shareProblem = e;
                        shareBlocked = true;
                        shareState = ShareState::Off;
                        journal("shared folder: %s", e.c_str());
                    } else {
                        shareFolder = done;
                        shareState = ShareState::Attaching;
                        shareTries = 0;
                        shareNext = 0;
                    }
                } else if (shareState == ShareState::Syncing) {
                    if (e.empty()) {
                        shareLast = done;
                        showToast("\"" + sharedVolumeName(shareFolder) + "\" is back on your Mac. " + done, 8.0);
                    } else {
                        shareLast.clear();
                        shareProblem = e;
                        shareBlocked = true;
                        journal("shared folder: %s", e.c_str());
                    }
                    shareState = bridge->running() && !shareRecovering ? ShareState::Ejected : ShareState::Off;
                    shareRecovering = false;
                }
            }
            const double now = glfwGetTime();
            switch (shareState) {
                case ShareState::Off:
                    if (shareBlocked || shareTask.busy()) break;
                    if (fs::exists(shareImage, ec)) {
                        // Restée d'un arrêt brutal : rien ne dit qu'elle est complète, donc sans suppression.
                        shareFolder = sharedManifestFolder(shareList);
                        if (shareFolder.empty()) {
                            shareProblem = "POMPPC couldn't read the disk's list of files. It's still here: " + shareImage;
                            shareBlocked = true;
                        } else {
                            shareRecovering = true;
                            shareStartSync(shareFolder, false);
                        }
                    } else if (bridge->running() && !wanted.empty() && !setup.active() && !trySession) {
                        const std::string problem = sharedFolderProblem(wanted, root);
                        if (!problem.empty()) {
                            shareProblem = problem;
                            shareBlocked = true;
                        } else {
                            shareProblem.clear();
                            shareLast.clear();
                            shareState = ShareState::Building;
                            shareTask.start([=](std::string* err) {
                                return buildSharedDisk(wanted, shareMount, shareImage, shareList, err) ? wanted : std::string();
                            });
                        }
                    }
                    break;
                case ShareState::Attaching:
                    if (!bridge->running()) { shareStartSync(shareFolder, false); break; }
                    if (now < shareNext) break;
                    shareNext = now + 1.0;
                    if (bridge->attachUsbDisk(shareImage, false, shareId)) {
                        shareState = ShareState::Attached;
                        journal("shared folder connected: %s", shareFolder.c_str());
                    } else if (++shareTries > 15) {
                        shareProblem = "POMPPC couldn't connect the shared folder's disk.";
                        shareStartSync(shareFolder, false);
                    }
                    break;
                case ShareState::Attached: {
                    if (!bridge->running()) {
                        shareStartSync(shareFolder, bridge->exitedCleanly());
                        break;
                    }
                    if (now < shareNext) break;
                    shareNext = now + 1.0;
                    std::string reply;
                    if (bridge->qmpCommand("{\"execute\":\"query-block\"}", &reply) && usbDiskEjected(reply, shareId) &&
                        bridge->detachUsbDisk(shareId)) {
                        shareState = ShareState::Releasing;
                        shareTries = 0;
                    }
                    break;
                }
                case ShareState::Releasing:
                    if (now < shareNext) break;
                    shareNext = now + 0.5;
                    // L'invité a éjecté : l'image est complète, même si QEMU tarde à la rendre.
                    if (!bridge->running() || bridge->releaseUsbDisk(shareId) || ++shareTries > 20)
                        shareStartSync(shareFolder, true);
                    break;
                case ShareState::Ejected:
                    if (!bridge->running()) shareState = ShareState::Off;
                    break;
                default:
                    break;
            }
        }
        {
            std::string tag, e;
            if (updateTask.poll(&tag, &e)) {
                const char* mine = std::getenv("POMPPC_VERSION");
                if (!e.empty()) showToast(e);
                else if (mine && compareVersions(tag, mine) > 0) updateFound = tag;
                else if (mine) showToast("POMPPC is up to date.");
                else showToast("The latest version is " + tag + ".");
            }
        }

        setup.poll();
        const bool setupPage = setup.active() && !setup.wantsVm();
        if (setup.step() != setupLastStep) {
            setupLastStep = setup.step();
            setupVmStarted = false;
            stoppedSeen = false;
            fitDone = false;
            fitSince = 0;
        }
        // L'invité redémarre à la fin de l'installateur ou du combo : -no-reboot fait sortir QEMU, et
        // l'étape suivante s'enchaîne. Tout autre arrêt reste un arrêt, avec son écran.
        const bool stepEndsOnExit = setup.step() == SetupFlow::Step::Installing ||
                                    setup.step() == SetupFlow::Step::Updating;
        if (setupVmStarted && stepEndsOnExit && !bridge->checkAlive() && startProblem.empty() &&
            bridge->exitedCleanly()) {
            guestW = guestH = 0;
            setup.onVmExit();
        } else if (setup.wantsVm() && !setupVmStarted && startProblem.empty()) {
            setupVmStarted = true;
            relaunch(setup.launcher());
        } else if (!setupPage && !bridge->checkAlive() && !stoppedSeen && startProblem.empty() &&
                   (!setup.active() || setupVmStarted)) {
            stoppedSeen = true;
            guestW = guestH = 0;
            chimePlayer.stop();
            journal(bridge->exitedCleanly() ? "%s has shut down" : "%s stopped unexpectedly",
                    osNameOf(curLauncher).c_str());
            if (quitWhenStopped) {
                forceQuit = true;
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }
        // Le pilote répond-il ? L'étape du pilote s'arrête là ; ensuite, le bandeau le rappelle.
        static double driverPoll = 0;
        static int driverState = -1;      // -1 inconnu, 0 absent, 1 présent
        static double vmSince = 0;
        static bool vmWasRunning = false;
        // Sans VM, plus rien n'est su du pilote : l'état de la VM précédente ne doit pas clore une étape.
        if (!bridge->running()) driverState = -1;
        else if (!vmWasRunning) vmSince = glfwGetTime();
        vmWasRunning = bridge->running();
        if (bridge->running() && curLauncher == tigerSh && glfwGetTime() - driverPoll > 2.0) {
            driverPoll = glfwGetTime();
            std::string reply;
            if (bridge->qmpCommand("{\"execute\":\"qom-get\",\"arguments\":{\"path\":"
                                   "\"/machine/peripheral/gpu0\",\"property\":\"guest-driver\"}}", &reply)) {
                if (reply.find("true") != std::string::npos) driverState = 1;
                else if (reply.find("false") != std::string::npos) driverState = 0;
            }
        }
        const char* appVersion = std::getenv("POMPPC_VERSION");
        const std::string driverMark = runtimeDir + "/pilote-annonce";
        auto markDriver = [&] { if (appVersion) std::ofstream(driverMark) << appVersion << "\n"; };
        if (setup.step() == SetupFlow::Step::Driver && driverState == 1) {
            markDriver();
            setup.go(SetupFlow::Step::Finished);
            journal("3D driver detected");
        }
        // Hors assistant : un rappel, si le pilote manque ou date d'une autre version de POMPPC.
        static bool driverHintHidden = false;
        static std::string driverMarkRead;
        static bool driverMarkLoaded = false;
        if (!driverMarkLoaded) {
            std::ifstream in(driverMark);
            in >> driverMarkRead;
            driverMarkLoaded = true;
        }
        auto setupScreen = [&]() {
            SetupActions actions;
            actions.quit = [&] { forceQuit = true; glfwSetWindowShouldClose(window, GLFW_TRUE); };
            if (!setupCancelTo.empty()) {
            actions.cancel = [&] {
                setup.end();
                const std::string back = setupCancelTo;
                setupCancelTo.clear();
                relaunch(back);
            };
            }
            drawSetupPage(setup, actions);
        };
        const char* driverHint = nullptr;
        if (appMode && !setup.active() && !driverHintHidden && bridge->running() && curLauncher == tigerSh) {
            // Deux minutes : le kext se charge tard au démarrage d'une machine lente.
            if (driverState == 0 && glfwGetTime() - vmSince > 120.0)
                driverHint = "The 3D driver isn't running. Open the POMPPC_GUEST CD on the desktop, double-click Install POMPPC, then restart.";
            else if (driverState == 1 && appVersion && driverMarkRead != appVersion)
                driverHint = "POMPPC was updated. Install its 3D driver again: open the POMPPC_GUEST CD on the desktop, double-click Install POMPPC, then restart.";
        }
        // Une bande qui paraît ou disparaît ne doit pas rendre l'image floue : la fenêtre la suit.
        static bool hintWasShown = false;
        if ((driverHint != nullptr) != hintWasShown) {
            hintWasShown = driverHint != nullptr;
            fitDone = false;
            fitSince = 0;
        }
        // La fenêtre suit l'invité, à 100 %, tant que l'utilisateur ne l'a pas redimensionnée.
        if (fe.autoFit && !fullscreen) {
            int ww = 0, wh = 0;
            glfwGetWindowSize(window, &ww, &wh);
            if (fitSettle > 0) {
                if (--fitSettle == 0) { fitLastW = ww; fitLastH = wh; }
            } else if (fitLastW > 0 && (ww != fitLastW || wh != fitLastH)) {
                fe.autoFit = false;
            }
            if (fe.autoFit && !fitDone && fitGuestW > 0 && glfwGetTime() - fitSince > 0.7) {
                fitDone = true;
                if (!showLibrary && !showBilan && !showJournal && !fe.showDisks) {
                    int ax = 0, ay = 0, aw = 0, ah = 0, wx = 0, wy = 0, ft = 0;
                    // [A] M4, 03/11 : lancé écran du Mac en veille, GLFW ne recense aucun écran,
                    // même au réveil : monitorOf() rend NULL et glfwGetMonitorWorkarea plantait
                    // le frontal. Sans écran connu, la fenêtre suit l'invité sans borne.
                    GLFWmonitor* fitMonitor = monitorOf(window);
                    if (fitMonitor) {
                        glfwGetMonitorWorkarea(fitMonitor, &ax, &ay, &aw, &ah);
                    } else {
                        ax = ay = -32768;
                        aw = ah = 65535;
                    }
                    glfwGetWindowFrameSize(window, nullptr, &ft, nullptr, nullptr);
                    const int menuH = (int)std::ceil(ImGui::GetFrameHeight());
                    const int tw = std::min(fitGuestW, aw);
                    const int barH = (int)std::ceil(setupBarHeight(setup) + (driverHint ? ImGui::GetFrameHeightWithSpacing() + 12 : 0));
                    const int th = std::min(fitGuestH + menuH + barH, ah - ft);
                    glfwGetWindowPos(window, &wx, &wy);
                    glfwSetWindowSize(window, tw, th);
                    wx = std::max(ax, std::min(wx, ax + aw - tw));
                    wy = std::max(ay + ft, std::min(wy, ay + ah - th));
                    glfwSetWindowPos(window, wx, wy);
                    fitSettle = 3;
                    journal("window fitted to the guest: %dx%d", tw, th);
                } else if (fitSettle == 0) {
                    glfwGetWindowSize(window, &fitLastW, &fitLastH);
                }
            }
        } else if (fullscreen) {
            fitSettle = 3;
        }
        // Souris : suivre le mode de QEMU et, si demandé, rendre la tablette
        // courante chaque fois que la souris HID relative la reprend.
        const bool wantAbsolute = wantsAbsolutePointer(fullscreen, preferTablet);
        bool retryPointer = bridge->running() &&
            bridge->mouseIsAbsolute() != wantAbsolute && glfwGetTime() >= nextPointerRetry;
        if ((int)bridge->mouseIsAbsolute() != lastAbs || pointerPrefChanged || retryPointer) {
            if ((int)bridge->mouseIsAbsolute() != lastAbs || pointerPrefChanged)
                journal("guest mouse: %s", bridge->mouseIsAbsolute() ? "absolute (SetAbsPosition)" : "relative");
            lastAbs = bridge->mouseIsAbsolute();
            lastGx = lastGy = -1;
            if (bridge->running() && (lastAbs != (int)wantAbsolute || pointerPrefChanged)) {
                int m = bridge->selectMouse(wantAbsolute);
                nextPointerRetry = glfwGetTime() + 1.0;
                if (m > 0)
                    journal("QEMU mouse #%d made current (%s)", m,
                            wantAbsolute ? "absolute tablet" : "relative");
            }
            pointerPrefChanged = false;
        }

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGuiID dsId = ImGui::GetID("POMPPCDockSpace");

        if (fullscreen) {
            // ── Plein écran : seul l'invité, au ratio, bandes noires ──────
            ImGui::DockSpace(dsId, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
            ImGui::SetNextWindowPos(vp->Pos);
            ImGui::SetNextWindowSize(vp->Size);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
            ImGui::Begin("##pleinecran", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoScrollWithMouse |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
            if (setup.active() && !setup.wantsVm()) setupScreen();
            else guestView(false);
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);
        } else {
        // ── Menu bar → machine control over QMP ──────────────────────────
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Machine")) {
                if (ImGui::MenuItem("Shut Down", nullptr, false, bridge->running())) {
                    if (paused) { paused = false; bridge->setPaused(false); }
                    bridge->pressPowerKey();
                }
                if (ImGui::MenuItem("Restart", nullptr, false, bridge->running())) {
                    confirmRestart("Restart " + osNameOf(curLauncher) + "?", [&] {
                        if (bridge->reset()) {
                            chimePlayer.stop();
                            chimeTrigger.arm();
                        }
                    });
                }
                if (ImGui::MenuItem(paused ? "Resume" : "Pause")) {
                    paused = !paused;
                    bridge->setPaused(paused);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save Screenshot", nullptr, false, bridge->running())) {
                    std::error_code ec;
                    const std::string dir = root + "/Screenshots";
                    fs::create_directories(dir, ec);
                    char stamp[32];
                    const std::time_t now = std::time(nullptr);
                    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", std::localtime(&now));
                    const std::string file = dir + "/POMPPC " + stamp + ".png";
                    if (bridge->saveScreenshot(file)) {
                        showToast("Screenshot saved in the Screenshots folder.");
#ifdef __APPLE__
                        SetupFlow::runTool({"open", "-R", file});
#else
                        SetupFlow::openExternal(dir);
#endif
                    } else {
                        showToast("POMPPC couldn't save the screenshot.");
                    }
                }
                if (ImGui::MenuItem("Try Without Saving", nullptr, trySession, !setup.active())) {
                    const bool next = !trySession;
                    confirmRestart(next ? "Try without saving?" : "Go back to saving?", [&, next] {
                        trySession = next;
                        relaunch(curLauncher);
                    });
                }
                if (ImGui::MenuItem("Preferences...")) showPrefs = true;
                ImGui::Separator();
                ImGui::MenuItem("Keyboard to Guest", nullptr, &grabbed);
                if (ImGui::MenuItem("Absolute Mouse in Window", nullptr, &preferTablet))
                    pointerPrefChanged = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) glfwSetWindowShouldClose(window, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Games")) {
                ImGui::MenuItem(kWinLibrary, nullptr, &showLibrary);
                ImGui::EndMenu();
            }
            ImGui::BeginDisabled(setup.active());
            if (ImGui::BeginMenu("OS")) {
                bool onTiger = (curLauncher == tigerSh);
                // Sans disque, le lanceur échouerait sans rien montrer : pas de choix à offrir.
                const bool hasTiger = std::getenv("DISK") || fs::exists(root + "/disks/tiger.qcow2") ||
                                      (!fe.tigerDisks.boot.empty() && fs::exists(root + "/disks/" + fe.tigerDisks.boot));
                const bool hasOs9 = std::getenv("OS9_DISK") || fs::exists(root + "/disks/os9.qcow2") ||
                                    (!fe.os9Disks.boot.empty() && fs::exists(root + "/disks/" + fe.os9Disks.boot));
                if (ImGui::MenuItem("Mac OS X 10.4 (Tiger)", nullptr, onTiger, !onTiger && hasTiger))
                    confirmRestart("Switch to Tiger?", [&] { relaunch(tigerSh); });
                bool onOs9 = (curLauncher == os9Sh);
                if (ImGui::MenuItem("Mac OS 9.2.2", nullptr, onOs9, !onOs9 && hasOs9))
                    confirmRestart("Switch to Mac OS 9?", [&] { relaunch(os9Sh); });
                ImGui::Separator();
                if (ImGui::MenuItem("Set Up Another System...")) {
                    confirmRestart("Set up another system?", [&] {
                        setupCancelTo = curLauncher;
                        chimePlayer.stop();
                        bridge->stop();
                        bridge = std::make_unique<QemuBridge>();
                        guestW = guestH = 0;
                        stoppedSeen = false;
                        setup.begin();
                    });
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Devices")) {
                if (ImGui::BeginMenu("Startup Chime (Host)")) {
                    ImGui::TextDisabled("Expected profile: Power Mac G4 AGP / PowerMac3,1");
                    ImGui::TextDisabled("WAV/AIFF file. Source: see frontend/README.md");
                    if (ImGui::Checkbox("Enable Chime", &chime.enabled)) {
                        if (!chime.enabled) chimePlayer.stop();
                        saveChime();
                    }
                    int percent = (int)std::lround(chime.volume * 100);
                    if (ImGui::SliderInt("Volume", &percent, 0, 100, "%d %%")) {
                        chime.volume = percent / 100.0f;
                        chimePlayer.volume(chime.volume);
                        saveChime();
                    }
                    if (ImGui::InputText("File", chimeFile, sizeof(chimeFile), ImGuiInputTextFlags_EnterReturnsTrue)) {
                        chime.file = chimeFile;
                        saveChime();
                    }
                    ImGui::TextDisabled("Press Return to confirm the path. No restart needed.");
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (usbDisks.empty()) {
                    ImGui::TextDisabled("Drop files on this window to share them with Mac OS");
                } else {
                    for (size_t i = 0; i < usbDisks.size(); ++i) {
                        const std::string label = "Eject \"" + usbDisks[i].label + "\" and Show on Mac";
                        if (ImGui::MenuItem(label.c_str())) {
                            UsbDisk d = usbDisks[i];
                            d.showOnMac = true;
                            if (bridge->detachUsbDisk(d.id)) {
                                usbReleasing.push_back(d);
                                usbDisks.erase(usbDisks.begin() + (long)i);
                                usbReleaseTries = 0;
                            }
                            break;
                        }
                    }
                    ImGui::TextDisabled("Eject the disk in Mac OS first, or recent changes may be lost");
                }
                ImGui::Separator();
                ImGui::TextDisabled("(changing these restarts the machine)");
                if (ImGui::MenuItem("Sound (Screamer)", nullptr, sound)) {
                    confirmRestart(sound ? "Turn sound off?" : "Turn sound on?", [&] {
                        sound = !sound;
                        relaunch(curLauncher);
                    });
                }
#ifndef _WIN32   // pas de libusb au paquet (LaunchPlan.cpp)
                if (ImGui::MenuItem("USB Gamepad", nullptr, pad)) {
                    confirmRestart(pad ? "Turn the USB gamepad off?" : "Turn the USB gamepad on?", [&] {
                        pad = !pad;
                        relaunch(curLauncher);
                    });
                }
#endif
                ImGui::EndMenu();
            }
            ImGui::EndDisabled();
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Full Screen", "Alt+Return, F11"))
                    toggleFullscreen = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Fit to Window", nullptr, fitView)) fitView = true;
                const float zooms[] = {0.5f, 1.0f, 1.5f, 2.0f};
                const char* zlab[] = {"50 %", "100 %", "150 %", "200 %"};
                for (int i = 0; i < 4; ++i)
                    if (ImGui::MenuItem(zlab[i], nullptr, !fitView && zoom == zooms[i])) {
                        zoom = zooms[i];
                        fitView = false;
                    }
                ImGui::Separator();
                ImGui::MenuItem(kWinBilan, nullptr, &showBilan);
                ImGui::MenuItem(kWinJournal, nullptr, &showJournal);
                ImGui::MenuItem(kWinDisks, nullptr, &fe.showDisks);
                if (ImGui::MenuItem("Sharp Image", nullptr, fe.sharpImage)) fe.sharpImage = true;
                if (ImGui::MenuItem("Smooth Image", nullptr, !fe.sharpImage)) fe.sharpImage = false;
                ImGui::Separator();
                if (ImGui::MenuItem("Fit Window to Guest", nullptr, false, !fullscreen)) {
                    fitView = fe.autoFit = true;
                    showLibrary = showBilan = showJournal = fe.showDisks = false;
                    fitDone = false;
                    fitSince = 0;
                }
                if (ImGui::MenuItem("Default Layout")) resetLayout = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("POMPPC Guide")) openFile("https://github.com/habib256/pomppc#readme");
                if (ImGui::MenuItem("Show My Data Folder")) openFile(root);
                std::error_code logEc;
                if (ImGui::MenuItem("Show Log", nullptr, false, fs::exists(appLog, logEc))) openFile(appLog);
                if (ImGui::MenuItem("Report a Problem")) openFile("https://github.com/habib256/pomppc/issues/new");
                ImGui::Separator();
                if (ImGui::MenuItem("Check for Updates", nullptr, false, !updateTask.busy())) {
                    updateTask.start([](std::string* e) {
                        std::string json;
                        if (!SetupFlow::runTool({"curl", "-fsSL", "--max-time", "20", "-H", "Accept: application/vnd.github+json",
                                                 "https://api.github.com/repos/habib256/pomppc/releases/latest"}, &json)) {
                            *e = "POMPPC couldn't reach GitHub.";
                            return std::string();
                        }
                        const std::string tag = parseLatestTag(json);
                        if (tag.empty()) *e = "POMPPC couldn't read the latest version.";
                        return tag;
                    });
                }
                if (ImGui::MenuItem("About POMPPC")) showAbout = true;
                ImGui::EndMenu();
            }
            ImGui::TextDisabled("  %dx%d  %s  %s", guestW, guestH,
                                bridge->running() ? "running" : "stopped",
                                paused ? "(pause)" : "");
            if (trySession) { ImGui::SameLine(); ImGui::TextDisabled(" changes won't be saved"); }
            ImGui::EndMainMenuBar();
        }

        // ── DockSpace plein cadre (sous la barre de menus) ───────────────
        if (resetLayout || !ImGui::DockBuilderGetNode(dsId)) {
            buildDefaultLayout(dsId, vp);
            if (resetLayout) {
                showLibrary = showBilan = showJournal = fe.showDisks = false;
                fitView = fe.autoFit = true;
                fitDone = false;
                fitSince = 0;
            }
            resetLayout = false;
        }
        ImGui::DockSpaceOverViewport(dsId, vp);

        // ── Écran : la vue de l'invité, ancrée au centre ─────────────────
        {
            ImGuiWindowClass wc;
            wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;
            ImGui::SetNextWindowClass(&wc);
            // No padding, no border: a 1-px border insets the clip rect and
            // shaved the first and last guest rows off a height-limited view.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
            ImGuiWindowFlags f = ImGuiWindowFlags_NoCollapse;
            if (fitView) f |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
            else f |= ImGuiWindowFlags_HorizontalScrollbar;
            ImGui::Begin(kWinScreen, nullptr, f);
            if (setup.active() && !setup.wantsVm()) {
                setupScreen();
            } else {
                drawSetupBar(setup);
                if (driverHint) {
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(32, 58, 100, 255));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
                    ImGui::BeginChild("driverhint", ImVec2(0, ImGui::GetFrameHeightWithSpacing() + 12),
                                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
                    if (ImGui::SmallButton("Done")) {
                        markDriver();
                        driverMarkRead = appVersion ? appVersion : "";
                        driverHintHidden = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(driverHint);
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor();
                }
                guestView(true);
            }
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }

        // ── Ludothèque : insert a game CD into the guest at runtime ───────
        if (showLibrary) {
            if (ImGui::Begin(kWinLibrary, &showLibrary)) {
                ImGui::TextDisabled("Insert a game CD while running (gamecd drive)");
                if (currentCd.empty())
                    ImGui::TextDisabled("CD inserted: none");
                else
                    ImGui::Text("CD inserted: %s", prettyName(currentCd).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Eject")) {
                    bridge->ejectCd();
                    currentCd.clear();
                    journal("CD ejected");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Refresh")) cdImagesStale = true;
                if (cdImagesStale) { cdImages = listCdImages(root); cdImagesStale = false; }
                ImGui::Separator();
                if (cdImages.empty() && ImGui::SmallButton("Show Folder")) openFile(root + "/disks");
                if (cdImages.empty()) ImGui::TextWrapped("Put .iso, .cdr, .img or .toast files in %s/disks, then click Refresh. Or drop a disc on the window.", root.c_str());
                ImGui::BeginChild("games");
                for (const std::string& g : cdImages) {
                    bool sel = (g == currentCd);
                    if (ImGui::Selectable(prettyName(g).c_str(), sel,
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (bridge->changeCd(g)) {
                            currentCd = g;
                            journal("CD inserted: %s", prettyName(g).c_str());
                        } else {
                            showToast("Eject the current CD in Mac OS first, or drop this disc on the window.");
                        }
                    }
                }
                ImGui::EndChild();
            }
            ImGui::End();
        }

        // ── Disks : disque de démarrage, disques en plus, création ───────
        if (fe.showDisks) {
            // Pour vérifier le panneau par une capture : un onglet derrière un autre n'y paraît pas.
            static bool focusDisks = true;
            if (!std::getenv("POMPPC_FE_DISKS")) focusDisks = false;
            if (focusDisks) { ImGui::SetNextWindowFocus(); focusDisks = false; }
            if (ImGui::Begin(kWinDisks, &fe.showDisks)) {
                const bool tiger = curLauncher == tigerSh;
                DiskSelection& sel = tiger ? fe.tigerDisks : fe.os9Disks;
                static DiskSelection running;
                static std::string runningFor;
                static std::vector<DiskEntry> disks;
                static std::string runningTypes, diskError;
                static bool disksStale = true;
                static char newName[65] = "";
                static int newGb = 16, newType = 0;
                auto typesOf = [](const std::vector<DiskEntry>& l) {
                    std::string t;
                    for (const DiskEntry& d : l) if (d.ssd) t += d.name + ";";
                    return t;
                };
                if (disksStale) { disks = listDisks(root); disksStale = false; }
                if (runningFor != curLauncher) {
                    running = sel;
                    runningFor = curLauncher;
                    runningTypes = typesOf(disks);
                }
                const int maxExtras = maxExtraDisks(root, tiger);
                if (std::getenv(tiger ? "DISK" : "OS9_DISK")) {
                    ImGui::TextWrapped("The disk is set by the %s variable.", tiger ? "DISK" : "OS9_DISK");
                } else {
                    ImGui::TextWrapped("Choose the startup disk, and up to %d more disk%s to use at the same time.",
                                       maxExtras, maxExtras > 1 ? "s" : "");
                    if (ImGui::BeginTable("disks", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Start");
                        ImGui::TableSetupColumn("Use");
                        ImGui::TableSetupColumn("Disk", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Size");
                        ImGui::TableSetupColumn("Type");
                        ImGui::TableHeadersRow();
                        for (DiskEntry& d : disks) {
                            ImGui::PushID(d.name.c_str());
                            ImGui::TableNextRow();
                            const bool isBoot = sel.boot == d.name;
                            auto it = std::find(sel.extras.begin(), sel.extras.end(), d.name);
                            bool used = isBoot || it != sel.extras.end();
                            ImGui::TableNextColumn();
                            if (ImGui::RadioButton("##boot", isBoot)) {
                                if (it != sel.extras.end()) sel.extras.erase(it);
                                sel.boot = d.name;
                            }
                            ImGui::TableNextColumn();
                            ImGui::BeginDisabled(isBoot || (!used && (int)sel.extras.size() >= maxExtras));
                            if (ImGui::Checkbox("##use", &used)) {
                                if (used) sel.extras.push_back(d.name);
                                else sel.extras.erase(it);
                            }
                            ImGui::EndDisabled();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(d.name.substr(0, d.name.size() - 6).c_str());
                            ImGui::SetItemTooltip("%s", d.name.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(formatDiskSize(d.bytes).c_str());
                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(58);
                            int type = d.ssd ? 1 : 0;
                            if (ImGui::Combo("##type", &type, "HD\0SSD\0") && setDiskSsd(root, d.name, type == 1))
                                d.ssd = type == 1;
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    const bool changed = sel != running || typesOf(disks) != runningTypes;
                    ImGui::BeginDisabled(!changed);
                    if (ImGui::Button("Apply and Restart")) ImGui::OpenPopup("Restart the machine?");
                    ImGui::EndDisabled();
                    if (ImGui::BeginPopupModal("Restart the machine?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::TextUnformatted("The new disks need a restart. Shut the system down first:\nany unsaved work will be lost.");
                        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                        ImGui::SameLine();
                        if (ImGui::Button("Restart")) {
                            relaunch(curLauncher);
                            running = sel;
                            runningTypes = typesOf(disks);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::SeparatorText("New Disk");
                    ImGui::InputText("Name", newName, sizeof(newName));
                    ImGui::SetNextItemWidth(110);
                    ImGui::InputInt("GB", &newGb);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(70);
                    ImGui::Combo("##newtype", &newType, "HD\0SSD\0");
                    if (ImGui::Button("Create")) {
                        if (createDisk(root, newName, newGb, newType == 1, &diskError)) {
                            journal("disk created: %s (%d GB)", newName, newGb);
                            diskError.clear();
                            newName[0] = '\0';
                            disksStale = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Refresh##disks")) disksStale = true;
                    if (!diskError.empty()) ImGui::TextWrapped("%s", diskError.c_str());
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextWrapped("Format a new disk with Disk Utility in Tiger, or Drive Setup in Mac OS 9.");
                    ImGui::TextWrapped("SSD is only a label for the guest: speed doesn't change.");
                    ImGui::PopStyleColor();
                }

                ImGui::SeparatorText("Shared Folder");
                std::string& sharedPath = tiger ? fe.sharedTiger : fe.sharedOs9;
                const bool shareInUse = shareState != ShareState::Off && shareState != ShareState::Ejected;
                {
                    std::string chosen, e;
                    if (shareChoose.poll(&chosen, &e) && !chosen.empty()) {
                        const std::string problem = sharedFolderProblem(chosen, root);
                        if (!problem.empty()) {
                            showToast(problem);
                        } else {
                            sharedPath = chosen;
                            shareProblem.clear();
                            shareLast.clear();
                            shareBlocked = fs::exists(shareImage);
                            if (shareState == ShareState::Ejected) shareState = ShareState::Off;
                        }
                    }
                }
                // buildSharedDisk ne sait faire son image HFS+ que par hdiutil.
#ifdef __APPLE__
                const bool shareHost = true;
#else
                const bool shareHost = false;
#endif
                if (!shareHost) {
                    ImGui::TextWrapped("Sharing a folder is only available on macOS for now.");
                } else if (sharedPath.empty()) {
                    ImGui::TextWrapped("Choose a folder on your Mac: it appears as a disk in Mac OS.");
                } else {
                    ImGui::TextWrapped("%s", sharedPath.c_str());
                    ImGui::SetItemTooltip("%s", sharedPath.c_str());
                }
                ImGui::BeginDisabled(!shareHost || shareInUse || shareChoose.busy());
                if (ImGui::Button(sharedPath.empty() ? "Choose Folder..." : "Change Folder...")) {
                    shareChoose.start([](std::string*) {
                        // POMPPC_SHARED_CHOOSE : le dossier d'une épreuve scriptée, sans fenêtre de macOS.
                        if (const char* forced = std::getenv("POMPPC_SHARED_CHOOSE")) return std::string(forced);
                        std::string out;
                        if (!SetupFlow::runTool({"osascript", "-e",
                                                 "return POSIX path of (choose folder with prompt \"Choose the folder to share\")"}, &out))
                            return std::string();
                        out.erase(out.find_last_not_of("\r\n") + 1);
                        if (out.size() > 1 && out.back() == '/') out.pop_back();
                        return out;
                    });
                }
                if (!sharedPath.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Stop Sharing")) {
                        sharedPath.clear();
                        shareLast.clear();
                        if (shareState == ShareState::Ejected) shareState = ShareState::Off;
                    }
                }
                ImGui::EndDisabled();
                if (!sharedPath.empty() && ImGui::Button("Show on Mac")) openFile(sharedPath);
                if (!shareProblem.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", shareProblem.c_str());
                    if (shareBlocked && ImGui::Button("Try Again")) {
                        shareBlocked = false;
                        shareProblem.clear();
                        if (shareState == ShareState::Ejected) shareState = ShareState::Off;
                    }
                }
                const std::string volume = sharedVolumeName(sharedPath);
                switch (shareState) {
                    case ShareState::Building:
                    case ShareState::Attaching:
                        ImGui::TextUnformatted("Preparing the disk...");
                        break;
                    case ShareState::Attached:
                        ImGui::TextWrapped("\"%s\" is on the desktop of Mac OS. Eject it there to bring your changes back to the Mac.",
                                           sharedVolumeName(shareFolder).c_str());
                        break;
                    case ShareState::Releasing:
                    case ShareState::Syncing:
                        ImGui::TextUnformatted("Bringing your changes back to the Mac...");
                        break;
                    case ShareState::Ejected:
                        if (!shareLast.empty()) ImGui::TextWrapped("Back on your Mac. %s", shareLast.c_str());
                        if (!sharedPath.empty() && !shareBlocked && ImGui::Button("Connect Again")) {
                            shareLast.clear();
                            shareState = ShareState::Off;
                        }
                        break;
                    case ShareState::Off:
                        if (trySession && !sharedPath.empty())
                            ImGui::TextDisabled("Not connected in Try Without Saving.");
                        else if (!bridge->running() && !sharedPath.empty())
                            ImGui::TextWrapped("\"%s\" connects when the system starts.", volume.c_str());
                        break;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("The disk is a copy, not a live view. What Mac OS changes comes back when you eject the disk "
                                   "or shut down; what the Mac changes appears the next time it connects. "
                                   "Deleted files go to the Trash.");
                ImGui::PopStyleColor();
            }
            ImGui::End();
        }

        // ── Bilan : géométrie et état des entrées ────────────────────────
        if (showBilan) {
            if (ImGui::Begin(kWinBilan, &showBilan)) {
                int fbw = 0, fbh = 0, ww = 0, wh = 0;
                glfwGetFramebufferSize(window, &fbw, &fbh);
                glfwGetWindowSize(window, &ww, &wh);
                ImGui::Text("QEMU: %s", bridge->qemuVersion().c_str());
                ImGui::Text("Guest: %dx%d  (%s)", guestW, guestH,
                            bridge->running() ? "running" : "stopped");
                ImGui::Text("View: %s", fitView ? "fit to ratio" : "fixed zoom");
                if (drawSize.x > 0)
                    ImGui::Text("Drawn rectangle: %.0fx%.0f @ %.0f,%.0f  (×%.3f)",
                                drawSize.x, drawSize.y, drawOrigin.x, drawOrigin.y,
                                guestW ? drawSize.x / guestW : 0.0f);
                ImGui::Text("Host window: %dx%d pt, %dx%d px", ww, wh, fbw, fbh);
                ImGui::Text("Mouse: %s, last guest position %d,%d",
                            bridge->mouseIsAbsolute() ? "absolute" : "relative", lastGx, lastGy);
                ImGui::Text("Keyboard to guest: %s", grabbed ? "yes" : "no");
                if (!currentCd.empty())
                    ImGui::Text("CD: %s", prettyName(currentCd).c_str());
            }
            ImGui::End();
        }

        // ── Journal ──────────────────────────────────────────────────────
        if (showJournal) {
            if (ImGui::Begin(kWinJournal, &showJournal)) {
                for (const std::string& l : gJournal) ImGui::TextUnformatted(l.c_str());
                if (gJournalScroll) { ImGui::SetScrollHereY(1.0f); gJournalScroll = false; }
            }
            ImGui::End();
        }
        }   // !fullscreen

        // ── Confirmations : hors du bloc précédent, pour exister aussi en plein écran ─────
        if (askQuit) { ImGui::OpenPopup("Quit POMPPC?"); askQuit = false; }
        if (askRestart) { ImGui::OpenPopup("###restart"); askRestart = false; }
        {
            const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("Quit POMPPC?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                const std::string os = osNameOf(curLauncher);
                ImGui::Text("%s is still running.", os.c_str());
                ImGui::Text("Shut it down first, or anything not saved in it will be lost.");
                ImGui::Spacing();
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
                ImGui::SameLine();
                if (ImGui::Button("Force Quit")) {
                    forceQuit = true;
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Shut Down")) {
                    // L'invité pose sa propre question ; POMPPC se ferme quand il s'est éteint.
                    if (paused) { paused = false; bridge->setPaused(false); }
                    bridge->pressPowerKey();
                    quitWhenStopped = true;
                    journal("shutdown requested: POMPPC quits when %s has stopped", os.c_str());
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            const std::string title = restartTitle + "###restart";
            if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%s will stop at once: anything not saved in it will be lost.",
                            osNameOf(curLauncher).c_str());
                ImGui::Spacing();
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
                ImGui::SameLine();
                if (ImGui::Button("Continue")) {
                    if (restartAction) restartAction();
                    restartAction = nullptr;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // ── Preferences ──────────────────────────────────────────────────
        if (showPrefs) {
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_Appearing);
            if (ImGui::Begin("Preferences", &showPrefs, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
                const bool tiger = curLauncher == tigerSh;
                static int draftMem = -1, draftCores = -1;
                static bool draftTiger = true, draftSound = true, draftPad = false;
                static bool lastSound = sound, lastPad = pad;   // changés aussi par le menu Devices
                const int savedMem = tiger ? fe.memoryTiger : fe.memoryOs9;
                if (draftMem < 0 || draftTiger != tiger || lastSound != sound || lastPad != pad ||
                    ImGui::IsWindowAppearing()) {
                    draftMem = savedMem;
                    draftCores = fe.coresTiger;
                    draftTiger = tiger;
                    draftSound = lastSound = sound;
                    draftPad = lastPad = pad;
                }
                const int defaultMem = tiger ? 1024 : 512;
                static const int memTiger[] = {512, 768, 1024};
                static const int memOs9[] = {256, 512, 768};
                const int* mem = tiger ? memTiger : memOs9;
                const int shownMem = draftMem > 0 ? draftMem : defaultMem;
                ImGui::Text("%s", tiger ? "Mac OS X 10.4 Tiger" : "Mac OS 9.2.2");
                ImGui::Spacing();
                if (ImGui::BeginCombo("Memory", (std::to_string(shownMem) + " MB").c_str())) {
                    for (int i = 0; i < 3; ++i) {
                        if (ImGui::Selectable((std::to_string(mem[i]) + " MB").c_str(), mem[i] == shownMem))
                            draftMem = mem[i];
                    }
                    ImGui::EndCombo();
                }
                if (tiger && draftSound && effectiveMemoryMb(shownMem, true) < shownMem) {
                    ImGui::TextWrapped("With sound on, Tiger gets 768 MB: the emulated sound chip needs less than 1 GB. "
                                       "Turn sound off to use all of it.");
                }
                if (tiger) {
                    const int shownCores = draftCores > 0 ? draftCores : 2;
                    if (ImGui::BeginCombo("Processors", std::to_string(shownCores).c_str())) {
                        if (ImGui::Selectable("1", shownCores == 1)) draftCores = 1;
                        if (ImGui::Selectable("2", shownCores == 2)) draftCores = 2;
                        ImGui::EndCombo();
                    }
                    ImGui::TextDisabled("Two processors are faster. One is the most stable.");
                } else {
                    ImGui::TextDisabled("Mac OS 9 always uses one processor.");
                }
                ImGui::Checkbox("Sound", &draftSound);
                ImGui::SameLine();
#ifndef _WIN32
                ImGui::Checkbox("USB gamepad", &draftPad);
#endif
                ImGui::Spacing();
                const bool changed = draftMem != savedMem || (tiger && draftCores != fe.coresTiger) ||
                                     draftSound != sound || draftPad != pad;
                ImGui::BeginDisabled(!changed);
                if (ImGui::Button("Apply and Restart")) {
                    confirmRestart("Apply the new settings?", [&, tiger] {
                        (tiger ? fe.memoryTiger : fe.memoryOs9) = draftMem;
                        if (tiger) fe.coresTiger = draftCores;
                        sound = draftSound;
                        pad = draftPad;
                        relaunch(curLauncher);
                    });
                }
                ImGui::EndDisabled();
                ImGui::Separator();
                // [A] Mesuré (M4, 03/11) : RES=800x600x32 laisse Tiger à 1024x768, et OS 9 reste à 640x480
                //     sous RES=1024x768x32. La définition du bureau appartient à l'invité : pas de réglage ici.
                if (guestW > 0)
                    ImGui::TextWrapped("Screen: %d x %d. To change it, use %s in Mac OS.", guestW, guestH,
                                       tiger ? "System Preferences > Displays" : "Control Panels > Monitors");
                ImGui::TextDisabled("Image smoothing: View menu.");
            }
            ImGui::End();
        }
        if (showAbout) { ImGui::OpenPopup("About POMPPC"); showAbout = false; }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("About POMPPC", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const char* mine = std::getenv("POMPPC_VERSION");
            ImGui::Text("POMPPC %s", mine ? mine : "(development build)");
            ImGui::TextDisabled("QEMU %s", bridge->qemuVersion().c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted("Free software, GPL-3.0-or-later.");
            if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!updateFound.empty()) ImGui::OpenPopup("Update Available");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Update Available", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("POMPPC %s is available.", updateFound.c_str());
            if (ImGui::Button("Later")) { updateFound.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Open the Download Page")) {
                openFile("https://github.com/habib256/pomppc/releases/latest");
                updateFound.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        // ── Message discret ──────────────────────────────────────────────
        if (glfwGetTime() < toastUntil) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const ImVec2 size = ImGui::CalcTextSize(toastText.c_str());
            const ImVec2 view = ImGui::GetMainViewport()->Size;
            const ImVec2 at((view.x - size.x) * 0.5f, 36.0f);
            dl->AddRectFilled(ImVec2(at.x - 12, at.y - 7), ImVec2(at.x + size.x + 12, at.y + size.y + 7),
                              IM_COL32(20, 24, 32, 235), 6.0f);
            dl->AddText(at, IM_COL32_WHITE, toastText.c_str());
        }

        // Clipboard bridge: offer host clipboard to the guest (throttled poll),
        // and paste any text the guest copied onto the host clipboard.
        static int clipTick = 0;
        if (++clipTick >= 30) {
            clipTick = 0;
#ifdef __APPLE__
            std::string hc = macClipboardText();
            if (!hc.empty()) bridge->publishLocalClipboard(hc);
#else
            gClipboardPoll = true;
            const char* hc = glfwGetClipboardString(window);
            gClipboardPoll = false;
            if (hc && hc[0]) bridge->publishLocalClipboard(hc);
#endif
        }
        std::string gclip;
        if (bridge->takeGuestClipboard(gclip))
            glfwSetClipboardString(window, gclip.c_str());

        // Keyboard → guest (skip while an ImGui text field wants input).
        // Shortcut keys handled above are swallowed; a release is only sent
        // for a key the guest saw pressed.
        // Focus requis : sur Cmd+Tab, ImGui efface l'état clavier
        // (ClearInputKeys) sans que IsKeyReleased devienne jamais vrai —
        // Commande restait enfoncée dans l'invité. La perte de focus passe
        // donc par la branche « relâcher tout » ci-dessous.
        // Une épreuve POMPPC_FE_SCRIPT en cours injecte ses propres
        // événements : elle vaut le focus (frontend lancé par un agent ou un
        // terminal, sans activation de l'application).
        const bool focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0
                             || probeIdx < probe.size();
        bool keysLive = grabbed && focused && !io.WantTextInput && bridge->running() && !uiModal;
        if (keysLive) {
            for (int i = 0; i < kKeyCount; ++i) {
                if (!swallow[i] && ImGui::IsKeyPressed(kKeys[i].k, false)) {
                    bridge->keyPress(kKeys[i].num);
                    keyHeld[i] = true;
                }
                if (keyHeld[i] && !ImGui::IsKeyDown(kKeys[i].k)) {
                    bridge->keyRelease(kKeys[i].num);
                    keyHeld[i] = false;
                }
            }
        } else if (keysWereLive) {
            // On vient de perdre le clavier : relâcher tout ce qui est tenu.
            for (int i = 0; i < kKeyCount; ++i) {
                if (keyHeld[i]) { bridge->keyRelease(kKeys[i].num); keyHeld[i] = false; }
            }
        }
        keysWereLive = keysLive;
        if (!focused) {
            for (int b = 0; b < 3; ++b) {
                if (!mouseHeld[b]) continue;
                bridge->mouseButton((b == 1) ? 2 : (b == 2 ? 1 : 0), false);
                mouseHeld[b] = false;
            }
        }

        ImGui::Render();
        int fbw, fbh;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        if (fullscreen) glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        else glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (!probeGlShot.empty()) {
            int pw = 0, ph = 0;
            glfwGetFramebufferSize(window, &pw, &ph);
            std::vector<unsigned char> px((size_t)pw * ph * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, pw, ph, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            if (FILE* out = std::fopen(probeGlShot.c_str(), "wb")) {
                std::fprintf(out, "P6\n%d %d\n255\n", pw, ph);
                for (int y = ph - 1; y >= 0; --y) std::fwrite(&px[(size_t)y * pw * 3], 3, pw, out);
                std::fclose(out);
            }
            probeGlShot.clear();
        }
        glfwSwapBuffers(window);
        if (!probeShot.empty()) {
            std::string cmd = "screencapture -x '" + probeShot + "'";
            if (std::system(cmd.c_str()) != 0) journal("capture failed: %s", probeShot.c_str());
            probeShot.clear();
        }

        // ── Bascule plein écran ↔ fenêtre (hors frame) ───────────────────
        if (toggleFullscreen) {
            toggleFullscreen = false;
            GLFWmonitor* mon = fullscreen ? nullptr : monitorOf(window);
            const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
            if (!fullscreen && !vm) {
                // Aucun écran recensé : voir l'ajustement de la fenêtre.
                journal("full screen ignored: no display available");
            } else if (!fullscreen) {
                glfwGetWindowPos(window, &winX, &winY);
                glfwGetWindowSize(window, &winW, &winH);
                glfwSetWindowMonitor(window, mon, 0, 0, vm->width, vm->height,
                                     vm->refreshRate);
                fullscreen = true;
                showToast("Ctrl+Cmd+F, F11 or Alt+Return leaves full screen.");
                journal("full screen: %s %dx%d @%d Hz", glfwGetMonitorName(mon),
                        vm->width, vm->height, vm->refreshRate);
            } else {
                glfwSetWindowMonitor(window, nullptr, winX, winY, winW, winH, 0);
                fullscreen = false;
                journal("window: %dx%d @ %d,%d", winW, winH, winX, winY);
            }
            lastGx = lastGy = -1;
            relAccX = relAccY = 0;
            pointerPrefChanged = true;
            nextPointerRetry = 0;
        }
    }

    // ── Réglages d'ImGuiDock, écrits à l'arrêt ────────────────────────────
    // En plein écran, la géométrie fenêtrée retenue avant la bascule est
    // celle à rétablir ; sinon, celle de la fenêtre au moment de quitter.
    if (!fullscreen) {
        glfwGetWindowPos(window, &winX, &winY);
        glfwGetWindowSize(window, &winW, &winH);
    }
    fe.os = (curLauncher == tigerSh) ? "tiger" : "os9";
    fe.sound = sound;
    fe.pad = pad;
    fe.grabbed = grabbed;
    fe.preferTablet = preferTablet;
    fe.fitView = fitView;
    fe.zoom = zoom;
    fe.showLibrary = showLibrary;
    fe.showBilan = showBilan;
    fe.showJournal = showJournal;
    fe.fullscreen = fullscreen;
    fe.winX = winX; fe.winY = winY; fe.winW = winW; fe.winH = winH;
    if (feKeep && !fe.save(feSettingsPath))
        std::fprintf(stderr, "[pomppc] could not write %s\n", feSettingsPath.c_str());

    const bool cleanExit = !bridge->running() && bridge->exitedCleanly();
    bridge->stop();
    shareFinishNow(cleanExit);
    glDeleteTextures(1, &tex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
