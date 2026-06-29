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
#include "cn105.h"
#include "esp_err.h"

namespace hvac_mqtt {

struct Config {
    std::string broker_uri;     ///< "mqtt://host:1883" or "mqtts://host:8883"
    std::string username;
    std::string password;
    std::string base_topic;     ///< e.g. "mitsubishi2mqtt"
    std::string friendly_name;  ///< e.g. "living_room_hp"
};

/// A command parsed from a .../*/set topic, to be applied to the heat pump.
struct Command {
    enum class Kind { Power, Mode, Temperature, Fan, Vane, WideVane, RemoteTemp, System, Ota, UpdateInstall };
    Kind kind;
    std::string value;  ///< raw payload (e.g. "HEAT", "21.5", "AUTO")
};

using CommandCallback = std::function<void(const Command&)>;

/// Start the MQTT client. The callback fires (on the MQTT task) for every
/// inbound command topic; apply it to your cn105::HeatPump there.
esp_err_t init(const Config& cfg, CommandCallback on_command);

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
/// HA flags an update when latest_version > installed_version.
esp_err_t publish_update_state(const std::string& installed,
                               const std::string& latest,
                               const std::string& release_url = "",
                               const std::string& release_summary = "");

bool is_connected();

}  // namespace hvac_mqtt
