#pragma once

// Range validator for the "nfcReaderType" field accepted by the
// captive-portal configuration API. WebServerManager was silently rejecting
// type 2 (ST25R3916) because its inline range check topped out at 1, which
// made the AtomS3 Lite / Unit NFC factory default impossible to save
// through the UI. This header pulls the range out of that HTTP handler so
// the check is testable off-target.
//
// NfcManager and ConfigManager continue to store the value as a raw uint8_t
// -- this header intentionally does not push an enum into them.

#include <cstdint>

namespace nfc {

enum class ReaderType : uint8_t {
    PN532_SPI     = 0,
    PN7160        = 1,
    ST25R3916_I2C = 2,
};

constexpr bool isValidReaderType(int v) noexcept {
    return v >= static_cast<int>(ReaderType::PN532_SPI) &&
           v <= static_cast<int>(ReaderType::ST25R3916_I2C);
}

}  // namespace nfc
