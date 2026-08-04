// Host-side unit tests for improv::Parser and framing.
//
// No ESP-IDF, no Arduino, no test framework: a tiny assert-and-count driver
// keeps the build command a single g++ invocation and makes the tests
// portable to any Linux/macOS developer machine.

#include "improv_protocol.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;
const char *g_current = nullptr;

#define TEST(name) static void name(); \
    struct Registrar_##name { Registrar_##name() { runners_().push_back({#name, name}); } } reg_##name; \
    static void name()

struct Runner { const char *name; void (*fn)(); };
static std::vector<Runner> &runners_() { static std::vector<Runner> r; return r; }

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, g_current, #cond); \
        g_fail++; return; \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    auto __a = (a); auto __b = (b); \
    if (!(__a == __b)) { \
        std::fprintf(stderr, "  FAIL %s:%d in %s: expected %lld got %lld\n", \
                     __FILE__, __LINE__, g_current, \
                     static_cast<long long>(__b), static_cast<long long>(__a)); \
        g_fail++; return; \
    } \
} while (0)

// --- Test helpers ---------------------------------------------------------

using improv::Command;
using improv::Error;
using improv::FeedResult;
using improv::PacketType;
using improv::Parser;
using improv::State;

// 8-bit sum, matches Parser's private computeChecksum.
uint8_t sum8(const std::vector<uint8_t> &v) {
    uint32_t s = 0;
    for (auto b : v) s += b;
    return static_cast<uint8_t>(s & 0xFF);
}

// Assemble a raw Improv Serial frame the way a client would, bypassing
// buildFrame() so the test exercises the parser against externally-computed
// bytes. Data is the whole packet payload (i.e. what appears after the
// length byte and before the checksum).
std::vector<uint8_t> makeFrame(PacketType type, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> out{'I','M','P','R','O','V', 1,
                             static_cast<uint8_t>(type),
                             static_cast<uint8_t>(data.size())};
    out.insert(out.end(), data.begin(), data.end());
    out.push_back(sum8(out));
    return out;
}

// Build a WIFI_SETTINGS RPC packet's data payload (what goes inside the
// frame). command(1) + inner_len(1) + ssid_len(1) + ssid + pwd_len(1) + pwd.
std::vector<uint8_t> makeWifiSettingsRpc(const std::string &ssid, const std::string &pwd) {
    std::vector<uint8_t> inner;
    inner.push_back(static_cast<uint8_t>(ssid.size()));
    inner.insert(inner.end(), ssid.begin(), ssid.end());
    inner.push_back(static_cast<uint8_t>(pwd.size()));
    inner.insert(inner.end(), pwd.begin(), pwd.end());

    std::vector<uint8_t> rpc;
    rpc.push_back(static_cast<uint8_t>(Command::WifiSettings));
    rpc.push_back(static_cast<uint8_t>(inner.size()));
    rpc.insert(rpc.end(), inner.begin(), inner.end());
    return rpc;
}

// Feed a byte buffer to the parser; return the first frame-complete result
// (or a default-constructed one if the stream didn't produce one).
FeedResult feedAll(Parser &p, const std::vector<uint8_t> &bytes) {
    FeedResult last;
    for (auto b : bytes) {
        auto r = p.feed(b);
        if (r.frame_complete) last = r;
    }
    return last;
}

// --- Tests ----------------------------------------------------------------

TEST(preamble_only_advances_on_correct_bytes) {
    Parser p;
    // Feed junk before the preamble; position must stay at 0 (or 1 if 'I').
    for (uint8_t b : {'X', 'Y', 'Z'}) {
        auto r = p.feed(b);
        CHECK(!r.frame_complete);
    }
    CHECK_EQ(p.position(), 0u);
    // Now an 'I' followed by a wrong second byte -- position should reset,
    // and if the wrong byte is itself an 'I' the parser must retain it.
    p.feed('I');
    CHECK_EQ(p.position(), 1u);
    p.feed('X');
    CHECK_EQ(p.position(), 0u);
    p.feed('I');
    p.feed('I');
    CHECK_EQ(p.position(), 1u);  // second 'I' is treated as a fresh start
}

TEST(happy_path_wifi_settings) {
    auto rpc = makeWifiSettingsRpc("mynet", "s3cret");
    auto frame = makeFrame(PacketType::Rpc, rpc);
    Parser p;
    auto res = feedAll(p, frame);
    CHECK(res.frame_complete);
    CHECK(res.error == Error::None);
    CHECK(res.command == Command::WifiSettings);
    CHECK_EQ(res.ssid.size(), std::string("mynet").size());
    CHECK(res.ssid == "mynet");
    CHECK(res.password == "s3cret");
}

TEST(partial_frames_arriving_one_byte_at_a_time_still_parse) {
    auto rpc = makeWifiSettingsRpc("net", "pw");
    auto frame = makeFrame(PacketType::Rpc, rpc);
    Parser p;
    FeedResult last;
    for (size_t i = 0; i < frame.size(); i++) {
        auto r = p.feed(frame[i]);
        if (r.frame_complete) last = r;
        // Nothing should complete before the final byte.
        if (i + 1 < frame.size()) CHECK(!r.frame_complete);
    }
    CHECK(last.frame_complete);
    CHECK(last.command == Command::WifiSettings);
    CHECK(last.ssid == "net");
    CHECK(last.password == "pw");
}

TEST(partial_frames_in_random_chunks) {
    // Deterministic RNG: reproducible failures.
    std::mt19937 rng(0xdeadbeef);
    auto rpc = makeWifiSettingsRpc("some_ssid_32_bytes______________",  // 32 chars = MAX_SSID
                                   std::string(64, 'p'));               // 64 chars = MAX_PWD
    auto frame = makeFrame(PacketType::Rpc, rpc);
    Parser p;
    FeedResult last;
    size_t off = 0;
    while (off < frame.size()) {
        size_t chunk = 1 + (rng() % 8);
        chunk = std::min(chunk, frame.size() - off);
        for (size_t i = 0; i < chunk; i++) {
            auto r = p.feed(frame[off + i]);
            if (r.frame_complete) last = r;
        }
        off += chunk;
    }
    CHECK(last.frame_complete);
    CHECK(last.command == Command::WifiSettings);
    CHECK_EQ(last.ssid.size(), 32u);
    CHECK_EQ(last.password.size(), 64u);
}

TEST(log_noise_before_a_frame_is_discarded) {
    auto rpc = makeWifiSettingsRpc("mynet", "pw");
    auto frame = makeFrame(PacketType::Rpc, rpc);
    Parser p;
    // Prepend a plausible ESP_LOG line.
    std::string log = "I (1234) NfcManager: Auth precompute: ready in 68 ms\n";
    for (char c : log) {
        auto r = p.feed(static_cast<uint8_t>(c));
        CHECK(!r.frame_complete);
    }
    auto res = feedAll(p, frame);
    CHECK(res.frame_complete);
    CHECK(res.command == Command::WifiSettings);
    CHECK(res.ssid == "mynet");
}

TEST(bad_checksum_reports_invalid_and_resyncs) {
    auto rpc = makeWifiSettingsRpc("mynet", "pw");
    auto frame = makeFrame(PacketType::Rpc, rpc);
    frame.back() ^= 0xFF;  // corrupt checksum
    Parser p;
    auto res = feedAll(p, frame);
    CHECK(res.frame_complete);
    CHECK(res.error == Error::InvalidRpc);
    CHECK(res.command == Command::Unknown);
    // Parser must be resynced afterwards -- a valid frame should now parse.
    auto good = makeFrame(PacketType::Rpc, makeWifiSettingsRpc("net", ""));
    auto r2 = feedAll(p, good);
    CHECK(r2.frame_complete);
    CHECK(r2.command == Command::WifiSettings);
    CHECK(r2.password.empty());
}

TEST(bad_version_reports_invalid) {
    auto rpc = makeWifiSettingsRpc("mynet", "pw");
    auto frame = makeFrame(PacketType::Rpc, rpc);
    frame[6] = 99;  // wrong version
    // Recompute checksum so the version failure is the only issue.
    frame.back() = 0;
    frame.back() = sum8(std::vector<uint8_t>(frame.begin(), frame.end() - 1));
    Parser p;
    auto res = feedAll(p, frame);
    CHECK(res.frame_complete);
    CHECK(res.error == Error::InvalidRpc);
}

TEST(unknown_command_reports_unknown_rpc) {
    // command 0xFE is not defined.
    std::vector<uint8_t> rpc{0xFE, 0x00};
    auto frame = makeFrame(PacketType::Rpc, rpc);
    Parser p;
    auto res = feedAll(p, frame);
    CHECK(res.frame_complete);
    CHECK(res.error == Error::UnknownRpc);
    CHECK(res.command == Command::Unknown);
}

TEST(wifi_settings_with_empty_password_open_network) {
    auto rpc = makeWifiSettingsRpc("openap", "");
    auto frame = makeFrame(PacketType::Rpc, rpc);
    Parser p;
    auto res = feedAll(p, frame);
    CHECK(res.frame_complete);
    CHECK(res.command == Command::WifiSettings);
    CHECK(res.ssid == "openap");
    CHECK(res.password.empty());
}

TEST(wifi_settings_inner_length_mismatch_is_rejected) {
    auto rpc = makeWifiSettingsRpc("mynet", "pw");
    // Lie about the inner RPC data length (byte 1 of the payload).
    rpc[1] = 99;
    auto frame = makeFrame(PacketType::Rpc, rpc);
    Parser p;
    auto res = feedAll(p, frame);
    CHECK(res.frame_complete);
    CHECK(res.error == Error::InvalidRpc);
}

TEST(get_current_state_command_recognised) {
    std::vector<uint8_t> rpc{
        static_cast<uint8_t>(Command::GetCurrentState),
        0x00,
    };
    auto frame = makeFrame(PacketType::Rpc, rpc);
    Parser p;
    auto res = feedAll(p, frame);
    CHECK(res.frame_complete);
    CHECK(res.error == Error::None);
    CHECK(res.command == Command::GetCurrentState);
}

TEST(non_rpc_frames_are_ignored_and_state_resets) {
    // Send an ErrorState frame at us; parser accepts framing but produces no
    // command and resyncs.
    auto frame = makeFrame(PacketType::ErrorState, {static_cast<uint8_t>(Error::None)});
    Parser p;
    for (auto b : frame) {
        auto r = p.feed(b);
        CHECK(!r.frame_complete);
    }
    CHECK_EQ(p.position(), 0u);
    // A valid RPC after that still parses.
    auto rpc = makeWifiSettingsRpc("net", "pw");
    auto rpc_frame = makeFrame(PacketType::Rpc, rpc);
    auto res = feedAll(p, rpc_frame);
    CHECK(res.frame_complete);
    CHECK(res.command == Command::WifiSettings);
}

// --- Framer tests --------------------------------------------------------

TEST(build_state_packet_matches_wire_format) {
    auto pkt = improv::buildStatePacket(State::Authorized);
    // IMPROV(6) + ver(1) + type(1) + len(1) + data(1) + checksum(1) = 11
    CHECK_EQ(pkt.size(), 11u);
    CHECK_EQ(pkt[0], 'I');
    CHECK_EQ(pkt[5], 'V');
    CHECK_EQ(pkt[6], 1);            // version
    CHECK_EQ(pkt[7], 0x01);         // TYPE_CURRENT_STATE
    CHECK_EQ(pkt[8], 1);            // data length
    CHECK_EQ(pkt[9], 0x02);         // STATE_AUTHORIZED
    CHECK_EQ(pkt[10], sum8(std::vector<uint8_t>(pkt.begin(), pkt.end() - 1)));
}

TEST(build_error_packet_matches_wire_format) {
    auto pkt = improv::buildErrorPacket(Error::UnableToConnect);
    CHECK_EQ(pkt.size(), 11u);
    CHECK_EQ(pkt[7], 0x02);         // TYPE_ERROR_STATE
    CHECK_EQ(pkt[9], 0x03);         // ERROR_UNABLE_TO_CONNECT
}

TEST(build_rpc_response_wifi_settings_with_url) {
    auto body = improv::buildRpcResponse(Command::WifiSettings, {"http://192.168.1.42/"});
    // [0] command 0x01, [1] inner length, [2] str length, [3..] "http://..."
    CHECK_EQ(body[0], 0x01);
    CHECK_EQ(body[1], 21u);          // 1 length byte + 20 chars
    CHECK_EQ(body[2], 20u);
    CHECK(std::string(body.begin() + 3, body.begin() + 3 + 20) == "http://192.168.1.42/");
}

TEST(build_rpc_response_wifi_settings_empty_url) {
    // Spec: "If there is no URL, omit the entry or add an empty string."
    auto body = improv::buildRpcResponse(Command::WifiSettings, {""});
    CHECK_EQ(body[0], 0x01);
    CHECK_EQ(body[1], 1u);          // one length byte, zero string bytes
    CHECK_EQ(body[2], 0u);
    CHECK_EQ(body.size(), 3u);      // command + inner_len + str_len
}

TEST(build_frame_rejects_data_over_255_bytes) {
    // The length header is a single uint8_t. Silent truncation would emit an
    // internally inconsistent frame; the framer must refuse instead.
    std::vector<uint8_t> too_big(256, 0xAB);
    auto out = improv::buildFrame(PacketType::Rpc, too_big);
    CHECK(out.empty());
    // Exactly 255 must still succeed.
    std::vector<uint8_t> at_limit(255, 0xCD);
    auto ok = improv::buildFrame(PacketType::Rpc, at_limit);
    CHECK(!ok.empty());
    CHECK_EQ(ok[8], 255);
    CHECK_EQ(ok.size(), 10u + 255u);
}

TEST(build_rpc_response_rejects_single_string_over_255) {
    std::string huge(256, 'x');
    auto out = improv::buildRpcResponse(Command::WifiSettings, {huge});
    CHECK(out.empty());
}

TEST(build_rpc_response_rejects_aggregate_over_255) {
    // Ten strings, 30 chars each, plus one length byte per: 310 bytes > 255.
    std::vector<std::string> many(10, std::string(30, 'x'));
    auto out = improv::buildRpcResponse(Command::WifiSettings, many);
    CHECK(out.empty());
}

TEST(build_rpc_response_at_aggregate_limit) {
    // Exactly 255 bytes: 5 strings of 50 chars each plus 5 length bytes = 255.
    std::vector<std::string> at_limit(5, std::string(50, 'y'));
    auto out = improv::buildRpcResponse(Command::WifiSettings, at_limit);
    CHECK(!out.empty());
    CHECK_EQ(out[1], 255);  // inner length
    CHECK_EQ(out.size(), 2u + 255u);
}

TEST(round_trip_build_then_parse) {
    // Build a WIFI_SETTINGS frame using our framer, feed it back through
    // Parser. Guarantees the two agree on the wire format.
    auto rpc = makeWifiSettingsRpc("myssid", "mypassword");
    auto frame = improv::buildFrame(PacketType::Rpc, rpc);
    Parser p;
    auto res = feedAll(p, frame);
    CHECK(res.frame_complete);
    CHECK(res.command == Command::WifiSettings);
    CHECK(res.ssid == "myssid");
    CHECK(res.password == "mypassword");
}

// --- Credential-safety test ---------------------------------------------

TEST(parser_does_not_leak_bytes_to_stdout_or_stderr) {
    // The parser must not print anything. Redirect stdout/stderr to a file,
    // feed a distinctive credential, then grep. This catches accidental
    // printf/fprintf debug lines.
    FILE *out_backup = tmpfile();
    FILE *err_backup = tmpfile();
    CHECK(out_backup != nullptr);
    CHECK(err_backup != nullptr);
    int saved_out = dup(fileno(stdout));
    int saved_err = dup(fileno(stderr));
    dup2(fileno(out_backup), fileno(stdout));
    dup2(fileno(err_backup), fileno(stderr));

    Parser p;
    auto rpc = makeWifiSettingsRpc("__DISTINCTIVE_SSID__", "__DISTINCTIVE_PASSWORD_12345__");
    auto frame = improv::buildFrame(PacketType::Rpc, rpc);
    (void) feedAll(p, frame);

    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    close(saved_out);
    close(saved_err);

    auto scan = [](FILE *f) -> std::string {
        rewind(f);
        std::string s;
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        return s;
    };
    std::string out_captured = scan(out_backup);
    std::string err_captured = scan(err_backup);
    fclose(out_backup);
    fclose(err_backup);

    CHECK(out_captured.find("__DISTINCTIVE_SSID__") == std::string::npos);
    CHECK(err_captured.find("__DISTINCTIVE_SSID__") == std::string::npos);
    CHECK(out_captured.find("__DISTINCTIVE_PASSWORD_12345__") == std::string::npos);
    CHECK(err_captured.find("__DISTINCTIVE_PASSWORD_12345__") == std::string::npos);
}

}  // namespace

int main() {
    for (auto &r : runners_()) {
        g_current = r.name;
        int before_fail = g_fail;
        r.fn();
        if (g_fail == before_fail) {
            g_pass++;
            std::printf("  ok  %s\n", r.name);
            std::fflush(stdout);
        }
    }
    std::printf("\n%d passed, %d failed of %d total\n",
                g_pass, g_fail, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
