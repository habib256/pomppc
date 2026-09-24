// POMPPC — Dear ImGui + GLFW/OpenGL3 shell around a QEMU PowerPC Mac.
// The QEMU display arrives over D-Bus (QemuBridge); we upload it as a texture
// and render it inside an ImGui window, exactly like pom68k does with its
// in-process framebuffer. Input is forwarded back over D-Bus; the menu bar
// drives the machine over QMP.
//
// F1 : the guest screen is an ImGui window docked in the middle of a
// full-frame DockSpace (menus, Ludothèque, Bilan, Journal around it; layout
// built once with DockBuilder, then saved in .run/imgui.ini). The guest image
// is always drawn at the largest rectangle keeping the scanout's aspect ratio
// (black bands, never stretched), unless a fixed zoom is picked in Vue.
// Vue ▸ Plein écran (Ctrl+Cmd+F, F11; Échap or the same shortcut to leave)
// puts the host window on its monitor at native size with only the guest
// shown. Pointer input is mapped on the rectangle actually drawn.
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder*
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "QemuBridge.h"

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

static void glfwErr(int e, const char* d) { std::fprintf(stderr, "GLFW %d: %s\n", e, d); }

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

// ── Épreuve scriptée (POMPPC_FE_SCRIPT) ──────────────────────────────────
// Joue une suite d'entrées *dans la file d'ImGui* (même chemin que la vraie
// souris et le vrai clavier : raccourcis, rectangle dessiné, pont D-Bus) et
// prend des captures au bon moment, sans les droits d'Accessibilité qu'un
// clic synthétique macOS exige. Étapes séparées par ';' :
//   wait <s> | move <x> <y> | click <x> <y> (points fenêtre) | fs (Ctrl+Cmd+F)
//   | f11 | esc | type <texte: a-z 0-9 espace .> | enter
//   | shot <fichier.png> (screencapture -x) | quit
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
#endif

// Window names shared by the DockBuilder layout and the Begin() calls.
static const char* kWinScreen  = "Écran";
static const char* kWinLibrary = "Ludothèque";
static const char* kWinBilan   = "Bilan";
static const char* kWinJournal = "Journal";

// Default layout, built only when imgui.ini has no node for the dockspace
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
    ImGui::DockBuilderDockWindow(kWinJournal, bottom);
    ImGui::DockBuilderDockWindow(kWinBilan, bottomRight);
    ImGui::DockBuilderFinish(dsId);
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
    // Docking layout persists next to the QMP socket, whatever the cwd.
    static std::string iniPath = runtimeDir + "/imgui.ini";

    std::string curLauncher = defLauncher;   // boots Mac OS 9 by default
    bool sound = true;    // on by default (PulseAudio Screamer); RAM capped ≤768
    bool pad = true;      // usb-host passthrough; the run script self-guards
    auto bridge = std::make_unique<QemuBridge>();
    std::string err;
    if (!bridge->start(makeConfig(curLauncher, runtimeDir, sound, pad), &err)) {
        std::fprintf(stderr, "QemuBridge start failed: %s\n", err.c_str());
        return 1;
    }
    journal("QEMU lancé : %s", curLauncher.c_str());

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
    GLFWwindow* window =
        glfwCreateWindow(1280, 900, "POMPPC — PowerPC Macintosh (QEMU)", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
#ifdef __APPLE__
    journal("Ctrl+Cmd+F rendu au frontend (%d entrée(s) « Enter Full Screen » du menu Cocoa)",
            disableNativeFullScreen(window));
#endif
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    {
        ImGuiIO& io0 = ImGui::GetIO();
        io0.ConfigWindowsMoveFromTitleBarOnly = true;
        io0.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io0.IniFilename = iniPath.c_str();
    }
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    GLuint tex = 0;
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
    bool grabbed = true;    // keyboard → guest (toggle in the Machine menu)
    // Touches actuellement enfoncées côté invité. Sans ce suivi, décocher
    // « Clavier → invité » (ou ouvrir un champ de saisie ImGui) pendant qu'une
    // touche est tenue n'envoyait jamais le relâchement : Shift/Ctrl restaient
    // collés dans l'invité.
    constexpr int kKeyCount = (int)(sizeof(kKeys) / sizeof(kKeys[0]));
    bool keyHeld[kKeyCount] = {};
    bool keysWereLive = false;
    bool paused = false;
    // Vue : « ajuster » (plus grand rectangle au ratio de l'invité) par
    // défaut ; un zoom fixe 50-200 % reste possible en fenêtre.
    bool fitView = true;
    float zoom = 1.0f;
    bool showLibrary = true;      // ludothèque window
    bool showBilan = true;
    bool showJournal = true;
    bool resetLayout = false;
    std::string currentCd;        // game CD currently inserted in gamecd

    // Plein écran hôte : la fenêtre GLFW passe sur son moniteur à la
    // résolution native ; on retient la géométrie fenêtrée pour y revenir.
    bool fullscreen = false;
    bool toggleFullscreen = false;   // appliqué après le swap (hors frame ImGui)
    int winX = 0, winY = 0, winW = 1280, winH = 900;

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
    bool preferTablet = true;
    bool pointerPrefChanged = false;

    auto relaunch = [&](const std::string& launcher) {
        bridge->stop();
        bridge = std::make_unique<QemuBridge>();
        std::string e;
        if (!bridge->start(makeConfig(launcher, runtimeDir, sound, pad), &e))
            journal("relance échouée : %s", e.c_str());
        else {
            curLauncher = launcher;
            journal("QEMU relancé : %s", launcher.c_str());
        }
        guestW = guestH = 0;
        texW = texH = 0;                 // la texture GPU sera réallouée
        fb.clear();                      // force un latch complet
        for (bool& k : keyHeld) k = false;
        lastAbs = -1;
        paused = false;
        currentCd.clear();
        cdImagesStale = true;
    };

    // Draw the guest texture in the current window at the largest rectangle
    // with the guest's aspect ratio (or the fixed zoom), centred, and forward
    // the pointer relative to that rectangle only. The window background is
    // black, so the rest of the content region is the letterbox.
    auto guestView = [&](bool allowZoom) {
        ImGuiIO& io = ImGui::GetIO();
        if (guestW <= 0) {
            ImGui::TextDisabled("En attente de l'affichage QEMU…");
            drawSize = ImVec2(0, 0);
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
        ImGui::Image((ImTextureID)(intptr_t)tex, sz);
        drawOrigin = origin;
        drawSize = sz;
        // IsItemHovered = inside the drawn (and visible, clipped) rectangle,
        // with no ImGui window or menu on top: the bands never send events.
        bool hovered = ImGui::IsItemHovered();
        float sx = (float)guestW / sz.x, sy = (float)guestH / sz.y;
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
            } else if (io.MouseDelta.x || io.MouseDelta.y) {
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
                if (b == 0)
                    journal("clic hôte (%.0f,%.0f) -> invité (%d,%d) %s [rect %.0fx%.0f @ %.0f,%.0f]",
                            io.MousePos.x, io.MousePos.y,
                            (int)std::floor((io.MousePos.x - origin.x) * sx),
                            (int)std::floor((io.MousePos.y - origin.y) * sy),
                            bridge->mouseIsAbsolute() ? "abs" : "rel", sz.x, sz.y,
                            origin.x, origin.y);
            }
            if (ImGui::IsMouseReleased(b)) bridge->mouseButton(qb, false);
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
    std::string probeShot;
    auto probeKeys = [](std::initializer_list<ImGuiKey> ks, bool down) {
        ImGuiIO& pio = ImGui::GetIO();
        for (ImGuiKey k : ks) {
            if (k == ImGuiKey_LeftCtrl) pio.AddKeyEvent(ImGuiMod_Ctrl, down);
            if (k == ImGuiKey_LeftSuper) pio.AddKeyEvent(ImGuiMod_Super, down);
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
        } else if (st.op == "fs" || st.op == "f11" || st.op == "esc" || st.op == "enter") {
            bool down = (probePhase == 0);
            if (st.op == "fs") probeKeys({ImGuiKey_LeftCtrl, ImGuiKey_LeftSuper, ImGuiKey_F}, down);
            else if (st.op == "f11") probeKeys({ImGuiKey_F11}, down);
            else if (st.op == "esc") probeKeys({ImGuiKey_Escape}, down);
            else probeKeys({ImGuiKey_Enter}, down);
            done = !down;
        } else if (st.op == "type") {
            size_t ci = (size_t)probePhase / 2;
            if (ci < st.arg.size()) {
                ImGuiKey k = probeKey(st.arg[ci]);
                if (k != ImGuiKey_None) probeKeys({k}, probePhase % 2 == 0);
                done = false;
            }
        } else if (st.op == "shot") {
            probeShot = st.arg;
        } else if (st.op == "quit") {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        if (done) {
            journal("épreuve : %s %s", st.op.c_str(), st.arg.c_str());
            ++probeIdx;
            probePhase = 0;
        } else {
            ++probePhase;
        }
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        probeTick();
        ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();

        // ── Raccourcis plein écran (avalés : jamais transmis à l'invité) ──
        // Ctrl+Cmd+F comme macOS, F11 ; Échap ne sert qu'à sortir du plein
        // écran (en fenêtre, Échap va à l'invité comme avant).
        bool swallow[kKeyCount] = {};
        auto swallowKey = [&](ImGuiKey k) {
            for (int i = 0; i < kKeyCount; ++i) if (kKeys[i].k == k) swallow[i] = true;
        };
        if (ImGui::IsKeyPressed(ImGuiKey_F11, false) ||
            (io.KeyCtrl && io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_F, false))) {
            toggleFullscreen = true;
            swallowKey(ImGuiKey_F11);
            swallowKey(ImGuiKey_F);
        }
        if (fullscreen && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            toggleFullscreen = true;
            swallowKey(ImGuiKey_Escape);
        }

        int w = 0, h = 0, dy0 = 0, dy1 = 0;
        bool resized = false;
        if (bridge->latchFrame(fb, w, h, dy0, dy1, resized)) {
            if (w != guestW || h != guestH) journal("scanout %dx%d", w, h);
            guestW = w; guestH = h;
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
        bridge->checkAlive();
        // Souris : suivre le mode de QEMU et, si demandé, rendre la tablette
        // courante chaque fois que la souris HID relative la reprend.
        if ((int)bridge->mouseIsAbsolute() != lastAbs || pointerPrefChanged) {
            lastAbs = bridge->mouseIsAbsolute();
            journal("souris invité : %s", lastAbs ? "absolue (SetAbsPosition)" : "relative");
            lastGx = lastGy = -1;
            if (bridge->running() && (lastAbs != (int)preferTablet || pointerPrefChanged)) {
                int m = bridge->selectMouse(preferTablet);
                if (m > 0)
                    journal("souris QEMU n°%d rendue courante (%s)", m,
                            preferTablet ? "tablette absolue" : "relative");
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
            guestView(false);
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);
        } else {
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
                if (ImGui::MenuItem("Souris absolue (tablette)", nullptr, &preferTablet))
                    pointerPrefChanged = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Quitter")) glfwSetWindowShouldClose(window, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Jeux")) {
                ImGui::MenuItem(kWinLibrary, nullptr, &showLibrary);
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
                ImGui::TextDisabled("(bascule = redémarrage immédiat de la VM)");
                if (ImGui::MenuItem("Son (PulseAudio)", nullptr, &sound))
                    relaunch(curLauncher);
                if (ImGui::MenuItem("Manette USB", nullptr, &pad))
                    relaunch(curLauncher);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Vue")) {
                if (ImGui::MenuItem("Plein écran", "Ctrl+Cmd+F, F11"))
                    toggleFullscreen = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Ajuster à la fenêtre", nullptr, fitView)) fitView = true;
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
                if (ImGui::MenuItem("Disposition par défaut")) resetLayout = true;
                ImGui::EndMenu();
            }
            ImGui::TextDisabled("  %dx%d  %s  %s", guestW, guestH,
                                bridge->running() ? "en marche" : "arrêté",
                                paused ? "(pause)" : "");
            ImGui::EndMainMenuBar();
        }

        // ── DockSpace plein cadre (sous la barre de menus) ───────────────
        if (resetLayout || !ImGui::DockBuilderGetNode(dsId)) {
            buildDefaultLayout(dsId, vp);
            showLibrary = showBilan = showJournal = true;
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
            guestView(true);
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }

        // ── Ludothèque : insert a game CD into the guest at runtime ───────
        if (showLibrary) {
            if (ImGui::Begin(kWinLibrary, &showLibrary)) {
                ImGui::TextDisabled("Insère un CD de jeu à chaud (lecteur gamecd)");
                if (currentCd.empty())
                    ImGui::TextDisabled("CD inséré : aucun");
                else
                    ImGui::Text("CD inséré : %s", prettyName(currentCd).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Éjecter")) {
                    bridge->ejectCd();
                    currentCd.clear();
                    journal("CD éjecté");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Rafraîchir")) cdImagesStale = true;
                if (cdImagesStale) { cdImages = listCdImages(root); cdImagesStale = false; }
                ImGui::Separator();
                ImGui::BeginChild("games");
                for (const std::string& g : cdImages) {
                    bool sel = (g == currentCd);
                    if (ImGui::Selectable(prettyName(g).c_str(), sel,
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (bridge->changeCd(g)) {
                            currentCd = g;
                            journal("CD inséré : %s", prettyName(g).c_str());
                        }
                    }
                }
                ImGui::EndChild();
            }
            ImGui::End();
        }

        // ── Bilan : géométrie et état des entrées ────────────────────────
        if (showBilan) {
            if (ImGui::Begin(kWinBilan, &showBilan)) {
                int fbw = 0, fbh = 0, ww = 0, wh = 0;
                glfwGetFramebufferSize(window, &fbw, &fbh);
                glfwGetWindowSize(window, &ww, &wh);
                ImGui::Text("Invité : %dx%d  (%s)", guestW, guestH,
                            bridge->running() ? "en marche" : "arrêté");
                ImGui::Text("Vue : %s", fitView ? "ajustée au ratio" : "zoom fixe");
                if (drawSize.x > 0)
                    ImGui::Text("Rectangle dessiné : %.0fx%.0f @ %.0f,%.0f  (×%.3f)",
                                drawSize.x, drawSize.y, drawOrigin.x, drawOrigin.y,
                                guestW ? drawSize.x / guestW : 0.0f);
                ImGui::Text("Fenêtre hôte : %dx%d pt, %dx%d px", ww, wh, fbw, fbh);
                ImGui::Text("Souris : %s, dernière position invité %d,%d",
                            bridge->mouseIsAbsolute() ? "absolue" : "relative", lastGx, lastGy);
                ImGui::Text("Clavier vers l'invité : %s", grabbed ? "oui" : "non");
                if (!currentCd.empty())
                    ImGui::Text("CD : %s", prettyName(currentCd).c_str());
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
        // Shortcut keys handled above are swallowed; a release is only sent
        // for a key the guest saw pressed.
        bool keysLive = grabbed && !io.WantTextInput && bridge->running();
        if (keysLive) {
            for (int i = 0; i < kKeyCount; ++i) {
                if (!swallow[i] && ImGui::IsKeyPressed(kKeys[i].k, false)) {
                    bridge->keyPress(kKeys[i].num);
                    keyHeld[i] = true;
                }
                if (keyHeld[i] && ImGui::IsKeyReleased(kKeys[i].k)) {
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

        ImGui::Render();
        int fbw, fbh;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        if (fullscreen) glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        else glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        if (!probeShot.empty()) {
            std::string cmd = "screencapture -x '" + probeShot + "'";
            if (std::system(cmd.c_str()) != 0) journal("capture échouée : %s", probeShot.c_str());
            probeShot.clear();
        }

        // ── Bascule plein écran ↔ fenêtre (hors frame) ───────────────────
        if (toggleFullscreen) {
            toggleFullscreen = false;
            if (!fullscreen) {
                glfwGetWindowPos(window, &winX, &winY);
                glfwGetWindowSize(window, &winW, &winH);
                GLFWmonitor* mon = monitorOf(window);
                const GLFWvidmode* vm = glfwGetVideoMode(mon);
                glfwSetWindowMonitor(window, mon, 0, 0, vm->width, vm->height,
                                     vm->refreshRate);
                fullscreen = true;
                journal("plein écran : %s %dx%d @%d Hz", glfwGetMonitorName(mon),
                        vm->width, vm->height, vm->refreshRate);
            } else {
                glfwSetWindowMonitor(window, nullptr, winX, winY, winW, winH, 0);
                fullscreen = false;
                journal("fenêtre : %dx%d @ %d,%d", winW, winH, winX, winY);
            }
            lastGx = lastGy = -1;
            relAccX = relAccY = 0;
        }
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
