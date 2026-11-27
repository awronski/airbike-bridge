#include <cassert>
#include <cstdio>
#include "live_trend.h"
int main(){
    trend::Buffer b;b.sample(0,true,140,true,185);assert(!b.count);
    b.select(1,true);b.setRideActive(true);b.sample(0,true,140,true,185);
    b.sample(4999,true,160,true,300);assert(b.count==1&&b.at(0).hr==140);
    b.sample(5000,false,0,true,0);assert(b.count==2&&!b.at(1).hrValid&&b.at(1).powerValid&&b.at(1).power==0);
    for(uint32_t t=10000;t<=2000000;t+=5000)b.sample(t,true,150,false,0);
    assert(b.count==trend::CAPACITY&&b.at(0).at==200000&&b.at(b.count-1).at==2000000);
    b.sample(4000000,true,155,true,210);assert(b.count==1); // No made-up data through a long gap.
    b.select(2,true);assert(!b.count);b.sample(4005000,true,160,true,220);assert(b.count==1&&b.riderID==2);
    b.select(2,false);assert(!b.count);b.sample(4010000,true,160,true,220);assert(!b.count);
    b.select(0,true);assert(!b.count); // Guest still records bike power.
    b.sample(UINT32_MAX-2000,false,0,true,100);b.sample(2999,true,130,true,200);
    assert(b.count==2&&b.at(1).power==200);

    trend::Buffer ride;
    ride.select(1,true);
    ride.sample(0,true,130,true,175);
    assert(ride.count==1&&!ride.at(0).hrValid&&ride.at(0).power==175);
    ride.setRideActive(true);
    ride.sample(5000,true,142,true,185);
    assert(!ride.at(0).hrValid&&ride.at(1).hrValid&&ride.at(1).hr==142);
    ride.setRideActive(true); // A paused ride is still active.
    ride.sample(10000,true,138,true,0);
    assert(ride.at(1).hrValid&&ride.at(2).hrValid);
    ride.setRideActive(false);
    assert(!ride.at(1).hrValid&&!ride.at(2).hrValid&&ride.at(1).power==185);
    ride.setRideActive(true);
    ride.sample(15000,true,125,true,170);
    assert(!ride.at(2).hrValid&&ride.at(3).hrValid&&ride.at(3).hr==125);
    std::puts("Trend sampling, bounded ring, missing/zero readings, rider isolation, rollover and ride-only HR passed.");
}
