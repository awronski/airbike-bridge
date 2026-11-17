#pragma once
#include "bridge_settings.h"
namespace bands {
constexpr uint32_t DETECT_MS=5000,PRESENT_MS=15000;
enum Phase:uint8_t { MANUAL, SCANNING, WAITING, CHOOSE, SELECTED, LOCKED };
inline const char* phaseName(Phase p){return p==MANUAL?"manual":p==SCANNING?"scanning":p==WAITING?"waiting":p==CHOOSE?"choose":p==SELECTED?"selected":"locked";}
struct Selection {
    Phase phase=MANUAL;uint32_t riderID=0,since=0;bool started=false,wasRide=false,wasAuto=false,explicitChoice=false,ambiguous=false;
    uint32_t candidates[app::MAX_RIDERS]={};size_t count=0;
    void reset(uint32_t now){phase=SCANNING;riderID=0;since=now;started=true;wasRide=false;wasAuto=false;explicitChoice=false;ambiguous=false;count=0;}
    bool ready()const{return phase==MANUAL||phase==SELECTED||phase==LOCKED;}
    void update(const app::Settings& cfg,const bool* present,uint32_t now,bool active,uint32_t activeRider){
        if(active){phase=LOCKED;riderID=activeRider;wasRide=true;return;}
        if(wasRide){reset(now);}
        if(!cfg.autoRider){phase=MANUAL;riderID=cfg.selectedID;started=false;wasAuto=false;explicitChoice=false;ambiguous=false;count=0;return;}
        if(!started||!wasAuto){reset(now);wasAuto=true;}
        uint32_t detected[app::MAX_RIDERS]={};size_t n=0;
        for(size_t i=0;i<cfg.count;++i)if(cfg.riders[i].bandAddress[0]&&present[i])detected[n++]=cfg.riders[i].id;
        if(explicitChoice){phase=SELECTED;return;}
        if(n>1)ambiguous=true;
        if(ambiguous){
            // Keep every offered rider until the user answers, even if advertising stops.
            for(size_t i=0;i<n;++i){bool exists=false;for(size_t j=0;j<count;++j)if(candidates[j]==detected[i])exists=true;
                if(!exists&&count<app::MAX_RIDERS)candidates[count++]=detected[i];}
            phase=CHOOSE;riderID=0;return;
        }
        count=n;for(size_t i=0;i<n;++i)candidates[i]=detected[i];
        if(uint32_t(now-since)<DETECT_MS){phase=SCANNING;riderID=0;return;}
        if(count==1){phase=SELECTED;riderID=candidates[0];}else{phase=WAITING;riderID=0;}
    }
    bool choose(const app::Settings& cfg,uint32_t id){
        if(!cfg.autoRider||wasRide||!cfg.find(id))return false;
        riderID=id;explicitChoice=true;phase=SELECTED;return true;
    }
};
}
