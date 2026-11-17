#include <assert.h>
#include <stdio.h>
#include "relay_peers.h"

int main() {
    relay::Peers peers;
    assert(peers.count() == 0 && !peers.any(relay::POWER));
    assert(peers.connect(7));
    peers.subscribe(7, relay::POWER, true);
    assert(peers.count() == 1 && peers.any(relay::POWER) && !peers.any(relay::CSC));
    // A second service can use the same link or a second link.
    peers.subscribe(7, relay::CSC, true);
    assert(peers.any(relay::CSC));
    peers.subscribe(7, relay::CSC, false);
    assert(peers.connect(19));
    peers.subscribe(19, relay::CSC, true);
    peers.subscribe(19, relay::CONTROL, true);
    assert(peers.count() == 2 && !peers.connect(22));
    assert(!peers.subscribed(7, relay::CONTROL));
    assert(peers.subscribed(19, relay::CONTROL));
    // Disconnecting one profile must preserve the other profile's subscription.
    peers.disconnect(7);
    assert(peers.count() == 1 && peers.any(relay::CSC) && !peers.any(relay::POWER));
    assert(peers.connect(22));
    peers.subscribe(22, relay::POWER, true);
    peers.disconnect(19);
    assert(peers.count() == 1 && peers.any(relay::POWER) && !peers.any(relay::CSC));
    peers.disconnect(22);
    assert(peers.count() == 0);
    // Handle reuse must not inherit subscriptions from an old watch/link.
    assert(peers.connect(19));
    assert(!peers.subscribed(19, relay::CONTROL) && !peers.any(relay::CSC));
    puts("Combined/separate subscriptions, independent disconnects, capacity and handle reuse passed.");
}
