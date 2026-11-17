#pragma once
#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <cmath>
#include "airbike_protocol.h"

namespace rides {
constexpr size_t LIMIT = 20;
constexpr uint32_t IDLE_MS = 10 * 60 * 1000;
enum EndReason : uint8_t { DISCONNECTED = 1, INACTIVE = 2, CONSOLE_RESET = 3 };
struct Record {
    uint32_t id = 0, durationS = 0, distanceM = 0;
    uint16_t maxPower = 0, maxSpeed = 0; // speed in 0.01 km/h
    uint64_t powerSum = 0, speedSum = 0; // sample value * console seconds
    uint32_t sampleSeconds = 0;
    uint8_t reason = 0;
    bool partial = false;
};

struct Tracker {
    Record history[LIMIT];
    size_t count = 0;
    uint32_t nextID = 1;
    Record current;
    bool active = false, dirty = false;
    bool baseline = false;
    uint32_t lastSequence = 0, lastProgressMs = 0;
    airbike::Metrics last, base;

    static uint16_t speed(const airbike::Metrics& m) {
        return static_cast<uint16_t>(std::lround(m.speed_kph * 100.0f));
    }
    void archive(EndReason why) {
        if (!active) return;
        current.id = nextID++;
        if (!nextID) nextID = 1;
        current.reason = why;
        for (size_t i = std::min(count, LIMIT - 1); i > 0; --i) history[i] = history[i - 1];
        history[0] = current;
        if (count < LIMIT) ++count;
        current = Record{};
        active = false;
        dirty = true;
    }
    void discard() {
        // Keep the latest console sample as the baseline for the next segment.
        // No history write, console reset or Garmin control is performed.
        active = false;
        current = Record{};
        base = last;
    }
    void observe(bool connected, bool hasData, uint32_t sequence,
                 const airbike::Metrics& m, uint32_t nowMs) {
        if (!connected) {
            archive(DISCONNECTED);
            baseline = false;
            return;
        }
        if (active && static_cast<uint32_t>(nowMs - lastProgressMs) >= IDLE_MS) {
            archive(INACTIVE);
            baseline = false;
        }
        if (!hasData || sequence == lastSequence) return;
        lastSequence = sequence;
        if (!baseline) {
            last = base = m;
            baseline = true;
            lastProgressMs = nowMs;
            return;
        }
        if (m.elapsed_s < last.elapsed_s || m.distance_m < last.distance_m || m.calories_kcal < last.calories_kcal) {
            archive(CONSOLE_RESET);
            last = base = m;
            lastProgressMs = nowMs;
            return;
        }
        const bool progress = m.elapsed_s > last.elapsed_s || m.distance_m > last.distance_m || m.calories_kcal > last.calories_kcal;
        if (progress) {
            if (!active) {
                current = Record{};
                base = last;
                current.partial = base.elapsed_s != 0 || base.distance_m != 0;
                current.maxPower = last.power_w;
                current.maxSpeed = speed(last);
                active = true;
            }
            const uint32_t dt = m.elapsed_s - last.elapsed_s;
            current.durationS = m.elapsed_s - base.elapsed_s;
            current.distanceM = m.distance_m - base.distance_m;
            if (dt > 0 && dt <= 5) {
                // Time-weighted, using the previous observed sample. Missing
                // intervals are excluded from averages instead of fabricated.
                current.powerSum += static_cast<uint64_t>(last.power_w) * dt;
                current.speedSum += static_cast<uint64_t>(speed(last)) * dt;
                current.sampleSeconds += dt;
            } else if (dt > 5) current.partial = true;
            lastProgressMs = nowMs;
        }
        if (active) {
            current.maxPower = std::max(current.maxPower, m.power_w);
            current.maxSpeed = std::max(current.maxSpeed, speed(m));
        }
        last = m;
    }
};

// Explicit, versioned wire format, independent of native struct padding.
constexpr size_t RECORD_BYTES = 40;
constexpr size_t STORAGE_BYTES = 12 + LIMIT * RECORD_BYTES + 4;
inline void writeLE(uint8_t* p, uint64_t n, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) { p[i] = static_cast<uint8_t>(n); n >>= 8; }
}
inline uint64_t readLE(const uint8_t* p, size_t bytes) {
    uint64_t n = 0;
    for (size_t i = 0; i < bytes; ++i) n |= static_cast<uint64_t>(p[i]) << (8 * i);
    return n;
}
inline uint32_t checksum(const uint8_t* p, size_t bytes) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * 16777619u;
    return hash;
}
inline void encode(const Tracker& t, uint8_t (&out)[STORAGE_BYTES]) {
    std::fill(out, out + STORAGE_BYTES, uint8_t(0));
    out[0] = 'A'; out[1] = 'G'; out[2] = 'R'; out[3] = 'H';
    out[4] = 1; out[5] = static_cast<uint8_t>(t.count);
    writeLE(out + 8, t.nextID, 4);
    for (size_t i = 0; i < t.count; ++i) {
        const auto& r = t.history[i];
        auto* p = out + 12 + i * RECORD_BYTES;
        writeLE(p, r.id, 4); writeLE(p + 4, r.durationS, 4); writeLE(p + 8, r.distanceM, 4);
        writeLE(p + 12, r.maxPower, 2); writeLE(p + 14, r.maxSpeed, 2);
        writeLE(p + 16, r.powerSum, 8); writeLE(p + 24, r.speedSum, 8);
        writeLE(p + 32, r.sampleSeconds, 4); p[36] = r.reason; p[37] = r.partial ? 1 : 0;
    }
    writeLE(out + STORAGE_BYTES - 4, checksum(out, STORAGE_BYTES - 4), 4);
}
inline bool decode(Tracker& t, const uint8_t* in, size_t size) {
    if (!in || size != STORAGE_BYTES || in[0] != 'A' || in[1] != 'G' || in[2] != 'R' || in[3] != 'H' ||
        in[4] != 1 || in[5] > LIMIT || readLE(in + 8, 4) == 0 ||
        readLE(in + STORAGE_BYTES - 4, 4) != checksum(in, STORAGE_BYTES - 4)) return false;
    // Validate all records before changing the in-memory history.
    for (size_t i = 0; i < in[5]; ++i) {
        const auto* p = in + 12 + i * RECORD_BYTES;
        if (!readLE(p, 4) || p[36] < DISCONNECTED || p[36] > CONSOLE_RESET || p[37] > 1) return false;
    }
    t.count = in[5]; t.nextID = static_cast<uint32_t>(readLE(in + 8, 4));
    for (size_t i = 0; i < t.count; ++i) {
        auto& r = t.history[i];
        const auto* p = in + 12 + i * RECORD_BYTES;
        r.id = readLE(p, 4); r.durationS = readLE(p + 4, 4); r.distanceM = readLE(p + 8, 4);
        r.maxPower = readLE(p + 12, 2); r.maxSpeed = readLE(p + 14, 2);
        r.powerSum = readLE(p + 16, 8); r.speedSum = readLE(p + 24, 8);
        r.sampleSeconds = readLE(p + 32, 4); r.reason = p[36]; r.partial = p[37];
    }
    return true;
}
} // namespace rides
