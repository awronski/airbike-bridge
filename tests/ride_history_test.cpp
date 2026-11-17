#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ride_history.h"

static airbike::Metrics sample(uint16_t elapsed, uint32_t distance, uint16_t power, float speed) {
    airbike::Metrics m;
    m.elapsed_s = elapsed; m.distance_m = distance; m.power_w = power; m.speed_kph = speed;
    return m;
}
static void observe(rides::Tracker& t, uint32_t seq, uint32_t now, uint16_t elapsed,
                    uint32_t distance, uint16_t power = 100, float speed = 10) {
    t.observe(true, true, seq, sample(elapsed,distance,power,speed), now);
}
int main() {
    rides::Tracker t;
    observe(t,1,0,0,0,100,10);
    assert(!t.active && !t.count);
    observe(t,2,1000,1,3,200,20);
    observe(t,3,2000,2,9,300,30);
    assert(t.active && t.current.durationS == 2 && t.current.distanceM == 9);
    assert(t.current.maxPower == 300 && t.current.maxSpeed == 3000);
    assert(t.current.sampleSeconds == 2 && t.current.powerSum == 300 && t.current.speedSum == 3000);
    // A repeated final packet, including a non-zero instantaneous value, does
    // not add duration, distance, average weight or keep the idle timer alive.
    for (uint32_t i = 4; i < 20; ++i) observe(t,i,2000 + (i-3)*1000,2,9,300,30);
    assert(t.current.durationS == 2 && t.current.powerSum == 300);
    observe(t,20,601999,2,9,300,30);
    assert(t.active);
    observe(t,21,602000,2,9,300,30);
    assert(!t.active && t.count == 1 && t.history[0].reason == rides::INACTIVE);
    assert(t.history[0].durationS == 2 && t.history[0].distanceM == 9);
    observe(t,22,603000,2,9,300,30);
    observe(t,23,1300000,2,9,300,30);
    assert(!t.active && t.count == 1); // No duplicate histories from repeats.

    // Clear ride discards only the current session and keeps history intact.
    observe(t,24,1301000,3,12,400,40);
    assert(t.active);
    const auto originalHistory = t.history[0];
    t.dirty = false;
    t.discard();
    assert(!t.active && !t.dirty && t.count == 1);
    assert(t.current.durationS == 0 && t.current.distanceM == 0);
    observe(t,25,1302000,3,12,400,40);
    assert(!t.active);
    observe(t,26,1303000,4,15,500,50);
    assert(t.active && t.current.durationS == 1 && t.current.distanceM == 3);
    assert(t.current.powerSum == 400 && t.current.maxPower == 500);
    t.observe(false,true,26,sample(4,15,500,50),1304000);
    assert(t.count == 2 && !t.active && t.history[0].reason == rides::DISCONNECTED);
    assert(t.history[1].id == originalHistory.id);
    t.observe(false,true,26,sample(4,15,500,50),1305000);
    assert(t.count == 2); // Disconnection saved once.

    // Reconnection to an old results screen cannot create a new ride.
    observe(t,27,1400000,4,15,500,50);
    observe(t,28,1401000,4,15,500,50);
    assert(!t.active && t.count == 2);
    observe(t,29,1402000,0,0);
    observe(t,30,1403000,1,2);
    assert(t.active && t.current.durationS == 1);
    observe(t,31,1404000,0,0);
    assert(!t.active && t.count == 3 && t.history[0].reason == rides::CONSOLE_RESET);

    // Long input gaps contribute no invented average samples.
    observe(t,32,1405000,1,2,200,20);
    observe(t,33,1415000,11,20,300,30);
    assert(t.current.partial && t.current.sampleSeconds == 1);
    assert(t.current.powerSum == 100);

    // Rolling history keeps exactly the newest 20 rides, in order.
    rides::Tracker ring;
    uint32_t seq=0;
    for (uint32_t ride=0; ride<25; ++ride) {
        observe(ring,++seq,ride*10000,0,0,100+ride,10);
        observe(ring,++seq,ride*10000+1000,1,ride+1,200+ride,20);
        ring.observe(false,true,seq,sample(1,ride+1,200+ride,20),ride*10000+2000);
    }
    assert(ring.count == 20 && ring.history[0].id == 25 && ring.history[19].id == 6);
    uint8_t encoded[rides::STORAGE_BYTES];
    rides::encode(ring, encoded);
    rides::Tracker restored;
    assert(rides::decode(restored, encoded, sizeof(encoded)));
    assert(restored.count == 20 && restored.nextID == 26 && restored.history[19].id == 6);
    assert(restored.history[0].maxPower == ring.history[0].maxPower);
    assert(restored.history[0].powerSum == ring.history[0].powerSum);
    assert(!restored.active); // Unfinished RAM session is not fabricated on boot.
    assert(!rides::decode(restored, encoded, sizeof(encoded)-1));
    encoded[20] ^= 1;
    assert(!rides::decode(restored, encoded, sizeof(encoded)) && restored.count == 20);
    // Millisecond timer rollover cannot prevent inactivity saving.
    rides::Tracker wrap;
    observe(wrap,1,UINT32_MAX-2000,0,0);
    observe(wrap,2,UINT32_MAX-1000,1,2);
    observe(wrap,3,598998,1,2);
    assert(wrap.active);
    observe(wrap,4,598999,1,2);
    assert(!wrap.active && wrap.count == 1);
    puts("Ride progress, idle/disconnect/reset saves, discard, averages, newest-20 history, storage validation and clock rollover passed.");
}
