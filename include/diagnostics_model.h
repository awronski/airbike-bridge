#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <algorithm>
#include "ride_history.h"

namespace diagnostic {
constexpr size_t EVENT_LIMIT=128, HEALTH_LIMIT=120, SAVED_EVENTS=40, STORAGE_BYTES=2048;
constexpr uint32_t CHECKPOINT_MS=300000, QUIET_MS=600000, GAP_MS=2500;
enum Source:uint8_t {SYSTEM=0,BIKE=1,POLAR=2,GARMIN=3,WIFI=4,STORAGE=5};
enum Kind:uint16_t {BOOT=1,SESSION_START,SESSION_END,CONNECT,DISCONNECT,CONNECT_START,CONNECT_FAILED,
    SUBSCRIBE,GAP_START,GAP_RECOVERED,STALE,REJECTED,CONTACT,CONSOLE,RIDE_STATE,
    SCAN_LOAD,SEND_FAILED,ADVERTISE_FAILED,TASK_DELAY,FLASH_WRITE,SETTING,LINK_PARAMS,CONNECT_DONE};
enum End:uint8_t {NONE=0,FINISHED=1,QUIET=2,RIDER_CHANGED=3,RECORDING_OFF=4};
enum Intent:uint8_t {UNEXPECTED=0,USER_CHANGE=1,SUBSCRIPTION_FAILED=2,WRONG_BAND=3};
enum Reason:uint8_t {OTHER=0,TIMEOUT=1,REMOTE_END=2,LOCAL_END=3};
inline Reason interpretReason(int code){return code==0x208?TIMEOUT:code==0x213?REMOTE_END:code==0x216?LOCAL_END:OTHER;}
enum Stat:size_t {BIKE_PACKETS,BIKE_REJECTED,BIKE_NOTIFICATIONS,HR_PACKETS,HR_REJECTED,
    BIKE_DISCONNECTS,HR_DISCONNECTS,GARMIN_DISCONNECTS,BIKE_GAPS,HR_GAPS,
    BIKE_MAX_GAP,HR_MAX_GAP,BIKE_TOTAL_GAP,HR_TOTAL_GAP,BIKE_MAX_INTERVAL,HR_MAX_INTERVAL,
    BIKE_INTERVAL_SUM,BIKE_INTERVAL_COUNT,CONNECT_FAILURES,TX_ATTEMPTS,TX_ACCEPTED,TX_FAILED,
    ADV_FAILED,LOOP_MAX_US,RELAY_MAX_US,CONNECT_MAX_MS,SCAN_MS,HEAP_MIN,LARGEST_MIN,
    LOOP_STACK_MIN,RELAY_STACK_MIN,BLE_STACK_MIN,FLASH_MAX_US,FLASH_FAILURES,EVENTS_OMITTED,
    HEALTH_OMITTED,BOOT_HEAP_MIN,COUNT};
struct Event {uint32_t at=0;uint16_t kind=0;uint8_t source=0,flags=0;int32_t a=0,b=0,c=0,d=0;uint32_t sequence=0,reserved=0;};
struct Health {uint32_t at=0,heap=0,largest=0,loopUs=0,relayUs=0;int16_t bikeRSSI=0,hrRSSI=0,wifiRSSI=0;uint16_t flags=0;uint32_t reserved=0;};
struct Rejected {uint32_t at=0;uint16_t length=0;uint8_t bytes[24]{};};
struct Compact {
    uint32_t stats[COUNT]{};char build[33]{};
    uint32_t sessionID=0,startMs=0,endMs=0,rider=0,resetReason=0,capturedMs=0,omitted=0;
    bool active=false,partial=false,hasSession=false;uint8_t endReason=NONE,eventCount=0,rejectedCount=0;
    int16_t rssiMin[3]{},rssiMax[3]{};
    Rejected rejected[4];Event events[SAVED_EVENTS];
};
struct Link {bool seen=false,connected=false,gap=false,stale=false;uint32_t last=0,lastReject=0;bool rejected=false;};
struct Recorder {
    uint32_t stats[COUNT]{};char build[33]{};
    Event events[EVENT_LIMIT];Health healths[HEALTH_LIMIT];Rejected rejected[4];Link links[2];
    size_t eventHead=0,eventCount=0,healthHead=0,healthCount=0;uint8_t rejectedCount=0;
    uint32_t sequence=0,sessionID=0,startMs=0,endMs=0,rider=0,resetReason=0,lastActivity=0,lastAttempt=0;
    bool enabled=true,active=false,hasSession=false,partial=false,pending=false,attemptMade=false,partialNext=false,rearm=false;
    uint8_t contextFlags=0;
    int16_t signal[2]{};uint32_t signalAt[2]{};
    uint8_t endReason=NONE;int16_t rssiMin[3]{},rssiMax[3]{};
    bool collecting()const{return enabled&&(!hasSession||active);}
    const Event& at(size_t i)const{return events[(eventHead+EVENT_LIMIT-eventCount+i)%EVENT_LIMIT];}
    const Health& healthAt(size_t i)const{return healths[(healthHead+HEALTH_LIMIT-healthCount+i)%HEALTH_LIMIT];}
    void event(uint32_t now,Kind kind,Source source=SYSTEM,int32_t a=0,int32_t b=0,int32_t c=0,int32_t d=0){
        if(!collecting())return;
        Event& e=events[eventHead];e=Event{};e.at=now;e.kind=kind;e.source=source;e.flags=contextFlags;e.a=a;e.b=b;e.c=c;e.d=d;e.sequence=++sequence;
        if((source==BIKE||source==POLAR)&&(kind==DISCONNECT||kind==GAP_START||kind==GAP_RECOVERED||kind==STALE||kind==CONNECT_FAILED)){
            const unsigned i=source==POLAR;e.c=signal[i];e.d=signal[i]?std::max(int32_t(now-signalAt[i]),int32_t(0)):0;
        }
        eventHead=(eventHead+1)%EVENT_LIMIT;if(eventCount<EVENT_LIMIT)++eventCount;else ++stats[EVENTS_OMITTED];
    }
    void boot(uint32_t now,uint32_t reason,const char* version){
        resetReason=reason;startMs=now;strncpy(build,version,sizeof(build)-1);event(now,BOOT,SYSTEM,reason);
    }
    void start(uint32_t now,uint32_t who){
        if(pending)return;
        if(hasSession){
            memset(stats,0,sizeof(stats));eventHead=eventCount=healthHead=healthCount=rejectedCount=0;
            memset(rssiMin,0,sizeof(rssiMin));memset(rssiMax,0,sizeof(rssiMax));
            for(auto& link:links){const bool connected=link.connected;link=Link{};link.connected=connected;}
        }
        ++sessionID;hasSession=active=true;partial=partialNext;partialNext=false;endReason=NONE;
        startMs=lastActivity=lastAttempt=now;endMs=0;rider=who;attemptMade=false;
        event(now,SESSION_START,SYSTEM,partial);
    }
    void finish(uint32_t now,End why){
        if(!active)return;event(now,SESSION_END,SYSTEM,why);active=false;pending=true;endMs=now;endReason=why;attemptMade=false;rearm=why==FINISHED;
    }
    void activity(uint32_t now,bool pedalling,uint32_t who){
        if(!enabled)return;
        if(!pedalling)rearm=false;
        if(active&&who!=UINT32_MAX&&who!=rider){finish(now,RIDER_CHANGED);return;}
        if(active&&uint32_t(now-lastActivity)>=QUIET_MS){finish(now,QUIET);return;}
        if(pedalling&&!rearm){if(!active&&who!=UINT32_MAX)start(now,who);if(active)lastActivity=now;}
    }
    void setEnabled(bool value,uint32_t now){
        if(value==enabled)return;
        if(!value){event(now,SETTING,SYSTEM,0);finish(now,RECORDING_OFF);}
        else partialNext=true;
        enabled=value;
    }
    bool due(uint32_t now)const{
        if(!hasSession||(!active&&!pending))return false;
        if(pending&&!attemptMade)return true;
        return uint32_t(now-lastAttempt)>=CHECKPOINT_MS;
    }
    void attempted(uint32_t now){lastAttempt=now;attemptMade=true;}
    void saved(uint32_t now){attempted(now);pending=false;}
    void gap(uint32_t now,unsigned index){
        auto& l=links[index];if(!l.seen)return;const uint32_t age=now-l.last;
        if(age>=GAP_MS&&!l.gap){l.gap=true;++stats[index?HR_GAPS:BIKE_GAPS];event(now,GAP_START,index?POLAR:BIKE,age,l.connected);}
        if(age>=(index?5000u:4000u)&&!l.stale){l.stale=true;event(now,STALE,index?POLAR:BIKE,age,l.connected);}
        if(l.gap)stats[index?HR_MAX_GAP:BIKE_MAX_GAP]=std::max(stats[index?HR_MAX_GAP:BIKE_MAX_GAP],age);
    }
    void poll(uint32_t now){if(collecting()){gap(now,0);gap(now,1);}}
    void connection(uint32_t now,Source source,bool up,int reason=0,Intent intent=UNEXPECTED){
        if(source==BIKE||source==POLAR)links[source==POLAR].connected=up;
        if(!collecting())return;
        event(now,up?CONNECT:DISCONNECT,source,reason,intent);
        if(!up)++stats[source==BIKE?BIKE_DISCONNECTS:source==POLAR?HR_DISCONNECTS:GARMIN_DISCONNECTS];
        if(source==BIKE||source==POLAR){auto& l=links[source==POLAR];l.connected=up;if(up&&!l.seen){l.seen=true;l.last=now;}}
    }
    void packet(uint32_t now,Source source,bool valid,const uint8_t* bytes=nullptr,size_t length=0){
        if(!collecting())return;const unsigned i=source==POLAR;auto& l=links[i];
        if(!valid){
            ++stats[i?HR_REJECTED:BIKE_REJECTED];
            if(!i&&bytes&&rejectedCount<4){auto& v=rejected[rejectedCount++];v.at=now;v.length=uint16_t(std::min(length,size_t(65535)));memcpy(v.bytes,bytes,std::min(length,sizeof(v.bytes)));}
            if(!l.rejected||uint32_t(now-l.lastReject)>=10000){event(now,REJECTED,source,length);l.lastReject=now;l.rejected=true;}
            return;
        }
        ++stats[i?HR_PACKETS:BIKE_PACKETS];
        l.connected=true;
        if(l.seen){
            const uint32_t interval=now-l.last;gap(now,i);
            stats[i?HR_MAX_INTERVAL:BIKE_MAX_INTERVAL]=std::max(stats[i?HR_MAX_INTERVAL:BIKE_MAX_INTERVAL],interval);
            if(!i){stats[BIKE_INTERVAL_SUM]+=interval;++stats[BIKE_INTERVAL_COUNT];}
            if(l.gap){stats[i?HR_TOTAL_GAP:BIKE_TOTAL_GAP]+=interval;event(now,GAP_RECOVERED,source,interval,l.connected);}
        }
        l.last=now;l.seen=true;l.gap=l.stale=false;
    }
    void health(const Health& h){
        if(!collecting())return;
        healths[healthHead]=h;healthHead=(healthHead+1)%HEALTH_LIMIT;if(healthCount<HEALTH_LIMIT)++healthCount;else ++stats[HEALTH_OMITTED];
        if(!stats[HEAP_MIN]||h.heap<stats[HEAP_MIN])stats[HEAP_MIN]=h.heap;
        if(!stats[LARGEST_MIN]||h.largest<stats[LARGEST_MIN])stats[LARGEST_MIN]=h.largest;
        const int16_t rssis[]={h.bikeRSSI,h.hrRSSI,h.wifiRSSI};
        for(unsigned i=0;i<3;++i)if(rssis[i]<0){if(!rssiMin[i]||rssis[i]<rssiMin[i])rssiMin[i]=rssis[i];if(!rssiMax[i]||rssis[i]>rssiMax[i])rssiMax[i]=rssis[i];}
    }
    void compact(Compact& out,uint32_t now)const{
        memcpy(out.stats,stats,sizeof(stats));memcpy(out.build,build,sizeof(build));
        out.sessionID=sessionID;out.startMs=startMs;out.endMs=endMs;out.rider=rider;out.resetReason=resetReason;out.capturedMs=now;
        out.active=active;out.partial=partial;out.hasSession=hasSession;out.endReason=endReason;
        out.eventCount=std::min(eventCount,SAVED_EVENTS);out.omitted=stats[EVENTS_OMITTED]+eventCount-out.eventCount;
        out.rejectedCount=rejectedCount;memcpy(out.rejected,rejected,sizeof(rejected));
        memcpy(out.rssiMin,rssiMin,sizeof(rssiMin));memcpy(out.rssiMax,rssiMax,sizeof(rssiMax));
        for(size_t i=0;i<out.eventCount;++i)out.events[i]=at(eventCount-out.eventCount+i);
    }
};
static_assert(sizeof(Event)==32&&sizeof(Health)==32,"Record size budget");
static_assert(COUNT*4+184<512,"Compact header budget");
inline void encode(const Compact& r,uint8_t* p){
    memset(p,0,STORAGE_BYTES);memcpy(p,"ABD1",4);p[4]=1;p[5]=r.eventCount;p[6]=r.rejectedCount;
    p[7]=(r.active?1:0)|(r.partial?2:0)|(r.hasSession?4:0);memcpy(p+8,r.build,32);
    const uint32_t meta[]={r.sessionID,r.startMs,r.endMs,r.rider,r.resetReason,r.capturedMs,r.omitted,r.endReason};
    for(unsigned i=0;i<8;++i)rides::writeLE(p+40+i*4,meta[i],4);
    for(unsigned i=0;i<COUNT;++i)rides::writeLE(p+72+i*4,r.stats[i],4);
    size_t at=72+COUNT*4;
    for(unsigned i=0;i<3;++i){rides::writeLE(p+at,uint16_t(r.rssiMin[i]),2);rides::writeLE(p+at+2,uint16_t(r.rssiMax[i]),2);at+=4;}
    for(unsigned i=0;i<4;++i){rides::writeLE(p+at,r.rejected[i].at,4);rides::writeLE(p+at+4,r.rejected[i].length,2);memcpy(p+at+6,r.rejected[i].bytes,24);at+=30;}
    for(unsigned i=0;i<r.eventCount;++i){const auto& e=r.events[i];auto* q=p+512+i*32;
        rides::writeLE(q,e.at,4);rides::writeLE(q+4,e.kind,2);q[6]=e.source;q[7]=e.flags;
        const uint32_t fields[]={uint32_t(e.a),uint32_t(e.b),uint32_t(e.c),uint32_t(e.d),e.sequence,0};
        for(unsigned j=0;j<6;++j)rides::writeLE(q+8+j*4,fields[j],4);
    }
    rides::writeLE(p+STORAGE_BYTES-4,rides::checksum(p,STORAGE_BYTES-4),4);
}
inline bool decode(Compact& r,const uint8_t* p,size_t n){
    if(!p||n!=STORAGE_BYTES||memcmp(p,"ABD1",4)||p[4]!=1||p[5]>SAVED_EVENTS||p[6]>4||p[7]>7||rides::readLE(p+68,4)>RECORDING_OFF||
       rides::readLE(p+n-4,4)!=rides::checksum(p,n-4))return false;
    for(unsigned i=0;i<p[5];++i){const auto* q=p+512+i*32;if(rides::readLE(q+4,2)<BOOT||rides::readLE(q+4,2)>CONNECT_DONE||q[6]>STORAGE)return false;}
    memcpy(r.build,p+8,32);r.build[32]=0;r.eventCount=p[5];r.rejectedCount=p[6];r.active=p[7]&1;r.partial=p[7]&2;r.hasSession=p[7]&4;
    uint32_t* meta[]={&r.sessionID,&r.startMs,&r.endMs,&r.rider,&r.resetReason,&r.capturedMs,&r.omitted};
    for(unsigned i=0;i<7;++i)*meta[i]=rides::readLE(p+40+i*4,4);r.endReason=rides::readLE(p+68,4);
    for(unsigned i=0;i<COUNT;++i)r.stats[i]=rides::readLE(p+72+i*4,4);
    size_t at=72+COUNT*4;
    for(unsigned i=0;i<3;++i){r.rssiMin[i]=int16_t(rides::readLE(p+at,2));r.rssiMax[i]=int16_t(rides::readLE(p+at+2,2));at+=4;}
    for(unsigned i=0;i<4;++i){r.rejected[i].at=rides::readLE(p+at,4);r.rejected[i].length=rides::readLE(p+at+4,2);memcpy(r.rejected[i].bytes,p+at+6,24);at+=30;}
    for(unsigned i=0;i<r.eventCount;++i){auto& e=r.events[i];const auto* q=p+512+i*32;e.at=rides::readLE(q,4);e.kind=rides::readLE(q+4,2);e.source=q[6];e.flags=q[7];
        e.a=int32_t(rides::readLE(q+8,4));e.b=int32_t(rides::readLE(q+12,4));e.c=int32_t(rides::readLE(q+16,4));e.d=int32_t(rides::readLE(q+20,4));e.sequence=rides::readLE(q+24,4);}
    return true;
}
}
