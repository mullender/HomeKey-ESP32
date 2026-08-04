// Improv Serial parser and framer. Host-buildable; see the header for scope.
//
// The parser mirrors the position-indexed state machine in
// improv-wifi/sdk-cpp/src/improv.cpp so this device interoperates with the
// same clients. Rewritten rather than vendored so the interface fits our
// tests (per-instance state, feed-a-byte-return-a-result) and drops the
// unused BLE UUIDs / Arduino-only overloads.

#include "improv_protocol.hpp"

#include <cstring>

namespace improv {

namespace {

// 8-bit sum of every byte, spec-defined.
uint8_t computeChecksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum & 0xFF);
}

// Fixed preamble at offsets 0..5 of every frame.
constexpr char kPreamble[6] = {'I', 'M', 'P', 'R', 'O', 'V'};

}  // namespace

void Parser::reset() {
    position_ = 0;
}

FeedResult Parser::feed(uint8_t byte) {
    FeedResult result;

    // Preamble bytes 0..5 -- any mismatch resets to 0 (or to 1 if the byte
    // itself is an 'I', so a stream like "IIMPROV..." doesn't discard the
    // second 'I' as noise). This is the resync path that lets the parser
    // survive interleaved log output on the same UART.
    if (position_ < 6) {
        if (byte == static_cast<uint8_t>(kPreamble[position_])) {
            buffer_[position_++] = byte;
        } else {
            position_ = 0;
            // A stray 'I' could be the start of the next real preamble.
            if (byte == 'I') {
                buffer_[0] = byte;
                position_ = 1;
            }
        }
        return result;
    }

    // Version at offset 6. A mismatch is a protocol violation, not framing
    // noise: report and resync.
    if (position_ == 6) {
        if (byte == kSerialVersion) {
            buffer_[position_++] = byte;
        } else {
            reset();
            result.frame_complete = true;
            result.error = Error::InvalidRpc;
        }
        return result;
    }

    // Type and length at offsets 7..8: always accepted; validated below.
    if (position_ == 7 || position_ == 8) {
        buffer_[position_++] = byte;
        return result;
    }

    const uint8_t data_len = buffer_[8];

    // Data bytes at offsets 9..(9 + data_len - 1).
    if (position_ < static_cast<size_t>(9) + data_len) {
        buffer_[position_++] = byte;
        return result;
    }

    // Final byte is the checksum.
    const uint8_t expected = computeChecksum(buffer_, position_);
    buffer_[position_++] = byte;
    if (byte != expected) {
        reset();
        result.frame_complete = true;
        result.error = Error::InvalidRpc;
        return result;
    }

    // Whole frame is intact. We only handle inbound RPC packets; anything
    // else is silently ignored and we resync (the client shouldn't be
    // sending anything else, so this is a spec-conformance check, not an
    // error to surface).
    const auto type = static_cast<PacketType>(buffer_[7]);
    if (type != PacketType::Rpc) {
        reset();
        return result;
    }

    result = parseRpc(data_len);
    reset();
    return result;
}

FeedResult Parser::parseRpc(uint8_t data_len) const {
    FeedResult result;
    result.frame_complete = true;

    // RPC data must contain at least command + inner length.
    if (data_len < 2) {
        result.error = Error::InvalidRpc;
        return result;
    }

    const uint8_t *rpc = buffer_ + 9;
    const uint8_t cmd_id     = rpc[0];
    const uint8_t inner_len  = rpc[1];

    // Inner length must exactly match what's left in the frame.
    if (static_cast<size_t>(inner_len) + 2 != data_len) {
        result.error = Error::InvalidRpc;
        return result;
    }

    switch (cmd_id) {
        case static_cast<uint8_t>(Command::WifiSettings): {
            // Layout: ssid_len(1) ssid(ssid_len) pwd_len(1) pwd(pwd_len).
            if (inner_len < 2) {
                result.error = Error::InvalidRpc;
                return result;
            }
            const uint8_t ssid_len = rpc[2];
            if (static_cast<size_t>(3) + ssid_len + 1 > static_cast<size_t>(2) + inner_len) {
                result.error = Error::InvalidRpc;
                return result;
            }
            const uint8_t pwd_len = rpc[3 + ssid_len];
            if (static_cast<size_t>(3) + ssid_len + 1 + pwd_len != static_cast<size_t>(2) + inner_len) {
                result.error = Error::InvalidRpc;
                return result;
            }
            result.command  = Command::WifiSettings;
            result.ssid.assign(reinterpret_cast<const char *>(rpc + 3), ssid_len);
            result.password.assign(reinterpret_cast<const char *>(rpc + 3 + ssid_len + 1), pwd_len);
            return result;
        }

        // Every command listed in the Improv Serial spec's Command IDs table
        // is a "valid parsed command" as far as the parser is concerned; the
        // application layer decides whether it's actually supported. This
        // matters because handleCommand distinguishes parser-level failures
        // (bad frame -> raw error, no pre-clear) from valid-but-unsupported
        // commands (spec-compliant error path: Error::None then UnknownRpc).
        case static_cast<uint8_t>(Command::GetCurrentState):
        case static_cast<uint8_t>(Command::GetDeviceInfo):
        case static_cast<uint8_t>(Command::GetWifiNetworks):
        case static_cast<uint8_t>(Command::GetHostname):
        case static_cast<uint8_t>(Command::GetDeviceName):
        case static_cast<uint8_t>(Command::GetNetworkState): {
            result.command = static_cast<Command>(cmd_id);
            return result;
        }

        default:
            result.error = Error::UnknownRpc;
            return result;
    }
}

std::vector<uint8_t> buildFrame(PacketType type, const std::vector<uint8_t> &data) {
    // The frame length byte is a single uint8_t, so >255 bytes of data cannot
    // be represented. Refuse rather than silently truncate: emitting a frame
    // whose length header disagrees with its payload would put the client
    // permanently out of sync until the next resync on IMPROV, and would
    // silently drop the tail of any oversized response.
    if (data.size() > 255) {
        return {};
    }
    // 6 preamble + 1 version + 1 type + 1 length + data + 1 checksum.
    std::vector<uint8_t> out;
    out.reserve(10 + data.size());
    out.insert(out.end(), kPreamble, kPreamble + sizeof(kPreamble));
    out.push_back(kSerialVersion);
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(static_cast<uint8_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    out.push_back(computeChecksum(out.data(), out.size()));
    return out;
}

std::vector<uint8_t> buildRpcResponse(Command command, const std::vector<std::string> &strings) {
    // Same reason as buildFrame: every length prefix is one byte.
    // Individually cap each string; the aggregate cap is checked at the end
    // and refuses the whole response so a partial output is never emitted.
    for (const auto &s : strings) {
        if (s.size() > 255) return {};
    }
    // [0] command, [1] inner length, [2..] Pascal-length strings.
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(command));
    out.push_back(0);  // placeholder; patched below
    for (const auto &s : strings) {
        out.push_back(static_cast<uint8_t>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
    const size_t inner = out.size() - 2;
    if (inner > 255) {
        return {};
    }
    out[1] = static_cast<uint8_t>(inner);
    return out;
}

std::vector<uint8_t> buildStatePacket(State state) {
    return buildFrame(PacketType::CurrentState, {static_cast<uint8_t>(state)});
}

std::vector<uint8_t> buildErrorPacket(Error error) {
    return buildFrame(PacketType::ErrorState, {static_cast<uint8_t>(error)});
}

}  // namespace improv
