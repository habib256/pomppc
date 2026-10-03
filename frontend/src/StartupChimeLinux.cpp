/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
// Linux host: the chime through PulseAudio (pa_simple, PipeWire's pulse
// server included), on a thread of its own. WAV and AIFF, integer PCM
// 8 to 32 bits or WAV float, decoded here to 16 bits; the volume is applied
// per 20 ms packet, so it can change while the sound plays (as NSSound).
#include "StartupChime.h"
#include <pulse/error.h>
#include <pulse/simple.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace {

struct Pcm {
    unsigned rate = 0, channels = 0;
    std::vector<int16_t> samples;   // interleaved
};

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t* p) { return p[0] | p[1] << 8; }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return p[0] << 8 | p[1]; }

// One sample of `bytes` bytes at p, integer PCM, to 16 bits.
int16_t pcm16(const uint8_t* p, unsigned bytes, bool big, bool unsigned8) {
    if (bytes == 1) return (int16_t)(((unsigned8 ? p[0] - 128 : (int8_t)p[0])) * 256);
    // most significant byte first in `big`, last otherwise: keep the top 16 bits
    const uint8_t hi = big ? p[0] : p[bytes - 1];
    const uint8_t lo = big ? p[1] : p[bytes - 2];
    return (int16_t)(hi << 8 | lo);
}

bool decode(const std::vector<uint8_t>& d, Pcm& out, std::string& error) {
    const size_t n = d.size();
    if (n >= 12 && !memcmp(&d[0], "RIFF", 4) && !memcmp(&d[8], "WAVE", 4)) {
        unsigned fmt = 0, bits = 0;
        for (size_t o = 12; o + 8 <= n;) {
            const uint32_t sz = le32(&d[o + 4]);
            const size_t body = o + 8;
            if (sz > n - body) break;
            if (!memcmp(&d[o], "fmt ", 4) && sz >= 16) {
                fmt = le16(&d[body]);
                out.channels = le16(&d[body + 2]);
                out.rate = le32(&d[body + 4]);
                bits = le16(&d[body + 14]);
                if (fmt == 0xfffe && sz >= 26) fmt = le16(&d[body + 24]);  // extensible
            } else if (!memcmp(&d[o], "data", 4) && out.channels) {
                const unsigned bytes = bits / 8;
                if (fmt == 3 && bits == 32) {
                    for (size_t i = 0; i + 4 <= sz; i += 4) {
                        float f;
                        memcpy(&f, &d[body + i], 4);
                        out.samples.push_back((int16_t)std::lround(std::clamp(f, -1.0f, 1.0f) * 32767));
                    }
                } else if (fmt == 1 && bytes >= 1 && bytes <= 4 && bits % 8 == 0) {
                    for (size_t i = 0; i + bytes <= sz; i += bytes)
                        out.samples.push_back(pcm16(&d[body + i], bytes, false, true));
                } else {
                    error = "WAV non PCM (format " + std::to_string(fmt) + ", " +
                            std::to_string(bits) + " bits)";
                    return false;
                }
                return out.rate && !out.samples.empty();
            }
            o = body + sz + (sz & 1);
        }
        error = "WAV sans bloc fmt/data lisible";
        return false;
    }
    if (n >= 12 && !memcmp(&d[0], "FORM", 4) && !memcmp(&d[8], "AIFF", 4)) {
        unsigned bits = 0;
        for (size_t o = 12; o + 8 <= n;) {
            const uint32_t sz = be32(&d[o + 4]);
            const size_t body = o + 8;
            if (sz > n - body) break;
            if (!memcmp(&d[o], "COMM", 4) && sz >= 18) {
                out.channels = be16(&d[body]);
                bits = be16(&d[body + 6]);
                // 80-bit IEEE extended sample rate
                const int exp = (be16(&d[body + 8]) & 0x7fff) - 16383 - 63;
                uint64_t mant = (uint64_t)be32(&d[body + 10]) << 32 | be32(&d[body + 14]);
                out.rate = (unsigned)std::ldexp((double)mant, exp);
            } else if (!memcmp(&d[o], "SSND", 4) && out.channels && sz >= 8) {
                const unsigned bytes = (bits + 7) / 8;
                if (bytes < 1 || bytes > 4) break;
                const size_t start = body + 8 + be32(&d[body]);
                for (size_t i = start; i + bytes <= body + sz; i += bytes)
                    out.samples.push_back(pcm16(&d[i], bytes, true, false));
                return out.rate && !out.samples.empty();
            }
            o = body + sz + (sz & 1);
        }
        error = "AIFF sans bloc COMM/SSND lisible";
        return false;
    }
    error = "format attendu : WAV ou AIFF";
    return false;
}

}  // namespace

struct ChimePlayer::Impl {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<float> level{0.5f};
};

ChimePlayer::ChimePlayer() : impl_(new Impl) {}
ChimePlayer::~ChimePlayer() { stop(); }

void ChimePlayer::stop() {
    impl_->stop = true;
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->stop = false;
}

void ChimePlayer::volume(float value) { impl_->level = std::clamp(value, 0.0f, 1.0f); }

bool ChimePlayer::play(const std::string& file, float level, std::string& error) {
    stop();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "fichier absent ou illisible : " + file;
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Pcm pcm;
    if (!decode(data, pcm, error)) {
        error += " : " + file;
        return false;
    }
    pa_sample_spec spec = {PA_SAMPLE_S16LE, pcm.rate, (uint8_t)pcm.channels};
    if (!pa_sample_spec_valid(&spec)) {
        error = "échantillonnage non pris en charge : " + file;
        return false;
    }
    int err = 0;
    pa_simple* s = pa_simple_new(nullptr, "POMPPC", PA_STREAM_PLAYBACK, nullptr, "carillon",
                                 &spec, nullptr, nullptr, &err);
    if (!s) {
        error = std::string("sortie audio indisponible : ") + pa_strerror(err);
        return false;
    }
    volume(level);
    impl_->thread = std::thread([this, s, pcm = std::move(pcm)] {
        const size_t packet = std::max<size_t>(pcm.channels, pcm.rate / 50 * pcm.channels);
        std::vector<int16_t> buf;
        int e = 0;
        for (size_t i = 0; i < pcm.samples.size() && !impl_->stop; i += packet) {
            const size_t k = std::min(packet, pcm.samples.size() - i);
            const float v = impl_->level;
            buf.assign(pcm.samples.begin() + i, pcm.samples.begin() + i + k);
            for (auto& x : buf) x = (int16_t)std::lround(x * v);
            if (pa_simple_write(s, buf.data(), k * sizeof(int16_t), &e) < 0) break;
        }
        if (impl_->stop) pa_simple_flush(s, &e);
        else pa_simple_drain(s, &e);
        pa_simple_free(s);
    });
    return true;
}
