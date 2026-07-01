/// @file wifi_manager.h
/// @brief WiFi station bring-up with a SoftAP provisioning fallback.
///
/// Adapted from the arctic-sniffer WiFi manager. Credentials are read from NVS
/// (namespace "wifi", keys "ssid"/"pass"); if absent, the CONFIG_WIFI_*
/// fallback is used; if that is also empty or STA fails, the device starts a
/// SoftAP so the user can provision it.

#pragma once

#include <string>

#include "esp_err.h"

namespace wifi {

enum class Mode { IDLE, CONNECTING, CONNECTED, PROVISIONING };

/// Short, stable per-chip id (4 hex = low 2 bytes of the factory base MAC).
/// Espressif assigns every chip a globally-unique MAC, so this is collision-free
/// for a handful of units. Reused for the SoftAP name, the mDNS hostname, and the
/// default MQTT friendly_name so all three stay unique and consistent.
std::string device_uid();

/// mDNS hostname actually in use: "<CONFIG_MDNS_HOSTNAME>-<device_uid>".
std::string mdns_hostname();

/// User-settable display name shown in the web UI, login screen and browser
/// tab. Empty when unset (callers fall back to the hostname / a default). This
/// is purely cosmetic — it does NOT affect the mDNS hostname, SoftAP name, or
/// MQTT friendly_name, so renaming never disturbs Home Assistant discovery.
std::string device_display_name();

/// Persist the display name to NVS and update the in-memory cache (no reboot).
/// Pass "" to clear it back to the default. Returns ESP_OK on success.
esp_err_t set_display_name(const char* name);

/// Bring up WiFi. Returns ESP_OK once STA is connected with an IP, or
/// ESP_ERR_NOT_FINISHED when it has fallen back to SoftAP provisioning.
esp_err_t init();

Mode get_mode();
bool is_connected();
const char* get_ip();
const char* get_ap_name();

/// Live RSSI (dBm) of the current association, or 0 when not connected.
int get_rssi();

/// Auth mode of the current association as a wifi_auth_mode_t int
/// (0 == WIFI_AUTH_OPEN), or -1 when not connected.
int get_auth();

/// SSID the device is configured to join (empty in out-of-box / provisioning).
const char* get_ssid();

/// Whether a WiFi password is currently stored (the password itself is never
/// exposed over the API — used server-side only to preserve it on partial saves).
bool has_password();
const char* get_password();

/// Store credentials in NVS (used by the provisioning portal). Caller should
/// reboot afterwards.
esp_err_t save_credentials(const char* ssid, const char* pass);

/// Blocking scan (~2-4s) for nearby networks. Fills @p out with a JSON array
/// string: [{"ssid":..,"rssi":..,"auth":..}] deduped by SSID (strongest kept),
/// hidden/empty SSIDs dropped, sorted by RSSI descending.
esp_err_t scan_json(std::string& out);

/// Erase stored credentials so the next boot re-enters provisioning.
esp_err_t erase_credentials();

}  // namespace wifi
