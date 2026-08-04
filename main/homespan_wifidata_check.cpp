#include "homespan_wifidata_check.hpp"

#include <cstdint>
#include <cstring>

namespace homespan {

WifiDataStatus classifyWifiDataBlob(const void *blob, size_t size) {
    if (blob == nullptr || size != kWifiDataBlobSize) {
        return WifiDataStatus::Malformed;
    }
    const auto *bytes = static_cast<const uint8_t *>(blob);

    // ssid[0] == 0 is HomeSpan's own criterion for "not configured", written
    // as the default when it opens the namespace fresh.
    if (bytes[0] == 0) {
        return WifiDataStatus::Unprovisioned;
    }

    // Provisioned: ssid is non-empty. Insist on a NUL somewhere in the ssid
    // field so downstream string handling is safe -- a blob whose ssid runs
    // off the end of its 33-byte slot is malformed, not a valid credential.
    const void *nul = std::memchr(bytes, 0, kWifiDataSsidMax + 1);
    if (nul == nullptr) {
        return WifiDataStatus::Malformed;
    }
    return WifiDataStatus::Provisioned;
}

}  // namespace homespan
