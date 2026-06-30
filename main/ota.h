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
#include <cstdint>
#include <functional>
#include <string>

#include "esp_err.h"

namespace ota {

enum class State { Idle, InProgress, Success, Failed };

struct Status {
    State       state    = State::Idle;
    int         progress = 0;  ///< 0..100, or -1 when the total size is unknown
    std::string message;       ///< human-readable detail (last error / phase)
};

/// Result of the most recent GitHub /releases/latest poll.
struct UpdateInfo {
    std::string current_version;   ///< running image version (no leading 'v')
    std::string latest_version;    ///< latest release tag (e.g. "v0.1.2"), empty if unknown
    std::string download_url;      ///< browser_download_url of the .bin asset
    std::string published_at;      ///< ISO-8601 publish time of the latest release
    std::string release_url;       ///< html_url of the latest release (for HA/UI)
    bool        update_available = false;  ///< latest > current AND a .bin asset exists
    bool        checking         = false;  ///< a check is currently in flight
    bool        checked          = false;  ///< at least one check has completed
    int64_t     last_checked_ms  = 0;      ///< esp_timer ms at last completed check (0 = never)
    std::string error;                     ///< last check error (empty when ok)
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

// --- GitHub release auto-update (background poller) ---

/// Start the periodic GitHub /releases/latest poller (single task, idempotent).
/// Call once after WiFi is up. Polls every `interval_seconds` (default 6h) and
/// on demand via check_now().
void start_update_checker(uint32_t interval_seconds = 6 * 3600);

/// Trigger an immediate check (non-blocking). No-op if the checker isn't running.
void check_now();

/// Snapshot of the most recent GitHub release check.
UpdateInfo get_update_info();

/// Begin a URL pull of the latest release asset, if an update is available.
/// Returns ESP_ERR_INVALID_STATE when no update is available / no asset / busy.
esp_err_t install_latest();

/// Register a callback fired (on the checker task) whenever a check completes,
/// successfully or not. Lets the app re-publish version state to HA, etc.
void set_on_update_changed(std::function<void()> cb);

/// Register a callback fired whenever the OTA install status/progress changes
/// (state transition or a new integer percent). Lets the app stream progress to
/// Home Assistant's update entity during an install. Fired from the OTA task.
void set_on_progress(std::function<void(const Status&)> cb);

Status get_status();
bool   busy();

}  // namespace ota
