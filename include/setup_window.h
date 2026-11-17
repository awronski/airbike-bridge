#pragma once
#include <stdint.h>

// Reopening settings while connected must not start the completion countdown.
// Only saving a successful new connection permits the hotspot to close.
struct SetupWindow {
    bool completed = false;
    uint32_t completedMs = 0;

    void reopen() { completed = false; }
    void connectionSaved(uint32_t now) {
        completed = true;
        completedMs = now;
    }
    bool shouldClose(uint32_t now) const {
        return completed && static_cast<uint32_t>(now - completedMs) >= 60000;
    }
};
