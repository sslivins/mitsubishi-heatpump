/// @file events.h
/// @brief Curated, human-readable device activity log (not a debug console).
///
/// Records *meaningful* events — mode/power changes with attribution, group
/// coordination, membership/pairing, OTA, connectivity, faults, and local
/// setpoint/fan changes — so an overnight action ("Eric's Room switched all
/// zones to Heat at 3am") is still legible in the morning. Events persist to a
/// rotating append-only file on the otherwise-unused SPIFFS `storage`
/// partition; the most recent ones are also mirrored in a small RAM ring that
/// backs the web UI's Activity view. This is deliberately NOT a mirror of
/// ESP_LOG: only semantic events are logged, from the sites that know the
/// source of a change.
#pragma once

#include <cstdint>
#include <string>

namespace events {

/// Broad category of an event — drives the icon/colour in the UI.
enum class Cat : uint8_t {
    Mode = 0,    ///< HVAC mode changed (heat/cool/dry/fan/auto)
    Power,       ///< powered on/off
    Setpoint,    ///< target temperature changed (local-only)
    Fan,         ///< fan speed / vane changed (local-only)
    Group,       ///< group coordination (whole-group switch, conflict, dwell)
    Membership,  ///< pairing / peer joined or left the group
    Ota,         ///< firmware update lifecycle
    Net,         ///< Wi-Fi / MQTT connectivity (rate-limited)
    Unit,        ///< CN105 unit link up/down
    Fault,       ///< unit fault / error code
    System,      ///< boot / brownout / misc
};

/// What (or who) caused the event.
enum class Actor : uint8_t {
    System = 0,      ///< the device itself (boot, connectivity, faults)
    WebUI,           ///< a signed-in web UI user (see @c name for the username)
    HomeAssistant,   ///< a command from Home Assistant over MQTT
    Group,           ///< a peer-initiated group change (see @c name for the peer)
    Local,           ///< a local/physical change with no distinguishable source
};

/// Initialize the log: mount SPIFFS, load the sequence high-water mark and the
/// tail of the persisted file into the RAM ring. Call once at boot, after
/// nvs_flash_init(). Safe to call before Wi-Fi/time are up (timestamps are
/// filled in with the wall clock once SNTP has synced; until then ts_epoch=0).
void init();

/// Record one event. @p msg is a firmware-composed, trusted sentence and must
/// NOT contain untrusted input. @p name is an optional label that MAY be
/// untrusted (a peer friendly-name or a username); it is stored verbatim and
/// escaped at render time. Thread-safe; cheap enough to call from any task.
void log(Cat cat, Actor actor, const std::string& msg,
         const std::string& name = "");

/// Serialize the newest events with @c seq > @p since (most-recent first),
/// capped at @p limit, as a JSON object:
///   { "now_epoch":N, "clock_valid":bool, "seq":N,
///     "events":[ {seq,ts,uptime_ms,cat,actor,msg,name}, ... ] }
/// Served from the RAM ring, so this is fast even under polling.
std::string get_json(uint32_t since, uint32_t limit);

/// Clear the persisted log and the RAM ring. The sequence counter is kept
/// monotonic across a clear so ids never repeat.
void clear();

/// Absolute path of the raw persisted log file (for an admin "download"
/// endpoint). May not exist yet if nothing has been logged.
const char* file_path();

}  // namespace events
