#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "setup_window.h"

int main() {
    SetupWindow window;
    window.reopen();
    // Opening settings on an already-connected ATOM must leave enough time to
    // enter the replacement network, even after the previous one-minute limit.
    assert(!window.shouldClose(0));
    assert(!window.shouldClose(60001));
    assert(!window.shouldClose(600000));

    window.connectionSaved(700000);
    assert(!window.shouldClose(759999));
    assert(window.shouldClose(760000));

    // Reopening an active hotspot cancels the old completion countdown.
    window.reopen();
    assert(!window.shouldClose(900000));
    window.connectionSaved(UINT32_MAX - 1000);
    assert(!window.shouldClose(1000));
    assert(window.shouldClose(59000));
    puts("Wi-Fi settings stay open until a successful replacement connection; countdown and wraparound passed.");
}
