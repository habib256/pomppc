// Headless verification of the QEMU↔D-Bus pipeline (no GLFW/ImGui/X needed).
// Launches the guest under -snapshot, waits for the first live Scanout, prints
// its dimensions and a PPM screenshot, then tears down.
#include "QemuBridge.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    // Default launcher is resolved from the *binary* (build/ → repo root), like
    // main.cpp does, so the probe works from any working directory. An explicit
    // argument is still taken as given (relative to the cwd).
    fs::path exeDir = fs::weakly_canonical(fs::path(argv[0])).parent_path();
    std::string launcher =
        (argc > 1) ? argv[1] : (exeDir / ".." / ".." / "run_tiger.sh").string();
    launcher = fs::weakly_canonical(launcher).string();
    std::string root = fs::path(launcher).parent_path().string();
    int timeoutSec = (argc > 2) ? atoi(argv[2]) : 40;

    QemuBridge bridge;
    QemuBridge::Config cfg;
    cfg.launcher = launcher;
    cfg.runtimeDir = root + "/.run";
    fs::create_directories(cfg.runtimeDir);
    cfg.env = {"NOPAD=1", "SNAPSHOT=1"};
    if (!getenv("PROBE_SOUND")) cfg.env.push_back("NOSOUND=1");  // PROBE_SOUND=1 keeps sound (Screamer OpenBIOS)
    if (const char* r = getenv("RES")) cfg.env.push_back(std::string("RES=") + r);

    std::string err;
    if (!bridge.start(cfg, &err)) {
        std::fprintf(stderr, "start failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "probe: launched %s, waiting for scanout…\n", launcher.c_str());

    // Keep latching for the whole window so we capture real boot progress
    // (resolution switches, grey screen, Tiger boot) — not just the first
    // OpenBIOS clear.
    std::vector<uint32_t> fb, latest;
    int w = 0, h = 0, lw = 0, lh = 0, pw = 0, ph = 0;
    bool got = false, didReset = false;
    int frames = 0;
    // RESET_TEST=1 explicitly exercises QMP reset. Keeping it opt-in is
    // essential for boot/display probes: an automatic half-time reset made a
    // healthy 1024x768 OS 9 boot look like a regression back to OpenBIOS
    // 640x480.
    bool resetTest = getenv("RESET_TEST") != nullptr;
    int resetAt = timeoutSec * 10 / 2;
    for (int i = 0; i < timeoutSec * 10; ++i) {
        if (bridge.latchFrame(fb, w, h)) {
            if (!got)
                std::fprintf(stderr, "probe: FIRST FRAME %dx%d at t=%.1fs\n", w, h,
                             i / 10.0);
            if (w != pw || h != ph) {
                std::fprintf(stderr, "probe: resolution → %dx%d at t=%.1fs\n", w, h,
                             i / 10.0);
                pw = w; ph = h;
            }
            latest = fb; lw = w; lh = h;
            got = true;
            ++frames;
        }
        // Clipboard: offer host text to the guest; QEMU should accept the Grab
        // (the async completion logs "grab accepted by QEMU"). Full text
        // transfer additionally needs a guest clipboard agent (absent on PPC).
        if (i == resetAt - 20 && got)
            bridge.publishLocalClipboard("POMPPC clipboard test ✔");
        // CDTEST=<image> : verify hot-insert into the gamecd drive over QMP.
        if (const char* cd = getenv("CDTEST"))
            if (i == resetAt - 30 && got)
                std::fprintf(stderr, "probe: changeCd(%s) → %s\n", cd,
                             bridge.changeCd(cd) ? "ok" : "FAIL");
        if (resetTest && i == resetAt && got) {
            std::fprintf(stderr, "probe: QMP system_reset → %s\n",
                         bridge.reset() ? "ok" : "FAIL");
            didReset = true;
        }
        // 4 s after reset, snapshot the screen to prove it actually rebooted.
        if (didReset && i == resetAt + 40 && got) {
            FILE* f = fopen("probe_reset.ppm", "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", lw, lh);
                for (int k = 0; k < lw * lh; ++k) {
                    uint32_t p = latest[k];
                    unsigned char rgb[3] = {(unsigned char)((p >> 16) & 0xff),
                                            (unsigned char)((p >> 8) & 0xff),
                                            (unsigned char)(p & 0xff)};
                    fwrite(rgb, 1, 3, f);
                }
                fclose(f);
                std::fprintf(stderr, "probe: wrote probe_reset.ppm (%dx%d)\n", lw, lh);
            }
        }
        usleep(100 * 1000);
    }
    (void)didReset;
    fb = latest; w = lw; h = lh;
    if (got)
        std::fprintf(stderr, "probe: %d frames total, last %dx%d\n", frames, w, h);

    if (got) {
        FILE* f = fopen("probe.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; ++i) {
                uint32_t p = fb[i];  // BGRA little-endian
                unsigned char rgb[3] = {(unsigned char)((p >> 16) & 0xff),
                                        (unsigned char)((p >> 8) & 0xff),
                                        (unsigned char)(p & 0xff)};
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
            std::fprintf(stderr, "probe: wrote probe.ppm\n");
        }
    } else {
        std::fprintf(stderr, "probe: NO FRAME within %ds\n", timeoutSec);
    }

    bridge.stop();
    return got ? 0 : 2;
}
