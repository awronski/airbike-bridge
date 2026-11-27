#include <assert.h>
#include <stdio.h>
#include "ride_session.h"
using session::Tracker;
static session::Input bike(uint32_t seq,uint32_t now,uint16_t elapsed,uint32_t distance,uint16_t power=100,float cadence=60){
    session::Input b;b.connected=b.hasData=true;b.sequence=seq;b.packetMs=now;b.metrics.elapsed_s=elapsed;
    b.metrics.distance_m=distance;b.metrics.power_w=power;b.metrics.cadence_rpm=cadence;b.metrics.speed_kph=power?20:0;
    b.metrics.calories_kcal=elapsed/10;return b;
}
static heart::Sample heartAt(uint32_t now,uint16_t bpm=140){heart::Sample h;h.connected=h.contact=h.received=true;h.at=now;h.bpm=bpm;return h;}
static void tick(Tracker& t,const app::Settings& c,uint32_t now,uint16_t elapsed,uint32_t distance,uint16_t power=100,float cadence=60){t.tick(bike(now/1000+1,now,elapsed,distance,power,cadence),heartAt(now),c,now);}
int main(){
    app::Settings cfg;
    Tracker t;
    for(unsigned i=0;i<=3;++i)tick(t,cfg,i*1000,i,i*5);
    assert(t.state==session::READY);
    tick(t,cfg,4000,4,20);assert(t.state==session::RECORDING&&t.current.durationMs==0);
    tick(t,cfg,5000,5,25);assert(t.current.durationMs==1000&&t.current.distanceM==5&&t.current.maxHR==140);
    assert(t.current.hrMs==1000&&t.current.hrSum==140000);
    t.stop(true,5000);
    for(unsigned i=6;i<15;++i)tick(t,cfg,i*1000,i,i*5,200);
    assert(t.state==session::STOPPED&&t.manualStop&&t.current.durationMs==1000&&t.current.distanceM==5);
    t.resume(14000);tick(t,cfg,15000,15,75,200);
    assert(t.state==session::RECORDING&&t.current.durationMs==2000&&t.current.distanceM==10);
    const auto savedName=t.current.riderName;
    (void)savedName;
    t.finish(session::FINISHED,15000);assert(t.pending&&t.history.count==0);
    uint8_t bytes[session::HISTORY_BYTES];session::encode(t.history,bytes,&t.current);
    session::History loaded;assert(session::decode(loaded,bytes,sizeof(bytes))&&loaded.count==1&&loaded.records[0].id==1);
    assert(loaded.records[0].hrMs==2000&&loaded.records[0].maxHR==140);
    assert(t.history.count==0); // Staging a failed flash write cannot append a ride.
    t.commit(15000);assert(t.state==session::READY&&t.history.count==1&&t.rearm);
    for(unsigned i=16;i<22;++i)tick(t,cfg,i*1000,i,i*5);
    assert(t.state==session::READY); // Continued pedalling cannot immediately restart after finish.
    tick(t,cfg,22000,22,110,0,0);assert(!t.rearm);
    for(unsigned i=23;i<=26;++i)tick(t,cfg,i*1000,i,i*5);
    assert(t.state==session::RECORDING);
    t.clear(26000);assert(t.history.count==1&&t.state==session::READY);
    // Automatic pause and resume, and coasting (speed alone) is inactivity.
    Tracker a;cfg.autoStart=false;tick(a,cfg,0,0,0);a.start(cfg.selected(),0);
    for(unsigned i=1;i<=10;++i){auto b=bike(i+1,i*1000,i,i*5,0,0);b.metrics.speed_kph=20;a.tick(b,heartAt(i*1000),cfg,i*1000);}
    assert(a.state==session::STOPPED&&!a.manualStop&&a.current.durationMs==10000);
    cfg.autoStart=true;
    for(unsigned i=11;i<=14;++i)tick(a,cfg,i*1000,i,i*5);
    assert(a.state==session::RECORDING&&a.current.durationMs==10000);
    // Identical final packets with nonzero watts cannot trigger automatic recording.
    Tracker repeated;
    for(unsigned i=0;i<20;++i)repeated.tick(bike(i+1,i*1000,100,300,200),heartAt(i*1000),cfg,i*1000);
    assert(repeated.state==session::READY);
    // Ready/finished console states override nonzero frozen source values.
    Tracker finished;
    for(unsigned i=0;i<20;++i){auto b=bike(i+1,i*1000,i,i*5,200);b.finished=true;finished.tick(b,heartAt(i*1000),cfg,i*1000);}
    assert(finished.state==session::READY);
    // Idle automatic finishing also applies to manual pauses; HR cannot keep a ride alive.
    Tracker idle;tick(idle,cfg,0,0,0);idle.start(cfg.selected(),0);tick(idle,cfg,1000,1,5);idle.stop(true,1000);
    auto zero=bike(3,601000,2,5,0,0);idle.tick(zero,heartAt(601000),cfg,601000);
    assert(idle.pending&&idle.current.reason==session::INACTIVE);
    // Manual Resume grants a full auto-stop window and a new inactivity deadline.
    for(bool autoStart:{false,true}){
        app::Settings timers;timers.autoStart=autoStart;
        Tracker paused;tick(paused,timers,0,0,0);paused.start(timers.selected(),0);
        tick(paused,timers,1000,1,5);paused.stop(true,1000);
        tick(paused,timers,541000,1,5,0,0);paused.resume(541000);
        tick(paused,timers,550999,1,5,0,0);
        assert(paused.state==session::RECORDING&&!paused.pending);
        tick(paused,timers,551000,1,5,0,0);
        assert(paused.state==session::STOPPED&&!paused.manualStop&&!paused.pending);
        tick(paused,timers,1140999,1,5,0,0);assert(!paused.pending);
        tick(paused,timers,1141000,1,5,0,0);assert(paused.pending&&paused.current.reason==session::INACTIVE);
    }
    // Disconnection and reset finish once without accumulating new-console deltas.
    Tracker d;tick(d,cfg,0,0,0);d.start(cfg.selected(),0);tick(d,cfg,1000,1,5);
    session::Input missing;d.tick(missing,heartAt(2000),cfg,2000);assert(d.pending&&d.current.reason==session::DISCONNECTED);
    d.commit(2000);d.tick(missing,{},cfg,3000);assert(d.history.count==1&&!d.pending);
    Tracker reset;tick(reset,cfg,0,10,100);reset.start(cfg.selected(),0);tick(reset,cfg,1000,11,105);
    tick(reset,cfg,2000,0,0);assert(reset.pending&&reset.current.distanceM==5&&reset.current.reason==session::CONSOLE_RESET);
    // HR gaps are excluded, not averaged as zero, and stopped HR peaks are ignored.
    Tracker h;cfg.autoStart=false;tick(h,cfg,0,0,0);h.start(cfg.selected(),0);
    h.tick(bike(2,1000,1,5),heartAt(1000,100),cfg,1000);
    h.tick(bike(3,2000,2,10),{},cfg,2000);
    h.stop(true,2000);h.tick(bike(4,3000,3,15),heartAt(3000,220),cfg,3000);
    assert(h.current.hrMs==1000&&h.current.hrSum==100000&&h.current.maxHR==100);
    // Rider identity is snapshotted; later profile edits cannot alter it.
    app::Rider rider;rider.id=1;snprintf(rider.name,sizeof(rider.name),"Rider A");Tracker owned;tick(owned,cfg,0,0,0);owned.start(rider,0);
    snprintf(rider.name,sizeof(rider.name),"Changed");assert(!strcmp(owned.current.riderName,"Rider A"));
    // Storage preserves legacy summaries and handles corruption without mutation.
    rides::Tracker old;old.count=1;old.nextID=8;old.history[0].id=7;old.history[0].reason=rides::INACTIVE;
    old.history[0].durationS=60;old.history[0].powerSum=6000;old.history[0].sampleSeconds=60;
    session::History migrated;session::migrate(old,migrated);session::encode(migrated,bytes);
    assert(session::decode(loaded,bytes,sizeof(bytes)));assert(loaded.records[0].durationMs==60000&&loaded.records[0].powerSum==6000000&&loaded.records[0].hrMs==0);
    assert(!strcmp(loaded.records[0].riderName,"Guest"));bytes[20]^=1;
    assert(!session::decode(loaded,bytes,sizeof(bytes))&&loaded.records[0].id==7);

    // A late clock sync can date the original start across pause and timer rollover.
    constexpr uint32_t unixSeconds = 1795478400;
    constexpr uint32_t startMs = UINT32_MAX - 499;
    Tracker dated;
    dated.start(cfg.selected(), startMs);
    dated.dateRideStart(startMs, 0);
    assert(dated.current.startedAt == 0);
    dated.stop(true, startMs + 500);
    dated.resume(startMs + 900);
    dated.dateRideStart(startMs + 1000, uint64_t(unixSeconds) * 1000 + 250);
    assert(dated.current.startedAt == unixSeconds - 1);
    dated.dateRideStart(startMs + 2000, uint64_t(unixSeconds) * 1000 + 8500);
    assert(dated.current.startedAt == unixSeconds - 1);
    dated.finish(session::FINISHED, startMs + 2000);
    session::encode(migrated, bytes, &dated.current);
    assert(session::decode(loaded, bytes, sizeof(bytes)));
    assert(loaded.count == 2 && loaded.records[0].startedAt == unixSeconds - 1);
    assert(loaded.records[1].id == 7 && loaded.records[1].startedAt == 0);

    for(unsigned i=0;i<25;++i){session::Record r;r.reason=session::FINISHED;migrated.add(r);}
    assert(migrated.count==20&&migrated.records[0].id==32&&migrated.records[19].id==13);
    // Timer wraparound cannot cause an early automatic stop.
    Tracker wrap;auto b=bike(1,UINT32_MAX-1000,0,0);wrap.tick(b,{},cfg,UINT32_MAX-1000);wrap.start(cfg.selected(),UINT32_MAX-1000);
    b=bike(2,0,1,5);wrap.tick(b,{},cfg,0);assert(wrap.state==session::RECORDING&&wrap.current.durationMs==1001);
    puts("Session transitions, auto timers, paused totals, HR coverage, rider snapshots and dated/legacy ride storage passed.");
}
