#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <errno.h>
#include "ride_session.h"
#include "polar_hr.h"
#include "band_selection.h"
#include <ESPmDNS.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "airbike_protocol.h"
#include "garmin_relay.h"
#include "ride_history.h"
#include "setup_window.h"
#include "wifi_credentials.h"
#include "live_trend.h"
#include "history_delete.h"
#include "diagnostics.h"
#include <esp_timer.h>
#include "setup_page.h"
#include "web_page.h"

namespace {

constexpr uint8_t BUTTON_PIN = 39;
constexpr char HOSTNAME[] = "airbike-atom";
constexpr char SETUP_ORIGIN[] = "http://192.168.77.1";
constexpr char UUID_BASE[] = "-0a7d-4ab3-97fa-f1500f9feb8b";
constexpr uint32_t SCAN_MS = 5000;

struct SharedState {
    bool connected = false;
    bool hasData = false;
    bool targetPending = false;
    bool retryRequested = false;
    bool unsupported = false;
    uint8_t addressType = 0;
    int16_t consoleState = -1;
    uint32_t dataMs = 0;
    uint32_t notifications = 0;
    uint32_t dataPackets = 0;
    uint32_t rejectedPackets = 0;
    char status[24] = "starting";
    char detail[160] = "Starting Bluetooth";
    char bikeName[64] = "";
    char bikeAddress[18] = "";
    char programme[48] = "";
    char dataHex[137] = "";
    char lastNotification[160] = "";
    airbike::Metrics metrics;
};

SharedState state;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
WebServer server(80);
Preferences preferences;
Preferences ridePreferences;
session::Tracker rideTracker;
trend::Buffer liveTrend;
session::History historyCandidate;
app::Settings appSettings;
bands::Selection bandSelection;
Preferences appPreferences;
bool settingsOpened=false, settingsStorageOK=true, historyDirty=false;
uint8_t historyBytes[session::HISTORY_BYTES];
uint8_t settingsBytes[app::SETTINGS_BYTES];
bool historyStorageOK = true;
bool historyLoadedOK = true;
uint32_t historyRetryMs = 0;
NimBLEClient* bikeClient = nullptr;
std::atomic<uint8_t> bikeDisconnectIntent{diagnostic::UNEXPECTED};
bool setupAP = false;
bool mdnsStarted = false;
bool joining = false;
bool pendingJoin = false;
bool storeOnSuccess = false;
bool wifiCredentialsStored = false;
bool manualSetup = false;
std::atomic<bool> stationGotIP{false};
SetupWindow setupWindow;
const char* setupPhase = "idle";
String joiningSSID;
String joiningPassword;
String setupName;
uint32_t wifiAttemptMs = 0;
uint32_t queuedJoinMs = 0;
uint32_t buttonDownMs = 0;
bool buttonHandled = false;

SharedState snapshot() {
    portENTER_CRITICAL(&stateMux);
    SharedState copy = state;
    portEXIT_CRITICAL(&stateMux);
    return copy;
}

void setStatus(const char* status, const char* detail) {
    portENTER_CRITICAL(&stateMux);
    snprintf(state.status, sizeof(state.status), "%s", status);
    snprintf(state.detail, sizeof(state.detail), "%s", detail);
    portEXIT_CRITICAL(&stateMux);
    Serial.printf("BLE: %s — %s\n", status, detail);
}

String uuid(const char* shortID) {
    return String("a026") + shortID + UUID_BASE;
}

String jsonString(const char* value) {
    String result = "\"";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        if (*p == '"' || *p == '\\') {
            result += '\\';
            result += static_cast<char>(*p);
        } else if (*p < 0x20) {
            char escaped[7];
            snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
            result += escaped;
        } else {
            result += static_cast<char>(*p);
        }
    }
    return result + '"';
}

void makeHex(const uint8_t* bytes, size_t length, char* out, size_t capacity) {
    constexpr char digits[] = "0123456789abcdef";
    size_t used = 0;
    for (size_t i = 0; i < length && used + 3 < capacity; ++i) {
        out[used++] = digits[bytes[i] >> 4];
        out[used++] = digits[bytes[i] & 0xf];
        if (i + 1 < length) out[used++] = ' ';
    }
    out[used] = '\0';
}

void onNotify(NimBLERemoteCharacteristic* characteristic, uint8_t* bytes, size_t length, bool) {
    const std::string id = characteristic->getUUID().toString();
    char hex[137];
    makeHex(bytes, length, hex, sizeof(hex));
    const bool data = id == uuid("e01d").c_str();
    const bool console = id == uuid("e01e").c_str();
    const bool programme = id == uuid("e01b").c_str();
    airbike::Metrics metrics;
    const bool decoded = data && airbike::decode(bytes, length, metrics);
    const uint32_t now = millis();

    portENTER_CRITICAL(&stateMux);
    ++state.notifications;
    snprintf(state.lastNotification, sizeof(state.lastNotification), "%.4s: %s", id.c_str() + 4, hex);
    if (console && length) {
        state.consoleState = bytes[0];
    }
    if (programme) {
        const size_t count = std::min(length, sizeof(state.programme) - 1);
        for (size_t i = 0; i < count; ++i) {
            state.programme[i] = bytes[i] >= 32 && bytes[i] < 127 ? static_cast<char>(bytes[i]) : ' ';
        }
        state.programme[count] = '\0';
    }
    if (data) {
        snprintf(state.dataHex, sizeof(state.dataHex), "%s", hex);
        if (decoded) {
            state.metrics = metrics;
            state.hasData = true;
            state.dataMs = now;
            ++state.dataPackets;
        } else {
            ++state.rejectedPackets;
        }
    }
    portEXIT_CRITICAL(&stateMux);
    diagnostic::count(diagnostic::BIKE_NOTIFICATIONS);
    if(data)diagnostic::packet(diagnostic::BIKE,decoded,bytes,length);
}

class BikeCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* client) override {
        diagnostic::connection(diagnostic::BIKE,true);
        if(diagnostic::isEnabled()){const auto info=client->getConnInfo();diagnostic::record(diagnostic::LINK_PARAMS,diagnostic::BIKE,info.getConnInterval(),info.getConnTimeout(),info.getConnLatency(),info.getMTU());}
    }
    void onDisconnect(NimBLEClient*, int reason) override {
        portENTER_CRITICAL(&stateMux);
        state.connected = false;
        portEXIT_CRITICAL(&stateMux);
        diagnostic::connection(diagnostic::BIKE,false,reason,static_cast<diagnostic::Intent>(bikeDisconnectIntent.exchange(diagnostic::UNEXPECTED)));
    }
} bikeCallbacks;

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* device) override {
        hr::onAdvertised(device);
        std::string name = device->getName();
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return tolower(c); });
        if (lower.find("assault airbike") == std::string::npos &&
            !device->isAdvertisingService(NimBLEUUID(uuid("ee07").c_str()))) {
            return;
        }
        if (!device->isConnectable()) return;
        const auto address = device->getAddress();
        const std::string addressText = address.toString();
        portENTER_CRITICAL(&stateMux);
        bool queued=false;
        if (!state.connected && !state.unsupported && !state.targetPending) {
            queued=true;
            snprintf(state.bikeName, sizeof(state.bikeName), "%s", name.empty() ? "Assault AirBike" : name.c_str());
            snprintf(state.bikeAddress, sizeof(state.bikeAddress), "%s", addressText.c_str());
            state.addressType = address.getType();
            state.targetPending = true;
        }
        portEXIT_CRITICAL(&stateMux);
        if(queued)NimBLEDevice::getScan()->stop();
    }
} scanCallbacks;

bool connectBike(const SharedState& target) {
    diagnostic::Attempt diagnosticAttempt(diagnostic::BIKE);
    setStatus("connecting", "Bike detected; connecting over Bluetooth");
    if (!bikeClient) {
        bikeClient = NimBLEDevice::createClient();
        if (!bikeClient) {
            diagnostic::count(diagnostic::CONNECT_FAILURES);diagnostic::record(diagnostic::CONNECT_FAILED,diagnostic::BIKE,-1);
            setStatus("retrying", "Could not allocate a Bluetooth client");
            return false;
        }
        bikeClient->setClientCallbacks(&bikeCallbacks, false);
        bikeClient->setConnectTimeout(8000);
    }
    portENTER_CRITICAL(&stateMux);
    state.hasData = false;
    state.consoleState = -1;
    state.programme[0] = '\0';
    state.dataHex[0] = '\0';
    state.lastNotification[0] = '\0';
    portEXIT_CRITICAL(&stateMux);
    if (!bikeClient->connect(NimBLEAddress(target.bikeAddress, target.addressType))) {
        diagnostic::count(diagnostic::CONNECT_FAILURES);diagnostic::record(diagnostic::CONNECT_FAILED,diagnostic::BIKE,bikeClient->getLastError());
        setStatus("retrying", "Bluetooth connection failed; will scan again");
        return false;
    }
    setStatus("subscribing", "Connected; subscribing to the console's notifications");
    struct Subscription { const char* service; const char* characteristic; };
    // Match the successful subscription order documented by the original decoder.
    const Subscription subscriptions[] = {
        {"ee01", "e002"}, {"ee01", "e004"}, {"ee03", "e00a"},
        {"ee07", "e01f"}, {"ee07", "e01e"}, {"ee07", "e020"},
        {"ee07", "e01b"}, {"ee07", "e01d"}, {"ee07", "e024"},
        {"ee07", "e028"}
    };
    unsigned subscribed = 0;
    bool liveSubscribed = false;
    for (const auto& item : subscriptions) {
        if (!bikeClient->isConnected()) break;
        auto* service = bikeClient->getService(uuid(item.service).c_str());
        if (!service) {diagnostic::record(diagnostic::SUBSCRIBE,diagnostic::BIKE,strtol(item.characteristic,nullptr,16),0,bikeClient->getLastError(),1);continue;}
        auto* characteristic = service->getCharacteristic(uuid(item.characteristic).c_str());
        if (!characteristic) {diagnostic::record(diagnostic::SUBSCRIBE,diagnostic::BIKE,strtol(item.characteristic,nullptr,16),0,bikeClient->getLastError(),2);continue;}
        bool ok = false;
        if (characteristic->canNotify()) ok = characteristic->subscribe(true, onNotify);
        else if (characteristic->canIndicate()) ok = characteristic->subscribe(false, onNotify);
        diagnostic::record(diagnostic::SUBSCRIBE,diagnostic::BIKE,strtol(item.characteristic,nullptr,16),ok,ok?0:bikeClient->getLastError());
        if (ok) {
            ++subscribed;
            if (strcmp(item.characteristic, "e01d") == 0) liveSubscribed = true;
        }
    }
    if (!bikeClient->isConnected()) {
        setStatus("retrying", "The bike disconnected during notification setup");
        return false;
    }
    if (!liveSubscribed) {
        bikeDisconnectIntent.store(diagnostic::SUBSCRIPTION_FAILED);
        bikeClient->disconnect();
        portENTER_CRITICAL(&stateMux);
        state.unsupported = true;
        state.connected = false;
        portEXIT_CRITICAL(&stateMux);
        setStatus("unsupported", "Expected E01D live-data notifications unavailable. Automatic attempts paused; use Try connecting again.");
        return false;
    }
    portENTER_CRITICAL(&stateMux);
    state.connected = true;
    portEXIT_CRITICAL(&stateMux);
    char detail[140];
    snprintf(detail, sizeof(detail), "%u notification subscriptions enabled, including E01D workout data", subscribed);
    setStatus("connected", detail);
    return true;
}

void relayTask(void*) {
    uint32_t lastHealth=0;
    for (;;) {
        diagnostic::pulse(true);
        if(diagnostic::isEnabled()&&uint32_t(millis()-lastHealth)>=10000){lastHealth=millis();diagnostic::minimum(diagnostic::RELAY_STACK_MIN,uxTaskGetStackHighWaterMark(nullptr));}
        const auto copy=snapshot();
        relay::update(airbike::isFresh(copy.connected,copy.hasData,millis(),copy.dataMs),
                      copy.metrics,copy.consoleState==0x80||copy.consoleState==0x01);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void bluetoothTask(void*) {
    if (!NimBLEDevice::init("AirBike ATOM receiver")) {
        setStatus("error", "Could not initialise Bluetooth");
        vTaskDelete(nullptr);
        return;
    }
    relay::begin();
    if(xTaskCreate(relayTask,"garmin-output",4096,nullptr,2,nullptr)!=pdPASS){
        setStatus("error","Could not start independent Garmin relay task");vTaskDelete(nullptr);return;
    }
    auto* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCallbacks, false);
    scan->setActiveScan(true);
    // Keep airtime available for Wi-Fi and avoid retaining nearby-device records.
    scan->setInterval(100);
    scan->setWindow(40);
    scan->setMaxResults(0);
    uint32_t diagnosticHealth=0,scanTick=millis();bool wasScanning=false;
    for (;;) {
        const uint32_t now=millis();const bool scanning=scan->isScanning();
        diagnostic::scan(scanning,wasScanning&&scanning?std::min(uint32_t(now-scanTick),1000u):0);
        wasScanning=scanning;scanTick=now;
        if(diagnostic::isEnabled()&&uint32_t(now-diagnosticHealth)>=10000){
            diagnosticHealth=now;diagnostic::minimum(diagnostic::BLE_STACK_MIN,uxTaskGetStackHighWaterMark(nullptr));
            if(bikeClient&&bikeClient->isConnected())diagnostic::rssi(diagnostic::BIKE,bikeClient->getRssi());else diagnostic::rssi(diagnostic::BIKE,0);
        }
        SharedState copy = snapshot();
        if (copy.retryRequested) {
            portENTER_CRITICAL(&stateMux);
            state.retryRequested = false;
            state.unsupported = false;
            portEXIT_CRITICAL(&stateMux);
            copy.unsupported = false;
        }
        // H10 discovery may block this connection worker, never the relay task.
        if (!copy.targetPending) hr::poll();
        if (copy.unsupported) {
            if(hr::needsScan()&&!scan->isScanning())scan->start(SCAN_MS);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (copy.targetPending) {
            portENTER_CRITICAL(&stateMux);
            state.targetPending = false;
            portEXIT_CRITICAL(&stateMux);
            if (!connectBike(copy)) vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        const bool bikeConnected=bikeClient&&bikeClient->isConnected();
        if ((!bikeConnected||hr::needsScan())&&!scan->isScanning()) {
            if(!bikeConnected)setStatus("scanning", "Waiting for the bike's Bluetooth/Connect button");
            scan->start(SCAN_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void sendJSON(const String& body) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", body);
}

bool persistHistory(bool force=false) {
    if(!rideTracker.pending&&!historyDirty)return historyStorageOK&&historyLoadedOK;
    if(rideTracker.pending&&!rideTracker.nonempty()){rideTracker.commit(millis());return true;}
    if(!historyLoadedOK)return false; // Never overwrite unvalidated saved history.
    if(!force&&!historyStorageOK&&uint32_t(millis()-historyRetryMs)<30000)return false;
    historyRetryMs=millis();
    const auto* pending=rideTracker.pending?&rideTracker.current:nullptr;
    session::encode(rideTracker.history,historyBytes,pending);
    const uint32_t expected=rides::checksum(historyBytes,sizeof(historyBytes)-4);
    const uint64_t diagnosticFlashStart=esp_timer_get_time();
    historyStorageOK=ridePreferences.putBytes("history-v2",historyBytes,sizeof(historyBytes))==sizeof(historyBytes);
    if(historyStorageOK){
        historyStorageOK=ridePreferences.getBytes("history-v2",historyBytes,sizeof(historyBytes))==sizeof(historyBytes)&&
            rides::readLE(historyBytes+sizeof(historyBytes)-4,4)==expected&&rides::checksum(historyBytes,sizeof(historyBytes)-4)==expected;
    }
    diagnostic::flash(1,esp_timer_get_time()-diagnosticFlashStart,historyStorageOK);
    if(historyStorageOK){if(rideTracker.pending)rideTracker.commit(millis());historyDirty=false;}
    return historyStorageOK;
}

app::Rider chosenRider(){return app::selectedBandRider(appSettings,rideTracker.hasRide()?rideTracker.current.riderID:bandSelection.riderID);}
void syncBandSelection(){
    const auto h=hr::snapshot();bool present[app::MAX_RIDERS]={};const uint32_t now=millis();
    for(size_t i=0;i<appSettings.count;++i){const auto& r=appSettings.riders[i];
        present[i]=h.sample.connected&&app::sameBand(r.bandAddress,r.bandType,h.address,h.type);
        for(size_t j=0;j<h.count&&!present[i];++j)present[i]=uint32_t(now-h.devices[j].seenMs)<bands::PRESENT_MS&&app::sameBand(r.bandAddress,r.bandType,h.devices[j].address,h.devices[j].type);
    }
    const bool before=bandSelection.ready();const uint32_t oldID=bandSelection.riderID;
    bandSelection.update(appSettings,present,now,rideTracker.hasRide(),rideTracker.current.riderID);
    if(!rideTracker.hasRide()&&(!before||oldID!=bandSelection.riderID))rideTracker.resetDetectors();
    hr::monitor(appSettings.autoRider&&!rideTracker.hasRide());
    const auto r=chosenRider();
    if(bandSelection.ready())hr::configure(r.bandAddress,r.bandType,r.bandName);
    else hr::configure("",0,"");
}

void updateDashboardRide() {
    const auto copy=snapshot();session::Input in;
    in.connected=copy.connected;in.hasData=copy.hasData;in.sequence=copy.dataPackets;in.packetMs=copy.dataMs;
    in.finished=copy.consoleState==0x80||copy.consoleState==0x01;in.metrics=copy.metrics;
    syncBandSelection();const auto rider=chosenRider();const auto h=hr::snapshot();auto sample=h.sample;
    if(!bandSelection.ready()||!app::sameBand(rider.bandAddress,rider.bandType,h.address,h.type))sample.connected=false;
    rideTracker.tick(in,sample,appSettings,millis(),&rider,bandSelection.ready());
    persistHistory();
    syncBandSelection();
    const auto selected=chosenRider();const uint32_t now=millis();
    liveTrend.select(selected.id,bandSelection.ready());
    const bool correct=app::sameBand(selected.bandAddress,selected.bandType,h.address,h.type);
    liveTrend.sample(now,correct&&sample.valid(now),sample.bpm,in.fresh(now),in.finished?0:in.metrics.power_w);
    diagnostic::tick(in.fresh(now)&&!in.finished&&(in.metrics.power_w>0||in.metrics.cadence_rpm>0),bandSelection.ready()?selected.id:UINT32_MAX,copy.consoleState,rideTracker.state);
}

String nullable(unsigned value){return value?String(value):String("null");}
String riderJSON(const app::Rider& r){
    return String("{\"id\":")+String(r.id)+",\"name\":"+jsonString(r.name)+",\"sex\":"+String(r.sex)+
        ",\"age\":"+nullable(r.age)+",\"weight_kg\":"+(r.weightDeciKg?String(r.weightDeciKg/10.0f,1):String("null"))+
        ",\"max_hr\":"+nullable(r.maxHR)+",\"band_address\":"+jsonString(r.bandAddress)+",\"band_type\":"+String(r.bandType)+",\"band_name\":"+jsonString(r.bandName)+"}";
}
String rideJSON(const session::Record& r) {
    String body;body.reserve(400);
    body="{\"id\":"+String(r.id)+",\"duration_s\":"+String(static_cast<unsigned long>(r.durationMs/1000));
    body+=",\"distance_m\":"+String(r.distanceM)+",\"calories_kcal\":"+String(r.calories);
    body+=",\"max_power_w\":"+String(r.maxPower)+",\"max_speed_kph\":"+String(r.maxSpeed/100.0f,2);
    body+=",\"avg_power_w\":"+(r.bikeMs?String(double(r.powerSum)/r.bikeMs,1):String("null"));
    body+=",\"avg_speed_kph\":"+(r.bikeMs?String(double(r.speedSum)/r.bikeMs/100.0,2):String("null"));
    body+=",\"avg_hr_bpm\":"+(r.hrMs?String(double(r.hrSum)/r.hrMs,1):String("null"));
    body+=",\"max_hr_bpm\":"+nullable(r.maxHR)+",\"rider_id\":"+String(r.riderID)+",\"rider_name\":"+jsonString(r.riderName);
    body+=",\"partial\":"+String(r.partial?"true":"false")+",\"reason\":";
    const char* why=r.reason==session::DISCONNECTED?"Bike disconnected":r.reason==session::INACTIVE?"10 minutes without pedalling":
        r.reason==session::CONSOLE_RESET?"New console ride":r.reason==session::FINISHED?"Saved by rider":"Current ride";
    return body+jsonString(why)+'}';
}
String selectionJSON(){
    const auto rider=chosenRider();String out=String("{\"mode\":")+jsonString(appSettings.autoRider?"auto":"manual")+",\"status\":"+jsonString(bands::phaseName(bandSelection.phase));
    out+=",\"ready\":"+String(bandSelection.ready()?"true":"false")+",\"rider_id\":"+String(rider.id)+",\"rider_name\":"+jsonString(rider.name)+",\"candidates\":[";
    for(size_t i=0;i<bandSelection.count;++i){const auto* r=appSettings.find(bandSelection.candidates[i]);if(i)out+=',';out+="{\"id\":"+String(r->id)+",\"name\":"+jsonString(r->name)+"}";}
    return out+"]}";
}
void sendRides(){
    String body;body.reserve(9000);
    body=String("{\"active\":")+(rideTracker.hasRide()?"true":"false");
    body+=",\"state\":"+jsonString(rideTracker.state==session::READY?"ready":rideTracker.state==session::RECORDING?"recording":"stopped");
    body+=",\"stop_reason\":"+jsonString(rideTracker.manualStop?"manual":"auto");
    body+=",\"saving\":"+String(rideTracker.pending?"true":"false");
    body+=",\"can_start\":"+String(rideTracker.fresh(millis())&&bandSelection.ready()?"true":"false");
    body+=",\"storage_ok\":"+String(historyStorageOK&&historyLoadedOK?"true":"false");
    body+=",\"selected_rider\":"+riderJSON(chosenRider())+",\"selection\":"+selectionJSON();
    body+=",\"current\":"+rideJSON(rideTracker.current)+",\"calories_kcal\":"+String(rideTracker.current.calories)+",\"history\":[";
    for(size_t i=0;i<rideTracker.history.count;++i){if(i)body+=',';body+=rideJSON(rideTracker.history.records[i]);}
    sendJSON(body+"]}");
}

void sendTrend(){
    const uint32_t now=millis();const auto rider=chosenRider();
    server.sendHeader("Cache-Control","no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200,"application/json","");
    String chunk=String("{\"window_s\":1800,\"interval_s\":5,\"rider_id\":")+String(liveTrend.riderID)+
        ",\"ready\":"+String(liveTrend.resolved?"true":"false")+",\"max_hr\":"+(rider.maxHR?String(rider.maxHR):String("null"))+",\"points\":[";
    bool first=true;
    for(size_t i=0;i<liveTrend.count;++i){
        const auto& p=liveTrend.at(i);const uint32_t age=uint32_t(now-p.at);
        if(age>trend::WINDOW_MS)continue;
        if(!first)chunk+=',';first=false;
        chunk+=String("[")+String(age/1000)+','+(p.hrValid?String(p.hr):String("null"))+','+(p.powerValid?String(p.power):String("null"))+']';
        if(chunk.length()>1500){server.sendContent(chunk);chunk="";}
    }
    server.sendContent(chunk+"]}");server.sendContent("");
}

String hrJSON(const hr::Status& h,bool devices=false){
    const auto rider=chosenRider();const bool correct=bandSelection.ready()&&app::sameBand(rider.bandAddress,rider.bandType,h.address,h.type);
    const uint32_t now=millis();String out="{\"configured\":"+String(h.configured?"true":"false");
    out+=",\"connected\":"+String(h.sample.connected&&correct?"true":"false")+",\"bpm\":"+(correct&&h.sample.valid(now)?String(h.sample.bpm):String("null"));
    out+=",\"contact\":"+String(h.sample.contact?"true":"false")+",\"data_age_ms\":"+(h.sample.received?String(uint32_t(now-h.sample.at)):String("null"));
    out+=",\"name\":"+jsonString(h.name)+",\"address\":"+jsonString(h.address)+",\"detail\":"+jsonString(h.detail);
    out+=",\"rider_id\":"+String(rider.id)+",\"max_hr\":"+(rider.maxHR?String(rider.maxHR):String("null"))+
        ",\"rider_name\":"+jsonString(bandSelection.ready()?rider.name:"")+",\"selection\":"+selectionJSON();
    out+=",\"scanning\":"+String(h.scanning?"true":"false")+",\"rejected_packets\":"+String(h.rejected);
    if(devices){out+=",\"devices\":[";for(size_t i=0;i<h.count;++i){if(i)out+=',';out+="{\"name\":"+jsonString(h.devices[i].name)+",\"address\":"+jsonString(h.devices[i].address)+",\"type\":"+String(h.devices[i].type)+",\"rssi\":"+String(h.devices[i].rssi)+",\"available\":"+String(uint32_t(now-h.devices[i].seenMs)<bands::PRESENT_MS||app::sameBand(h.address,h.type,h.devices[i].address,h.devices[i].type)&&h.sample.connected?"true":"false")+"}";}out+=']';}
    return out+'}';
}

void sendLive() {
    const SharedState copy = snapshot();
    const auto bridge = relay::stats();
    const bool wifi = WiFi.status() == WL_CONNECTED;
    const auto& m = copy.metrics;
    String body;
    body.reserve(1900);
    body = "{\"connected\":";
    body += copy.connected ? "true" : "false";
    body += ",\"has_data\":";
    body += copy.hasData ? "true" : "false";
    body += ",\"status\":" + jsonString(copy.status);
    body += ",\"detail\":" + jsonString(copy.detail);
    body += ",\"bike_name\":" + jsonString(copy.bikeName);
    body += ",\"bike_address\":" + jsonString(copy.bikeAddress);
    body += ",\"programme\":" + jsonString(copy.programme);
    body += ",\"console_state\":" + (copy.consoleState < 0 ? String("null") : String(copy.consoleState));
    body += ",\"notifications\":" + String(copy.notifications);
    body += ",\"data_packets\":" + String(copy.dataPackets);
    body += ",\"rejected_packets\":" + String(copy.rejectedPackets);
    body += ",\"data_age_ms\":" + (copy.hasData ? String(static_cast<uint32_t>(millis() - copy.dataMs)) : String("null"));
    body += ",\"last_data_hex\":" + jsonString(copy.dataHex);
    body += ",\"last_notification\":" + jsonString(copy.lastNotification);
    body += ",\"wifi_connected\":";
    body += wifi ? "true" : "false";
    body += ",\"wifi_rssi\":" + String(wifi ? WiFi.RSSI() : 0);
    body += ",\"ip\":" + jsonString(WiFi.localIP().toString().c_str());
    body += ",\"relay\":{\"name\":\"AirBike Bridge\",\"wheel_mm\":2000,\"ready\":";
    body += bridge.ready ? "true" : "false";
    body += ",\"advertising\":";
    body += bridge.advertising ? "true" : "false";
    body += ",\"output_connections\":" + String(bridge.outputConnections);
    body += ",\"advertising_failures\":" + String(bridge.advertisingFailures);
    body += ",\"power_includes_wheel\":true";
    body += ",\"connected\":";
    body += bridge.connected ? "true" : "false";
    body += ",\"power_subscribed\":";
    body += bridge.powerSubscribed ? "true" : "false";
    body += ",\"csc_subscribed\":";
    body += bridge.cscSubscribed ? "true" : "false";
    body += ",\"active\":";
    body += bridge.active ? "true" : "false";
    body += ",\"power_notifications\":" + String(bridge.powerNotifications);
    body += ",\"csc_notifications\":" + String(bridge.cscNotifications);
    body += ",\"send_failures\":" + String(bridge.sendFailures) + '}';
    body += ",\"hr\":" + hrJSON(hr::snapshot());
    body += ",\"metrics\":{\"power_w\":" + String(m.power_w);
    body += ",\"speed_kph\":" + String(m.speed_kph, 2);
    body += ",\"cadence_rpm\":" + String(m.cadence_rpm, 1);
    body += ",\"distance_m\":" + String(m.distance_m);
    body += ",\"calories_kcal\":" + String(m.calories_kcal);
    body += ",\"elapsed_s\":" + String(m.elapsed_s);
    body += ",\"remaining_s\":" + (m.has_remaining ? String(m.remaining_s) : String("null"));
    body += "}}";
    sendJSON(body);
}


bool fromSetupAP() {
    return setupAP && server.client().localIP() == WiFi.softAPIP();
}

void startSetupAP(bool manual = false) {
    manualSetup = manualSetup || manual;
    setupWindow.reopen();
    if (!joining && !pendingJoin) setupPhase = "idle";
    if (setupAP) return;
    WiFi.mode(WIFI_AP_STA);
    const IPAddress setupIP(192, 168, 77, 1);
    if (!WiFi.softAPConfig(setupIP, setupIP, IPAddress(255, 255, 255, 0))) {
        Serial.println("Could not configure Wi-Fi setup hotspot address");
        return;
    }
    // A temporary first-setup network. No router credentials are printed or served back.
    if (!WiFi.softAP(setupName.c_str())) {
        Serial.println("Could not start Wi-Fi setup hotspot");
        return;
    }
    setupAP = true;
    Serial.printf("Wi-Fi setup: join %s and open %s\n", setupName.c_str(), SETUP_ORIGIN);
}

bool allowAction(const char* action){
    const String origin=server.header("Origin"),local=String("http://")+server.client().localIP().toString();
    if(server.header("X-AirBike-Action")!=action||(origin!=local&&origin!=local+":80"&&origin!="http://airbike-atom.local"&&origin!="http://airbike-atom.local:80")){
        server.send(403,"text/plain","Use the ATOM's own dashboard");return false;
    }return true;
}
void apiError(int status,const char* message){server.sendHeader("Cache-Control","no-store");server.send(status,"text/plain; charset=utf-8",message);}
bool integer(const String& s,unsigned low,unsigned high,unsigned& out){
    if(!s.length()||s.length()>10)return false;uint64_t n=0;
    for(size_t i=0;i<s.length();++i){if(s[i]<'0'||s[i]>'9')return false;n=n*10+(s[i]-'0');if(n>high)return false;}
    if(n<low)return false;out=unsigned(n);return true;
}
bool optionalInteger(const char* key,unsigned high,uint16_t& out){
    String value=server.arg(key);value.trim();if(!value.length()){out=0;return true;}
    unsigned n;if(!integer(value,1,high,n))return false;out=n;return true;
}
bool saveSettings(const app::Settings& candidate){
    if(!settingsOpened||!app::valid(candidate))return false;
    app::encode(candidate,settingsBytes);const size_t size=sizeof(settingsBytes);
    const uint32_t expected=rides::checksum(settingsBytes,size-4);
    const uint64_t diagnosticFlashStart=esp_timer_get_time();
    settingsStorageOK=appPreferences.putBytes("settings-v2",settingsBytes,size)==size;
    if(settingsStorageOK)settingsStorageOK=appPreferences.getBytes("settings-v2",settingsBytes,size)==size&&
        rides::checksum(settingsBytes,size-4)==expected&&rides::readLE(settingsBytes+size-4,4)==expected;
    diagnostic::flash(2,esp_timer_get_time()-diagnosticFlashStart,settingsStorageOK);
    if(settingsStorageOK){appSettings=candidate;bandSelection.reset(millis());hr::watchKnown(appSettings);syncBandSelection();}
    return settingsStorageOK;
}
void sendSettings(){
    String body=String("{\"storage_ok\":")+(settingsStorageOK?"true":"false")+",\"auto_start\":"+(appSettings.autoStart?"true":"false")+
        ",\"auto_stop\":"+(appSettings.autoStop?"true":"false")+",\"start_seconds\":"+String(appSettings.startS)+",\"stop_seconds\":"+String(appSettings.stopS)+
        ",\"selected_rider\":"+riderJSON(chosenRider())+",\"rider_mode\":"+jsonString(appSettings.autoRider?"auto":"manual")+",\"hr_address\":"+jsonString(appSettings.hrAddress)+",\"hr_name\":"+jsonString(appSettings.hrName)+"}";
    sendJSON(body);
}
void sendRiders(uint32_t savedID=0){
    String body="{\"selected_id\":"+String(appSettings.selectedID)+",\"mode\":"+jsonString(appSettings.autoRider?"auto":"manual")+",\"selection\":"+selectionJSON()+",\"max_riders\":10,\"riders\":[";
    for(size_t i=0;i<appSettings.count;++i){if(i)body+=',';body+=riderJSON(appSettings.riders[i]);}
    body+=']';if(savedID)body+=",\"saved_id\":"+String(savedID);sendJSON(body+'}');
}
void configureRideHTTP(){
    server.on("/api/rides/delete",HTTP_POST,[]{
        if(!allowAction("delete-ride"))return;updateDashboardRide();
        unsigned id;
        if(!integer(server.arg("id"),1,0xffffffffu,id)){apiError(400,"Invalid ride ID");return;}
        if(rideTracker.pending){apiError(409,"Wait for the pending ride save before deleting history");return;}
        if(!historyLoadedOK){apiError(503,"Saved history could not be read; deletion is unavailable");return;}
        const uint64_t diagnosticFlashStart=esp_timer_get_time();
        const auto result=session::deleteSaved(ridePreferences,rideTracker.history,id,historyCandidate,historyBytes);
        diagnostic::flash(1,esp_timer_get_time()-diagnosticFlashStart,result==session::DeleteResult::SAVED);
        if(result==session::DeleteResult::NOT_FOUND){apiError(404,"This ride is no longer in history");return;}
        historyStorageOK=result==session::DeleteResult::SAVED;
        if(!historyStorageOK){apiError(503,"Could not verify deletion in flash. Refresh history before retrying.");return;}
        historyDirty=false;sendRides();
    });
    server.on("/api/rides/control",HTTP_POST,[]{
        if(!allowAction("ride-control"))return;updateDashboardRide();
        const String action=server.arg("action");const uint32_t now=millis();
        if(action=="finish"){
            if(!rideTracker.hasRide()){apiError(409,"There is no current ride to save");return;}
            rideTracker.finish(session::FINISHED,now);
            if(!persistHistory(true)){apiError(503,"Could not save to flash. The stopped ride is retained; retry Save & finish.");return;}
            diagnostic::finishCapture();
        }else if(rideTracker.pending){apiError(409,"Saving is pending. Retry Save & finish.");return;}
        else if(action=="start"&&rideTracker.state==session::READY){
            if(!rideTracker.fresh(now)){apiError(409,"Connect the bike and wait for fresh readings before starting");return;}
            if(!bandSelection.ready()){apiError(409,"Choose a rider before starting");return;}
            rideTracker.start(chosenRider(),now);
        }else if(action=="stop"&&rideTracker.state==session::RECORDING)rideTracker.stop(true,now);
        else if(action=="resume"&&rideTracker.state==session::STOPPED){
            if(!rideTracker.fresh(now)){apiError(409,"Wait for fresh bike readings before resuming");return;}
            rideTracker.resume(now);
        }else {apiError(409,"This action is not available in the current ride state");return;}
        sendRides();
    });
    server.on("/api/rides/clear",HTTP_POST,[]{
        if(!allowAction("clear-ride"))return;updateDashboardRide();
        if(!rideTracker.hasRide()){apiError(409,"There is no current dashboard ride to clear");return;}
        if(rideTracker.pending){apiError(409,"A save is pending; retry Save & finish before clearing");return;}
        rideTracker.clear(millis());sendJSON("{\"ok\":true}");
    });
    server.on("/api/settings",HTTP_GET,sendSettings);
    server.on("/api/settings",HTTP_POST,[]{
        if(!allowAction("settings"))return;updateDashboardRide();
        if(rideTracker.hasRide()){apiError(409,"Save or clear the current ride before changing automatic controls");return;}
        auto cfg=appSettings;unsigned start,stop;
        if((server.arg("auto_start")!="0"&&server.arg("auto_start")!="1")||(server.arg("auto_stop")!="0"&&server.arg("auto_stop")!="1")||
            !integer(server.arg("start_seconds"),1,600,start)||!integer(server.arg("stop_seconds"),1,600,stop)){
            apiError(400,"Use whole seconds from 1 to 600 and valid enable switches");return;}
        cfg.autoStart=server.arg("auto_start")=="1";cfg.autoStop=server.arg("auto_stop")=="1";cfg.startS=start;cfg.stopS=stop;
        if(!saveSettings(cfg)){apiError(503,"Could not save settings to flash; previous settings retained");return;}
        rideTracker.resetDetectors();sendSettings();
    });
    server.on("/api/riders",HTTP_GET,[]{sendRiders();});
    server.on("/api/riders",HTTP_POST,[]{
        if(!allowAction("riders"))return;auto cfg=appSettings;unsigned id=0,sex=0;
        if(server.hasArg("id")&&!integer(server.arg("id"),0,0xfffffffeu,id)){apiError(400,"Invalid rider ID");return;}
        size_t index=cfg.count;
        if(id){for(size_t i=0;i<cfg.count;++i)if(cfg.riders[i].id==id)index=i;if(index==cfg.count){apiError(404,"Rider not found");return;}}
        if(index==cfg.count&&cfg.count==app::MAX_RIDERS){apiError(409,"The ATOM stores up to 10 riders");return;}
        if(id&&rideTracker.hasRide()&&rideTracker.current.riderID==id){apiError(409,"Save or clear this rider's current ride before editing the profile");return;}
        app::Rider r=id?cfg.riders[index]:app::Rider{};r.id=id?id:cfg.nextID;String name=server.arg("name");name.trim();
        if(name.length()>=sizeof(r.name)){apiError(400,"Name must fit 48 UTF-8 bytes");return;}
        snprintf(r.name,sizeof(r.name),"%s",name.c_str());uint16_t age=0,maxHR=0;
        if(!app::validName(r.name)||!integer(server.arg("sex"),0,2,sex)||!optionalInteger("age",120,age)||!optionalInteger("max_hr",250,maxHR)||(maxHR&&maxHR<30)){
            apiError(400,"Check name, sex, age (1-120) and maximum HR (30-250); optional fields may be empty");return;}
        r.sex=sex;r.age=age;r.maxHR=maxHR;
        r.weightDeciKg=0;String weight=server.arg("weight_kg");weight.trim();
        if(weight.length()){
            char* end=nullptr;errno=0;double kg=strtod(weight.c_str(),&end);
            if(errno||!end||*end||!std::isfinite(kg)||kg<1||kg>500||fabs(kg*10-round(kg*10))>.001){apiError(400,"Weight must be 1-500 kg, with at most one decimal place");return;}
            r.weightDeciKg=std::lround(kg*10);
        }
        cfg.riders[index]=r;if(!id){++cfg.count;++cfg.nextID;}
        if(!saveSettings(cfg)){apiError(503,"Could not save rider; previous profiles retained");return;}sendRiders(r.id);
    });
    server.on("/api/riders/select",HTTP_POST,[]{
        if(!allowAction("rider-select"))return;updateDashboardRide();unsigned id;
        if(rideTracker.hasRide()){apiError(409,"Save or clear the current ride before switching riders");return;}
        auto cfg=appSettings;
        if(server.arg("id")=="auto"){cfg.autoRider=true;}
        else {if(!integer(server.arg("id"),0,0xfffffffeu,id)||(id&&!appSettings.find(id))){apiError(400,"Choose Auto, Guest or a saved rider");return;}cfg.autoRider=false;cfg.selectedID=id;}
        if(!saveSettings(cfg)){apiError(503,"Could not save rider selection");return;}sendRiders();
    });
    server.on("/api/riders/resolve",HTTP_POST,[]{
        if(!allowAction("rider-select"))return;updateDashboardRide();unsigned id;
        if(rideTracker.hasRide()){apiError(409,"The current ride's rider is locked");return;}
        if(!integer(server.arg("id"),1,0xfffffffeu,id)||!bandSelection.choose(appSettings,id)){apiError(400,"Choose a saved rider in Auto mode");return;}
        rideTracker.resetDetectors();syncBandSelection();sendRiders();
    });
    server.on("/api/hr/sensor",HTTP_GET,[]{sendJSON(hrJSON(hr::snapshot(),true));});
    server.on("/api/hr/sensor",HTTP_POST,[]{
        if(!allowAction("hr-sensor"))return;
        if(server.arg("action")=="scan"){hr::requestScan();sendJSON(hrJSON(hr::snapshot(),true));return;}
        if(rideTracker.hasRide()){apiError(409,"Finish or clear the current ride before changing band links");return;}
        unsigned riderID;
        if(!integer(server.arg("rider_id"),1,0xfffffffeu,riderID)||!appSettings.find(riderID)){apiError(400,"Choose a saved rider to link this band");return;}
        auto cfg=appSettings;app::Rider* rider=nullptr;for(size_t i=0;i<cfg.count;++i)if(cfg.riders[i].id==riderID)rider=&cfg.riders[i];
        if(server.arg("action")=="unlink"){
            // A migrated/shared identity must not silently reactivate for Guest.
            if(app::sameBand(cfg.hrAddress,cfg.hrType,rider->bandAddress,rider->bandType)){cfg.hrAddress[0]=0;cfg.hrName[0]=0;cfg.hrType=0;}
            rider->bandAddress[0]=0;rider->bandName[0]=0;rider->bandType=0;
        }else if(server.arg("action")=="select"){
            const auto found=hr::snapshot();unsigned type;
            if(!integer(server.arg("type"),0,1,type)){apiError(400,"Invalid Bluetooth address type");return;}
            const String address=server.arg("address");const hr::Device* device=nullptr;
            for(size_t i=0;i<found.count;++i)if(address==found.devices[i].address&&type==found.devices[i].type&&uint32_t(millis()-found.devices[i].seenMs)<bands::PRESENT_MS)device=&found.devices[i];
            const bool remembered=app::sameBand(cfg.hrAddress,cfg.hrType,address.c_str(),type);
            if(!device&&!remembered){apiError(409,"Scan and choose a recently discovered H10");return;}
            const auto* owner=app::ownerOf(cfg,address.c_str(),type);
            if(owner&&owner->id!=riderID){apiError(409,"That band is already linked to another rider; unlink it there first");return;}
            if(app::sameBand(cfg.hrAddress,cfg.hrType,rider->bandAddress,rider->bandType)&&!app::sameBand(cfg.hrAddress,cfg.hrType,address.c_str(),type)){cfg.hrAddress[0]=0;cfg.hrName[0]=0;cfg.hrType=0;}
            snprintf(rider->bandAddress,sizeof(rider->bandAddress),"%s",address.c_str());rider->bandType=type;
            snprintf(rider->bandName,sizeof(rider->bandName),"%s",device?device->name:cfg.hrName);
        }else {apiError(400,"Choose scan, select or unlink");return;}
        if(!saveSettings(cfg)){apiError(503,"Could not save the rider's band link");return;}sendJSON(hrJSON(hr::snapshot(),true));
    });
}

void configureHTTP() {
    server.on("/", HTTP_GET, [] {
        server.sendHeader("Cache-Control", "no-store");
        if (fromSetupAP()) server.send_P(200, "text/html; charset=utf-8", SETUP_PAGE);
        else server.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
    });
    diagnostic::configureHTTP(server,allowAction);
    server.on("/api/live", HTTP_GET, sendLive);
    server.on("/api/rides", HTTP_GET, sendRides);
    server.on("/api/trend", HTTP_GET, sendTrend);
    configureRideHTTP();
    server.on("/api/retry", HTTP_POST, [] {
        // No cross-origin control requests. The dashboard uses same-origin fetch.
        const String origin = server.header("Origin");
        const String expected = String("http://") + server.header("Host");
        if (origin.length() && origin != expected) {
            server.send(403, "text/plain", "Use the ATOM's own page");
            return;
        }
        portENTER_CRITICAL(&stateMux);
        state.retryRequested = true;
        portEXIT_CRITICAL(&stateMux);
        sendJSON("{\"ok\":true}");
    });
    server.on("/wifi", HTTP_POST, [] {
        if (!fromSetupAP()) {
            server.send(403, "text/plain", "Use the Wi-Fi setup hotspot");
            return;
        }
        const String origin = server.header("Origin");
        if (origin.length() && origin != SETUP_ORIGIN) {
            server.send(403, "text/plain", "Open http://192.168.77.1 directly");
            return;
        }
        String ssid = server.arg("ssid");
        String password = server.arg("password");
        wifi::Credentials candidate;
        if (!wifi::make(ssid.c_str(), password.c_str(), candidate) ||
            strlen(candidate.ssid) != ssid.length() || strlen(candidate.password) != password.length()) {
            server.send(400, "text/plain", "Check the Wi-Fi name and password length");
            return;
        }
        if (joining || pendingJoin) {
            server.send(409, "text/plain", "Please wait for the current connection attempt");
            return;
        }
        joiningSSID = ssid;
        joiningPassword = password;
        pendingJoin = true;
        queuedJoinMs = millis();
        setupPhase = "queued";
        setupWindow.reopen();
        storeOnSuccess = true;
        sendJSON("{\"ok\":true}");
    });
    server.on("/api/setup", HTTP_GET, [] {
        if (!fromSetupAP()) {
            server.send(403, "text/plain", "Use the Wi-Fi setup hotspot");
            return;
        }
        String body = "{\"connected\":";
        body += WiFi.status() == WL_CONNECTED && strcmp(setupPhase, "connected") == 0 ? "true" : "false";
        body += ",\"failed\":";
        body += strcmp(setupPhase, "failed") == 0 ? "true" : "false";
        body += ",\"phase\":" + jsonString(setupPhase);
        body += ",\"ip\":" + jsonString(WiFi.localIP().toString().c_str()) + '}';
        sendJSON(body);
    });
    server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
    const char* collected[] = {"Origin", "Host", "X-AirBike-Action"};
    server.collectHeaders(collected, 3);
    server.begin();
}

bool saveWiFiCredentials(const String& ssid, const String& password) {
    wifi::Credentials candidate;
    const uint64_t diagnosticFlashStart=esp_timer_get_time();
    const bool saved = wifi::make(ssid.c_str(), password.c_str(), candidate) && wifi::save(preferences, candidate);
    diagnostic::flash(3,esp_timer_get_time()-diagnosticFlashStart,saved);
    if (saved) wifiCredentialsStored = true;
    return saved;
}

void updateWiFi() {
    const uint32_t now = millis();
    // Let the HTTP acknowledgement reach the phone before switching Wi-Fi channels.
    if (pendingJoin && static_cast<uint32_t>(now - queuedJoinMs) >= 1000) {
        pendingJoin = false;
        stationGotIP.store(false);
        WiFi.disconnect(false, false);
        WiFi.begin(joiningSSID.c_str(), joiningPassword.c_str());
        wifiAttemptMs = now;
        joining = true;
        setupPhase = "joining";
        Serial.println("Wi-Fi: connection attempt started");
    }
    if (joining && static_cast<uint32_t>(now - wifiAttemptMs) > 30000) {
        joining = false;
        storeOnSuccess = false;
        joiningPassword = "";
        WiFi.disconnect(false, false);
        startSetupAP();
        setupPhase = "failed";
        Serial.println("Wi-Fi: connection attempt failed; saved credentials preserved");
    }
    if (WiFi.status() == WL_CONNECTED) {
        if (joining) {
            // A previous connection can still report WL_CONNECTED briefly after
            // disconnect(). Require a new GOT_IP event for the requested network.
            if (!stationGotIP.load() || WiFi.SSID() != joiningSSID) return;
            joining = false;
            if (storeOnSuccess) {
                storeOnSuccess = false;
                if (!saveWiFiCredentials(joiningSSID, joiningPassword)) {
                    setupWindow.reopen();
                    setupPhase = "save_failed";
                    joiningPassword = "";
                    Serial.println("Wi-Fi: connected, but credential storage could not be verified");
                    return;
                }
                setupWindow.connectionSaved(now);
            }
            setupPhase = "connected";
            joiningPassword = "";
            Serial.printf("Dashboard: http://%s/ or http://%s.local/\n", WiFi.localIP().toString().c_str(), HOSTNAME);
        }
        if (!mdnsStarted) {
            mdnsStarted = MDNS.begin(HOSTNAME);
            if (mdnsStarted) MDNS.addService("http", "tcp", 80);
        }
        if (setupAP) {
            if (setupWindow.shouldClose(now)) {
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_STA);
                setupAP = false;
                manualSetup = false;
            }
        }
    } else {
        if (!joining && !pendingJoin && !manualSetup && wifiCredentialsStored &&
            static_cast<uint32_t>(now - wifiAttemptMs) > 20000) {
            wifiAttemptMs = now;
            WiFi.reconnect();
        }
    }
}

void updateButton() {
    if (digitalRead(BUTTON_PIN) == LOW) {
        if (!buttonDownMs) buttonDownMs = millis();
        if (!buttonHandled && static_cast<uint32_t>(millis() - buttonDownMs) > 4000) {
            buttonHandled = true;
            startSetupAP(true);
        }
    } else {
        buttonDownMs = 0;
        buttonHandled = false;
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    pinMode(BUTTON_PIN, INPUT);
    delay(100);
    Serial.println("\nAirBike ATOM proof of concept");
    diagnostic::begin();
    preferences.begin("airbike-poc", false);
    historyStorageOK=ridePreferences.begin("airbike-rides",false);
    if(historyStorageOK){
        const size_t v2=ridePreferences.getBytesLength("history-v2");
        if(v2){historyLoadedOK=v2==session::HISTORY_BYTES&&ridePreferences.getBytes("history-v2",historyBytes,sizeof(historyBytes))==sizeof(historyBytes)&&session::decode(rideTracker.history,historyBytes,sizeof(historyBytes));}
        else {
            const size_t v1=ridePreferences.getBytesLength("history");
            if(v1){rides::Tracker legacy;uint8_t bytes[rides::STORAGE_BYTES];
                historyLoadedOK=v1==sizeof(bytes)&&ridePreferences.getBytes("history",bytes,sizeof(bytes))==sizeof(bytes)&&rides::decode(legacy,bytes,sizeof(bytes));
                if(historyLoadedOK)session::migrate(legacy,rideTracker.history);
            }
            historyDirty=historyLoadedOK; // Legacy key retained for recovery.
        }
    }
    settingsOpened=appPreferences.begin("airbike-app",false);settingsStorageOK=settingsOpened;
    if(settingsOpened){
        const size_t modern=appPreferences.getBytesLength("settings-v2"),legacy=appPreferences.getBytesLength("settings");
        const char* key=modern?"settings-v2":"settings";const size_t length=modern?modern:legacy;
        if(length){settingsStorageOK=length<=sizeof(settingsBytes)&&appPreferences.getBytes(key,settingsBytes,length)==length&&app::decode(appSettings,settingsBytes,length);
            if(settingsStorageOK&&!modern)saveSettings(appSettings);
        }
    }
    hr::watchKnown(appSettings);bandSelection.reset(millis());syncBandSelection();
    const uint64_t chipID = ESP.getEfuseMac();
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%06x", static_cast<unsigned>((chipID >> 24) & 0xffffff));
    setupName = String("AirBike-Setup-") + suffix;
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.onEvent([](arduino_event_id_t) {
        stationGotIP.store(true);
    }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    wifi::Credentials savedWiFi;
    wifiCredentialsStored = wifi::load(preferences, savedWiFi);
    if (wifiCredentialsStored) {
        joiningSSID = savedWiFi.ssid;
        joiningPassword = savedWiFi.password;
        pendingJoin = true;
        queuedJoinMs = millis();
    } else {
        startSetupAP();
    }
    configureHTTP();
    if (xTaskCreate(bluetoothTask, "airbike-ble", 10240, nullptr, 1, nullptr) != pdPASS) {
        setStatus("error", "Could not start the Bluetooth task");
    }
}

void loop() {
    diagnostic::pulse(false);
    server.handleClient();
    updateWiFi();
    updateButton();
    updateDashboardRide();
    delay(2);
}
