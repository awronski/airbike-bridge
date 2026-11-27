#include <cassert>
#include <cstdio>
#include "ride_session.h"

namespace {
struct WireRide {
    app::Settings settings;
    session::Tracker tracker;
    session::Input input;

    WireRide() {
        settings.autoStart = settings.autoStop = false;
        input.connected = input.hasData = true;
    }

    void packet(uint16_t seconds, uint32_t metres) { packet(seconds, seconds, metres); }

    void packet(uint16_t receivedAt, uint16_t elapsed, uint32_t metres) {
        // Synthetic manual E01D measurement: 36 km/h, 60 rpm and 100 W.
        const uint8_t wire[] = {
            0x73, 0x58, 0x12, uint8_t(elapsed), uint8_t(elapsed >> 8),
            0x10, 0x0e, 0x58, 0x02, uint8_t(metres), uint8_t(metres >> 8),
            uint8_t(metres >> 16), 0, 0, 100, 0
        };
        assert(airbike::decode(wire, sizeof(wire), input.metrics));
        ++input.sequence;
        input.packetMs = uint32_t(receivedAt) * 1000;
        tracker.tick(input, {}, settings, input.packetMs);
    }

    void startAt(uint32_t metres) {
        packet(0, metres);
        tracker.start(settings.selected(), 0);
    }
};

bool expectDistance(const WireRide& ride, const char* stage, uint32_t expected) {
    const auto actual = ride.tracker.current.distanceM;
    if (actual == expected) return true;
    std::fprintf(stderr, "%s: expected %u dashboard metres, got %u (raw counter %u)\n",
                 stage, unsigned(expected), unsigned(actual), unsigned(ride.input.metrics.distance_m));
    return false;
}
}

int main() {
    // The historical 19,995 m and 19,999 m were saved session totals. Model a
    // possible 19,999 m wire plateau independently of the production cap constant.
    WireRide ride;
    ride.startAt(4);
    for (uint16_t seconds = 1; seconds <= 1999; ++seconds)
        ride.packet(seconds, 4 + 10u * seconds);
    ride.packet(2000, 19999);
    bool ok = expectDistance(ride, "First arrival at plateau", 19995);
    for (uint16_t seconds = 2001; seconds <= 2060; ++seconds) {
        // The dashboard polls the same measurement every 50 ms between packets.
        for (uint32_t delay = 50; delay < 1000; delay += 50)
            ride.tracker.tick(ride.input, {}, ride.settings, ride.input.packetMs + delay);
        ride.packet(seconds, 19999);
    }
    assert(ride.tracker.state == session::RECORDING);
    ok = expectDistance(ride, "60 s of fresh 36 km/h packets at plateau", 20595) && ok;
    ride.tracker.finish(session::FINISHED, 2060000);
    uint8_t bytes[session::HISTORY_BYTES];
    session::encode(ride.tracker.history, bytes, &ride.tracker.current);
    session::History saved;
    assert(session::decode(saved, bytes, sizeof(bytes)));
    assert(saved.count == 1 && saved.records[0].distanceM == 20595);

    // A console can repeat a reading and then catch up. Its 20 m advance
    // already covers both seconds since arrival at the repeated reading.
    WireRide continuing;
    continuing.startAt(19989);
    continuing.packet(1, 19999);
    continuing.packet(2, 19999);
    continuing.packet(3, 20019);
    ok = expectDistance(continuing, "Counter advanced after a brief plateau", 30) && ok;

    // The console can send frozen nonzero speed after its workout clock stops.
    WireRide frozen;
    frozen.startAt(20000);
    frozen.packet(1, 20000);
    for (uint16_t seconds = 2; seconds <= 8; ++seconds) frozen.packet(seconds, 1, 20000);
    ok = expectDistance(frozen, "Frozen workout clock", 10) && ok;
    frozen.packet(9, 2, 20000);
    ok = expectDistance(frozen, "Clock resumed without crediting frozen time", 10) && ok;
    frozen.packet(10, 3, 20000);
    ok = expectDistance(frozen, "Fresh speed after the clock resumed", 20) && ok;
    if (ok) std::puts("Wire replay past 20 km, saved distance, counter catch-up and frozen clock passed.");
    return ok ? 0 : 1;
}
