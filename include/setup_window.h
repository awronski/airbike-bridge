#pragma once
#include <stdint.h>

// Explicit setup closes after a newly saved connection and a grace period.
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
