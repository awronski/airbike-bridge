#pragma once
#include "bridge_settings.h"
#include "heart_rate.h"
namespace session {
enum State : uint8_t { READY, RECORDING, STOPPED };
enum Reason : uint8_t { DISCONNECTED=1, INACTIVE=2, CONSOLE_RESET=3, FINISHED=4 };
constexpr uint32_t EARLIEST_START_TIME_S = 1577836800; // 2020-01-01 UTC, after an unsynchronized clock.
struct Record {
    uint32_t id=0, distanceM=0, calories=0, riderID=0, startedAt=0;
    uint64_t durationMs=0, powerSum=0, speedSum=0, bikeMs=0, hrSum=0, hrMs=0;
    uint16_t maxPower=0, maxSpeed=0, maxHR=0; uint8_t reason=0; bool partial=false;
    char riderName[app::NAME_BYTES]="Guest";
};
struct History {
    Record records[rides::LIMIT]; size_t count=0; uint32_t nextID=1;
    void add(const Record& record) {
        for(size_t i=std::min(count,rides::LIMIT-1);i>0;--i)records[i]=records[i-1];
        records[0]=record;records[0].id=nextID++;if(!nextID)nextID=1;
        if(count<rides::LIMIT)++count;
    }
};
struct Input {
    bool connected=false, hasData=false, finished=false;
    uint32_t sequence=0, packetMs=0;airbike::Metrics metrics;
    bool fresh(uint32_t now) const {return airbike::isFresh(connected,hasData,now,packetMs);}
};
struct Tracker {
    History history;Record current;State state=READY;
    bool manualStop=false, pending=false, baseline=false, clockSet=false, tracking=false, rearm=false;
    uint32_t lastTick=0,lastSequence=0,lastActivity=0,activitySince=0;
    uint32_t recordingSinceMs=0, cappedDistanceRemainder=0;
    uint32_t rideStartedMs=0;
    uint32_t lastConsoleClockAdvanceMs=0, pendingSpeedDistanceUnits=0, speedDistanceCreditM=0;
    Input latest;airbike::Metrics last;
    bool hasRide() const {return state!=READY;}
    bool fresh(uint32_t now) const {return latest.fresh(now);}
    static uint16_t speed(float kph){return uint16_t(std::lround(kph*100));}
    void resetDetectors(){tracking=false;}
    void baselineNow(){if(latest.hasData){last=latest.metrics;lastSequence=latest.sequence;baseline=true;lastConsoleClockAdvanceMs=latest.packetMs;}}
    void start(const app::Rider& rider,uint32_t now) {
        current=Record{};current.riderID=rider.id;memcpy(current.riderName,rider.name,app::NAME_BYTES);
        state=RECORDING;manualStop=false;pending=false;lastTick=now;clockSet=true;lastActivity=now;
        rideStartedMs=now;
        recordingSinceMs=now;cappedDistanceRemainder=0;
        pendingSpeedDistanceUnits=0;speedDistanceCreditM=0;
        current.partial=latest.metrics.elapsed_s>0 || latest.metrics.distance_m>0;
        baselineNow();
        if(!latest.finished){current.maxPower=latest.metrics.power_w;current.maxSpeed=speed(latest.metrics.speed_kph);}
    }
    void dateRideStart(uint32_t now, uint64_t wallTimeMs) {
        if (!hasRide() || current.startedAt) return;
        // A clock sync during the ride can still date its original start.
        const uint32_t elapsedMs = now - rideStartedMs;
        if (wallTimeMs < elapsedMs) return;
        const uint64_t startedAt = (wallTimeMs - elapsedMs) / 1000;
        if (startedAt >= EARLIEST_START_TIME_S && startedAt <= UINT32_MAX) {
            current.startedAt = uint32_t(startedAt);
        }
    }
    void stop(bool manual,uint32_t now){state=STOPPED;manualStop=manual;lastTick=now;tracking=false;pendingSpeedDistanceUnits=0;}
    void resume(uint32_t now){
        state=RECORDING;manualStop=false;lastTick=now;lastActivity=now;
        recordingSinceMs=now;
        baselineNow();tracking=false;
    }
    void clear(uint32_t now) {
        current=Record{};state=READY;manualStop=false;pending=false;tracking=false;lastTick=now;
        rearm=latest.fresh(now)&&!latest.finished&&(latest.metrics.power_w>0||latest.metrics.cadence_rpm>0);
        baselineNow();
    }
    void finish(uint8_t why,uint32_t now){if(!hasRide()||pending)return;current.reason=why;pending=true;stop(true,now);}
    bool nonempty() const {return current.durationMs>0||current.distanceM>0;}
    void commit(uint32_t now){if(pending&&nonempty())history.add(current);clear(now);}

    void addCappedDistance(const Input& previous, const Input& packet, uint32_t now, bool clockAdvanced) {
        if (previous.finished || packet.finished || !previous.fresh(packet.packetMs) || !packet.fresh(now) ||
            uint32_t(packet.packetMs - lastConsoleClockAdvanceMs) >= airbike::DATA_FRESH_MS) {
            pendingSpeedDistanceUnits = 0;
            return;
        }
        // Only count the observed interval after Start or Resume, even if it began between packets.
        const uint32_t packetAge=now-packet.packetMs;
        const uint32_t recordingAge=now-recordingSinceMs;
        if(packetAge>=recordingAge)return;
        const uint32_t interval=std::min(packet.packetMs-previous.packetMs,recordingAge-packetAge);
        // Confirm buffered sub-second samples when the console clock advances.
        pendingSpeedDistanceUnits += uint32_t(speed(previous.metrics.speed_kph)) * interval;
        if (!clockAdvanced) return;
        constexpr uint32_t CENTIKPH_MS_PER_METRE = 360000;
        const uint32_t distanceUnits = cappedDistanceRemainder + pendingSpeedDistanceUnits;
        const uint32_t metres = distanceUnits / CENTIKPH_MS_PER_METRE;
        current.distanceM += metres;
        speedDistanceCreditM += metres;
        cappedDistanceRemainder = distanceUnits % CENTIKPH_MS_PER_METRE;
        pendingSpeedDistanceUnits = 0;
    }

    void tick(const Input& b,const heart::Sample& hr,const app::Settings& cfg,uint32_t now,const app::Rider* rider=nullptr,bool allowStart=true) {
        const bool isFresh=b.fresh(now);
        const uint32_t dt=clockSet?uint32_t(now-lastTick):0;lastTick=now;clockSet=true;
        // Duration is elapsed Recording time, including source gaps. The next
        // cumulative counters cover that same interval if recording continues.
        // Averages exclude unknown samples, and partial marks incomplete coverage.
        if(state==RECORDING&&!pending){
            current.durationMs+=dt;
            if(dt<=5000&&isFresh&&latest.fresh(now)){
                current.bikeMs+=dt;
                current.powerSum+=uint64_t(latest.finished?0:latest.metrics.power_w)*dt;
                current.speedSum+=uint64_t(latest.finished?0:speed(latest.metrics.speed_kph))*dt;
                if(hr.valid(now)){current.hrSum+=uint64_t(hr.bpm)*dt;current.hrMs+=dt;current.maxHR=std::max(current.maxHR,hr.bpm);}
            }else current.partial=true;
        }
        const auto previous=latest;
        latest=b;
        if(!b.connected){if(hasRide())finish(DISCONNECTED,now);baseline=false;tracking=false;return;}
        if(tracking&&uint32_t(now-lastActivity)>=4000)tracking=false;
        if(hasRide()&&!pending&&uint32_t(now-lastActivity)>=rides::IDLE_MS){finish(INACTIVE,now);return;}
        bool progressed=false;
        if(b.hasData&&b.sequence!=lastSequence){
            lastSequence=b.sequence;const auto& m=b.metrics;
            if(!baseline){last=m;baseline=true;lastConsoleClockAdvanceMs=b.packetMs;}
            else {
                const bool reset=m.elapsed_s<last.elapsed_s||m.distance_m<last.distance_m||m.calories_kcal<last.calories_kcal;
                if(reset){if(hasRide())finish(CONSOLE_RESET,now);tracking=false;rearm=false;}
                else {
                    const bool clockAdvanced = m.elapsed_s > last.elapsed_s;
                    const uint32_t distanceDelta = m.distance_m - last.distance_m;
                    const bool recording = state == RECORDING && !pending;
                    progressed=clockAdvanced||distanceDelta>0||m.calories_kcal>last.calories_kcal;
                    if (distanceDelta > 0) {
                        // If the console catches up, count only metres we have yet to add.
                        const uint32_t alreadyCounted = std::min(distanceDelta, speedDistanceCreditM);
                        speedDistanceCreditM -= alreadyCounted;
                        pendingSpeedDistanceUnits = 0;
                        cappedDistanceRemainder = 0;
                        if (recording) current.distanceM += distanceDelta - alreadyCounted;
                    } else if (recording && m.distance_m >= airbike::CONSOLE_DISTANCE_FALLBACK_MIN_M &&
                               previous.metrics.distance_m == m.distance_m) {
                        addCappedDistance(previous, b, now, clockAdvanced);
                    }
                    if (clockAdvanced) lastConsoleClockAdvanceMs = b.packetMs;
                    if(recording){
                        current.calories+=m.calories_kcal-last.calories_kcal;
                        if(isFresh&&!b.finished){current.maxPower=std::max(current.maxPower,m.power_w);current.maxSpeed=std::max(current.maxSpeed,speed(m.speed_kph));}}
                    if(isFresh&&!b.finished&&(m.power_w>0||m.cadence_rpm>0)){
                        if(progressed){if(!tracking){activitySince=now;tracking=true;}lastActivity=now;}
                    }else {tracking=false;if(isFresh)rearm=false;}
                }
                last=m;
            }
        }
        if(pending)return;
        // A short idle timeout must not expire between normal active packets.
        // Nonzero but frozen readings still time out after the progress watchdog.
        const bool motion=isFresh&&!b.finished&&(b.metrics.power_w>0||b.metrics.cadence_rpm>0);
        const uint32_t idleMs=uint32_t(now-lastActivity);
        if(state==RECORDING&&cfg.autoStop&&idleMs>=uint32_t(cfg.stopS)*1000&&
            (!motion||idleMs>=4000))stop(false,now);
        if(cfg.autoStart&&allowStart&&progressed&&tracking&&uint32_t(now-activitySince)>=uint32_t(cfg.startS)*1000){
            if(state==READY&&!rearm)start(rider?*rider:cfg.selected(),now);
            else if(state==STOPPED&&!manualStop)resume(now);
        }
    }
};
constexpr size_t RECORD_BYTES=128, HISTORY_BYTES=16+rides::LIMIT*RECORD_BYTES+4;
// The last four bytes were zero-filled in earlier v2 records.
constexpr size_t STARTED_AT_OFFSET = RECORD_BYTES - 4;
static_assert(72 + app::NAME_BYTES <= STARTED_AT_OFFSET, "Ride date overlaps rider name");
inline void encodeRecord(const Record& r,uint8_t* p){
    rides::writeLE(p,r.id,4);rides::writeLE(p+4,r.durationMs,8);rides::writeLE(p+12,r.distanceM,4);rides::writeLE(p+16,r.calories,4);
    rides::writeLE(p+20,r.maxPower,2);rides::writeLE(p+22,r.maxSpeed,2);rides::writeLE(p+24,r.powerSum,8);rides::writeLE(p+32,r.speedSum,8);
    rides::writeLE(p+40,r.bikeMs,8);rides::writeLE(p+48,r.hrSum,8);rides::writeLE(p+56,r.hrMs,8);rides::writeLE(p+64,r.maxHR,2);
    p[66]=r.reason;p[67]=r.partial;rides::writeLE(p+68,r.riderID,4);memcpy(p+72,r.riderName,app::NAME_BYTES);
    rides::writeLE(p+STARTED_AT_OFFSET,r.startedAt,4);
}
inline void encode(const History& h,uint8_t* out,const Record* pending=nullptr){
    memset(out,0,HISTORY_BYTES);memcpy(out,"ABS2",4);out[4]=2;
    const size_t count=std::min(h.count+(pending?1:0),rides::LIMIT);out[5]=count;
    uint32_t next=h.nextID+(pending?1:0);if(!next)next=1;rides::writeLE(out+8,next,4);
    for(size_t i=0;i<count;++i){Record r=(pending&&i==0)?*pending:h.records[i-(pending?1:0)];if(pending&&i==0)r.id=h.nextID;encodeRecord(r,out+16+i*RECORD_BYTES);}
    rides::writeLE(out+HISTORY_BYTES-4,rides::checksum(out,HISTORY_BYTES-4),4);
}
inline bool decode(History& h,const uint8_t* in,size_t n){
    if(!in||n!=HISTORY_BYTES||memcmp(in,"ABS2",4)||in[4]!=2||in[5]>rides::LIMIT||!rides::readLE(in+8,4)||
        rides::readLE(in+n-4,4)!=rides::checksum(in,n-4))return false;
    // Validate before mutating the destination.
    for(size_t i=0;i<in[5];++i){const auto* p=in+16+i*RECORD_BYTES;
        if(!rides::readLE(p,4)||p[66]<1||p[66]>4||p[67]>1||!app::validName(reinterpret_cast<const char*>(p+72)))return false;
        for(size_t j=0;j<i;++j)if(rides::readLE(p,4)==rides::readLE(in+16+j*RECORD_BYTES,4))return false;
    }
    h.count=in[5];h.nextID=rides::readLE(in+8,4);
    for(size_t i=0;i<h.count;++i){const auto* p=in+16+i*RECORD_BYTES;auto& r=h.records[i];
        r.id=rides::readLE(p,4);r.durationMs=rides::readLE(p+4,8);r.distanceM=rides::readLE(p+12,4);r.calories=rides::readLE(p+16,4);
        r.maxPower=rides::readLE(p+20,2);r.maxSpeed=rides::readLE(p+22,2);r.powerSum=rides::readLE(p+24,8);r.speedSum=rides::readLE(p+32,8);
        r.bikeMs=rides::readLE(p+40,8);r.hrSum=rides::readLE(p+48,8);r.hrMs=rides::readLE(p+56,8);r.maxHR=rides::readLE(p+64,2);
        r.reason=p[66];r.partial=p[67];r.riderID=rides::readLE(p+68,4);memcpy(r.riderName,p+72,app::NAME_BYTES);
        const uint32_t startedAt=rides::readLE(p+STARTED_AT_OFFSET,4);
        r.startedAt=startedAt>=EARLIEST_START_TIME_S?startedAt:0;
    }return true;
}
inline void migrate(const rides::Tracker& old,History& h){
    h.count=old.count;h.nextID=old.nextID;
    for(size_t i=0;i<h.count;++i){const auto& r=old.history[i];auto& d=h.records[i];d=Record{};
        d.id=r.id;d.durationMs=uint64_t(r.durationS)*1000;d.distanceM=r.distanceM;d.maxPower=r.maxPower;d.maxSpeed=r.maxSpeed;
        d.powerSum=r.powerSum*1000;d.speedSum=r.speedSum*1000;d.bikeMs=uint64_t(r.sampleSeconds)*1000;d.reason=r.reason;d.partial=r.partial;
    }
}
}
