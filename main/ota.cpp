/// @file ota.cpp
/// @brief OTA firmware update implementation.

#include "ota.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ota";

namespace ota {

namespace {

std::mutex  s_mtx;
Status      s_status;
bool        s_busy = false;

// Local-upload streaming state.
esp_ota_handle_t       s_handle    = 0;
const esp_partition_t* s_partition = nullptr;
size_t                 s_total     = 0;
size_t                 s_written   = 0;

void set_status(State state, int progress, const char* msg) {
    std::lock_guard<std::mutex> lock(s_mtx);
    s_status.state = state;
    s_status.progress = progress;
    s_status.message = msg ? msg : "";
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

constexpr char kGithubApiUrl[] =
    "https://api.github.com/repos/sslivins/mitsubishi-heatpump/releases/latest";

std::mutex            s_upd_mtx;
UpdateInfo            s_upd;
TaskHandle_t          s_checker = nullptr;
uint32_t              s_interval_ms = 6 * 3600 * 1000;
std::function<void()> s_on_changed;

// Grows to hold the (chunked) GitHub API response body.
struct RespBuf { char* data = nullptr; size_t len = 0; size_t cap = 0; };

esp_err_t github_evt(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    RespBuf* b = static_cast<RespBuf*>(evt->user_data);
    if (!b) return ESP_OK;
    if (b->len + evt->data_len + 1 > b->cap) {
        size_t ncap = b->len + evt->data_len + 1024;
        char* nd = static_cast<char*>(realloc(b->data, ncap));
        if (!nd) return ESP_OK;  // OOM: keep what we have; parse will fail cleanly
        b->data = nd;
        b->cap  = ncap;
    }
    memcpy(b->data + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    b->data[b->len] = '\0';
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

    RespBuf buf;
    std::string latest, url, published, release_url, err_msg;
    bool ok = false;

    esp_http_client_config_t cfg = {};
    cfg.url = kGithubApiUrl;
    cfg.event_handler = github_evt;
    cfg.user_data = &buf;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli) {
        // GitHub requires a User-Agent; ask for the v3 JSON media type.
        esp_http_client_set_header(cli, "User-Agent", "mitsubishi-heatpump");
        esp_http_client_set_header(cli, "Accept", "application/vnd.github+json");
        esp_err_t e = esp_http_client_perform(cli);
        int sc = esp_http_client_get_status_code(cli);
        if (e == ESP_OK && sc == 200 && buf.data) {
            cJSON* root = cJSON_Parse(buf.data);
            if (root) {
                cJSON* tag = cJSON_GetObjectItem(root, "tag_name");
                if (cJSON_IsString(tag)) latest = tag->valuestring;
                cJSON* pub = cJSON_GetObjectItem(root, "published_at");
                if (cJSON_IsString(pub)) published = pub->valuestring;
                cJSON* html = cJSON_GetObjectItem(root, "html_url");
                if (cJSON_IsString(html)) release_url = html->valuestring;
                cJSON* assets = cJSON_GetObjectItem(root, "assets");
                if (cJSON_IsArray(assets)) {
                    cJSON* a;
                    cJSON_ArrayForEach(a, assets) {
                        cJSON* nm = cJSON_GetObjectItem(a, "name");
                        if (!cJSON_IsString(nm)) continue;
                        const char* name = nm->valuestring;
                        size_t nlen = strlen(name);
                        // The release ships 4 .bin files (app + bootloader +
                        // partition-table + ota_data); pick the app image only.
                        bool is_bin = nlen >= 4 && strcmp(name + nlen - 4, ".bin") == 0;
                        if (!is_bin || strstr(name, "mitsubishi-heatpump") == nullptr)
                            continue;
                        cJSON* du = cJSON_GetObjectItem(a, "browser_download_url");
                        if (cJSON_IsString(du)) { url = du->valuestring; break; }
                    }
                }
                cJSON_Delete(root);
                ok = !latest.empty();
                if (!ok) err_msg = "no tag_name in response";
            } else {
                err_msg = "JSON parse failed";
            }
        } else {
            if (sc == 404) {
                err_msg = "no published release yet";
            } else {
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "HTTP %d (%s)", sc, esp_err_to_name(e));
                err_msg = tmp;
            }
        }
        esp_http_client_cleanup(cli);
    } else {
        err_msg = "http client init failed";
    }
    free(buf.data);

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
        } else {
            s_upd.error = err_msg;
            ESP_LOGW(TAG, "update check failed: %s", err_msg.c_str());
        }
    }

    if (s_on_changed) s_on_changed();
}

void checker_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(10000));  // let WiFi/SNTP/TLS time settle
    for (;;) {
        perform_check();
        // Sleep until the interval elapses, or wake early when check_now() fires.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_interval_ms));
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

void check_now() {
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

}  // namespace ota
