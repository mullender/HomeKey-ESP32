// Host tests for the pure NFC reader-type range validator. Regression
// coverage for the WebServerManager captive-portal HTTP boundary, whose
// previous inline check topped out at 1 and silently rejected type 2
// (ST25R3916) -- making the AtomS3 Lite / Unit NFC factory default
// impossible to save through the UI. The HTTP handler itself depends on
// the ESP-IDF HTTP server and cJSON and cannot run off-target; this
// exercises the pure header-only constexpr validator it now delegates to.

#include "nfc_reader_type.hpp"

#include <cstdio>
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

using nfc::isValidReaderType;
using nfc::ReaderType;

// Boundary sweep across -1, 0, 1, 2, 3. Regression guard: the pre-fix
// range in WebServerManager.cpp was 0..1, which silently rejected 2
// (ST25R3916, the AtomS3 Lite / Unit NFC reader).
TEST(rejects_minus_one) {
    CHECK(!isValidReaderType(-1));
}
TEST(accepts_zero_pn532) {
    CHECK(isValidReaderType(0));
    CHECK(isValidReaderType(static_cast<int>(ReaderType::PN532_SPI)));
}
TEST(accepts_one_pn7160) {
    CHECK(isValidReaderType(1));
    CHECK(isValidReaderType(static_cast<int>(ReaderType::PN7160)));
}
TEST(accepts_two_st25r3916) {
    CHECK(isValidReaderType(2));
    CHECK(isValidReaderType(static_cast<int>(ReaderType::ST25R3916_I2C)));
}
TEST(rejects_three) {
    CHECK(!isValidReaderType(3));
}

}  // namespace

int main() {
    std::printf("==> Running test_nfc_reader_type (%zu tests)\n", runners_().size());
    for (auto &r : runners_()) {
        g_current = r.name;
        int before_fail = g_fail;
        r.fn();
        if (g_fail == before_fail) { std::printf("  ok  %s\n", r.name); g_pass++; }
    }
    std::printf("\n%d passed, %d failed of %zu total\n", g_pass, g_fail, runners_().size());
    return g_fail == 0 ? 0 : 1;
}
