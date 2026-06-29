/// @file ota.h
/// @brief Over-the-air firmware update (local upload + HTTPS URL pull).
///
/// Two delivery paths share one apply pipeline and one status object:
///   - Local upload: the web handler streams a raw .bin POST body straight into
///     the inactive OTA slot via local_begin/local_write/local_end.
///   - URL pull:     start_url() spawns a background task that runs
///     esp_https_ota against a URL (e.g. a GitHub release asset), reporting
///     progress as it goes. Triggered from the dashboard or an MQTT /ota/set.
///
/// Rollback: the build enables CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, so a
/// freshly-booted OTA image starts in PENDING_VERIFY and the bootloader will
/// roll it back on the next reset unless mark_valid() is called once the device
/// proves itself healthy (WiFi up). Call mark_valid() exactly once per boot.

#pragma once

#include <cstddef>
#include <string>

#include "esp_err.h"

namespace ota {

enum class State { Idle, InProgress, Success, Failed };

struct Status {
    State       state    = State::Idle;
    int         progress = 0;  ///< 0..100, or -1 when the total size is unknown
    std::string message;       ///< human-readable detail (last error / phase)
};

/// Log the running partition's OTA state at boot. Call before mark_valid().
void init();

/// Confirm the running image is good, cancelling any pending rollback. No-op if
/// the image is not in PENDING_VERIFY. Call once WiFi is up.
void mark_valid();

// --- Local upload (streamed by the web handler, one at a time) ---
esp_err_t local_begin(size_t total_size);              ///< 0 = unknown size
esp_err_t local_write(const void* data, size_t len);
esp_err_t local_end();                                 ///< sets boot partition
void      local_abort();

// --- URL pull (background task; returns immediately) ---
esp_err_t start_url(const std::string& url);

Status get_status();
bool   busy();

}  // namespace ota
