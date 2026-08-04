// Host tests for the pure-function shape check that decides whether
// HomeSpan's WIFIDATA blob represents a provisioned device. Deliberately
// separate from the improv component's tests because this helper is part
// of the application-side boot adapter, not the protocol library.

#include "homespan_wifidata_check.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;
const char *g_current = nullptr;

#define TEST(name) static void name(); \
    struct Registrar_##name { Registrar_##name() { runners_().push_back({#name, name}); } } reg_##name; \
    static void name()

struct Runner { const char *name; void (*fn)(); };
static std::vector<Runner> &runners_() { static std::vector<Runner> r; return r; }

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, g_current, #cond); \
    g_fail++; return; } } while (0)

using homespan::classifyWifiDataBlob;
using homespan::kWifiDataBlobSize;
using homespan::WifiDataStatus;

// Build a canonical blob: fill first bytes with ssid, then a NUL, then pad
// to kWifiDataBlobSize. Zero password.
std::vector<uint8_t> makeBlob(const std::string &ssid) {
    std::vector<uint8_t> b(kWifiDataBlobSize, 0);
    if (!ssid.empty()) {
        assert(ssid.size() <= 32);
        std::memcpy(b.data(), ssid.data(), ssid.size());
    }
    return b;
}

// --- Tests --------------------------------------------------------------

TEST(nullptr_is_malformed) {
    CHECK(classifyWifiDataBlob(nullptr, 0)  == WifiDataStatus::Malformed);
    CHECK(classifyWifiDataBlob(nullptr, 98) == WifiDataStatus::Malformed);
}

TEST(wrong_size_is_malformed) {
    // Simulate a schema drift or corruption: sizes above/below 98.
    std::vector<uint8_t> too_small(50, 0x41);
    std::vector<uint8_t> too_big(200, 0x41);
    too_small[0] = 'x';
    too_big[0]   = 'x';
    CHECK(classifyWifiDataBlob(too_small.data(), too_small.size()) == WifiDataStatus::Malformed);
    CHECK(classifyWifiDataBlob(too_big.data(),   too_big.size())   == WifiDataStatus::Malformed);
}

TEST(empty_ssid_is_unprovisioned) {
    // ssid[0] == 0 is HomeSpan's own criterion for "not configured".
    auto b = makeBlob("");
    CHECK(classifyWifiDataBlob(b.data(), b.size()) == WifiDataStatus::Unprovisioned);
}

TEST(nonempty_ssid_is_provisioned) {
    for (const auto ssid : {"mynet", "a", "some_ssid_32_bytes______________"}) {
        auto b = makeBlob(ssid);
        auto r = classifyWifiDataBlob(b.data(), b.size());
        if (r != WifiDataStatus::Provisioned) {
            std::fprintf(stderr, "  FAIL: ssid=%s classified as %d\n", ssid, (int)r);
            g_fail++;
            return;
        }
    }
}

TEST(ssid_without_terminator_is_malformed) {
    // Fill the ssid field with non-zero bytes and no NUL. A downstream string
    // copy would run off the end -- classify as malformed instead.
    auto b = makeBlob("");
    for (size_t i = 0; i < 33; i++) b[i] = 0xAB;
    CHECK(classifyWifiDataBlob(b.data(), b.size()) == WifiDataStatus::Malformed);
}

TEST(ssid_terminator_at_last_valid_byte_is_provisioned) {
    // NUL at byte 32 (i.e. ssid uses the whole 32-char capacity plus the
    // terminator at index 32). Still valid.
    auto b = makeBlob("");
    for (size_t i = 0; i < 32; i++) b[i] = 'a';
    b[32] = 0;
    CHECK(classifyWifiDataBlob(b.data(), b.size()) == WifiDataStatus::Provisioned);
}

TEST(size_bound_is_exact_98_bytes) {
    // Off-by-one: exactly 97 and exactly 99 must both be malformed even if
    // ssid[0] indicates provisioned.
    std::vector<uint8_t> b97(97, 0);
    std::vector<uint8_t> b99(99, 0);
    b97[0] = 'x'; b97[1] = 0;
    b99[0] = 'x'; b99[1] = 0;
    CHECK(classifyWifiDataBlob(b97.data(), b97.size()) == WifiDataStatus::Malformed);
    CHECK(classifyWifiDataBlob(b99.data(), b99.size()) == WifiDataStatus::Malformed);
}

}  // namespace

int main() {
    for (auto &r : runners_()) {
        g_current = r.name;
        int before = g_fail;
        r.fn();
        if (g_fail == before) {
            g_pass++;
            std::printf("  ok  %s\n", r.name);
            std::fflush(stdout);
        }
    }
    std::printf("\n%d passed, %d failed of %d total\n",
                g_pass, g_fail, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
