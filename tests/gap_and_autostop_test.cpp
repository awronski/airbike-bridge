#include <cstdio>
#include "ride_session.h"

static unsigned failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); ++failures; \
} } while (0)

static session::Input packet(uint32_t sequence, uint32_t now, uint16_t elapsed, uint32_t distance) {
    session::Input b;
    b.connected = b.hasData = true;
    b.sequence = sequence; b.packetMs = now;
    b.metrics.elapsed_s = elapsed; b.metrics.distance_m = distance;
    b.metrics.calories_kcal = elapsed;
    b.metrics.power_w = 100; b.metrics.speed_kph = 18; b.metrics.cadence_rpm = 60;
    return b;
}

static void packetGap(bool delayedLoop) {
    app::Settings cfg; cfg.autoStart = cfg.autoStop = false;
    session::Tracker t;
    auto b = packet(1, 0, 0, 0);
    t.tick(b, {}, cfg, 0); t.start(cfg.selected(), 0);
    for (uint32_t now = 10; now <= 10000; now += 10) {
        if (now == 1000) b = packet(2, now, 1, 5);
        if (now == 9000) b = packet(3, now, 9, 45);
        if (now == 10000) b = packet(4, now, 10, 50);
        if (!delayedLoop || now <= 1000 || now >= 9000) t.tick(b, {}, cfg, now);
    }
    CHECK(t.current.durationMs == 10000);
    CHECK(t.current.distanceM == 50 && t.current.calories == 10);
    CHECK(t.current.partial);
    CHECK(t.current.bikeMs < t.current.durationMs); // Unknown samples are not invented.
    CHECK(t.current.powerSum == t.current.bikeMs * 100);
    CHECK(t.current.speedSum == t.current.bikeMs * 1800);
    t.stop(true, 10000);
    t.tick(packet(5, 11000, 11, 55), {}, cfg, 11000);
    CHECK(t.current.durationMs == 10000 && t.current.distanceM == 50);
}

static void shortTimeout() {
    app::Settings cfg; cfg.autoStart = false; cfg.stopS = 1;
    session::Tracker t;
    auto b = packet(1, 0, 0, 0);
    t.tick(b, {}, cfg, 0); t.start(cfg.selected(), 0);
    bool stoppedWhilePedalling = false;
    for (uint32_t now = 10; now <= 6600; now += 10) {
        if (now % 1100 == 0) b = packet(now / 1100 + 1, now, now / 1000, now / 200);
        t.tick(b, {}, cfg, now);
        stoppedWhilePedalling |= t.state != session::RECORDING;
    }
    CHECK(!stoppedWhilePedalling);
    b = packet(8, 7700, 7, 38);
    b.metrics.power_w = 0; b.metrics.cadence_rpm = 0; // Coasting is inactive.
    t.tick(b, {}, cfg, 7700);
    CHECK(t.state == session::STOPPED && !t.manualStop);

    // A frozen nonzero result must not keep recording indefinitely.
    session::Tracker frozen;
    b = packet(1, 0, 0, 0);
    frozen.tick(b, {}, cfg, 0); frozen.start(cfg.selected(), 0);
    for (uint32_t now = 10; now <= 4000; now += 10) {
        if (now % 1000 == 0) { ++b.sequence; b.packetMs = now; }
        frozen.tick(b, {}, cfg, now);
    }
    CHECK(frozen.state == session::STOPPED);

    session::Tracker wrap;
    const uint32_t start = UINT32_MAX - 500;
    b = packet(1, start, 0, 0);
    wrap.tick(b, {}, cfg, start); wrap.start(cfg.selected(), start);
    wrap.tick(b, {}, cfg, start + 1000u);
    CHECK(wrap.state == session::RECORDING);
}

int main() {
    packetGap(false); packetGap(true); shortTimeout();
    if (!failures) std::puts("Packet gaps, elapsed recording time, short auto-stop, coasting, frozen data and rollover passed.");
    return failures ? 1 : 0;
}
