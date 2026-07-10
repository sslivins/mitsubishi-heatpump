/// @file web_ui.h
/// @brief On-device diagnostics/control web server (REST API + dashboard).
///
/// Served only while WiFi STA is connected (the SoftAP path uses the captive
/// portal in wifi_manager instead). Endpoints:
///
///   GET  /                       -> gzip'd dashboard (web/index.html)
///   GET  /api/status             -> device/network/power/link JSON
///   GET  /api/settings           -> current heat-pump settings + status JSON
///   POST /api/settings           -> apply {power,mode,temperature,fan,vane,
///                                   wideVane,remoteTemp} (any subset)
///   POST /api/system/restart     -> esp_restart()
///   POST /api/system/factory_reset -> erase WiFi creds + restart
///   GET  /api/mqtt               -> current broker settings (password masked)
///   POST /api/mqtt               -> save broker settings to NVS + restart
///   GET  /api/wifi               -> current network {ssid,mode,connected,ip,
///                                   ap_name,password_set} (password never sent)
///   POST /api/wifi               -> save {ssid,password?} to NVS + restart
///                                   (password preserved when omitted)
///
/// The module is decoupled from the rest of the firmware via the Hooks struct:
/// the app supplies getters and a command applier so this layer never touches
/// global state directly. Commands reuse hvac_mqtt::Command so the web path and
/// the MQTT path funnel through the exact same apply logic in main.cpp.

#pragma once

#include <cstdint>
#include <functional>

#include "cn105.h"
#include "hvac_mqtt.h"
#include "esp_err.h"

namespace web_ui {

/// Snapshot of PMIC telemetry for the dashboard. Filled by the app from its
/// cached PMIC readings so the web task never touches the I2C bus directly.
struct PowerTelemetry {
    bool        present  = false;
    uint16_t    vbat_mv  = 0;
    uint16_t    vin_mv   = 0;   ///< raw dedicated 5VIN pin
    uint16_t    vinout_mv= 0;   ///< raw bidirectional 5VINOUT port
    uint16_t    input_mv = 0;   ///< effective supply = max(vin, vinout)
    const char* source   = "unknown";  ///< "vin" | "vin_out" | "battery" | "unknown"
    bool        charging = false;
};

/// Diagnostics snapshot for the dashboard (boot/brownout/power-sag).
struct DiagTelemetry {
    uint32_t    boot_count      = 0;
    uint32_t    brownout_count  = 0;
    uint16_t    vin_min_ever_mv = 0;
    const char* reset_reason    = "unknown";
    bool        last_was_brownout = false;
    uint16_t    vin_min_mv      = 0;
    uint32_t    vin_sag_count   = 0;
};

/// Application-supplied accessors. All are invoked from the HTTP server task.
struct Hooks {
    std::function<cn105::Settings()>               get_settings;
    std::function<cn105::Status()>                 get_status;
    std::function<bool()>                          unit_connected;
    std::function<bool()>                          mqtt_connected;
    std::function<PowerTelemetry()>                get_power;
    std::function<DiagTelemetry()>                 get_diag;
    std::function<void(const hvac_mqtt::Command&)> apply_command;
};

/// Start the HTTP server. Call once, after wifi::init() reports CONNECTED.
esp_err_t init(const Hooks& hooks);

}  // namespace web_ui
