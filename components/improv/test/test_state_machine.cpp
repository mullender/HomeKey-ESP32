// Host-side tests for improv::StateMachine.
//
// Drives the class with scripted transports and a virtual clock. Nothing
// links against ESP-IDF; the log macros collapse to no-ops in the state
// machine .cpp when esp_log.h is absent, which is exactly the host build
// environment.

#include "improv_protocol.hpp"
#include "improv_state_machine.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
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

#define CHECK_EQ(a, b) do { auto __a = (a); auto __b = (b); if (!(__a == __b)) { \
    std::fprintf(stderr, "  FAIL %s:%d in %s: expected %lld got %lld\n", \
                 __FILE__, __LINE__, g_current, (long long)__b, (long long)__a); \
    g_fail++; return; } } while (0)

// --- Test doubles -------------------------------------------------------

// A scripted transport: read_byte pulls from a queue, write_bytes appends to
// a captured buffer. Both are set as std::function<> for the state machine.
struct FakeTransport {
    std::deque<uint8_t> incoming;
    std::vector<uint8_t> outgoing;

    improv::SerialIo io() {
        return {
            .read_byte = [this]() -> int {
                if (incoming.empty()) return -1;
                int b = incoming.front();
                incoming.pop_front();
                return b;
            },
            .write_bytes = [this](const uint8_t *d, size_t n) {
                outgoing.insert(outgoing.end(), d, d + n);
            },
        };
    }

    void push(const std::vector<uint8_t> &bytes) {
        incoming.insert(incoming.end(), bytes.begin(), bytes.end());
    }
};

// A scripted Wi-Fi driver. Tests set is_connected_ / failed_ directly and
// count calls to begin/disconnect so ordering can be asserted.
struct FakeWifi {
    bool is_connected_ = false;
    bool failed_       = false;
    std::string last_ssid;
    std::string last_password;
    int begin_calls = 0;
    int disconnect_calls = 0;
    std::string url = "http://192.168.1.42/";

    improv::WifiDriver driver() {
        return {
            .begin = [this](const std::string &s, const std::string &p) {
                last_ssid = s; last_password = p; begin_calls++;
            },
            .is_connected = [this]() { return is_connected_; },
            .failed_hard  = [this]() { return failed_; },
            .device_url   = [this]() { return url; },
            .disconnect   = [this]() { disconnect_calls++; },
        };
    }
};

// A virtual clock. Tests advance it explicitly; loop() reads from it.
struct FakeClock {
    int64_t us = 0;
    std::function<int64_t()> as_fn() { return [this]() { return us; }; }
    void advance(int64_t d) { us += d; }
};

// Records the persist and complete callbacks. persist can be scripted to
// fail so the state machine's persistence-failure path is exercised.
struct ProvisionRecorder {
    bool persist_called = false;
    bool complete_called = false;
    bool persist_result = true;         // default: persist succeeds
    std::string ssid, password;

    // Order-of-events log: 'P' = persist, 'C' = complete. The state machine
    // guarantees a specific ordering with respect to Serial writes; the
    // captured outgoing buffer is checkpointed at each callback so tests can
    // assert what had been written to the wire by the time persist / complete
    // fired.
    std::vector<char> events;
    size_t bytes_written_at_persist  = 0;
    size_t bytes_written_at_complete = 0;

    std::function<bool(const std::string &, const std::string &)>
    persistFn(FakeTransport &t) {
        return [this, &t](const std::string &s, const std::string &p) {
            persist_called = true;
            ssid = s; password = p;
            events.push_back('P');
            bytes_written_at_persist = t.outgoing.size();
            return persist_result;
        };
    }
    // Deliberately returns (unlike on target which reboots), so the state
    // machine's post-callback fallback path is exercised.
    std::function<void()> completeFn(FakeTransport &t) {
        return [this, &t]() {
            complete_called = true;
            events.push_back('C');
            bytes_written_at_complete = t.outgoing.size();
        };
    }
};

// Compose a full test config wired to the doubles above.
improv::SerialConfig makeCfg(FakeTransport &t, FakeWifi &w, FakeClock &c,
                             ProvisionRecorder &r) {
    return {
        .firmware_name    = "HomeKey-ESP32",
        .firmware_version = "test-1.0",
        .chip_family      = "ESP32-S3",
        .device_name      = "HomeKey-ABCDEF",
        .io = t.io(),
        .wifi = w.driver(),
        .persist  = r.persistFn(t),
        .complete = r.completeFn(t),
        .now_us = c.as_fn(),
    };
}

// --- Wire helpers -------------------------------------------------------

uint8_t sum8(const std::vector<uint8_t> &v) {
    uint32_t s = 0; for (auto b : v) s += b; return (uint8_t)(s & 0xFF);
}

std::vector<uint8_t> makeFrame(improv::PacketType type,
                               const std::vector<uint8_t> &data) {
    std::vector<uint8_t> out{'I','M','P','R','O','V', 1,
                             (uint8_t)type, (uint8_t)data.size()};
    out.insert(out.end(), data.begin(), data.end());
    out.push_back(sum8(out));
    return out;
}

std::vector<uint8_t> makeWifiSettings(const std::string &ssid, const std::string &pwd) {
    std::vector<uint8_t> inner;
    inner.push_back((uint8_t)ssid.size());
    inner.insert(inner.end(), ssid.begin(), ssid.end());
    inner.push_back((uint8_t)pwd.size());
    inner.insert(inner.end(), pwd.begin(), pwd.end());

    std::vector<uint8_t> rpc;
    rpc.push_back((uint8_t)improv::Command::WifiSettings);
    rpc.push_back((uint8_t)inner.size());
    rpc.insert(rpc.end(), inner.begin(), inner.end());
    return makeFrame(improv::PacketType::Rpc, rpc);
}

std::vector<uint8_t> makeSimpleRpc(improv::Command c) {
    std::vector<uint8_t> rpc{(uint8_t)c, 0x00};
    return makeFrame(improv::PacketType::Rpc, rpc);
}

// Split the captured outbound stream into whole Improv frames so tests can
// reason about "packet 3" instead of counting bytes. Any bytes that don't
// belong to a frame are surfaced as an empty vector marker for the caller.
std::vector<std::vector<uint8_t>> splitFrames(const std::vector<uint8_t> &buf) {
    std::vector<std::vector<uint8_t>> frames;
    size_t i = 0;
    while (i + 10 <= buf.size()) {
        if (std::memcmp(&buf[i], "IMPROV", 6) != 0) { i++; continue; }
        if (buf[i + 6] != 1) { i++; continue; }
        uint8_t data_len = buf[i + 8];
        size_t total = 10 + data_len;
        if (i + total > buf.size()) break;
        frames.push_back(std::vector<uint8_t>(buf.begin() + i, buf.begin() + i + total));
        i += total;
    }
    return frames;
}

// --- Assertions on captured frames -------------------------------------

bool frameIsCurrentState(const std::vector<uint8_t> &f, improv::State expected) {
    if (f.size() < 11) return false;
    if (f[7] != (uint8_t)improv::PacketType::CurrentState) return false;
    if (f[8] != 1) return false;
    return f[9] == (uint8_t)expected;
}

bool frameIsError(const std::vector<uint8_t> &f, improv::Error expected) {
    if (f.size() < 11) return false;
    if (f[7] != (uint8_t)improv::PacketType::ErrorState) return false;
    if (f[8] != 1) return false;
    return f[9] == (uint8_t)expected;
}

bool frameIsRpcResponse(const std::vector<uint8_t> &f, improv::Command expected) {
    if (f.size() < 11) return false;
    if (f[7] != (uint8_t)improv::PacketType::RpcResponse) return false;
    return f[9] == (uint8_t)expected;
}

// --- Tests --------------------------------------------------------------

TEST(begin_sends_authorized_state) {
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));

    auto frames = splitFrames(t.outgoing);
    CHECK_EQ(frames.size(), 1u);
    CHECK(frameIsCurrentState(frames[0], improv::State::Authorized));
}

TEST(successful_flow_persist_then_state_then_response_then_complete) {
    // Reference ordering per ESPHome improv_serial_component.cpp:53-63:
    // persist first (must succeed before ANY success signal on the wire),
    // then Provisioned state, then WIFI_SETTINGS RPC response, then the
    // adapter completion callback.
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));
    t.outgoing.clear();  // ignore the initial Authorized frame

    // Push WIFI_SETTINGS
    t.push(makeWifiSettings("mynet", "s3cret"));
    sm.loop();

    CHECK_EQ(w.begin_calls, 1);
    CHECK(w.last_ssid == "mynet");
    CHECK(w.last_password == "s3cret");
    auto frames = splitFrames(t.outgoing);
    // Pre-clear + Provisioning state emitted on receipt.
    CHECK_EQ(frames.size(), 2u);
    CHECK(frameIsError(frames[0], improv::Error::None));
    CHECK(frameIsCurrentState(frames[1], improv::State::Provisioning));
    // Nothing has been persisted or completed yet.
    CHECK(!r.persist_called);
    CHECK(!r.complete_called);

    // Simulate the AP taking a moment to associate.
    c.advance(2 * 1000 * 1000);
    sm.loop();
    CHECK_EQ(splitFrames(t.outgoing).size(), 2u);
    CHECK(!r.persist_called);
    CHECK(!r.complete_called);

    // Now connected.
    w.is_connected_ = true;
    sm.loop();

    // Full sequence at this point:
    //   [0] Error::None            (pre-clear on WIFI_SETTINGS receipt)
    //   [1] State::Provisioning    (from handleWifiSettings)
    //   [2] State::Provisioned     (AFTER persist succeeded)
    //   [3] RpcResponse WifiSettings (URL)
    // and callbacks: persist BEFORE any success signal, complete AFTER both.
    frames = splitFrames(t.outgoing);
    CHECK_EQ(frames.size(), 4u);
    if (frames.size() < 4u) return;
    CHECK(frameIsError(frames[0], improv::Error::None));
    CHECK(frameIsCurrentState(frames[1], improv::State::Provisioning));
    CHECK(frameIsCurrentState(frames[2], improv::State::Provisioned));
    CHECK(frameIsRpcResponse(frames[3], improv::Command::WifiSettings));

    // URL check on the RPC response.
    const auto &resp = frames[3];
    CHECK(resp.size() > 12u);
    uint8_t str_len = resp[11];
    std::string url(resp.begin() + 12, resp.begin() + 12 + str_len);
    CHECK(url == "http://192.168.1.42/");

    // Order of callbacks: persist, then complete.
    CHECK(r.persist_called);
    CHECK(r.complete_called);
    CHECK_EQ(r.events.size(), 2u);
    if (r.events.size() >= 2u) {
        CHECK_EQ(r.events[0], 'P');
        CHECK_EQ(r.events[1], 'C');
    }

    // Enforce the critical invariant: at the moment persist fires, no
    // Provisioned state or RPC response has been written yet. The client
    // must never see success on the wire before persistence succeeds.
    // Bytes written at persist call = the two pre-callback frames only.
    const size_t pre_success_bytes = frames[0].size() + frames[1].size();
    CHECK_EQ(r.bytes_written_at_persist, pre_success_bytes);
    // By the time complete fires, all four frames are on the wire.
    CHECK_EQ(r.bytes_written_at_complete, t.outgoing.size());

    // persist saw the exact credentials.
    CHECK(r.ssid == "mynet");
    CHECK(r.password == "s3cret");
}

TEST(failed_hard_reports_unable_to_connect_and_returns_to_authorized) {
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));
    t.outgoing.clear();

    t.push(makeWifiSettings("mynet", "wrong"));
    sm.loop();

    // Simulate immediate failure -- WL_CONNECT_FAILED style.
    w.failed_ = true;
    sm.loop();

    auto frames = splitFrames(t.outgoing);
    // Sequence: Error::None (per-spec pre-clear), Provisioning (from
    // handleWifiSettings), Error::UnableToConnect, Authorized.
    CHECK(frames.size() >= 4u);
    if (frames.size() < 4u) return;
    CHECK(frameIsError(frames[0], improv::Error::None));
    CHECK(frameIsCurrentState(frames[1], improv::State::Provisioning));
    CHECK(frameIsError(frames[2], improv::Error::UnableToConnect));
    CHECK(frameIsCurrentState(frames[3], improv::State::Authorized));

    // STA-only disconnect must have fired; the callback must NOT have run.
    CHECK_EQ(w.disconnect_calls, 1);
    CHECK(!r.persist_called);
    CHECK(!r.complete_called);

    // Sending another WIFI_SETTINGS after a failure must still work: the
    // reset-to-Authorized path leaves the machine ready to retry.
    w.failed_ = false;
    w.is_connected_ = true;
    t.outgoing.clear();
    t.push(makeWifiSettings("mynet2", "pw2"));
    sm.loop();  // handles WIFI_SETTINGS, sends Provisioning
    sm.loop();  // sees is_connected, sends response/state/callback
    CHECK(r.persist_called);
    CHECK(r.complete_called);
    CHECK(r.ssid == "mynet2");
}

TEST(timeout_reports_unable_to_connect) {
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));
    t.outgoing.clear();

    t.push(makeWifiSettings("mynet", "pw"));
    sm.loop();

    // Neither connected nor hard-failed for the whole 15 seconds.
    c.advance(improv::StateMachine::kConnectTimeoutUs + 1);
    sm.loop();

    auto frames = splitFrames(t.outgoing);
    // Error::None (pre-clear), Provisioning, Error::UnableToConnect, Authorized.
    CHECK(frames.size() >= 4u);
    if (frames.size() < 4u) return;
    CHECK(frameIsError(frames[0], improv::Error::None));
    CHECK(frameIsCurrentState(frames[1], improv::State::Provisioning));
    CHECK(frameIsError(frames[2], improv::Error::UnableToConnect));
    CHECK(frameIsCurrentState(frames[3], improv::State::Authorized));
    CHECK(!r.persist_called);
    CHECK(!r.complete_called);
    CHECK_EQ(w.disconnect_calls, 1);
}

TEST(get_current_state_while_connecting_reports_provisioning) {
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));
    t.outgoing.clear();

    t.push(makeWifiSettings("mynet", "pw"));
    sm.loop();
    // GetCurrentState while the association is still in flight.
    t.push(makeSimpleRpc(improv::Command::GetCurrentState));
    sm.loop();

    auto frames = splitFrames(t.outgoing);
    // Error::None + Provisioning from the WIFI_SETTINGS handling, then
    // Error::None + Provisioning again from the GetCurrentState reply
    // (per-spec pre-clear applies to every valid parsed command).
    CHECK(frames.size() >= 4u);
    if (frames.size() < 4u) return;
    CHECK(frameIsError(frames[0], improv::Error::None));
    CHECK(frameIsCurrentState(frames[1], improv::State::Provisioning));
    CHECK(frameIsError(frames[2], improv::Error::None));
    CHECK(frameIsCurrentState(frames[3], improv::State::Provisioning));
}

TEST(get_device_info_returns_configured_fields) {
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));
    t.outgoing.clear();

    t.push(makeSimpleRpc(improv::Command::GetDeviceInfo));
    sm.loop();

    auto frames = splitFrames(t.outgoing);
    // Error::None (pre-clear), then the actual RPC response.
    CHECK_EQ(frames.size(), 2u);
    if (frames.size() < 2) return;
    CHECK(frameIsError(frames[0], improv::Error::None));
    CHECK(frameIsRpcResponse(frames[1], improv::Command::GetDeviceInfo));

    // Concatenate all length-prefixed strings and match; simpler than
    // parsing them individually for this smoke check.
    const auto &f = frames[1];
    std::string joined(f.begin() + 12, f.end() - 0);  // strings live from [12..]
    CHECK(joined.find("HomeKey-ESP32") != std::string::npos);
    CHECK(joined.find("test-1.0") != std::string::npos);
    CHECK(joined.find("ESP32-S3") != std::string::npos);
    CHECK(joined.find("HomeKey-ABCDEF") != std::string::npos);
}

TEST(wifi_settings_with_empty_ssid_is_rejected_without_calling_wifi_begin) {
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));
    t.outgoing.clear();

    t.push(makeWifiSettings("", "pw"));
    sm.loop();

    CHECK_EQ(w.begin_calls, 0);
    auto frames = splitFrames(t.outgoing);
    // The command parses fine (empty ssid is a valid frame), so per-spec
    // pre-clear applies; then handleWifiSettings raises InvalidRpc.
    CHECK_EQ(frames.size(), 2u);
    if (frames.size() < 2) return;
    CHECK(frameIsError(frames[0], improv::Error::None));
    CHECK(frameIsError(frames[1], improv::Error::InvalidRpc));
}

TEST(disconnect_is_called_on_failure_but_not_on_success) {
    // Ensures we don't tear down WiFi on the happy path (a bug that would
    // kill the STA the flasher just brought up).
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));
    t.outgoing.clear();

    t.push(makeWifiSettings("mynet", "pw"));
    sm.loop();
    w.is_connected_ = true;
    sm.loop();

    CHECK_EQ(w.disconnect_calls, 0);
    CHECK(r.persist_called);
    CHECK(r.complete_called);
}

TEST(persistence_failure_prevents_success_signal) {
    // ESPHome reference contract: the client must never see success on the
    // wire for credentials that were not persisted. If the persist callback
    // returns false, no Provisioned state and no RPC response are emitted;
    // the client sees UnableToConnect and Authorized instead, and the
    // completion callback is not invoked.
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    r.persist_result = false;  // simulate HomeSpan NVS write failure
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));
    t.outgoing.clear();

    t.push(makeWifiSettings("mynet", "s3cret"));
    sm.loop();
    w.is_connected_ = true;
    sm.loop();

    // persist ran; complete did NOT.
    CHECK(r.persist_called);
    CHECK(!r.complete_called);

    // On the wire: pre-clear, Provisioning, UnableToConnect, Authorized.
    // Crucially, no Provisioned state and no RPC response for WIFI_SETTINGS.
    auto frames = splitFrames(t.outgoing);
    CHECK_EQ(frames.size(), 4u);
    if (frames.size() < 4u) return;
    CHECK(frameIsError(frames[0], improv::Error::None));
    CHECK(frameIsCurrentState(frames[1], improv::State::Provisioning));
    CHECK(frameIsError(frames[2], improv::Error::UnableToConnect));
    CHECK(frameIsCurrentState(frames[3], improv::State::Authorized));

    for (const auto &f : frames) {
        CHECK(!frameIsCurrentState(f, improv::State::Provisioned));
        CHECK(!frameIsRpcResponse(f, improv::Command::WifiSettings));
    }

    // Persistence failure must also cause STA teardown so we don't keep
    // holding a live association whose credentials we could not remember.
    CHECK_EQ(w.disconnect_calls, 1);
}

TEST(retry_after_failure_clears_error_first) {
    // After a failed WIFI_SETTINGS, the next WIFI_SETTINGS must preface
    // with Error::None so a client latched in the UnableToConnect error
    // (ESP Web Tools does this) recovers cleanly on retry.
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));
    t.outgoing.clear();

    // First attempt: fails hard.
    t.push(makeWifiSettings("wrong-net", "wrong"));
    sm.loop();
    w.failed_ = true;
    sm.loop();
    // Sanity: the failure error was emitted before the retry.
    {
        auto f = splitFrames(t.outgoing);
        bool saw_unable = false;
        for (const auto &fr : f) if (frameIsError(fr, improv::Error::UnableToConnect)) saw_unable = true;
        CHECK(saw_unable);
    }
    t.outgoing.clear();

    // Second attempt: the very first frame emitted must be Error::None,
    // BEFORE any state transition. Otherwise a client still holding the
    // previous UnableToConnect will keep showing failure.
    w.failed_ = false;
    t.push(makeWifiSettings("good-net", "pw"));
    sm.loop();

    auto frames = splitFrames(t.outgoing);
    CHECK(frames.size() >= 2u);
    if (frames.size() < 2u) return;
    CHECK(frameIsError(frames[0], improv::Error::None));
    CHECK(frameIsCurrentState(frames[1], improv::State::Provisioning));
}

TEST(malformed_frame_error_is_not_prefaced_with_error_none) {
    // Reviewer requirement: do NOT emit Error::None before a parser-level
    // failure. Only *valid parsed* commands get the pre-clear.
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));
    t.outgoing.clear();

    // Build a WIFI_SETTINGS frame and corrupt the checksum.
    auto bad = makeWifiSettings("net", "pw");
    bad.back() ^= 0xFF;
    t.push(bad);
    sm.loop();

    auto frames = splitFrames(t.outgoing);
    CHECK_EQ(frames.size(), 1u);
    if (frames.empty()) return;
    // Exactly one frame: the InvalidRpc from the parser. No preceding None.
    CHECK(frameIsError(frames[0], improv::Error::InvalidRpc));
}

TEST(unsupported_optional_commands_return_unknown_rpc) {
    // GetWifiNetworks / GetNetworkState / GetHostname / GetDeviceName are
    // recognized-but-unimplemented. Returning an empty RPC result would be
    // ambiguous (for a scan it means "successful, zero networks"). Return
    // UnknownRpc so the client can distinguish "not supported" from
    // "successful and empty". Each is prefaced with Error::None because the
    // frames themselves parsed fine.
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));

    for (auto cmd : {improv::Command::GetWifiNetworks,
                     improv::Command::GetNetworkState,
                     improv::Command::GetHostname,
                     improv::Command::GetDeviceName}) {
        t.outgoing.clear();
        t.push(makeSimpleRpc(cmd));
        sm.loop();
        auto frames = splitFrames(t.outgoing);
        if (frames.size() != 2u ||
            !frameIsError(frames[0], improv::Error::None) ||
            !frameIsError(frames[1], improv::Error::UnknownRpc)) {
            std::fprintf(stderr, "  FAIL: cmd 0x%02X yielded %zu frames\n",
                         (unsigned)cmd, frames.size());
            g_fail++;
            return;
        }
    }
}

TEST(read_budget_is_bounded_per_loop) {
    // Push more than 128 bytes of noise. loop() should process at most 128
    // bytes; the remainder stays in the queue for the next call.
    FakeTransport t; FakeWifi w; FakeClock c; ProvisionRecorder r;
    improv::StateMachine sm;
    sm.begin(makeCfg(t, w, c, r));
    t.outgoing.clear();

    std::vector<uint8_t> noise(200, 'x');
    t.push(noise);
    sm.loop();
    CHECK(t.incoming.size() >= 200u - 128u);
    CHECK(t.incoming.size() <= 200u - 128u + 1u);
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
