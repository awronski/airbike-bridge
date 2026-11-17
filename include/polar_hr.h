#pragma once
#include <NimBLEDevice.h>
#include "heart_rate.h"
#include "bridge_settings.h"
namespace hr {
constexpr size_t MAX_DEVICES=12;
struct Device {char name[64]="";char address[18]="";uint8_t type=0;int rssi=0;uint32_t seenMs=0;bool known=false;};
struct Status {heart::Sample sample;char address[18]="";char name[64]="";char detail[100]="No Polar H10 selected";uint8_t type=0;bool configured=false,scanning=false,monitoring=false;uint32_t rejected=0;Device devices[MAX_DEVICES];size_t count=0;};
Status snapshot();
void configure(const char* address,uint8_t type,const char* name);
void requestScan();
void watchKnown(const app::Settings& settings);
void monitor(bool enabled);
void onAdvertised(const NimBLEAdvertisedDevice* device);
void poll(); // BLE connection/discovery worker only; never called by relay task.
bool needsScan();
}
