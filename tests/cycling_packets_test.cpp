#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "cycling_packets.h"

int main() {
    airbike::Metrics m;
    m.power_w = 321; m.speed_kph = 36; m.cadence_rpm = 90;
    cycling::Encoder encoder;
    for (uint64_t t = 0; t <= 10000000; t += 100000) encoder.update(t, true, m);
    // 36 km/h with a 2m wheel = 5 rev/s; 90rpm = 1.5 rev/s.
    assert(encoder.wheel.count == 50 && encoder.crank.count == 15);
    assert(encoder.wheel.eventTime == 10240 && encoder.crank.eventTime == 10240);
    uint8_t cps[14], csc[11];
    encoder.powerPacket(m.power_w, cps);
    encoder.cscPacket(csc);
    // CPS wheel time uses 1/2048s, twice the CSC wheel-time ticks.
    const uint8_t expectedPower[] = {0x30,0x00,0x41,0x01,0x32,0,0,0,0x00,0x50,0x0f,0,0x00,0x28};
    const uint8_t expectedCSC[] = {0x03,0x32,0,0,0,0x00,0x28,0x0f,0,0x00,0x28};
    assert(memcmp(cps, expectedPower, sizeof(cps)) == 0);
    assert(memcmp(csc, expectedCSC, sizeof(csc)) == 0);
    // Independent receiver-side reconstruction of all requested quantities.
    const uint32_t wheelRevs = cps[4] | (uint32_t(cps[5]) << 8) |
        (uint32_t(cps[6]) << 16) | (uint32_t(cps[7]) << 24);
    const double wheelSeconds = airbike::little16(cps + 8) / 2048.0;
    const double crankSeconds = airbike::little16(cps + 12) / 1024.0;
    assert(airbike::little16(cps + 2) == 321);
    assert(std::fabs(wheelRevs * 2.0 / wheelSeconds * 3.6 - 36.0) < 0.001);
    assert(std::fabs(airbike::little16(cps + 10) * 60.0 / crankSeconds - 90.0) < 0.001);
    assert(wheelRevs * 2 == 100); // 100 metres in this 10-second fixture.

    // Last event time must remain the crossing time between update boundaries.
    cycling::Encoder slow;
    m.cadence_rpm = 60; m.speed_kph = 7.2f;
    slow.update(0, true, m);
    slow.update(1300000, true, m);
    assert(slow.crank.count == 1 && slow.crank.eventTime == 1024);
    const auto oldTime = slow.crank.eventTime;
    m.cadence_rpm = 0; m.speed_kph = 0;
    slow.update(2000000, true, m);
    assert(slow.crank.count == 1 && slow.crank.eventTime == oldTime);

    // Data loss, reconnection and blocking discovery cannot invent old motion.
    encoder.update(11000000, false, m);
    m.cadence_rpm = 90; m.speed_kph = 36;
    encoder.update(30000000, true, m);
    assert(encoder.wheel.count == 50 && encoder.crank.count == 15);
    encoder.update(40000000, true, m);
    assert(encoder.wheel.count == 50);

    // Standard counter and event-time rollover; set-cumulative-value semantics.
    cycling::Encoder rollover;
    rollover.setWheelCount(UINT32_MAX);
    m.speed_kph = 7.2f; m.cadence_rpm = 60;
    rollover.update(63900000, true, m);
    rollover.update(65000000, true, m);
    assert(rollover.wheel.count == 0 && rollover.crank.count == 1);
    assert(rollover.crank.eventTime == 921); // 64.9s modulo 64s, truncated.
    rollover.setWheelCount(123456);
    rollover.cscPacket(csc);
    assert(csc[1] == 0x40 && csc[2] == 0xe2 && csc[3] == 0x01 && csc[4] == 0);
    rollover.powerPacket(65535, cps);
    assert(cps[2] == 0xff && cps[3] == 0x7f);
    assert(cps[8] == 0x32 && cps[9] == 0x07); // 64.9s wraps to 1842 wheel ticks.
    puts("CPS/CSC wire fixtures, rate conversion, event timing, idle, stale input and rollover passed.");
}
