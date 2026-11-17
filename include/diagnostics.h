#pragma once
#include "diagnostics_model.h"
#include <WebServer.h>
namespace diagnostic {
void begin();
bool isEnabled();
void record(Kind kind,Source source=SYSTEM,int32_t a=0,int32_t b=0,int32_t c=0,int32_t d=0);
void connection(Source source,bool up,int reason=0,Intent intent=UNEXPECTED);
void packet(Source source,bool valid,const uint8_t* bytes=nullptr,size_t length=0);
void count(Stat which,uint32_t n=1);
void maximum(Stat which,uint32_t n);
void minimum(Stat which,uint32_t n);
void rssi(Source source,int value);
void pulse(bool relay);
void scan(bool scanning,uint32_t elapsed);
void flash(uint8_t operation,uint32_t durationUs,bool ok);
void tick(bool pedalling,uint32_t rider,int console,uint8_t rideState);
void finishCapture();
void configureHTTP(WebServer& server,bool(*allow)(const char*));
struct Attempt {Source source;uint32_t started;explicit Attempt(Source);~Attempt();};
}
