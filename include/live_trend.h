#pragma once
#include <stddef.h>
#include <stdint.h>

namespace trend {
constexpr uint32_t WINDOW_MS=30*60*1000, SAMPLE_MS=5000;
constexpr size_t CAPACITY=WINDOW_MS/SAMPLE_MS+1;
struct Point { uint32_t at=0;uint16_t hr=0,power=0;bool hrValid=false,powerValid=false; };
struct Buffer {
    Point points[CAPACITY];size_t count=0,head=0;
    uint32_t riderID=0,lastSample=0;bool resolved=false,clockSet=false,rideActive=false;
    const Point& at(size_t index) const {return points[(head+CAPACITY-count+index)%CAPACITY];}
    void select(uint32_t id,bool ready){
        if(id!=riderID||ready!=resolved){count=head=0;clockSet=false;}
        riderID=id;resolved=ready;
    }
    void setRideActive(bool active) {
        if (rideActive && !active) {
            for (auto& point : points) point.hrValid = false;
        }
        rideActive = active;
    }
    void sample(uint32_t now,bool hrValid,uint16_t hr,bool powerValid,uint16_t power){
        while(count&&uint32_t(now-at(0).at)>WINDOW_MS)--count;
        if(!resolved||(clockSet&&uint32_t(now-lastSample)<SAMPLE_MS))return;
        auto& point=points[head];point.at=now;point.power=power;
        point.hrValid=rideActive&&hrValid;point.hr=point.hrValid?hr:0;point.powerValid=powerValid;
        head=(head+1)%CAPACITY;if(count<CAPACITY)++count;
        lastSample=now;clockSet=true;
    }
};
}
