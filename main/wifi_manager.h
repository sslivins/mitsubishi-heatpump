/// @file wifi_manager.h
/// @brief WiFi station bring-up with a SoftAP provisioning fallback.
///
/// Adapted from the arctic-sniffer WiFi manager. Credentials are read from NVS
/// (namespace "wifi", keys "ssid"/"pass"); if absent, the CONFIG_WIFI_*
/// fallback is used; if that is also empty or STA fails, the device starts a
/// SoftAP so the user can provision it.

#pragma once

#include "esp_err.h"

namespace wifi {

enum class Mode { IDLE, CONNECTING, CONNECTED, PROVISIONING };

/// Bring up WiFi. Returns ESP_OK once STA is connected with an IP, or
/// ESP_ERR_NOT_FINISHED when it has fallen back to SoftAP provisioning.
esp_err_t init();

Mode get_mode();
bool is_connected();
const char* get_ip();
const char* get_ap_name();

/// Store credentials in NVS (used by the provisioning portal). Caller should
/// reboot afterwards.
esp_err_t save_credentials(const char* ssid, const char* pass);

/// Erase stored credentials so the next boot re-enters provisioning.
esp_err_t erase_credentials();

}  // namespace wifi
