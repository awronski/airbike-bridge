#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_timer.h>
#include "cycling_packets.h"
#include "garmin_relay.h"
#include "relay_peers.h"
#include "diagnostics.h"

namespace relay {
namespace {
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Stats status;
cycling::Encoder encoder;
NimBLECharacteristic* power = nullptr;
NimBLECharacteristic* csc = nullptr;
uint64_t lastSentUs = 0;
uint64_t lastAdvertisingAttemptUs = 0;
Peers peers;
uint32_t lastDiagnosticSendFailure=0,lastDiagnosticAdvFailure=0;

void updatePeerStatus() { // mux must be held.
    status.outputConnections = peers.count();
    status.connected = status.outputConnections != 0;
    status.powerSubscribed = peers.any(POWER);
    status.cscSubscribed = peers.any(CSC);
}

void maintainAdvertising() {
    auto* advertising = NimBLEDevice::getAdvertising();
    const auto copy = stats();
    const uint64_t now = esp_timer_get_time();
    bool active = advertising->isAdvertising();
    // Retry from the worker after transient controller/connection errors too.
    if (!active && copy.outputConnections < MAX_OUTPUT_PEERS &&
        now - lastAdvertisingAttemptUs >= 1000000ULL) {
        lastAdvertisingAttemptUs = now;
        const bool ok = advertising->start();
        active = advertising->isAdvertising();
        if (!ok) {
            portENTER_CRITICAL(&mux);
            ++status.advertisingFailures;
            portEXIT_CRITICAL(&mux);
            diagnostic::count(diagnostic::ADV_FAILED);
            if(!lastDiagnosticAdvFailure||uint32_t(millis()-lastDiagnosticAdvFailure)>=10000){lastDiagnosticAdvFailure=millis();diagnostic::record(diagnostic::ADVERTISE_FAILED,diagnostic::GARMIN);}
        }
    }
    portENTER_CRITICAL(&mux);
    status.advertising = active;
    portEXIT_CRITICAL(&mux);
}

class Connections : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& peer) override {
        portENTER_CRITICAL(&mux);
        peers.connect(peer.getConnHandle());
        updatePeerStatus();
        portEXIT_CRITICAL(&mux);
        diagnostic::connection(diagnostic::GARMIN,true);
        diagnostic::record(diagnostic::LINK_PARAMS,diagnostic::GARMIN,peer.getConnInterval(),peer.getConnTimeout(),peer.getConnLatency(),peer.getMTU());
        Serial.println("Garmin relay: Bluetooth client connected");
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo& peer, int reason) override {
        portENTER_CRITICAL(&mux);
        peers.disconnect(peer.getConnHandle());
        updatePeerStatus();
        portEXIT_CRITICAL(&mux);
        diagnostic::connection(diagnostic::GARMIN,false,reason);
        Serial.println("Garmin relay: client disconnected; worker will resume advertising");
    }
} connections;

class Subscriptions : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* ch, NimBLEConnInfo& peer, uint16_t value) override {
        portENTER_CRITICAL(&mux);
        if (ch == power) peers.subscribe(peer.getConnHandle(), POWER, (value & 1) != 0);
        if (ch == csc) peers.subscribe(peer.getConnHandle(), CSC, (value & 1) != 0);
        updatePeerStatus();
        portEXIT_CRITICAL(&mux);
        diagnostic::record(diagnostic::SUBSCRIBE,diagnostic::GARMIN,ch==power?0x2a63:0x2a5b,value,peer.getConnHandle());
    }
} subscriptions;

class ControlPoint : public NimBLECharacteristicCallbacks {
    bool m_power;
    uint8_t kind() const { return m_power ? POWER_CONTROL : CONTROL; }
  public:
    explicit ControlPoint(bool powerService) : m_power(powerService) {}
  private:
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& peer, uint16_t value) override {
        portENTER_CRITICAL(&mux);
        peers.subscribe(peer.getConnHandle(), kind(), (value & 2) != 0);
        portEXIT_CRITICAL(&mux);
    }
    void onWrite(NimBLECharacteristic* ch, NimBLEConnInfo& peer) override {
        // Do not change counters unless the client is listening for the result.
        portENTER_CRITICAL(&mux);
        const bool subscribed = peers.subscribed(peer.getConnHandle(), kind());
        portEXIT_CRITICAL(&mux);
        if (!subscribed) return;
        const auto value = ch->getValue();
        if (value.size() == 0) return;
        const uint8_t op = static_cast<uint8_t>(value[0]);
        uint8_t response[] = {static_cast<uint8_t>(m_power ? 0x20 : 0x10), op, 0x02};
        if (op == 0x01) { // Set cumulative wheel revolutions (CSC).
            response[2] = 0x03; // Invalid parameter unless exactly 4 bytes follow.
            if (value.size() == 5) {
                const auto* bytes = reinterpret_cast<const uint8_t*>(value.data());
                const uint32_t count = static_cast<uint32_t>(bytes[1]) |
                    (static_cast<uint32_t>(bytes[2]) << 8) |
                    (static_cast<uint32_t>(bytes[3]) << 16) |
                    (static_cast<uint32_t>(bytes[4]) << 24);
                portENTER_CRITICAL(&mux);
                encoder.setWheelCount(count);
                portEXIT_CRITICAL(&mux);
                response[2] = 0x01;
            }
        }
        ch->setValue(response, sizeof(response));
        ch->indicate(peer.getConnHandle());
    }
};
ControlPoint controlPoint(false);
ControlPoint powerControlPoint(true);
}

Stats stats() {
    portENTER_CRITICAL(&mux);
    const Stats copy = status;
    portEXIT_CRITICAL(&mux);
    return copy;
}

bool begin() {
    NimBLEDevice::setDeviceName("AirBike Bridge");
    auto* server = NimBLEDevice::createServer();
    if (!server) return false;
    server->setCallbacks(&connections, false);
    auto* cps = server->createService(uint16_t(0x1818));
    auto* speed = server->createService(uint16_t(0x1816));
    if (!cps || !speed) return false;
    power = cps->createCharacteristic(uint16_t(0x2a63), NIMBLE_PROPERTY::NOTIFY);
    csc = speed->createCharacteristic(uint16_t(0x2a5b), NIMBLE_PROPERTY::NOTIFY);
    auto* cpFeature = cps->createCharacteristic(uint16_t(0x2a65), NIMBLE_PROPERTY::READ);
    auto* cpLocation = cps->createCharacteristic(uint16_t(0x2a5d), NIMBLE_PROPERTY::READ);
    auto* scFeature = speed->createCharacteristic(uint16_t(0x2a5c), NIMBLE_PROPERTY::READ);
    auto* scLocation = speed->createCharacteristic(uint16_t(0x2a5d), NIMBLE_PROPERTY::READ);
    auto* control = speed->createCharacteristic(uint16_t(0x2a55), NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE);
    auto* powerControl = cps->createCharacteristic(uint16_t(0x2a66), NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE);
    if (!power || !csc || !cpFeature || !cpLocation || !scFeature || !scLocation || !control || !powerControl) return false;
    const uint8_t cpFeatures[] = {0x0c, 0, 0, 0}; // Wheel + crank revolution data supported.
    const uint8_t scFeatures[] = {0x03, 0}; // Wheel + crank, single sensor location.
    const uint8_t other[] = {0};
    cpFeature->setValue(cpFeatures, sizeof(cpFeatures));
    scFeature->setValue(scFeatures, sizeof(scFeatures));
    cpLocation->setValue(other, sizeof(other));
    scLocation->setValue(other, sizeof(other));
    power->setCallbacks(&subscriptions);
    csc->setCallbacks(&subscriptions);
    control->setCallbacks(&controlPoint);
    powerControl->setCallbacks(&powerControlPoint);
    if (!server->start()) return false;
    auto* advertising = NimBLEDevice::getAdvertising();
    advertising->setName("AirBike Bridge");
    advertising->addServiceUUID(uint16_t(0x1818));
    advertising->addServiceUUID(uint16_t(0x1816));
    advertising->enableScanResponse(true);
    const bool ok = advertising->start();
    const bool active = advertising->isAdvertising();
    portENTER_CRITICAL(&mux);
    status.ready = true; // GATT is ready; the worker retries failed advertising.
    status.advertising = active;
    if (!ok) ++status.advertisingFailures;
    portEXIT_CRITICAL(&mux);
    Serial.println(ok ? "Garmin relay: AirBike Bridge advertising power and speed/cadence; wheel 2000 mm" : "Garmin relay: advertising failed");
    return ok;
}

void update(bool fresh, const airbike::Metrics& metrics, bool finished) {
    const auto current = stats();
    if (!current.ready) return;
    maintainAdvertising();
    airbike::Metrics output = metrics;
    if (finished) { output.power_w = 0; output.speed_kph = 0; output.cadence_rpm = 0; }
    const uint64_t now = esp_timer_get_time();
    uint8_t powerBytes[14], cscBytes[11];
    portENTER_CRITICAL(&mux);
    encoder.update(now, fresh, output);
    encoder.powerPacket(output.power_w, powerBytes);
    encoder.cscPacket(cscBytes);
    status.active = fresh;
    portEXIT_CRITICAL(&mux);
    if (!fresh || !current.connected || now - lastSentUs < 1000000ULL) return;
    lastSentUs = now;
    if (current.powerSubscribed) {
        power->setValue(powerBytes, sizeof(powerBytes));
        const bool ok = power->notify();
        diagnostic::count(diagnostic::TX_ATTEMPTS);diagnostic::count(ok?diagnostic::TX_ACCEPTED:diagnostic::TX_FAILED);
        if(!ok&&(!lastDiagnosticSendFailure||uint32_t(millis()-lastDiagnosticSendFailure)>=10000)){lastDiagnosticSendFailure=millis();diagnostic::record(diagnostic::SEND_FAILED,diagnostic::GARMIN,0x2a63);}
        portENTER_CRITICAL(&mux);
        if (ok) ++status.powerNotifications; else ++status.sendFailures;
        portEXIT_CRITICAL(&mux);
    }
    if (current.cscSubscribed) {
        csc->setValue(cscBytes, sizeof(cscBytes));
        const bool ok = csc->notify();
        diagnostic::count(diagnostic::TX_ATTEMPTS);diagnostic::count(ok?diagnostic::TX_ACCEPTED:diagnostic::TX_FAILED);
        if(!ok&&(!lastDiagnosticSendFailure||uint32_t(millis()-lastDiagnosticSendFailure)>=10000)){lastDiagnosticSendFailure=millis();diagnostic::record(diagnostic::SEND_FAILED,diagnostic::GARMIN,0x2a5b);}
        portENTER_CRITICAL(&mux);
        if (ok) ++status.cscNotifications; else ++status.sendFailures;
        portEXIT_CRITICAL(&mux);
    }
}
} // namespace relay
