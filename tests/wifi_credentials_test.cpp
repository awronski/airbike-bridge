#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include "wifi_credentials.h"

// Atomic replacement models the NVS blob contract; interrupted writes retain the old blob.
struct Store {
    std::map<std::string, std::vector<uint8_t>> blobs;
    std::map<std::string, std::string> legacy;
    bool writeFails = false, readFails = false;
    unsigned writes = 0;
    bool isKey(const char* key) { return blobs.count(key) || legacy.count(key); }
    size_t getBytesLength(const char* key) { return blobs.count(key) ? blobs[key].size() : 0; }
    std::string getString(const char* key) { return legacy[key]; }
    size_t putBytes(const char* key, const void* bytes, size_t size) {
        ++writes;
        if (writeFails) return 0;
        const auto* p = static_cast<const uint8_t*>(bytes);
        blobs[key] = std::vector<uint8_t>(p, p + size);
        return size;
    }
    size_t getBytes(const char* key, void* bytes, size_t size) {
        if (readFails || !blobs.count(key) || blobs[key].size() != size) return 0;
        std::memcpy(bytes, blobs[key].data(), size); return size;
    }
};

int main() {
    Store store;
    store.legacy["ssid"] = "previous-network";
    store.legacy["password"] = "previous-password";
    wifi::Credentials read, candidate;
    assert(wifi::load(store, read));
    assert(!strcmp(read.ssid, "previous-network") && store.writes == 0);
    assert(wifi::make("new-network", "new-password", candidate));
    store.writeFails = true;
    assert(!wifi::save(store, candidate));
    assert(wifi::load(store, read) && !strcmp(read.ssid, "previous-network"));
    store.writeFails = false;
    assert(wifi::save(store, candidate));
    assert(store.blobs.size() == 1); // One record contains both fields.
    assert(wifi::load(store, read));
    assert(!strcmp(read.ssid, "new-network") && !strcmp(read.password, "new-password"));
    assert(store.legacy["ssid"] == "previous-network");

    assert(wifi::make("replacement", "replacement-password", candidate));
    store.writeFails = true;
    assert(!wifi::save(store, candidate));
    assert(wifi::load(store, read));
    assert(!strcmp(read.ssid, "new-network") && !strcmp(read.password, "new-password"));
    store.writeFails = false; store.readFails = true;
    assert(!wifi::save(store, candidate)); // Successful write is insufficient without readback.
    store.readFails = false;
    assert(wifi::load(store, read) && !strcmp(read.ssid, "replacement"));
    store.blobs.begin()->second[10] ^= 1;
    assert(!wifi::load(store, read)); // No silent fallback to obsolete legacy credentials.

    Store open;
    assert(wifi::make("open-network", "", candidate));
    assert(wifi::save(open, candidate) && wifi::load(open, read) && !read.password[0]);
    const std::string ssid(32, 's'), psk(64, 'a');
    assert(wifi::make(ssid.c_str(), psk.c_str(), candidate));
    assert(wifi::save(open, candidate) && wifi::load(open, read));
    assert(!strcmp(read.password, psk.c_str()));
    assert(!wifi::make("", "12345678", candidate));
    assert(!wifi::make(std::string(33, 's').c_str(), "12345678", candidate));
    assert(!wifi::make("network", "short", candidate));
    assert(!wifi::make("network", std::string(64, 'z').c_str(), candidate));
    std::puts("Atomic Wi-Fi credential replacement, legacy reads, failed writes/readback, corruption and boundary lengths passed.");
}
