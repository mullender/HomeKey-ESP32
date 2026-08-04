#pragma once

// Pure helper for deciding whether a HomeSpan "WIFI/WIFIDATA" NVS blob
// indicates the device has stored Wi-Fi credentials. Isolated from any
// NVS/ESP-IDF dependency so it can be host-tested for the four cases the
// Improv boot decision has to distinguish: missing key, empty ssid,
// non-empty ssid, and malformed/short blob.
//
// This is only the shape-check on the bytes; the NVS-level distinction
// between "key not found" and "key present" is made by the caller before
// calling this function.

#include <cstddef>

namespace homespan {

// HomeSpan writes exactly sizeof(wifiData) = ssid[33] + pwd[65] = 98 bytes.
// A blob of any other length is treated as malformed.
constexpr size_t kWifiDataBlobSize = 98;
constexpr size_t kWifiDataSsidMax  = 32;  // ssid[MAX_SSID+1] with room for NUL

enum class WifiDataStatus {
    Unprovisioned,  // blob is present and ssid[0] == 0 -- Improv may run
    Provisioned,    // blob is present and ssid is non-empty and NUL-terminated
    Malformed,      // wrong size, missing NUL terminator, or otherwise unexpected
};

/**
 * Classify a blob read from NVS "WIFI"/"WIFIDATA".
 *
 * Callers must handle the missing-key case (ESP_ERR_NVS_NOT_FOUND) separately
 * before calling this: a missing key is genuinely unprovisioned and should
 * enable Improv. Any other read error is treated as Malformed and, by policy,
 * must fail closed to console ownership.
 *
 * @return Unprovisioned only when the blob is exactly kWifiDataBlobSize bytes
 *         AND ssid[0] == 0. Provisioned only when the blob is the right size
 *         AND ssid is non-empty AND a NUL exists within the ssid field.
 *         Malformed otherwise.
 */
WifiDataStatus classifyWifiDataBlob(const void *blob, size_t size);

}  // namespace homespan
