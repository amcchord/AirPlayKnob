#pragma once
#include <stddef.h>
#include <stdint.h>

// A successful submission means the driver accepted a frame, not that the
// receiver heard it. This deliberately does not claim over-the-air delivery.
struct NetworkTxSnapshot {
    uint32_t submitted, accepted, rejected, mdns_submitted, mdns_rejected;
    int32_t last_error;
};
NetworkTxSnapshot network_tx_snapshot();

// Pure helpers shared with the host regression checks.
inline bool network_is_mdns(const uint8_t *p, size_t n) {
    if (!p || n < 14) return false;
    size_t udp = 0;
    if (p[12] == 8 && p[13] == 0 && n >= 34) {
        const size_t ihl = (p[14] & 15) * 4;
        if ((p[14] >> 4) != 4 || ihl < 20 || p[23] != 17 || (p[20] & 31) || p[21]) return false;
        udp = 14 + ihl;
    } else if (p[12] == 0x86 && p[13] == 0xDD && n >= 54 && p[20] == 17) {
        udp = 54;
    } else return false;
    return n >= udp + 8 && p[udp + 2] == 0x14 && p[udp + 3] == 0xE9;
}

struct UdpTxStats {
    uint32_t attempted = 0, accepted = 0, rejected = 0, retries = 0;
    int last_error = 0;
    bool record(int result, size_t expected, int error) {
        ++attempted;
        if (result == (int)expected) { ++accepted; return true; }
        ++rejected;
        last_error = error;
        return false;
    }
};

inline bool artwork_network_allowed(bool playing, bool native_only, bool busy,
                                    uint32_t buffered_ms, uint32_t minimum_ms,
                                    uint32_t mp3_bytes = 0, uint32_t minimum_mp3_bytes = 0) {
    return !busy && (!playing || native_only ||
                     (buffered_ms >= minimum_ms && mp3_bytes >= minimum_mp3_bytes));
}

inline bool discovery_must_wait(bool playing, bool native_only, bool selected_needs_ip,
                                bool initial_scan, bool raop_connected, bool connecting) {
    return playing && !selected_needs_ip &&
           (raop_connected || connecting || (!native_only && !initial_scan));
}
