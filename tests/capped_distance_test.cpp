#include <algorithm>
#include <cassert>
#include <cstdio>
#include "ride_session.h"

namespace {
constexpr uint32_t CONSOLE_LIMIT_M = 19995;

struct Ride {
    session::Tracker tracker;
    app::Settings settings;
    uint32_t sequence = 0;

    Ride() { settings.autoStart = settings.autoStop = false; }

    void packet(uint32_t now, uint16_t elapsed, uint32_t distance, float speed, bool finished = false) {
        session::Input input;
        input.connected = input.hasData = true;
        input.finished = finished;
        input.sequence = ++sequence;
        input.packetMs = now;
        input.metrics.elapsed_s = elapsed;
        input.metrics.distance_m = distance;
        input.metrics.speed_kph = speed;
        input.metrics.power_w = speed > 0 ? 100 : 0;
        tracker.tick(input, {}, settings, now);
    }

    void start(uint32_t now) { tracker.start(settings.selected(), now); }

    void tickWithoutPacket(uint32_t now) {
        const auto input = tracker.latest;
        tracker.tick(input, {}, settings, now);
    }
};
}

int main() {
    Ride longRide;
    longRide.packet(0, 0, 0, 18);
    longRide.start(0);
    for (uint32_t seconds = 1; seconds <= 5580; ++seconds) {
        uint32_t distance = std::min(seconds * 5, CONSOLE_LIMIT_M);
        longRide.packet(seconds * 1000, seconds, distance, 18);
        if (seconds == 3999) assert(longRide.tracker.current.distanceM == CONSOLE_LIMIT_M);
    }
    assert(longRide.tracker.current.distanceM == 27900);
    longRide.packet(5581000, 0, 0, 0, true);
    assert(longRide.tracker.pending && longRide.tracker.current.reason == session::CONSOLE_RESET);
    uint8_t bytes[session::HISTORY_BYTES];
    session::encode(longRide.tracker.history, bytes, &longRide.tracker.current);
    session::History saved;
    assert(session::decode(saved, bytes, sizeof(bytes)) && saved.records[0].distanceM == 27900);

    // A sub-metre remainder survives each packet, and loop ticks cannot count one interval twice.
    Ride fraction;
    fraction.packet(0, 4000, CONSOLE_LIMIT_M, 18);
    fraction.start(0);
    for (uint32_t now = 50; now <= 1000; now += 50) {
        if (now % 250 == 0) fraction.packet(now, 4000 + now / 1000, CONSOLE_LIMIT_M, 18);
        else fraction.tickWithoutPacket(now);
    }
    assert(fraction.tracker.current.distanceM == 5);

    Ride pause;
    pause.packet(0, 4000, CONSOLE_LIMIT_M, 36);
    pause.start(0);
    pause.packet(1000, 4001, CONSOLE_LIMIT_M, 36);
    pause.tracker.stop(true, 1000);
    pause.packet(10000, 4010, CONSOLE_LIMIT_M, 36);
    pause.tracker.resume(10500);
    pause.packet(11000, 4011, CONSOLE_LIMIT_M, 36);
    assert(pause.tracker.current.distanceM == 15); // 10 m before Stop, 5 m after Resume.

    // The first packet after a stale interval provides a new baseline, and final packets are ignored.
    Ride gap;
    gap.packet(1000, 4000, CONSOLE_LIMIT_M, 36);
    gap.start(1000);
    gap.packet(2000, 4001, CONSOLE_LIMIT_M, 36);
    gap.tickWithoutPacket(6000);
    gap.packet(7000, 4006, CONSOLE_LIMIT_M, 36);
    assert(gap.tracker.current.distanceM == 10 && gap.tracker.current.partial);
    gap.packet(8000, 4007, CONSOLE_LIMIT_M, 36);
    gap.packet(9000, 4008, CONSOLE_LIMIT_M, 36, true);
    assert(gap.tracker.current.distanceM == 20);

    // A console that continues through this reading keeps its own distance.
    Ride uncapped;
    uncapped.packet(0, 4000, 19990, 36);
    uncapped.start(0);
    uncapped.packet(1000, 4001, 19995, 36);
    uncapped.packet(2000, 4002, 20004, 36);
    assert(uncapped.tracker.current.distanceM == 14);
    std::puts("Console distance cap, saved totals, fractions, pause, stale packets and continued console counters passed.");
}
