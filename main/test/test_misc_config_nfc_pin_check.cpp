// Host tests for webcfg::decideNfcPin -- the narrow per-pin decision the
// captive-portal misc-config validator delegates to. Covers the four
// scenarios the WebServerManager patch must get right.

#include "misc_config_nfc_pin_check.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;
const char *g_current = nullptr;

struct Runner { const char *name; void (*fn)(); };
static std::vector<Runner> &runners_() { static std::vector<Runner> r; return r; }

#define TEST(name) static void name(); \
    struct Registrar_##name { Registrar_##name() { runners_().push_back({#name, name}); } } reg_##name; \
    static void name()

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, g_current, #cond); \
    g_fail++; return; } } while (0)

using webcfg::decideNfcPin;
using webcfg::NfcPinDecision;

TEST(unchanged_full_form_pin_is_ok_even_when_currently_owned) {
    // Immediate bug repro: AtomS3 Lite installer boot has GPIO 2 owned
    // as I2C_SDA. User saves the misc form without changing anything;
    // incoming == current so no ownership question arises.
    auto d = decideNfcPin(/*incoming*/2, /*current*/2,
                          std::optional<std::string>("I2C_SDA"),
                          /*override_strapping*/false);
    CHECK(d == NfcPinDecision::Ok);
}

TEST(switch_reader_type_reuses_old_nfc_lease_pin_is_ok) {
    // Atomic-replacement scenario. Only one NfcManager is ever live at
    // once; before this save it is PN532 SPI, owning GPIO 5 as SPI2_SS.
    // User switches to ST25 I2C and configures GPIO 5 as SDA (a new
    // custom pin, incoming != current). decideNfcPin accepts it: the
    // current SPI2_SS lease is NFC-owned and gets released on the save's
    // reboot before the new I2C_SDA lease is taken at construction.
    auto d = decideNfcPin(/*incoming*/5, /*current*/16,
                          std::optional<std::string>("SPI2_SS"),
                          false);
    CHECK(d == NfcPinDecision::Ok);
}

TEST(changed_pin_conflicting_with_unrelated_owner_is_rejected) {
    // Proposed NFC pin GPIO 35 is the AtomS3 Lite NeoPixel data pin,
    // owned by HardwareManager as NEOPIXEL_PIN. Not NFC-owned, not
    // strapping.
    auto d = decideNfcPin(/*incoming*/35, /*current*/2,
                          std::optional<std::string>("NEOPIXEL_PIN"),
                          false);
    CHECK(d == NfcPinDecision::Conflict);
}

TEST(eth_spi_owned_pin_is_not_exempted_as_nfc) {
    // Regression for the pre-fix owner->contains("SPI") substring
    // exemption that used to accept unrelated SPI owners. ETH_SPI_MISO
    // is NOT in kNfcOwnerNames, so an NFC pin colliding with it must be
    // Conflict.
    auto d = decideNfcPin(/*incoming*/12, /*current*/2,
                          std::optional<std::string>("ETH_SPI_MISO"),
                          false);
    CHECK(d == NfcPinDecision::Conflict);
}

TEST(free_incoming_pin_is_ok) {
    auto d = decideNfcPin(/*incoming*/10, /*current*/2, std::nullopt, false);
    CHECK(d == NfcPinDecision::Ok);
}

TEST(strapping_incoming_pin_only_ok_with_override) {
    auto owner = std::optional<std::string>("STRAPPING");
    CHECK(decideNfcPin(45, 2, owner, false) == NfcPinDecision::Conflict);
    CHECK(decideNfcPin(45, 2, owner, true)  == NfcPinDecision::Ok);
}

}  // namespace

int main() {
    std::printf("==> Running test_misc_config_nfc_pin_check (%zu tests)\n", runners_().size());
    for (auto &r : runners_()) {
        g_current = r.name;
        int before_fail = g_fail;
        r.fn();
        if (g_fail == before_fail) { std::printf("  ok  %s\n", r.name); g_pass++; }
    }
    std::printf("\n%d passed, %d failed of %zu total\n", g_pass, g_fail, runners_().size());
    return g_fail == 0 ? 0 : 1;
}
