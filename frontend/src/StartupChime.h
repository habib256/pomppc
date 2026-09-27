#pragma once
#include <cmath>
#include <memory>
#include <string>

// Host-side sound, deliberately independent of the guest's Screamer device.
class ChimePlayer {
public:
    ChimePlayer();
    ~ChimePlayer();
    bool play(const std::string& file, float volume, std::string& error);
    void stop();
    void volume(float value);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A resize, resume, or subsequent frame must never replay the boot sound.
struct ChimeTrigger {
    bool pending = false;
    void arm() { pending = true; }
    bool frame(bool alive, int width, int height, bool enabled, float volume) {
        if (!pending || !alive || width <= 0 || height <= 0) return false;
        pending = false; // muted/missing/invalid sound also consumes this boot
        return enabled && std::isfinite(volume) && volume > 0;
    }
};

struct ChimeSettings {
    bool enabled = true;
    float volume = 0.5f;
    std::string file;
    void load(const std::string& path);
    bool save(const std::string& path) const;
};
