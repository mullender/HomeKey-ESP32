#pragma once

// Improv Serial state machine, extracted from the ESP-side wrapper so it can
// be exercised entirely off-target: byte I/O, Wi-Fi and the clock are all
// caller-supplied callbacks, and no ESP-IDF header is required to compile
// or link this class.
//
// improv_serial.{hpp,cpp} is a thin singleton facade that wraps this class
// for main.cpp's use.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "improv_protocol.hpp"

namespace improv {

/**
 * Byte-level transport, provided by the caller. read_byte must be
 * non-blocking (returning -1 when nothing is available); write_bytes writes
 * a whole packet and may block briefly to flush.
 */
struct SerialIo {
    std::function<int()> read_byte;
    std::function<void(const uint8_t *data, size_t len)> write_bytes;
};

/**
 * Wi-Fi driver, provided by the caller (an Arduino WiFi wrapper on target,
 * a scripted fake in tests). All operations are non-blocking; the state
 * machine polls is_connected under a deadline it sets internally.
 */
struct WifiDriver {
    std::function<void(const std::string &ssid, const std::string &password)> begin;
    std::function<bool()> is_connected;
    std::function<bool()> failed_hard;   // WL_CONNECT_FAILED / WL_NO_SSID_AVAIL
    std::function<std::string()> device_url;   // "http://x.y.z.w/"
    // STA-only disconnect. Must NOT tear the shared radio down: HomeSpan's
    // captive-portal AP has to survive a failed Improv attempt.
    std::function<void()> disconnect;
};

/**
 * Full session configuration. now_us is an injectable clock in microseconds;
 * on target it's esp_timer_get_time, in tests a lambda over scripted values.
 *
 * The persist/complete split follows the ESPHome reference
 * (improv_serial_component.cpp:53-63): persistence must succeed BEFORE the
 * client sees any success signal on the wire, so ESP Web Tools never
 * observes an "OK" that the device then fails to remember. persist returns
 * true on success and false on failure; a false return aborts the
 * transaction as UnableToConnect. complete runs after Provisioned + the RPC
 * response have been queued to the transport; the adapter flushes the
 * transport and signals its main setup task to resume deferred hardware
 * init. complete returns normally, and the state machine remains in
 * Phase::Provisioned so subsequent GET_CURRENT_STATE queries keep
 * reporting Provisioned.
 */
struct SerialConfig {
    std::string firmware_name;
    std::string firmware_version;
    std::string chip_family;
    std::string device_name;
    SerialIo io;
    WifiDriver wifi;
    std::function<bool(const std::string &ssid, const std::string &password)> persist;
    std::function<void()> complete;
    std::function<int64_t()> now_us;
};

/**
 * Testable state machine. Holds its own parser and phase so tests can
 * construct a fresh instance per case without touching globals.
 *
 * Timeout for STA association is fixed: 15 seconds. Chosen to cover a normal
 * WPA2 join plus DHCP; anything longer is treated as UNABLE_TO_CONNECT.
 */
class StateMachine {
public:
    static constexpr int64_t kConnectTimeoutUs = 15 * 1000 * 1000;

    /** Called by the facade after cfg is filled in. */
    void begin(const SerialConfig &cfg);

    /** Pump the state machine. Non-blocking. Safe to call when not begun. */
    void loop();

    /** For diagnostics/tests. */
    State currentReportedState() const;

private:
    enum class Phase {
        Idle,          // waiting for a WIFI_SETTINGS frame
        Connecting,    // wifi.begin() called, polling status
        Provisioned,   // persist succeeded and complete callback fired;
                       // stable post-success state, GET_CURRENT_STATE
                       // continues to answer Provisioned
    };

    SerialConfig cfg_{};
    Parser       parser_{};
    Phase        phase_ = Phase::Idle;
    int64_t      deadline_us_ = 0;
    std::string  pending_ssid_;
    std::string  pending_password_;
    bool         active_ = false;

    void writeAll(const std::vector<uint8_t> &pkt);
    void sendState(State s);
    void sendError(Error e);
    void sendRpcResponse(Command command, const std::vector<std::string> &strings);
    void handleWifiSettings(const std::string &ssid, const std::string &password);
    void pollConnecting();
    void handleCommand(const FeedResult &r);

    // Zero the buffer under a volatile write, then release.
    static void scrub(std::string &s);
};

}  // namespace improv
