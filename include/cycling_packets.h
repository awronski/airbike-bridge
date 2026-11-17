#pragma once
#include <cmath>
#include <stdint.h>
#include "airbike_protocol.h"

namespace cycling {
// Configure the Garmin speed sensor to this same manual wheel circumference.
constexpr uint16_t WHEEL_MM = 2000;
inline void put16(uint8_t* p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
inline void put32(uint8_t* p, uint32_t n) { put16(p, n); put16(p + 2, n >> 16); }

struct Revolutions {
    uint32_t count = 0;
    double fraction = 0;
    uint16_t eventTime = 0;
    void advance(uint64_t startUs, uint64_t dtUs, double perSecond) {
        if (!(perSecond > 0) || !std::isfinite(perSecond)) return;
        const double total = fraction + perSecond * (dtUs / 1000000.0);
        const uint32_t crossed = static_cast<uint32_t>(std::floor(total + 1e-9));
        if (crossed) {
            // Time of the last full revolution, not the notification time.
            const uint64_t eventUs = startUs + static_cast<uint64_t>(std::llround(
                (crossed - fraction) * 1000000.0 / perSecond));
            eventTime = static_cast<uint16_t>((eventUs * 1024ULL) / 1000000ULL);
            count += crossed;
        }
        fraction = total > crossed ? total - crossed : 0;
    }
};

struct Encoder {
    Revolutions wheel, crank;
    bool tracking = false;
    uint64_t lastUs = 0;
    void update(uint64_t nowUs, bool fresh, const airbike::Metrics& m) {
        if (!fresh) { tracking = false; return; }
        // Rebase after input loss or a blocking BLE connection operation.
        if (tracking && nowUs >= lastUs && nowUs - lastUs <= 2000000ULL) {
            const auto dt = nowUs - lastUs;
            wheel.advance(lastUs, dt, m.speed_kph * 1000000.0 / (3600.0 * WHEEL_MM));
            crank.advance(lastUs, dt, m.cadence_rpm / 60.0);
        }
        tracking = true;
        lastUs = nowUs;
    }
    void setWheelCount(uint32_t count) { wheel.count = count; wheel.fraction = 0; }
    void powerPacket(uint16_t watts, uint8_t (&out)[14]) const {
        put16(out, 0x0030); // Wheel + crank revolution data present.
        put16(out + 2, watts > 32767 ? 32767 : watts); // Signed instantaneous W.
        put32(out + 4, wheel.count);
        put16(out + 8, static_cast<uint16_t>(wheel.eventTime * 2u)); // CPS wheel: 1/2048 s.
        put16(out + 10, static_cast<uint16_t>(crank.count));
        put16(out + 12, crank.eventTime); // CPS crank: 1/1024 s.
    }
    void cscPacket(uint8_t (&out)[11]) const {
        out[0] = 0x03; // Wheel and crank data present.
        put32(out + 1, wheel.count);
        put16(out + 5, wheel.eventTime); // CSC wheel time: 1/1024 s.
        put16(out + 7, static_cast<uint16_t>(crank.count));
        put16(out + 9, crank.eventTime);
    }
};
} // namespace cycling
