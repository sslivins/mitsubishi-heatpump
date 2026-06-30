/// @file ota.cpp
/// @brief OTA firmware update implementation.

#include "ota.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <mutex>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ota";

namespace ota {

namespace {

std::mutex  s_mtx;
Status      s_status;
bool        s_busy = false;
std::function<void(const Status&)> s_on_progress;

// Local-upload streaming state.
esp_ota_handle_t       s_handle    = 0;
const esp_partition_t* s_partition = nullptr;
size_t                 s_total     = 0;
size_t                 s_written   = 0;

void set_status(State state, int progress, const char* msg) {
    Status snap;
    bool changed;
    {
        std::lock_guard<std::mutex> lock(s_mtx);
        changed = (s_status.state != state) || (s_status.progress != progress);
        s_status.state = state;
        s_status.progress = progress;
        s_status.message = msg ? msg : "";
        snap = s_status;
    }
    // Notify outside the lock — the callback publishes to MQTT.
    if (changed && s_on_progress) s_on_progress(snap);
}

// Atomically claim the OTA pipeline; returns false if an update is already busy.
bool try_claim() {
    std::lock_guard<std::mutex> lock(s_mtx);
    if (s_busy) return false;
    s_busy = true;
    return true;
}

void release() {
    std::lock_guard<std::mutex> lock(s_mtx);
    s_busy = false;
}

void url_task(void* arg) {
    std::string* url_ptr = static_cast<std::string*>(arg);
    std::string url = *url_ptr;
    delete url_ptr;

    ESP_LOGI(TAG, "URL OTA: %s", url.c_str());
    set_status(State::InProgress, -1, "connecting");

    esp_http_client_config_t http = {};
    http.url = url.c_str();
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = 20000;
    http.keep_alive_enable = true;
    // GitHub release downloads 302-redirect to a long presigned CDN URL; the
    // default 512-byte buffers overflow ("HTTP_CLIENT: Out of buffer") when the
    // redirected request line is written. Size them up so redirects work.
    http.buffer_size = 4096;
    http.buffer_size_tx = 4096;

    esp_https_ota_config_t cfg = {};
    cfg.http_config = &http;

    esp_https_ota_handle_t h = nullptr;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK || h == nullptr) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        set_status(State::Failed, 0, esp_err_to_name(err));
        release();
        vTaskDelete(nullptr);
        return;
    }

    int image_size = esp_https_ota_get_image_size(h);
    set_status(State::InProgress, image_size > 0 ? 0 : -1, "downloading");

    while (true) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int read = esp_https_ota_get_image_len_read(h);
        if (image_size > 0) {
            int pct = (int)((int64_t)read * 100 / image_size);
            set_status(State::InProgress, pct, "downloading");
        }
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        err = esp_https_ota_finish(h);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "URL OTA complete — rebooting");
            set_status(State::Success, 100, "complete; rebooting");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
            set_status(State::Failed, 0, esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "URL OTA failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        set_status(State::Failed, 0, esp_err_to_name(err));
    }
    release();
    vTaskDelete(nullptr);
}

}  // namespace

void init() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
        ESP_LOGI(TAG, "running '%s' (state=%d, version=%s)", running->label,
                 (int)st, esp_app_get_description()->version);
    }
}

void mark_valid() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "image marked valid — rollback cancelled");
        } else {
            ESP_LOGW(TAG, "failed to mark image valid");
        }
    }
}

esp_err_t local_begin(size_t total_size) {
    if (!try_claim()) {
        ESP_LOGW(TAG, "local OTA rejected — already busy");
        return ESP_ERR_INVALID_STATE;
    }
    s_partition = esp_ota_get_next_update_partition(nullptr);
    if (!s_partition) {
        set_status(State::Failed, 0, "no OTA partition");
        release();
        return ESP_FAIL;
    }
    s_total = total_size;
    s_written = 0;
    esp_err_t err = esp_ota_begin(s_partition, OTA_SIZE_UNKNOWN, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        set_status(State::Failed, 0, esp_err_to_name(err));
        release();
        return err;
    }
    ESP_LOGI(TAG, "local OTA -> '%s' (%u bytes)", s_partition->label,
             (unsigned)total_size);
    set_status(State::InProgress, s_total ? 0 : -1, "uploading");
    return ESP_OK;
}

esp_err_t local_write(const void* data, size_t len) {
    if (!s_handle) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        local_abort();
        return err;
    }
    s_written += len;
    if (s_total > 0) {
        int pct = (int)((int64_t)s_written * 100 / s_total);
        set_status(State::InProgress, pct, "uploading");
    }
    return ESP_OK;
}

esp_err_t local_end() {
    if (!s_handle) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        set_status(State::Failed, 0, esp_err_to_name(err));
        release();
        return err;
    }
    err = esp_ota_set_boot_partition(s_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        set_status(State::Failed, 0, esp_err_to_name(err));
        release();
        return err;
    }
    ESP_LOGI(TAG, "local OTA complete (%u bytes) — boot set to '%s'",
             (unsigned)s_written, s_partition->label);
    set_status(State::Success, 100, "complete; rebooting");
    release();
    return ESP_OK;
}

void local_abort() {
    if (s_handle) {
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
    set_status(State::Failed, 0, "aborted");
    release();
}

esp_err_t start_url(const std::string& url) {
    if (url.empty()) return ESP_ERR_INVALID_ARG;
    if (busy()) {
        ESP_LOGW(TAG, "URL OTA rejected — already busy");
        return ESP_ERR_INVALID_STATE;
    }
    if (!try_claim()) return ESP_ERR_INVALID_STATE;
    // try_claim succeeded; hand ownership of the busy flag to the task.
    auto* arg = new std::string(url);
    if (xTaskCreate(url_task, "ota_url", 8192, arg, 5, nullptr) != pdPASS) {
        delete arg;
        set_status(State::Failed, 0, "task spawn failed");
        release();
        return ESP_FAIL;
    }
    return ESP_OK;
}

Status get_status() {
    std::lock_guard<std::mutex> lock(s_mtx);
    return s_status;
}

bool busy() {
    std::lock_guard<std::mutex> lock(s_mtx);
    return s_busy;
}

// ── GitHub release auto-update poller ──────────────────────────────────
namespace {

constexpr char kReleasesLatestUrl[] =
    "https://github.com/sslivins/mitsubishi-heatpump/releases/latest";
constexpr char kReleaseDownloadFmt[] =
    "https://github.com/sslivins/mitsubishi-heatpump/releases/download/%s/mitsubishi-heatpump.bin";

std::mutex            s_upd_mtx;
UpdateInfo            s_upd;
std::string           s_pending_source;  ///< set by check_now(), consumed by the checker task
TaskHandle_t          s_checker = nullptr;
uint32_t              s_interval_ms = 6 * 3600 * 1000;
std::function<void()> s_on_changed;

// Record what kicked off the upcoming check so it can be read remotely.
void record_trigger(const char* trigger, const std::string& requester) {
    std::lock_guard<std::mutex> lk(s_upd_mtx);
    s_upd.last_trigger = trigger;
    s_upd.last_requester = requester;
}

// Captures the Location response header from GitHub's 302 redirect so we can
// read the latest tag without hitting the rate-limited api.github.com.
struct CheckCtx { std::string location; };

esp_err_t check_evt(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key &&
        strcasecmp(evt->header_key, "Location") == 0) {
        auto* c = static_cast<CheckCtx*>(evt->user_data);
        if (c) c->location = evt->header_value ? evt->header_value : "";
    }
    return ESP_OK;
}

// Semantic compare of dotted versions, ignoring a leading 'v'.
// >0 if a newer than b, <0 if older, 0 if equal.
int compare_versions(const char* a, const char* b) {
    if (*a == 'v' || *a == 'V') ++a;
    if (*b == 'v' || *b == 'V') ++b;
    int a1 = 0, a2 = 0, a3 = 0, b1 = 0, b2 = 0, b3 = 0;
    sscanf(a, "%d.%d.%d", &a1, &a2, &a3);
    sscanf(b, "%d.%d.%d", &b1, &b2, &b3);
    if (a1 != b1) return a1 > b1 ? 1 : -1;
    if (a2 != b2) return a2 > b2 ? 1 : -1;
    if (a3 != b3) return a3 > b3 ? 1 : -1;
    return 0;
}

void perform_check() {
    const char* cur = esp_app_get_description()->version;
    {
        std::lock_guard<std::mutex> lk(s_upd_mtx);
        s_upd.checking = true;
        s_upd.current_version = cur;
    }

    std::string latest, url, published, release_url, err_msg;
    bool ok = false;
    bool no_release = false;  ///< no published release → treat as up to date

    // Resolve the latest release via github.com's /releases/latest 302 redirect
    // (Location: .../releases/tag/<tag>) rather than api.github.com, which imposes
    // a 60-request/hour unauthenticated limit *per source IP* — a shared home IP
    // (all units + browsers) exhausts it and the check then fails with HTTP 403.
    CheckCtx ctx;
    esp_http_client_config_t cfg = {};
    cfg.url = kReleasesLatestUrl;
    cfg.event_handler = check_evt;
    cfg.user_data = &ctx;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.disable_auto_redirect = true;  // we want the 302 Location, not its target

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli) {
        esp_http_client_set_header(cli, "User-Agent", "mitsubishi-heatpump");
        esp_err_t e = esp_http_client_perform(cli);
        int sc = esp_http_client_get_status_code(cli);
        if (e == ESP_OK && (sc == 301 || sc == 302 || sc == 307 || sc == 308)) {
            static const char kMark[] = "/releases/tag/";
            size_t p = ctx.location.find(kMark);
            if (p != std::string::npos) {
                latest = ctx.location.substr(p + sizeof(kMark) - 1);
                size_t q = latest.find_first_of("?#\r\n /");
                if (q != std::string::npos) latest.erase(q);
                if (!latest.empty()) {
                    char dl[256];
                    snprintf(dl, sizeof(dl), kReleaseDownloadFmt, latest.c_str());
                    url = dl;
                    release_url = ctx.location;
                    // Normalize the tag for display/compare so it matches the
                    // app's installed_version, which carries no 'v' prefix.
                    // Otherwise HA compares "0.2.6" != "v0.2.6" and shows
                    // "update available" forever even when already current.
                    if (latest[0] == 'v' || latest[0] == 'V') latest.erase(0, 1);
                    ok = true;
                } else {
                    err_msg = "empty tag in redirect";
                }
            } else {
                // Redirect to .../releases (no /tag/) => no published release yet.
                no_release = true;
            }
        } else if (e == ESP_OK && sc == 404) {
            // No published (non-draft) release — nothing newer; report up to date.
            no_release = true;
        } else {
            char tmp[80];
            snprintf(tmp, sizeof(tmp), "HTTP %d (%s)", sc, esp_err_to_name(e));
            err_msg = tmp;
        }
        esp_http_client_cleanup(cli);
    } else {
        err_msg = "http client init failed";
    }

    {
        std::lock_guard<std::mutex> lk(s_upd_mtx);
        s_upd.checking = false;
        s_upd.checked = true;
        s_upd.current_version = cur;
        s_upd.last_checked_ms = esp_timer_get_time() / 1000;
        if (ok) {
            s_upd.latest_version = latest;
            s_upd.download_url = url;
            s_upd.published_at = published;
            s_upd.release_url = release_url;
            s_upd.update_available =
                !url.empty() && compare_versions(latest.c_str(), cur) > 0;
            s_upd.error.clear();
            ESP_LOGI(TAG, "update check: current=%s latest=%s available=%s",
                     cur, latest.c_str(), s_upd.update_available ? "yes" : "no");
        } else if (no_release) {
            // No published release: nothing newer than what's running.
            s_upd.latest_version = cur;
            s_upd.download_url.clear();
            s_upd.published_at.clear();
            s_upd.release_url.clear();
            s_upd.update_available = false;
            s_upd.error.clear();
            ESP_LOGI(TAG, "update check: no published release — up to date (%s)", cur);
        } else {
            s_upd.error = err_msg;
            ESP_LOGW(TAG, "update check failed: %s", err_msg.c_str());
        }
    }

    if (s_on_changed) s_on_changed();
}

void checker_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(10000));  // let WiFi/SNTP/TLS time settle
    ESP_LOGI(TAG, "update check trigger: boot");
    record_trigger("boot", "");
    perform_check();
    for (;;) {
        // Sleep until the interval elapses, or wake early when check_now() fires.
        // A non-zero return means we were notified (on-demand); zero is the timer.
        // NOTE: pdMS_TO_TICKS() multiplies in 32-bit (TickType_t), so a 6h value
        // at CONFIG_FREERTOS_HZ=1000 (21,600,000 ms * 1000) overflows uint32 and
        // collapses the "6 hour" wait to ~2 minutes. Compute ticks in 64-bit.
        TickType_t wait_ticks =
            (TickType_t)(((uint64_t)s_interval_ms * configTICK_RATE_HZ) / 1000ULL);
        uint32_t notified = ulTaskNotifyTake(pdTRUE, wait_ticks);
        std::string src;
        {
            std::lock_guard<std::mutex> lk(s_upd_mtx);
            src = s_pending_source;
            s_pending_source.clear();
        }
        const char* trig = notified ? "on-demand (check_now)" : "interval timer";
        if (notified && !src.empty())
            ESP_LOGI(TAG, "update check trigger: %s — %s", trig, src.c_str());
        else
            ESP_LOGI(TAG, "update check trigger: %s", trig);
        record_trigger(notified ? "on-demand" : "interval", notified ? src : std::string());
        perform_check();
    }
}

}  // namespace

void start_update_checker(uint32_t interval_seconds) {
    if (s_checker) return;
    if (interval_seconds > 0) s_interval_ms = interval_seconds * 1000;
    {
        std::lock_guard<std::mutex> lk(s_upd_mtx);
        s_upd.current_version = esp_app_get_description()->version;
    }
    if (xTaskCreate(checker_task, "ota_chk", 8192, nullptr, 4, &s_checker) != pdPASS) {
        s_checker = nullptr;
        ESP_LOGE(TAG, "failed to start update checker task");
    }
}

void check_now(const char* source) {
    {
        std::lock_guard<std::mutex> lk(s_upd_mtx);
        s_pending_source = source ? source : "";
    }
    if (s_checker) xTaskNotifyGive(s_checker);
}

UpdateInfo get_update_info() {
    std::lock_guard<std::mutex> lk(s_upd_mtx);
    return s_upd;
}

esp_err_t install_latest() {
    std::string url;
    {
        std::lock_guard<std::mutex> lk(s_upd_mtx);
        if (!s_upd.update_available || s_upd.download_url.empty())
            return ESP_ERR_INVALID_STATE;
        url = s_upd.download_url;
    }
    return start_url(url);
}

void set_on_update_changed(std::function<void()> cb) {
    s_on_changed = std::move(cb);
}

void set_on_progress(std::function<void(const Status&)> cb) {
    s_on_progress = std::move(cb);
}

}  // namespace ota
