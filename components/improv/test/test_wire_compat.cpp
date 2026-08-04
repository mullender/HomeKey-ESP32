// Interoperability tests for our device-side framer against a compact port
// of improv-wifi/sdk-serial-js `_processInput` (serial.ts:527-577).
//
//   - baseline: our success stream parses cleanly on the client side.
//   - regression: a partial (unterminated) log line drops the client into
//     its "discard until \\n" state; the first Improv frame that follows
//     is swallowed, but the trailing 0x0A we emit after that swallowed
//     frame recovers the parser so later frames still land. Removing the
//     0x0A from buildFrame flips this test to failing; that is the
//     mutation-sensitive coverage.

#include "improv_protocol.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

using improv::buildStatePacket;
using improv::buildErrorPacket;
using improv::buildFrame;
using improv::buildRpcResponse;
using improv::State;
using improv::Error;
using improv::PacketType;
using improv::Command;

// Compact port of the sdk-serial-js reader. `states` and `urls` are all the
// tests need to observe.
class SdkParser {
public:
    std::vector<uint8_t>     states;   // one push per CURRENT_STATE frame
    std::vector<std::string> urls;     // one push per WIFI_SETTINGS RPC_RESULT string
    int checksum_failures = 0;

    void feed(const std::vector<uint8_t> &bytes) {
        for (uint8_t byte : bytes) feedByte(byte);
    }

private:
    int isImprov_ = -1;      // -1 undefined, 0 false (discard until \n), 1 true
    std::vector<uint8_t> line_;
    size_t improvLength_ = 0;

    void feedByte(uint8_t byte) {
        if (isImprov_ == 0) { if (byte == 10) isImprov_ = -1; return; }
        if (isImprov_ == 1) {
            line_.push_back(byte);
            if (line_.size() == improvLength_) { handle(line_); isImprov_ = -1; line_.clear(); }
            return;
        }
        if (byte == 10) { line_.clear(); return; }
        line_.push_back(byte);
        if (line_.size() != 9) return;
        static const uint8_t improv[6] = {'I','M','P','R','O','V'};
        if (std::memcmp(line_.data(), improv, 6) != 0) { isImprov_ = 0; line_.clear(); return; }
        isImprov_ = 1;
        improvLength_ = 9 + line_[8] + 1;
    }

    void handle(const std::vector<uint8_t> &line) {
        if (line[6] != 1) return;
        uint32_t sum = 0;
        for (size_t i = 0; i + 1 < line.size(); ++i) sum += line[i];
        if (static_cast<uint8_t>(sum & 0xff) != line[9 + line[8]]) { checksum_failures++; return; }
        uint8_t type = line[7], len = line[8];
        const uint8_t *data = &line[9];
        if (type == 0x01) states.push_back(data[0]);
        else if (type == 0x04 && data[0] == static_cast<uint8_t>(Command::WifiSettings)) {
            // command(1) + inner_len(1) + [Pascal strings]. Only the first
            // string (URL) is what our tests care about.
            uint8_t inner = data[1];
            if (inner >= 1) {
                uint8_t sl = data[2];
                urls.emplace_back(reinterpret_cast<const char *>(data + 3), sl);
            }
            (void)len;
        }
    }
};

std::vector<uint8_t> successStream(const std::string &url) {
    std::vector<uint8_t> out;
    auto add = [&](const std::vector<uint8_t> &v) { out.insert(out.end(), v.begin(), v.end()); };
    add(buildErrorPacket(Error::None));
    add(buildStatePacket(State::Provisioning));
    add(buildStatePacket(State::Provisioned));
    add(buildFrame(PacketType::RpcResponse,
                   buildRpcResponse(Command::WifiSettings, {url})));
    return out;
}

// ---------------------------------------------------------------------------

TEST(no_noise_success_stream_reaches_client) {
    SdkParser p;
    p.feed(successStream("http://192.168.1.60/"));
    CHECK(p.checksum_failures == 0);
    CHECK(p.states.size() == 2u);
    CHECK(p.states[0] == static_cast<uint8_t>(State::Provisioning));
    CHECK(p.states[1] == static_cast<uint8_t>(State::Provisioned));
    CHECK(p.urls.size() == 1u);
    CHECK(p.urls[0] == "http://192.168.1.60/");
}

TEST(partial_log_before_frames_first_is_lost_but_later_frames_recover) {
    // Regression proof for the trailing-0x0A change. A log line without
    // its terminating \n parks the client parser in "discard". The next
    // device frame's preamble is swallowed. Our LF at the end of that
    // swallowed frame is what re-arms the parser so subsequent frames
    // parse. Mutation: remove the push_back(0x0A) from buildFrame and
    // this test fails -- every subsequent frame is also lost.
    SdkParser p;
    std::vector<uint8_t> bytes;
    auto add = [&](const std::vector<uint8_t> &v) { bytes.insert(bytes.end(), v.begin(), v.end()); };
    std::string partial = "I (1000) some_component: partial log line still in flight";
    bytes.insert(bytes.end(), partial.begin(), partial.end());
    // First pair: State swallowed, RpcResponse recovered by State's LF.
    add(buildStatePacket(State::Provisioned));
    add(buildFrame(PacketType::RpcResponse,
                   buildRpcResponse(Command::WifiSettings, {"http://x/"})));
    // Second pair: both delivered.
    add(buildStatePacket(State::Provisioned));
    add(buildFrame(PacketType::RpcResponse,
                   buildRpcResponse(Command::WifiSettings, {"http://x/"})));
    p.feed(bytes);

    CHECK(p.checksum_failures == 0);
    // Exactly one Provisioned event: the first State frame was eaten.
    CHECK(p.states.size() == 1u);
    CHECK(p.states[0] == static_cast<uint8_t>(State::Provisioned));
    // Both RpcResponse frames delivered.
    CHECK(p.urls.size() == 2u);
    CHECK(p.urls[0] == "http://x/");
    CHECK(p.urls[1] == "http://x/");
}

TEST(newline_terminated_log_lines_between_frames_do_not_disturb_parsing) {
    // General compatibility only -- passes with or without our trailing
    // 0x0A, because each log line already terminates itself.
    SdkParser p;
    auto logLine = [](const std::string &s) {
        std::vector<uint8_t> v(s.begin(), s.end()); v.push_back(0x0A); return v;
    };
    p.feed(logLine("I (1234) log a"));
    p.feed(buildStatePacket(State::Provisioning));
    p.feed(logLine("I (1235) log b"));
    p.feed(buildStatePacket(State::Provisioned));
    p.feed(buildFrame(PacketType::RpcResponse,
                     buildRpcResponse(Command::WifiSettings, {"http://y/"})));
    CHECK(p.checksum_failures == 0);
    CHECK(p.states.size() == 2u);
    CHECK(p.urls.size() == 1u);
    CHECK(p.urls[0] == "http://y/");
}

}  // namespace

int main() {
    std::printf("==> Running test_wire_compat (%zu tests)\n", runners_().size());
    for (auto &r : runners_()) {
        g_current = r.name;
        int before_fail = g_fail;
        r.fn();
        if (g_fail == before_fail) { std::printf("  ok  %s\n", r.name); g_pass++; }
    }
    std::printf("\n%d passed, %d failed of %zu total\n", g_pass, g_fail, runners_().size());
    return g_fail == 0 ? 0 : 1;
}
