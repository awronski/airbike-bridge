#pragma once

#include <stddef.h>
#include <stdint.h>

// Observed Assault AirBike Elite protocol, not a generic FTMS decoder.
// Source: https://github.com/roelofkl/ha-assault-airbike/blob/main/docs/PROTOCOL.md
namespace airbike {

// The Elite's distance counter can stall near 20 km.
constexpr uint32_t CONSOLE_DISTANCE_FALLBACK_MIN_M = 19995;
constexpr uint32_t DATA_FRESH_MS = 4000;

struct Metrics {
    uint16_t elapsed_s = 0;
    uint16_t remaining_s = 0;
    uint16_t power_w = 0;
    uint16_t calories_kcal = 0;
    uint32_t distance_m = 0;
    float speed_kph = 0;
    float cadence_rpm = 0;
    bool has_remaining = false;
};

inline uint16_t little16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline bool decode(const uint8_t* bytes, size_t length, Metrics& result) {
    if (!bytes || length < 16 || (bytes[0] & 0x7f) != 0x73 || bytes[1] != 0x58) {
        return false;
    }
    const bool programme = (bytes[0] & 0x80) != 0;
    const size_t offset = programme ? 2 : 0;
    if (length < 16 + offset) {
        return false;
    }
    Metrics next;
    next.elapsed_s = little16(bytes + 3);
    next.has_remaining = programme;
    next.remaining_s = programme ? little16(bytes + 5) : 0;
    next.speed_kph = little16(bytes + 5 + offset) / 100.0f;
    next.cadence_rpm = little16(bytes + 7 + offset) / 10.0f;
    next.distance_m = static_cast<uint32_t>(bytes[9 + offset]) |
                      (static_cast<uint32_t>(bytes[10 + offset]) << 8) |
                      (static_cast<uint32_t>(bytes[11 + offset]) << 16);
    next.calories_kcal = little16(bytes + 12 + offset);
    next.power_w = little16(bytes + 14 + offset);
    result = next;
    return true;
}

inline bool isFresh(bool connected, bool has_data, uint32_t now_ms, uint32_t packet_ms) {
    return connected && has_data && static_cast<uint32_t>(now_ms - packet_ms) < DATA_FRESH_MS;
}

}  // namespace airbike
