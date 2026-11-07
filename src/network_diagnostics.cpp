#include "network_diagnostics.h"
#include <esp_wifi.h>
#include <atomic>

static std::atomic<uint32_t> s_submitted{0}, s_accepted{0}, s_rejected{0};
static std::atomic<uint32_t> s_mdns_submitted{0}, s_mdns_rejected{0};
static std::atomic<int32_t> s_last_error{0};

static esp_err_t record_tx(esp_err_t result, bool mdns) {
    s_submitted.fetch_add(1, std::memory_order_relaxed);
    if (mdns) s_mdns_submitted.fetch_add(1, std::memory_order_relaxed);
    if (result == ESP_OK) s_accepted.fetch_add(1, std::memory_order_relaxed);
    else {
        s_rejected.fetch_add(1, std::memory_order_relaxed);
        s_last_error.store(result, std::memory_order_relaxed);
        if (mdns) s_mdns_rejected.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

extern "C" esp_err_t __real_esp_wifi_internal_tx(wifi_interface_t, void *, uint16_t);
extern "C" esp_err_t __real_esp_wifi_internal_tx_by_ref(wifi_interface_t, void *, size_t, void *);
extern "C" esp_err_t __wrap_esp_wifi_internal_tx(wifi_interface_t iface, void *data, uint16_t len) {
    const bool mdns = network_is_mdns((const uint8_t *)data, len);
    // The driver may take ownership of a reference on success. Classify before
    // submitting and never inspect the frame after the real function returns.
    const esp_err_t result = __real_esp_wifi_internal_tx(iface, data, len);
    return record_tx(result, mdns);
}
extern "C" esp_err_t __wrap_esp_wifi_internal_tx_by_ref(wifi_interface_t iface, void *data,
                                                        size_t len, void *ref) {
    // Snapshot the classification before ownership can pass to the driver.
    const bool mdns = network_is_mdns((const uint8_t *)data, len);
    const esp_err_t result = __real_esp_wifi_internal_tx_by_ref(iface, data, len, ref);
    return record_tx(result, mdns);
}

NetworkTxSnapshot network_tx_snapshot() {
    return {s_submitted.load(), s_accepted.load(), s_rejected.load(),
            s_mdns_submitted.load(), s_mdns_rejected.load(), s_last_error.load()};
}
