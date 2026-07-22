/// @file cn105_codec.h
/// @brief Pure, ESP-IDF-free CN105 wire codec: framing + typed decoders + packet
///        encoders. This is the narrow wire boundary of the CN105 protocol,
///        deliberately carved out of cn105.cpp so it can be exhaustively unit
///        tested on the host (see test/host/test_cn105_codec.cpp).
///
/// What lives here (pure input -> output, no I/O, no shared state):
///   * checksum()            — the frame checksum.
///   * FrameParser           — incremental bytes -> validated Frame.
///   * decode*()             — a validated info payload -> a typed value struct
///                             (these do NOT mutate any HeatPump state).
///   * build*()              — explicit inputs -> a complete 22-byte packet.
///
/// What deliberately stays in HeatPump (cn105.cpp): timers, dirty flags,
/// mutexes, callbacks, adoption/retry policy, the infoMode cycling counter and
/// the tempMode_/wideVaneAdj_ sticky flags. The codec is stateless.
///
/// Wire protocol: [0xFC][type][0x01][0x30][len][payload...][checksum]
///   checksum = (0xFC - sum(all preceding bytes)) & 0xFF

#pragma once

#include <cstdint>

#include "cn105_types.h"

namespace cn105 {
namespace codec {

/// Full length of a SET / INFO / remote-temp packet on the wire.
constexpr int kPacketLen  = 22;
/// Maximum payload length accepted by a frame (header[4] sanity bound).
constexpr int kMaxPayload = 32;

/// Frame checksum over `len` bytes: (0xFC - sum(bytes)) & 0xFF.
uint8_t checksum(const uint8_t* bytes, int len);

// ---------------------------------------------------------------------------
// Incremental frame parser
// ---------------------------------------------------------------------------
/// Feed bytes one at a time; the parser scans for the 0xFC start byte,
/// validates the fixed header (0x01,0x30) and length, collects the payload and
/// verifies the checksum. It mirrors the byte-consumption of the original
/// readPacket_ loop exactly: the whole 5-byte header is consumed before it is
/// validated, so a malformed header is rejected only after all its bytes are
/// taken (this keeps stream re-sync identical to the driver's original).
///
/// The parser holds no I/O and no timeouts — framing timeouts are an I/O-layer
/// concern owned by HeatPump::readPacket_.
class FrameParser {
public:
    /// A validated frame. `data` is always fully zero-filled; only `data[0..len-1]`
    /// are meaningful. Decoders may safely read fixed offsets beyond `len`
    /// (they read zeros) — this preserves the original zero-initialised buffer.
    struct Frame {
        uint8_t type{0};
        uint8_t data[kMaxPayload]{};
        int     len{0};
        uint8_t checksum{0};
    };

    enum class Push {
        More,      ///< byte consumed, frame not complete yet
        Complete,  ///< `out` now holds a fully validated frame
        Error,     ///< header/length/checksum invalid; parser has reset
    };

    /// Feed one byte. On Complete, `out` is populated. On Error the parser has
    /// already reset itself to scan for the next start byte.
    Push push(uint8_t byte, Frame& out);

    /// Reset to scanning for a start byte.
    void reset();

    /// True once a 0xFC start byte has been seen and we are mid-frame. Used by
    /// the driver to pick the correct per-byte read timeout.
    bool inFrame() const { return st_ != St::WaitStart; }

private:
    enum class St { WaitStart, Header, Data, Checksum };
    St      st_{St::WaitStart};
    uint8_t hdr_[5]{};
    int     hdrCount_{0};
    uint8_t data_[kMaxPayload]{};
    int     len_{0};
    int     dataCount_{0};
    uint8_t type_{0};
};

// ---------------------------------------------------------------------------
// Typed decoders (pure: payload -> value struct, no HeatPump state touched)
// ---------------------------------------------------------------------------
// Each takes the frame payload (`data`, guaranteed zero-filled to kMaxPayload)
// and its declared length. String fields point into static tables and are
// valid for the process lifetime.

/// 0x02 settings reply.
struct SettingsPayload {
    const char* power;
    const char* mode;
    float       temperature;
    bool        tempMode;          ///< true iff half-degree (0.5C) encoding seen
    const char* fan;
    const char* vane;
    const char* wideVane;
    bool        wideVaneAdj;       ///< (data[10] & 0xF0) == 0x80
    bool        hasTargetHumidity; ///< len>=13 && data[12] in 1..100
    int         targetHumidity;
};

/// 0x03 room-temperature reply (+ extended outside temp / runtime).
struct RoomTempPayload {
    float roomTemperature;
    bool  hasOutsideTemp;
    float outsideTemp;
    bool  hasRuntimeHours;
    float runtimeHours;
};

/// 0x06 operating-status reply (+ extended input power / energy).
struct OperatingPayload {
    bool  operating;
    int   compressorFrequency;
    bool  hasInputPower;
    int   inputPowerW;
    bool  hasEnergy;
    float energyKwh;
};

/// 0x09 power/standby reply (sub-mode + fan stage). has* is false when the raw
/// byte did not match a known table entry (matching the original behaviour of
/// only updating on a recognised value).
struct StandbyPayload {
    bool        hasSubMode;
    const char* subMode;
    bool        hasStage;
    const char* stage;
};

/// 0x04 error reply. `text` is "No Error" or "Error 0xXX sub 0xXX".
struct ErrorPayload {
    char text[32];
};

// --- Input normalisers ------------------------------------------------------
// Map an arbitrary user string to the canonical vocabulary member
// (case-insensitive), or the default (first) member if unrecognised. Used by the
// driver's setters so the value vocabularies have a single source of truth.
const char* normPower(const std::string& v);
const char* normMode(const std::string& v);
const char* normFan(const std::string& v);
const char* normVane(const std::string& v);
const char* normWideVane(const std::string& v);
/// If `celsius` rounds to a representable 1-degree setpoint, returns it
/// unchanged; otherwise returns the default setpoint (TEMP_MAP[0]).
float snapIntSetpoint(float celsius);

SettingsPayload  decodeSettings(const uint8_t* data, int len);
RoomTempPayload  decodeRoomTemp(const uint8_t* data, int len);
OperatingPayload decodeOperating(const uint8_t* data, int len);
StandbyPayload   decodeStandby(const uint8_t* data, int len);
ErrorPayload     decodeError(const uint8_t* data, int len);

// ---------------------------------------------------------------------------
// Packet encoders (explicit inputs -> complete 22-byte packet incl. checksum)
// ---------------------------------------------------------------------------

/// Build a SET packet that carries only the fields where `wanted` differs from
/// `current`, accumulating the control-mask bytes exactly as the unit expects.
/// `tempMode` selects half-degree temp encoding; `wideVaneAdj` OR-s 0x80 into
/// the wide-vane byte. `out` must point at kPacketLen bytes.
void buildSetPacket(uint8_t* out, const Settings& wanted, const Settings& current,
                    bool tempMode, bool wideVaneAdj);

/// Build a remote-temperature ("remote_temp/set") packet. `celsius <= 0` clears
/// the remote sensor (reverts the unit to its internal sensor).
void buildRemoteTempPacket(uint8_t* out, float celsius);

/// Build an INFO request packet for the given info-mode byte (e.g. 0x02).
void buildInfoPacket(uint8_t* out, uint8_t infoModeByte);

}  // namespace codec
}  // namespace cn105
