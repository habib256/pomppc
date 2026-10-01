/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
#include "StartupChime.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unistd.h>

int main() {
    ChimeTrigger t;
    assert(!t.frame(true, 640, 480, true, .5f));
    t.arm();
    assert(!t.frame(false, 640, 480, true, .5f));
    assert(!t.frame(true, 0, 480, true, .5f));
    assert(t.frame(true, 640, 480, true, .5f));
    assert(!t.frame(true, 1024, 768, true, .5f));
    t.arm();
    assert(!t.frame(true, 640, 480, false, .5f));
    assert(!t.frame(true, 640, 480, true, .5f)); // unmute does not replay
    t.arm();
    assert(!t.frame(true, 640, 480, true, 0));
    t.arm();
    assert(!t.frame(true, 640, 480, true, std::numeric_limits<float>::quiet_NaN()));
    t.arm();
    assert(t.frame(true, 1024, 768, true, 1));
    char dir[] = "/tmp/pomppc-chime-test.XXXXXX";
    assert(mkdtemp(dir));
    std::string path = std::string(dir) + "/prefs";
    ChimeSettings s;
    s.file = "/tmp/son avec espaces et \"guillemets\".aiff";
    s.enabled = false;
    s.volume = .25f;
    assert(s.save(path));
    ChimeSettings loaded;
    loaded.load(path);
    assert(loaded.file == s.file && !loaded.enabled && loaded.volume == .25f);
    { std::ofstream out(path); out << "1 42 \"test.wav\""; }
    loaded.load(path);
    assert(loaded.volume == 1 && loaded.enabled);
    { std::ofstream out(path); out << "broken"; }
    loaded.load(path);
    assert(loaded.file == "test.wav"); // malformed config preserves defaults
    assert(!s.save(std::string(dir) + "/missing/prefs"));
    std::filesystem::remove(path);
    std::filesystem::remove(dir);
}
