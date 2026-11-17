#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "airbike_protocol.h"

int main() {
    // Explicit wire fixtures, independently laid out from the protocol table.
    // Manual: 300s, 46.80km/h, 76rpm, 0x012345m, 123kcal, 557W.
    const uint8_t manual[] = {0x73,0x58,0x12,0x2c,0x01,0x48,0x12,0xf8,0x02,0x45,0x23,0x01,0x7b,0x00,0x2d,0x02};
    airbike::Metrics m;
    assert(airbike::decode(manual, sizeof(manual), m));
    assert(m.elapsed_s == 300 && m.power_w == 557 && m.distance_m == 0x012345);
    assert(m.calories_kcal == 123 && !m.has_remaining);
    assert(fabs(m.speed_kph - 46.8f) < 0.001 && fabs(m.cadence_rpm - 76.0f) < 0.001);

    // Timed programme: 60s elapsed, 120s remaining, 25.00km/h, 45.5rpm,
    // 1,000m, 40kcal, 200W. The countdown shifts every later field by 2 bytes.
    const uint8_t timed[] = {0xf3,0x58,0x12,0x3c,0x00,0x78,0x00,0xc4,0x09,0xc7,0x01,0xe8,0x03,0x00,0x28,0x00,0xc8,0x00};
    assert(airbike::decode(timed, sizeof(timed), m));
    assert(m.elapsed_s == 60 && m.remaining_s == 120 && m.has_remaining);
    assert(m.power_w == 200 && m.distance_m == 1000 && m.calories_kcal == 40);
    assert(fabs(m.speed_kph - 25.0f) < 0.001 && fabs(m.cadence_rpm - 45.5f) < 0.001);

    // Never read beyond truncated notifications or turn malformed packets into zeros.
    for (size_t size = 0; size < sizeof(manual); ++size) assert(!airbike::decode(manual, size, m));
    for (size_t size = 0; size < sizeof(timed); ++size) assert(!airbike::decode(timed, size, m));
    assert(!airbike::decode(nullptr, 20, m));
    uint8_t invalid[sizeof(manual)];
    for (size_t i = 0; i < sizeof(manual); ++i) invalid[i] = manual[i];
    invalid[1] = 0;
    assert(!airbike::decode(invalid, sizeof(invalid), m));
    assert(m.power_w == 200);  // A failed decode leaves the last reading intact.

    assert(airbike::isFresh(true, true, 5000, 1001));
    assert(!airbike::isFresh(true, true, 5000, 1000));
    assert(!airbike::isFresh(false, true, 5000, 4999));
    assert(!airbike::isFresh(true, false, 5000, 4999));
    assert(airbike::isFresh(true, true, 32, UINT32_MAX - 32));
    puts("Protocol fixtures, malformed packets and data freshness passed.");
}
