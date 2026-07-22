/// @file cn105.h
/// @brief Mitsubishi Electric CN105 indoor-unit serial protocol driver.
///
/// CN105 is the 5-pin connector on Mitsubishi Electric indoor units that their
/// official Wi-Fi interface (MAC-558IF-E / kumo cloud) plugs into. The wire
/// protocol is 2400 baud, 8-E-1, with 0xFC-framed packets. This driver is an
/// ESP-IDF/C++ port of the well-known SwiCago/HeatPump Arduino library used by
/// gysmo38/mitsubishi2MQTT, exposing the same conceptual API.
///
/// PORTING STATUS: full packet engine implemented (see cn105.cpp). Not yet
/// validated against physical hardware.
///
/// Reference: https://github.com/SwiCago/HeatPump

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>

#include "cn105_types.h"  // Settings / Status (ESP-IDF-free, shared with the codec)

namespace cn105 {

using SettingsCallback = std::function<void(const Settings&)>;
using StatusCallback   = std::function<void(const Status&)>;
using PacketCallback   = std::function<void(const uint8_t* data, size_t len,
                                            const char* direction)>;

/// CN105 protocol driver. One instance per indoor unit (one UART).
class HeatPump {
public:
    HeatPump();  // creates internal mutexes

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

    // --- Wide-vane capability probe (used by capability detection) ---
    // Suspend normal wide-vane enforcement and send `target` once, so a caller
    // can observe whether the unit holds it (powered) or reverts to its
    // physical detent (manual louver). While a probe is active the pump neither
    // re-asserts nor optimistically echoes the wide vane, so getSettings()
    // reflects only the unit's real reported position.
    void beginWideVaneProbe(const std::string& target);
    // End the probe, resuming enforcement and driving the vane back to `restore`.
    void endWideVaneProbeRestore(const std::string& restore);
    // End the probe without altering wanted state — a concurrent user change
    // made during the probe wins.
    void abortWideVaneProbe();

    /// Push an external room-temperature reading to the unit so it regulates on
    /// a remote sensor instead of its own (the "remote_temp/set" feature).
    void setRemoteTemperature(float celsius);

    // --- Accessors (thread-safe: return a snapshot published by the pump) ---
    Settings getSettings() const;
    Status   getStatus() const;
    bool     isConnected() const;

    // --- Callbacks (set once at init, before the pump task starts) ---
    void setSettingsChangedCallback(SettingsCallback cb) { on_settings_ = std::move(cb); }
    void setStatusChangedCallback(StatusCallback cb) { on_status_ = std::move(cb); }
    void setPacketCallback(PacketCallback cb) { on_packet_ = std::move(cb); }

    /// Apply external (e.g. handheld remote) changes locally when they arrive.
    void enableExternalUpdate() { externalUpdate_ = true; autoUpdate_ = true; }
    /// Automatically flush queued writes from update().
    void enableAutoUpdate() { autoUpdate_ = true; }

private:
    // ---- protocol helpers (implemented in cn105.cpp) ----
    void pump_();                              ///< one iteration of the state machine
    void sendConnect_();                       ///< send 0x5A handshake + read reply
    void flushWanted_(const Settings& wanted); ///< send SET packet, await 0x61 ACK
    void sendWideVaneOnce_(const std::string& target); ///< one-shot wide-vane SET (probe)
    void sendInfoPacket_(int packetType);      ///< send 0x42 info request + read reply
    void sendRemoteTemp_(float celsius);       ///< send 0x07 remote-temperature packet
    void drainPackets_(int maxPackets);        ///< read up to N buffered packets
    int  readPacket_(uint32_t start_to_ms);    ///< parse one frame -> RCVD_PKT_*
    int  decodeInfo_(const uint8_t* data, int len); ///< dispatch a 0x62 info reply
    void writePacket_(const uint8_t* packet, int len);
    int  readByte_(uint32_t to_ms);
    int  available_();
    bool canSend_(bool isInfo) const;
    void createSetPacket_(uint8_t* packet, const Settings& wanted);
    void createInfoPacket_(uint8_t* packet, int packetType);
    void publishSnapshot_();
    void fireCallbacks_();
    static uint8_t checkSum_(const uint8_t* bytes, int len);

    uart_port_t port_{UART_NUM_1};
    int tx_gpio_{-1};
    int rx_gpio_{-1};

    // Owned exclusively by the pump task (update()); no lock required.
    Settings current_{};
    Status   status_{};
    bool  connected_{false};
    bool  firstRun_{true};
    bool  tempMode_{false};
    bool  wideVaneAdj_{false};
    bool  autoUpdate_{false};
    bool  externalUpdate_{false};
    int   infoMode_{0};
    uint32_t lastSend_{0};
    uint32_t lastRecv_{0};
    uint32_t connectBackoff_{1000};    ///< current reconnect interval (ms), backs off on failure
    uint32_t lastConnectAttempt_{0};   ///< when we last sent a CONNECT handshake
    bool  settingsChanged_{false};  ///< set during a pump iteration, consumed by fireCallbacks_
    bool  statusChanged_{false};

    // Wanted settings + per-field dirty flags (guarded by want_mtx_).
    Settings wanted_{};
    bool  d_power_{false};
    bool  d_mode_{false};
    bool  d_temp_{false};
    bool  d_fan_{false};
    bool  d_vane_{false};
    bool  d_wideVane_{false};
    bool  wvProbe_{false};         ///< wide-vane enforcement suspended for a probe
    bool  wvProbeSend_{false};     ///< a one-shot probe SET is queued
    std::string wvProbeTarget_{};  ///< value for the one-shot probe SET
    uint32_t lastWanted_{0};
    bool  pendingRemoteTempSet_{false};
    float pendingRemoteTemp_{0.0f};
    mutable SemaphoreHandle_t want_mtx_{nullptr};

    // Published snapshot for cross-task accessors (guarded by pub_mtx_).
    Settings pub_settings_{};
    Status   pub_status_{};
    bool     pub_connected_{false};
    mutable SemaphoreHandle_t pub_mtx_{nullptr};

    std::atomic<bool> sync_requested_{false};

    SettingsCallback on_settings_{};
    StatusCallback   on_status_{};
    PacketCallback   on_packet_{};
};

}  // namespace cn105
