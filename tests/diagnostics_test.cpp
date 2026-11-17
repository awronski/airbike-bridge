#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "diagnostics_model.h"

int main() {
    using namespace diagnostic;
    Recorder r;
    r.boot(100, 7, "test-build");
    assert(r.enabled && !r.active && !r.hasSession);
    r.connection(110, BIKE, true);
    r.packet(1000, BIKE, true);
    r.activity(1000, true, 1);
    assert(r.active && r.hasSession && !r.due(300999));
    assert(r.due(301000));
    r.poll(3500);
    assert(r.stats[BIKE_GAPS] == 1 && r.stats[BIKE_DISCONNECTS] == 0);
    r.poll(5000);
    r.packet(6000, BIKE, true);
    assert(r.stats[BIKE_MAX_GAP] == 5000 && r.stats[BIKE_GAPS] == 1);
    r.signal[0]=-82;r.signalAt[0]=6000;
    r.connection(6100, BIKE, false, 0x208);
    assert(r.at(r.eventCount-1).c == -82 && r.at(r.eventCount-1).d == 100);
    r.activity(6200, false, 1);
    assert(r.active && r.stats[BIKE_DISCONNECTS] == 1);
    r.connection(7000, BIKE, true);
    r.packet(7100, BIKE, true);
    assert(r.active);
    r.event(7200, DISCONNECT, POLAR, 0x216, USER_CHANGE);
    assert(r.at(r.eventCount-1).a == 0x216 && r.at(r.eventCount-1).b == USER_CHANGE);
    r.activity(8000, true, 2);
    assert(!r.active && r.pending && r.endReason == RIDER_CHANGED);
    assert(r.due(8000));
    r.saved(8000);
    r.activity(8100, true, 2);
    assert(r.active && r.rider == 2);
    r.activity(8150, true, UINT32_MAX); // Auto rider temporarily unresolved after a drop.
    assert(r.active && r.rider == 2);

    // Invalid notifications do not conceal a missing valid-measurement stream.
    uint8_t bad[32]; memset(bad, 0xa5, sizeof(bad));
    for(unsigned i=0;i<20;++i)r.packet(8200+i, BIKE, false, bad, sizeof(bad));
    assert(r.stats[BIKE_REJECTED] == 20 && r.rejectedCount == 4);
    assert(r.rejected[0].length == 32 && r.rejected[0].bytes[23] == 0xa5);
    r.packet(9000, POLAR, true);r.poll(12000);r.packet(13000,POLAR,true);
    assert(r.stats[HR_GAPS] == 1 && r.stats[HR_MAX_GAP] == 4000);

    // Ring limits and explicit omission counters, with continued whole-capture totals.
    for(unsigned i=0;i<300;++i)r.event(15000+i, CONNECT_FAILED, BIKE, 0x20d);
    assert(r.eventCount == EVENT_LIMIT && r.stats[EVENTS_OMITTED] > 0);
    for(unsigned i=0;i<130;++i){Health h{};h.at=20000+i*10000;h.heap=80000-i;r.health(h);}
    assert(r.healthCount == HEALTH_LIMIT && r.stats[HEALTH_OMITTED] == 10);
    Compact report; r.compact(report, 2000000);
    uint8_t bytes[STORAGE_BYTES]; encode(report,bytes);
    Compact restored; assert(decode(restored,bytes,sizeof(bytes)));
    assert(restored.eventCount == SAVED_EVENTS && restored.stats[BIKE_REJECTED] == 20);
    assert(!strcmp(restored.build,"test-build") && restored.rejectedCount == 4);
    const auto savedID=restored.sessionID;
    bytes[100]^=1;
    assert(!decode(restored,bytes,sizeof(bytes)) && restored.sessionID == savedID);
    assert(!decode(restored,bytes,sizeof(bytes)-1));

    Recorder wrap;wrap.boot(UINT32_MAX-5000,1,"wrap");
    wrap.packet(UINT32_MAX-1000,BIKE,true);wrap.activity(UINT32_MAX-1000,true,0);
    wrap.poll(2000);wrap.packet(3000,BIKE,true);
    assert(wrap.stats[BIKE_MAX_GAP] == 4001);
    wrap.activity(598000,false,0);assert(wrap.active);
    wrap.activity(600000,false,0);assert(!wrap.active && wrap.pending);
    wrap.saved(600000);assert(!wrap.due(600001));
    wrap.setEnabled(false,600002);auto count=wrap.eventCount;
    wrap.packet(600003,BIKE,true);wrap.event(600004,CONNECT,BIKE);
    assert(wrap.eventCount == count);
    wrap.setEnabled(true,600005);wrap.activity(600006,true,0);
    assert(wrap.active && wrap.partial);

    // A failed checkpoint is retried on the five-minute schedule, never every loop.
    r.attempted(2000000);assert(!r.due(2000001));assert(r.due(2300000));
    assert(interpretReason(0x208) == TIMEOUT);
    assert(interpretReason(8) == OTHER); // host error 8 is not HCI timeout 0x208
    assert(interpretReason(0x213) == REMOTE_END);
    assert(interpretReason(0x216) == LOCAL_END);
    Recorder finished;finished.boot(0,1,"test");finished.activity(1,true,0);
    finished.finish(100,FINISHED);finished.saved(100);finished.activity(101,true,0);
    assert(!finished.active); // Do not replace a finished ride with its last cached pedal reading.
    finished.activity(200,false,0);finished.activity(300,true,0);assert(finished.active);
    static_assert(sizeof(Recorder)+sizeof(Compact)+STORAGE_BYTES+2048 <= 16384,"Diagnostic RAM budget");
    puts("Diagnostic gaps, capture lifetime, rollover, bounded retention, persistence and reasons passed.");
}
