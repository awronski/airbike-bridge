#pragma once
#include <stddef.h>
#include <stdint.h>
namespace heart {
struct Measurement { uint16_t bpm = 0; bool contactSupported = false, contact = true; };
inline bool decode(const uint8_t* p, size_t n, Measurement& out) {
    if (!p || n < 2 || (p[0] & 0xe0)) return false;
    Measurement m;
    const uint8_t flags = p[0];
    size_t i = 1;
    if (flags & 1) { if (n < 3) return false; m.bpm = p[i] | (uint16_t(p[i+1]) << 8); i += 2; }
    else m.bpm = p[i++];
    m.contactSupported = flags & 4;
    m.contact = !m.contactSupported || (flags & 2);
    if (flags & 8) { if (n < i+2) return false; i += 2; }
    if (flags & 16) { if (n < i+2 || (n-i)%2) return false; i = n; }
    if (i != n || m.bpm > 300) return false;
    out = m; return true;
}
struct Sample {
    uint16_t bpm = 0; uint32_t at = 0; bool connected = false, contact = false, received = false;
    bool valid(uint32_t now) const { return connected && received && contact && bpm > 0 && uint32_t(now-at) < 5000; }
};
}
