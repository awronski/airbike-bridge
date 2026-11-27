#include <assert.h>
#include <stdio.h>
#include "heart_rate.h"
#include "bridge_settings.h"
int main(){
    heart::Measurement m;
    const uint8_t basic[]={0,142};assert(heart::decode(basic,sizeof(basic),m)&&m.bpm==142&&m.contact&&!m.contactSupported);
    const uint8_t wide[]={7,0x04,0x01};assert(heart::decode(wide,sizeof(wide),m)&&m.bpm==260&&m.contact);
    const uint8_t contactLost[]={4,140};assert(heart::decode(contactLost,2,m)&&!m.contact);
    const uint8_t rr[]={0x1e,142,1,0,0x20,3};assert(heart::decode(rr,sizeof(rr),m)&&m.bpm==142);
    assert(!heart::decode(rr,5,m));assert(!heart::decode(wide,2,m));assert(!heart::decode(nullptr,0,m));
    const uint8_t extra[]={0,140,9};assert(!heart::decode(extra,3,m));
    const uint8_t tooHigh[]={1,0xff,0xff};assert(!heart::decode(tooHigh,3,m));
    heart::Sample sample;sample.connected=sample.received=sample.contact=true;sample.bpm=142;sample.at=UINT32_MAX-1000;
    assert(sample.valid(0)&&!sample.valid(5000));
    app::Settings s;assert(app::valid(s));s.count=1;s.riders[0].id=1;s.nextID=2;s.selectedID=1;
    snprintf(s.riders[0].name,sizeof(s.riders[0].name),"Żaneta");s.riders[0].age=40;s.riders[0].sex=1;s.riders[0].weightDeciKg=655;s.riders[0].maxHR=185;
    snprintf(s.hrAddress,sizeof(s.hrAddress),"aa:bb:cc:dd:ee:ff");snprintf(s.hrName,sizeof(s.hrName),"Polar H10 ABC");assert(app::valid(s));
    uint8_t bytes[app::SETTINGS_BYTES];app::encode(s,bytes);app::Settings restored;assert(app::decode(restored,bytes,sizeof(bytes)));
    assert(restored.selectedID==1&&restored.riders[0].weightDeciKg==655&&!strcmp(restored.selected().name,"Żaneta"));
    assert(restored.selected().ftpWatts==0&&!restored.selected().createKey); // Existing v2 reserved bytes are zero.
    assert(!restored.findCreatedRider(0));
    constexpr uint64_t request=0x96a5f02136b487c1ULL;
    s.riders[0].ftpWatts=247;s.riders[0].createKey=request;app::encode(s,bytes);
    assert(app::decode(restored,bytes,sizeof(bytes))&&restored.selected().ftpWatts==247);
    assert(restored.findCreatedRider(request)==restored.find(1));
    assert(!restored.findCreatedRider(request+1));
    assert(!strcmp(restored.hrAddress,"aa:bb:cc:dd:ee:ff"));
    s.riders[0].ftpWatts=app::MAX_FTP_WATTS+1;assert(!app::valid(s));s.riders[0].ftpWatts=247;
    bytes[40]^=1;assert(!app::decode(restored,bytes,sizeof(bytes)));assert(restored.selectedID==1);
    s.riders[0].maxHR=1;assert(!app::valid(s));s.riders[0].maxHR=185;s.selectedID=99;assert(!app::valid(s));
    assert(!app::validName(" ")&&!app::validName("bad\nname")&&!app::validName("\xc0\xaf")&&!app::validName("\xed\xa0\x80"));
    assert(app::validAddress("aa:bb:cc:dd:ee:ff")&&!app::validAddress("aa:bb:cc:dd:ee:zz"));
    puts("Heart rate flags/length/contact/freshness and rider/settings validation, Unicode and persistence passed.");
}
