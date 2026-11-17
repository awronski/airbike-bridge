#include <assert.h>
#include <stdio.h>
#include "band_selection.h"
#include "ride_session.h"
static app::Settings profiles(){
    app::Settings s;s.count=2;s.nextID=3;s.riders[0].id=1;s.riders[1].id=2;
    snprintf(s.riders[0].name,49,"Rider A");snprintf(s.riders[1].name,49,"Rider B");
    snprintf(s.riders[0].bandAddress,18,"aa:bb:cc:dd:ee:01");snprintf(s.riders[1].bandAddress,18,"aa:bb:cc:dd:ee:02");
    snprintf(s.riders[0].bandName,64,"Polar H10 Rider A");snprintf(s.riders[1].bandName,64,"Polar H10 Rider B");return s;
}
int main(){
    auto cfg=profiles();assert(app::valid(cfg));bands::Selection pick;bool seen[app::MAX_RIDERS]={};
    pick.update(cfg,seen,0,false,0);assert(pick.ready()&&pick.riderID==0);
    cfg.selectedID=2;pick.update(cfg,seen,1,false,0);assert(pick.riderID==2);
    cfg.autoRider=true;pick.reset(100);pick.update(cfg,seen,100,false,0);assert(!pick.ready()&&pick.phase==bands::SCANNING);
    seen[0]=true;pick.update(cfg,seen,5099,false,0);assert(!pick.ready());
    pick.update(cfg,seen,5100,false,0);assert(pick.ready()&&pick.riderID==1);
    // A second band before recording requires a choice, regardless of signal strengths.
    seen[1]=true;pick.update(cfg,seen,5200,false,0);assert(!pick.ready()&&pick.phase==bands::CHOOSE&&pick.count==2);
    seen[0]=false;pick.update(cfg,seen,20000,false,0);assert(pick.phase==bands::CHOOSE&&pick.count==2); // No silent fallback after ambiguity.
    assert(!pick.choose(cfg,999)&&pick.choose(cfg,2));pick.update(cfg,seen,20001,false,0);assert(pick.ready()&&pick.riderID==2);
    pick.update(cfg,seen,20002,true,2);assert(pick.phase==bands::LOCKED&&pick.riderID==2);
    // Lost band or other bands never changes a recording or stopped ride's owner.
    seen[1]=false;seen[0]=true;pick.update(cfg,seen,40000,true,2);assert(pick.riderID==2&&!pick.choose(cfg,1));
    pick.update(cfg,seen,40001,false,0);assert(pick.phase==bands::SCANNING&&!pick.ready());
    pick.update(cfg,seen,45001,false,0);assert(pick.ready()&&pick.riderID==1);
    seen[0]=false;pick.update(cfg,seen,45002,false,0);assert(pick.phase==bands::WAITING&&!pick.ready());
    // Two bands seen immediately trigger the prompt; never briefly choose the first.
    pick.reset(0);seen[0]=seen[1]=true;pick.update(cfg,seen,1,false,0);assert(pick.phase==bands::CHOOSE);
    // Settings codec and duplicate identity protection.
    uint8_t encoded[app::SETTINGS_BYTES];app::encode(cfg,encoded);app::Settings decoded;
    assert(app::decode(decoded,encoded,sizeof(encoded))&&decoded.autoRider&&decoded.riders[1].bandAddress[16]=='2');
    assert(app::ownerOf(decoded,"AA:BB:CC:DD:EE:02",0)->id==2);
    memcpy(cfg.riders[1].bandAddress,cfg.riders[0].bandAddress,18);assert(!app::valid(cfg));cfg=profiles();
    // v1 storage migration attaches the old shared band to the selected named rider.
    uint8_t old[app::SETTINGS_V1_BYTES]={};memcpy(old,"ABCF",4);old[4]=1;old[5]=2;
    rides::writeLE(old+8,3,4);rides::writeLE(old+12,2,4);old[16]=old[17]=1;rides::writeLE(old+18,3,2);rides::writeLE(old+20,10,2);
    memcpy(old+23,cfg.riders[1].bandAddress,18);memcpy(old+42,cfg.riders[1].bandName,64);
    for(unsigned i=0;i<2;++i){uint8_t* p=old+128+i*64;rides::writeLE(p,cfg.riders[i].id,4);memcpy(p+4,cfg.riders[i].name,49);p[53]=i?1:2;p[54]=40+i;rides::writeLE(p+56,655+i*150,2);rides::writeLE(p+58,180+i*5,2);}
    rides::writeLE(old+sizeof(old)-4,rides::checksum(old,sizeof(old)-4),4);
    assert(app::decode(decoded,old,sizeof(old))&&!decoded.autoRider&&decoded.selectedID==2);
    assert(decoded.riders[0].age==40&&decoded.riders[1].sex==1&&decoded.riders[1].weightDeciKg==805&&decoded.riders[1].maxHR==185);
    assert(!decoded.riders[0].bandAddress[0]&&app::sameBand(decoded.riders[1].bandAddress,0,cfg.riders[1].bandAddress,0));
    assert(!app::selectedBandRider(decoded,0).bandAddress[0]); // A bound band cannot fall back to Guest.
    // Legacy Guest band remains available until explicitly linked to a rider.
    rides::writeLE(old+12,0,4);rides::writeLE(old+sizeof(old)-4,rides::checksum(old,sizeof(old)-4),4);
    assert(app::decode(decoded,old,sizeof(old))&&app::selectedBandRider(decoded,0).bandAddress[0]);
    app::encode(decoded,encoded);encoded[100]^=1;assert(!app::decode(decoded,encoded,sizeof(encoded))&&decoded.count==2);
    // Detection timer across millis rollover.
    cfg=profiles();cfg.autoRider=true;pick.reset(UINT32_MAX-2000);seen[0]=true;seen[1]=false;
    pick.update(cfg,seen,UINT32_MAX-2000,false,0);pick.update(cfg,seen,2999,false,0);assert(pick.ready()&&pick.riderID==1);
    // Unresolved Auto selection blocks automatic recording but not raw data acquisition.
    session::Tracker tracker;cfg.autoStart=true;session::Input input;input.connected=input.hasData=true;
    for(unsigned i=0;i<10;++i){input.packetMs=i*1000;input.sequence=i+1;input.metrics.elapsed_s=i;input.metrics.distance_m=i*5;input.metrics.power_w=100;tracker.tick(input,{},cfg,i*1000,nullptr,false);}
    assert(tracker.state==session::READY&&tracker.latest.metrics.power_w==100);
    puts("Rider-band mappings, old-settings migration, Auto detection/ambiguity, ride lock, no-band gate and rollover passed.");
}
