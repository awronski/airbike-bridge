#pragma once
#include <stdint.h>

namespace relay {
// Reserve two additional controller connections for bike and H10 inputs.
constexpr unsigned MAX_OUTPUT_PEERS = 2;
constexpr uint16_t NO_PEER = 0xffff;
enum Subscription : uint8_t { POWER = 1, CSC = 2, CONTROL = 4, POWER_CONTROL = 8 };
struct Peers {
    struct Peer { uint16_t handle = NO_PEER; uint8_t subscriptions = 0; } peers[MAX_OUTPUT_PEERS];
    bool connect(uint16_t handle) {
        for (const auto& p : peers) if (p.handle == handle) return true;
        for (auto& p : peers) if (p.handle == NO_PEER) {
            p.handle = handle; p.subscriptions = 0; return true;
        }
        return false;
    }
    void disconnect(uint16_t handle) {
        for (auto& p : peers) if (p.handle == handle) p = Peer{};
    }
    void subscribe(uint16_t handle, uint8_t kind, bool enabled) {
        for (auto& p : peers) if (p.handle == handle) {
            if (enabled) p.subscriptions |= kind;
            else p.subscriptions &= static_cast<uint8_t>(~kind);
        }
    }
    bool subscribed(uint16_t handle, uint8_t kind) const {
        for (const auto& p : peers) if (p.handle == handle) return (p.subscriptions & kind) != 0;
        return false;
    }
    bool any(uint8_t kind) const {
        for (const auto& p : peers) if (p.handle != NO_PEER && (p.subscriptions & kind)) return true;
        return false;
    }
    unsigned count() const {
        unsigned n = 0;
        for (const auto& p : peers) if (p.handle != NO_PEER) ++n;
        return n;
    }
};
}
