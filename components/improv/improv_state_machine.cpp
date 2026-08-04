// Improv Serial state machine. Host-buildable: no ESP-IDF header is
// required for compile or link. Log macros collapse to no-ops when the
// esp_log header is not on the include path.

#include "improv_state_machine.hpp"

#if __has_include(<esp_log.h>)
  #include <esp_log.h>
#else
  #define ESP_LOGI(...) ((void)0)
  #define ESP_LOGW(...) ((void)0)
  #define ESP_LOGE(...) ((void)0)
#endif

#include <cstring>

namespace improv {

// TAG is referenced only by ESP_LOG macros; those expand to no-ops on host,
// which leaves the constant unreferenced. Silence the host-side warning
// without conditional compilation.
namespace {
[[maybe_unused]] constexpr const char *TAG = "improv";
}

void StateMachine::begin(const SerialConfig &cfg) {
    cfg_ = cfg;
    parser_.reset();
    phase_ = Phase::Idle;
    active_ = true;
    ESP_LOGI(TAG, "Improv Serial active on this boot (device not yet provisioned)");
    // Announce readiness so a client that connects immediately sees state
    // without having to poll.
    sendState(State::Authorized);
}

void StateMachine::loop() {
    if (!active_) return;

    // Feed anything waiting through the parser. Bounded work per call so we
    // never starve other loop() consumers.
    if (cfg_.io.read_byte) {
        for (int budget = 128; budget > 0; budget--) {
            const int b = cfg_.io.read_byte();
            if (b < 0) break;
            FeedResult r = parser_.feed(static_cast<uint8_t>(b));
            if (r.frame_complete) handleCommand(r);
        }
    }

    if (phase_ == Phase::Connecting) {
        pollConnecting();
    }
}

State StateMachine::currentReportedState() const {
    return (phase_ == Phase::Connecting)   ? State::Provisioning
         : (phase_ == Phase::Provisioning) ? State::Provisioned
                                           : State::Authorized;
}

void StateMachine::writeAll(const std::vector<uint8_t> &pkt) {
    if (pkt.empty() || !cfg_.io.write_bytes) return;
    cfg_.io.write_bytes(pkt.data(), pkt.size());
}

void StateMachine::sendState(State s) { writeAll(buildStatePacket(s)); }
void StateMachine::sendError(Error e) { writeAll(buildErrorPacket(e)); }

void StateMachine::sendRpcResponse(Command command, const std::vector<std::string> &strings) {
    auto body = buildRpcResponse(command, strings);
    if (body.empty()) return;
    writeAll(buildFrame(PacketType::RpcResponse, body));
}

void StateMachine::handleWifiSettings(const std::string &ssid, const std::string &password) {
    // No credential values in any log line, ever. Sizes and status only.
    ESP_LOGI(TAG, "WIFI_SETTINGS received (ssid=%u bytes, pwd=%u bytes)",
             static_cast<unsigned>(ssid.size()), static_cast<unsigned>(password.size()));

    if (ssid.empty()) {
        sendError(Error::InvalidRpc);
        return;
    }
    if (!cfg_.wifi.begin || !cfg_.now_us) {
        sendError(Error::UnableToConnect);
        return;
    }

    pending_ssid_     = ssid;
    pending_password_ = password;
    phase_            = Phase::Connecting;
    deadline_us_      = cfg_.now_us() + kConnectTimeoutUs;
    sendState(State::Provisioning);

    // Non-blocking: driver returns immediately.
    cfg_.wifi.begin(pending_ssid_, pending_password_);
}

void StateMachine::pollConnecting() {
    const bool connected = cfg_.wifi.is_connected && cfg_.wifi.is_connected();
    const bool failed    = cfg_.wifi.failed_hard  && cfg_.wifi.failed_hard();

    if (connected) {
        const std::string url = cfg_.wifi.device_url ? cfg_.wifi.device_url() : "";
        ESP_LOGI(TAG, "STA associated, device URL is %s", url.c_str());

        // Move the credentials into locals, scrub the members, then call
        // persist. Ordering per ESPHome reference (improv_serial_component
        // .cpp:53-63): persist first -- the client must never see success on
        // the wire before persistence has actually succeeded.
        auto ssid = pending_ssid_;
        auto pwd  = pending_password_;
        scrub(pending_ssid_);
        scrub(pending_password_);

        const bool persisted = cfg_.persist ? cfg_.persist(ssid, pwd) : false;
        if (!persisted) {
            ESP_LOGE(TAG, "credential persistence failed; not signalling success");
            sendError(Error::UnableToConnect);
            if (cfg_.wifi.disconnect) cfg_.wifi.disconnect();
            phase_ = Phase::Idle;
            sendState(State::Authorized);
            return;
        }

        // Now safe to signal: Provisioned state, then RPC response with URL.
        // This is the reference ordering the browser client expects.
        phase_ = Phase::Provisioning;
        sendState(State::Provisioned);
        sendRpcResponse(Command::WifiSettings, {url});

        // Adapter's completion: flush the transport and reboot. Expected not
        // to return. If it does (test double or misbehaving), fall back to
        // Idle rather than wedge in Provisioning.
        if (cfg_.complete) cfg_.complete();
        phase_ = Phase::Idle;
        return;
    }

    const bool deadline = cfg_.now_us && cfg_.now_us() >= deadline_us_;
    if (deadline || failed) {
        ESP_LOGW(TAG, "STA association failed (deadline=%d, driver_failed=%d)",
                 deadline, failed);
        sendError(Error::UnableToConnect);
        if (cfg_.wifi.disconnect) cfg_.wifi.disconnect();
        scrub(pending_ssid_);
        scrub(pending_password_);
        phase_ = Phase::Idle;
        sendState(State::Authorized);
    }
}

void StateMachine::handleCommand(const FeedResult &r) {
    // Parser-level failures (bad checksum, bad version, unknown command byte,
    // length mismatches): surface the specific error without preceding it
    // with an Error::None clear. This mirrors the spec's "first send
    // ErrorState 0, then the RPC result or a new error" rule -- which
    // applies to *valid, parsed RPC commands*, not to malformed frames.
    if (r.error != Error::None) {
        sendError(r.error);
        return;
    }

    // Valid parsed command: clear any latched error on the client (a
    // previous UNABLE_TO_CONNECT would otherwise keep ESP Web Tools stuck
    // in the failure state even after a successful retry), then handle it.
    sendError(Error::None);
    // One log line per valid received RPC: this is the ground truth
    // signal that the browser has reached us on the correct transport.
    // No payload logged (WIFI_SETTINGS carries credentials).
    ESP_LOGI(TAG, "RPC received: command=0x%02X", static_cast<unsigned>(r.command));

    switch (r.command) {
        case Command::WifiSettings:
            handleWifiSettings(r.ssid, r.password);
            return;
        case Command::GetCurrentState:
            sendState(currentReportedState());
            return;
        case Command::GetDeviceInfo:
            sendRpcResponse(Command::GetDeviceInfo,
                {cfg_.firmware_name, cfg_.firmware_version,
                 cfg_.chip_family, cfg_.device_name});
            return;
        // The following four are legitimate Improv commands that this device
        // does not implement. Returning an empty RPC result would be
        // ambiguous -- for GetWifiNetworks in particular, "no networks
        // returned" is a valid successful outcome meaning the scan completed
        // and found nothing. Report UnknownRpc instead so the client can
        // distinguish "not supported" from "successful and empty".
        case Command::GetWifiNetworks:
        case Command::GetNetworkState:
        case Command::GetHostname:
        case Command::GetDeviceName:
        default:
            sendError(Error::UnknownRpc);
            return;
    }
}

void StateMachine::scrub(std::string &s) {
    volatile char *p = const_cast<volatile char *>(s.data());
    for (size_t i = 0; i < s.size(); i++) p[i] = 0;
    s.clear();
    s.shrink_to_fit();
}

}  // namespace improv
