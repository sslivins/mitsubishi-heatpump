/// @file hvac_mqtt.h
/// @brief MQTT bridge between the CN105 driver and Home Assistant.
///
/// Reproduces the topic contract of gysmo38/mitsubishi2MQTT so existing Home
/// Assistant entities and automations keep working unchanged:
///
///   Base:        <base_topic>/<friendly_name>
///   Subscribe:   .../mode/set .../temp/set .../remote_temp/set .../fan/set
///                .../vane/set .../wideVane/set .../system/set .../ota/set
///                .../update/install
///   Publish:     .../state (retained) .../settings .../availability (LWT)
///                .../update/state (retained)
///   HA discovery: homeassistant/climate/<friendly_name>/config (retained)
///                 homeassistant/update/<friendly_name>/config (retained)
///
/// Uses esp-mqtt (esp_mqtt). LWT marks the unit unavailable instantly on a
/// power blip — ideal for the battery-buffered Stamp-S3Bat.
///
/// PUBLISH paths are implemented; command parsing is a STUB (see hvac_mqtt.cpp).

#pragma once

#include <functional>
#include <string>
#include <cstdint>
#include "cn105.h"
#include "esp_err.h"

namespace hvac_mqtt {

struct Config {
    std::string broker_uri;     ///< "mqtt://host:1883" or "mqtts://host:8883"
    std::string username;
    std::string password;
    std::string base_topic;     ///< e.g. "mitsubishi2mqtt"
    std::string friendly_name;  ///< e.g. "living_room_hp" (the MQTT node name)
    std::string device_uid;     ///< hardware-unique id (from the ESP32 MAC). Used
                                ///< for HA unique_id/device.identifiers so 4 units
                                ///< never merge or collide. e.g. "E609A1".
    std::string sw_version;     ///< firmware version, shown on the HA device.
};

/// A command parsed from a .../*/set topic, to be applied to the heat pump.
struct Command {
    enum class Kind { Power, Mode, Temperature, Fan, Vane, WideVane, RemoteTemp, System, Ota, UpdateInstall };
    Kind kind;
    std::string value;  ///< raw payload (e.g. "HEAT", "21.5", "AUTO")
};

using CommandCallback = std::function<void(const Command&)>;

/// Broker settings that are user-configurable at runtime (web UI) and persisted
/// to NVS. Kept separate from the internal Config so the UI can present host/port
/// fields directly (matching the mitsubishi2MQTT layout).
struct StoredSettings {
    std::string host;
    int         port = 1883;
    std::string username;
    std::string password;
    std::string base_topic;
    std::string friendly_name;  ///< blank → firmware derives "heatpump-<uid>"
};

/// Read the persisted broker settings from NVS, falling back per-field to
/// `fallback` (typically the Kconfig defaults) for any key not yet saved.
StoredSettings load_settings(const StoredSettings& fallback);

/// Persist broker settings to NVS. Takes effect on the next boot.
esp_err_t save_settings(const StoredSettings& s);

/// The settings the running client was started with (for the web UI to display).
StoredSettings get_settings();

/// Start the MQTT client. The callback fires (on the MQTT task) for every
/// inbound command topic; apply it to your cn105::HeatPump there.
esp_err_t init(const Config& cfg, CommandCallback on_command);

/// Register a callback fired (on the MQTT task) on every successful (re)connect,
/// after subscriptions and availability are published. Use it to (re)publish the
/// retained discovery configs and current state so HA re-syncs after a drop.
void set_on_connected(std::function<void()> cb);

/// Publish the Home Assistant MQTT-discovery config (retained). Call once after
/// the first successful connect.
esp_err_t publish_discovery(const cn105::Settings& s);

/// Publish current state/settings. Call when cn105 reports a change.
esp_err_t publish_state(const cn105::Settings& s, const cn105::Status& st);
esp_err_t publish_settings(const cn105::Settings& s);

/// Publish availability ("online"/"offline"). "offline" is also the retained
/// LWT the broker sends automatically when the device drops.
esp_err_t publish_availability(bool online);

/// Publish the Home Assistant MQTT-discovery config for a firmware `update`
/// entity (retained) to homeassistant/update/<friendly_name>/config. HA shows
/// an "update available" badge and an Install button wired to .../update/install.
/// Call once after the first successful connect.
esp_err_t publish_update_discovery();

/// Publish the firmware update state (retained) as JSON to .../update/state:
/// {"installed_version","latest_version","release_url","release_summary"}.
/// HA flags an update when latest_version > installed_version. When
/// @p update_percentage is >= 0 the payload also carries update_percentage +
/// in_progress=true so Home Assistant shows a live progress bar during an
/// install; passing -1 publishes update_percentage=null to clear it.
esp_err_t publish_update_state(const std::string& installed,
                               const std::string& latest,
                               const std::string& release_url = "",
                               const std::string& release_summary = "",
                               int update_percentage = -1);

/// Diagnostics reported to Home Assistant as `entity_category: diagnostic`
/// sensors: last reset reason, cumulative brownout count, and the lowest input
/// voltage seen. Lets HA alert/graph power health without a battery attached.
struct DiagState {
    const char* reset_reason   = "unknown";
    uint32_t    brownout_count = 0;
    uint32_t    vin_sag_count  = 0;
    uint16_t    vin_min_mv     = 0;   ///< lowest effective input this session (mV)
};

/// Publish the HA MQTT-discovery configs (retained) for the diagnostic sensors
/// above. Additive — does not touch the existing climate/update topic contract.
/// Call once after the first successful connect.
esp_err_t publish_diag_discovery();

/// Publish the current diagnostic values (retained JSON) to .../diag/state.
esp_err_t publish_diag_state(const DiagState& d);

bool is_connected();

}  // namespace hvac_mqtt
