#include <Arduino.h>
#include <algorithm>
#include <string.h>
#include "polar_hr.h"
#include "diagnostics.h"
#include <atomic>
namespace hr {
namespace {
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
Status status;
struct Known {char address[18]="";uint8_t type=0;};Known known[app::MAX_RIDERS+1];size_t knownCount=0;
bool isKnown(const char* address,uint8_t type){for(size_t i=0;i<knownCount;++i)if(app::sameBand(known[i].address,known[i].type,address,type))return true;return false;}
uint8_t targetType=0;bool changed=false,pending=false;
uint32_t scanUntil=0,lastAttempt=0;bool attempted=false;
Device target;
NimBLEClient* client=nullptr;
std::atomic<uint8_t> disconnectIntent{diagnostic::UNEXPECTED};
uint32_t diagnosticHealth=0;int diagnosticContact=-1;
void detail(const char* value){portENTER_CRITICAL(&mux);snprintf(status.detail,sizeof(status.detail),"%s",value);portEXIT_CRITICAL(&mux);}
void notify(NimBLERemoteCharacteristic*,uint8_t* bytes,size_t n,bool){
    heart::Measurement m;const bool valid=heart::decode(bytes,n,m);portENTER_CRITICAL(&mux);
    if(valid&&status.sample.connected){status.sample.bpm=m.bpm;status.sample.contact=m.contact;status.sample.at=millis();status.sample.received=true;}
    else if(!valid) ++status.rejected;portEXIT_CRITICAL(&mux);
    diagnostic::packet(diagnostic::POLAR,valid);
    if(valid&&int(m.contact)!=diagnosticContact){diagnosticContact=m.contact;diagnostic::record(diagnostic::CONTACT,diagnostic::POLAR,m.contact,m.contactSupported);}
}
class Callbacks:public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* connected) override {diagnostic::connection(diagnostic::POLAR,true);if(diagnostic::isEnabled()){const auto info=connected->getConnInfo();diagnostic::record(diagnostic::LINK_PARAMS,diagnostic::POLAR,info.getConnInterval(),info.getConnTimeout(),info.getConnLatency(),info.getMTU());}}
    void onDisconnect(NimBLEClient*,int reason) override {portENTER_CRITICAL(&mux);status.sample.connected=false;status.sample.received=false;snprintf(status.detail,sizeof(status.detail),"H10 disconnected; waiting to reconnect");portEXIT_CRITICAL(&mux);diagnostic::connection(diagnostic::POLAR,false,reason,static_cast<diagnostic::Intent>(disconnectIntent.exchange(diagnostic::UNEXPECTED)));}
} callbacks;
}
Status snapshot(){portENTER_CRITICAL(&mux);Status s=status;portEXIT_CRITICAL(&mux);return s;}
void configure(const char* address,uint8_t type,const char* name){
    portENTER_CRITICAL(&mux);
    snprintf(status.name,sizeof(status.name),"%s",name);
    const bool sameBand=app::sameBand(status.address,targetType,address,type);
    if(!sameBand&&(status.address[0]||address[0]||targetType!=type)){
        snprintf(status.address,sizeof(status.address),"%s",address);snprintf(status.name,sizeof(status.name),"%s",name);
        targetType=type;status.type=type;changed=true;pending=false;status.configured=address[0];status.sample=heart::Sample{};
        snprintf(status.detail,sizeof(status.detail),"%s",address[0]?"Waiting for selected H10":"No Polar H10 selected");
    }
    portEXIT_CRITICAL(&mux);
}
void watchKnown(const app::Settings& cfg){
    portENTER_CRITICAL(&mux);knownCount=0;
    for(size_t i=0;i<cfg.count;++i)if(cfg.riders[i].bandAddress[0]){auto& k=known[knownCount++];memcpy(k.address,cfg.riders[i].bandAddress,18);k.type=cfg.riders[i].bandType;}
    if(cfg.hrAddress[0]&&!app::ownerOf(cfg,cfg.hrAddress,cfg.hrType)){auto& k=known[knownCount++];memcpy(k.address,cfg.hrAddress,18);k.type=cfg.hrType;}
    for(size_t i=0;i<status.count;++i)status.devices[i].known=isKnown(status.devices[i].address,status.devices[i].type);
    portEXIT_CRITICAL(&mux);
}
void monitor(bool enabled){portENTER_CRITICAL(&mux);status.monitoring=enabled;portEXIT_CRITICAL(&mux);}
void requestScan(){portENTER_CRITICAL(&mux);
    size_t kept=0;for(size_t i=0;i<status.count;++i)if(status.devices[i].known)status.devices[kept++]=status.devices[i];status.count=kept;
    scanUntil=millis()+15000;status.scanning=true;portEXIT_CRITICAL(&mux);
}
void onAdvertised(const NimBLEAdvertisedDevice* device){
    if(!device->isConnectable())return;
    std::string name=device->getName();std::string lower=name;
    std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return tolower(c);});
    const auto addr=device->getAddress();const auto address=addr.toString();
    portENTER_CRITICAL(&mux);
    const bool selected=status.configured&&app::sameBand(status.address,targetType,address.c_str(),addr.getType());
    const bool registered=isKnown(address.c_str(),addr.getType());
    if(lower.find("polar h10")==std::string::npos&&!selected&&!registered){portEXIT_CRITICAL(&mux);return;}
    Device found;snprintf(found.address,sizeof(found.address),"%s",address.c_str());snprintf(found.name,sizeof(found.name),"%s",name.empty()?"Polar H10":name.c_str());found.type=addr.getType();found.rssi=device->getRSSI();found.seenMs=millis();found.known=registered;
    // Keep registered-band presence even outside an explicit discovery request.
    size_t i=0;while(i<status.count&&!app::sameBand(status.devices[i].address,status.devices[i].type,found.address,found.type))++i;
    if(i==status.count&&i==MAX_DEVICES){
        size_t oldest=MAX_DEVICES;uint32_t age=0;
        for(size_t j=0;j<status.count;++j)if(!status.devices[j].known&&uint32_t(found.seenMs-status.devices[j].seenMs)>=age){age=found.seenMs-status.devices[j].seenMs;oldest=j;}
        i=oldest;
    }
    if(i<MAX_DEVICES){status.devices[i]=found;if(i==status.count)++status.count;}
    if(selected&&!status.sample.connected){target=found;pending=true;}
    portEXIT_CRITICAL(&mux);
}
bool needsScan(){auto s=snapshot();return s.monitoring||s.scanning||(s.configured&&!s.sample.connected);}
void poll(){
    Device selected;bool mustChange=false,connect=false;const uint32_t now=millis();
    portENTER_CRITICAL(&mux);
    if(status.scanning&&int32_t(now-scanUntil)>=0)status.scanning=false;
    if(changed){changed=false;mustChange=true;attempted=false;}
    if(pending&&status.configured&&(!attempted||uint32_t(now-lastAttempt)>=5000)){
        selected=target;pending=false;connect=true;lastAttempt=now;attempted=true;
    }
    portEXIT_CRITICAL(&mux);
    if(diagnostic::isEnabled()&&uint32_t(now-diagnosticHealth)>=10000){diagnosticHealth=now;diagnostic::rssi(diagnostic::POLAR,client&&client->isConnected()?client->getRssi():0);}
    if(mustChange&&client&&client->isConnected()){disconnectIntent.store(diagnostic::USER_CHANGE);client->disconnect();return;}
    if(!connect)return;
    if(client&&client->isConnected())return;
    diagnostic::Attempt diagnosticAttempt(diagnostic::POLAR);
    NimBLEDevice::getScan()->stop();detail("Connecting to selected Polar H10");
    if(!client){client=NimBLEDevice::createClient();if(!client){diagnostic::count(diagnostic::CONNECT_FAILURES);diagnostic::record(diagnostic::CONNECT_FAILED,diagnostic::POLAR,-1);detail("No Bluetooth capacity for H10");return;}client->setClientCallbacks(&callbacks,false);client->setConnectTimeout(3000);}
    if(!client->connect(NimBLEAddress(selected.address,selected.type))){diagnostic::count(diagnostic::CONNECT_FAILURES);diagnostic::record(diagnostic::CONNECT_FAILED,diagnostic::POLAR,client->getLastError());detail("H10 unavailable; check its two-device setting");return;}
    auto* service=client->getService(NimBLEUUID(uint16_t(0x180d)));
    auto* ch=service?service->getCharacteristic(NimBLEUUID(uint16_t(0x2a37))):nullptr;
    if(!ch||!ch->canNotify()||!ch->subscribe(true,notify)){diagnostic::record(diagnostic::SUBSCRIBE,diagnostic::POLAR,0x2a37,0,client->getLastError());disconnectIntent.store(diagnostic::SUBSCRIPTION_FAILED);client->disconnect();detail("H10 heart-rate subscription failed");return;}
    diagnostic::record(diagnostic::SUBSCRIBE,diagnostic::POLAR,0x2a37,1);
    portENTER_CRITICAL(&mux);
    // A settings change during discovery must not publish the wrong band as connected.
    const bool same=app::sameBand(status.address,targetType,selected.address,selected.type);
    if(same){status.sample.connected=true;snprintf(status.detail,sizeof(status.detail),"Connected to Polar H10");}
    portEXIT_CRITICAL(&mux);
    if(!same){disconnectIntent.store(diagnostic::WRONG_BAND);client->disconnect();}
}
}
