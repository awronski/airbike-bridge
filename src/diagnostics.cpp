#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <atomic>
#include "diagnostics.h"
#include "diagnostics_storage.h"
#include "diagnostics_build.h"

namespace diagnostic {
namespace {
Recorder recorder;
Compact report;
uint8_t storageBytes[STORAGE_BYTES];
Preferences prefs;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
struct Guard {Guard(){portENTER_CRITICAL(&mux);}~Guard(){portEXIT_CRITICAL(&mux);}};
std::atomic<bool> enabled{true};
bool opened=false,settingOK=true,savedAvailable=false,savedThisBoot=false;
uint32_t savedAt=0,lastPoll=0,lastHealth=0;
const char* saveStatus="not_saved";
int16_t bikeRSSI=0,hrRSSI=0;
uint32_t bikeRSSIAt=0,hrRSSIAt=0;
uint64_t lastPulse[2]{};uint32_t lastDelay[2]{};
int lastConsole=-2,lastRide=-1;
bool(*authorize)(const char*)=nullptr;
const char* const kinds[]={"unknown","boot","session_start","session_end","connect","disconnect","connect_start","connect_failed",
    "subscribe","gap_start","gap_recovered","stale","rejected","contact","console","ride_state","scan_load","send_failed",
    "advertise_failed","task_delay","flash_write","setting","link_params","connect_done"};
const char* const sources[]={"atom","bike","polar","garmin","wifi","storage"};
const char* const statsNames[]={"bike_packets","bike_rejected","bike_notifications","hr_packets","hr_rejected",
    "bike_disconnects","hr_disconnects","garmin_disconnects","bike_gaps","hr_gaps","bike_max_gap_ms","hr_max_gap_ms",
    "bike_completed_gap_ms","hr_completed_gap_ms","bike_max_interval_ms","hr_max_interval_ms","bike_interval_sum_ms","bike_interval_count",
    "connect_failures","tx_attempts","tx_accepted_locally","tx_failed","advertising_failed","loop_max_gap_us","relay_max_gap_us",
    "connect_max_ms","scan_ms","heap_min_bytes","largest_block_min_bytes","loop_stack_min_bytes","relay_stack_min_bytes","ble_stack_min_bytes",
    "flash_max_us","flash_failures","events_overwritten","health_overwritten","boot_heap_min_bytes"};
static_assert(sizeof(statsNames)/sizeof(*statsNames)==COUNT,"Stat names");
static_assert(sizeof(recorder)+sizeof(report)+sizeof(storageBytes)+2048<=16384,"Diagnostic RAM budget including HTTP scratch");

// Coalesce JSON fragments so an export does not produce hundreds of tiny Wi-Fi writes.
class Chunked {
    WebServer& server;char bytes[768];size_t used=0;
    void flush(){if(used){server.sendContent(bytes,used);used=0;}}
public:
    explicit Chunked(WebServer& s):server(s){}
    void sendContent(const char* text){
        size_t left=strlen(text);if(!left){flush();server.sendContent("");return;}
        while(left){const size_t n=std::min(left,sizeof(bytes)-used);memcpy(bytes+used,text,n);used+=n;text+=n;left-=n;if(used==sizeof(bytes))flush();}
    }
};

bool readSaved(){
    const bool valid=opened&&prefs.getBytesLength("report-v1")==STORAGE_BYTES&&prefs.getBytes("report-v1",storageBytes,STORAGE_BYTES)==STORAGE_BYTES&&decode(report,storageBytes,STORAGE_BYTES);
    if(!valid&&savedAvailable){savedAvailable=false;saveStatus="invalid_saved_report";}return valid;
}
void persist(uint32_t now){
    {Guard g;if(!recorder.due(now))return;recorder.compact(report,now);recorder.attempted(now);}
    const uint64_t start=esp_timer_get_time();
    const auto result=opened?save(prefs,report,storageBytes):SaveResult::WRITE_FAILED;
    const uint32_t elapsed=esp_timer_get_time()-start;
    if(result==SaveResult::OK){Guard g;recorder.saved(now);savedAvailable=savedThisBoot=true;savedAt=now;saveStatus="saved";}
    else saveStatus=result==SaveResult::NO_SPACE?"insufficient_storage":result==SaveResult::VERIFY_FAILED?"verification_failed":"write_failed";
    flash(0,elapsed,result==SaveResult::OK);
}
const char* boolean(bool v){return v?"true":"false";}
void jsonEvent(Chunked& s,const Event& e){
    char b[280];snprintf(b,sizeof(b),"{\"at_ms\":%lu,\"sequence\":%lu,\"kind\":\"%s\",\"source\":\"%s\",\"context_flags\":%u,\"a\":%ld,\"b\":%ld,\"c\":%ld,\"d\":%ld}",
        (unsigned long)e.at,(unsigned long)e.sequence,kinds[e.kind],sources[e.source],e.flags,(long)e.a,(long)e.b,(long)e.c,(long)e.d);s.sendContent(b);
}
void stream(WebServer& server,bool saved,bool full){
    uint32_t endSequence=0;size_t eventCount=0,healthCount=0;
    if(saved){if(!readSaved()){server.send(404,"text/plain","No valid saved diagnostic report");return;}eventCount=report.eventCount;}
    else {Guard g;recorder.compact(report,millis());endSequence=recorder.sequence;eventCount=full?recorder.eventCount:std::min(recorder.eventCount,size_t(12));healthCount=full?recorder.healthCount:0;}
    server.sendHeader("Cache-Control","no-store");
    if(full)server.sendHeader("Content-Disposition",saved?"attachment; filename=airbike-diagnostics-saved.json":"attachment; filename=airbike-diagnostics-current.json");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);server.send(200,"application/json","");Chunked s(server);
    char b[480];
    snprintf(b,sizeof(b),"{\"schema\":1,\"source\":\"%s\",\"firmware\":\"%s\",\"enabled\":%s,\"setting_storage_ok\":%s,\"saved_available\":%s,\"save_status\":\"%s\",\"saved_this_boot\":%s,\"save_age_ms\":",
        saved?"saved":"current",report.build,boolean(enabled.load()),boolean(settingOK),boolean(savedAvailable),saveStatus,boolean(savedThisBoot));s.sendContent(b);
    if(savedThisBoot){snprintf(b,sizeof(b),"%lu",(unsigned long)(millis()-savedAt));s.sendContent(b);}else s.sendContent("null");
    snprintf(b,sizeof(b),",\"boot_reset_reason\":%u,\"report_reset_reason\":%lu,\"session_id\":%lu,\"has_session\":%s,\"active\":%s,\"partial\":%s,\"start_ms\":%lu,\"end_ms\":%lu,\"captured_ms\":%lu,\"end_reason\":%u,\"events_omitted\":%lu,\"stats\":{",
        unsigned(esp_reset_reason()),(unsigned long)report.resetReason,(unsigned long)report.sessionID,boolean(report.hasSession),boolean(report.active),boolean(report.partial),
        (unsigned long)report.startMs,(unsigned long)report.endMs,(unsigned long)report.capturedMs,report.endReason,
        (unsigned long)(saved?report.omitted:report.stats[EVENTS_OMITTED]));s.sendContent(b);
    for(unsigned i=0;i<COUNT;++i){snprintf(b,sizeof(b),"%s\"%s\":%lu",i?",":"",statsNames[i],(unsigned long)report.stats[i]);s.sendContent(b);}
    s.sendContent("},\"rssi_ranges\":[");
    for(unsigned i=0;i<3;++i){snprintf(b,sizeof(b),"%s{\"source\":\"%s\",\"min_dbm\":",i?",":"",i==0?"bike":i==1?"polar":"wifi");s.sendContent(b);
        if(report.rssiMin[i]){snprintf(b,sizeof(b),"%d,\"max_dbm\":%d}",report.rssiMin[i],report.rssiMax[i]);s.sendContent(b);}else s.sendContent("null,\"max_dbm\":null}");}
    s.sendContent("],\"events\":[");size_t emitted=0;
    for(size_t i=0;i<eventCount;++i){Event e;bool valid=true;
        if(saved)e=report.events[i];else {Guard g;const uint32_t wanted=endSequence-uint32_t(eventCount-1-i),delta=recorder.sequence-wanted;
            valid=delta<recorder.eventCount;if(valid)e=recorder.events[(recorder.eventHead+EVENT_LIMIT-1-delta)%EVENT_LIMIT];}
        if(valid){if(emitted++)s.sendContent(",");jsonEvent(s,e);}}
    snprintf(b,sizeof(b),"],\"export_events_lost\":%u,\"health\":[",unsigned(eventCount-emitted));s.sendContent(b);
    // Only the application loop writes health records; this HTTP handler runs on that same loop.
    for(size_t i=0;i<healthCount;++i){Health h;{Guard g;h=recorder.healthAt(i);}
        snprintf(b,sizeof(b),"%s{\"at_ms\":%lu,\"heap_bytes\":%lu,\"largest_block_bytes\":%lu,\"loop_gap_us\":%lu,\"relay_gap_us\":%lu,\"bike_rssi\":%d,\"polar_rssi\":%d,\"wifi_rssi\":%d}",
            i?",":"",(unsigned long)h.at,(unsigned long)h.heap,(unsigned long)h.largest,(unsigned long)h.loopUs,(unsigned long)h.relayUs,h.bikeRSSI,h.hrRSSI,h.wifiRSSI);s.sendContent(b);}
    s.sendContent("],\"rejected_bike_samples\":[");
    for(unsigned i=0;i<report.rejectedCount;++i){const auto& v=report.rejected[i];snprintf(b,sizeof(b),"%s{\"at_ms\":%lu,\"length\":%u,\"prefix_hex\":\"",i?",":"",(unsigned long)v.at,v.length);s.sendContent(b);
        for(unsigned j=0;j<std::min(unsigned(v.length),24u);++j){snprintf(b,sizeof(b),"%02x",v.bytes[j]);s.sendContent(b);}s.sendContent("\"}");}
    s.sendContent("],\"notes\":\"Times are milliseconds since boot; RSSI 0 means unavailable. Gaps are missing valid measurements, not proven lost radio packets. TX accepted means local stack only. Event a/b/c/d meanings: disconnect=raw reason/intent; link_params=interval units 1.25ms/timeout units 10ms/latency/MTU; gap=reception interval ms/connected; flash=operation/duration us/success; task_delay=loop(0) or relay(1)/gap us. For bike/Polar gap/disconnect/failure events c/d are cached RSSI dBm and its age ms (0 RSSI unavailable). Context flag bits: 1 scanning, 2 connecting. Scan time is a worker estimate. Reason codes retain NimBLE category; 0x208 is timeout, 0x213 remote termination, 0x216 local termination. Saved reports omit RAM health timeline.\"}");s.sendContent("");
}
}
void begin(){
    opened=prefs.begin("airbike-diag",false);settingOK=opened;
    uint8_t value=opened?prefs.getUChar("enabled",1):1;if(value>1){value=1;settingOK=false;}
    enabled.store(value);recorder.enabled=value;
    recorder.boot(millis(),esp_reset_reason(),DIAGNOSTIC_BUILD);
    savedAvailable=readSaved();saveStatus=savedAvailable?"saved_previous_boot":opened&&prefs.getBytesLength("report-v1")?"invalid_saved_report":"not_saved";
    WiFi.onEvent([](arduino_event_id_t e,arduino_event_info_t info){
        if(e==ARDUINO_EVENT_WIFI_STA_DISCONNECTED)record(DISCONNECT,WIFI,info.wifi_sta_disconnected.reason);
        else if(e==ARDUINO_EVENT_WIFI_STA_GOT_IP)record(CONNECT,WIFI);
    });
}
bool isEnabled(){return enabled.load(std::memory_order_relaxed);}
void record(Kind k,Source s,int32_t a,int32_t b,int32_t c,int32_t d){if(!isEnabled())return;const auto now=millis();Guard g;recorder.event(now,k,s,a,b,c,d);}
void connection(Source s,bool up,int reason,Intent intent){if(!isEnabled())return;const auto now=millis();Guard g;recorder.connection(now,s,up,reason,intent);}
void packet(Source s,bool valid,const uint8_t* bytes,size_t length){if(!isEnabled())return;const auto now=millis();Guard g;recorder.packet(now,s,valid,bytes,length);}
void count(Stat which,uint32_t n){if(!isEnabled())return;Guard g;if(recorder.collecting())recorder.stats[which]+=n;}
void maximum(Stat which,uint32_t n){if(!isEnabled())return;Guard g;if(recorder.collecting())recorder.stats[which]=std::max(recorder.stats[which],n);}
void minimum(Stat which,uint32_t n){if(!isEnabled())return;Guard g;if(recorder.collecting()&&(!recorder.stats[which]||n<recorder.stats[which]))recorder.stats[which]=n;}
void rssi(Source s,int value){if(!isEnabled())return;Guard g;const unsigned i=s==POLAR;
    recorder.signal[i]=value<0?value:0;recorder.signalAt[i]=millis();
    if(s==BIKE){bikeRSSI=recorder.signal[i];bikeRSSIAt=recorder.signalAt[i];}else {hrRSSI=recorder.signal[i];hrRSSIAt=recorder.signalAt[i];}}
void pulse(bool relay){
    if(!isEnabled())return;const uint64_t now=esp_timer_get_time();const unsigned i=relay;const uint32_t ms=millis();Guard g;
    if(lastPulse[i]&&recorder.collecting()){
        const uint32_t elapsed=uint32_t(std::min(now-lastPulse[i],uint64_t(UINT32_MAX)));
        auto& high=recorder.stats[relay?RELAY_MAX_US:LOOP_MAX_US];high=std::max(high,elapsed);
        if(elapsed>(relay?500000u:250000u)&&(!lastDelay[i]||uint32_t(ms-lastDelay[i])>=10000)){recorder.event(ms,TASK_DELAY,SYSTEM,i,elapsed);lastDelay[i]=ms;}
    }lastPulse[i]=now;
}
void scan(bool scanning,uint32_t elapsed){if(!isEnabled())return;Guard g;recorder.contextFlags=(recorder.contextFlags&~1u)|(scanning?1:0);if(scanning&&recorder.collecting())recorder.stats[SCAN_MS]+=elapsed;}
Attempt::Attempt(Source s):source(s),started(millis()){{Guard g;recorder.contextFlags=(recorder.contextFlags&~1u)|2u;}record(CONNECT_START,s);}
Attempt::~Attempt(){const auto elapsed=uint32_t(millis()-started);maximum(CONNECT_MAX_MS,elapsed);record(CONNECT_DONE,source,elapsed);Guard g;recorder.contextFlags&=~2u;}
void flash(uint8_t op,uint32_t us,bool ok){maximum(FLASH_MAX_US,us);if(!ok)count(FLASH_FAILURES);record(FLASH_WRITE,STORAGE,op,us,ok);}
void tick(bool pedalling,uint32_t rider,int console,uint8_t rideState){
    const uint32_t now=millis();if(uint32_t(now-lastPoll)<100)return;lastPoll=now;
    if(isEnabled()){
        {Guard g;recorder.activity(now,pedalling,rider);recorder.poll(now);
            if(console!=lastConsole){recorder.event(now,CONSOLE,BIKE,console);lastConsole=console;}
            if(rideState!=lastRide){recorder.event(now,RIDE_STATE,SYSTEM,rideState);lastRide=rideState;}}
        if(uint32_t(now-lastHealth)>=10000){
            lastHealth=now;Health h;h.at=now;h.heap=ESP.getFreeHeap();h.largest=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            h.wifiRSSI=WiFi.status()==WL_CONNECTED?WiFi.RSSI():0;
            minimum(BOOT_HEAP_MIN,ESP.getMinFreeHeap());
            minimum(LOOP_STACK_MIN,uxTaskGetStackHighWaterMark(nullptr));
            Guard g;h.bikeRSSI=uint32_t(now-bikeRSSIAt)<=20000?bikeRSSI:0;h.hrRSSI=uint32_t(now-hrRSSIAt)<=20000?hrRSSI:0;
            h.loopUs=recorder.stats[LOOP_MAX_US];h.relayUs=recorder.stats[RELAY_MAX_US];recorder.health(h);
        }
    }
    persist(now);
}
void finishCapture(){{Guard g;recorder.finish(millis(),FINISHED);}persist(millis());}
void configureHTTP(WebServer& s,bool(*allow)(const char*)){
    authorize=allow;
    s.on("/api/diagnostics",HTTP_GET,[&s]{stream(s,false,false);});
    s.on("/api/diagnostics/export",HTTP_GET,[&s]{const String which=s.arg("source");if(which!="current"&&which!="saved"){s.send(400,"text/plain","Choose current or saved report");return;}stream(s,which=="saved",true);});
    s.on("/api/diagnostics/settings",HTTP_POST,[&s]{
        if(!authorize("diagnostics"))return;const String value=s.arg("enabled");if(value!="0"&&value!="1"){s.send(400,"text/plain","Use enabled=0 or enabled=1");return;}
        const bool desired=value=="1";
        if(desired==isEnabled()&&settingOK){stream(s,false,false);return;}
        const uint64_t start=esp_timer_get_time();
        settingOK=opened&&prefs.putUChar("enabled",desired)==1&&prefs.getUChar("enabled",2)==desired;
        flash(4,esp_timer_get_time()-start,settingOK);
        if(!settingOK){s.send(503,"text/plain","Could not verify diagnostic preference; current setting retained");return;}
        {Guard g;recorder.setEnabled(desired,millis());lastPulse[0]=lastPulse[1]=0;}
        enabled.store(desired);persist(millis());stream(s,false,false);
    });
}
}
