/// @file cn105.h
/// @brief Mitsubishi Electric CN105 indoor-unit serial protocol driver.
///
/// CN105 is the 5-pin connector on Mitsubishi Electric indoor units that their
/// official Wi-Fi interface (MAC-558IF-E / kumo cloud) plugs into. The wire
/// protocol is 2400 baud, 8-E-1, with 0xFC-framed packets. This driver is an
/// ESP-IDF/C++ port of the well-known SwiCago/HeatPump Arduino library used by
/// gysmo38/mitsubishi2MQTT, exposing the same conceptual API.
///
/// PORTING STATUS: interface complete; packet engine is a STUB (see cn105.cpp).
///
/// Reference: https://github.com/SwiCago/HeatPump

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include "driver/uart.h"
#include "esp_err.h"

namespace cn105 {

/// Desired/echoed unit settings. String fields use the same vocabularies as
/// the SwiCago library so the MQTT contract from mitsubishi2MQTT carries over
/// 1:1.
struct Settings {
    std::string power;       ///< "ON" | "OFF"
    std::string mode;        ///< "HEAT" | "DRY" | "COOL" | "FAN" | "AUTO"
    float       temperature; ///< setpoint, °C (0.5 steps; some units 1.0)
    std::string fan;         ///< "AUTO" | "QUIET" | "1".."4"
    std::string vane;        ///< "AUTO" | "1".."5" | "SWING"
    std::string wideVane;    ///< "<<" | "<" | "|" | ">" | ">>" | "<>" | "SWING"
    bool        connected;   ///< true once a valid packet has been exchanged
};

/// Read-only status reported by the unit.
struct Status {
    float roomTemperature;     ///< measured room temp, °C
    bool  operating;           ///< compressor actively heating/cooling
    int   compressorFrequency; ///< Hz (0 when idle), if the unit reports it
};

using SettingsCallback = std::function<void(const Settings&)>;
using StatusCallback   = std::function<void(const Status&)>;
using PacketCallback   = std::function<void(const uint8_t* data, size_t len,
                                            const char* direction)>;

/// CN105 protocol driver. One instance per indoor unit (one UART).
class HeatPump {
public:
    HeatPump() = default;

    /// Bind to a UART and send the connect handshake. Installs the UART driver
    /// at 2400 baud / 8-E-1 on the given pins.
    esp_err_t connect(uart_port_t port, int tx_gpio, int rx_gpio);

    /// Pump the protocol: drain RX, parse frames, fire callbacks, and (when
    /// auto-update is on) flush any pending setting writes. Call frequently
    /// from the CN105 task loop.
    void update();

    /// Request a fresh settings+status sync from the unit.
    void sync();

    // --- Control (mirrors SwiCago/HeatPump setters) ---
    void setPowerSetting(const std::string& power);
    void setModeSetting(const std::string& mode);
    void setTemperature(float celsius);
    void setFanSpeed(const std::string& fan);
    void setVaneSetting(const std::string& vane);
    void setWideVaneSetting(const std::string& wideVane);

    /// Push an external room-temperature reading to the unit so it regulates on
    /// a remote sensor instead of its own (the "remote_temp/set" feature).
    void setRemoteTemperature(float celsius);

    // --- Accessors ---
    Settings getSettings() const { return settings_; }
    Status   getStatus() const { return status_; }
    bool     isConnected() const { return settings_.connected; }

    // --- Callbacks ---
    void setSettingsChangedCallback(SettingsCallback cb) { on_settings_ = std::move(cb); }
    void setStatusChangedCallback(StatusCallback cb) { on_status_ = std::move(cb); }
    void setPacketCallback(PacketCallback cb) { on_packet_ = std::move(cb); }

    /// Apply external (e.g. handheld remote) changes locally when they arrive.
    void enableExternalUpdate() { external_update_ = true; }
    /// Automatically flush queued writes from update() rather than requiring an
    /// explicit flush call.
    void enableAutoUpdate() { auto_update_ = true; }

private:
    uart_port_t port_{UART_NUM_1};
    Settings settings_{};
    Status status_{};
    SettingsCallback on_settings_{};
    StatusCallback on_status_{};
    PacketCallback on_packet_{};
    bool external_update_{false};
    bool auto_update_{false};
};

}  // namespace cn105
