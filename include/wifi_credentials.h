#pragma once
#include <string.h>
#include "ride_history.h"

namespace wifi {
struct Credentials {
    char ssid[33] = "";
    char password[65] = "";
};
constexpr char KEY[] = "wifi-v1";
constexpr size_t STORAGE_BYTES = 4 + 33 + 65 + 4;

inline bool make(const char* ssid, const char* password, Credentials& out) {
    if (!ssid || !password) return false;
    const size_t nameLength = strnlen(ssid, 33), passLength = strnlen(password, 65);
    if (!nameLength || nameLength > 32 || passLength > 64 || (passLength && passLength < 8)) return false;
    if (passLength == 64) {
        for (size_t i = 0; i < passLength; ++i) {
            const char c = password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        }
    }
    Credentials next;
    memcpy(next.ssid, ssid, nameLength);
    memcpy(next.password, password, passLength);
    out = next;
    return true;
}

// Preferences replaces one NVS blob, so SSID and password cannot be mixed from
// separate updates. Only report success after reading the entire record back.
template<class Store> bool save(Store& store, const Credentials& credentials) {
    Credentials checked;
    if (!make(credentials.ssid, credentials.password, checked)) return false;
    uint8_t bytes[STORAGE_BYTES] = {};
    memcpy(bytes, "ABW1", 4);
    memcpy(bytes + 4, checked.ssid, sizeof(checked.ssid));
    memcpy(bytes + 37, checked.password, sizeof(checked.password));
    rides::writeLE(bytes + STORAGE_BYTES - 4, rides::checksum(bytes, STORAGE_BYTES - 4), 4);
    if (store.putBytes(KEY, bytes, sizeof(bytes)) != sizeof(bytes)) return false;
    uint8_t verified[STORAGE_BYTES];
    return store.getBytes(KEY, verified, sizeof(verified)) == sizeof(verified) &&
        memcmp(bytes, verified, sizeof(bytes)) == 0;
}

template<class Store> bool load(Store& store, Credentials& out) {
    if (store.isKey(KEY)) {
        uint8_t bytes[STORAGE_BYTES];
        if (store.getBytesLength(KEY) != sizeof(bytes) ||
            store.getBytes(KEY, bytes, sizeof(bytes)) != sizeof(bytes) ||
            memcmp(bytes, "ABW1", 4) ||
            rides::readLE(bytes + STORAGE_BYTES - 4, 4) != rides::checksum(bytes, STORAGE_BYTES - 4)) return false;
        Credentials decoded;
        memcpy(decoded.ssid, bytes + 4, sizeof(decoded.ssid));
        memcpy(decoded.password, bytes + 37, sizeof(decoded.password));
        return make(decoded.ssid, decoded.password, out);
    }
    // Existing devices can boot without a migration write or clearing old keys.
    if (!store.isKey("ssid") || !store.isKey("password")) return false;
    const auto ssid = store.getString("ssid"), password = store.getString("password");
    return make(ssid.c_str(), password.c_str(), out);
}
}
