#pragma once
#include "airbike_protocol.h"
namespace relay {
struct Stats {
    bool ready = false;
    bool advertising = false;
    uint8_t outputConnections = 0;
    uint32_t advertisingFailures = 0;
    bool connected = false;
    bool powerSubscribed = false;
    bool cscSubscribed = false;
    bool active = false;
    uint32_t powerNotifications = 0;
    uint32_t cscNotifications = 0;
    uint32_t sendFailures = 0;
};
bool begin(); // Call after NimBLEDevice::init, in the BLE worker.
void update(bool fresh, const airbike::Metrics& metrics, bool finished);
Stats stats();
}
