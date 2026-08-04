#pragma once

// Improv Serial protocol parser and framer.
//
// Pure C++. No ESP-IDF, no Arduino, no NVS -- so this file compiles and runs
// on a host with plain GCC/Clang, which is how the tests exercise it.
//
// Wire format is per https://www.improv-wifi.com/serial/ and matches the
// reference implementation at https://github.com/improv-wifi/sdk-cpp
// (Apache-2.0). Enum values are copied from that reference so this can
// interoperate with the same clients (ESP Web Tools, esphome-web, the Improv
// browser app).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace improv {

inline constexpr uint8_t kSerialVersion = 1;

enum class State : uint8_t {
    Stopped              = 0x00,
    AwaitingAuthorization = 0x01,
    Authorized           = 0x02,
    Provisioning         = 0x03,
    Provisioned          = 0x04,
};

enum class Error : uint8_t {
    None             = 0x00,
    InvalidRpc       = 0x01,
    UnknownRpc       = 0x02,
    UnableToConnect  = 0x03,
    NotAuthorized    = 0x04,
    BadHostname      = 0x05,
    Unknown          = 0xFF,
};

enum class PacketType : uint8_t {
    CurrentState = 0x01,
    ErrorState   = 0x02,
    Rpc          = 0x03,
    RpcResponse  = 0x04,
};

enum class Command : uint8_t {
    Unknown          = 0x00,
    WifiSettings     = 0x01,
    GetCurrentState  = 0x02,   // same numeric id as IDENTIFY in the SDK
    GetDeviceInfo    = 0x03,
    GetWifiNetworks  = 0x04,
    GetHostname      = 0x05,
    GetDeviceName    = 0x06,
    GetNetworkState  = 0x07,
};

/**
 * Result of feeding one byte to the parser.
 *
 * The three fields are independent: a single byte can complete a frame that is
 * either a valid command (Command set, no error) or a protocol error (Error
 * set, Command left Unknown). Non-terminal bytes leave everything at defaults.
 */
struct FeedResult {
    bool     frame_complete = false;
    Command  command        = Command::Unknown;
    Error    error          = Error::None;
    std::string ssid;
    std::string password;
};

/**
 * Byte-at-a-time parser for Improv Serial frames on a shared TX/RX line.
 *
 * Feed every received byte; frames arrive interleaved with arbitrary log text
 * without corrupting the parser -- the six-byte IMPROV preamble acts as a
 * resync marker and any position-mismatch resets the state to zero. This is
 * the same resync scheme the reference SDK uses.
 *
 * State is per-instance, not global, so a test can reset by constructing a
 * fresh Parser.
 */
class Parser {
public:
    Parser() = default;

    /** Feed a single byte. Returns { frame_complete, command|error, [ssid, password] }. */
    FeedResult feed(uint8_t byte);

    /** Discard any partial frame in progress. Called by the caller between sessions. */
    void reset();

    /** Bytes buffered so far in the current frame -- for tests. */
    size_t position() const { return position_; }

private:
    // The preamble + version + type + length header sits at offsets 0..8. The
    // maximum RPC data length is one byte (0..255), so total frame size is
    // capped at 9 + 255 + 1 = 265 bytes. Fixed-size buffer avoids any dynamic
    // allocation in the RX path.
    static constexpr size_t kMaxFrame = 9 + 255 + 1;

    uint8_t buffer_[kMaxFrame] = {};
    size_t  position_ = 0;

    // Parse the RPC data at buffer_[9..9+data_len). Returns a FeedResult with
    // command/error/payload populated; frame_complete is set by the caller.
    FeedResult parseRpc(uint8_t data_len) const;
};

/**
 * Frame an outbound packet: prepend the IMPROV header and version, tag with
 * type + length, append the 8-bit sum checksum. The caller passes the raw
 * data payload; this returns a byte buffer ready for Serial.write().
 */
std::vector<uint8_t> buildFrame(PacketType type, const std::vector<uint8_t> &data);

/**
 * Build an RPC response body (the payload of a TYPE_RPC_RESPONSE frame).
 *
 * Format per spec:
 *   [0] command being responded to
 *   [1] inner data length
 *   [2..] list of Pascal-length strings (one length byte + that many bytes)
 *
 * The first string in the list, per the Wi-Fi Settings spec, is the device
 * URL the client should redirect the user to. Pass an empty string to send
 * "no URL" without dropping the entry.
 */
std::vector<uint8_t> buildRpcResponse(Command command, const std::vector<std::string> &strings);

/** Compact helper: build a full framed CurrentState packet. */
std::vector<uint8_t> buildStatePacket(State state);

/** Compact helper: build a full framed ErrorState packet. */
std::vector<uint8_t> buildErrorPacket(Error error);

}  // namespace improv
