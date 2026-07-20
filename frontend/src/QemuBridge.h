// QemuBridge — drives a QEMU process as POMPPC's "CPU".
//
// Unlike the other Pommes (pom68k, POM1/2, POMIIGS) whose core is an in-process
// library, POMPPC's engine is QEMU. QemuBridge launches it with
// `-display dbus,p2p=on` and speaks the org.qemu.Display1 protocol over a
// peer-to-peer GDBus connection (bootstrapped through QMP add_client, exactly
// like qemu/tests/qtest/dbus-display-test.c):
//
//   * receives display updates (Listener.Scanout / .Update) → a BGRA framebuffer
//     the GLFW/ImGui thread uploads as a texture (same as pom68k's latchFrame);
//   * sends input (Keyboard.Press/Release, Mouse.SetAbsPosition/Press/Release).
//
// All GDBus I/O runs on a private GLib thread with its own GMainContext; the
// render thread only touches the framebuffer under a mutex and posts input via
// g_main_context_invoke.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

typedef struct _GMainLoop GMainLoop;
typedef struct _GMainContext GMainContext;
typedef struct _GThread GThread;

class QemuBridge {
public:
    struct Config {
        std::string launcher;               // run script, e.g. ".../run_tiger.sh"
        std::vector<std::string> env;       // extra "KEY=VAL" for the launcher
        std::string runtimeDir;             // where the QMP socket lives
    };

    QemuBridge();
    ~QemuBridge();

    // Launch QEMU and establish the D-Bus display connection. On failure sets
    // *err and returns false. Non-blocking: display arrives asynchronously.
    bool start(const Config& cfg, std::string* err);
    void stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }

    // Copy the newest framebuffer into `out` (BGRA, tightly packed WxH).
    // Returns true when a new frame has arrived since the last latch.
    bool latchFrame(std::vector<uint32_t>& out, int& w, int& h);

    // Input — thread-safe, marshalled onto the D-Bus thread.
    void keyPress(uint32_t qkeycode);
    void keyRelease(uint32_t qkeycode);
    void mouseAbs(int x, int y);          // absolute pointer, guest pixels (clamped)
    void mouseRel(int dx, int dy);        // relative motion (fallback)
    void mouseButton(int button, bool down);                  // 0=L 1=M 2=R
    void mouseWheel(int dir);                                 // 3=up 4=down
    bool mouseIsAbsolute() const;

    // ── Clipboard bridge (host ↔ guest, text) ──────────────────────────
    // Call each frame with the host clipboard; a change is offered to the
    // guest (Grab on QEMU). Poll takeGuestClipboard for text the guest copied.
    void publishLocalClipboard(const std::string& text);
    bool takeGuestClipboard(std::string& out);
    bool requestClipboard(std::string& out);   // pull QEMU's current clipboard
    // D-Bus-thread helpers used by the clipboard trampolines / control code.
    void clipResetSerial();
    void clipStoreGuest(const std::string& text);
    std::string clipLocalText();
    void doClipGrab();

    // ── Machine control over QMP (thread-safe; runs on the caller thread) ──
    bool reset();                         // system_reset
    bool setPaused(bool paused);          // stop / cont
    bool changeCd(const std::string& path, const std::string& id = "gamecd");
    bool ejectCd(const std::string& id = "gamecd");
    bool qmpCommand(const std::string& json);   // raw QMP line, waits for reply

    // Called from the D-Bus thread by the C trampolines. Public so the
    // generated-code callbacks can reach them; not part of the app API.
    void ingestScanout(uint32_t w, uint32_t h, uint32_t stride,
                       uint32_t pixmanFormat, const uint8_t* data, size_t len);
    void ingestUpdate(int x, int y, int w, int h, uint32_t stride,
                      uint32_t pixmanFormat, const uint8_t* data, size_t len);
    // Unix.Map fast path (shared memory): fd is mmap'd; UpdateMap copies from it.
    void mapScanout(int fd, uint32_t offset, uint32_t w, uint32_t h,
                    uint32_t stride, uint32_t pixmanFormat);
    void mapUpdate(int x, int y, int w, int h);

    struct Impl;   // GLib/GDBus state kept out of this header
    Impl* impl();
    void runGlibThread();   // D-Bus thread entry (via C trampoline)

private:
    Config cfg_;
    Impl* impl_ = nullptr;
    GThread* thread_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> mouseAbs_{true};

    // Persistent QMP channel (opened in start(), used by the control methods).
    int qmpFd_ = -1;
    std::mutex qmpMtx_;
    std::string qmpBuf_;

    // Clipboard state (text only, selection 0).
    std::mutex clipMtx_;
    std::string localClip_;        // current host clipboard (offered to guest)
    std::string guestClip_;        // guest → host, pending
    std::string lastLocalSent_;    // dedupe host-clipboard polling
    bool guestClipReady_ = false;
    uint32_t clipSerial_ = 0;      // our grab serial (D-Bus thread only)

    // Framebuffer shared with the render thread.
    std::mutex fbMtx_;
    std::vector<uint32_t> fb_;      // BGRA, fbW_*fbH_
    int fbW_ = 0, fbH_ = 0;
    bool fbDirty_ = false;

    long qemuPid_ = -1;
};
