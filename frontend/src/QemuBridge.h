/* GPL3 - Copyleft VERHILLE Arnaud */
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
//
// Display handlers do not copy pixels. ScanoutMap is a synchronous QEMU call
// on the same thread as the audio timer: a full-frame memcpy before the reply
// stalls that timer. Handlers only publish the shared mapping or a ref on the
// inline bytes, then return. The render thread copies once per displayed
// frame, coalescing every UpdateMap received in between.
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
    //
    // Only the rows that actually changed are copied: `out` is expected to
    // persist across calls (it mirrors the guest surface). `y0`/`y1` bound the
    // touched rows [y0, y1); `resized` says the surface geometry changed, so
    // the caller must re-upload everything. The shared-memory damage since the
    // previous latch is folded into one copy here, on the render thread.
    bool latchFrame(std::vector<uint32_t>& out, int& w, int& h,
                    int& y0, int& y1, bool& resized);
    // Convenience wrapper (whole-surface semantics) for the headless probe.
    bool latchFrame(std::vector<uint32_t>& out, int& w, int& h);

    // Reap QEMU if it died on its own; returns false once it is gone. Call it
    // from the UI loop: nothing else clears running_, so a crashed QEMU used to
    // leave the frontend showing a frozen frame labelled "en marche" forever.
    bool checkAlive();

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
    // Raw QMP line, waits for reply (the reply line is copied to *reply).
    bool qmpCommand(const std::string& json, std::string* reply = nullptr);
    // Make an absolute (tablet) or relative pointer QEMU's current mouse.
    // mac99 has both a virtio/USB tablet and a USB HID mouse; the HID mouse
    // grabs "current" as soon as the guest polls it, which turned OS 9's
    // absolute tablet back into relative motion. Returns the mouse index
    // selected, 0 if the current one already matches, -1 if none/failed.
    int selectMouse(bool absolute);

    // Called from the D-Bus thread by the C trampolines. Public so the
    // generated-code callbacks can reach them; not part of the app API.
    void ingestScanout(uint32_t w, uint32_t h, uint32_t stride,
                       uint32_t pixmanFormat, const uint8_t* data, size_t len);
    void ingestUpdate(int x, int y, int w, int h, uint32_t stride,
                      uint32_t pixmanFormat, const uint8_t* data, size_t len);
    // Unix.Map fast path (shared memory): fd is mmap'd. UpdateMap only records
    // the damaged rows; latchFrame copies them from the mapping.
    void mapScanout(int fd, uint32_t offset, uint32_t w, uint32_t h,
                    uint32_t stride, uint32_t pixmanFormat);
    void mapUpdate(int x, int y, int w, int h);
    // Inline Scanout/Update: keep the GVariant alive and let latchFrame copy.
    // `variant` is a GVariant* (glib stays out of this header).
    void queueInline(bool scanout, int x, int y, int w, int h,
                     uint32_t width, uint32_t height, uint32_t stride,
                     void* variant);
    // Mouse.IsAbsolute changed (e.g. OS 9's virtio-tablet driver came up
    // after OpenBIOS): switch SetAbsPosition ↔ RelMotion accordingly.
    void noteMouseMode(bool absolute);

    struct Impl;   // GLib/GDBus state kept out of this header
    Impl* impl();
    void runGlibThread();   // D-Bus thread entry (via C trampoline)

private:
    bool setupGlib();       // fil D-Bus : connexions, proxys, listener
    void teardownGlib();    // fil D-Bus : libère ce que setupGlib a créé
    void stopQemu();        // SIGTERM, attente, SIGKILL si besoin
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

    // fb_ is touched only on the render thread. fbMtx_ guards the damage
    // flags, the inline queue and the shared-mapping pointer: handlers publish
    // them and return, without waiting out the pixel copy.
    std::mutex fbMtx_;
    std::vector<uint32_t> fb_;      // BGRA, fbW_*fbH_
    int fbW_ = 0, fbH_ = 0;
    bool fbDirty_ = false;
    bool fbResized_ = false;        // geometry changed since the last latch
    int fbY0_ = 0, fbY1_ = 0;       // dirty row span [fbY0_, fbY1_)
    bool mapDirty_ = false;         // damaged rows still only in the mapping
    bool mapResize_ = false;
    int mapY0_ = 0, mapY1_ = 0;
    void markRows(int y0, int y1);  // render thread only
    void ingestScanoutLocked(uint32_t w, uint32_t h, uint32_t stride,
                             const uint8_t* data, size_t len);
    void ingestUpdateLocked(int x, int y, int w, int h, uint32_t stride,
                            const uint8_t* data, size_t len);
    void copyMapRows(const uint8_t* data, uint32_t mapW, uint32_t mapH,
                     uint32_t stride, int y0, int y1);

    long qemuPid_ = -1;
    bool qemuReaped_ = false;       // waitpid() already collected it
    uint32_t qmpSeq_ = 0;           // correlates QMP replies with commands
};
